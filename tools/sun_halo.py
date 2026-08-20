#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Measure the Sun's per-eye glow difference in a pair of eye-image layers.

Task #49. The defect is NOT a brightness difference: the Sun's core is saturated
in both eyes (peak 1.0000) and identical, while the warm halo around it is
20-30% weaker in the right eye, red-dominant. It reads as "the right Sun is
brighter" because a white core unmuted by orange haze looks brighter than the
same core inside one.

    tools/sun_halo.py <layer0> <layer1> [--crop out.png] [--dx -1024]
    tools/sun_halo.py --prefix=/tmp/x4vr-t179    # every pair it can find

**FINDING THE SUN IS THE HARD PART, AND EVERY WRONG ANSWER IN THIS TASK CAME
FROM GETTING IT WRONG.** "Brightest blob" fails: in a cockpit frame the brightest
thing is the cyan HUD arc, and on a loading screen it is a planet. Three separate
conclusions were built on mismatched objects -- a planet limb against an
asteroid, a loadscreen against a cockpit, a HUD arc against the Sun -- and two of
them passed a correlation check. So this looks for the WARM peak (R > 1.15*B) in
the upper band, reports `dx` against what the cant predicts, and can write the
crop it measured. **Look at the crop before believing the number.**

Expected `dx` is the eye-to-eye offset for content at infinity:
    B_x * width  =  0.24251 * 4224 / 2 * 2  =  1024 px
Anything else means the two eyes' peaks are different objects, and the ratio
below is then meaningless rather than merely noisy.
"""
import glob
import os
import re
import sys

import numpy as np
from scipy import ndimage as ndi          # noqa: F401  (kept for parity)
from PIL import Image

EXPECT_DX = -1024.0
TOL = 40.0


def load(path):
    a = np.array(Image.open(path).convert('RGB'))
    c = a / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    return a, lin


def luma(x):
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def find_sun(lin, lum):
    """The warm peak in the upper band — not the brightest pixel in the frame."""
    h = lum.shape[0]
    lo, hi = int(h * 0.15), int(h * 0.60)
    warm = lin[lo:hi, ..., 0] > lin[lo:hi, ..., 2] * 1.15
    m = np.where(warm, lum[lo:hi], 0.0)
    if m.max() <= 0:
        return None
    y, x = np.unravel_index(int(np.argmax(m)), m.shape)
    return int(x), int(y) + lo


def measure(p0, p1, crop=None, expect_dx=EXPECT_DX):
    a0, l0 = load(p0)
    a1, l1 = load(p1)
    m0, m1 = luma(l0), luma(l1)
    s0, s1 = find_sun(l0, m0), find_sun(l1, m1)
    if not s0 or not s1:
        return {'error': 'no warm peak in one of the layers'}
    (lx, ly), (rx, ry) = s0, s1
    dx = rx - lx
    h, w = m0.shape
    yy, xx = np.mgrid[0:h, 0:w]
    dl = np.hypot(xx - lx, yy - ly)
    dr = np.hypot(xx - rx, yy - ry)
    out = {'sunL': (lx, ly), 'sunR': (rx, ry), 'dx': dx,
           'same_object': abs(dx - expect_dx) < TOL and abs(ry - ly) < 25,
           'peakL': float(m0[dl < 6].max()), 'peakR': float(m1[dr < 6].max()),
           'bands': []}
    for a, b in ((0, 20), (20, 35), (35, 50), (50, 80)):
        i0 = (dl >= a) & (dl < b)
        i1 = (dr >= a) & (dr < b)
        c0, c1 = l0[i0].mean(0), l1[i1].mean(0)
        out['bands'].append((a, b, float(m1[i1].mean() / max(m0[i0].mean(), 1e-9)),
                             tuple(float(c1[k] / max(c0[k], 1e-9)) for k in range(3))))
    if crop:
        rd, z = 150, 2
        ca = a0[max(0, ly - rd):ly + rd, max(0, lx - rd):lx + rd]
        cb = a1[max(0, ry - rd):ry + rd, max(0, rx - rd):rx + rd]
        n = min(ca.shape[0], cb.shape[0])
        m = min(ca.shape[1], cb.shape[1])
        za = np.kron(ca[:n, :m], np.ones((z, z, 1), np.uint8))
        zb = np.kron(cb[:n, :m], np.ones((z, z, 1), np.uint8))
        img = np.full((za.shape[0], za.shape[1] * 2 + 12, 3), 35, np.uint8)
        img[:, :za.shape[1]] = za
        img[:, za.shape[1] + 12:] = zb
        Image.fromarray(img).save(crop)
        out['crop'] = crop
    return out


def report(tag, r):
    if 'error' in r:
        print(f"{tag:<16} {r['error']}")
        return
    flag = '' if r['same_object'] else '   <- NOT THE SAME OBJECT, ratio meaningless'
    print(f"{tag:<16} sunL={str(r['sunL']):<14} sunR={str(r['sunR']):<14} "
          f"dx={r['dx']:>7}{flag}")
    print(f"{'':<16} peak {r['peakL']:.4f}/{r['peakR']:.4f}   "
          + "  ".join(f"{a}-{b}:{v:.3f}" for a, b, v, _ in r['bands']))
    a, b, v, rgb = r['bands'][2]
    print(f"{'':<16} halo {a}-{b} R/G/B {rgb[0]:.2f}/{rgb[1]:.2f}/{rgb[2]:.2f}"
          + (f"   crop -> {r['crop']}" if 'crop' in r else ''))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=')[0]: (a.split('=', 1)[1] if '=' in a else '1')
            for a in sys.argv[1:] if a.startswith('--')}
    if '--prefix' in opts:
        pre = opts['--prefix']
        pairs = []
        for p0 in sorted(glob.glob(f"{pre}-*layer0.png")):
            p1 = p0.replace('layer0', 'layer1')
            if os.path.exists(p1):
                pairs.append((os.path.basename(p0).replace(pre.split('/')[-1] + '-', '')
                              .replace('-layer0.png', ''), p0, p1))
        if not pairs:
            print(f"no layer0/layer1 pairs under {pre}-*")
            return 1
        # Sorted so the bisect reads in pass order, not shell glob order.
        def key(t):
            m = re.search(r'afterrp(\d+)', t[0])
            return (0, int(m.group(1))) if m else (1, t[0])
        for tag, p0, p1 in sorted(pairs, key=key):
            report(tag, measure(p0, p1, crop=opts.get('--crop') and
                                f"{opts['--crop']}-{tag}.png" or None))
        return 0
    if len(args) < 2:
        print(__doc__.strip())
        return 2
    report(os.path.basename(args[0]), measure(args[0], args[1],
                                              crop=opts.get('--crop')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
