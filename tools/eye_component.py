#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Characterise a component that one eye has and the other has not.

    tools/eye_component.py <layer0.png> <layer1.png>
    tools/eye_component.py --prefix=/tmp/x4vr-t183 --frames=10,16,12,17

Task #49 was mis-modelled three times before this tool existed. Each mistake
is now a check here, and each one cost a conclusion:

1. **Absent or displaced?** This is the top-level split and nothing else means
   anything until it is settled. If a MONO screen-space buffer is sampled at
   each eye's own UV, the component still exists -- drawn about one infinity
   offset (~1027 px at 4224) from where it belongs. If it is nowhere in the
   frame, the mechanism is a GATE and every "wrong UV" story is dead. #49 spent
   several takes on the godray buffer before this was measured; the answer was
   "absent", and it retired the whole hypothesis in one command.

2. **Do not measure angular structure at the radius of peak amplitude.** The
   first version reported the angular profile at 20-40 px -- hard against the
   solar disc edge, where a plume has not yet separated from the core -- and
   called a plainly diagonal plume "isotropic". Structure and amplitude peak at
   different radii. This reports every radius band.

3. **Always carry a negative control.** Take 183's n12 (both eyes carry the
   component) and n17 (neither does) read 35-100x below the positives with
   random directions. Without them, noise at 0.005 and signal at 0.176 are just
   two numbers.

