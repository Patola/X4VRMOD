#!/usr/bin/env python3
"""Did X4's camera rotate between two presented frames, and by how much?

Usage:
    tools/camera_rotation.py <a.ppm> <b.ppm> [--fov-half-deg 55] [--max-shift 128]
    tools/camera_rotation.py --selftest <any.ppm>
    tools/camera_rotation.py --series <prefix> [--layer 0]

#33 needs an instrument that can tell "the camera rotated" from "nothing
happened" *from the log and the dumps*, never from a description of the screen.
The camera uniform block cannot be that instrument -- see the refutation below
-- so this measures the only thing left, which is also the most direct: a
rotation moves every pixel, and the presented eye image is already dumped.

**Why the camera block was refuted, offline, before this file existed.**
`X4VR_DUMP_MATRICES=1` logs `M_view`, `M_viewprojection` and `M_viewinverse`.
Across fourteen takes (52-58, 104-110, 112, 113) all three are *byte-identical*
every time: view and viewinverse are the identity, viewprojection is the same
pure projection. Two independent reasons, either of which is fatal:

  1. X4 renders camera-relative. The quirks survey found `M_view` identity in
     all 128 blocks of the arena; the camera orientation is baked into the
     per-object `M_worldviewprojection` on the CPU and never appears here.
  2. The dump is gated on `g_track.logged_matrices`, so it fires **once**, at
     the first frame with 50+ credited draws -- the menu. A one-shot sample
     cannot show a change by construction, so it could not have answered this
     question even if the matrix did rotate.

Fourteen takes agreeing byte-for-byte across a huge range of knobs is the
"same number every run" alarm, and this is what was behind it.

**What separates rotation from translation: depth.** Under a pure camera
rotation every pixel moves by the same amount *regardless of its distance* --
that is what makes rotation-only timewarp exact. Under ship translation, near
geometry moves further than far geometry. So the shape of the per-block shift
distribution is the verdict, and reporting a mean over it would destroy exactly
the structure being measured:

    all blocks at 0                -> NOTHING HAPPENED
    tight peak away from 0         -> ROTATED (by that shift)
    broad spread of shifts         -> TRANSLATED (parallax), not a rotation
    peak at 0 *and* peak away      -> ROTATED, with screen-fixed pixels
                                      (cockpit frame, HUD, UI) held at 0

That last case is the normal one in a cockpit, and it carries its own control:
the cockpit is rigidly attached to the camera, so those blocks *must* read 0.
If they drift with the world, the estimator is reporting a global offset rather
than a measurement, and the answer is not to be trusted.

This reports the whole distribution and never a mean. Two of this project's
metrics have already reported "no defect" on frames that were visibly wrong.
"""
import sys

import numpy as np

# Big enough that phase correlation can resolve a shift of +-BLOCK/2, which at
# 1408 px and +-55 deg is +-14.9 deg -- past any plausible per-frame rotation.
BLOCK = 256
STRIDE = 128
# A block has to carry some structure before its best match means anything.
# Empty space (and X4 has a lot of it) matches everything equally well at every
# shift, and those blocks would otherwise pile into whichever bin won by noise.
MIN_STD = 3.0
# Correlation peak sharpness: peak height over the surface's own std. Below
# this the block matched nothing in particular.
MIN_SNR = 4.0
# How tight a population has to be, in pixels, to count as one rigid motion
# rather than a parallax spread.
TIGHT_PX = 2.0
# A handful of stray blocks is not a motion. Take 115 produced three "ROTATED"
# verdicts off 1, 2 and 3 moved blocks, where the spread is ~0 *by
# construction* -- the scatter of two points about their own median is zero
# however they lie. A rule that cannot fail is not a measurement.
MIN_MOVED = 8
MIN_MOVED_FRAC = 0.10
# Directional coherence: the resultant length of the moved blocks' unit
# direction vectors. Real motion -- rotation OR parallax -- moves the frame one
# way with varying magnitude, so the directions agree and R is near 1. Blocks
# matching noise point everywhere and R falls to ~0.
#
# This exists because take 115's cockpit frames reported "100 blocks moved,
# spread 50 px" with a median of 3 px: frames that differ only by exposure
# drift and animation, where the correlator finds a peak somewhere in every
# block and none of them mean anything. Reporting that as "TRANSLATED
# (parallax)" asserted a physical cause that had not been established, which is
# the take-97 mistake verbatim. Magnitude spread alone cannot separate the two
# -- real parallax is legitimately spread out -- but direction can.
MIN_COHERENCE = 0.70
MAX_INCOHERENCE = 0.40


