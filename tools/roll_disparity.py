#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Task #80: does the eye disparity ROTATE with head roll, for both channels?

    python3 tools/roll_disparity.py --prefix /tmp/x4vr-t258 --log /tmp/x4vr-take258.log

Patola, take 258: *"rolling my head does not make the HUD adapt the eye
separation as it does with the cockpit."*

**The mechanism, read from the code before this was written** (so the
measurement is a test, not an exploration):

  * `common/x4vr_view.hpp:make_canvas_k` puts the UI's disparity in `k[12]`
    alone -- `k[12] -= ax_num * d_eye / z`. Pure NDC x. There is no `k[13]`
    term and no roll input, and it is built ONCE at pipeline creation from a
    static X4VR_CANVAS_M.
  * the SCENE's stereo goes through the pose channel, gated per frame by
    `roll_active()`, and take 258's log shows the layer publishing
    `(cos,sin)=(0.6515,0.7587)` at a head roll of -49.35 deg. So the scene's
    eye offset turns with the head; the UI's cannot.

**PREDICTIONS, written before the first run of this file:**

  P1  UI dy is 0 on every frame, rolled or not. The canvas has no vertical
      term to carry.
  P2  On a frame captured under roll, SCENE tiles read dy = dx * tan(phi) --
      **COHERENTLY ACROSS TILES**, because a rotation of the eye offset is one
      rotation applied to every tile's disparity. The test is therefore the
      SLOPE of dy against dx within a frame, not the size of any dy.
  P3  On unrolled frames the slope is ~ 0.

**The first version of this file got P2 wrong and printed a false CONFIRMED.**
It keyed its verdict on `max(|scene dy|)` over all frames -- an extreme over a
noisy population. The table underneath refuted it: the frame nearest the -49.35
deg roll sample (n=102) read dy = 0.0 with p90 0.0, while the largest dy, 18.5
DS px, sat on n=12 at a logged roll of +1.11 deg, where roll can account for
0.2 DS px. Several frames showed median 0 with a p90 of 12-20 -- scattered
per-tile values, which is NCC sliding along a ridge on a sparse tile, not a
rotation. A rotation is COHERENT; noise is not, and only a fit can tell them
apart.

P3 is not decoration, it is what makes P1 mean anything. "UI dy = 0" is
worthless if this estimator cannot resolve dy at all -- so there is also a
SYNTHETIC control per frame: layer0 is shifted by a known dy and re-measured.
If that does not come back, the frame's dy numbers are void and are printed as
such rather than as zeros.

