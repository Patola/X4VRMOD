#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""#76 follow-up: is the sliding HUD ONE object, or a mixture of parts?

    tools/hud_tiles.py 0 600 --burst-log /tmp/x4vr-take260.log --prefix /tmp/x4vr-t260
    tools/hud_tiles.py --selftest

`hud_slide.py` measures one ratio per pair: (UI motion) / (scene motion). Take
260 put it at +1.77 on the well-measured pairs, multiplicative, with a spread
of +-0.26 that is FIVE TIMES the stated per-pair error. That spread is the
question this file exists to answer, because two very different worlds produce
the same +1.77:

    ONE OBJECT   every part of the UI moves at +1.77, and a single transform
                 in our pipeline is scaling its motion. Find that transform.
    A MIXTURE    some parts sit at +1 (world-anchored brackets) and others run
                 well past +2, and +1.77 is the average of things that should
                 never have been averaged. Then "the HUD slides at 1.77x" is
                 not a fact about anything, and a fix aimed at it cannot work.

**The existing control cannot tell these apart, and that is why this is a new
file rather than a flag.** `hud_slide.py` validates each channel against a
CHECKERBOARD half of itself. A checkerboard samples every part of the screen in
both halves, so a mixture appears in both halves equally and the control reads
a contented +1.00 either way. Only a SPATIAL split can separate them.

**The range, stated before the run** -- an instrument that cannot reach the
alternative cannot refute it. The per-tile search is centred on the global UI
shift with radius |global| + 24 px, so it spans

    -24 px  ..  2 * (global UI shift) + 24 px

which contains BOTH rival answers: a screen-locked tile at 0 and a
double-moving tile at 2x. A tile that lands on the edge of that window is
reported as AT BOUND rather than as a number, because at the edge the true peak
is outside the search and the offset is the bound, not a measurement.

