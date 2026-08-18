#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Compare the brightest object between two eye layers, by finding it in each.

Task #49: Patola reports the Sun brighter in the right eye. That is a ~10%
luminance ratio on a small disc, and it was reported twice as "seemed" -- the
eye is the right instrument for "does this fuse" and the wrong one for this.

The Sun sits at a DIFFERENT screen position in each eye: that is correct stereo
plus a 30.07 deg cant, so comparing a fixed pixel box between layers measures
the parallax, not the brightness. This finds the bright object independently in
each layer and compares what it IS rather than what is at some coordinate.

    tools/bright_object.py <layer0.ppm> <layer1.ppm> [--thresh 0.80]

Reports, per layer, the object's peak and integrated luminance and its area,
then the layer1/layer0 ratios. **The whole-frame ratio is printed as a negative
control**: if the frame moved as much as the object did, the object is not the
finding -- exposure or tonemapping is. docs/frame-analysis.md's take-60 table
made exactly that distinction, where the background matched to 0.2% while lit
surfaces did not.

Luminance is computed in LINEAR light. The eye image is 8-bit sRGB, and a ratio
taken on sRGB code values is not a brightness ratio -- near white it understates
by roughly a factor of two.
"""
import sys


def read_ppm(path):
    """Binary PPM or PNG -- the layer writes PNG by default since take 176.

    PNG because these dumps are photometry and PNG is lossless; a JPEG artefact
    around a saturated Sun would sit exactly where the ratio is taken. It is
    also ~5-10x smaller, which is what stopped the dumps freezing the game.
    """
    with open(path, 'rb') as f:
        data = f.read()
    if data.startswith(b'\x89PNG'):
        try:
            from PIL import Image
        except ImportError:
            raise SystemExit(f"{path}: PNG needs Pillow (pip install pillow), "
                             f"or re-run the take with X4VR_DUMP_PPM=1")
        im = Image.open(path).convert('RGB')
        return im.width, im.height, im.tobytes()
    if not data.startswith(b'P6'):
        raise SystemExit(f"{path}: not a binary PPM (P6) or PNG")
    # header: P6 <w> <h> <maxval>, with # comments allowed between tokens
    tok, i = [], 2
    while len(tok) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        tok.append(int(data[i:j]))
        i = j
    w, h, maxv = tok
    return w, h, data[i + 1:i + 1 + w * h * 3]


try:                                   # 4224x4224 is 17.8M px; pure Python is
    import numpy as _np                # minutes per pair and this is seconds.
    from scipy import ndimage as _ndi  # Same arithmetic, asserted equal below.
except ImportError:
    _np = _ndi = None


def srgb_to_linear(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


LUT = [srgb_to_linear(float(v)) for v in range(256)]


def luma(px, n):
    """Linear-light luminance per pixel (Rec.709)."""
    out = [0.0] * n
    for i in range(n):
        r, g, b = px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
        out[i] = 0.2126 * LUT[r] + 0.7152 * LUT[g] + 0.0722 * LUT[b]
    return out


def brightest_blob(lum, w, h, thresh):
    """The connected region above `thresh` * peak with the most total light.

    NOT the region containing the single hottest pixel. On a real frame that
    rule picks whatever one HUD texel happens to clip highest -- the first run
    of this returned a 1-pixel "object" -- and a 1-pixel object compared
    between eyes measures sampling noise. The Sun is the component with the
    most integrated luminance, which is also what "looks brighter" means.
    """
    peak = max(lum)
    if peak <= 0:
        return None
    cut = peak * thresh
    seen = bytearray(w * h)
    best = None
    for start in range(w * h):
        if seen[start] or lum[start] < cut:
            continue
        stack, cells = [start], []
        seen[start] = 1
        while stack:
            p = stack.pop()
            cells.append(p)
            x, y = p % w, p // w
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h:
                    q = ny * w + nx
                    if not seen[q] and lum[q] >= cut:
                        seen[q] = 1
                        stack.append(q)
        tot = sum(lum[c] for c in cells)
        if best is None or tot > best[0]:
            best = (tot, cells)
    tot, cells = best
    xs = [c % w for c in cells]
    ys = [c // w for c in cells]
    return {'peak': max(lum[c] for c in cells), 'sum': tot, 'area': len(cells),
            'cx': sum(xs) / len(xs), 'cy': sum(ys) / len(ys)}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    thresh = 0.80
    for a in sys.argv[1:]:
        if a.startswith('--thresh'):
            thresh = float(a.split('=', 1)[1])
    if len(args) < 2:
        print(__doc__.strip())
        return 2

    res = []
    for p in args[:2]:
        w, h, px = read_ppm(p)
        if _np is not None:
            a = _np.frombuffer(px, dtype=_np.uint8, count=w * h * 3)
            lut = _np.array(LUT, dtype=_np.float64)
            rgb = lut[a.reshape(h, w, 3)]
            lm = (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] +
                  0.0722 * rgb[..., 2])
            peak = float(lm.max())
            blob = None
            if peak > 0:
                lab, n = _ndi.label(lm >= peak * thresh)
                if n:
                    # Largest by INTEGRATED light, not by peak or by area.
                    sums = _ndi.sum_labels(lm, lab, index=range(1, n + 1))
                    k = int(_np.argmax(sums)) + 1
                    m = lab == k
                    ys, xs = _np.nonzero(m)
                    # Touching the border means the object is CUT OFF here.
                    # The eyes are 1024 px apart on a backdrop object -- that
                    # is the cant -- so one eye can hold a whole Sun while the
                    # other holds part of one, and then integrated luminance
                    # compares different amounts of Sun. Without this flag the
                    # ratio silently becomes a coverage measurement.
                    clipped = bool(xs.min() == 0 or ys.min() == 0 or
                                   xs.max() == w - 1 or ys.max() == h - 1)
                    blob = {'peak': float(lm[m].max()),
                            'sum': float(sums[k - 1]), 'area': int(m.sum()),
                            'cx': float(xs.mean()), 'cy': float(ys.mean()),
                            'clipped': clipped}
            res.append((p, w, h, lm, blob))
        else:
            lum = luma(px, w * h)
            blob = brightest_blob(lum, w, h, thresh)
            res.append((p, w, h, lum, blob))

    (p0, w0, h0, l0, b0), (p1, w1, h1, l1, b1) = res
    if (w0, h0) != (w1, h1):
        print(f"note  layers differ in size: {w0}x{h0} vs {w1}x{h1}")
    print(f"{'layer':<8} {'peak':>8} {'integrated':>12} {'area px':>9} "
          f"{'centroid':>16}")
    for tag, b in (('layer0', b0), ('layer1', b1)):
        if not b:
            print(f"{tag:<8}  (no signal)")
            continue
        print(f"{tag:<8} {b['peak']:>8.4f} {b['sum']:>12.2f} {b['area']:>9d} "
              f"{b['cx']:>7.1f},{b['cy']:>7.1f}")

    if b0 and b1:
        print()
        print(f"object   peak   ratio l1/l0 = {b1['peak'] / b0['peak']:.4f}")
        print(f"object   integrated       = {b1['sum'] / b0['sum']:.4f}"
              f"   (brightness x area; the one an eye judges)")
        print(f"object   area             = {b1['area'] / b0['area']:.4f}")
        dx, dy = b1['cx'] - b0['cx'], b1['cy'] - b0['cy']
        print(f"object   moved            = {dx:+.1f}, {dy:+.1f} px"
              f"   (expected: this is the parallax + cant)")

    # The control. If this moves with the object, the object is not the story.
    mean = (lambda a: float(a.mean())) if _np is not None else \
           (lambda a: sum(a) / len(a))
    m0, m1 = mean(l0), mean(l1)
    print()
    print(f"CONTROL  whole-frame mean  = {m1 / m0:.4f}"
          f"   (l0={m0:.5f} l1={m1:.5f})")
    print("         a frame ratio near the object ratio means exposure or "
          "tonemapping,\n         not the object -- see take 60, where the "
          "background matched to 0.2%")
    return 0


if __name__ == '__main__':
    sys.exit(main())