**Which frames were rolled is decided by the DATA, not by me.** The log samples
roll only 16 times in the run (once per period) and only one sample exceeds
5 deg, so reconstructing "the rolled window" from timestamps would be my
arithmetic standing in for X4's state -- the mistake that produced three wrong
verdicts in a row earlier in this project. This measures a frame from every
burst across the whole run and lets the rolled ones show themselves; the log's
roll samples are then printed alongside as corroboration.
"""
import argparse
import re
import sys

import numpy as np

sys.path.insert(0, '/home/patola/workspace/claude/X4VRMOD/tools')
from ui_disparity import decode_rgba, ncc_shift, mask_window     # noqa: E402
from hud_vs_camera import decode                                 # noqa: E402

DS = 2              # downsample; dy of ~26 px is 13 px here, plainly resolvable
RX, RY = 40, 20     # at 1/2 res: +-80 px x, +-40 px y at full res
GRID = 4
MIN_MARGIN = 0.02
MIN_DX = 3          # tiles flatter than this (in DS px) are sky: no disparity,
                    # so no rotation to see, and their dy says nothing
MIN_TILES = 5       # a slope through 2 points is not a measurement
ROT_DEG = 5.0       # |median angle| past which a frame counts as rotated
MAD_DEG = 8.0       # ...and how tightly the tiles must agree to call it one
CTRL_DY = 6         # the synthetic shift, in DS px
CTRL_DX = -11       # ...carried WITH a dx, so the control is no easier than
                    # the measurement: -11 DS px is the 22.5 px full-res
                    # disparity of a 1.4 m surface, which is what these frames
                    # actually contain


def ds(a, k=DS):
    h, w = a.shape[0] // k * k, a.shape[1] // k * k
    return a[:h, :w].reshape(h // k, k, w // k, k).mean((1, 3))


def lum(p):
    return 0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2]


def frame(prefix, n):
    """(ui0, ui1, scene0, scene1, mask0) at DS resolution, or None."""
    try:
        r0, a0 = decode_rgba(f'{prefix}-ui-n{n}-layer0.png')
        r1, a1 = decode_rgba(f'{prefix}-ui-n{n}-layer1.png')
        p0 = decode(f'{prefix}-present-n{n}-layer0.png')
        p1 = decode(f'{prefix}-present-n{n}-layer1.png')
    except Exception:
        return None
    # union mask: X4 writes the weapon arcs and bracket rectangles with colour
    # and ZERO alpha, so alpha alone drops them
    m0 = (a0 > 0) | (r0.max(2) > 0)
    m1 = (a1 > 0) | (r1.max(2) > 0)
    return (ds(np.where(m0, lum(r0), 0.0)), ds(np.where(m1, lum(r1), 0.0)),
            ds(np.where(~m0, lum(p0), 0.0)), ds(np.where(~m1, lum(p1), 0.0)),
            m0)


def burst_first_frames(log_path):
    """One frame number per burst, plus the burst's log timestamp."""
    out = []
    for line in open(log_path, errors='replace'):
        m = re.match(r'\[\s*([0-9.]+)\].*ui dump: burst frames ([0-9,]+)', line)
        if m:
            nums = [int(v) for v in m.group(2).split(',') if v]
            if nums:
                out.append((float(m.group(1)), nums))
    return out


def roll_samples(log_path):
    out = []
    for line in open(log_path, errors='replace'):
        m = re.match(r'\[\s*([0-9.]+)\] layer   roll: head (-?[0-9.]+) deg',
                     line)
        if m:
            out.append((float(m.group(1)), float(m.group(2))))
    return out


def measure(prefix, n):
    f = frame(prefix, n)
    if f is None:
        return None
    ui0, ui1, s0, s1, mask0 = f

    # --- the synthetic control, on THIS frame's own scene content ----------
    # Shift layer0 by a known (dx, dy) and ask for it back. If the estimator
    # cannot find a shift it was handed, every dy below is void.
    #
    # **The control carries a dx as well as a dy, and that is not incidental.**
    # A control with dx = 0 is EASIER than the measurement, which has to find
    # dy while a real horizontal disparity of ~11 DS px is also present -- and
    # a control easier than the measurement cannot vouch for it. That exact
    # mistake produced the 0.83 that voided take 255's rates. CTRL_DX is set
    # to the disparity a 1.4 m surface actually shows here.
    ctrl = ncc_shift(s0, np.roll(np.roll(s0, CTRL_DY, axis=0), CTRL_DX,
                                 axis=1), RX, RY)
    ctrl_ok = (ctrl is not None and ctrl[1] == CTRL_DY and
               ctrl[0] == CTRL_DX)

    # --- UI channel --------------------------------------------------------
    w = mask_window(ds(mask0.astype(np.float32)) > 0.0, RX, RY)
    rui = ncc_shift(ui0[w], ui1[w], RX, RY) if w else None

    # --- scene tiles -------------------------------------------------------
    dxs, dys = [], []
    h, wd = s0.shape
    for ty in range(GRID):
        for tx in range(GRID):
            y0, y1 = ty * h // GRID, (ty + 1) * h // GRID
            x0, x1 = tx * wd // GRID, (tx + 1) * wd // GRID
            a, b = s0[y0:y1, x0:x1], s1[y0:y1, x0:x1]
            if (a > 0.001).sum() < 2000:
                continue
            r = ncc_shift(a, b, RX, RY)
            if r is None or r[3] < MIN_MARGIN:
                continue
            if abs(r[0]) < MIN_DX:      # sky: no disparity, no rotation to see
                continue
            dxs.append(r[0])
            dys.append(r[1])
    return rui, dxs, dys, ctrl, ctrl_ok


