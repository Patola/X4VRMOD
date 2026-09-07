#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
r"""#76: does the HUD move like a plane at the depth it is drawn at?

    tools/depth_motion.py 0 600 --burst-log /tmp/x4vr-take260.log --prefix /tmp/x4vr-t260
    tools/depth_motion.py --selftest

`hud_tiles.py --channel both` established that the HUD moves **1.39x** as far
as whatever is directly behind it at the same place on screen. That is real but
it does not say WHY, because two very different worlds produce it:

    PLACEMENT   the HUD sits on a canvas at 1.37 m, the scene behind it is
                mostly further away, the head TRANSLATES, and a nearer plane
                simply moves more. Then nothing is broken -- the slide is a
                consequence of choosing 1.37 m, and the fix is that distance.
    TRANSPORT   the HUD moves further than ANY object at 1.37 m could in this
                camera motion. Then a transform of ours is over-moving it and
                the canvas distance is a red herring.

**The frames separate these on their own, with no take and no pose log.** For a
camera that rotates by theta and translates laterally by d, the image shift of
a point at screen tangent x and inverse depth u = 1/z is

    dx  =  f * theta * (1 + x^2)   +   f * d * u
           \_____ rotation _____/       \__ translation __/

and the two terms have DIFFERENT signatures. Rotation carries the field factor
(1 + x^2) -- the tangent-space expansion that makes the edges of a 110 deg
frame move further than the centre -- and does not depend on depth at all.
Translation depends on depth and has NO field factor: shifting the camera by d
moves a point at depth z by f*d/z wherever it sits on screen.

So per frame pair, fit

    dx_tile  =  P * (1 + x_tile^2)  +  Q * u_tile        P = f*theta, Q = f*d

over the scene tiles, whose x comes from the tile's position and whose u comes
from that tile's own STEREO DISPARITY (u = |disparity| / 31.556). Then ask what
inverse depth the UI's measured motion implies:

    u_eff  =  (dx_ui - P * (1 + x^2)) / Q          z_eff = 1 / u_eff

**z_eff is the answer.** If it comes back at ~1.37 m the HUD moves exactly as a
plane at the canvas distance should, and PLACEMENT is the story. If it comes
back markedly nearer, the HUD is being transported further than its own depth
allows, and TRANSPORT is the story.

**Q must be well determined or z_eff means nothing.** If the head barely
translated in a pair then Q ~ 0, dividing by it is meaningless, and the pair is
reported as UNCONDITIONED rather than given a number. That check is the
difference between this measurement and a plausible-looking division.
"""
import argparse
import sys

import numpy as np

sys.path.insert(0, '/home/patola/workspace/claude/X4VRMOD/tools')
from ui_disparity import decode_rgba, ncc_shift, mask_window   # noqa: E402
from hud_vs_camera import decode                               # noqa: E402
from hud_slide import burst_groups, shift2                     # noqa: E402
from hud_tiles import tile_shift                               # noqa: E402
from ui_disparity import tiles                                  # noqa: E402

K = 0.70044 * 0.064 * (1408 / 2)     # 31.556; z = K / |disparity_px|
W = 1408
HALF_TAN = 1.4281                    # tan(55 deg), X4VR_FOV=1.4917 -> 110 deg
GRID = 3                             # motion tiles, 469 px
DSUB = 2                             # depth sub-tiles inside each motion tile
RX, RY = 60, 8                       # disparity search, same as scene_depth
MIN_MARGIN = 0.02
MIN_TILE_PX = 3000
MIN_UI_PX = 1500
MIN_TILES = 4
MAX_MOTION = 100
MIN_SCENE = 15
# A motion tile whose sub-tiles disagree about depth by more than this is not
# one depth, and pairing its single motion with its median depth would pair a
# number from the console with a number from the starfield.
DEPTH_SPREAD_PX = 18
# Q is f*d in pixels. Below this the head barely translated, u_eff is a
# division by noise, and the pair says nothing about depth.
MIN_Q_PX = 8.0


def tangent(px_centre):
    return (px_centre - W / 2) / (W / 2) * HALF_TAN


def field(px_centre):
    """The rotation term's field factor (1 + x^2) at a screen position."""
    return 1.0 + tangent(px_centre) ** 2


