#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""How far does X4's HUD wander against the world? Track ONE element.

    tools/hud_drift.py <clip.mp4|glob> --px-per-deg 9.07

Task #57. The HUD is meant to sit still on the cockpit; whatever it does
instead is the "slip". This measures it as an absolute position — the HUD
element's position minus the world's accumulated motion, which is CONSTANT if
the HUD is locked to the cockpit — and reports its wander.

**Why this replaces the largest-blob version, and take 251 is the reason.**
The first cut tracked "the largest cyan blob in the frame". That is the same
physical element only while it stays in frame AND stays the largest. Patola
recorded take 251 with wider head sweeps than the earlier clips, the
target-bracket cluster left the view — one frame kept 22 HUD pixels — and the
tracker silently latched onto a different element. Its centroid jumped up to
1577 px, and the wander came out at 21.5 deg: a confident number describing the
distance between two different HUD elements.

Checked rather than assumed: the two earlier clips were re-measured for the same
failure and are CLEAN — 0 re-targets in 300 and 386 frame pairs, max jump 24.2
and 33.4 px. So the 2.34 deg (2.50 m) and 7.52 deg (0.70 m) figures those clips
produced, and the 1/distance scaling that identified the slip as parallax, are
not affected by this defect. Take 251's clip alone is, with 24 re-targets.

**So: match ONE template, taken once, and say TARGET LOST rather than following
something else.** A tracker that silently re-targets is the same failure class
as an instrument that answers the question it was built for after the question
moved — it reports confidently about a different object.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hud_vs_camera import decode, channels, shift          # noqa: E402


def ncc_find(tpl, img, margin_min):
    """Locate tpl anywhere in img by normalised cross-correlation.

    Returns (x, y, peak, margin) or None when the peak does not beat its
    rivals — which is reported as a LOST frame, never as a position.
    """
    th, tw = tpl.shape
    t = tpl - tpl.mean()
    nt = float(np.sqrt((t * t).sum()))
    if nt < 1e-6:
        return None
    # FFT-free: the frames are small and this stays legible.
    best = None
    scores = np.full((img.shape[0] - th + 1, img.shape[1] - tw + 1), -2.0)
    for y in range(0, scores.shape[0], 2):
        for x in range(0, scores.shape[1], 2):
            w = img[y:y + th, x:x + tw]
            b = w - w.mean()
            nb = float(np.sqrt((b * b).sum()))
            if nb < 1e-6:
                continue
            scores[y, x] = float((t * b).sum() / (nt * nb))
    iy, ix = np.unravel_index(np.argmax(scores), scores.shape)
    pk = scores[iy, ix]
    # rivals: anything further than the template's own size away
    mask = np.ones_like(scores, dtype=bool)
    y0, y1 = max(0, iy - th), min(scores.shape[0], iy + th)
    x0, x1 = max(0, ix - tw), min(scores.shape[1], ix + tw)
    mask[y0:y1, x0:x1] = False
    rival = float(scores[mask].max()) if mask.any() else -1.0
    m = pk - rival
    if m < margin_min:
        return None
    return ix + tw / 2.0, iy + th / 2.0, pk, m


def frames_of(pattern, outdir, fps):
    if pattern.lower().endswith(('.mp4', '.mkv', '.webm', '.mov', '.avi')):
        os.makedirs(outdir, exist_ok=True)
        for old in glob.glob(f"{outdir}/f*.png"):
            os.unlink(old)
        subprocess.run(['ffmpeg', '-v', 'error', '-i', pattern,
                        '-vf', f'fps={fps}', f'{outdir}/f%05d.png'], check=True)
        pattern = f"{outdir}/f*.png"
    return sorted(glob.glob(pattern),
                  key=lambda p: [int(t) if t.isdigit() else t
                                 for t in re.split(r'(\d+)', p)])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern', help='clip or frame glob')
    ap.add_argument('--px-per-deg', type=float,
                    help='angular scale; without it the report stays in px')
    ap.add_argument('--fps', type=float, default=15)
    ap.add_argument('--frames-dir', default='/tmp/x4vr-drift')
    ap.add_argument('--margin', type=float, default=0.08,
                    help='NCC peak must beat its best rival by this')
    a = ap.parse_args()

    fs = frames_of(a.pattern, a.frames_dir, a.fps)
    if len(fs) < 20:
        print(f'need frames, got {len(fs)}', file=sys.stderr)
        return 2

    tpl = None
    prev = None
    wx = 0.0
    rows = []
    lost = 0
    for f in fs:
        ch = channels(decode(f))
        hud, grey = ch['hud'], ch['grey']
        if prev is not None:
            s = shift(prev, grey, -60, 60)
            if s and s[2] > 0.03:
                wx += s[0]
        prev = grey
        if tpl is None:
            m = hud > 0
            if m.sum() < 80:
                continue
            lab, n = ndimage.label(m)
            if n == 0:
                continue
            sz = ndimage.sum(m, lab, range(1, n + 1))
            b = int(np.argmax(sz)) + 1
            ys, xs = np.nonzero(lab == b)
            pad = 8
            y0, y1 = max(0, ys.min() - pad), min(hud.shape[0], ys.max() + pad)
            x0, x1 = max(0, xs.min() - pad), min(hud.shape[1], xs.max() + pad)
            if (y1 - y0) < 12 or (x1 - x0) < 12:
                continue
            tpl = hud[y0:y1, x0:x1].copy()
            print(f'template: {x1-x0}x{y1-y0} px from {f.split("/")[-1]}')
            continue
        r = ncc_find(tpl, hud, a.margin)
        if r is None:
            lost += 1
            continue
        rows.append((wx, r[0], r[1], r[3]))

    if len(rows) < 20:
        print(f'TARGET LOST in {lost} frame(s); only {len(rows)} usable. '
              f'This clip cannot be measured with this template.')
        return 1
    a_ = np.array(rows)
    relx = a_[:, 1] - a_[:, 0]
    k = a.px_per_deg or 1.0
    unit = 'deg' if a.px_per_deg else 'px'
    print(f'  tracked {len(rows)} frame(s), TARGET LOST in {lost} '
          f'({100*lost/(len(rows)+lost):.0f}%)   median match margin '
          f'{np.median(a_[:,3]):.3f}')
    print(f'  world travel      {a_[:,0].min():.0f}..{a_[:,0].max():.0f} px')
    print(f'  HUD-vs-world sd   {relx.std()/k:.2f} {unit}'
          f'   range {(relx.max()-relx.min())/k:.2f} {unit}')
    q = len(a_) // 4
    print('  by quarter        ' + '  '.join(
        f'{relx[i*q:(i+1)*q].mean()/k:+.2f}' for i in range(4)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