Within one frame the eyes are the same instant, so at a distant object's
position parallax is zero and the eye difference IS the component -- no
temporal change, no scene motion, no exposure drift to argue about. That is
why this compares layer0 against layer1 of ONE dump rather than one eye across
two dumps.
"""
import argparse
import sys

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

# Content at infinity lands this far apart between the eyes: B_x * width.
OFFSET = 1027.0
HALF = 400
MIN_CORE_PX = 150
R_IN, R_OUT = 20, 60


def load_lin(path):
    a = np.array(Image.open(path).convert('RGB')) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def luma(x):
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def anchor(lin):
    """Centroid of the largest saturated blob, or None.

    argmax returns the raster-first pixel of a saturated plateau, and the
    plateau is a different shape in each eye -- that misalignment used to go
    straight into the numbers.
    """
    lum = luma(lin)
    lab, n = ndi.label(lum >= 0.99)
    if n == 0:
        return None
    sizes = ndi.sum(np.ones_like(lab), lab, range(1, n + 1))
    k = int(np.argmax(sizes)) + 1
    if sizes[k - 1] < MIN_CORE_PX:
        return None
    cy, cx = ndi.center_of_mass(lum >= 0.99, lab, k)
    return int(round(cy)), int(round(cx))


def annulus_map(w):
    """Mean of w over an annulus at every pixel, from two box filters."""
    a = ndi.uniform_filter(w, size=2 * R_OUT + 1)
    b = ndi.uniform_filter(w, size=2 * R_IN + 1)
    ao, bo = (2 * R_OUT + 1) ** 2, (2 * R_IN + 1) ** 2
    return (a * ao - b * bo) / (ao - bo)


def absent_or_displaced(rich, poor, c_rich, c_poor):
    """Is the component elsewhere in the poorer eye, or nowhere at all?"""
    amap = annulus_map(poor[..., 0] - poor[..., 2])
    rmap = annulus_map(rich[..., 0] - rich[..., 2])
    here = float(amap[c_poor[0], c_poor[1]])
    there = float(rmap[c_rich[0], c_rich[1]])
    print(f'    poorer eye at its own anchor reads {here:+.4f}; '
          f'richer eye reads {there:+.4f}')
    verdict = 'ABSENT'
    for name, x in (('+offset', c_poor[1] + OFFSET),
                    ('-offset', c_poor[1] - OFFSET)):
        if not 0 <= x < amap.shape[1]:
            print(f'    {name} site is off-image')
            continue
        v = float(amap[c_poor[0], int(x)])
        hit = v > 0.5 * there
        if hit:
            verdict = 'DISPLACED'
        print(f'    {name} site x={x:.0f} reads {v:+.4f}   '
              f'{"CANDIDATE" if hit else "nothing there"}')
    m = amap.copy()
    yy, xx = np.mgrid[0:m.shape[0], 0:m.shape[1]]
    m[np.hypot(yy - c_poor[0], xx - c_poor[1]) < 150] = -9
    i = int(np.argmax(m))
    py, px = i // m.shape[1], i % m.shape[1]
    print(f'    strongest annulus elsewhere: ({px},{py}) {m.ravel()[i]:+.4f}, '
          f'dx from anchor {px - c_poor[1]:+.0f} px')
    print(f'    -> {verdict}: '
          + ('the component exists, drawn in the wrong place — look for a '
             'mono buffer sampled at each eye\'s own UV'
             if verdict == 'DISPLACED' else
             'the component was never generated for that eye — a GATE, not a '
             'misread UV'))


def directional(diff):
    """Amplitude and axis of the excess, per radius band."""
    n = diff.shape[0]
    yy, xx = np.mgrid[0:n, 0:n] - n / 2.0
    r = np.hypot(yy, xx)
    th = np.arctan2(yy, xx)
    print('    radius      amp     anisotropy   axis')
    for lo in (20, 40, 60, 90, 130, 180, 240, 320):
        hi = lo * 1.4
        m = (r >= lo) & (r < hi)
        v = diff[m]
        w = np.clip(v, 0, None)
        if w.sum() <= 0:
            print(f'    {lo:>4}-{hi:<5.0f} {v.mean():+.4f}   --')
            continue
        a = th[m]
        cx_ = float((w * np.cos(2 * a)).sum() / w.sum())
        sy_ = float((w * np.sin(2 * a)).sum() / w.sum())
        print(f'    {lo:>4}-{hi:<5.0f} {v.mean():+.4f}   '
              f'{np.hypot(cx_, sy_):.3f}        '
              f'{np.degrees(np.arctan2(sy_, cx_)) / 2.0:+6.1f} deg')


def run(tag, p0, p1):
    l0, l1 = load_lin(p0), load_lin(p1)
    c0, c1 = anchor(l0), anchor(l1)
    print(f'=== {tag} ===')
    if not c0 or not c1:
        print('    no saturated core in one or both eyes — nothing to compare')
        return
    w0 = l0[c0[0] - HALF:c0[0] + HALF, c0[1] - HALF:c0[1] + HALF]
    w1 = l1[c1[0] - HALF:c1[0] + HALF, c1[1] - HALF:c1[1] + HALF]
    if w0.shape != w1.shape:
        print('    anchor too close to an edge — window would be clipped')
        return
    d = luma(w0) - luma(w1)
    band = np.hypot(*np.mgrid[0:d.shape[0], 0:d.shape[0]] - d.shape[0] / 2.0)
    amp = float(d[(band >= 40) & (band < 56)].mean())
    rich, poor = (0, 1) if amp > 0 else (1, 0)
    print(f'    layer0 anchor {c0[::-1]}  layer1 anchor {c1[::-1]}  '
          f'dx={c1[1] - c0[1]:+d}')

    # The absent/displaced verdict must not fire when there is nothing to
    # locate. On a both-eyes-agree control the difference is noise, and an
    # earlier version dutifully reported "ABSENT" for it -- a verdict that
    # cannot fail is not a verdict. Scale against what the richer eye actually
    # carries, not against a fixed number: the component's own amplitude
    # varies 3x across a session.
    ref = float(annulus_map((l0 if rich == 0 else l1)[..., 0]
                            - (l0 if rich == 0 else l1)[..., 2])[
                    (c0 if rich == 0 else c1)[0],
                    (c0 if rich == 0 else c1)[1]])
    frac = abs(amp) / max(ref, 1e-6)
    print(f'    layer{rich} carries more (40-56 px difference {amp:+.4f}, '
          f'{frac:.0%} of that eye\'s own {ref:+.4f})')
    if frac < 0.10:
        print('    -> BOTH EYES AGREE: no component to locate. This is a '
              'control, not a null result.')
        return
    directional(d if rich == 0 else -d)
    absent_or_displaced(*((l0, l1, c0, c1) if rich == 0 else (l1, l0, c1, c0)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('images', nargs='*')
    ap.add_argument('--prefix', help='dump prefix, e.g. /tmp/x4vr-t183')
    ap.add_argument('--frames', help='comma-separated present indices')
    a = ap.parse_args()
    if a.prefix:
        if not a.frames:
            sys.exit('--prefix needs --frames (a negative control among them)')
        for f in a.frames.split(','):
            run(f'present-n{f}', f'{a.prefix}-present-n{f}-layer0.png',
                f'{a.prefix}-present-n{f}-layer1.png')
    elif len(a.images) == 2:
        run('pair', *a.images)
    else:
        ap.error('give two images, or --prefix with --frames')


if __name__ == '__main__':
    main()
