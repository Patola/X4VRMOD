#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Does X4's HUD move with the world, or with the screen? Measure it, finally.

    tools/hud_vs_camera.py '/tmp/x4vr-t245-present-n*-layer0.png' \
                           --log /tmp/x4vr-take245.log

**Why this exists, and it is not a flattering reason.** Takes 242, 243 and 244
all placed X4's UI on a quad, three different ways, and Patola's report came
back materially unchanged each time: *"the HUD still goes to the opposite
direction of my head"*. Every one of those placements was built on an ASSUMPTION
about where the HUD sits inside X4's UI texture as the camera turns, and the
assumption was different each time:

    take 242   screen-locked  (content at 0)
    take 243   world-anchored (content at -C)
    take 244   ditto, orbited

Nothing ever measured it. #55 measured the HUD's DISPARITY and said in writing
that disparity cannot separate screen-locked from world-at-infinity — the two
diverge only under a head ROTATION. This is that measurement.

**The method, and the control is the point.** Take the frames in order and
cross-correlate CONSECUTIVE ones, on both axes, in two channels:

    hud    cyan-only mask   X4's overlay (see channels() for why not (G+B)/2-R)
    grey   luma, HUD removed   the cockpit

`grey` is not decoration: it is the WORLD's motion in the same image, measured
by the same code, at the same instant. It calibrates pixels-per-degree without
anyone having to know the field, and it is the negative control for camera
shake. The verdict is the RATIO of the two, and every reading is reported:

    dx_hud / dx_grey ~ 1     the HUD moves with the world  -> world-anchored
    dx_hud ~ 0, dx_grey != 0 the HUD is nailed to the frame -> screen-locked
    ratio ~ 2                X4 counter-rotates it twice — which is what the
                             three failed takes imply and nothing has confirmed

