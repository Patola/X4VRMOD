#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Does X4's HUD stay fixed to the COCKPIT? Measure the thing Patola describes.

    tools/hud_vs_cockpit.py <clip.mp4> --px-per-deg 14.17

Task #57. Patola's report has been the same for a dozen takes: *"the HUD goes
to the opposite side of my head, not becoming fixed in relation to the
cockpit"*. Every instrument built for it so far measured something else.

**Why the previous two instruments cannot answer this.**

`hud_drift` measures HUD position minus the ACCUMULATED world shift. Two faults,
both fatal here. The accumulation is a random walk -- a per-frame bias of 0.05 px
integrates to 20 px of invented slip over 400 frames -- and it fails its own
negative control: on the vanilla clip, where relative slip is structurally zero,
it reports 45 px of MAD with 69% of frames lost.

`hud_vs_camera` measures the RATE ratio dx_hud/dx_grey. It is sound (it reads
1.00 on the vanilla control) but it answers "world-anchored or screen-locked",
and its `grey` channel is the whole scene -- a starfield at infinity mixed with
a cockpit at arm's length. Those two move identically under head ROTATION and
differently under head TRANSLATION, so averaging them is averaging the reference
frame itself. It also measures rates, and a parallax offset is a position.

**What this does instead.** Track two patches in the SAME frame: one HUD, one
cockpit. Report the VECTOR BETWEEN THEM.

    d(t) = hud_pos(t) - cockpit_pos(t)

If the HUD is fixed to the cockpit, d is constant -- that is what "fixed to the
cockpit" MEANS, and it is the sentence Patola keeps writing. Everything common
to the two cancels exactly, in the frame, with no estimate in between: head
rotation, the servo's lag, the compositor's rigid reprojection, camera shake,
and the FOV mapping all move both patches together and drop out of the
difference. Nothing accumulates, so there is no walk to drift, and the floor is
template-match noise rather than a property of the clip.

The cockpit patch is the reference Patola actually uses. Naming it explicitly is
the point: an instrument that averages the cockpit together with infinity cannot
answer a question posed about the cockpit.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hud_vs_camera import decode, channels                   # noqa: E402
from hud_drift import ncc_find, frames_of                    # noqa: E402


def pick_hud(hud, min_px=80, pad=8):
    """Largest cyan blob -- the same choice hud_drift makes, kept on purpose so
    the two tools are talking about the same object."""
    m = hud > 0
    if m.sum() < min_px:
        return None
    lab, n = ndimage.label(m)
    if n == 0:
        return None
    sz = ndimage.sum(m, lab, range(1, n + 1))
    ys, xs = np.nonzero(lab == int(np.argmax(sz)) + 1)
    y0, y1 = max(0, ys.min() - pad), min(hud.shape[0], ys.max() + pad)
    x0, x1 = max(0, xs.min() - pad), min(hud.shape[1], xs.max() + pad)
    if (y1 - y0) < 12 or (x1 - x0) < 12:
        return None
    return hud[y0:y1, x0:x1].copy(), ((x0 + x1) / 2.0, (y0 + y1) / 2.0)