def field_weighted(content, x0, x1):
    """(1 + x^2) averaged over where the CONTENT actually is in a tile.

    Not field(tile_centre). (1 + x^2) is convex, so the factor at the mean
    position understates the mean of the factor -- across the whole frame by
    1.68x, which is larger than the effect under test and produced a negative
    z_eff before it was caught. A sparse UI tile can sit anywhere within its
    tile, so the same argument applies at tile scale.
    """
    cols = np.asarray(content > 1e-6).sum(axis=0).astype(float)
    if cols.sum() < 1.0:
        return field((x0 + x1) / 2)
    xs = np.arange(x0, x1, dtype=float)
    return float((cols * (1.0 + ((xs - W / 2) / (W / 2) * HALF_TAN) ** 2)).sum()
                 / cols.sum())


def fit_pair(x_field, u, dx):
    """Least squares for (P, Q) in dx = P*field + Q*u, plus conditioning.

    Returns (P, Q, cond) where cond is the spread of u across the tiles used --
    if every tile sits at the same depth the two regressors are collinear and
    Q is not identifiable, however good the residual looks.
    """
    A = np.stack([x_field, u], axis=1)
    sol, *_ = np.linalg.lstsq(A, dx, rcond=None)
    return float(sol[0]), float(sol[1]), float(u.max() - u.min())


def frame_data(prefix, n):
    """Per-tile (field, inv-depth) for the scene, plus the UI's own numbers."""
    try:
        p0 = decode(f'{prefix}-present-n{n}-layer0.png')
        p1 = decode(f'{prefix}-present-n{n}-layer1.png')
        r0, a0 = decode_rgba(f'{prefix}-ui-n{n}-layer0.png')
        r1, a1 = decode_rgba(f'{prefix}-ui-n{n}-layer1.png')
    except Exception:
        return None
    lum = lambda p: 0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2]
    L0, L1 = lum(p0), lum(p1)
    ui0 = (a0 > 0) | (r0.max(2) > 0)
    ui1 = (a1 > 0) | (r1.max(2) > 0)
    s0 = np.where(~ui0, L0, 0.0)
    s1 = np.where(~ui1, L1, 0.0)
    uim0 = np.where(ui0, lum(r0), 0.0)
    uim1 = np.where(ui1, lum(r1), 0.0)

    # The UI's own disparity, from the same frame's two layers.
    w = mask_window(ui0, RX, RY)
    rui = ncc_shift(uim0[w], uim1[w], RX, RY) if w else None
    u_ui = abs(rui[0]) / K if rui and rui[3] >= MIN_MARGIN else None

    h, wd = L0.shape
    depth = {}
    for ty in range(GRID):
        for tx in range(GRID):
            y0, y1 = ty * h // GRID, (ty + 1) * h // GRID
            x0, x1 = tx * wd // GRID, (tx + 1) * wd // GRID
            subs = []
            for sy in range(DSUB):
                for sx in range(DSUB):
                    yy0 = y0 + sy * (y1 - y0) // DSUB
                    yy1 = y0 + (sy + 1) * (y1 - y0) // DSUB
                    xx0 = x0 + sx * (x1 - x0) // DSUB
                    xx1 = x0 + (sx + 1) * (x1 - x0) // DSUB
                    a, b = s0[yy0:yy1, xx0:xx1], s1[yy0:yy1, xx0:xx1]
                    if (a > 0.001).sum() < MIN_TILE_PX:
                        continue
                    r = ncc_shift(a, b, RX, RY)
                    if r is not None and r[3] >= MIN_MARGIN:
                        subs.append(r[0])
            if len(subs) < 2:
                continue
            # One depth, or several? A tile holding both console and starfield
            # must not lend its median depth to a single motion number.
            if max(subs) - min(subs) > DEPTH_SPREAD_PX:
                continue
            depth[(tx, ty)] = abs(float(np.median(subs))) / K
    return s0, uim0, depth, u_ui, ui0


