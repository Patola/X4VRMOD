#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Measures the separable affine between two screenshots of the same scene:
#
#     x' = A_x * x + B_x        y' = A_y * y + B_y      (in NDC, per SBS half)
#
# which is exactly the shape task #35's off-axis map has, so a run that asks for
# a cant can be checked against the coefficients the layer logged rather than
# against an impression of the picture. Take 155 is why this exists as a tool:
# the scorer said FAIL, the screen said otherwise, and registering two JPEGs
# settled it in one step.
#
# **The measurement is separable and that is an assumption, not a convenience.**
# The off-axis map is separable by construction (x' depends on x and w only,
# y' on y and w only), so column and row profiles are sufficient statistics for
# it. They would NOT be sufficient for a rotation or a keystone, and this tool
# would report a confident wrong answer for either. --self-check pins what it
# can recover; anything outside that shape is out of range and it says so.
#
# Registration runs on GRADIENT energy, not intensity. X4's skybox is drawn by a
# procedural fullscreen shader, which classifies NonWorld and therefore does not
# move; the cockpit mesh does. Intensity profiles are dominated by the smooth
# bright starfield and the HUD, gradient profiles by the cockpit's hard edges,
# which are the thing under test.
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, map_coordinates


def gray(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.float64) / 255.0


def half(img, which):
    """One SBS eye. The composite is two eyes side by side; a screenshot may be
    a pixel or two off a perfect halving, so split on the measured width."""
    w = img.shape[1] // 2
    return img[:, :w] if which == 0 else img[:, w:2 * w]


def profiles(img, band=(0.0, 1.0)):
    """Gradient energy summed along each axis, over a vertical band.

    The band exists to exclude the HUD, which does not carry the transform: the
    off-axis affine rides on the World patches only, so the message box, the
    radar arcs and the reticle stay exactly where they were. Including them
    pulls the fit toward the identity and would make a working transform read
    as a partial one."""
    a, b = int(band[0] * img.shape[0]), int(band[1] * img.shape[0])
    sub = img[a:b]
    gy, gx = np.gradient(sub)
    e = np.hypot(gx, gy)
    col = e.sum(axis=0)  # length W, indexed by frame x
    row = e.sum(axis=1)  # length b-a, indexed by (frame y - a)
    # Remove the mean so the correlation is about structure, not brightness --
    # two runs of the same scene differ in exposure by more than they differ in
    # geometry.
    #
    # `a` is returned because the row profile is NOT indexed in frame
    # coordinates and the caller has to put it back; see warp().
    return col - col.mean(), row - row.mean(), a


def fit_1d(src, dst, scales, shift_px, centre=None, half_extent=None):
    """Best (scale, shift) mapping `src` onto `dst` by normalised correlation.

    Returns (scale, shift, score, margin). The forward model is

        dst[i] ~ src[(i - c - shift)/scale + c]

    with c the centre, which is the inverse of what the layer applies. For a
    fixed scale the shift is a plain integer translation of the resampled
    signal, so every shift is evaluated at once by cross-correlation rather
    than in a loop.

    **`margin` is the point of the return tuple.** A profile with repeated
    structure -- a cockpit has evenly spaced struts, and so does any grating --
    correlates almost as well one period away as it does at the truth, and a
    search that reports only its winner will hand back that alias with full
    confidence. margin is the best score MINUS the best score found at least
    one lobe away from the winner. A small margin means the answer is one of
    several equally good ones and must not be read as a measurement."""
    n = len(dst)
    # The frame's centre, not the profile's. A profile taken over a band is
    # still indexed in frame coordinates; see warp() for what assuming
    # otherwise costs.
    c = (n - 1) / 2.0 if centre is None else centre
    m = int(shift_px)
    d = dst - dst.mean()
    dn = np.linalg.norm(d)
    if dn < 1e-9:
        return (1.0, 0.0, -2.0, 0.0)
    results = []  # (score, scale, shift)
    for s in scales:
        # Resample once, on a grid extended by the shift range, so a shift is a
        # window offset into it.
        idx = np.arange(-m, n + m, dtype=np.float64)
        u = map_coordinates(src, [(idx - c) / s + c], order=1, mode="nearest")
        # Normalised cross-correlation of every length-n window of u with d.
        cs = np.concatenate(([0.0], np.cumsum(u)))
        cs2 = np.concatenate(([0.0], np.cumsum(u * u)))
        k = len(u) - n + 1
        wsum = cs[n:n + k] - cs[:k]
        wsq = cs2[n:n + k] - cs2[:k]
        var = wsq - wsum * wsum / n
        # Correlate: corr[j] = sum_i u[j+i]*d[i]
        nfft = 1 << int(np.ceil(np.log2(len(u) + n)))
        cor = np.fft.irfft(np.fft.rfft(u, nfft) *
                           np.conj(np.fft.rfft(d, nfft)), nfft)[:k]
        ok = var > 1e-12
        sc = np.full(k, -2.0)
        sc[ok] = cor[ok] / (np.sqrt(var[ok]) * dn)
        for j in np.arange(k)[ok]:
            # window start j  <->  u index j-m  <->  shift = m - j ... derive:
            # window i-th sample is u[j+i] = src[((j+i-m)-c)/s+c], and the
            # model wants src[((i-c-sh)/s)+c], so sh = m - j.
            results.append((float(sc[j]), float(s), float(m - j)))
    if not results:
        return (1.0, 0.0, -2.0, 0.0)
    results.sort(reverse=True)
    top = results[0]
    # One lobe away: far enough in shift that it is a different alignment, not
    # the same peak sampled again. Scale is allowed to differ freely -- an
    # alias usually trades scale against shift.
    lobe = max(4.0, 0.02 * n)
    alt = next((r for r in results if abs(r[2] - top[2]) > lobe), None)
    margin = top[0] - (alt[0] if alt else -1.0)
    return (top[1], top[2], top[0], margin)