Pairs whose global UI motion exceeds MAX_UI are skipped: the search window
would then be wider than the tile and the correlation core would vanish. That
is a stated limit of this tool, not a silent filter -- the count is printed.
"""
import argparse
import sys

import numpy as np

sys.path.insert(0, '/home/patola/workspace/claude/X4VRMOD/tools')
from ui_disparity import decode_rgba, ncc_shift, tiles   # noqa: E402
from hud_vs_camera import decode                         # noqa: E402
from hud_slide import burst_groups, shift2, COARSE, ds   # noqa: E402

GRID = 3            # 3x3 tiles of 469 px on a 1408 image
MIN_TILE_PX = 1500  # UI pixels a tile needs before it is worth correlating
MIN_TILES = 4       # tiles a pair needs before its spread means anything
MAX_UI = 100        # px of global UI motion this tool can tile (see above)
MIN_SCENE = 15      # same floor hud_slide.py uses
CTRL_TOL = 0.25
CTRL_FLOOR_PX = 6   # a control tolerance must survive a tile that does not move
FINE_R = 6
# A spread this wide cannot be one rigid object. Chosen from the per-pair error
# hud_slide.py already reports (median 0.10) and the 4 px coarse quantisation:
# at a 60 px UI motion, 4 px is 0.07, so tiles of ONE object should agree to
# well inside 0.20 -- and IQR is about twice MAD for a normal, hence 0.35.
# Stated here, before the data is read.
#
# **IQR, not MAD, and that is the whole point.** The first version used the
# median absolute deviation and its own selftest exposed the flaw: the mixture
# case gave tile ratios [1,1,1,0,0,0,0,0,0], whose median is 0 and whose MAD is
# therefore 0.00 -- a PERFECT "one object" verdict on a textbook mixture. MAD
# collapses to zero whenever more than half the values tie, so a HUD that was
# mostly screen-locked with a moving minority would have been reported as
# rigid. A dispersion statistic has to be able to express the alternative it is
# being used to rule out.
ONE_OBJECT_IQR = 0.35


def spread(v):
    """Interquartile range — a dispersion that survives ties. See ONE_OBJECT_IQR."""
    s = sorted(v)
    n = len(s)
    return s[min(n - 1, (3 * n) // 4)] - s[n // 4]


def channels(prefix, n, raw_scene=False):
    """(scene, ui, ui_mask) for dump n, or None. Same construction as #76.

    `raw_scene` leaves the UI pixels IN the scene channel instead of zeroing
    them. **This is an elimination, not a convenience.** #76's scene channel is
    the present image with UI-shaped holes punched in it, and those holes MOVE
    WITH THE UI. A scene tile containing a large hole therefore carries a
    hole-edge signal at the UI's rate, which would show up as the scene
    disagreeing with itself — the exact result being tested for. Re-measuring
    with the holes filled separates a real depth mixture from an artifact of
    the mask. The UI is ~3% of a cockpit frame, so leaving it in barely
    perturbs a dense correlation.
    """
    try:
        p = decode(f'{prefix}-present-n{n}-layer0.png')
        r, al = decode_rgba(f'{prefix}-ui-n{n}-layer0.png')
    except Exception:
        return None
    L = 0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2]
    uim = (al > 0) | (r.max(2) > 0)
    uil = 0.2126 * r[..., 0] + 0.7152 * r[..., 1] + 0.0722 * r[..., 2]
    scene = L if raw_scene else np.where(~uim, L, 0.0)
    return scene, np.where(uim, uil, 0.0), uim


def tile_shift(a, b, cx, cy, rad):
    """Shift of one tile, searched around (cx, cy) with radius `rad` px.

    Returns (dx, dy, at_bound) or None. Coarse-to-fine like shift2, but the
    window is placed by the caller so the search can span both rival answers.
    """
    k = COARSE
    ca, cb = ds(a, k), ds(b, k)
    rk = max(2, int(round(rad / k)))
    h, w = ca.shape
    if min(h, w) <= 2 * (rk + 2) + 8:
        return None                      # no correlation core would be left
    # Search around the centre by rolling b to the centre first, so ncc_shift's
    # symmetric window does the rest.
    ox, oy = int(round(cx / k)), int(round(cy / k))
    rolled = np.roll(np.roll(cb, -oy, axis=0), -ox, axis=1)
    c = ncc_shift(ca, rolled, rk, rk)
    if c is None:
        return None
    at_bound = abs(c[0]) >= rk or abs(c[1]) >= rk
    dx, dy = (ox + c[0]) * k, (oy + c[1]) * k
    bb = np.roll(np.roll(b, -dy, axis=0), -dx, axis=1)
    f = ncc_shift(a, bb, FINE_R, FINE_R)
    if f is None:
        return None
    return dx + f[0], dy + f[1], at_bound


def shift_fill(a, dx, dy):
    """Shift by (dx, dy) with ZERO fill, not np.roll's wrap.

    A wrap would move real content into the opposite edge tiles, which are
    exactly the tiles the rigid control has to be trusted on.
    """
    out = np.zeros_like(a)
    h, w = a.shape
    ys, yd = (slice(0, h - dy), slice(dy, h)) if dy >= 0 else (slice(-dy, h), slice(0, h + dy))
    xs, xd = (slice(0, w - dx), slice(dx, w)) if dx >= 0 else (slice(-dx, w), slice(0, w + dx))
    out[yd, xd] = a[ys, xs]
    return out


def checker(t):
    yy, xx = np.indices(t.shape)
    chk = ((yy // 2 + xx // 2) & 1).astype(bool)
    return np.where(chk, t, 0.0), np.where(~chk, t, 0.0)


def analyse(lo, hi, prefix, burst_log, chan=1, raw_scene=False):
    groups, gap = burst_groups(burst_log)
    if not groups:
        sys.exit(f'NO BURSTS in {burst_log} — nothing is adjacent, so no pair '
                 'here is measurable. Not a null about the HUD.')
    adjacent = set()
    for g in groups:
        for i in range(len(g) - 1):
            adjacent.add((g[i], g[i + 1]))

    print(f'  {len(groups)} burst(s), frames {gap:.1f} ms apart'
          if gap else f'  {len(groups)} burst(s)')
    print(f'  per-tile search spans -24 .. 2x(global UI) + 24 px — a '
          f'screen-locked tile (0) and a double tile (2x) are both inside it')
    print(f'\n{"pair":>9} {"scene":>7} {"ui":>7} {"glob":>6}  '
          f'{"tiles":>5} {"ratios (per tile)":<28} {"IQR":>5} {"ctrl":>6}  '
          f'verdict')

    prev = prev_n = None
    per_pair, skipped_big, no_tiles, decoded = [], 0, 0, 0
    by_pos = {}
    matched = []
    for n in range(lo, hi + 1):
        cur = channels(prefix, n, raw_scene)
        if cur is None:
            continue
        decoded += 1
        if prev is not None and (prev_n, n) in adjacent:
            s = shift2(prev[0], cur[0])
            u = shift2(prev[1], cur[1])
            if s and u and not s[3] and not u[3] and abs(s[0]) >= MIN_SCENE:
                g_ratio = u[0] / s[0]
                # In matched mode BOTH channels get tiled, so the range guard
                # must cover whichever moved further — guarding on the scene
                # alone would let a large UI shift through with a search window
                # wider than the tile.
                g = (u if chan == 1 else s if chan == 0 else
                     (u if abs(u[0]) > abs(s[0]) else s))
                if abs(g[0]) > MAX_UI or abs(g[1]) > MAX_UI:
                    skipped_big += 1
                elif chan == 2:
                    # **THE MATCHED MEASUREMENT.** Both channels, same pair,
                    # same tile: how much further does the HUD move than
                    # whatever is behind it AT THAT PLACE ON SCREEN.
                    #
                    # Comparing the two separate per-position maps does NOT do
                    # this. They are built over different pair populations --
                    # the scene tiles 200 pairs, the UI only the 35 whose
                    # motion is under MAX_UI, which are the small-motion pairs
                    # whose ratios run high. Dividing one map by the other
                    # compares different runs of the world and cancels nothing.
                    # Here every ratio is one pair, one tile, both channels.
                    srow = {(r[0], r[1]): r[2]
                            for r in tile_pair(prev, cur, s, 0)}
                    urow = {(r[0], r[1]): r[2]
                            for r in tile_pair(prev, cur, u, 1)}
                    both = [(k, urow[k] / srow[k]) for k in urow
                            if k in srow and abs(srow[k]) >= 4]
                    if len(both) >= MIN_TILES:
                        for k, v in both:
                            by_pos.setdefault(k, []).append(v)
                        vals = sorted(v for _, v in both)
                        matched.append(float(np.median(vals)))
                        print(f'{prev_n:4d}->{n:<4d} {s[0]:+7.0f} {u[0]:+7.0f} '
                              f'{g_ratio:+6.2f}  {len(both):5d} '
                              f'{" ".join(f"{v:+.2f}" for _, v in both[:6]):<28} '
                              f'{spread(vals):5.2f} {0.0:6.2f}  '
                              f'matched median {np.median(vals):+.2f}')
                    else:
                        no_tiles += 1
                else:
                    rows = tile_pair(prev, cur, g, chan)
                    # The SAME frame displaced rigidly by the SAME amount, tiled
                    # the same way: whatever spread this shows is the floor.
                    ctrl = tile_pair(prev, cur, g, chan,
                                     rigid=shift_fill(prev[chan],
                                                      int(g[0]), int(g[1])))
                    if len(rows) >= MIN_TILES and len(ctrl) >= MIN_TILES:
                        rs = [r[2] / s[0] for r in rows]
                        cs = [c[2] / g[0] for c in ctrl]
                        for tx, ty, dx in rows:
                            by_pos.setdefault((tx, ty), []).append(dx / s[0])
                        med = float(np.median(rs))
                        iqr, ciqr = spread(rs), spread(cs)
                        txt = ' '.join(f'{x:+.2f}' for x in rs[:6])
                        verdict = ('tiles AGREE — one object'
                                   if iqr <= max(ONE_OBJECT_IQR, 2 * ciqr) else
                                   'tiles DISAGREE — a mixture')
                        print(f'{prev_n:4d}->{n:<4d} {s[0]:+7.0f} {u[0]:+7.0f} '
                              f'{g_ratio:+6.2f}  {len(rs):5d} {txt:<28} '
                              f'{iqr:5.2f} {ciqr:6.2f}  {verdict}')
                        per_pair.append((med, iqr, len(rs), ciqr))
                    else:
                        no_tiles += 1
        prev, prev_n = cur, n

    # **Matched mode reports first.** It fills `matched`, not `per_pair`, and
    # the generic block below returns early on an empty `per_pair` — so the
    # smoke test printed four matched medians and then "NO PAIR could be
    # tiled" underneath them. A summary that contradicts the rows above it is
    # worse than no summary, because the summary is what gets quoted.
    if chan == 2:
        print(f'\n  {decoded} frame(s) decoded under {prefix}')
        print(f'  {len(matched)} pair(s) had >= {MIN_TILES} tiles measurable in '
              f'BOTH channels, {skipped_big} skipped (UI motion over {MAX_UI} '
              f'px), {no_tiles} with too few shared tiles')
        if not matched:
            print('  NO PAIR had both channels measurable at a shared tile — '
                  'this says nothing about the HUD.')
            return
        print(f'  UI motion / SCENE motion AT THE SAME TILE, same pair:')
        print(f'    median over pairs {np.median(matched):+.2f}   '
              f'IQR {spread(matched):.2f}   n={len(matched)}')
        print(f'\n  PER-TILE-POSITION median of that matched ratio:')
        for ty in range(GRID):
            cells = []
            for tx in range(GRID):
                v = by_pos.get((tx, ty))
                cells.append(f'{np.median(v):+5.2f}({len(v):3d})' if v
                             else '   --      ')
            print('      ' + ' '.join(cells))
        print(f'\n  +1.00 everywhere = the HUD moves with whatever is behind '
              f'it at that spot.\n  Above +1 = it slides. This cancels the '
              f'field dependence that made\n  the single global ratio '
              f'meaningless, because both channels are read\n  at the SAME '
              f'screen position in the SAME pair.')
        return

    print(f'\n  {decoded} frame(s) decoded under {prefix}')
    print(f'  {len(per_pair)} pair(s) tiled, {skipped_big} skipped (UI motion '
          f'over {MAX_UI} px, past this tool\'s tiling range), '
          f'{no_tiles} with under {MIN_TILES} usable tiles')
    if not per_pair:
        print('  NO PAIR could be tiled — this says nothing about the HUD, '
              'only that no pair met the range and coverage this tool needs.')
        return
    iqrs = [p[1] for p in per_pair]
    meds = [p[0] for p in per_pair]
    ctrls = [p[3] for p in per_pair]
    typical = float(np.median(iqrs))
    floor = float(np.median(ctrls))
    print(f'  per-tile ratio: median of pair medians {np.median(meds):+.2f}')
    print(f'  per-pair spread ACROSS TILES: median IQR {typical:.2f}')
    print(f'  RIGID CONTROL, same frames displaced by a known amount: '
          f'median IQR {floor:.2f}')
    # **The control sets the bar, not the constant.** A tile of a sparse UI is
    # a far worse estimator than the whole frame, and ONE_OBJECT_IQR was
    # reasoned from the global precision. Whatever spread the rigid control
    # shows is what this instrument produces on something that IS one object.
    bar = max(ONE_OBJECT_IQR, 2 * floor)
    over = sum(1 for x in iqrs if x > bar)
    print(f'  bar = max(stated {ONE_OBJECT_IQR:.2f}, 2x control {2*floor:.2f}) '
          f'= {bar:.2f}; {over}/{len(iqrs)} pair(s) over it')
    if typical <= bar:
        print(f'  => NO EVIDENCE OF A MIXTURE. The tile spread is within what '
              f'this instrument produces on a rigid displacement of the same '
              f'frames, so it cannot separate one object from several here.')
        if floor >= ONE_OBJECT_IQR:
            print(f'  The control is ABOVE the stated threshold on its own, so '
                  f'the threshold was too tight for tile-scale sparsity and '
                  f'any verdict read off it alone would have been noise.')
    else:
        rel = (f'{typical/floor:.1f}x the rigid control' if floor > 0.01
               else f'far above the rigid control, which reads {floor:.2f} — '
                    f'the machinery contributes nothing measurable here')
        print(f'  => A MIXTURE. The tile spread is {rel}, so parts of this '
              f'channel move at different rates and the single ratio '
              f'hud_slide.py reports averages over causes that differ.')
        # **The control is EASIER than the measurement, and that is a limit on
        # this verdict.** It displaces identical content by an exact integer,
        # while real consecutive frames also differ by new content, lighting
        # and geometry that moved. So 0.00 bounds the CORRELATOR's error, not
        # frame-to-frame change. The spatial map below is what separates real
        # structure from per-pair noise: noise cannot hold a stable position.
        print()
        print(f'  PER-TILE-POSITION median ratio over all pairs — a stable '
              f'spatial pattern is structure, a shuffled one is noise:')
        for ty in range(GRID):
            cells = []
            for tx in range(GRID):
                v = by_pos.get((tx, ty))
                cells.append(f'{np.median(v):+5.2f}({len(v):3d})' if v
                             else '   --      ')
            print('      ' + ' '.join(cells))


def tile_pair(pa, pb, u, chan=1, rigid=None):
    """Per-tile dx, searched around the global shift `u` of that channel.

    `chan` selects which channel is tiled: 1 = UI, 0 = scene.

    `rigid` replaces frame B with a supplied image. Passing
    `shift_fill(pa[chan], u)` makes this THE NEGATIVE CONTROL: the same real
    frame, the same sparsity, the same tiles, the same search, displaced by a
    known amount that is rigid by construction. Its tile spread is the noise
    floor, and without it the mixture threshold is a number I picked.

    **The scene needs tiling for its own reason, not as a control.** The
    measurement divides by a single "scene motion", and in the cockpit the
    scene is not one thing: the ship's interior sits about a metre away and the
    starfield is at infinity. Take 260's head moved over a 0.26 m span, and 5 cm
    of sideways translation shifts an object at 1.4 m by about 26 px while
    leaving infinity alone. If the scene's own tiles disagree, then the
    denominator of every #76 ratio is an average over depths and the ratio is
    not a property of the HUD at all.
    """
    rad = int(abs(u[0])) + 24
    out = []
    mask = pa[2] if chan == 1 else (pa[0] > 0.02)
    for (_tx, _ty, y0, y1, x0, x1) in tiles(mask, GRID, MIN_TILE_PX):
        ta, tb = pa[chan][y0:y1, x0:x1], pb[chan][y0:y1, x0:x1]
        if rigid is not None:
            tb = rigid[y0:y1, x0:x1]
        t = tile_shift(ta, tb, u[0], u[1], rad)
        if t is None or t[2]:
            continue
        # Same sparsity, same tile, same instant: the halves must agree before
        # this tile's number is allowed to speak.
        a1, a2 = checker(ta)
        b1, b2 = checker(tb)
        c1 = tile_shift(a1, b1, u[0], u[1], rad)
        c2 = tile_shift(a2, b2, u[0], u[1], rad)
        if not c1 or not c2:
            continue
        # **In pixels, not as a ratio against the tile's own shift.** The first
        # version divided by t[0] and skipped the tile when it was zero -- so a
        # SCREEN-LOCKED tile, which is one of the two answers this tool exists
        # to find, was discarded by the control before it could be counted.
        # The selftest's mixture case caught it: 3 tiles survived instead of 6.
        # A tolerance that vanishes with the measured value cannot judge a null.
        tol = max(CTRL_FLOOR_PX, CTRL_TOL * abs(t[0]))
        if abs(c1[0] - t[0]) > tol or abs(c2[0] - t[0]) > tol:
            continue
        out.append((_tx, _ty, t[0]))
    return out


def selftest():
    """Separate a rigid UI from a known mixture, or this tool proves nothing.

    Both cases are built at the sparsity and tile size of the real thing. A
    tool that only separates them on a dense synthetic image would be answering
    an easier question than the one it is used for.
    """
    rng = np.random.default_rng(20260906)
    H = W = 1408
    fails = 0

    def build(col_shifts):
        """Per-tile-COLUMN shifts, split on the tile boundaries.

        The splits land at x = W/3 and 2W/3 so every tile is pure. An earlier
        version split at W/2, straddling the middle column, which meant a
        failure could be blamed on the tiling instead of on the statistic under
        test. A selftest should exercise one thing.
        """
        a = np.zeros((H, W))
        m = np.zeros((H, W), bool)
        for _ in range(900):                 # sparse blobs, like HUD chrome
            y, x = rng.integers(24, H - 24), rng.integers(24, W - 24)
            a[y:y + 9, x:x + 9] = rng.uniform(0.4, 1.0)
            m[y:y + 9, x:x + 9] = True
        b = np.zeros_like(a)
        for i, sh in enumerate(col_shifts):
            x0, x1 = i * W // GRID, (i + 1) * W // GRID
            b[:, x0:x1] = np.roll(a, sh, axis=1)[:, x0:x1]
        return a, b, m

    def case(name, col_shifts, want_agree):
        nonlocal fails
        a, b, m = build(col_shifts)
        rows = tile_pair((None, a, m), (None, b, m), (40, 0, 1.0, False))
        if len(rows) < MIN_TILES:
            print(f'  FAIL {name}: only {len(rows)} tiles survived, '
                  'cannot judge')
            fails += 1
            return
        r = sorted(x[2] / 40.0 for x in rows)
        iqr = spread(r)
        agree = iqr <= ONE_OBJECT_IQR
        good = 'correct' if agree == want_agree else 'WRONG'
        print(f'  {name}: {len(r)} tiles {[f"{x:+.1f}" for x in r]} '
              f'median {np.median(r):+.2f} IQR {iqr:.2f} -> '
              f'{"AGREE" if agree else "DISAGREE"} ({good})')
        if agree != want_agree:
            fails += 1
        if want_agree and abs(np.median(r) - 1.0) > 0.15:
            print(f'  FAIL {name}: recovered the wrong magnitude')
            fails += 1

    # One object: every column moves together.
    case('A rigid          ', (40, 40, 40), True)
    # A mixture where the moving part is the MINORITY. This is the case that
    # exposed MAD: ratios [1,1,1,0,0,0,0,0,0] have MAD exactly 0.00.
    case('B minority moves ', (40, 0, 0), False)
    # ...and the mirror image, where the STATIC part is the minority. Both
    # must read DISAGREE, or the statistic only works from one side.
    case('C minority locked', (40, 40, 0), False)

    # D: the RIGID CONTROL, run on the mixture image. The control replaces
    # frame B with frame A displaced by a known amount, so it must read AGREE
    # even where the real pair is a mixture -- it measures the INSTRUMENT, not
    # the data. If it inherited the mixture it would excuse any spread.
    a, b, m = build((40, 0, 0))
    rows = tile_pair((None, a, m), (None, b, m), (40, 0, 1.0, False),
                     rigid=shift_fill(a, 40, 0))
    if len(rows) < MIN_TILES:
        print(f'  FAIL D control: only {len(rows)} tiles survived')
        fails += 1
    else:
        r = sorted(x[2] / 40.0 for x in rows)
        iqr = spread(r)
        ok_ = iqr <= ONE_OBJECT_IQR
        print(f'  D rigid control  : {len(r)} tiles median {np.median(r):+.2f} '
              f'IQR {iqr:.2f} -> '
              f'{"AGREE (correct)" if ok_ else "DISAGREE (WRONG — the control "
                                              "inherited the mixture)"}')
        if not ok_:
            fails += 1

    print(f'  {"FAIL" if fails else "ok  "} hud_tiles selftest, '
          f'{fails} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('lo', nargs='?', type=int)
    ap.add_argument('hi', nargs='?', type=int)
    ap.add_argument('--prefix', default='/tmp/x4vr-t260')
    ap.add_argument('--burst-log')
    ap.add_argument('--channel', choices=('ui', 'scene', 'both'), default='ui',
                    help='which channel to tile. "scene" asks whether the '
                         'DENOMINATOR of every #76 ratio is itself one object.')
    ap.add_argument('--raw-scene', action='store_true',
                    help='do NOT punch UI-shaped holes in the scene channel. '
                         'Those holes move with the UI, so a scene tile '
                         'containing one carries a signal at the UI rate — '
                         'which is the very disagreement being measured.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    if a.lo is None or a.hi is None or not a.burst_log:
        sys.exit('need: lo hi --burst-log LOG [--prefix P] [--channel ui|scene]')
    print(f'  tiling the {a.channel.upper()} channel, {GRID}x{GRID}'
          f'{", UI holes FILLED" if a.raw_scene else ""}')
    analyse(a.lo, a.hi, a.prefix, a.burst_log,
            {'ui': 1, 'scene': 0, 'both': 2}[a.channel], a.raw_scene)
