#!/usr/bin/env python3
"""Did the UI move by the canvas disparity, and did the run without one not?

Usage:
    tools/canvas_shift_map.py <canvas-prefix> <expected_px> [control-prefix] [frames...]

    tools/canvas_shift_map.py /tmp/x4vr-t98-present -30 /tmp/x4vr-t97-present

A prefix is everything before `-nN-layerL.ppm`. `expected_px` is TWICE the
per-eye shift the log reports, because layer 0 was given +s and layer 1 -s.

Tests P100: with X4VR_CANVAS_M set, the UI moves as a rigid whole and the run
without a canvas does not.

The measurement is blockwise and reports the whole distribution, never a mean.
Two of this project's metrics have already reported "no defect" on frames that
were visibly wrong -- a 2.4x per-eye shading error survived two aggregates --
and a mean over this frame would be the same mistake a third time: outside the
map the UI is a small fraction of the pixels, so the correct answer and total
failure have nearly the same average.

**The prediction this file first carried was wrong, and take 97 refuted it.**
It said a healthy frame shows a population at 0 (the world at space distances),
a population at the expected shift (the UI), and "nothing in between", on the
grounds that intermediate shifts would mean the UI had been sheared with depth
instead of translated. Take 97 -- the control, with no canvas at all and
therefore nothing that could have been sheared -- shows 29 of 64 blocks at
intermediate shifts. That population is the *world's own parallax*: near
geometry legitimately occupies every shift between 0 and the canvas distance,
and in a cockpit frame it outnumbers the UI. The first verdict rule read it as
a defect and returned SHEARED for a run that was working perfectly.

So the control is not optional and not merely good practice here -- it is the
only thing that makes any of these numbers mean anything, and the tool now
refuses to give a verdict without it. Wrong turn kept rather than edited away,
because it is the same shape as the aggregates that hid a real defect twice:
a rule that looks principled, applied to a distribution nobody had measured.
"""
import sys

import numpy as np

BLOCK = 64
# A block has to carry some structure before its best match means anything.
# Empty space matches everything equally well at every shift, and those blocks
# would otherwise pile into whichever bin the search happens to visit first.
MIN_STD = 4.0
# How confident a match must be to be counted at all.
MIN_NCC = 0.90


def load_ppm(path):
    """Read a binary P6 PPM. Comments would shift the pixel offset, so they are
    parsed rather than assumed absent."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM (P6)")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval}, only 8-bit is written")
    px = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos + 1)
    return px.reshape(h, w, 3).astype(np.float32)


def gray(img):
    return img @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


def ncc(u, v):
    u = u - u.mean()
    v = v - v.mean()
    d = float(np.sqrt((u * u).sum() * (v * v).sum()))
    return float((u * v).sum() / d) if d > 1e-6 else -1.0


def best_shift(ref, tgt, x0, y0, lo, hi):
    """The x displacement that best takes ref's block to tgt, and how sure."""
    patch = ref[y0:y0 + BLOCK, x0:x0 + BLOCK]
    best, best_c = 0, -1.0
    for dx in range(lo, hi + 1):
        sx = x0 + dx
        if sx < 0 or sx + BLOCK > tgt.shape[1]:
            continue
        c = ncc(patch, tgt[y0:y0 + BLOCK, sx:sx + BLOCK])
        if c > best_c:
            best, best_c = dx, c
    return best, best_c


def frame_shifts(prefix, n, span):
    """Per-block x displacements between the two layers of one dumped frame,
    or None if that frame was not dumped."""
    try:
        a = gray(load_ppm(f"{prefix}-n{n}-layer0.ppm"))
        b = gray(load_ppm(f"{prefix}-n{n}-layer1.ppm"))
    except (OSError, ValueError):
        return None
    if a.shape != b.shape:
        return None
    h, w = a.shape
    out = []
    for y0 in range(0, h - BLOCK + 1, BLOCK):
        for x0 in range(span, w - BLOCK - span + 1, BLOCK):
            if a[y0:y0 + BLOCK, x0:x0 + BLOCK].std() < MIN_STD:
                continue
            dx, c = best_shift(a, b, x0, y0, -span, span)
            if c >= MIN_NCC:
                out.append(dx)
    return np.array(out)


