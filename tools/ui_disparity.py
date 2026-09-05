#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Is X4's UI at the depth the canvas claims? Measure it per eye, per tile.

    tools/ui_disparity.py '/tmp/x4vr-t256-present-n*-layer0.png' --expect 22.5

Patola has reported the same defect unprompted at takes 253, 255 and 256: the
menu text and the savegame loadscreens have **visibly wrong per-eye
separation**. This measures it, and it needs no run — every present dump already
writes both eye layers of the SAME instant, losslessly.

**This is the easy correlation, unlike everything else in this file.** The two
layers are the same scene at the same moment differing by a pure horizontal
translation: no rotation, no new content entering, no time between them. That is
the one configuration where NCC is unambiguous, and it is why this measurement
can be believed where the cross-time ones could not.

**The prediction is exact.** `canvas: 1.400 m -> s=0.01601 NDC (L=+s R=-s)` and
`s` is the PER-EYE shift, so the layer-to-layer disparity is 2s:

    2 * 0.01601 NDC * (1408/2) px  =  22.5 px

Independently, from the angles: at the centre the scale is 704*sx = 8.60 px/deg
(not the 12.8 px/deg the full field averages to -- x_ndc = sx*tan(θ) compresses
the centre), and 1.4 m at 64 mm subtends 2.619 deg. 2.619 * 8.60 = 22.5 px. The
two derivations agree, which is the check that 11.3 is not the number.

**Per TILE, not just globally.** A flat canvas at a fixed distance produces the
SAME disparity everywhere -- two eyes' projections of a fronto-parallel plane
differ by exactly sx*ipd/z at every point. So a spread across tiles is itself the
defect: it means the UI is not being placed as a flat canvas at all. A global
correlation would average that spread away and report a healthy number.

