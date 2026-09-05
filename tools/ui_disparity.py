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

**--ui: the mask by construction, which is what makes this trustworthy.**
Without it the UI is picked out of the composed frame by colour (`channels()`'s
cyan test), and that channel is CONTAMINATED in cockpit frames — rendering the
mask shows it covering stars and cockpit console panels, both world geometry. No
threshold fixes it: the G/R medians are 2.35 for known-good UI and 3.08 for the
contaminated mask, overlapping, and brightness cannot separate them either
because the menu UI is dim while the cockpit HUD is a bright stroke.

`X4VR_UI_DUMP=N` writes X4's own UI-only target beside the eye image, from the
same composite. Pass --ui and the mask stops being a guess:

    tools/ui_disparity.py '/tmp/x4vr-t257-present-n*-layer0.png' --ui

Three numbers come out of it, and they check each other:

  A  disparity measured on the UI dump ALONE, layer0 against layer1. Assumes
     nothing about the composite -- it is the UI, in isolation, per eye, at one
     instant. This is the number to believe.
  B  disparity measured on the composed frame, restricted to the mask. What the
     player's eyes actually got.
  C  the offset between the UI dump and the composed frame. It must be (0,0):
     that is what says the composite maps the UI 1:1 and therefore that A and B
     describe the same pixels. If it is not zero, B is meaningless and the tool
     says so instead of reporting it.