def load_ppm(path):
    """Read a binary P6 PPM. Comments would shift the pixel offset, so they are
    parsed rather than assumed absent."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM (P6)")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval}, only 8-bit is written")
    px = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos + 1)
    return px.reshape(h, w, 3).astype(np.float32)


def gray(img):
    return img @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


_HANN = {}


def hann(n):
    """Windowed, because phase correlation on an unwindowed block correlates
    the block *edges* -- which are identical in both frames -- and reports 0."""
    if n not in _HANN:
        w = np.hanning(n).astype(np.float32)
        _HANN[n] = np.outer(w, w)
    return _HANN[n]


def block_shift(a, b):
    """Shift (sx, sy) such that b(x, y) ~ a(x - sx, y - sy): how far the content
    moved from a to b. Returns None if the block is featureless or the
    correlation peak is not convincing."""
    if a.std() < MIN_STD or b.std() < MIN_STD:
        return None
    n = a.shape[0]
    w = hann(n)
    fa = np.fft.rfft2((a - a.mean()) * w)
    fb = np.fft.rfft2((b - b.mean()) * w)
    cross = fb * np.conj(fa)
    mag = np.abs(cross)
    mag[mag < 1e-9] = 1e-9
    surf = np.fft.irfft2(cross / mag, s=a.shape)
    peak = np.unravel_index(np.argmax(surf), surf.shape)
    hi = surf[peak]
    snr = hi / (surf.std() + 1e-9)
    if snr < MIN_SNR:
        return None

    # Subpixel: parabolic fit through the peak and its two neighbours per axis,
    # on the wrapped surface.
    def refine(axis):
        i = peak[axis]
        lo = surf[(i - 1) % n, peak[1]] if axis == 0 else surf[peak[0], (i - 1) % n]
        up = surf[(i + 1) % n, peak[1]] if axis == 0 else surf[peak[0], (i + 1) % n]
        d = 2.0 * hi - lo - up
        frac = 0.5 * (up - lo) / d if abs(d) > 1e-9 else 0.0
        v = i + float(np.clip(frac, -0.5, 0.5))
        return v - n if v > n / 2 else v

    return refine(1), refine(0)


def coarse_shift(a, b):
    """Whole-frame phase correlation. Returns (dx, dy, snr).

    **This stage exists because the tool was blind to exactly what it was
    built to measure.** Phase correlation on a block of N pixels is
    unambiguous only to +-N/2, so with BLOCK=256 the per-block pass tops out
    at +-128 px, which at 1408 px and +-55 deg is **+-10 deg**. Take 115's
    free-look moves were +24 deg. They aliased, every block returned a
    different piece of nonsense, and the run was scored NO COHERENT MOTION and
    written up as a null. Patola had already read the moves straight off the
    PPMs by eye.

    A full frame reaches +-w/2, which is +-the half-FOV by construction --
    +-55 deg here -- and covers free-look. The lesson is the general one:
    **state an instrument's measurable range before believing a null from
    it.** A silent aliasing limit turns a large real signal into confident
    noise.

    One caveat on the per-block pass once the coarse shift is large: a big
    camera rotation is *not* a translation of a rectilinear image. Linear in
    tangent, not angle -- so the centre and the edges shift by different
    amounts and the residual spread is genuinely wide (41 px at 24 deg). The
    `ROTATED` / `TRANSLATED` split is only meaningful for SMALL motions; for
    large ones read the coarse shift and the coherence, which was 0.99 across
    take 115's +24 deg free-look.
    """
    n = a.shape[0]
    win = np.outer(np.hanning(n), np.hanning(n)).astype(np.float32)
    fa = np.fft.rfft2((a - a.mean()) * win)
    fb = np.fft.rfft2((b - b.mean()) * win)
    cross = fb * np.conj(fa)
    mag = np.abs(cross)
    mag[mag < 1e-9] = 1e-9
    surf = np.fft.irfft2(cross / mag, s=a.shape)
    p = np.unravel_index(np.argmax(surf), surf.shape)
    dy = p[0] - n if p[0] > n / 2 else p[0]
    dx = p[1] - n if p[1] > n / 2 else p[1]
    return int(dx), int(dy), float(surf[p] / (surf.std() + 1e-9))


def measure(a_img, b_img, coarse=(0, 0)):
    """Per-block shifts over the whole frame. Returns (shifts, coords).

    `coarse` pre-aligns b so the per-block pass only ever sees the small
    residual, keeping it inside its own +-BLOCK/2 range."""
    a, b = gray(a_img), gray(b_img)
    if coarse != (0, 0):
        b = np.roll(b, (-coarse[1], -coarse[0]), axis=(0, 1))
    h, w = a.shape
    shifts, coords = [], []
    for y in range(0, h - BLOCK + 1, STRIDE):
        for x in range(0, w - BLOCK + 1, STRIDE):
            s = block_shift(a[y:y + BLOCK, x:x + BLOCK],
                            b[y:y + BLOCK, x:x + BLOCK])
            if s is not None:
                shifts.append(s)
                coords.append((x + BLOCK // 2, y + BLOCK // 2))
    return np.array(shifts, dtype=np.float64).reshape(-1, 2), coords


def deg_of_px(px, width, fov_half_deg):
    """Rectilinear, so linear in TANGENT, not angle -- the project's central
    lesson and the reason a pixel near the edge is worth less than one at the
    centre. Reported as the angle subtended at the centre."""
    t = np.tan(np.radians(fov_half_deg))
    return np.degrees(np.arctan(np.asarray(px) * t / (width / 2.0)))


def verdict(shifts, width, fov_half_deg):
    """Describe the distribution. Never summarise it to one number."""
    if len(shifts) < 4:
        return "UNSCOREABLE", f"only {len(shifts)} blocks carried enough structure"

    mag = np.hypot(shifts[:, 0], shifts[:, 1])
    still = mag < 0.5
    moved = ~still
    n_still, n_moved = int(still.sum()), int(moved.sum())

    if n_moved == 0:
        return "NOTHING HAPPENED", f"all {n_still} blocks at 0"
    if n_moved < MIN_MOVED or n_moved < MIN_MOVED_FRAC * len(shifts):
        return "NOTHING HAPPENED", (
            f"{n_still} blocks at 0, {n_moved} stray — below the "
            f"{MIN_MOVED}-block floor, so no verdict is claimed")

    mv = shifts[moved]
    # Spread of the moving population about its own median, which is what
    # separates one rigid motion from depth parallax.
    med = np.median(mv, axis=0)
    spread = float(np.median(np.hypot(mv[:, 0] - med[0], mv[:, 1] - med[1])))
    dx_deg, dy_deg = deg_of_px(med, width, fov_half_deg)

    # Magnitude-weighted, so a 0.6 px block does not get the same vote as a
    # 79 px one: |sum(v)| / sum(|v|). Unweighted, genuine take-101 ship motion
    # scored 0.5-0.87 purely because near-still blocks diluted the direction.
    coh = float(np.hypot(*mv.sum(axis=0)) /
                max(np.hypot(mv[:, 0], mv[:, 1]).sum(), 1e-9))

    tail = (f"{n_moved} blocks moved by ({med[0]:+.2f}, {med[1]:+.2f}) px "
            f"= ({dx_deg:+.3f}, {dy_deg:+.3f}) deg, spread {spread:.2f} px, "
            f"coherence {coh:.2f}; {n_still} blocks held at 0")

    if coh < MAX_INCOHERENCE:
        return "NO COHERENT MOTION", (
            f"directions disagree (coherence {coh:.2f}); the frames differ but "
            f"not by any rigid transform — exposure drift, animation or noise. "
            f"{n_moved} blocks, median ({med[0]:+.2f}, {med[1]:+.2f}) px, "
            f"{n_still} at 0")
    if coh < MIN_COHERENCE:
        return "AMBIGUOUS", tail
    return ("ROTATED" if spread <= TIGHT_PX else "TRANSLATED (parallax)"), tail


def report(a_path, b_path, fov_half_deg):
    a_img, b_img = load_ppm(a_path), load_ppm(b_path)
    if a_img.shape != b_img.shape:
        raise ValueError(f"shape mismatch: {a_img.shape} vs {b_img.shape}")
    w = a_img.shape[1]
    cdx, cdy, csnr = coarse_shift(gray(a_img), gray(b_img))
    shifts, coords = measure(a_img, b_img, coarse=(cdx, cdy))
    if len(shifts):
        shifts = shifts + np.array([cdx, cdy], dtype=np.float64)
    what, how = verdict(shifts, w, fov_half_deg)
    print(f"{a_path}")
    print(f"  -> {b_path}")
    print(f"  whole-frame {cdx:+d},{cdy:+d} px = "
          f"({float(deg_of_px(cdx, w, fov_half_deg)):+.2f}, "
          f"{float(deg_of_px(cdy, w, fov_half_deg)):+.2f}) deg  snr {csnr:.0f}"
          f"   [range +-{w // 2} px = "
          f"+-{float(deg_of_px(w // 2, w, fov_half_deg)):.1f} deg]")
    print(f"  {what}: {how}")
    if len(shifts):
        mag = np.hypot(shifts[:, 0], shifts[:, 1])
        qs = np.percentile(mag, [0, 25, 50, 75, 90, 100])
        print("  |shift| px  min/p25/p50/p75/p90/max  "
              + "/".join(f"{q:.2f}" for q in qs))
    return what, shifts


def selftest(path, fov_half_deg):
    """Recover known shifts from real X4 pixels.

    The pair is cut as two overlapping windows of one frame, so the shift is
    exact and there is no wrap-around edge to explain away. The identity case
    is the negative control and must read exactly 0 -- an estimator that cannot
    return 0 cannot report "nothing happened", which is half of what #33 asks.
    """
    img = load_ppm(path)
    h, w = img.shape[:2]
    pad = 160
    if h < 2 * pad + BLOCK or w < 2 * pad + BLOCK:
        raise ValueError(f"{path}: {w}x{h} is too small to cut a selftest pair")

    def window(dx, dy):
        return img[pad + dy:h - pad + dy, pad + dx:w - pad + dx]

    cases = [(0, 0), (1, 0), (-3, 2), (17, -9), (-48, 33), (96, 96)]
    worst = 0.0
    worst_spread = [0.0]
    fails = 0
    for dx, dy in cases:
        a, b = window(0, 0), window(dx, dy)
        # b samples the source further along +x/+y, so content sits EARLIER in
        # b: it moved by (-dx, -dy).
        want = (-dx, -dy)
        shifts, _ = measure(a, b)
        if len(shifts) < 4:
            print(f"  shift {want}: UNSCOREABLE ({len(shifts)} blocks)")
            fails += 1
            continue
        med = np.median(shifts, axis=0)
        err = float(np.hypot(med[0] - want[0], med[1] - want[1]))
        worst = max(worst, err)
        # A rigid whole-frame translation is what a camera rotation looks like,
        # so this is the spread TIGHT_PX has to sit above. Measured, not
        # assumed: the threshold that separates rotation from parallax is only
        # meaningful against a known-rigid case, and this is the only one we
        # can build without spending a take.
        spread = float(np.median(np.hypot(shifts[:, 0] - med[0],
                                          shifts[:, 1] - med[1])))
        worst_spread[0] = max(worst_spread[0], spread)
        ok = err < 0.25 and spread < TIGHT_PX
        fails += not ok
        print(f"  want ({want[0]:+4d},{want[1]:+4d})  got "
              f"({med[0]:+7.3f},{med[1]:+7.3f})  err {err:.4f} px  "
              f"spread {spread:.4f} px  "
              f"{'ok' if ok else 'FAIL'}  ({len(shifts)} blocks)")
    print(f"  worst error {worst:.4f} px "
          f"= {abs(float(deg_of_px(worst, w, fov_half_deg))):.4f} deg")
    print(f"  worst rigid-motion spread {worst_spread[0]:.4f} px "
          f"(TIGHT_PX = {TIGHT_PX}, so a rigid motion is separated from "
          f"parallax by {TIGHT_PX / max(worst_spread[0], 1e-6):.0f}x)")
    print("SELFTEST", "PASS" if fails == 0 else f"FAIL ({fails} cases)")
    return fails == 0


def main(argv):
    fov_half_deg = 55.0
    layer = 0
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == "--fov-half-deg":
            fov_half_deg = float(argv[i + 1])
            i += 2
        elif argv[i] == "--layer":
            layer = int(argv[i + 1])
            i += 2
        else:
            args.append(argv[i])
            i += 1

    if args and args[0] == "--selftest":
        return 0 if selftest(args[1], fov_half_deg) else 1

    if args and args[0] == "--series":
        import glob
        import re
        prefix = args[1]
        paths = glob.glob(f"{prefix}-n*-layer{layer}.ppm")
        if not paths:
            print(f"no dumps match {prefix}-n*-layer{layer}.ppm")
            return 1
        paths.sort(key=lambda p: int(re.search(r"-n(\d+)-", p).group(1)))
        for a, b in zip(paths, paths[1:]):
            report(a, b, fov_half_deg)
        return 0

    if len(args) != 2:
        print(__doc__)
        return 2
    report(args[0], args[1], fov_half_deg)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