def register(a_path, c_path, which, band, smax, shmax):
    A, C = gray(a_path), gray(c_path)
    ha, hc = half(A, which), half(C, which)
    h = min(ha.shape[0], hc.shape[0])
    w = min(ha.shape[1], hc.shape[1])
    ha, hc = ha[:h, :w], hc[:h, :w]
    ca, ra, off = profiles(ha, band)
    cc, rc, _ = profiles(hc, band)
    scales = np.arange(1.0 / smax, smax + 1e-9, 0.002)
    sx, tx, qx, mx = fit_1d(ca, cc, scales, shmax)
    # The row profile starts at frame row `off`, so the frame's centre sits at
    # (h-1)/2 - off within it. Passing the profile's own centre instead is a
    # bias of (A_y-1)*(frame centre - band centre) -- 38 px on the 0-0.72 band,
    # which is 0.054 in NDC and reads as a real disagreement.
    sy, ty, qy, my = fit_1d(ra, rc, scales, shmax, centre=(h - 1) / 2.0 - off)
    # NDC: a half is 2 units wide, so a shift of `tx` pixels over a half of
    # width W is 2*tx/W in NDC. The DENOMINATOR IS THE FRAME, not the band.
    return dict(A_x=sx, B_x=2.0 * tx / w, q_x=qx, m_x=mx,
                A_y=sy, B_y=2.0 * ty / h, q_y=qy, m_y=my, w=w, h=h)


def warp(img, ax, bx, ay, by, cy=None, hh=None):
    """Apply the forward map, resampling img at the inverse. Out-of-source
    samples come back NaN so the correlation can exclude them: a magnified
    frame is fed by less than a full source, and counting the edge padding as
    agreement is how a big scale factor scores well for the wrong reason.

    **`cy` and `hh` are the FRAME's centre row and half-height, in this array's
    coordinates, and they are not optional when `img` is a band.** The affine's
    fixed point is NDC 0, which is the centre of the rendered frame, and B_y is
    a fraction of the frame's half-height. Letting a cropped band supply its own
    centre silently re-references the whole transform: for the 0–0.72 band used
    here that is a bias of (A_y−1)·(704.5−507.6) = 38 px = 0.054 in NDC, which
    is large enough to look exactly like a real disagreement with the shader.
    It did."""
    h, w = img.shape
    cx = (w - 1) / 2.0
    if cy is None:
        cy = (h - 1) / 2.0
    if hh is None:
        hh = h / 2.0
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float64)
    sx = (xx - cx - bx * w / 2.0) / ax + cx
    sy = (yy - cy - by * hh) / ay + cy
    out = map_coordinates(img, [sy, sx], order=1, mode="constant", cval=np.nan)
    out[(sx < 0) | (sx > w - 1) | (sy < 0) | (sy > h - 1)] = np.nan
    return out


def ncc(a, b):
    """Normalised correlation over the samples where both are defined."""
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 500:
        return -2.0
    u, v = a[m], b[m]
    u = u - u.mean()
    v = v - v.mean()
    du, dv = np.linalg.norm(u), np.linalg.norm(v)
    if du < 1e-9 or dv < 1e-9:
        return -2.0
    return float(np.dot(u, v) / (du * dv))


