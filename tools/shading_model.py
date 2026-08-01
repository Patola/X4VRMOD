#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Decide *what kind* of shading term differs between the two eyes.

Every brightness number this project has reported on `#57` and `#52` -- the
1.686, the 1.530, the tile map in stereo_residual.py -- was read straight off
the dumped PPM. But the dumper is not a window onto the render:

    v = v / (1 + v);                       // Reinhard
    byte = powf(v, 1.0f / 2.2f) * 255.0f;  // gamma

so the file holds a *compressed* image. That matters far more than a scale
factor would. Reinhard is not affine: a constant multiplicative term in the
render produces a ratio in the file that varies with brightness, and a constant
offset produces one that varies the other way. Asking "is the ratio constant?"
of the encoded bytes measures the tone curve, not the defect.

Both steps invert exactly wherever nothing clipped:

    t = (byte / 255) ** 2.2        # == v/(1+v)
    linear = t / (1 - t)

Reinhard saturates as v grows, so the top codes carry almost no information --
at byte 250 one code step is 15% of the reconstructed value, and 255 means
"infinity". The bottom clamps too, for the opposite reason: byte 1 to 2 is a
factor of 4.5. So the reconstruction is trusted only over a middle band, and
pixels outside it are dropped rather than fitted.

With linear values in hand the question becomes a two-parameter fit per tile,

    L1 = a * L0 + b

and the three candidate mechanisms separate cleanly:

  - a != 1, b == 0   a **multiplicative** term is missing from one eye:
                     occlusion, ambient occlusion, a shadow factor, a
                     visibility term. Scales what is already there.
  - a == 1, b != 0   an **additive** term is missing: a light, a reflection,
                     an emissive contribution. Adds regardless of what is
                     underneath.
  - a and b both     the fit is not identifiable on this tile, or the surface
                     is not uniform. Reported, never averaged in.

Two guards, both of which this project has already paid for by skipping them:

  - a tile is fitted only if it **aligns** above --ncc (default 0.90). An
    earlier attempt at this measurement compared patches correlating at 0.38,
    i.e. different surfaces, and produced numbers that meant nothing.
  - a tile is fitted only if its own brightness **spans** a factor of --span
    (default 3). Gain and offset are indistinguishable on a flat patch: any
    (a, b) with a*L + b = L1 fits a single value of L. Only a tile holding both
    dark and bright pixels can tell the two apart.

Per-channel gains are reported alongside, because they split the multiplicative
case in two: an achromatic factor (equal across R, G, B) is geometry --
occlusion or shadow. A coloured one is a light source.

Usage:

    tools/shading_model.py <layer0.ppm> <layer1.ppm> [--tile 32] [--ncc 0.9]
                           [--span 3] [--region Y0,Y1,X0,X1] [--top 20]