The old cyan channel is measured too, side by side, so the contamination it
carries is quantified rather than merely asserted.
"""

import argparse
import glob
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hud_vs_camera import decode, channels, cockpit_mask     # noqa: E402

try:
    from PIL import Image
except ImportError:                                          # pragma: no cover
    Image = None


def decode_rgba(path):
    """(linear RGB, raw alpha) for a UI dump.

    Alpha is NOT linearised: it is a coverage fraction, not a colour, and X4
    writes it as one. Running it through the sRGB transfer curve would bend a
    mask into a shape it never had.
    """
    a = np.asarray(Image.open(path).convert('RGBA'), dtype=np.float32) / 255.0
    rgb = a[..., :3]
    lin = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    return lin, a[..., 3]


def ui_sibling(present_path, layer):
    """The UI dump written from the SAME composite as `present_path`.

    Derived from the filename rather than globbed and zipped by position. Two
    sorted lists paired by index is how a mask from one frame gets applied to
    another when a single file is missing, and that is precisely the error this
    whole instrument exists to remove.
    """
    m = re.search(r'^(.*)-present-n(\d+)-layer\d\.png$', present_path)
    if not m:
        return None
    return f'{m.group(1)}-ui-n{m.group(2)}-layer{layer}.png'


def ui_masks(rgb, alpha):
    """Candidate UI masks, and their coverage.

    Two of them, because which one IS the UI is not settled and guessing would
    be the fourth clean audit of the wrong quantity in this investigation.
    `alpha` is X4's own composite coverage; `colour` is what survives if the
    composite ignores alpha and keys on content instead. They are reported
    separately and their disagreement is reported too.
    """
    lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    return {'alpha': alpha > 0.0, 'colour': rgb.max(2) > 0.0}, lum


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


def measure_ui_dump(f, a):
    """The three numbers from one present dump and its UI sibling.

    Returns None when the pair is not there or carries no UI, so an absent
    sibling reads as "this frame was skipped" and never as "the UI did not
    move".
    """
    u0p, u1p = ui_sibling(f, 0), ui_sibling(f, 1)
    if not u0p or not os.path.exists(u0p) or not os.path.exists(u1p):
        return None
    rgb0, al0 = decode_rgba(u0p)
    rgb1, al1 = decode_rgba(u1p)
    m0, lum0 = ui_masks(rgb0, al0)
    m1, lum1 = ui_masks(rgb1, al1)
    key = a.mask
    if m0[key].sum() < a.min_ui_px:
        return None

    out = {'cover': {k: float(v.mean()) for k, v in m0.items()},
           'agree': float((m0['alpha'] == m0['colour']).mean())}

    # A: the UI alone. Masked to itself so the transparent field -- which is
    # identical in both layers and would correlate perfectly at every offset --
    # cannot contribute.
    ua = np.where(m0[key], lum0, 0.0)
    ub = np.where(m1[key], lum1, 0.0)
    out['A'] = ncc_shift(ua, ub, a.range_x, a.range_y)

    # C: does the composite put the UI where the UI dump has it? Compared over
    # the mask, layer 0 against layer 0 -- one eye, so any offset found here is
    # the composite moving the UI, not parallax.
    p0 = decode(f)
    plum0 = 0.2126 * p0[..., 0] + 0.7152 * p0[..., 1] + 0.0722 * p0[..., 2]
    if plum0.shape != lum0.shape:
        out['C'] = None
        out['B'] = None
        out['ua'], out['ub'] = ua, ub
        out['mask0'] = m0[key]
        out['shape'] = (lum0.shape, plum0.shape)
        return out
    out['C'] = ncc_shift(np.where(m0[key], lum0, 0.0),
                         np.where(m0[key], plum0, 0.0), a.range_x, a.range_y)

    # B: the composed frame, restricted to the mask.
    p1 = decode(f.replace('-layer0.png', '-layer1.png'))
    plum1 = 0.2126 * p1[..., 0] + 0.7152 * p1[..., 1] + 0.0722 * p1[..., 2]
    out['B'] = ncc_shift(np.where(m0[key], plum0, 0.0),
                         np.where(m1[key], plum1, 0.0), a.range_x, a.range_y)
    out['mask0'] = m0[key]
    out['ua'], out['ub'] = ua, ub
    return out


def run_ui_mode(fs, a):
    """--ui: mask from X4's own UI target, not from colour."""
    A, B, C, cyan, cover, agree, per_tile = [], [], [], [], [], [], []
    used = skipped = 0
    for f in fs:
        if used >= a.max_frames:
            break
        if not os.path.exists(f.replace('-layer0.png', '-layer1.png')):
            continue
        m = measure_ui_dump(f, a)
        if m is None:
            skipped += 1
            continue
        if m['A'] is None or m['A'][3] < a.min_margin:
            skipped += 1
            continue
        used += 1
        A.append(m['A'])
        cover.append(m['cover'])
        agree.append(m['agree'])
        if m['B'] is not None and m['B'][3] >= a.min_margin:
            B.append(m['B'])
        if m['C'] is not None:
            C.append(m['C'])
        # The old channel on the SAME frame, so the contamination it carries is
        # a measured difference rather than a claim.
        c0 = channels(decode(f))
        c1 = channels(decode(f.replace('-layer0.png', '-layer1.png')))
        if (c0['hud'] > 0).sum() >= 2000:
            rc = ncc_shift(c0['hud'], c1['hud'], a.range_x, a.range_y)
            if rc is not None and rc[3] >= a.min_margin:
                cyan.append(rc)
        # Per tile on the UI dump itself. A flat canvas has the SAME disparity
        # everywhere, so a spread across tiles IS the defect -- and a
        # whole-frame correlation averages exactly that away.
        for (tx, ty, y0, y1, x0, x1) in tiles(m['mask0'], a.tiles, a.min_px):
            rt = ncc_shift(m['ua'][y0:y1, x0:x1], m['ub'][y0:y1, x0:x1],
                           a.range_x, a.range_y)
            if rt is not None and rt[3] >= a.min_margin:
                per_tile.append((tx, ty, rt[0], rt[1]))
        print(f'  {os.path.basename(f):38s} A dx {m["A"][0]:+4d} dy '
              f'{m["A"][1]:+3d} (margin {m["A"][3]:.3f})   '
              f'B {"dx %+4d dy %+3d" % (m["B"][0], m["B"][1]) if m["B"] else "  --":18s}   '
              f'C {"dx %+3d dy %+3d" % (m["C"][0], m["C"][1]) if m["C"] else " --"}')

    if used < 3:
        print(f'\nonly {used} frame(s) had a usable UI dump ({skipped} skipped)'
              f' -- cannot answer. If every frame skipped, check the log for '
              f'"ui dump:" lines: no lines at all means X4VR_UI_DUMP never '
              f'fired, and empty coverage means the UI target was blank.')
        return 1

    ax = np.array([r[0] for r in A], dtype=float)
    ay = np.array([r[1] for r in A], dtype=float)
    print(f'\n  {used} frame(s) measured, {skipped} skipped')
    print(f'  mask coverage: alpha {100*np.mean([c["alpha"] for c in cover]):.2f}%'
          f'  colour {100*np.mean([c["colour"] for c in cover]):.2f}%'
          f'  — the two criteria agree on {100*np.mean(agree):.2f}% of pixels')
    print(f'\n  A  UI dump alone   dx median {np.median(ax):+.1f} px  '
          f'(quartiles {np.percentile(ax,25):+.1f}..{np.percentile(ax,75):+.1f})'
          f'   dy median {np.median(ay):+.1f} px')
    got = abs(np.median(ax))
    print(f'     against {a.expect:.1f} predicted = {got/a.expect:.2f}x')

    # C first: it decides whether B may be read at all.
    ok_c = False
    if C:
        cx = np.array([r[0] for r in C], dtype=float)
        cy = np.array([r[1] for r in C], dtype=float)
        ok_c = abs(np.median(cx)) <= 1.0 and abs(np.median(cy)) <= 1.0
        print(f'\n  C  UI vs composed  dx median {np.median(cx):+.1f} '
              f'dy {np.median(cy):+.1f} px — '
              + ('ZERO, so the composite maps the UI 1:1 and B below is about '
                 'the same pixels as A.'
                 if ok_c else
                 'NOT zero. The composite moves the UI relative to its own '
                 'target, so the mask does not land on the UI in the composed '
                 'frame and B is NOT reported.'))
    if B and ok_c:
        bx = np.array([r[0] for r in B], dtype=float)
        by = np.array([r[1] for r in B], dtype=float)
        print(f'\n  B  composed frame  dx median {np.median(bx):+.1f} px   '
              f'dy median {np.median(by):+.1f} px  over {len(B)} frame(s)')
        if abs(np.median(bx) - np.median(ax)) > 2.0:
            print(f'     DISAGREES with A by '
                  f'{abs(np.median(bx)-np.median(ax)):.1f} px. The UI is not '
                  f'reaching the screen with the disparity its own target has.')
    if cyan:
        zx = np.array([r[0] for r in cyan], dtype=float)
        print(f'\n  the old cyan channel on the same frames: dx median '
              f'{np.median(zx):+.1f} px over {len(cyan)} frame(s), against '
              f'A\'s {np.median(ax):+.1f}. A difference here is the '
              f'contamination, measured.')
    if abs(np.median(ay)) > 1.0:
        print(f'\n  VERTICAL disparity {np.median(ay):+.1f} px — the eyes are '
              f'asked to diverge vertically, which does not fuse. That is #52.')
    if per_tile:
        t = np.array([p[2] for p in per_tile], dtype=float)
        print(f'\n  per-tile dx over {len(t)} tile measurement(s): '
              f'median {np.median(t):+.1f}  p10 {np.percentile(t,10):+.1f}  '
              f'p90 {np.percentile(t,90):+.1f}  spread '
              f'{np.percentile(t,90)-np.percentile(t,10):.1f} px')
        print('  a flat canvas has the SAME disparity everywhere; spread here '
              'means the UI is not being placed as one.')
    return 0


