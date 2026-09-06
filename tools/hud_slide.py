#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Task #76: does the HUD slide against the cockpit, and by how much?

    tools/hud_slide.py 41 65

Patola, across takes 253/255/256/257: *"The HUD still goes to the opposite
side."* This measures it as a RATIO WITHIN EACH PAIR of consecutive dumps --

    ratio = (UI motion in the image) / (scene motion in the image)

-- because a ratio within one pair needs no calibration, no px/deg constant, and
no comparison against another run. Every cross-run and every calibrated version
of this measurement in this project has failed.

**What the ratio means.** The scene is the ship and the world, which move
together in the image when the camera turns:

    ratio  0   the HUD is screen-locked: it does not move in the image at all
    ratio +1   the HUD is cockpit-fixed -- it moves exactly with the ship,
               which is what Patola says is correct in the cockpit
    ratio +2   the HUD moves TWICE as far as the ship. Relative to the cockpit
               it then slides in the direction the world moves, i.e. AGAINST
               the head turn -- which is what "goes to the opposite side"
               looks like
    ratio <0   the HUD moves against the world in the image

**THE CONTROL IS THE POINT OF THIS FILE.** `hud_drift.py` and
`hud_vs_cockpit.py` were both retired for failing theirs, after their numbers
had been believed and built on. So each channel is also measured against a
CHECKERBOARD half of itself, over the same pair, at the same sparsity:

    scene_a vs scene_b   truth +1: the same rigid content, same instant pair
    ui_a    vs ui_b      truth +1: likewise, and separately because the UI
                         channel is far sparser than the scene channel and a
                         control proving the DENSE one works says nothing
                         about the sparse one

A pair where either control misses +1 is DROPPED, not explained. That is the
rule the retired tools broke.