def edges(img, step):
    """Downsampled gradient magnitude — the cockpit's hard edges, which are
    what moved, rather than the smooth skybox, which did not.

    Blurred BEFORE subsampling. A gradient image is nearly all high frequency,
    so plain `g[::4, ::4]` aliases; the aliasing pattern is identical in two
    unwarped frames and cancels, and completely different once one of them has
    been resampled. That made a correct transform score worse than the identity
    -- an artefact of the instrument that reads exactly like a refutation."""
    g = np.hypot(*np.gradient(img))
    if step > 1:
        g = gaussian_filter(g, sigma=step * 0.5)
    return g[::step, ::step]


def verify(a_path, c_path, pred, band, step=4):
    """Score the PREDICTED transform against the identity, then coordinate-
    descend from it. This is a hypothesis test, not a search: a global search
    over a large transform has competing alignments (see fit_1d's margin), so
    the question asked here is the narrower and more honest one -- does the
    predicted map explain the measured frame, and does the local optimum sit on
    top of it or somewhere else?"""
    A, C = gray(a_path), gray(c_path)
    out = []
    for wch in (0, 1):
        ha, hc = half(A, wch), half(C, wch)
        # Screenshots of the same window differ by a few pixels in both axes
        # (164a is 2819x1410, 164c 2818x1405). Crop to the common rectangle
        # BEFORE anything else: the transform is defined against the frame's
        # centre and its half-width, so two frames of different size have
        # different NDC per pixel and the coefficients would not be comparable.
        h = min(ha.shape[0], hc.shape[0])
        w = min(ha.shape[1], hc.shape[1])
        ha, hc = ha[:h, :w], hc[:h, :w]
        lo, hi = int(band[0] * h), int(band[1] * h)
        ea = edges(ha[lo:hi], step)
        ec = edges(hc[lo:hi], step)
        # The frame's centre and half-height, expressed in the band's
        # downsampled row coordinates. Without these the band supplies its own
        # centre and the whole transform is re-referenced.
        cy = ((h - 1) / 2.0 - lo) / step
        hh = (h / 2.0) / step
        p = pred[wch]
        s_id = ncc(warp(ea, 1.0, 0.0, 1.0, 0.0, cy, hh), ec)
        s_pr = ncc(warp(ea, *p, cy, hh), ec)
        best = list(p)
        s_best = s_pr
        for _ in range(3):
            for i, span in ((0, 0.10), (1, 0.10), (2, 0.10), (3, 0.10)):
                grid = np.arange(best[i] - span, best[i] + span + 1e-9,
                                 span / 12.0)
                for v in grid:
                    t = list(best)
                    t[i] = v
                    s = ncc(warp(ea, *t, cy, hh), ec)
                    if s > s_best:
                        s_best, best = s, t
        out.append(dict(view=wch, identity=s_id, predicted=s_pr,
                        best=s_best, at=best))
    return out