numpy only.
"""
import argparse
import re
import sys

import numpy as np

GAMMA = 2.2


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    if not m:
        sys.exit('%s: not a binary P6 PPM' % path)
    w, h, maxval = (int(m.group(i)) for i in (1, 2, 3))
    if maxval != 255:
        sys.exit('%s: maxval %d, expected 255' % (path, maxval))
    px = np.frombuffer(data, dtype=np.uint8, offset=m.end())
    return px[:w * h * 3].reshape(h, w, 3)


def to_linear(img, lo, hi):
    """Invert Reinhard-then-gamma. Returns (linear, valid-mask)."""
    e = img.astype(np.float64)
    t = (e / 255.0) ** GAMMA
    # t -> 1 is v -> infinity; clamp so the division cannot produce inf, then
    # let the mask throw those pixels away anyway.
    t = np.minimum(t, 1.0 - 1e-9)
    lin = t / (1.0 - t)
    ok = (img >= lo).all(axis=2) & (img <= hi).all(axis=2)
    return lin, ok


def block_sum(a, t):
    h, w = a.shape[:2]
    return a[:h // t * t, :w // t * t].reshape(h // t, t, w // t, t).sum((1, 3))


def align(g0, g1, tile, dxmax, dymax):
    """Per-tile NCC over integer shifts. Returns (best_ncc, best_dx, best_dy)."""
    n = float(tile * tile)
    ny, nx = g0.shape[0] // tile, g0.shape[1] // tile
    best = np.full((ny, nx), -2.0)
    bdx = np.zeros((ny, nx), np.int32)
    bdy = np.zeros((ny, nx), np.int32)

    Sa = block_sum(g0, tile)
    Saa = block_sum(g0 * g0, tile)
    va = Saa - Sa * Sa / n

    for dy in range(-dymax, dymax + 1):
        for dx in range(-dxmax, dxmax + 1):
            # layer 1 shifted by (dy, dx) and compared where layer 0 sits.
            s = np.roll(np.roll(g1, -dy, axis=0), -dx, axis=1)
            Sb = block_sum(s, tile)
            Sbb = block_sum(s * s, tile)
            Sab = block_sum(g0 * s, tile)
            vb = Sbb - Sb * Sb / n
            cov = Sab - Sa * Sb / n
            den = np.sqrt(np.maximum(va, 1e-12) * np.maximum(vb, 1e-12))
            ncc = cov / den
            hit = ncc > best
            best = np.where(hit, ncc, best)
            bdx = np.where(hit, dx, bdx)
            bdy = np.where(hit, dy, bdy)

    # Tiles whose match window ran off the frame are not trustworthy.
    my, mx = (dymax + tile - 1) // tile + 1, (dxmax + tile - 1) // tile + 1
    edge = np.zeros_like(best, bool)
    edge[:my, :] = edge[-my:, :] = edge[:, :mx] = edge[:, -mx:] = True
    best = np.where(edge, -2.0, best)
    return best, bdx, bdy


def fit_tile(l0, l1):
    """Least squares L1 = a*L0 + b, plus the two restricted models."""
    x = l0.ravel()
    y = l1.ravel()
    n = x.size
    sx, sy = x.sum(), y.sum()
    sxx, sxy = (x * x).sum(), (x * y).sum()
    det = n * sxx - sx * sx
    if det <= 0:
        return None
    a = (n * sxy - sx * sy) / det
    b = (sy - a * sx) / n

    var = ((y - y.mean()) ** 2).sum()
    if var <= 0:
        return None
    r_full = ((y - (a * x + b)) ** 2).sum()
    # Restricted: pure gain (b fixed at 0) and pure offset (a fixed at 1).
    a_gain = sxy / sxx if sxx > 0 else np.nan
    r_gain = ((y - a_gain * x) ** 2).sum()
    b_off = (sy - sx) / n
    r_off = ((y - (x + b_off)) ** 2).sum()
    return dict(a=a, b=b, r2=1.0 - r_full / var,
                gain=a_gain, rms_gain=np.sqrt(r_gain / n),
                off=b_off, rms_off=np.sqrt(r_off / n),
                rms_full=np.sqrt(r_full / n), sd=np.sqrt(var / n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('layer0')
    ap.add_argument('layer1')
    ap.add_argument('--tile', type=int, default=32)
    ap.add_argument('--ncc', type=float, default=0.90)
    ap.add_argument('--span', type=float, default=3.0)
    ap.add_argument('--lo', type=int, default=10)
    ap.add_argument('--hi', type=int, default=235)
    ap.add_argument('--dxmax', type=int, default=80)
    ap.add_argument('--dymax', type=int, default=1)
    ap.add_argument('--region', default=None, help='Y0,Y1,X0,X1')
    # --focus filters which tiles are *reported*, leaving the frame whole for
    # alignment. Cropping first would delete the surroundings a flat face needs
    # to find its own disparity, and the edge guard alone eats 80px a side.
    ap.add_argument('--focus', default=None, help='Y0,Y1,X0,X1')
    ap.add_argument('--top', type=int, default=20)
    ap.add_argument('--why', action='store_true',
                    help='name the guard that rejected each tile')
    a = ap.parse_args()

    i0, i1 = read_ppm(a.layer0), read_ppm(a.layer1)
    if i0.shape != i1.shape:
        sys.exit('shape mismatch %s vs %s' % (i0.shape, i1.shape))
    if a.region:
        y0, y1, x0, x1 = (int(v) for v in a.region.split(','))
        i0, i1 = i0[y0:y1, x0:x1], i1[y0:y1, x0:x1]
    h, w = i0.shape[:2]
    print('%dx%d, tile %d, ncc>=%.2f, span>=%.1fx, codes %d..%d'
          % (w, h, a.tile, a.ncc, a.span, a.lo, a.hi))

    lin0, ok0 = to_linear(i0, a.lo, a.hi)
    lin1, ok1 = to_linear(i1, a.lo, a.hi)

    # Align on the encoded luminance: NCC is affine-invariant, and the encoded
    # image is far better conditioned than the reconstruction.
    g0 = i0.astype(np.float64).mean(axis=2)
    g1 = i1.astype(np.float64).mean(axis=2)
    ncc, bdx, bdy = align(g0, g1, a.tile, a.dxmax, a.dymax)
    ny, nx = ncc.shape

    fy0, fy1, fx0, fx1 = 0, h, 0, w
    if a.focus:
        fy0, fy1, fx0, fx1 = (int(v) for v in a.focus.split(','))

    rows = []
    reject = {}
    for ty in range(ny):
        for tx in range(nx):
            if not (fy0 <= ty * a.tile < fy1 and fx0 <= tx * a.tile < fx1):
                continue
            ys, xs = ty * a.tile, tx * a.tile

            def drop(why, **kw):
                reject.setdefault(why, 0)
                reject[why] += 1
                if a.why:
                    print('  reject %-9s y=%4d x=%4d ncc=%+.3f %s'
                          % (why, ys, xs, ncc[ty, tx],
                             ' '.join('%s=%s' % kv for kv in kw.items())))

            if ncc[ty, tx] < a.ncc:
                drop('ncc')
                continue
            dy, dx = int(bdy[ty, tx]), int(bdx[ty, tx])
            sl = (slice(ys, ys + a.tile), slice(xs, xs + a.tile))
            sl1 = (slice(ys + dy, ys + dy + a.tile),
                   slice(xs + dx, xs + dx + a.tile))
            m = ok0[sl] & ok1[sl1]
            if m.sum() < 0.6 * a.tile * a.tile:
                drop('clipped', frac='%.2f' % (m.mean()))
                continue
            L0 = lin0[sl][m]
            L1 = lin1[sl1][m]
            lum0 = L0.mean(axis=1)
            lum1 = L1.mean(axis=1)
            span = np.percentile(lum0, 95) / max(np.percentile(lum0, 5), 1e-9)
            if not np.isfinite(span) or span < a.span:
                drop('span', span='%.2f' % span)
                continue
            f = fit_tile(lum0, lum1)
            if f is None or f['r2'] < 0.5:
                drop('r2', r2='%.3f' % (f['r2'] if f else float('nan')))
                continue
            ch = [(L0[:, c] * L1[:, c]).sum() / max((L0[:, c] ** 2).sum(), 1e-12)
                  for c in range(3)]
            f.update(y=ys, x=xs, dx=dx, dy=dy, ncc=ncc[ty, tx], span=span,
                     n=int(m.sum()), rgb=ch,
                     mean0=float(lum0.mean()), mean1=float(lum1.mean()))
            rows.append(f)

    if reject:
        print('rejected: ' + ', '.join('%s=%d' % kv
                                       for kv in sorted(reject.items())))
    if not rows:
        print('no tile satisfied the guards -- see the counts above; '
              'a bare zero here would say nothing about the defect')
        return

    print('%d tiles fitted (of %d aligning >= %.2f)\n'
          % (len(rows), int((ncc >= a.ncc).sum()), a.ncc))

    # Which restricted model explains the tile better, per tile.
    for r in rows:
        r['verdict'] = ('gain' if r['rms_gain'] < r['rms_off'] else 'offset')
        r['ratio'] = r['mean1'] / max(r['mean0'], 1e-12)

    ng = sum(1 for r in rows if r['verdict'] == 'gain')
    print('pure-gain fits better on %d/%d tiles, pure-offset on %d'
          % (ng, len(rows), len(rows) - ng))

    med = lambda k: float(np.median([r[k] for r in rows]))
    print('median gain a=%.3f   median offset b=%+.5f   median L1/L0=%.3f'
          % (med('gain'), med('off'), med('ratio')))
    rg = np.median([[r['rgb'][c] for c in range(3)] for r in rows], axis=0)
    print('median per-channel gain  R=%.3f G=%.3f B=%.3f  (spread %.1f%%)'
          % (rg[0], rg[1], rg[2], 100 * (rg.max() - rg.min()) / rg.mean()))

    rows.sort(key=lambda r: -abs(np.log(max(r['ratio'], 1e-9))))
    print('\n  y    x   dx  ncc   span   L0      L1     ratio  '
          'a      b        gain   rmsG   rmsO   verdict')
    for r in rows[:a.top]:
        print('%4d %4d %4d %.3f %5.1f %7.4f %7.4f %6.3f %6.3f %+8.5f '
              '%6.3f %6.4f %6.4f  %s'
              % (r['y'], r['x'], r['dx'], r['ncc'], r['span'], r['mean0'],
                 r['mean1'], r['ratio'], r['a'], r['b'], r['gain'],
                 r['rms_gain'], r['rms_off'], r['verdict']))


if __name__ == '__main__':
    main()