**Stated before running, so a null cannot be reinterpreted afterwards:**
the search is coarse-to-fine, +-320 px at full resolution, which at roughly
12.8 px/deg is about +-25 deg of image motion. Motion beyond that is
unmeasurable and the pair is dropped rather than folded in at the bound --
several pairs in take 257 hit exactly that, because consecutive dumps there are
about 1.3 s apart in wall time and a head turns a long way in 1.3 s.
"""
import sys

import numpy as np

sys.path.insert(0, '/home/patola/workspace/claude/X4VRMOD/tools')
from ui_disparity import decode_rgba, ncc_shift          # noqa: E402
from hud_vs_camera import decode                         # noqa: E402

COARSE = 4          # downsample factor for the first pass
COARSE_R = 80       # +-80 at 1/4 res = +-320 px full res
FINE_R = 6
CTRL_TOL = 0.25     # how far a control may sit from +1 before the pair is void
MIN_MARGIN = 0.05
ADJACENT = None     # burst membership, set from --burst-log
MIN_MOTION = 15     # px of scene motion below which the ratio is quantisation
# The dump prefix was hard-coded to t257, and take 257's dumps had been deleted
# by the time take 258 landed: every decode would have failed, every frame been
# skipped, and the tool would have printed "no answer from this data" -- a false
# null attributed to the NEW run. A path that names one take must be an
# argument, and a prefix that matches nothing must be an error, not an answer.
PREFIX = '/tmp/x4vr-t258'


def ds(a, k=COARSE):
    h, w = a.shape[0] // k * k, a.shape[1] // k * k
    return a[:h, :w].reshape(h // k, k, w // k, k).mean((1, 3))


def shift2(a, b):
    """Coarse-to-fine (dx, dy, margin, at_bound). None when a stage fails.

    Two stages because the motion here is hundreds of pixels and a full-res
    brute force over that range is not affordable -- but a coarse-only answer
    is quantised to 4 px, the size of the effect on the quieter pairs.

    **The margin returned is the COARSE stage's, and that is not a detail.**
    The first version returned the FINE stage's, and every one of take 257's 24
    cockpit pairs was then rejected as "unmeasurable" while its own control sat
    at +1.00 -- the channels correlating perfectly while the gate threw them
    out. `ncc_shift`'s margin is `best - best_peak_more_than_3px_away`, which
    measures how ISOLATED the peak is. That is meaningful across a +-320 px
    search and meaningless inside a +-6 px refinement, where every offset is
    within a few pixels of the peak and the surface is smooth by construction.
    A discrimination metric only discriminates over the range it was built for;
    reusing it in the refinement made it answer a question that had moved.

    Take 256 hit the same wall from the other side: the margin gate, not the
    search range, was what rejected measurements whose channels agreed to the
    pixel. Second time. The gate belongs on the wide search, always.
    """
    c = ncc_shift(ds(a), ds(b), COARSE_R, COARSE_R // 3)
    if c is None:
        return None
    # At the edge of the search the true peak is outside it, and the reported
    # offset is the bound rather than a measurement. Say so instead of
    # returning a number that looks like one.
    at_bound = abs(c[0]) >= COARSE_R or abs(c[1]) >= COARSE_R // 3
    ox, oy = c[0] * COARSE, c[1] * COARSE
    bb = np.roll(np.roll(b, -oy, axis=0), -ox, axis=1)
    f = ncc_shift(a, bb, FINE_R, FINE_R)
    if f is None:
        return None
    return ox + f[0], oy + f[1], c[3], at_bound


def channels(n):
    """(scene, ui, scene_a, scene_b, ui_a, ui_b) for dump n, or None."""
    try:
        p = decode(f'{PREFIX}-present-n{n}-layer0.png')
        r, al = decode_rgba(f'{PREFIX}-ui-n{n}-layer0.png')
    except Exception:
        return None
    L = 0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2]
    uim = (al > 0) | (r.max(2) > 0)
    uil = 0.2126 * r[..., 0] + 0.7152 * r[..., 1] + 0.0722 * r[..., 2]
    scene = np.where(~uim, L, 0.0)
    ui = np.where(uim, uil, 0.0)
    # A CHECKERBOARD, not left half against right half. Content flows across a
    # screen boundary between frames, so a half-split control is HARDER than
    # the measurement -- and a control harder than the measurement cannot
    # condemn it. That mistake produced the 0.83 that voided take 255's rates.
    yy, xx = np.indices(scene.shape)
    chk = ((yy // 2 + xx // 2) & 1).astype(bool)
    return (scene, ui,
            np.where(chk, scene, 0.0), np.where(~chk, scene, 0.0),
            np.where(chk, ui, 0.0), np.where(~chk, ui, 0.0))


def burst_groups(log_path):
    """Frame numbers that are ADJACENT IN TIME, read from the layer's own log.

    The layer writes one `ui dump: burst frames a,b,c,...` line per burst. This
    does not re-derive them from the numbering: a slot with no eye dump is
    skipped before it consumes a sequence number, so "n..n+BURST-1 is a burst"
    goes wrong from the first skip onward and every later pair would be
    compared across a boundary while looking adjacent.
    """
    groups = []
    try:
        with open(log_path) as f:
            for line in f:
                i = line.find('ui dump: burst frames ')
                if i < 0:
                    continue
                rest = line[i + len('ui dump: burst frames '):]
                nums = rest.split(' ')[0].strip()
                try:
                    groups.append([int(v) for v in nums.split(',') if v != ''])
                except ValueError:
                    continue
    except OSError as e:
        print(f'could not read {log_path}: {e}', file=sys.stderr)
        return None
    return groups


def main(lo, hi):
    print(f'{"pair":>9} {"scene":>7} {"ui":>7} {"ratio":>7}  '
          f'{"ctl scene":>9} {"ctl ui":>7}  verdict')
    prev = prev_n = None
    ratios, dropped, void, skipped_gap, decoded = [], 0, 0, 0, 0
    for n in range(lo, hi + 1):
        cur = channels(n)
        if cur is None:
            continue
        decoded += 1
        # Only compare frames the layer says are adjacent. Without a burst log
        # every consecutive pair is compared, which is right for a run that had
        # no bursts and wrong for one that did.
        adjacent = (ADJACENT is None or
                    (prev_n is not None and
                     any(prev_n in g and n in g and
                         abs(g.index(n) - g.index(prev_n)) == 1
                         for g in ADJACENT)))
        if prev is not None and not adjacent:
            skipped_gap += 1
        elif prev is not None:
            s = shift2(prev[0], cur[0])
            u = shift2(prev[1], cur[1])
            ca = shift2(prev[2], cur[3])   # scene checkerboard, truth +1
            cu = shift2(prev[4], cur[5])   # ui checkerboard, truth +1
            tag, ok = '', True
            if s is None or u is None:
                tag, ok = 'unmeasurable', False
                dropped += 1
            elif s[3] or u[3]:
                tag, ok = 'motion beyond +-320 px — OUT OF RANGE', False
                dropped += 1
            elif s[2] < MIN_MARGIN or u[2] < MIN_MARGIN:
                tag, ok = (f'weak peak (margin {min(s[2],u[2]):.3f})'), False
                dropped += 1
            elif abs(s[0]) < MIN_MOTION:
                # A ratio of two small integers is mostly quantisation. With
                # +-1 px on each shift the ratio's own uncertainty is
                # ratio*(1/|s| + 1/|u|): at 7 px that is +-14%, enough to move
                # 0.71 to 1.0 and back. 15 px keeps it near +-10%.
                tag, ok = (f'scene moved only {s[0]:+d} px — ratio would be '
                           f'quantisation'), False
                dropped += 1
            else:
                # The control must reproduce the channel's OWN shift.
                for c, ref, name in ((ca, s, 'scene'), (cu, u, 'ui')):
                    if c is None or abs(ref[0]) < 1e-6:
                        tag, ok = f'{name} control unmeasurable', False
                    elif abs(c[0] / ref[0] - 1.0) > CTRL_TOL:
                        tag, ok = (f'{name} control {c[0]/ref[0]:+.2f} != +1 '
                                   f'— pair VOID'), False
                if not ok:
                    void += 1
            fmt = lambda v: f'{v[0]:+7d}' if v else '     --'
            cr = (lambda c, ref: f'{c[0]/ref[0]:+9.2f}'
                  if c and ref and abs(ref[0]) > 1e-6 else '       --')
            ratio = (u[0] / s[0]) if ok else None
            if ok:
                err = abs(ratio) * (1.0 / abs(s[0]) + 1.0 / max(abs(u[0]), 1))
                ratios.append((ratio, err))
                tag = f'ratio {ratio:+.2f} +- {err:.2f}'
            print(f'{str(prev_n)+"->"+str(n):>9} {fmt(s)} {fmt(u)} '
                  f'{ratio if ratio is None else round(ratio,2)!s:>7}  '
                  f'{cr(ca,s)} {cr(cu,u):>7}  {tag}')
        prev, prev_n = cur, n

    # Distinguish "the data says nothing" from "there is no data". They print
    # the same way otherwise, and the second one is about the command line.
    if decoded == 0:
        print(f'\nNO FRAME DECODED in n{lo}..n{hi} under prefix {PREFIX!r}.\n'
              f'  This is not a measurement — nothing was read. Check '
              f'--prefix and the range:\n'
              f'    ls {PREFIX}-present-n*-layer0.png | wc -l',
              file=sys.stderr)
        return 2
    print(f'\n  {decoded} frame(s) decoded under {PREFIX}, '
          f'{skipped_gap} pair(s) skipped as non-adjacent')
    print(f'  {len(ratios)} pair(s) survived their controls, '
          f'{void} VOIDED by a control, {dropped} unmeasurable or too still')
    if not ratios:
        print('  no answer from this data. The cadence is the limit: '
              'consecutive dumps are ~1.3 s apart here and a head turns far '
              'in 1.3 s.')
        return 1
    r = np.array([x[0] for x in ratios])
    e = np.array([x[1] for x in ratios])
    print(f'  ratio  median {np.median(r):+.2f}  '
          f'(min {r.min():+.2f}  max {r.max():+.2f})')
    for v, u_ in zip(r, e):
        print(f'         {v:+.2f} +- {u_:.2f}   '
              f'{"excludes +1" if abs(v - 1) > 2 * u_ else "consistent with +1"}')
    print('    +1 = HUD moves with the ship (correct in the cockpit)\n'
          '     0 = HUD is screen-locked\n'
          '    +2 = HUD moves twice the ship, so it slides AGAINST the head')
    return 0


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('lo', type=int)
    ap.add_argument('hi', type=int)
    ap.add_argument('--burst-log', help="the take's log, for the layer's "
                                        "`ui dump: burst frames` lines")
    ap.add_argument('--prefix', default=PREFIX,
                    help=f'dump prefix, i.e. X4VR_MV_DUMP (default {PREFIX})')
    a = ap.parse_args()
    PREFIX = a.prefix
    ADJACENT = None
    if a.burst_log:
        ADJACENT = burst_groups(a.burst_log)
        if not ADJACENT:
            print(f'no `ui dump: burst frames` lines in {a.burst_log} — that '
                  f'run had no bursts, so no pair in it is one frame apart',
                  file=sys.stderr)
            sys.exit(2)
        print(f'  {len(ADJACENT)} burst(s) in the log, '
              f'{sum(len(g) for g in ADJACENT)} frame(s) total')
    sys.exit(main(a.lo, a.hi))