def self_check():
    """Recover a known affine from a synthetic image before trusting the tool
    on a real one. Without this the instrument is only assumed to work, and
    this project has already shipped an estimator whose range silently capped
    below the signal it was pointed at."""
    rng = np.random.default_rng(7)
    n = 512
    base = rng.random((n, 2 * n))
    # Structure, not noise: hard edges are what the gradient profile keys on.
    #
    # APERIODIC on purpose. The first version of this fixture put the bars on a
    # 38 px pitch and the third case came back +0.1016 against a wanted
    # -0.1500 -- 64.4 px out, which is exactly two of that pitch after the 0.85
    # scale. That was the fixture being ambiguous rather than the estimator
    # being wrong, but it is also the reason fit_1d now returns a margin: a
    # cockpit has evenly spaced struts too, and an alias that reports itself is
    # the difference between a measurement and a guess.
    pos = np.cumsum(rng.integers(17, 61, size=14)) + 12
    for x0 in pos[pos < n - 8]:
        base[:, x0:x0 + 6] = 1.0
    for y0 in (np.cumsum(rng.integers(19, 67, size=12)) + 9):
        if y0 < n - 8:
            base[y0:y0 + 6, :] = 0.0
    ok = True
    for want_ax, want_bx, want_ay, want_by in [
            (1.0, 0.0, 1.0, 0.0),
            (1.2892, 0.2425, 1.1931, -0.1932),
            (0.85, -0.15, 1.10, 0.10)]:
        out = np.zeros_like(base)
        for wch in (0, 1):
            src = base[:, :n] if wch == 0 else base[:, n:]
            yy, xx = np.mgrid[0:n, 0:n].astype(np.float64)
            cx = cy = (n - 1) / 2.0
            # inverse of x' = A(x-c)+c + B*(n/2)
            sxc = (xx - cx - want_bx * n / 2.0) / want_ax + cx
            syc = (yy - cy - want_by * n / 2.0) / want_ay + cy
            w = map_coordinates(src, [syc, sxc], order=1, mode="nearest")
            out[:, wch * n:(wch + 1) * n] = w
        Image.fromarray((base * 255).astype(np.uint8)).save("/tmp/_ra_a.png")
        Image.fromarray((out * 255).astype(np.uint8)).save("/tmp/_ra_b.png")
        # Both bands, every case. The 0-0.72 band is the one real screenshots
        # are measured over, and it is the one that exposed the centre bug: a
        # band whose centre is not the frame's biases B_y by
        # (A_y-1)*(frame centre - band centre), which for this band and
        # A_y=1.193 is 0.054 in NDC -- the same size as the coefficient itself.
        # A self-check that only ever ran on the full frame could not see it,
        # and did not.
        for band in ((0.0, 1.0), (0.0, 0.72)):
            r = register("/tmp/_ra_a.png", "/tmp/_ra_b.png", 0, band, 1.6, 260)
            eax, ebx = abs(r["A_x"] - want_ax), abs(r["B_x"] - want_bx)
            eay, eby = abs(r["A_y"] - want_ay), abs(r["B_y"] - want_by)
            good = eax < 0.012 and ebx < 0.012 and eay < 0.012 and eby < 0.012
            ok &= good
            print(f"{'ok  ' if good else 'FAIL'} band {band[0]:g}-{band[1]:g} "
                  f"want A_x={want_ax:.4f} B_x={want_bx:+.4f} "
                  f"A_y={want_ay:.4f} B_y={want_by:+.4f}  ->  "
                  f"got A_x={r['A_x']:.4f} B_x={r['B_x']:+.4f} "
                  f"A_y={r['A_y']:.4f} B_y={r['B_y']:+.4f}  "
                  f"margin {r['m_x']:.3f}/{r['m_y']:.3f}")
    print("self-check:", "PASSED" if ok else "FAILED")
    print("range: scale 0.63..1.60, shift +-260 px of a 512 px half "
          "(= +-1.02 in NDC). A transform outside that reads as the boundary,")
    print("       and a margin below ~0.05 means the profile had a competing "
          "alignment -- read it as unmeasured, not as the number printed.")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-check":
        sys.exit(self_check())
    if len(sys.argv) > 1 and sys.argv[1] == "--verify":
        # --verify ref meas A_x B_x A_y B_y [band_lo band_hi]
        # View 1's B_x is the negation of view 0's, which is the mirrored
        # frustum; everything else is shared.
        ax, bx, ay, by = (float(v) for v in sys.argv[4:8])
        lo = float(sys.argv[8]) if len(sys.argv) > 8 else 0.0
        hi = float(sys.argv[9]) if len(sys.argv) > 9 else 0.72
        pred = [(ax, bx, ay, by), (ax, -bx, ay, by)]
        for r in verify(sys.argv[2], sys.argv[3], pred, (lo, hi)):
            p = pred[r["view"]]
            print(f"view {r['view']}: corr identity={r['identity']:.4f}  "
                  f"predicted={r['predicted']:.4f}  best={r['best']:.4f}")
            print(f"        predicted A_x={p[0]:.4f} B_x={p[1]:+.4f} "
                  f"A_y={p[2]:.4f} B_y={p[3]:+.4f}")
            print(f"        best fit  A_x={r['at'][0]:.4f} "
                  f"B_x={r['at'][1]:+.4f} A_y={r['at'][2]:.4f} "
                  f"B_y={r['at'][3]:+.4f}")
        sys.exit(0)
    if len(sys.argv) < 3:
        print("usage: register_affine.py <reference.jpg> <measured.jpg> "
              "[band_lo] [band_hi]\n"
              "       register_affine.py --verify <ref> <meas> "
              "<A_x> <B_x> <A_y> <B_y> [band_lo] [band_hi]\n"
              "       register_affine.py --self-check")
        sys.exit(2)
    lo = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    hi = float(sys.argv[4]) if len(sys.argv) > 4 else 0.72
    for wch in (0, 1):
        r = register(sys.argv[1], sys.argv[2], wch, (lo, hi), 1.6, 400)
        warn = ("   <- MARGIN LOW, competing alignment: unmeasured"
                if min(r["m_x"], r["m_y"]) < 0.05 else "")
        print(f"view {wch}: A_x={r['A_x']:.4f} B_x={r['B_x']:+.4f} "
              f"(corr {r['q_x']:.3f} margin {r['m_x']:.3f})   "
              f"A_y={r['A_y']:.4f} B_y={r['B_y']:+.4f} "
              f"(corr {r['q_y']:.3f} margin {r['m_y']:.3f})   "
              f"[{r['w']}x{r['h']} half, band {lo:g}-{hi:g}]{warn}")