def pick_cockpit(grey, hud, size=64, avoid=48, exclude=None):
    """The most distinctive cockpit patch: highest gradient energy, away from
    the HUD and away from the frame edge.

    Gradient energy rather than brightness because the cockpit's struts are
    DARK against a starfield -- a luminance test would pick the stars, which
    are at infinity and are exactly the reference this tool exists to avoid.
    """
    gy, gx = np.gradient(grey.astype(np.float32))
    e = gx * gx + gy * gy
    # Stars are tiny and isolated; a box filter rewards extended structure.
    e = ndimage.uniform_filter(e, size)
    e[hud > 0] = 0.0
    if avoid:
        # Not just "no HUD pixels here" but "no HUD pixels NEARBY": a patch that
        # merely touches the HUD would drag the HUD's own motion into the
        # reference, which is the one contamination this tool cannot tolerate.
        near = ndimage.uniform_filter((hud > 0).astype(np.float32), avoid)
        e[near > 0] = 0.0
    h, w = grey.shape
    m = size
    e[:m, :] = 0; e[-m:, :] = 0; e[:, :m] = 0; e[:, -m:] = 0
    if exclude:
        # Keep a second patch well clear of the first, so the two are not
        # overlapping views of one feature.
        for (ex, ey) in exclude:
            y0, y1 = max(0, int(ey) - 2 * size), min(h, int(ey) + 2 * size)
            x0, x1 = max(0, int(ex) - 2 * size), min(w, int(ex) + 2 * size)
            e[y0:y1, x0:x1] = 0.0
    iy, ix = np.unravel_index(np.argmax(e), e.shape)
    y0, y1 = iy - size // 2, iy + size // 2
    x0, x1 = ix - size // 2, ix + size // 2
    return grey[y0:y1, x0:x1].copy(), ((x0 + x1) / 2.0, (y0 + y1) / 2.0)


def track(fs, margin, radius, cock_size, control=False):
    """control=True is the MATCHED negative control, and it is not optional
    diligence -- it is the only thing that says whether a reading is real.

    It replaces the HUD patch with a SECOND COCKPIT patch. Two patches on the
    same rigid cockpit cannot move relative to each other, so the true answer
    is ZERO, measured on the same clip, with the same head motion, the same
    template sizes and the same tracking difficulty as the real run.

    The vanilla clip does NOT do this job. Its median per-frame motion is 0.0 px
    -- it is nearly static, so it only ever showed that the tool invents nothing
    when nothing moves. It never showed that the tool TRACKS under motion, which
    is the failure mode that retired hud_drift.
    """
    hud_t = cock_t = None
    hud_p = cock_p = None
    rows, lost_h, lost_c = [], 0, 0
    for f in fs:
        ch = channels(decode(f))
        hud, grey = ch['hud'], ch['grey']
        first = hud if not control else grey
        if hud_t is None:
            if control:
                cock_t, cock_p = pick_cockpit(grey, hud, cock_size)
                got = pick_cockpit(grey, hud, cock_size, exclude=[cock_p])
            else:
                got = pick_hud(hud)
            if not got:
                continue
            hud_t, hud_p = got
            if not control:
                cock_t, cock_p = pick_cockpit(grey, hud, cock_size)
            print(f'  {"cockpit-B" if control else "HUD      "} template '
                  f'{hud_t.shape[1]}x{hud_t.shape[0]} px '
                  f'at ({hud_p[0]:.0f},{hud_p[1]:.0f})')
            print(f'  cockpit   template {cock_t.shape[1]}x{cock_t.shape[0]} px '
                  f'at ({cock_p[0]:.0f},{cock_p[1]:.0f})')
            continue
        rh = ncc_find(hud_t, first, margin, near=hud_p, radius=radius)
        rc = ncc_find(cock_t, grey, margin, near=cock_p, radius=radius)
        if rh is None:
            lost_h += 1
        if rc is None:
            lost_c += 1
        if rh is None or rc is None:
            continue
        hud_p, cock_p = (rh[0], rh[1]), (rc[0], rc[1])
        rows.append((rh[0] - rc[0], rh[1] - rc[1], rh[3], rc[3]))
    return np.array(rows), lost_h, lost_c