def analyse(lo, hi, prefix, burst_log):
    groups, gap = burst_groups(burst_log)
    if not groups:
        sys.exit(f'NO BURSTS in {burst_log} — nothing here is adjacent.')
    adjacent = set()
    for g in groups:
        for i in range(len(g) - 1):
            adjacent.add((g[i], g[i + 1]))

    print(f'  {len(groups)} burst(s), frames {gap:.1f} ms apart' if gap
          else f'  {len(groups)} burst(s)')
    print(f'  model: dx = P*(1+x^2) + Q*u   P = f*theta (rotation, field '
          f'dependent, depth free)\n{"":24}Q = f*d  (translation, depth '
          f'dependent, field free)')
    print(f'\n{"pair":>9} {"tiles":>5} {"P":>7} {"Q":>7} {"u range":>8} '
          f'{"dx_ui":>6} {"excess":>6} {"offer":>6} {"z_eff":>7}  verdict')

    prev = prev_n = None
    zs, excesses, offers, resid = [], [], [], []
    unconditioned, skipped = 0, 0
    for n in range(lo, hi + 1):
        cur = frame_data(prefix, n)
        if cur is None:
            continue
        if prev is not None and (prev_n, n) in adjacent:
            s = shift2(prev[0], cur[0])
            u_shift = shift2(prev[1], cur[1])
            if (s and u_shift and not s[3] and not u_shift[3]
                    and abs(s[0]) >= MIN_SCENE
                    and abs(u_shift[0]) <= MAX_MOTION
                    and prev[3] is not None):
                rows = []
                h, wd = prev[0].shape
                for (tx, ty), uu in prev[2].items():
                    y0, y1 = ty * h // GRID, (ty + 1) * h // GRID
                    x0, x1 = tx * wd // GRID, (tx + 1) * wd // GRID
                    t = tile_shift(prev[0][y0:y1, x0:x1],
                                   cur[0][y0:y1, x0:x1],
                                   s[0], s[1], int(abs(s[0])) + 24)
                    if t is None or t[2]:
                        continue
                    rows.append((field_weighted(
                        prev[0][y0:y1, x0:x1], x0, x1), uu, t[0]))
                # UI motion PER TILE, each with its own field factor.
                # Never a single f_ui at the UI's mean position: (1+x^2) is
                # CONVEX, so the mean of the factor over content spread across
                # the screen is 1.68 while the factor at the mean position is
                # 1.00. That 1.68x understates the predicted motion and
                # manufactures precisely the "UI moves more than physics
                # allows" excess this tool exists to test for. Caught by the
                # z_eff column coming back negative.
                uirows = []
                for (tx, ty, y0, y1, x0, x1) in tiles(prev[4], GRID,
                                                      MIN_UI_PX):
                    t = tile_shift(prev[1][y0:y1, x0:x1],
                                   cur[1][y0:y1, x0:x1],
                                   u_shift[0], u_shift[1],
                                   int(abs(u_shift[0])) + 24)
                    if t is not None and not t[2]:
                        uirows.append((field_weighted(
                            prev[1][y0:y1, x0:x1], x0, x1), t[0]))
                if len(rows) >= MIN_TILES and uirows:
                    xf = np.array([r[0] for r in rows])
                    uu = np.array([r[1] for r in rows])
                    dd = np.array([r[2] for r in rows])
                    P, Q, cond = fit_pair(xf, uu, dd)
                    u_ui = prev[3]
                    # What has to be explained, per tile: the UI's motion
                    # beyond the rotation already accounted for AT THAT TILE.
                    exc = [d - P * f for f, d in uirows]
                    # What translation can offer at the UI's depth.
                    offer = Q * u_ui
                    excesses.extend(exc)
                    offers.append(offer)
                    # **THE CONTROL.** The same model on the SCENE tiles, whose
                    # depth was measured. If this residual is as large as the
                    # UI's excess then the model is simply wrong and the excess
                    # says nothing about the UI. Without it, "the UI has a
                    # 20 px excess" cannot be told from "the model has 20 px
                    # of error".
                    resid.extend(dd - (P * xf + Q * uu))
                    if abs(Q) < MIN_Q_PX or cond < 0.2:
                        unconditioned += 1
                        tag = (f'UNCONDITIONED — |Q|={abs(Q):.1f} px, u range '
                               f'{cond:.2f}: translation not identifiable')
                        z_txt = '     --'
                    else:
                        ze = [Q / e for e in exc if abs(e) > 1e-6]
                        ze = [v for v in ze if 0.05 < v < 50]
                        if ze:
                            zm = float(np.median(ze))
                            zs.append(zm)
                            z_txt = f'{zm:7.2f}'
                            tag = (f'moves like {zm:.2f} m; drawn at '
                                   f'{1/u_ui:.2f} m')
                        else:
                            z_txt = '     --'
                            tag = ('no physical z_eff — the excess has the '
                                   'wrong sign for this pair\'s translation')
                    print(f'{prev_n:4d}->{n:<4d} {len(rows):5d} {P:+7.1f} '
                          f'{Q:+7.1f} {cond:8.2f} '
                          f'{np.median([d for _, d in uirows]):+6.0f} '
                          f'{np.median(exc):+6.0f} {offer:+6.1f} {z_txt}  '
                          f'{tag}')
                else:
                    skipped += 1
        prev, prev_n = cur, n

    print(f'\n  {len(zs)} pair(s) conditioned, {unconditioned} UNCONDITIONED '
          f'(head barely translated), {skipped} with under {MIN_TILES} '
          f'depth-homogeneous tiles')

    # **The primary verdict, and it needs no division by Q.** z_eff divides by
    # the translation term, so when the head barely moved it is a ratio of two
    # small noisy numbers and swings wildly. This comparison does not divide:
    # how much UI motion is left unexplained by rotation, against the most
    # that translation could possibly contribute at the UI's own depth. If the
    # excess dwarfs the offer, no placement of the canvas can account for it.
    if excesses and offers:
        ex = float(np.median(np.abs(excesses)))
        of = float(np.median(np.abs(offers)))
        print(f'\n  WHAT NEEDS EXPLAINING vs WHAT PARALLAX CAN OFFER, per pair:')
        print(f'    UI motion beyond rotation (|excess|)   median {ex:6.1f} px')
        print(f'    translation available at the UI depth  median {of:6.1f} px')
        rs = float(np.median(np.abs(resid))) if resid else float('nan')
        print(f'    SCENE fit residual, same model, same tiles  '
              f'median {rs:6.1f} px  <- THE CONTROL')
        print(f'    ratio excess/offer {ex / of:.1f}x' if of > 1e-6
              else '    offer is zero')
        if not (rs < 0.5 * ex):
            print(f'  => INCONCLUSIVE. The scene tiles, whose depths were '
                  f'MEASURED, are themselves fitted only to {rs:.1f} px by '
                  f'this model — comparable to the UI\'s {ex:.1f} px excess. '
                  f'The model is not accurate enough here to attribute that '
                  f'excess to the UI rather than to itself. No verdict.')
            return
        if of > 1e-6 and ex > 3 * of:
            print(f'  => TRANSPORT, and parallax is REFUTED as the cause. The '
                  f'HUD carries {ex:.0f} px of motion that rotation does not '
                  f'explain, while the head translation measured from the '
                  f'scene\'s own depth spread can supply at most {of:.0f} px '
                  f'at 1.37 m. No canvas DISTANCE can close a {ex/of:.0f}x '
                  f'gap — moving the canvas changes the offer, not the '
                  f'excess. Something transports the UI.')
        elif of > 1e-6 and ex < 1.5 * of:
            print(f'  => PLACEMENT is sufficient: the excess is within what '
                  f'translation at this depth supplies, so the canvas '
                  f'DISTANCE is the lever.')
        else:
            print(f'  => between the two; this data does not separate them.')

    if not zs:
        print('\n  No pair had translation large enough to invert for z_eff, '
              'so the depth-equivalent is not reported. That is a statement '
              'about the head motion in this take, not about the HUD.')
        return
    z = np.array([v for v in zs if 0.05 < v < 50])
    print(f'\n  {len(z)} pair(s) give a physically meaningful z_eff '
          f'(0.05..50 m)')
    if len(z) < 4:
        print('  too few to summarise.')
        return
    med = float(np.median(z))
    print(f'  z_eff  median {med:.2f} m   IQR '
          f'{np.percentile(z,25):.2f}..{np.percentile(z,75):.2f} m')
    print(f'  the canvas is drawn at 1.37 m')
    if med > 1.15:
        print(f'  => PLACEMENT. The HUD moves like something at {med:.2f} m, '
              f'consistent with the plane it is drawn on. Nothing is '
              f'over-moving it; the slide follows from the canvas DISTANCE.')
    elif med < 1.0:
        print(f'  => TRANSPORT. The HUD moves like something at {med:.2f} m, '
              f'NEARER than the 1.37 m it is drawn at, so it is being moved '
              f'further than its own depth allows. The canvas distance is not '
              f'the cause.')
    else:
        print(f'  => z_eff {med:.2f} m straddles the canvas distance; this '
              f'data does not separate placement from transport.')


