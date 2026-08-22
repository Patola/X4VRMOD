#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Is a HUD element screen-space or world geometry? Read it off an SBS frame.

    tools/hud_frame.py <sbs.png|jpg> --region reticle=672,660,64,64 \
                                     --region radar=1100,975,190,190
    tools/hud_frame.py <sbs.png|jpg> --anaglyph radar=1080,960,220 --out /tmp/a

Task #55. Our per-eye shear displaces by **inverse depth** -- a draw at `w = 1`
is immune by construction -- so the horizontal disparity between the eyes of
one frame says what frame an element was drawn in. No new run, no instrument in
the layer.

**THREE CHANNELS, ALL REPORTED. This is the whole design.**

    luma    what the eye sees: overlay AND background together
    hud     (G+B)/2 - R -- keeps X4's green/cyan HUD, kills its warm cockpit
            and its white stars
    grey    luma - 3*hud -- the opposite: the background with the overlay taken
            out

A thin overlay lying on textured geometry has TWO things in the window at two
depths, and a single number reports whichever won the correlation. Take 162
measured the radar at -30 px through luma and concluded `separateRadar=0` makes
the radar world geometry; splitting the same window here gives **hud +0,
grey -29**. The -30 was the cockpit panel the radar is drawn on. Where the
channels disagree, that disagreement IS the finding, and the `hud` number is
the one about the overlay.

**THE RANGE, because a zero has two causes.** Screen-space content reads 0 px.
So does world geometry far enough away that `sx * ipd * width / (2*z)` rounds
to nothing -- the starfield reads 0 in every frame here. This method therefore
separates *screen-space or infinity* from *near geometry*; it does NOT separate
screen-locked from world-at-infinity, and for #33 that is the distinction that
matters. What separates those is a canvas at a FINITE distance
(`X4VR_CANVAS_M=2`): it moves screen-space content and leaves world alone. At
infinity #40 deliberately makes the canvas agree with world geometry, so a
canted take cannot answer it either.

**Search wide, and check the peak is not the bound.** The first version of this
capped at -40 px and returned -40 for every near-cockpit window -- the bound,
not a measurement. `PEGGED` is printed when the peak sits within 2 px of an end
of the range.

**`--anaglyph` is the check a person can make.** Left eye into red, right into
green+blue: zero-disparity content comes out sharp and single, parallax comes
out fringed. It agreed with the numbers on both elements #55 settled, and it is
the form to hand over when the question is "is this thing flat".
"""
import argparse
import sys

import numpy as np
from PIL import Image


def decode(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def channels(lin):
    luma = 0.2126 * lin[..., 0] + 0.7152 * lin[..., 1] + 0.0722 * lin[..., 2]
    hud = np.clip(0.5 * (lin[..., 1] + lin[..., 2]) - lin[..., 0], 0.0, None)
    return {'luma': luma, 'hud': hud, 'grey': np.clip(luma - 3.0 * hud, 0, None)}


def scan(ch, w, x, y, tw, th, lo, hi):
    left, right = ch[:, :w], ch[:, w:2 * w]
    tpl = left[y:y + th, x:x + tw]
    if tpl.shape != (th, tw):
        return None
    t = tpl - tpl.mean()
    nt = float(np.sqrt((t * t).sum()))
    curve = []
    for dx in range(lo, hi + 1):
        xs = x + dx
        if xs < 0 or xs + tw > w:
            continue
        b = right[y:y + th, xs:xs + tw]
        if b.shape != tpl.shape:
            continue
        b0 = b - b.mean()
        nb = float(np.sqrt((b0 * b0).sum()))
        if nt < 1e-9 or nb < 1e-9:
            continue
        curve.append((dx, float((t * b0).sum() / (nt * nb))))
    if not curve:
        return None
    dx, pk = max(curve, key=lambda p: p[1])
    rivals = [c for d, c in curve if abs(d - dx) > 3]
    return dx, pk, pk - (max(rivals) if rivals else 0.0), float(tpl.std())


def anaglyph(path, spec, out):
    im = np.asarray(Image.open(path).convert('RGB'))
    w = im.shape[1] // 2
    name, _, rect = spec.partition('=')
    v = [int(t) for t in rect.split(',')]
    x, y, tw = v[0], v[1], v[2]
    th = v[3] if len(v) > 3 else tw
    lc, rc = im[y:y + th, x:x + tw], im[y:y + th, w + x:w + x + tw]
    if lc.shape != rc.shape:
        sys.exit(f'{name}: rectangle falls outside one of the eyes')
    a = np.zeros_like(lc)
    a[..., 0] = lc[..., 0]              # left eye -> red
    a[..., 1] = rc[..., 1]              # right eye -> green + blue
    a[..., 2] = rc[..., 2]
    p = f'{out}-{name}.png'
    Image.fromarray(a).resize((tw * 3, th * 3), Image.NEAREST).save(p)
    print(f'anaglyph {name}: {p}  (sharp = zero disparity, fringed = parallax)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('--region', action='append', default=[],
                    metavar='NAME=X,Y,W[,H]', help='left-eye rectangle')
    ap.add_argument('--anaglyph', action='append', default=[],
                    metavar='NAME=X,Y,W[,H]')
    ap.add_argument('--out', default='/tmp/hud_frame')
    ap.add_argument('--search', default='-200,60', help='dx range (px)')
    a = ap.parse_args()

    for spec in a.anaglyph:
        anaglyph(a.image, spec, a.out)
    if not a.region:
        if not a.anaglyph:
            sys.exit('give --region and/or --anaglyph')
        return

    lin = decode(a.image)
    H, W = lin.shape[:2]
    w = W // 2
    chans = channels(lin)
    lo, hi = (int(v) for v in a.search.split(','))
    print(f'{a.image}  {W}x{H}, eye {w}x{H}, search {lo}..{hi} px\n')
    print(f"{'region':<16}{'channel':<8}{'dx':>7}{'ncc':>8}{'margin':>8}"
          f"{'texture':>9}  note")
    for spec in a.region:
        name, _, rect = spec.partition('=')
        v = [int(t) for t in rect.split(',')]
        x, y, tw = v[0], v[1], v[2]
        th = v[3] if len(v) > 3 else tw
        for cn, ch in chans.items():
            r = scan(ch, w, x, y, tw, th, lo, hi)
            if r is None:
                print(f'{name:<16}{cn:<8}   region falls outside the eye')
                continue
            dx, pk, margin, sd = r
            note = ''
            if dx <= lo + 2 or dx >= hi - 2:
                note = 'PEGGED at the search bound — widen --search'
            elif pk < 0.45 or margin < 0.05:
                note = 'UNRESOLVED — nothing to lock onto here'
            elif dx == 0:
                note = 'flat: screen-space OR at infinity (not separable)'
            else:
                note = 'parallax: world geometry'
            print(f'{name:<16}{cn:<8}{dx:>+7d}{pk:>8.3f}{margin:>8.3f}'
                  f'{sd:>9.4f}  {note}')
        print()


if __name__ == '__main__':
    main()