def _synth(a, du, dw, blobs=26):
    """Build one synthetic pair on disk and measure it. Returns (dir, prefix, m).

    Caller removes the directory.
    """
    import tempfile
    w = h = 400
    rng = np.random.default_rng(20260906)

    # World: cyan-ish structure everywhere, dense, and NOT the UI.
    base = (rng.random((h, w)) * 90 + 40).astype(np.float32)
    world0 = np.zeros((h, w, 3), np.float32)
    world0[..., 0] = base * 0.25          # R low
    world0[..., 1] = base                 # G high
    world0[..., 2] = base * 0.98          # B ~ G  -> reads as "cyan"
    world1 = np.roll(world0, dw, axis=1)

    # UI: sparse bright blobs on transparent black, with structure of its own.
    ui0 = np.zeros((h, w, 4), np.float32)
    for i in range(blobs):
        y, x = int(rng.integers(20, h - 40)), int(rng.integers(20, w - 40))
        bh, bw = int(rng.integers(9, 22)), int(rng.integers(9, 22))
        v = float(rng.integers(120, 255))
        ui0[y:y + bh, x:x + bw, :3] = (v * 0.3, v, v * 0.95)
        # Two thirds of the blobs are see-through, so inside the mask the
        # composed frame is 0.43*UI + 0.57*world -- and the world is the thing
        # moving the other way.
        ui0[y:y + bh, x:x + bw, 3] = 255.0 if i % 3 == 0 else 110.0
    ui1 = np.roll(ui0, du, axis=1)

    def composite(world, ui):
        al = (ui[..., 3:4] / 255.0)
        return world * (1.0 - al) + ui[..., :3] * al

    d = tempfile.mkdtemp(prefix='x4vr-uidisp-')
    pre = os.path.join(d, 'synth')
    for L, (wl, ul) in enumerate(((world0, ui0), (world1, ui1))):
        Image.fromarray(composite(wl, ul).clip(0, 255).astype(np.uint8),
                        'RGB').save(f'{pre}-present-n0-layer{L}.png')
        Image.fromarray(ul.clip(0, 255).astype(np.uint8),
                        'RGBA').save(f'{pre}-ui-n0-layer{L}.png')

    class Args:
        pass
    b = Args()
    for k in ('range_x', 'range_y', 'mask', 'min_ui_px', 'min_margin'):
        setattr(b, k, getattr(a, k))
    b.range_x = max(b.range_x, 60)
    return d, pre, b, measure_ui_dump(f'{pre}-present-n0-layer0.png', b)


