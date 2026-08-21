#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Measure the Sun's per-eye difference in a pair of eye-image layers.

Task #49. **What is wrong is that a warm, red-dominant, radially extended glow
component is ABSENT in the right eye** -- not attenuated. Take 179 settled it by
looking: the left eye's Sun is a round orange glow at one moment and a large
diagonal orange plume a few seconds later; the right eye's is a smaller white
core with radial spikes, twice, essentially unchanged. The right eye's spikes
look *sharper* because nothing is washing them out.

    tools/sun_halo.py <layer0> <layer1> [--crop=out.png] [--profile]
    tools/sun_halo.py --prefix=/tmp/x4vr-t180 --crop=/tmp/c

THREE THINGS THIS TOOL GOT WRONG BEFORE, EACH OF WHICH COST A CONCLUSION:

1. **It anchored on argmax.** On a saturated plateau argmax returns the
   raster-first pixel, and the plateau has a different shape in each eye -- so
   the two eyes were centred on different parts of the disc and that
   misalignment went into the ratio. The anchor is now the CENTROID of the
   saturated core, which put dx at -1025.0 against a predicted -1024.36.
2. **It reported a band ratio.** A ratio of two numbers at one radius cannot
   distinguish "the glow is 25% weaker" from "a whole component is missing and
   what remains is a different thing". It said "the halo is 20-30% weaker",
   which was a real measurement of the wrong model. The warm excess below (R-B
   in linear light) measures the missing component directly.
3. **`same_object` was necessary but not sufficient.** present-n4 of take 179
   was a starfield with no Sun in it; a random warm star matched at dx=-1024 and
   the row scored a clean 1.002. A real solar disc is now required, and a row
   that has not got one says so instead of printing numbers.

