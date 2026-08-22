#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Did X4 actually apply #33's head rotation? Measured from two screenshots.

    tools/headlook_verify.py --self-check
    tools/headlook_verify.py --verify <off.jpg> <on.jpg> --sx 0.70024 --yaw 10

**`register_affine.py` cannot score this and says so itself** — "they would NOT
be sufficient for a rotation or a keystone, and this tool would report a
confident wrong answer for either". The off-axis map of #35 is separable; a
rotation is not. So this is its sibling for the shape #33 has.

**The shape.** In NDC the rotation is a HOMOGRAPHY, and — this is the useful
part — it is **depth-independent**. Writing `n` for NDC and setting `w = 1`,
the conjugation gives

    D    = cos(yaw) − (sin(yaw)/sx)·n_x          for pure yaw
    n_x' = (cos(yaw)·n_x + sx·sin(yaw)) / D
    n_y' = n_y / D

so every pixel moves by a rule that depends on where it is on screen and on
`sx`, and on nothing else. That is what makes a two-screenshot check sufficient:
no depth map, no scene knowledge, and the SAME warp must fit near geometry and
far geometry alike. A fit that works on the cockpit and fails on the background
is evidence *against* the rotation, not a limitation of the method.

One number worth carrying separately, because it can be checked by eye: screen
centre (`n_x = 0`) moves to `n_x' = sx·tan(yaw)`. At `sx = 0.70024` and 10° that
is 0.1235 NDC — **87 px on a 1408-wide eye**.