def survey(prefix, frames, expected, span, label):
    """The at-expected population of each frame of one run."""
    print(f"\n{label}  ({prefix})")
    print(f"  {'frame':>7s} {'blocks':>7s} {'at 0':>6s} "
          f"{'at ' + format(expected, '+.0f'):>8s} {'other':>6s} {'peak':>7s}")
    peak = 0.0
    for n in frames:
        d = frame_shifts(prefix, n, span)
        if d is None or not len(d):
            print(f"  {n:>7d}  (not dumped, or nothing matched)")
            continue
        z = int((np.abs(d) <= 1).sum())
        e = int((np.abs(d - expected) <= 1).sum())
        frac = e / len(d)
        peak = max(peak, frac)
        print(f"  {n:>7d} {len(d):7d} {z:6d} {e:8d} {len(d) - z - e:6d} "
              f"{frac:6.1%}")
    return peak


# How large the at-expected population has to get, in the best frame, before
# it is a population rather than a few blocks of near geometry that happen to
# sit at the canvas distance. And how small it must stay in the control.
#
# Both are read off the takes that motivated them and are an order of magnitude
# apart, so neither is a threshold the data was squeezed through: take 98 peaks
# at 99.6% (the map, which is drawn almost entirely by the UI pass) and take 97
# at 3.7%.
CANVAS_MIN = 0.15
CONTROL_MAX = 0.05


def main(argv):
    # The first version of this tool judged a single frame pair, on the rule
    # "more blocks at intermediate shifts than at the expected one means the UI
    # was sheared rather than translated". It reported SHEARED for take 97 --
    # the control, which had no canvas at all and could not have sheared
    # anything. The intermediate population is the world's own parallax: near
    # geometry legitimately occupies every shift between 0 and the canvas
    # distance, and in a cockpit frame it outnumbers the UI.
    #
    # So a verdict needs the control, not a cleverer threshold. The tool now
    # surveys frames from both runs and judges on the contrast, which is the
    # comparison the experiment was designed around and which the tool
    # previously threw away.
    if len(argv) < 3:
        print(__doc__)
        return 2
    canvas_prefix = argv[1]
    expected = float(argv[2])
    control_prefix = argv[3] if len(argv) > 3 else None
    frames = [int(x) for x in argv[4:]] or [150, 180, 210, 240]
    # Wider than the expected shift, so "did not move" and "moved somewhere
    # else" cannot both come back as "not found".
    span = max(64, int(abs(expected) * 3))

    peak = survey(canvas_prefix, frames, expected, span, "CANVAS run")
    if control_prefix is None:
        print("\nNo control given, so this is a description and not a verdict. "
              "Pass the control run's prefix as the third argument -- a "
              "population at the expected shift means nothing until the same "
              "measurement on a run without a canvas is shown not to have one.")
        return 0
    cpeak = survey(control_prefix, frames, expected, span, "CONTROL run")

    print(f"\npeak at {expected:+.0f} px: canvas {peak:.1%}, "
          f"control {cpeak:.1%}")
    print()
    if cpeak > CONTROL_MAX:
        print("VERDICT: NOT ATTRIBUTABLE. The control also has a population at "
              "the canvas distance, so this shift is something the frame does "
              "on its own and the canvas cannot be credited with it.")
        return 1
    if peak < CANVAS_MIN:
        print("VERDICT: NO CANVAS. Nothing moved by the expected amount. If "
              "the log also says variants were built and swapped, the shift "
              "reached the modules but not the pixels.")
        return 1
    print("VERDICT: CANVAS. A population at the expected shift that the "
          "control does not have: the UI translated as a rigid whole, by the "
          "distance asked for, and the run without a canvas did not.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