def selftest():
    """Audit on synthetic frames, in both configurations that matter.

    RIGID: shift both patches together. A tool that reports anything but zero
    is inventing slip. RELATIVE: shift the HUD alone by a known amount, which
    the tool must recover exactly. The first version of `hud_vs_camera` passed
    a rigid audit and had never been shown a differential one.
    """
    rng = np.random.default_rng(7)
    base = rng.random((400, 400)).astype(np.float32)
    base = ndimage.gaussian_filter(base, 2) * 255.0
    hud_t = base[100:130, 100:130].copy()
    cock_t = base[300:340, 300:340].copy()
    ok = True
    for name, dh, dc in (('rigid  +10/+10', 10, 10), ('rigid  -7/-7', -7, -7),
                         ('relative +6', 6, 0), ('relative -4', -4, 0)):
        img_h = np.roll(base, dh, axis=1)
        img_c = np.roll(base, dc, axis=1)
        rh = ncc_find(hud_t, img_h, 0.05, near=(115 + dh, 115), radius=40)
        rc = ncc_find(cock_t, img_c, 0.05, near=(320 + dc, 320), radius=40)
        if rh is None or rc is None:
            print(f'  {name:16s} LOST'); ok = False; continue
        got = (rh[0] - rc[0]) - (115 - 320)
        want = float(dh - dc)
        good = abs(got - want) < 1.0
        ok &= good
        print(f'  {name:16s} want {want:+.1f}  got {got:+.1f}   '
              f'{"ok" if good else "FAIL"}')
    print('selftest', 'PASSED' if ok else 'FAILED')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern', nargs='?', help='clip or frame glob')
    ap.add_argument('--px-per-deg', type=float)
    ap.add_argument('--fps', type=float, default=15)
    ap.add_argument('--frames-dir', default='/tmp/x4vr-hvc')
    ap.add_argument('--margin', type=float, default=0.08)
    ap.add_argument('--radius', type=float, default=140.0)
    ap.add_argument('--cockpit-size', type=int, default=64)
    ap.add_argument('--control', action='store_true',
                    help='MATCHED negative control: track two COCKPIT patches '
                         'instead of HUD-vs-cockpit. They are rigidly attached, '
                         'so the true answer is zero on the same clip with the '
                         'same motion and the same tracking difficulty. Any '
                         'reading here is the floor for that clip.')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if not a.pattern:
        ap.error('need a clip (or --selftest)')

    fs = frames_of(a.pattern, a.frames_dir, a.fps)
    if len(fs) < 20:
        print(f'need frames, got {len(fs)}', file=sys.stderr)
        return 2
    attempted = len(fs) - 1          # frame 1 supplies the templates
    rows, lost_h, lost_c = track(fs, a.margin, a.radius, a.cockpit_size,
                                 a.control)
    if a.control:
        print('  [MATCHED CONTROL: two cockpit patches, true answer is ZERO]')
    if len(rows) < 20:
        print(f'TARGET LOST -- HUD {lost_h}, cockpit {lost_c}; only '
              f'{len(rows)} usable pair(s). This clip cannot be measured.')
        return 1

    k = a.px_per_deg or 1.0
    unit = 'deg' if a.px_per_deg else 'px'
    n = len(rows)
    # Percentages are per tracker over the frames ATTEMPTED, not over the
    # frames that survived -- dividing losses by successes is how a 67% loss
    # rate printed as "328%".
    print(f'  tracked {n} of {attempted} frame(s); LOST  '
          f'HUD {lost_h} ({100*lost_h/attempted:.0f}%)  '
          f'cockpit {lost_c} ({100*lost_c/attempted:.0f}%)')
    print(f'  match margins    HUD {np.median(rows[:,2]):.3f}  '
          f'cockpit {np.median(rows[:,3]):.3f}')
    for ax, col in (('x', 0), ('y', 1)):
        v = rows[:, col]
        mad = float(np.median(np.abs(v - np.median(v)))) * 1.4826
        print(f'  HUD-vs-cockpit {ax}   MAD {mad/k:.2f} {unit}'
              f'   sd {v.std()/k:.2f}   range {(v.max()-v.min())/k:.2f} '
              f'   (offset {np.median(v)/k:+.1f})')
        if v.std() > 2.5 * mad:
            print(f'  NOTE {ax}: sd over 2.5x the robust spread -- outliers; '
                  f'trust the MAD and check the lost counts')
    return 0


if __name__ == '__main__':
    sys.exit(main())