**What must NOT move.** Only World modules are rotated. The HUD is excluded on
purpose (#30's canvas), and so is X4's procedural fullscreen backdrop. So the
correct picture is cockpit and world geometry rotating against a stationary
backdrop and a stationary HUD — `--band` restricts the fit to a horizontal
strip so the cockpit can be measured without the HUD dragging the numbers.
"""
import argparse
import math
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter, map_coordinates


def gray(path):
    return np.asarray(Image.open(path).convert('L'), dtype=np.float32)


def half(img, which):
    w = img.shape[1] // 2
    return img[:, :w] if which == 0 else img[:, w:2 * w]


def rot3(yaw, pitch):
    """Ry(yaw) . Rx(pitch), the same composition x4vr::pose_set_euler builds."""
    cy, sy_ = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    return np.array([[cy, 0, sy_], [0, 1, 0], [-sy_, 0, cy]]) @ \
        np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])


def ndc_map(nx, ny, sx, sy, r):
    """Where NDC (nx, ny) lands after the conjugation. Depth drops out.

    Takes the 3x3 ROTATION, not angles. It took angles first, and `warp` built
    its inverse by negating them -- which is right for one axis and WRONG for a
    composition, because Ry(-y).Rx(-p) is not the inverse of Ry(y).Rx(p). Pure
    yaw round-tripped at 0.999 and yaw+pitch at 0.771, and the only reason that
    was not read as resampling loss is that the self-check had a threshold
    between the two. The inverse is the TRANSPOSE, and now it cannot be
    expressed any other way.
    """
    vx, vy, vz = nx / sx, ny / sy, np.ones_like(nx)
    ax = r[0, 0] * vx + r[0, 1] * vy + r[0, 2] * vz
    ay = r[1, 0] * vx + r[1, 1] * vy + r[1, 2] * vz
    az = r[2, 0] * vx + r[2, 1] * vy + r[2, 2] * vz
    az = np.where(np.abs(az) < 1e-6, np.nan, az)
    return (sx * ax) / az, (sy * ay) / az


def warp(img, sx, sy, yaw, pitch):
    """Resample `img` so it should match the rotated frame.

    Runs the map BACKWARDS: for each destination pixel, find where it came
    from. Doing it forwards would scatter rather than gather and leave holes
    the correlation would then have to be excused for.
    """
    h, w = img.shape
    col, row = np.meshgrid(np.arange(w), np.arange(h))
    nx = 2.0 * (col + 0.5) / w - 1.0
    ny = 2.0 * (row + 0.5) / h - 1.0
    # Gather with the INVERSE, which for a rotation is its transpose.
    sxn, syn = ndc_map(nx, ny, sx, sy, rot3(yaw, pitch).T)
    c = (sxn + 1.0) * w / 2.0 - 0.5
    r = (syn + 1.0) * h / 2.0 - 0.5
    bad = ~np.isfinite(c) | ~np.isfinite(r)
    c = np.nan_to_num(c, nan=-1.0)
    r = np.nan_to_num(r, nan=-1.0)
    out = map_coordinates(img, [r, c], order=1, mode='constant', cval=np.nan)
    out[bad] = np.nan
    return out


def edges(img):
    """Gradient energy. Same reasoning as register_affine.py: X4's frames are
    dominated by a smooth bright backdrop, and intensity correlation would
    measure that instead of the cockpit under test."""
    g = gaussian_filter(img, 1.2)
    gy, gx = np.gradient(g)
    return np.hypot(gx, gy)


def ncc(a, b):
    m = np.isfinite(a) & np.isfinite(b)
    if m.sum() < 500:
        return float('nan'), 0
    x, y = a[m], b[m]
    x = x - x.mean()
    y = y - y.mean()
    d = math.sqrt(float((x * x).sum()) * float((y * y).sum()))
    return (float((x * y).sum() / d) if d > 0 else float('nan')), int(m.sum())


def reconcile(a, b):
    """Crop two near-equal frames to a common extent, centred.

    Screen captures of the same 2816x1408 composite came back 2816x1412 and
    2818x1411 -- a few pixels of capture border, differing between grabs. The
    tool used to refuse outright, which is safe but useless.

    Cropping is NOT free here, and the reason is the one register_affine.py
    already learned: `ndc_map` is referenced to the frame's CENTRE, so removing
    k pixels from one edge moves the origin the whole measurement is expressed
    in by k/2. Centring the crop keeps that bias to half the size DIFFERENCE
    rather than half the border, and `--scan` exists so the answer can be shown
    to be insensitive to what is left. Never silently absorb this: the caller
    prints the bias next to the result.
    """
    h = min(a.shape[0], b.shape[0])
    w = min(a.shape[1], b.shape[1])

    def crop(x):
        dy, dx = (x.shape[0] - h) // 2, (x.shape[1] - w) // 2
        return x[dy:dy + h, dx:dx + w]

    return crop(a), crop(b)


def fit_at(ref, meas, sx, sy, yaw, pitch, band):
    """(correlation before, correlation after the predicted warp, px used)."""
    h = ref.shape[0]
    lo, hi = int(band[0] * h), int(band[1] * h)
    b = edges(meas)[lo:hi]
    raw, _ = ncc(edges(ref)[lo:hi], b)
    fit, n = ncc(edges(warp(ref, sx, sy, yaw, pitch))[lo:hi], b)
    return raw, fit, n


def report(ref, meas, sx, sy, yaw, pitch, band, label):
    h = ref.shape[0]
    lo, hi = int(band[0] * h), int(band[1] * h)
    raw, fit, n = fit_at(ref, meas, sx, sy, yaw, pitch, band)
    centre = sx * math.tan(math.radians(yaw))
    print(f'  {label:<10} rows {lo}-{hi}   correlation "off" vs "on": '
          f'{raw:.3f}')
    print(f'  {"":<10} after the predicted warp:          {fit:.3f}'
          f'   ({n} px)')
    print(f'  {"":<10} lift {fit - raw:+.3f}   '
          f'predicted centre shift {centre:+.4f} NDC = '
          f'{centre * ref.shape[1] / 2:+.0f} px')
    return raw, fit


def self_check():
    """Synthesise a frame, warp it by the prediction, and see the tool recover
    it. The negative control is the point: the SAME comparison at the wrong
    angle must not also lift, or a lift means nothing."""
    rng = np.random.default_rng(3)
    img = gaussian_filter(rng.uniform(0, 255, (600, 600)).astype(np.float32), 3)
    sx, sy = 0.70024, -0.70024
    ok = True
    # `warp(img, t)` is "img as it looks after rotating by t". So warping by t
    # and then by -t must return the original, and warping back by the WRONG
    # angle must not. The first version of this check compared warp(-t) against
    # warp(+t) -- two frames 2t apart -- and reported no fit for a predictor
    # that was exact to 2e-16. A self-check that fails on correct code is worse
    # than none: it would have sent the next person after the transform.
    for yaw, pitch in ((10.0, 0.0), (-7.5, 0.0), (6.0, 4.0)):
        rotated = warp(img, sx, sy, yaw, pitch)
        raw, _ = ncc(edges(img), edges(rotated))
        back = warp(rotated, sx, sy, -yaw, -pitch)
        fit, _ = ncc(edges(img), edges(back))
        wrong_img = warp(rotated, sx, sy, -yaw - 5.0, -pitch)
        wrong, _ = ncc(edges(img), edges(wrong_img))
        # RELATIVE, not an absolute floor. The NDC round trip above is exact
        # to 4e-16, so what is left here is resampling: two bilinear passes,
        # and with pitch a band of content leaves the frame and cannot come
        # back. Pure yaw returns 0.999 and yaw+pitch 0.77, and lowering a
        # threshold until both pass would be fitting the test to the result.
        # What the check is actually for is that the right angle separates
        # decisively from a wrong one, and 0.77 against -0.01 does.
        good = fit > 0.7 and fit > raw + 0.3 and fit > wrong + 0.3
        ok &= good
        print(f'  yaw {yaw:+5.1f} pitch {pitch:+4.1f}:  rotated vs original '
              f'{raw:.3f}  round trip {fit:.3f}  wrong angle back {wrong:.3f}'
              f'   {"ok" if good else "FAIL"}')
    # The round trip as PURE MATH, before any resampling can be blamed for it.
    gx0 = np.linspace(-0.8, 0.8, 17)
    gy0 = np.linspace(-0.8, 0.8, 17)
    mx, my = np.meshgrid(gx0, gy0)
    for yaw, pitch in ((10.0, 0.0), (6.0, 4.0), (-9.0, -5.0)):
        r = rot3(yaw, pitch)
        ax, ay = ndc_map(mx, my, sx, sy, r)
        bx, by = ndc_map(ax, ay, sx, sy, r.T)
        e = float(max(np.nanmax(np.abs(bx - mx)), np.nanmax(np.abs(by - my))))
        print(f'  round trip in NDC, yaw {yaw:+5.1f} pitch {pitch:+4.1f}: '
              f'worst {e:.2e}')
        ok &= e < 1e-9

    # And the map itself, against the closed form for pure yaw.
    nx = np.linspace(-0.9, 0.9, 21)
    gx, _ = ndc_map(nx, np.zeros_like(nx), sx, sy, rot3(12.0, 0.0))
    c, s = math.cos(math.radians(12.0)), math.sin(math.radians(12.0))
    closed = (c * nx + sx * s) / (c - (s / sx) * nx)
    err = float(np.abs(gx - closed).max())
    print(f'  the map vs the closed form for pure yaw: worst {err:.2e}')
    ok &= err < 1e-9
    print('SELF-CHECK ' + ('PASS' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--self-check', action='store_true')
    ap.add_argument('--verify', nargs=2, metavar=('OFF', 'ON'))
    ap.add_argument('--sx', type=float, default=0.70024,
                    help="the SCENE camera's sx, from the run's proj lines")
    ap.add_argument('--sy', type=float, default=None, help='defaults to -sx')
    ap.add_argument('--yaw', type=float, default=10.0)
    ap.add_argument('--pitch', type=float, default=0.0)
    ap.add_argument('--scan', default=None, metavar='LO,HI,STEP',
                    help='sweep yaw and print the whole fit curve, e.g. '
                         '"-20,20,2". This is the diagnostic the docs call '
                         'for when a fit at the expected angle is absent — it '
                         'answers "what DID X4 apply", not "did it apply what '
                         'I asked"')
    ap.add_argument('--band', default='0.15,0.55',
                    help='row fraction to fit over; the default avoids the '
                         'HUD, which is EXCLUDED from the rotation on purpose')
    a = ap.parse_args()
    if a.self_check:
        sys.exit(self_check())
    if not a.verify:
        ap.error('give --verify <off> <on>, or --self-check')
    sy = a.sy if a.sy is not None else -a.sx
    band = tuple(float(v) for v in a.band.split(','))
    off, on = gray(a.verify[0]), gray(a.verify[1])
    if off.shape != on.shape:
        s0, s1 = off.shape, on.shape
        off, on = reconcile(off, on)
        bias_y, bias_x = abs(s0[0] - s1[0]) / 2.0, abs(s0[1] - s1[1]) / 2.0
        print(f'NOTE: the captures differ in size — {s1[1]}x{s1[0]} against '
              f'{s0[1]}x{s0[0]}. Both centre-cropped to '
              f'{off.shape[1]}x{off.shape[0]}.')
        print(f'      Worst-case origin bias from that crop: {bias_x:.1f} px '
              f'in x, {bias_y:.1f} px in y. Compare against the predicted '
              f'shift below; if it is a few percent of it, the crop is not '
              f'what decides the answer — and --scan settles it outright.\n')
    print(f'{a.verify[0]} -> {a.verify[1]}   {off.shape[1]}x{off.shape[0]}')
    print(f'sx={a.sx} sy={sy} yaw={a.yaw} pitch={a.pitch} band={band}\n')
    if a.scan:
        lo, hi, step = (float(v) for v in a.scan.split(','))
        print('Scanning yaw. The CURVE is the result, not the argmax: a peak '
              'on a plateau is')
        print('aperture ambiguity, and reading the maximum off one would '
              'invent a precision\nthat is not there.\n')
        for which, name in ((0, 'left eye'), (1, 'right eye')):
            r, m = half(off, which), half(on, which)
            best, curve = None, []
            y = lo
            while y <= hi + 1e-9:
                _, f, _ = fit_at(r, m, a.sx, sy, y, a.pitch, band)
                curve.append((y, f))
                if best is None or f > best[1]:
                    best = (y, f)
                y += step
            print(f'  {name}: best yaw {best[0]:+.2f}°  fit {best[1]:.3f}')
            for y, f in curve:
                bar = '#' * max(0, int(round(f * 40)))
                mark = '  <-' if y == best[0] else ''
                print(f'    {y:+6.2f}°  {f:+.3f} {bar}{mark}')
            print()
        return
    for which, name in ((0, 'left eye'), (1, 'right eye')):
        report(half(off, which), half(on, which), a.sx, sy, a.yaw, a.pitch,
               band, name)
    print('\nA LIFT is the result. Correlation "off" vs "on" is low because the')
    print('world moved; if the predicted warp is the one X4 applied, warping')
    print('the "off" frame by it brings the correlation back up. No lift means')
    print('the transform is not the one predicted -- run --self-check first to')
    print('confirm the predictor itself, then vary --yaw to see what does fit.')


if __name__ == '__main__':
    main()