Vertical disparity must be ZERO. Any vertical component asks the eyes to diverge
vertically, which does not fuse, and is the "cross-eyed" half of #52.
"""

import argparse
import glob
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hud_vs_camera import decode, channels, cockpit_mask     # noqa: E402


def ncc_shift(a, b, rx, ry):
    """Best (dx, dy) aligning b to a, with the peak and its margin.

    Brute force over a small window: the expected answer is ~22 px and the
    images are the same instant, so there is no need for anything cleverer and
    every candidate offset is checked rather than assumed.
    """
    h, w = a.shape
    pad_x, pad_y = rx + 2, ry + 2
    core = a[pad_y:h - pad_y, pad_x:w - pad_x]
    t = core - core.mean()
    nt = float(np.sqrt((t * t).sum()))
    if nt < 1e-6:
        return None
    best, second, bxy = -2.0, -2.0, (0, 0)
    for dy in range(-ry, ry + 1):
        for dx in range(-rx, rx + 1):
            win = b[pad_y + dy:h - pad_y + dy, pad_x + dx:w - pad_x + dx]
            c = win - win.mean()
            nc = float(np.sqrt((c * c).sum()))
            if nc < 1e-6:
                continue
            v = float((t * c).sum() / (nt * nc))
            if v > best:
                if abs(dx - bxy[0]) > 3 or abs(dy - bxy[1]) > 3:
                    second = best
                best, bxy = v, (dx, dy)
            elif v > second and (abs(dx - bxy[0]) > 3 or abs(dy - bxy[1]) > 3):
                second = v
    return bxy[0], bxy[1], best, best - max(second, -1.0)


def tiles(mask, n, min_px):
    h, w = mask.shape
    for ty in range(n):
        for tx in range(n):
            y0, y1 = ty * h // n, (ty + 1) * h // n
            x0, x1 = tx * w // n, (tx + 1) * w // n
            if (mask[y0:y1, x0:x1] > 0).sum() >= min_px:
                yield (tx, ty, y0, y1, x0, x1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern', help='glob of -layer0 dumps')
    ap.add_argument('--expect', type=float, default=22.5,
                    help='predicted layer-to-layer disparity in px')
    ap.add_argument('--range-x', type=int, default=60)
    ap.add_argument('--range-y', type=int, default=16)
    ap.add_argument('--tiles', type=int, default=4)
    ap.add_argument('--min-px', type=int, default=400,
                    help='UI pixels a tile needs before it is measured')
    ap.add_argument('--max-frames', type=int, default=40)
    a = ap.parse_args()

    fs = sorted(glob.glob(a.pattern),
                key=lambda p: int(re.search(r'-n(\d+)-', p).group(1)))
    if not fs:
        print('no dumps matched', file=sys.stderr)
        return 2

    ui_dx, ui_dy, ck_dx, per_tile = [], [], [], []
    used = 0
    for f in fs:
        if used >= a.max_frames:
            break
        g = f.replace('-layer0.png', '-layer1.png')
        if not os.path.exists(g):
            continue
        c0, c1 = channels(decode(f)), channels(decode(g))
        h0, h1 = c0['hud'], c1['hud']
        if (h0 > 0).sum() < 2000:
            continue
        r = ncc_shift(h0, h1, a.range_x, a.range_y)
        if r is None or r[3] < 0.02:
            continue
        used += 1
        ui_dx.append(r[0]); ui_dy.append(r[1])
        # the cockpit in the SAME frame: sbs_depth put it at 1.40 m, the same
        # distance the canvas claims, so its disparity is the natural control
        rc = ncc_shift(c0['cockpit'], c1['cockpit'], a.range_x, a.range_y)
        if rc is not None and rc[3] >= 0.02:
            ck_dx.append(rc[0])
        for (tx, ty, y0, y1, x0, x1) in tiles(h0, a.tiles, a.min_px):
            rt = ncc_shift(h0[y0:y1, x0:x1], h1[y0:y1, x0:x1],
                           a.range_x, a.range_y)
            if rt is not None and rt[3] >= 0.02:
                per_tile.append((tx, ty, rt[0], rt[1]))
        print(f'  {os.path.basename(f):34s} UI dx {r[0]:+4d} dy {r[1]:+3d} '
              f'(peak {r[2]:.3f}, margin {r[3]:.3f})')

    if used < 3:
        print(f'\nonly {used} frame(s) had a measurable UI -- cannot answer')
        return 1
    ux = np.array(ui_dx, dtype=float)
    uy = np.array(ui_dy, dtype=float)
    print(f'\n  {used} frame(s) with a measurable UI')
    print(f'  UI disparity      dx median {np.median(ux):+.1f} px  '
          f'(quartiles {np.percentile(ux,25):+.1f}..{np.percentile(ux,75):+.1f})'
          f'   dy median {np.median(uy):+.1f} px')
    print(f'  PREDICTED         dx {-a.expect:+.1f} px (or {a.expect:+.1f}, '
          f'sign depends which layer is the left eye)   dy 0.0')
    got = abs(np.median(ux))
    print(f'  => |measured| {got:.1f} px against {a.expect:.1f} predicted  '
          f'= {got / a.expect:.2f}x')
    if ck_dx:
        ck = np.array(ck_dx, dtype=float)
        print(f'  cockpit in the same frames: dx median {np.median(ck):+.1f} px '
              f'over {len(ck)} frame(s) — sbs_depth put the cockpit at 1.40 m, '
              f'the SAME distance the canvas claims, so this should match the UI')
    if abs(np.median(uy)) > 1.0:
        print(f'  VERTICAL disparity {np.median(uy):+.1f} px — the eyes are '
              f'asked to diverge vertically, which does not fuse. That is #52.')
    if per_tile:
        t = np.array([(p[2]) for p in per_tile], dtype=float)
        print(f'\n  per-tile dx over {len(t)} tile measurement(s): '
              f'median {np.median(t):+.1f}  p10 {np.percentile(t,10):+.1f}  '
              f'p90 {np.percentile(t,90):+.1f}  spread '
              f'{np.percentile(t,90)-np.percentile(t,10):.1f} px')
        print('  a flat canvas has the SAME disparity everywhere; spread here '
              'means the UI is not being placed as one.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