**A zero in `grey` means the run says NOTHING**, not that the HUD is fixed. If
the camera did not turn between two dumps there is no question to answer, and
those pairs are reported as UNUSABLE rather than folded into an average. Take
237's 74 dumps are exactly that case — its log shows X4's camera at 0.00 for
the whole session — and they make a real negative control: both channels must
read zero there, and a tool that reports motion on them is broken.
"""

import argparse
import glob
import re
import sys

import numpy as np
from PIL import Image
from scipy import ndimage


def decode(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    return np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)


def channels(lin):
    """Isolate X4's HUD from the scene behind it.

    **`hud_frame.py`'s channel does not work here, and take 245b is the
    counter-example.** That tool uses `(G+B)/2 - R`, which was right for take
    162's frame. Take 245b's cockpit looks out at a blue-purple NEBULA, and
    that expression scores the entire nebula as HUD: measured coverage 66% of
    the frame, where a HUD overlay is well under 1%. Both channels then carried
    the same content, every correlation margin collapsed to ~0.001, and the
    tool reported "the camera did not turn" for a run whose log shows it
    turning through 62 degrees.

    The separator that works is that X4's HUD is **cyan** — G and B roughly
    equal — while the nebula is **blue-purple**, B clearly above G. So require
    G to be at least as strong as B, and both well above R. On take 245b's n17
    that gives 0.09% coverage and picks out the radar arc and the target
    brackets; `(G+B)/2 - R` gave 66%.

    A colour test alone still catches blue-white stars, which are WORLD content
    and would bias the answer toward "world-anchored" — the very verdict being
    tested. Do not trust this channel on a star-heavy frame without checking
    the margin, and prefer dumping the UI target on its own, where the question
    does not arise at all.
    """
    R, G, B = lin[..., 0], lin[..., 1], lin[..., 2]
    mx = lin.max(2)
    hud = np.where((G >= 0.9 * B) & (G > 1.3 * R) & (mx > 0.1), mx, 0.0)
    luma = 0.2126 * R + 0.7152 * G + 0.0722 * B
    grey = np.where(hud > 0, 0.0, luma)
    cock = cockpit_mask(grey, hud)
    # Two disjoint halves of the SAME cockpit, for the matched control. Both
    # are the same rigid object, so their motion ratio must be 1.00 -- measured
    # on the same clip, with the same motion and the same difficulty as the
    # real run. An unmatched control licenses nothing: `hud_vs_cockpit` read
    # 0.00 on a nearly-static clip and 2.41 deg where the truth was zero on a
    # moving one.
    w = cock.shape[1] // 2
    ca, cb = np.zeros_like(cock), np.zeros_like(cock)
    ca[:, :w] = cock[:, :w]
    cb[:, w:] = cock[:, w:]
    return {'hud': hud, 'grey': grey, 'cockpit': cock,
            'cockpit_a': ca, 'cockpit_b': cb}


def cockpit_mask(grey, hud=None, scale=31, keep=75.0, min_area=0):
    """`grey` with everything but the COCKPIT suppressed.

    **Why `grey` is the wrong reference for #57.** It is the whole scene: a
    cockpit at arm's length mixed with a starfield at infinity. Those two move
    IDENTICALLY under head rotation and DIFFERENTLY under head translation, so
    a ratio against `grey` averages over the reference frame itself. Patola's
    report is always about the cockpit — "not becoming fixed in relation to the
    cockpit" — so the reference has to be the cockpit alone.

    **Local variance at a large scale is the separator, and brightness is not.**
    X4's canopy struts are DARK against a lit nebula in places and light grey
    against black space in others, so no luminance threshold catches both. What
    the cockpit always has is extended structure. In a 31 px window:

        nebula     smooth gradient             -> tiny variance
        strut edge half dark, half light       -> large variance

    **The variance test alone is NOT enough, and looking at the mask is how
    that was found rather than reasoning about it.** The first version argued
    that a star is "one bright pixel in 961, so tiny variance" and kept the top
    quartile. Rendered over the frame, it outlined the canopy correctly *and
    marked every star with its own little box* — a point source against black
    is exactly a high-variance neighbourhood, and the box filter smears it to
    the filter's footprint. That is the starfield at infinity walking straight
    back into the reference the mask exists to purify.

    **Size is the separator that works.** The canopy is a few huge connected
    regions; a star's box is about one filter footprint. Dropping components
    below `min_area` keeps the struts and deletes the sky.

    **And the HUD must be cut out with a MARGIN, which the render also showed.**
    `grey` sets HUD pixels to zero, so every HUD glyph leaves a hard-edged hole,
    and a hole is high variance. With only the exact HUD pixels excluded, the
    mask grew red blobs precisely on the target bracket and the speed gauge —
    the HUD leaking into the channel that is supposed to be its independent
    reference, which would drag the ratio toward 1.00 and manufacture the
    verdict "world-anchored". Excluding a neighbourhood removes the holes' edges
    as well as the holes.
    """
    m1 = ndimage.uniform_filter(grey, scale)
    m2 = ndimage.uniform_filter(grey * grey, scale)
    var = np.maximum(m2 - m1 * m1, 0.0)
    keep_px = var >= np.percentile(var, keep)
    if hud is not None:
        near_hud = ndimage.uniform_filter(
            (hud > 0).astype(np.float32), 2 * scale + 1) > 0
        keep_px &= ~near_hud
    lab, n = ndimage.label(keep_px)
    if n:
        area = ndimage.sum(keep_px, lab, np.arange(1, n + 1))
        big = np.zeros(n + 1, dtype=bool)
        big[1:] = area >= (min_area if min_area else 4 * scale * scale)
        keep_px = big[lab]
    return np.where(keep_px, grey, 0.0)


def shift(a, b, lo, hi, band=None):
    """Horizontal shift from a to b by normalised cross-correlation.

    Returns (dx, peak, margin) or None. `margin` is the peak against the best
    rival more than 3 px away — #55's lesson, where a flat plateau over 40 px
    varying by 0.006 was read as a measurement by taking its argmax. A margin
    near zero means aperture ambiguity, not agreement.
    """
    if band is not None:
        a, b = a[band[0]:band[1]], b[band[0]:band[1]]
    h, w = a.shape
    pad = max(abs(lo), abs(hi)) + 4
    if w <= 2 * pad:
        # **Loud, because silence here reads as a finding.** The first version
        # returned None, the caller printed "no signal", and the summary said
        # "the camera did not turn" — a confident wrong answer produced by the
        # search window not fitting the image. Never let a geometry mistake
        # exit through the same door as a real null.
        raise ValueError(
            f"search range +-{max(abs(lo), abs(hi))} px does not fit a "
            f"{w}px-wide image; pass a smaller --range")
    core = a[:, pad:w - pad]
    t = core - core.mean()
    nt = float(np.sqrt((t * t).sum()))
    if nt < 1e-6:
        return None
    curve = []
    for dx in range(lo, hi + 1):
        win = b[:, pad + dx:w - pad + dx]
        if win.shape != core.shape:
            continue
        b0 = win - win.mean()
        nb = float(np.sqrt((b0 * b0).sum()))
        if nb < 1e-6:
            continue
        curve.append((dx, float((t * b0).sum() / (nt * nb))))
    if not curve:
        return None
    dx, pk = max(curve, key=lambda p: p[1])
    rivals = [c for d, c in curve if abs(d - dx) > 3]
    if dx in (lo, hi):
        return dx, pk, -1.0   # PEGGED: the bound, not a measurement
    return dx, pk, pk - (max(rivals) if rivals else 0.0)


def shift_v(a, b, lo, hi):
    """Vertical shift, by transposing into the horizontal routine.

    **Added because the clip contained the answer and the tool could not see
    it.** Patola's video pans left, right, up and down; measuring only `dx`
    reads the two pitch segments as near-zero motion with noisy ratios, and
    then folds that noise into the same median as the yaw segments. Yaw and
    pitch are separate questions here — the servo drives them with separate
    gains and separate clamps — so they get separate numbers.
    """
    return shift(a.T.copy(), b.T.copy(), lo, hi)


def camera_series(logpath):
    """X4's own camera yaw over time, from X4VR_CAMREAD's lines."""
    out = []
    # `poselag` carries `x4=<yaw>,<pitch>` — X4's OWN readback, present in
    # every run with X4VR_POSELOG=1. The first version grepped `camread`,
    # which only ever announces that the symbol resolved and carries no angle
    # at all, so it reported "no camread lines" on a run that had 15735 of
    # them. Grep for the line that holds the NUMBER, not the one whose name
    # sounds right.
    pat = re.compile(r"^\[\s*(\d+)\.(\d+)\].*poselag:.*x4=(-?\d+\.\d+),")
    try:
        with open(logpath, errors='replace') as f:
            for line in f:
                m = pat.match(line)
                if m:
                    out.append((float(f"{m.group(1)}.{m.group(2)}"),
                                float(m.group(3))))
    except OSError as e:
        print(f"note: no camera series ({e})", file=sys.stderr)
    return out


def frames_from_video(path, outdir, fps=None):
    """Decode a clip to PNG frames and return the glob for them.

    **A video is a better instrument than the dumps for this question**, and
    take 245b is why. Dumps land every ~4 s, so between two of them the head
    has moved through a large, unknown angle and the correlation has no
    unambiguous peak — every margin came back ~0.001. At 30 fps consecutive
    frames differ by a few pixels, which is the regime cross-correlation is
    actually good at, and 10 s gives 300 samples instead of 25.

    Lossy compression is acceptable HERE and would not be for a brightness
    measurement: this reads the POSITION of an edge, not its value. The
    PNG-never-JPEG rule is about photometry.
    """
    import os
    import subprocess
    os.makedirs(outdir, exist_ok=True)
    for old in glob.glob(f"{outdir}/f*.png"):
        os.unlink(old)
    cmd = ['ffmpeg', '-v', 'error', '-i', path]
    if fps:
        cmd += ['-vf', f'fps={fps}']
    cmd += [f'{outdir}/f%05d.png']
    subprocess.run(cmd, check=True)
    n = len(glob.glob(f"{outdir}/f*.png"))
    print(f"decoded {n} frame(s) from {path.split('/')[-1]}")
    return f"{outdir}/f*.png"


def selftest(frame):
    """Prove the tool recovers a ratio it is TOLD, before believing one it finds.

    Two audits, and the second is the one that matters:

      rigid       shift the whole frame by a known dx; both channels must
                  return exactly that, so the ratio is 1.000 by construction
      differential  move the HUD and the background by DIFFERENT known amounts
                  and check the reported ratio is A/B

    The rigid test alone is not enough and nearly let a real question go
    unanswered: it cannot see a bias that only appears when the two channels
    disagree, which is the only case this tool is ever used in. `grey` is the
    whole frame and `hud` is a couple of hundred sparse pixels, and nothing had
    shown that asymmetry harmless until this ran.
    """
    a = decode(frame)
    m = (channels(a)['hud'] > 0)[..., None]
    hud_layer, bg_layer = a * m, a * (~m)
    bad = 0
    for A, B in ((10, 10), (9, 10), (8, 10), (18, 20), (27, 30)):
        b = np.roll(bg_layer, B, axis=1) + np.roll(hud_layer, A, axis=1)
        ca, cb = channels(a), channels(b)
        sh = shift(ca['hud'], cb['hud'], -60, 60)
        sg = shift(ca['grey'], cb['grey'], -60, 60)
        if sh is None or sg is None or not sg[0]:
            print(f"  hud={A} world={B}: NO SIGNAL")
            bad += 1
            continue
        got, want = sh[0] / sg[0], A / B
        ok = abs(got - want) < 0.06
        bad += not ok
        print(f"  hud={A:3d} world={B:3d}  want {want:.2f}  got {got:.3f}"
              f"   {'ok' if ok else 'BIAS'}")
    print("selftest PASSED" if not bad else f"selftest FAILED ({bad})")
    return 0 if not bad else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern', help='glob for a dump series, or a video file')
    ap.add_argument('--log', help='run log, for X4 camera yaw')
    ap.add_argument('--range', type=int, default=400,
                    help='search half-width in px (default 400)')
    ap.add_argument('--min-grey', type=float, default=8.0,
                    help='px of world motion below which a pair says nothing')
    ap.add_argument('--min-margin', type=float, default=0.05,
                    help='correlation peak must beat its rivals by this '
                         '(default 0.05; take 237 static pairs read 0.34-0.52 '
                         'and its scene changes 0.000-0.009)')
    ap.add_argument('--frames-dir', default='/tmp/x4vr-clip',
                    help='where to decode a video (default /tmp/x4vr-clip)')
    ap.add_argument('--fps', type=float,
                    help='resample the video to this rate before analysing')
    ap.add_argument('--reference', default='grey',
                    choices=('grey', 'cockpit'),
                    help="denominator channel. 'grey' is the whole scene "
                         "(cockpit AND a starfield at infinity, which move "
                         "alike under rotation and differently under "
                         "translation). 'cockpit' is the reference #57 is "
                         "actually posed about.")
    ap.add_argument('--control', action='store_true',
                    help='MATCHED negative control: ratio of one half of the '
                         'cockpit against the other. Same rigid object, so it '
                         'MUST read 1.00 on the same clip with the same '
                         'motion. Any departure is this clip\'s floor.')
    ap.add_argument('--selftest', action='store_true',
                    help='audit the tool on synthetic shifts of `pattern`'
                         ' (a single frame) and exit')
    a = ap.parse_args()

    if a.selftest:
        return selftest(sorted(glob.glob(a.pattern))[0])

    # A video argument re-tunes the search: at 30 fps the per-frame motion is a
    # few px, so a +-400 px window is mostly noise to be maximised over, and an
    # 8 px "did the world move" floor would discard almost every frame. Both
    # defaults were sized for dumps 4 s apart.
    if a.pattern.lower().endswith(('.mp4', '.mkv', '.webm', '.mov', '.avi')):
        if ap.get_default('range') == a.range:
            a.range = 60
        if ap.get_default('min_grey') == a.min_grey:
            a.min_grey = 2.0
        a.pattern = frames_from_video(a.pattern, a.frames_dir, a.fps)
        print(f"video mode: search +-{a.range} px, world floor "
              f"{a.min_grey:.0f} px")

    files = sorted(glob.glob(a.pattern),
                   key=lambda p: [int(t) if t.isdigit() else t
                                  for t in re.split(r'(\d+)', p)])
    if len(files) < 2:
        print(f"need at least 2 dumps, matched {len(files)}", file=sys.stderr)
        return 2

    # Half the image: these are SBS composites, and correlating across the
    # seam would measure the split, not the content.
    #
    # **CONSECUTIVE pairs, not one global reference, and the first draft got
    # this wrong.** A run's dumps are not one continuous scene: black start-up,
    # menu, load screens, then the cockpit. Correlating everything against a
    # single early frame returns "no signal" for the whole series and reads
    # exactly like a broken tool — which is what take 237's set did. Adjacent
    # frames are almost always the same scene, and the camera motion BETWEEN
    # two dumps is the quantity the ratio needs anyway.
    #
    # A pair where either frame has no cockpit is skipped and the next pair is
    # still usable, so one load screen no longer destroys the run.
    prev = None
    prev_name = None
    # NUM/DEN are channel names, so the control reuses this loop untouched
    # rather than getting a parallel copy that could drift from it.
    NUM, DEN = ('cockpit_a', 'cockpit_b') if a.control else ('hud', a.reference)
    print(f"{'pair':<40} {'dx_'+NUM:>10} {'dx_'+DEN:>10} {'ratio':>7} "
          f"{'m_num':>6} {'m_den':>6}  verdict")
    ratios = []
    by_axis = {}
    for i, f in enumerate(files):
        ch = channels(decode(f))
        # **Halve ONLY a side-by-side composite.** The layer's present dumps
        # are written one file per array layer and are already a single eye —
        # 1408x1408 — and halving those was what made every pair read "no
        # signal" on the first run of this tool. Aspect is the honest test:
        # an SBS composite is 2:1, an eye is square.
        hh, ww = ch['hud'].shape
        if ww >= 1.8 * hh:
            ch = {k: v[:, :ww // 2] for k, v in ch.items()}
        name = f.split('/')[-1].replace('-layer0.png', '').replace(
            'x4vr-', '')
        gs, hs = float(ch[DEN].std()), float(ch[NUM].std())
        if gs < 0.01 or hs < 1e-4:
            prev, prev_name = None, None    # scene break: do not bridge it
            continue
        if prev is None:
            prev, prev_name = ch, name
            continue
        pair = f"{prev_name} -> {name}"
        sh = shift(prev[NUM], ch[NUM], -a.range, a.range)
        sg = shift(prev[DEN], ch[DEN], -a.range, a.range)
        vh = shift_v(prev[NUM], ch[NUM], -a.range, a.range)
        vg = shift_v(prev[DEN], ch[DEN], -a.range, a.range)
        prev, prev_name = ch, name
        f = pair
        # Each axis is scored on its OWN motion. A yaw-only pass has no
        # vertical signal and its dy ratio is noise; folding the two into one
        # median is how the first version buried the pitch answer.
        for axis, s_, g_ in (('yaw', sh, sg), ('pitch', vh, vg)):
            if s_ is None or g_ is None:
                continue
            dh, _, mh_ = s_
            dg, _, mg_ = g_
            if abs(dg) < a.min_grey or mh_ < a.min_margin or mg_ < a.min_margin:
                continue
            by_axis.setdefault(axis, []).append(dh / dg)
        if sh is None or sg is None:
            print(f"{f:<40} {'—':>8} {'—':>8}   no signal")
            continue
        dxh, _, mh = sh
        dxg, _, mg = sg
        if abs(dxg) < a.min_grey:
            print(f"{f:<40} {dxh:>8} {dxg:>8} {'—':>7} "
                  f"{mh:>6.3f} {mg:>6.3f}  UNUSABLE: world barely moved")
            continue
        if mh < 0 or mg < 0:
            print(f"{f:<40} {dxh:>8} {dxg:>8} {'—':>7} "
                  f"{mh:>6.3f} {mg:>6.3f}  PEGGED at the search bound")
            continue
        # **The margin gate, which #55 spelled out and my first draft still
        # omitted.** On take 237's control set, the static pairs come back with
        # margins of 0.34-0.52 and the scene-change pairs with 0.000-0.009 —
        # and those four junk pairs were being folded into a headline median of
        # -0.65, a confident number assembled entirely from noise. A peak that
        # beats its rivals by nothing is aperture ambiguity, not a shift.
        if min(mh, mg) < a.min_margin:
            print(f"{f:<40} {dxh:>8} {dxg:>8} {'—':>7} "
                  f"{mh:>6.3f} {mg:>6.3f}  AMBIGUOUS: no correlation peak "
                  f"(scene change?)")
            continue
        r = dxh / dxg
        ratios.append(r)
        v = ('world-anchored' if 0.75 <= r <= 1.25 else
             'screen-locked' if abs(r) < 0.25 else
             'DOUBLE counter-rotation' if 1.6 <= r <= 2.4 else
             'neither — read the number')
        print(f"{f:<40} {dxh:>8} {dxg:>8} {r:>7.2f} "
              f"{mh:>6.3f} {mg:>6.3f}  {v}")

    print()
    if not ratios:
        print("NO USABLE PAIR. Every dump had the world within "
              f"{a.min_grey:.0f} px of the reference, so the camera did not "
              "turn between captures and this series cannot answer the "
              "question. That is a fact about the run, not about the HUD.")
    else:
        m = float(np.median(ratios))
        print(f"median ratio over {len(ratios)} usable pair(s): {m:.2f}")
        for axis in ('yaw', 'pitch'):
            v = by_axis.get(axis, [])
            if v:
                print(f"  {axis:<6} {float(np.median(v)):.2f} "
                      f"over {len(v)} pair(s)")
            else:
                print(f"  {axis:<6} no pair had enough motion on this axis")
        print("  ~1 world-anchored | ~0 screen-locked | ~2 doubled by X4")

    if a.log:
        cam = camera_series(a.log)
        if cam:
            ys = [y for _, y in cam]
            print(f"log: X4 camera yaw over {len(cam)} readback(s), "
                  f"{min(ys):+.2f} to {max(ys):+.2f} deg")
            if max(ys) - min(ys) < 5.0:
                print("  X4's camera barely moved in this run — expect every "
                      "pair above to be UNUSABLE, and do not read the dumps "
                      "as evidence about the HUD")
        else:
            print("log: no camread lines — was X4VR_CAMREAD=1 set?")
    return 0


if __name__ == '__main__':
    sys.exit(main())
