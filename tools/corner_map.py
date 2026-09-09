#!/usr/bin/env python3
"""Does the UI's disparity vary from its CENTRE to its CORNERS?

The question take 271 could not answer, because `ui_disparity.py` tiled the
2816x2816 eye image while #81 shrinks the UI to a ~1484 px box in the middle of
it -- so the UI's own corners lived inside the four central cells.

Two things make this affordable where the fixed tool is not:

  * it tiles the UI's own bounding box (that is the fix, and it is what makes
    every cell expensive -- all 36 carry UI, where the image grid measured ~3);
  * it DECIMATES by 2 with a block mean before correlating. Area/4 and a
    halved search range is ~40x less work, at +-2 px of full-res resolution.

**+-2 px is justified, not convenient.** A gradient that fuses at the centre and
fails at the corner has to be large: at the UI corner the scale is ~27 px/deg,
and peripheral diplopia sets in around 1-2 deg, so the effect must be tens of
px. Anything +-2 px can miss cannot be what Patola is seeing.

`--full` re-measures the same frames without decimation, which is the control
that says the decimation did not invent or hide the answer.
"""
import sys, os, glob, re, argparse, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', '..',
                                '..', '..', 'home', 'patola', 'workspace',
                                'claude', 'X4VRMOD', 'tools'))
sys.path.insert(0, '/home/patola/workspace/claude/X4VRMOD/tools')
import numpy as np
import ui_disparity as U


def dec2(a):
    """2x2 block mean. Averaging, not subsampling: the UI is thin line art and
    dropping every other pixel would decimate the features being correlated."""
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    return a[:h, :w].reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern')
    ap.add_argument('--frames', required=True,
                    help='comma-separated dump indices, e.g. 11,21,81')
    ap.add_argument('--tiles', type=int, default=6)
    ap.add_argument('--range-x', type=int, default=160)
    ap.add_argument('--range-y', type=int, default=40)
    ap.add_argument('--min-px', type=int, default=400)
    ap.add_argument('--band', type=int, default=40,
                    help='per-tile search half-width around the GLOBAL '
                         'disparity, in full-res px. 40 covers ~1.5 deg of '
                         'gradient at the UI corner, which is well past where '
                         'peripheral fusion fails; cells that saturate it are '
                         'counted and reported')
    ap.add_argument('--full', action='store_true',
                    help='no decimation -- the control for the decimation')
    a = ap.parse_args()

    want = {int(v) for v in a.frames.split(',')}
    fs = sorted(glob.glob(a.pattern),
                key=lambda p: int(re.search(r'-n(\d+)-', p).group(1)))
    fs = [f for f in fs if int(re.search(r'-n(\d+)-', f).group(1)) in want]
    if not fs:
        print('no dumps matched those indices', file=sys.stderr)
        return 2

    d = 1 if a.full else 2
    rx, ry = a.range_x // d, a.range_y // d
    minpx = a.min_px // (d * d)
    print(f'  decimation {d}x, search +-{rx} x +-{ry} at that scale '
          f'(= +-{rx*d} x +-{ry*d} full-res), min {minpx} px/cell')

    for f in fs:
        n = int(re.search(r'-n(\d+)-', f).group(1))
        g = f.replace('-layer0.png', '-layer1.png')
        if not os.path.exists(g):
            print(f'  n{n}: no layer1'); continue
        t0 = time.time()
        rgb0, al0 = U.decode_rgba(f)
        rgb1, al1 = U.decode_rgba(g)
        m0, lum0 = U.ui_masks(rgb0, al0)
        m1, lum1 = U.ui_masks(rgb1, al1)
        k0, k1 = m0['alpha'], m1['alpha']
        ua = np.where(k0, lum0, 0.0)
        ub = np.where(k1, lum1, 0.0)
        if d == 2:
            ua, ub, k0 = dec2(ua), dec2(ub), dec2(k0.astype(float)) > 0.25

        whole = U.ncc_shift(ua, ub, rx, ry)
        cov = 100.0 * k0.mean()
        print(f'\n=== n{n}  UI coverage {cov:.2f}%  whole-UI dx '
              f'{whole[0]*d:+d} dy {whole[1]*d:+d} px (margin {whole[3]:.3f}) '
              f'[{time.time()-t0:.1f}s]')

        # **Per tile, search a BAND around the global disparity, not +-160.**
        # The first version searched the full range inside each tile and every
        # menu frame came back empty: at 2x decimation a 6x6 grid over a 742 px
        # box gives 124 px cells while +-80 needs 164 px of margin, so
        # ncc_shift's core was empty and the frame printed nothing at all --
        # which reads as "no gradient" and is really "the ruler did not fit".
        #
        # The global dx is already measured. Per tile the question is only how
        # far each cell DEVIATES from it, so the band is the right instrument
        # and it is also what makes the cells affordable.
        dx0, dy0 = whole[0], whole[1]
        bx, by = a.band // d, max(2, a.band // (2 * d))
        cells = list(U.tiles(k0, a.tiles, minpx, box=True))
        if cells:
            ch = min(y1 - y0 for _, _, y0, y1, _, _ in cells)
            cw = min(x1 - x0 for _, _, _, _, x0, x1 in cells)
            need_x, need_y = 2 * (bx + 2) + 4, 2 * (by + 2) + 4
            if cw <= need_x or ch <= need_y:
                print(f'  REFUSED: smallest cell is {cw}x{ch} at this '
                      f'decimation but the +-{bx}x+-{by} search needs more '
                      f'than {need_x}x{need_y}. Fewer --tiles or a smaller '
                      f'--band. No per-tile number from this frame.')
                continue
        per_tile, saturated = [], 0
        H, W = ua.shape
        for (tx, ty, y0, y1, x0, x1) in cells:
            sy0, sy1 = y0 + dy0, y1 + dy0
            sx0, sx1 = x0 + dx0, x1 + dx0
            if sy0 < 0 or sx0 < 0 or sy1 > H or sx1 > W:
                continue            # the shifted window leaves the image
            rt = U.ncc_shift(ua[y0:y1, x0:x1], ub[sy0:sy1, sx0:sx1], bx, by)
            if rt is not None and rt[3] >= 0.02:
                if abs(rt[0]) >= bx or abs(rt[1]) >= by:
                    saturated += 1  # answer may lie outside the band
                per_tile.append((tx, ty, (rt[0] + dx0) * d, (rt[1] + dy0) * d))
        U.report_per_tile(per_tile, a.tiles)
        if saturated:
            print(f'  {saturated} cell(s) hit the edge of the +-{a.band} px '
                  f'band — their true value may be OUTSIDE it, so treat the '
                  f'gradient above as a LOWER bound.')
        # dy matters as much as dx: a vertical component at the corners is the
        # half of #52 that cannot fuse at all, and pooling dx alone hides it.
        if per_tile:
            dys = np.array([p[3] for p in per_tile], dtype=float)
            print(f'  per-tile dy: median {np.median(dys):+.1f}  '
                  f'min {dys.min():+.0f}  max {dys.max():+.0f} px')
        print(f'  [{time.time()-t0:.1f}s total]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
