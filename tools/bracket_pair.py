#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""#84: pair each object bracket with ITS ship, by CO-MOVEMENT, not proximity.

    tools/bracket_pair.py 0 600 --burst-log /tmp/x4vr-take267.log --prefix /tmp/x4vr-t267
    tools/bracket_pair.py --selftest

Take 267 left a real but unexplained defect: the object brackets sit about
29 px off their ships at the edge of the field and nothing at the centre. The
first attempt to characterise it paired each bracket with the NEAREST bright
blob, and the middle eccentricity bins came back with sd ~26 px -- as large as
the effect -- because a star or a second ship is often nearer than the right
one. No mechanism could be read out of that.

**Patola identified the pairing by eye and stated the principle:** *"the reason
I say that it belongs to the transport ship is that they moved together. It is
very unlikely this would happen to different objects."* This automates that
rule. A bracket and its ship share a VELOCITY; a bracket and a coincidentally
nearby star do not.

So per adjacent frame pair: track each green bracket cluster, track every
candidate scene blob near it, and pair on matching velocity -- with a MARGIN,
so a pairing is only accepted when the best candidate clearly beats the
runner-up. An ambiguous pairing is dropped and counted, never resolved by
falling back to distance, because distance is the thing that failed.

**The channels are split by COLOUR.** The object brackets are green and the
cockpit chrome is blue, and the chrome outnumbers the brackets by roughly 640
to 1 in pixels -- a correlation over both at once locks onto the chrome and
reports the chrome's answer. That mistake was made on these very frames.
"""
import argparse
import sys

import numpy as np

sys.path.insert(0, '/home/patola/workspace/claude/X4VRMOD/tools')
from ui_disparity import decode_rgba                      # noqa: E402
from hud_vs_camera import decode                          # noqa: E402
from hud_slide import burst_groups                        # noqa: E402

W = 1408
MIN_CLUSTER = 30      # green pixels before a cluster is a bracket
CLUSTER_GAP = 40      # row gap that separates two brackets
SEARCH_R = 90         # px around a bracket to look for its ship
BLOB_FRAC = 0.5       # of the window max, to call a pixel part of a blob
MIN_BLOB, MAX_BLOB = 4, 4000
MAX_OFFSET = 90       # a bracket further than this from any blob is unpaired
# A pairing is accepted only if the best velocity match beats the runner-up by
# this many px of velocity difference. Without it "co-movement" degrades into
# "whichever candidate happened to be closest", which is the method that failed.
VEL_MARGIN = 2.5
VEL_TOL = 6.0         # px/frame; beyond this nothing co-moves with anything
# **A lone candidate gets margin = infinity below, which is an unconditional
# pass -- and that IS a check that cannot fail.** --strict-lone holds it to
# this tolerance instead. OFF by default, and the reason is measured, not
# assumed: applied ALONE against a baseline confirmed to reproduce 48 pairings
# exactly, it dropped 31 of them (65% of the data) and left the centre bin's
# scatter untouched, 46.52 -> 45.11. The prediction on record was that the
# centre would clean up if lone acceptances were its cause. THEY ARE NOT.
# Outer-bin values barely moved (-21.79 -> -20.28, -35.47 -> -33.55), so the
# loophole was not biasing them either -- it costs sample size and buys
# nothing. Kept as a flag because the rigour is real even where the benefit is
# not, and so the negative result is not re-derived.
VEL_TOL_LONE = 1.8
STRICT_LONE = False


def green_clusters(prefix, n, layer=0):
    """(x, y, npx) per green bracket cluster, plus the UI mask and scene."""
    try:
        r, a = decode_rgba(f'{prefix}-ui-n{n}-layer{layer}.png')
        p = decode(f'{prefix}-present-n{n}-layer{layer}.png')
    except Exception:
        return None, None
    ui = (a > 0) | (r.max(2) > 0)
    g = (ui & (r[..., 1] > 0.25) & (r[..., 1] > r[..., 2] * 1.35)
         & (r[..., 1] > r[..., 0] * 1.35))
    ys, xs = np.nonzero(g)
    out = []
    if len(xs):
        o = np.argsort(ys)
        ys, xs = ys[o], xs[o]
        cur, cl = [0], []
        for i in range(1, len(ys)):
            if ys[i] - ys[i - 1] > CLUSTER_GAP:
                cl.append(cur)
                cur = []
            cur.append(i)
        cl.append(cur)
        for c in cl:
            if len(c) >= MIN_CLUSTER:
                out.append((float(xs[c].mean()), float(ys[c].mean()), len(c)))
    lum = 0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2]
    return out, np.where(~ui, lum, 0.0)


def blobs_near(scene, bx, by, r=SEARCH_R):
    """Candidate objects near a bracket: local bright groups, not one maximum.

    Returns [(x, y, weight)]. Several are returned ON PURPOSE -- picking one
    here would re-introduce the proximity assumption this file exists to
    replace.
    """
    y0, y1 = max(0, int(by - r)), min(scene.shape[0], int(by + r))
    x0, x1 = max(0, int(bx - r)), min(scene.shape[1], int(bx + r))
    w = scene[y0:y1, x0:x1]
    if w.size == 0 or w.max() < 0.08:
        return []
    ys, xs = np.nonzero(w >= w.max() * BLOB_FRAC)
    if len(xs) < MIN_BLOB or len(xs) > MAX_BLOB:
        return []
    # Split the bright pixels into groups by proximity so two ships in the
    # window stay two candidates.
    pts = np.stack([xs, ys], 1).astype(float)
    used = np.zeros(len(pts), bool)
    out = []
    for i in range(len(pts)):
        if used[i]:
            continue
        sel = (np.abs(pts[:, 0] - pts[i, 0]) < 25) & \
              (np.abs(pts[:, 1] - pts[i, 1]) < 25) & ~used
        if sel.sum() < MIN_BLOB:
            continue
        used |= sel
        wt = w[ys[sel], xs[sel]]
        out.append((float((xs[sel] * wt).sum() / wt.sum()) + x0,
                    float((ys[sel] * wt).sum() / wt.sum()) + y0,
                    float(wt.sum())))
    return out


def nearest(items, x, y, maxd):
    best, bd = None, maxd
    for it in items:
        d = ((it[0] - x) ** 2 + (it[1] - y) ** 2) ** 0.5
        if d < bd:
            best, bd = it, d
    return best


def pair_frame(ca, sa, cb, sb):
    """Pairings for one adjacent frame pair, by co-movement.

    Yields (ecc, dx, dy, vel_err, margin) per accepted bracket.
    """
    for (bx, by, _n) in ca:
        nxt = nearest(cb, bx, by, 60)      # the same bracket, next frame
        if nxt is None:
            continue
        vb = (nxt[0] - bx, nxt[1] - by)
        cands = blobs_near(sa, bx, by)
        scored = []
        for (sx, sy, _w) in cands:
            m = nearest(blobs_near(sb, sx, sy, 60), sx, sy, 60)
            if m is None:
                continue
            vs = (m[0] - sx, m[1] - sy)
            err = ((vs[0] - vb[0]) ** 2 + (vs[1] - vb[1]) ** 2) ** 0.5
            scored.append((err, sx, sy))
        if not scored:
            continue
        scored.sort()
        err, sx, sy = scored[0]
        margin = (scored[1][0] - err) if len(scored) > 1 else 1e9
        if len(scored) == 1 and STRICT_LONE:
            if err > VEL_TOL_LONE:
                yield None
                continue
        elif err > VEL_TOL or margin < VEL_MARGIN:
            yield None                     # ambiguous: counted, not resolved
            continue
        if abs(bx - sx) > MAX_OFFSET:
            continue
        yield (sx - W / 2.0, bx - sx, by - sy, err, margin)


def selftest():
    """Does velocity beat proximity? That is the whole claim of this file."""
    fails = 0
    # A bracket co-moving with a FAR object, and a NEARER decoy moving the
    # other way. "Nearest blob" gets this wrong by construction; co-movement
    # must get it right, or the tool is no better than what it replaces.
    sa = np.zeros((400, 400))
    sb = np.zeros((400, 400))
    # Both candidates INSIDE SEARCH_R -- the first version put the true ship
    # 102 px away against a 90 px search, so it was never a candidate and the
    # test failed for a reason that had nothing to do with the claim.
    sa[200:206, 255:261] = 1.0            # true ship, ~58 px from the bracket
    sb[210:216, 265:271] = 1.0            # ...moves (+10, +10)
    sa[200:206, 215:221] = 1.0            # decoy, only ~18 px away
    sb[190:196, 205:211] = 1.0            # ...moves (-10, -10)
    ca = [(200.0, 200.0, 50)]
    cb = [(210.0, 210.0, 50)]             # bracket moves (+10, +10) = true ship
    got = [g for g in pair_frame(ca, sa, cb, sb) if g]
    if len(got) != 1:
        print(f'  FAIL velocity-vs-proximity: {len(got)} pairing(s), want 1')
        fails += 1
    else:
        ecc, dx, dy, err, margin = got[0]
        picked_true = abs(dx - (200.0 - 257.5)) < 8
        print(f'  velocity beats proximity : dx {dx:+7.1f} '
              f'(true ship {200.0-257.5:+.1f}, decoy {200.0-217.5:+.1f}) '
              f'err {err:.2f} margin {margin:.2f} -> '
              f'{"correct" if picked_true else "WRONG — picked the decoy"}')
        if not picked_true:
            fails += 1

    # Two candidates co-moving identically: genuinely ambiguous, must be
    # DROPPED rather than resolved by distance.
    sa2 = np.zeros((400, 400)); sb2 = np.zeros((400, 400))
    sa2[200:206, 255:261] = 1.0; sb2[210:216, 265:271] = 1.0
    sa2[200:206, 215:221] = 1.0; sb2[210:216, 225:231] = 1.0
    res = list(pair_frame(ca, sa2, cb, sb2))
    dropped = [r for r in res if r is None]
    print(f'  ambiguous pair is dropped : {len(dropped)} dropped, '
          f'{len([r for r in res if r])} accepted -> '
          f'{"correct" if dropped and not [r for r in res if r] else "WRONG"}')
    if not dropped or [r for r in res if r]:
        fails += 1

    print(f'  {"FAIL" if fails else "ok  "} bracket_pair selftest, '
          f'{fails} failure(s)')
    return 1 if fails else 0


def main(lo, hi, prefix, log, layer=0):
    groups, _ = burst_groups(log)
    if not groups:
        sys.exit(f'NO BURSTS in {log}')
    adj = set()
    for g in groups:
        for i in range(len(g) - 1):
            adj.add((g[i], g[i + 1]))
    rows, ambiguous = [], 0
    prev = prev_n = None
    for n in range(lo, hi + 1):
        cur = green_clusters(prefix, n, layer)
        if cur[0] is None:
            continue
        if prev is not None and (prev_n, n) in adj:
            for r in pair_frame(prev[0], prev[1], cur[0], cur[1]):
                if r is None:
                    ambiguous += 1
                else:
                    rows.append(r)
        prev, prev_n = cur, n
    print(f'  {len(rows)} pairing(s) accepted by CO-MOVEMENT, '
          f'{ambiguous} dropped as ambiguous')
    if len(rows) < 12:
        print('  too few to fit — this says nothing about the offset.')
        return
    a = np.array([r[:3] for r in rows])
    e, dx, dy = a[:, 0], a[:, 1], a[:, 2]
    print(f'\n  signed ecc      n    mean dx     sd     mean dy     sd')
    for lo_, hi_ in ((-800, -400), (-400, -150), (-150, 150),
                     (150, 400), (400, 800)):
        m = (e >= lo_) & (e < hi_)
        if m.sum() > 2:
            print(f'  {lo_:+5d}..{hi_:+5d}  {m.sum():4d}  {dx[m].mean():+7.2f} '
                  f'{dx[m].std():6.2f}   {dy[m].mean():+7.2f} {dy[m].std():6.2f}')
    A = np.stack([np.ones_like(e), e], 1)
    sol, *_ = np.linalg.lstsq(A, dx, rcond=None)
    res = dx - (sol[0] + sol[1] * e)
    print(f'\n  fit  dx = {sol[0]:+.2f} {sol[1]:+.5f}*ecc   residual sd {res.std():.2f}')
    print(f'  constant-only residual sd {dx.std():.2f}')
    print(f'\n  A RADIAL SCALE needs intercept ~0 and slope < 0 (dx = -k*ecc).')
    print(f'  A CONSTANT SHIFT needs slope ~0.')
    print(f'  If the residual sd is still comparable to the effect, the pairing '
          f'is\n  still not clean enough and NO mechanism should be read off '
          f'this table.')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('lo', nargs='?', type=int)
    ap.add_argument('hi', nargs='?', type=int)
    ap.add_argument('--prefix', default='/tmp/x4vr-t267')
    ap.add_argument('--burst-log')
    ap.add_argument('--layer', type=int, default=0,
                    help='which eye. Running BOTH and comparing splits a mono '
                         'layout error from a stereo one, which is the first '
                         'question about a constant offset.')
    ap.add_argument('--strict-lone', action='store_true',
                    help='hold a LONE candidate to a tight velocity match '
                         'instead of passing it unconditionally. Measured: '
                         'costs 65%% of pairings and does NOT clean the centre '
                         'bin (46.52 -> 45.11).')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    STRICT_LONE = a.strict_lone
    globals()['STRICT_LONE'] = a.strict_lone
    if a.selftest:
        sys.exit(selftest())
    if a.lo is None or a.hi is None or not a.burst_log:
        sys.exit('need: lo hi --burst-log LOG [--prefix P]')
    main(a.lo, a.hi, a.prefix, a.burst_log, a.layer)
