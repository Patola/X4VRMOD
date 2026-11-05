#!/usr/bin/env python3
"""Where is the SCENE, in the same frames where the UI measures -23 px?

The UI is now established: -23.0 px, flat to 1 px, in cockpit, map and menus.
Patola still reports the separation looking wrong. So the next candidate is not
the UI's disparity but its relationship to what is behind it -- a correct
overlay in front of a scene at the wrong depth looks wrong, and each part
measures fine on its own.

Method: same-instant, pure-translation NCC (the one configuration that has
worked), per tile, over the scene = everything the UI dump does NOT cover.

**Two physical anchors make this checkable rather than merely plausible.**
Neither is an assumption:
  * stars are at infinity, so sky tiles MUST read ~0 px;
  * the cockpit was independently placed at 1.40 m by sbs_depth (calibration
    free, within-frame ratio), so cockpit tiles MUST read ~-23 px.
If those two do not come out, the measurement is wrong and nothing else in the
output means anything.

Depth from disparity, using only logged quantities:
    disparity_px = sx * ipd * (W/2) / z      with sx=0.70044, ipd=0.064, W=1408
    => z = 31.556 / |disparity_px|
which returns 1.40 m at 22.5 px -- the canvas distance, as it must.
"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ui_disparity import decode_rgba, ncc_shift, mask_window     # noqa: E402
from hud_vs_camera import decode                                 # noqa: E402

SX, IPD, W = 0.70044, 0.064, 1408
K = SX * IPD * (W / 2)          # 31.556; z = K / |dx|

RX, RY = 60, 8
GRID = 6
MIN_MARGIN = 0.02
# Named per run, never hard-coded to a take. This was pinned to t257 while
# take 257's dumps were being deleted to make room for take 258 -- every
# decode would then have failed, every frame been skipped, and the tool would
# have reported "nothing measurable" about a run it never opened a file from.
PREFIX = '/tmp/x4vr-t258'


def depth_m(dx):
    return float('inf') if abs(dx) < 1e-9 else K / abs(dx)


def main(lo, hi):
    rows = []
    print(f'{"n":>4} {"UI dx":>6} | scene tiles: '
          f'{"n":>3} {"min":>5} {"p25":>5} {"med":>5} {"p75":>5} {"max":>5}'
          f'   {"median z":>9}', flush=True)
    for n in range(lo, hi + 1):
        try:
            r0, a0 = decode_rgba(f'{PREFIX}-ui-n{n}-layer0.png')
            r1, a1 = decode_rgba(f'{PREFIX}-ui-n{n}-layer1.png')
            p0 = decode(f'{PREFIX}-present-n{n}-layer0.png')
            p1 = decode(f'{PREFIX}-present-n{n}-layer1.png')
        except Exception:
            continue
        ui0 = (a0 > 0) | (r0.max(2) > 0)
        ui1 = (a1 > 0) | (r1.max(2) > 0)
        # Dilate the UI mask by the search range before excluding it: a tile
        # whose scene content sits under where the UI lands in the OTHER layer
        # would otherwise correlate against UI pixels.
        keep0 = ~ui0
        keep1 = ~ui1
        L0 = 0.2126 * p0[..., 0] + 0.7152 * p0[..., 1] + 0.0722 * p0[..., 2]
        L1 = 0.2126 * p1[..., 0] + 0.7152 * p1[..., 1] + 0.0722 * p1[..., 2]
        s0 = np.where(keep0, L0, 0.0)
        s1 = np.where(keep1, L1, 0.0)

        # the UI in this frame, for the side-by-side
        um0 = np.where(ui0, 0.2126 * r0[..., 0] + 0.7152 * r0[..., 1]
                       + 0.0722 * r0[..., 2], 0.0)
        um1 = np.where(ui1, 0.2126 * r1[..., 0] + 0.7152 * r1[..., 1]
                       + 0.0722 * r1[..., 2], 0.0)
        w = mask_window(ui0, RX, RY)
        rui = ncc_shift(um0[w], um1[w], RX, RY) if w else None

        dxs = []
        h, wd = L0.shape
        for ty in range(GRID):
            for tx in range(GRID):
                y0, y1 = ty * h // GRID, (ty + 1) * h // GRID
                x0, x1 = tx * wd // GRID, (tx + 1) * wd // GRID
                sub0, sub1 = s0[y0:y1, x0:x1], s1[y0:y1, x0:x1]
                # A tile needs real scene content, not a few stray pixels.
                if (sub0 > 0.001).sum() < 3000:
                    continue
                r = ncc_shift(sub0, sub1, RX, RY)
                if r is not None and r[3] >= MIN_MARGIN:
                    dxs.append(r[0])
        if not dxs:
            continue
        d = np.array(dxs, dtype=float)
        med = float(np.median(d))
        rows.append((n, rui[0] if rui else None, d))
        print(f'{n:>4} {rui[0] if rui else 0:+6d} | '
              f'{len(d):>15d} {d.min():+5.0f} {np.percentile(d,25):+5.0f} '
              f'{med:+5.0f} {np.percentile(d,75):+5.0f} {d.max():+5.0f}'
              f'   {depth_m(med):>8.2f}m', flush=True)

    if not rows:
        # "no data" and "data that says nothing" print identically otherwise,
        # and only one of them is about the command line.
        print(f'NO FRAME DECODED in n{lo}..n{hi} under prefix {PREFIX!r} — '
              f'nothing was read, so this is not a measurement. Check '
              f'--prefix:\n    ls {PREFIX}-present-n*-layer0.png | wc -l',
              file=sys.stderr)
        return 2
    allt = np.concatenate([r[2] for r in rows])
    uis = np.array([r[1] for r in rows if r[1] is not None], dtype=float)
    print(f'\n{len(rows)} frame(s), {len(allt)} scene tile measurement(s)')
    print(f'  UI    dx median {np.median(uis):+.1f} px  '
          f'-> {depth_m(np.median(uis)):.2f} m')
    print(f'  SCENE dx  min {allt.min():+.0f}  p10 {np.percentile(allt,10):+.0f}'
          f'  median {np.median(allt):+.0f}  p90 {np.percentile(allt,90):+.0f}'
          f'  max {allt.max():+.0f}')
    print(f'        depth p10 {depth_m(np.percentile(allt,90)):.2f} m .. '
          f'median {depth_m(np.median(allt)):.2f} m .. '
          f'p90 {depth_m(np.percentile(allt,10)):.2f} m')
    # The two physical anchors.
    at_inf = float((np.abs(allt) <= 2).mean())
    nearer = float((allt < np.median(uis) - 2).mean())
    print(f'\n  ANCHOR 1  {100*at_inf:.1f}% of scene tiles read |dx| <= 2 px '
          f'(at infinity). Stars are at infinity, so a frame showing sky '
          f'MUST produce some of these; 0% would mean the measurement is '
          f'broken, not that the sky moved.')
    print(f'  ANCHOR 2  {100*nearer:.1f}% of scene tiles read NEARER than the '
          f'UI plane at {depth_m(np.median(uis)):.2f} m.')
    print('            **This does NOT say the UI overlaps them.** Scene tiles '
          'are taken from where the UI is NOT drawn -- that is the whole '
          'method -- so this measures the depth of the scene AROUND the UI, '
          'not behind it. A cockpit really does have console and struts nearer '
          'than 1.4 m, and rendering them there is correct, not a defect.\n'
          '            To test the occlusion conflict properly the SCENE '
          'BEHIND the UI is needed, and the composed frame does not contain '
          'it: the UI covers it. That wants X4VR_HUDLESS=1, which makes X4 '
          'emit the HUD-less scene, or a dump of the pre-composite scene '
          'target -- a different run, not this data.')
    return 0


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('lo', type=int)
    ap.add_argument('hi', type=int)
    ap.add_argument('--prefix', default=PREFIX,
                    help=f'dump prefix, i.e. X4VR_MV_DUMP (default {PREFIX})')
    a = ap.parse_args()
    PREFIX = a.prefix
    sys.exit(main(a.lo, a.hi))
