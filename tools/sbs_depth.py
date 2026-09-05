#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""How deep is X4's cockpit, really? Measure it against the HUD's set depth.

    tools/sbs_depth.py <sbs-clip.mp4> --hud-depth 1.4

Task #57 / #25. `X4VR_CANVAS_M` places the UI at a distance chosen by hand --
1.4 m for every take since it was added. **Nothing has ever measured what that
distance should be.** The cockpit HUD is meant to read as part of the cockpit,
and if it is set to twice the cockpit's distance then it is at the wrong depth,
which shows up twice: statically as the per-eye separation Patola calls "visibly
wrong", and dynamically as content that will not sit still against the cockpit.
That is one unmeasured number under several failed fixes.

**The measurement is calibration-free, which is the point.** In one SBS frame,
disparity is inversely proportional to depth, so for two objects in the same
frame:

    disparity_hud / disparity_cockpit = depth_cockpit / depth_hud

The projection scale, the recorder's field of view and the pixels-per-degree all
cancel. `depth_hud` is known by construction -- it is the knob -- so the
cockpit's depth follows without a single calibration constant. `14.17 px/deg`
has been carried through this project's commands with **no recorded
derivation**; this tool needs no such number and so cannot inherit that error.

Vertical disparity is reported alongside because it should be ZERO. Anything
else is the eyes being asked to diverge vertically, which does not fuse, and is
the "cross-eyed" half of #52.
"""

import argparse
import os
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hud_vs_camera import decode, channels                   # noqa: E402
from hud_drift import ncc_find, frames_of                    # noqa: E402
from hud_vs_cockpit import pick_hud, pick_cockpit            # noqa: E402


def halves(img2d):
    w = img2d.shape[1] // 2
    return img2d[:, :w], img2d[:, w:]


def disparity(tpl, centre, right, margin, band):
    """Find tpl (taken from the LEFT eye) in the RIGHT eye.

    Searched in a band around the same position: a stereo pair differs by a
    horizontal shift, so a match far from the epipolar line is a different
    object, not a large disparity.
    """
    r = ncc_find(tpl, right, margin, near=centre, radius=band)
    if r is None:
        return None
    return r[0] - centre[0], r[1] - centre[1], r[3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern', help='SBS clip (width = 2x eye width)')
    ap.add_argument('--hud-depth', type=float, default=1.4,
                    help='the depth the HUD is SET to, in m (X4VR_CANVAS_M)')
    ap.add_argument('--ipd', type=float, default=0.0630,
                    help='runtime eye separation in m, for the absolute check')
    ap.add_argument('--fps', type=float, default=2.0)
    ap.add_argument('--frames-dir', default='/tmp/x4vr-sbs')
    ap.add_argument('--margin', type=float, default=0.05)
    ap.add_argument('--band', type=float, default=90.0)
    ap.add_argument('--cockpit-size', type=int, default=64)
    a = ap.parse_args()

    fs = frames_of(a.pattern, a.frames_dir, a.fps)
    if not fs:
        print('no frames', file=sys.stderr)
        return 2

    rows = []
    for f in fs:
        ch = channels(decode(f))
        hudL, hudR = halves(ch['hud'])
        greyL, greyR = halves(ch['grey'])
        got_h = pick_hud(hudL)
        if not got_h:
            continue
        hud_t, hud_c = got_h
        cock_t, cock_c = pick_cockpit(greyL, hudL, a.cockpit_size)
        dh = disparity(hud_t, hud_c, hudR, a.margin, a.band)
        dc = disparity(cock_t, cock_c, greyR, a.margin, a.band)
        if dh is None or dc is None:
            continue
        rows.append((dh[0], dh[1], dc[0], dc[1], hud_c[0], cock_c[0]))
        print(f'  {os.path.basename(f):14s} HUD dx {dh[0]:+7.1f} dy {dh[1]:+5.1f}'
              f'   cockpit dx {dc[0]:+7.1f} dy {dc[1]:+5.1f} px')

    if len(rows) < 3:
        print(f'only {len(rows)} usable frame(s) -- cannot measure')
        return 1
    r = np.array(rows)
    hx, hy, cx, cy = (np.median(r[:, i]) for i in range(4))
    print(f'\n  {len(rows)} frame(s)')
    print(f'  HUD      disparity  dx {hx:+.1f} px   dy {hy:+.1f} px')
    print(f'  cockpit  disparity  dx {cx:+.1f} px   dy {cy:+.1f} px')

    # Ratio PER FRAME, then median -- not median-then-ratio. The patches are
    # re-picked in every frame, so the two medians can describe two different
    # pairs of objects and their quotient would describe neither. Only a
    # within-frame ratio is a depth comparison.
    ok = (np.abs(r[:, 2]) > 1.0) & (r[:, 0] * r[:, 2] > 0)
    if ok.sum() < 3:
        print(f'  only {int(ok.sum())} frame(s) have a usable within-frame '
              f'pair (non-zero cockpit disparity, same sign) -- no depth ratio')
        return 1
    per = r[ok, 0] / r[ok, 2]
    ratio = float(np.median(per))
    q1, q3 = np.percentile(per, [25, 75])
    print(f'\n  within-frame ratio HUD/cockpit over {int(ok.sum())} frame(s): '
          f'median {ratio:.3f}  (quartiles {q1:.3f}..{q3:.3f})')
    print(f'  => cockpit depth = {a.hud_depth:.2f} m x {ratio:.3f} = '
          f'{a.hud_depth * ratio:.2f} m,  with the HUD SET to '
          f'{a.hud_depth:.2f} m')
    print(f'     quartile span {a.hud_depth*q1:.2f}..{a.hud_depth*q3:.2f} m -- '
          f'the patches are re-picked every frame, so this spread is partly '
          f'different cockpit features at different depths, not noise')
    if abs(hy) > 2.0 or abs(cy) > 2.0:
        print(f'  VERTICAL disparity is not zero (HUD {hy:+.1f}, cockpit '
              f'{cy:+.1f} px) -- the eyes are being asked to diverge '
              f'vertically, which does not fuse. That is #52.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