**Look at the crop before believing the number.** `--crop=` writes both crops
side by side at 2x. Four separate mis-identifications in this task -- a planet
limb against an asteroid, a loadscreen against a cockpit, a HUD arc against the
Sun, and cockpit strut highlights against stars -- were all caught by looking,
and three of them had passed a numeric check first.
"""
import glob
import os
import re
import sys

import numpy as np
from scipy import ndimage as ndi
from PIL import Image

# Eye-to-eye offset for content at infinity: B_x * width = 0.24251 * 4224.
EXPECT_DX = -1024.0
TOL = 40.0
# A solar disc saturates a solid blob. Stars, HUD text and specular highlights
# on cockpit trim do not -- they were what the old finder kept locking onto.
MIN_CORE_PX = 150


def load(path):
    a = np.array(Image.open(path).convert('RGB'))
    c = a / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    return a, lin


def luma(x):
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def find_sun(lin, lum):
    """Centroid and area of the largest saturated blob, or None.

    Returns None rather than a best guess: a frame with no Sun in it must read
    as "no Sun", not as a clean result.
    """
    lab, n = ndi.label(lum >= 0.99)
    if n == 0:
        return None, 0
    sizes = ndi.sum(np.ones_like(lab), lab, range(1, n + 1))
    k = int(np.argmax(sizes)) + 1
    area = int(sizes[k - 1])
    if area < MIN_CORE_PX:
        return None, area
    cy, cx = ndi.center_of_mass(lum >= 0.99, lab, k)
    return (float(cx), float(cy)), area


def measure(p0, p1, crop=None, expect_dx=EXPECT_DX, profile=False):
    a0, l0 = load(p0)
    a1, l1 = load(p1)
    m0, m1 = luma(l0), luma(l1)
    s0, area0 = find_sun(l0, m0)
    s1, area1 = find_sun(l1, m1)
    if s0 is None or s1 is None:
        big = max(area0, area1)
        return {'error': f'no solar disc (largest saturated blob {big} px, '
                         f'needs {MIN_CORE_PX}) — this frame has no Sun in it'}
    (lx, ly), (rx, ry) = s0, s1
    dx = rx - lx
    h, w = m0.shape
    yy, xx = np.mgrid[0:h, 0:w]
    dl = np.hypot(xx - lx, yy - ly)
    dr = np.hypot(xx - rx, yy - ry)
    out = {'sunL': (round(lx, 1), round(ly, 1)), 'sunR': (round(rx, 1), round(ry, 1)),
           'dx': round(dx, 1), 'coreL': area0, 'coreR': area1,
           'same_object': abs(dx - expect_dx) < TOL and abs(ry - ly) < 25,
           'bands': [], 'warm': []}
    for a, b in ((0, 20), (20, 35), (35, 50), (50, 80)):
        i0 = (dl >= a) & (dl < b)
        i1 = (dr >= a) & (dr < b)
        c0, c1 = l0[i0].mean(0), l1[i1].mean(0)
        out['bands'].append((a, b, float(m1[i1].mean() / max(m0[i0].mean(), 1e-9)),
                             tuple(float(c1[k] / max(c0[k], 1e-9)) for k in range(3))))
        # The warm component itself: red minus blue in linear light. The core
        # is white in both eyes, so luminance hides the difference that the
        # orange haze makes; R-B does not.
        wl = float(c0[0] - c0[2]) if (i0.sum()) else 0.0
        wr = float(c1[0] - c1[2]) if (i1.sum()) else 0.0
        out['warm'].append((a, b, wl, wr, wr / wl if abs(wl) > 1e-6 else float('nan')))
    if profile:
        out['profile'] = []
        for r in range(0, 260, 10):
            i0 = (dl >= r) & (dl < r + 10)
            i1 = (dr >= r) & (dr < r + 10)
            out['profile'].append((r, float(m0[i0].mean()), float(m1[i1].mean())))
    if crop:
        rd, z = 150, 2
        ca = a0[max(0, int(ly) - rd):int(ly) + rd, max(0, int(lx) - rd):int(lx) + rd]
        cb = a1[max(0, int(ry) - rd):int(ry) + rd, max(0, int(rx) - rd):int(rx) + rd]
        n = min(ca.shape[0], cb.shape[0])
        m = min(ca.shape[1], cb.shape[1])
        if n > 0 and m > 0:
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
    flag = '' if r['same_object'] else '   <- NOT THE SAME OBJECT, ratios meaningless'
    print(f"{tag:<16} sunL={str(r['sunL']):<18} sunR={str(r['sunR']):<18} "
          f"dx={r['dx']:>8}{flag}")
    print(f"{'':<16} core {r['coreL']}/{r['coreR']} px  "
          f"(R/L {r['coreR'] / max(r['coreL'], 1):.3f})   luma "
          + "  ".join(f"{a}-{b}:{v:.3f}" for a, b, v, _ in r['bands']))
    # The headline. A component that is present in one eye and absent in the
    # other shows here as a ratio far below 1 while luma looks merely dented.
    #
    # A ratio is only printed where the left eye HAS a warm component to be
    # missing. Inside the saturated core both eyes read ~0 and the quotient
    # explodes -- band 0-20 printed 40.63 for take 179's n10, which is two
    # zeros dividing, not a forty-fold excess.
    FLOOR = 0.01
    w = "  ".join(f"{a}-{b}:" + (f"{wr / wl:.2f}" if abs(wl) >= FLOOR else "--")
                  for a, b, wl, wr, _ in r['warm'])
    a, b, wl, wr, _ = r['warm'][2]
    print(f"{'':<16} WARM EXCESS (R-B, linear) R/L by band  {w}"
          + (f"   crop -> {r['crop']}" if 'crop' in r else ''))
    if all(abs(x) < FLOOR and abs(y) < FLOOR for _, _, x, y, _ in r['warm']):
        print(f"{'':<16} neutral in BOTH eyes — no warm component anywhere, so "
              f"this blob is probably not the Sun. Look at the crop.")
        return
    note = ""
    if abs(wl) >= FLOOR:
        note = ("   <- the warm component is largely ABSENT in the right eye"
                if wr / wl < 0.75 else "   <- both eyes carry it")
    print(f"{'':<16} at {a}-{b} px: left {wl:.4f}, right {wr:.4f}{note}")
    if 'profile' in r:
        print(f"{'':<16} {'r':>4} {'lumL':>8} {'lumR':>8} {'R/L':>7}")
        for rr, v0, v1 in r['profile']:
            print(f"{'':<16} {rr:>4} {v0:8.4f} {v1:8.4f} "
                  f"{v1 / max(v0, 1e-9):7.3f}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = {a.split('=')[0]: (a.split('=', 1)[1] if '=' in a else '1')
            for a in sys.argv[1:] if a.startswith('--')}
    prof = '--profile' in opts
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

        # Sorted so a bisect reads in pass order, not shell glob order.
        def key(t):
            m = re.search(r'afterrp(\d+)', t[0])
            if m:
                return (0, int(m.group(1)), 0)
            m = re.search(r'n(\d+)$', t[0])
            return (1, int(m.group(1)) if m else 0, 0)
        for tag, p0, p1 in sorted(pairs, key=key):
            report(tag, measure(p0, p1, profile=prof,
                                crop=opts.get('--crop') and
                                f"{opts['--crop']}-{tag}.png" or None))
        return 0
    if len(args) < 2:
        print(__doc__.strip())
        return 2
    report(os.path.basename(args[0]),
           measure(args[0], args[1], crop=opts.get('--crop'), profile=prof))
    return 0


if __name__ == '__main__':
    sys.exit(main())