def _cleanup(d, pre):
    for f in glob.glob(f'{pre}-*'):
        os.remove(f)
    os.rmdir(d)


def selftest(a):
    """Synthetic pairs whose answers are known, built to be as HARD as the real
    measurement rather than merely to have a known answer.

    **The world moves too, and by a different amount.** A control where only the
    UI moves would be satisfied by a tool that simply reports whatever shift
    dominates the frame, and that tool would then be wrong on every real cockpit
    frame. Here the world moves by -35 px and the UI by +22, so the only way to
    return +22 is to have actually separated them.

    **Most of the UI is PARTIALLY transparent**, which is what makes B a real
    test rather than a restatement of A. Where alpha is 1 the composed frame
    inside the mask is simply the UI, and B could not fail; at alpha 0.43 it is
    a blend with a world moving the other way, so recovering +22 there means the
    UI actually dominates through the composite. X4's HUD is antialiased and
    semi-transparent, so this is also the truthful case.

    **A NULL case with the world still moving.** An instrument that reports a
    disparity because it was asked for one passes every positive test ever
    written. So the second configuration imposes zero UI shift while the world
    still slides 35 px, and the answer must be exactly 0.

    **The world is CYAN**, matching what `channels()` keys on, so the old colour
    channel is contaminated here as it is in game. Its result is PRINTED and NOT
    asserted -- see the line where it is printed.
    """
    if Image is None:
        print('PIL is not installed', file=sys.stderr)
        return 2

    fails = 0

    def check(name, ok, detail):
        nonlocal fails
        print(f'{"ok  " if ok else "FAIL"} {name:-<52} {detail}')
        if not ok:
            fails += 1

    # ---- the measurement case: UI +22, world -35 -------------------------
    du, dw = 22, -35
    d, pre, b, m = _synth(a, du, dw)
    if m is None:
        print('FAIL the synthetic pair produced no measurement at all')
        _cleanup(d, pre)
        return 1
    check('mask coverage is the UI, not the frame',
          0.001 < m['cover']['alpha'] < 0.25,
          f'alpha covers {100*m["cover"]["alpha"]:.2f}%')
    check('A recovers the UI shift, not the world shift',
          m['A'] is not None and m['A'][0] == du and m['A'][1] == 0,
          f'A = {m["A"][:2] if m["A"] else None}, imposed ({du}, 0); '
          f'the world moved {dw}')
    check('C is zero -- the composite maps the UI 1:1',
          m['C'] is not None and m['C'][0] == 0 and m['C'][1] == 0,
          f'C = {m["C"][:2] if m["C"] else None}')
    check('B recovers the UI shift THROUGH the composed frame',
          m['B'] is not None and m['B'][0] == du and m['B'][1] == 0,
          f'B = {m["B"][:2] if m["B"] else None}, imposed ({du}, 0)')

    # The old channel on the very same frames. REPORTED, NOT ASSERTED: tuning a
    # control until it condemns the method you have already decided against is
    # not evidence. It is here so the two are visible side by side.
    c0 = channels(decode(f'{pre}-present-n0-layer0.png'))
    c1 = channels(decode(f'{pre}-present-n0-layer1.png'))
    cov = float((c0['hud'] > 0).mean())
    rc = ncc_shift(c0['hud'], c1['hud'], b.range_x, b.range_y)
    print(f'     the old cyan channel here: covers {100*cov:.1f}% of the frame '
          f'(a HUD is well under 1%, so it IS contaminated) and reads dx '
          f'{rc[0] if rc else None} where the UI moved {du} and the world '
          f'{dw}. Reported, not asserted.')
    _cleanup(d, pre)

    # ---- the null: UI still, world moving ---------------------------------
    d, pre, b, mz = _synth(a, 0, dw)
    if mz is None or mz['A'] is None:
        check('NULL: zero UI shift is measurable at all', False,
              'the null pair produced no measurement')
    else:
        check('NULL: UI still while the world slides 35 px reads ZERO',
              mz['A'][0] == 0 and mz['A'][1] == 0,
              f'A = {mz["A"][:2]}, imposed (0, 0); the world moved {dw}')
        check('NULL: the composed frame reads zero too',
              mz['B'] is not None and mz['B'][0] == 0 and mz['B'][1] == 0,
              f'B = {mz["B"][:2] if mz["B"] else None}, imposed (0, 0)')
    _cleanup(d, pre)

    # ---- an empty UI must SKIP, never return a number ----------------------
    d, pre, b, me = _synth(a, du, dw, blobs=0)
    check('an empty UI target is skipped, not measured',
          me is None,
          'returned None' if me is None else f'returned {me.get("A")}')
    _cleanup(d, pre)

    if fails:
        print(f'\n{fails} case(s) FAILED')
        return 1
    print('\nall cases passed')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern', nargs='?', help='glob of -layer0 dumps')
    ap.add_argument('--expect', type=float, default=22.5,
                    help='predicted layer-to-layer disparity in px')
    ap.add_argument('--range-x', type=int, default=60)
    ap.add_argument('--range-y', type=int, default=16)
    ap.add_argument('--tiles', type=int, default=4)
    ap.add_argument('--min-px', type=int, default=400,
                    help='UI pixels a tile needs before it is measured')
    ap.add_argument('--max-frames', type=int, default=40)
    ap.add_argument('--ui', action='store_true',
                    help="mask from X4's own UI target (X4VR_UI_DUMP), not "
                         "from colour")
    ap.add_argument('--mask', choices=('alpha', 'colour'), default='alpha',
                    help='which UI-dump criterion is the mask, with --ui')
    ap.add_argument('--min-ui-px', type=int, default=2000,
                    help='UI pixels a frame needs before it is measured')
    ap.add_argument('--min-margin', type=float, default=0.02)
    ap.add_argument('--selftest', action='store_true',
                    help='synthetic pair with a known answer; takes no dumps')
    a = ap.parse_args()

    if a.selftest:
        return selftest(a)

    fs = sorted(glob.glob(a.pattern),
                key=lambda p: int(re.search(r'-n(\d+)-', p).group(1)))
    if not fs:
        print('no dumps matched', file=sys.stderr)
        return 2

    if a.ui:
        return run_ui_mode(fs, a)

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