def selftest():
    """Recover a known (P, Q) and a known z_eff, including an over-moved UI."""
    fails = 0
    rng = np.random.default_rng(7)
    P_true, Q_true = 30.0, 12.0
    centres = np.array([234.0, 704.0, 1173.0])
    xf, uu = [], []
    for cx in centres:
        for z in (0.6, 1.4, 6.0, 1e6):
            xf.append(field(cx))
            uu.append(1.0 / z)
    xf, uu = np.array(xf), np.array(uu)
    dx = P_true * xf + Q_true * uu
    P, Q, cond = fit_pair(xf, uu, dx)
    ok = abs(P - P_true) < 0.01 and abs(Q - Q_true) < 0.01
    print(f'  exact fit          P {P:+.2f} (want {P_true:+.2f})  '
          f'Q {Q:+.2f} (want {Q_true:+.2f})  -> '
          f'{"ok" if ok else "FAIL"}')
    fails += 0 if ok else 1

    dxn = dx + rng.normal(0, 1.5, dx.shape)
    P, Q, cond = fit_pair(xf, uu, dxn)
    ok = abs(P - P_true) < 3 and abs(Q - Q_true) < 3
    print(f'  with 1.5 px noise  P {P:+.2f}  Q {Q:+.2f}  u range {cond:.2f}'
          f'  -> {"ok" if ok else "FAIL"}')
    fails += 0 if ok else 1

    # A UI at 1.37 m that moves as it should must read back 1.37 m...
    f_ui, u_ui = field(704.0), 1 / 1.37
    dx_ui = P_true * f_ui + Q_true * u_ui
    P, Q, _ = fit_pair(xf, uu, dx)
    z_eff = 1.0 / ((dx_ui - P * f_ui) / Q)
    ok = abs(z_eff - 1.37) < 0.02
    print(f'  honest UI          z_eff {z_eff:.2f} m (want 1.37) -> '
          f'{"ok" if ok else "FAIL"}')
    fails += 0 if ok else 1

    # ...and one transported 1.4x further must read back NEARER, or this tool
    # cannot tell the two worlds apart and is worth nothing.
    z_eff = 1.0 / ((1.4 * dx_ui - P * f_ui) / Q)
    ok = 0.0 < z_eff < 1.37
    print(f'  over-moved 1.4x    z_eff {z_eff:.2f} m (want < 1.37, > 0) -> '
          f'{"ok" if ok else "FAIL"}')
    fails += 0 if ok else 1

    # Collinear tiles: every tile at one depth cannot identify Q.
    u1 = np.full_like(uu, 1 / 1.4)
    _, _, cond = fit_pair(xf, u1, P_true * xf + Q_true * u1)
    ok = cond < 0.2
    print(f'  all one depth      u range {cond:.2f} -> '
          f'{"flagged unconditioned (ok)" if ok else "NOT FLAGGED (FAIL)"}')
    fails += 0 if ok else 1

    print(f'  {"FAIL" if fails else "ok  "} depth_motion selftest, '
          f'{fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('lo', nargs='?', type=int)
    ap.add_argument('hi', nargs='?', type=int)
    ap.add_argument('--prefix', default='/tmp/x4vr-t260')
    ap.add_argument('--burst-log')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    if a.lo is None or a.hi is None or not a.burst_log:
        sys.exit('need: lo hi --burst-log LOG [--prefix P]')
    analyse(a.lo, a.hi, a.prefix, a.burst_log)