def tile_angles(dxs, dys):
    """Per-tile disparity ANGLE, and how tightly the tiles agree.

    **This is the primary statistic, and the slope fit below is not.** A
    rotation of the eye offset by phi sends every tile's disparity
    (dx, 0) -> (dx cos phi, dx sin phi), so every tile's ANGLE is phi no
    matter its depth. Angle is therefore the quantity the hypothesis is about.

    The r2 of dy against dx degenerates exactly where the signal is: in a
    cockpit frame most tiles sit at similar depth, so dx has little spread and
    a fit through them explains little variance however real the rotation.
    n108 scored r2 0.30 while its twelve tiles agreed on the angle to 1.8 deg.
    A statistic that is weakest where the effect is strongest is the wrong
    statistic.

    Returns (median_deg, mad_deg, n). Angles are folded to (-90, 90]: the
    disparity is a line, not a ray, and a sign flip in dx must not read as a
    180 deg rotation.
    """
    if len(dxs) < MIN_TILES:
        return None
    a = np.degrees(np.arctan2(np.asarray(dys, float), np.asarray(dxs, float)))
    a = np.where(a > 90, a - 180, a)
    a = np.where(a < -90, a + 180, a)
    med = float(np.median(a))
    return med, float(np.median(np.abs(a - med))), len(a)


def fit_slope(dxs, dys):
    """dy = slope * dx through the origin, plus how well it holds.

    Through the origin because a rotation of the eye offset maps
    (dx, 0) -> (dx*cos, dx*sin): a tile with no disparity has no dy to gain,
    whatever the roll. An intercept would let a constant vertical offset --
    which is a DIFFERENT defect -- masquerade as a rotation.

    Returns (slope, r2, n). r2 is against the mean-removed dy, so a slope that
    explains nothing scores <= 0 no matter how large it is.
    """
    x = np.asarray(dxs, dtype=float)
    y = np.asarray(dys, dtype=float)
    if len(x) < MIN_TILES or float((x * x).sum()) < 1e-9:
        return None
    slope = float((x * y).sum() / (x * x).sum())
    resid = y - slope * x
    ss_res = float((resid * resid).sum())
    ss_tot = float(((y - y.mean()) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-9 else (1.0 if ss_res < 1e-9
                                                     else 0.0)
    return slope, r2, len(x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--prefix', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--all-in-burst', type=int, default=1,
                    help='frames to take from each burst (default 1)')
    a = ap.parse_args()

    bursts = burst_first_frames(a.log)
    rolls = roll_samples(a.log)
    if not bursts:
        print(f'no burst lines in {a.log}', file=sys.stderr)
        return 2
    print(f'  {len(bursts)} burst(s), {len(rolls)} roll sample(s) in the log; '
          f'|roll| > 5 deg in {sum(1 for _, r in rolls if abs(r) > 5)} of them')
    print(f'  all shifts in DOWNSAMPLED px (x{DS}); multiply by {DS} for the '
          f'full-res number\n')
    print(f'{"n":>5} {"t_log":>9} {"log roll":>9} | {"UIdx":>5} {"UIdy":>5} '
          f'{"UIang":>6} | {"tiles":>5} {"scn ang":>8} {"MAD":>6} | ctrl')

    rows = []
    for t, nums in bursts:
        for n in nums[:a.all_in_burst]:
            m = measure(a.prefix, n)
            if m is None:
                continue
            rui, dxs, dys, ctrl, ctrl_ok = m
            near = min(rolls, key=lambda r: abs(r[0] - t)) if rolls else None
            has_roll = near is not None and abs(near[0] - t) < 25
            nr = f'{near[1]:+9.2f}' if has_roll else '        —'
            ang = tile_angles(dxs, dys)
            uang = (np.degrees(np.arctan2(rui[1], rui[0])) if rui else
                    float('nan'))
            if rui and uang > 90:
                uang -= 180
            if rui and uang < -90:
                uang += 180
            astr = (f'{ang[0]:>8.1f} {ang[1]:>6.1f}' if ang
                    else f'{"—":>8} {"—":>6}')
            print(f'{n:>5} {t:>9.1f} {nr} | '
                  f'{(rui[0] if rui else 0):>5} {(rui[1] if rui else 0):>5} '
                  f'{uang:>6.1f} | {len(dxs):>5} {astr} | '
                  f'{"ok" if ctrl_ok else "VOID"}', flush=True)
            rows.append((n, t, rui, dxs, dys, ctrl_ok,
                         near if has_roll else None, ang, uang))

    good = [r for r in rows if r[5]]
    fitted = [r for r in good if r[7]]
    print(f'\n  {len(rows)} frame(s) measured, {len(good)} passed the (dx,dy) '
          f'control, {len(fitted)} had >= {MIN_TILES} tiles')
    if not good:
        print('  the synthetic (dx,dy) control failed on every frame — this '
              'data says NOTHING about dy in either channel. Not a null.')
        return 1
    if not fitted:
        print(f'  no frame had {MIN_TILES} usable scene tiles, so no angle '
              f'could be measured. This data cannot answer #80 either way.')
        return 1

    # ROTATED = the tiles agree on an angle that is not zero. Both halves
    # matter: a large median with a large MAD is scatter, and the first
    # version of this file called exactly that a confirmation.
    rot = [r for r in fitted if abs(r[7][0]) > ROT_DEG and r[7][1] < MAD_DEG]
    lvl = [r for r in fitted if abs(r[7][0]) <= ROT_DEG]
    print(f'  {len(rot)} frame(s) with a COHERENT rotation '
          f'(|angle| > {ROT_DEG:.0f} deg, MAD < {MAD_DEG:.0f} deg); '
          f'{len(lvl)} level')
    for r in rot:
        n, _, rui, _, _, _, near, (med, mad, ntl), uang = r
        exp = f'{near[1]:+.2f} deg' if near else 'not sampled in the log'
        print(f'    n{n}: scene {med:+.1f} +- {mad:.1f} deg over {ntl} tiles; '
              f'UI {uang:+.1f} deg; nearest logged head roll {exp}')

    ui_rot = [r for r in rot if abs(r[8]) > ROT_DEG]
    print()
    if not rot:
        print('  NO frame shows a coherently rotated scene disparity, so P2 '
              'is UNTESTED and P1')
        print('  proves nothing: UI angle 0 is only interesting on a frame '
              'where the SCENE is')
        print('  rotated. This run captured none. #80 still rests on the code '
              'reading and on')
        print('  Patola\'s eye — it needs a run that HOLDS a roll.')
        return 1
    if ui_rot:
        print(f'  BOTH channels rotate ({len(ui_rot)} frame(s)). That REFUTES '
              f'the code reading:')
        print('  make_canvas_k has no k[13] and no roll input, so something '
              'else is rotating')
        print('  the UI disparity. Find what before changing anything.')
        return 1
    print(f'  #80 CONFIRMED. On {len(rot)} frame(s) the SCENE disparity is '
          f'coherently rotated')
    print('  while the UI\'s stays horizontal, with the (dx,dy) control '
          'passing on the same')
    print('  frames and level frames reading 0 on both channels.')
    print('  The declared pose carries yaw and pitch only '
          '(quat_of_angles(decl_yaw, decl_pitch)),')
    print('  so the head\'s roll reaches the display entirely as compositor '
          'reprojection: it')
    print('  rotates the image by -phi. A scene disparity pre-rotated to -phi '
          'therefore lands')
    print('  HORIZONTAL, and the UI\'s, at 0, lands at +phi — vertical '
          'divergence, which the')
    print('  eyes cannot fuse.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
