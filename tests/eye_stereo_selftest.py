#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Controls for tools/eye_stereo.py, built from a real X4 frame.

The point of #32's metric is that it separates three cases the old layer1/layer0
ratio could not tell apart. So it is checked against all three, and the negative
control is checked FIRST -- a metric that cannot report "no stereo here" is the
one that produced the take 41-44 false passes.

The source frame is whatever PPM is passed on the command line (X4's own output
is never committed, so this takes a path rather than shipping a fixture); with
no argument it synthesises a textured frame instead, which exercises the same
code paths and keeps the test runnable on a machine with no dumps.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import numpy as np  # noqa: E402
import eye_stereo as es  # noqa: E402

FAIL = []


def check(name, cond, detail=""):
    print(f"  {'ok  ' if cond else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not cond:
        FAIL.append(name)


def synth(h=704, w=704, seed=7):
    rng = np.random.default_rng(seed)
    # Lowpass noise: correlates over a few pixels, like real geometry, unlike
    # white noise which correlates with nothing but itself at shift 0.
    a = rng.random((h // 8 + 2, w // 8 + 2)).astype(np.float32)
    a = np.repeat(np.repeat(a, 8, axis=0), 8, axis=1)[:h, :w]
    return a * 200.0


def shift_by(img, s):
    """img sampled at x-s, i.e. the content moves right by s."""
    return np.roll(img, s, axis=1)


def tile_shift_by(img, per_tile, tiles=es.TILES):
    """Each tile column displaced by its own amount — a depth-varying pair."""
    out = np.empty_like(img)
    tw = img.shape[1] // tiles
    for t in range(tiles):
        lo, hi = t * tw, (t + 1) * tw if t + 1 < tiles else img.shape[1]
        out[:, lo:hi] = np.roll(img, per_tile[t], axis=1)[:, lo:hi]
    return out


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else None
    if src:
        base = es.luma(es.read_ppm(src))
        print(f"source: {src} ({base.shape[1]}x{base.shape[0]}, "
              f"mean luma {base.mean():.2f})")
    else:
        base = synth()
        print("source: synthetic lowpass noise (no PPM given)")

    print("negative control — an identical pair must not read as stereo")
    s, e, r = es.tile_shifts(base, base.copy())
    live = e >= es.MIN_ENERGY
    check("identical pair aligns at 0 everywhere", bool((s[live] == 0).all()),
          f"shifts {s[live].min()}..{s[live].max()}")
    check("identical pair correlates perfectly", bool((r[live] > 0.999).all()),
          f"r_min={r[live].min():.5f}")
    check("identical pair has zero spread",
          int(s[live].max() - s[live].min()) == 0)

    print("uniform translation — must be measurable and must read as spread 0")
    for want in (-11, 5, 23):
        b = shift_by(base, want)
        s, e, r = es.tile_shifts(base, b)
        live = e >= es.MIN_ENERGY
        got = int(np.median(s[live]))
        check(f"recovers a {want:+d} px slide", got == want, f"got {got:+d}")
        check(f"a {want:+d} px slide has zero spread",
              int(s[live].max() - s[live].min()) == 0,
              f"{s[live].min():+d}..{s[live].max():+d}")

    print("depth-varying — the case the ratio could not see")
    per_tile = [0, 2, 4, 7, 11, 16, 22, 29]
    b = tile_shift_by(base, per_tile)
    s, e, r = es.tile_shifts(base, b)
    live = e >= es.MIN_ENERGY
    spread = int(s[live].max() - s[live].min())
    check("depth-varying pair has nonzero spread", spread > 0,
          f"spread={spread}")
    # Each tile column should recover its own displacement. Tiles straddling a
    # column boundary see two shifts at once, so this checks the column medians.
    okcols = 0
    for t in range(es.TILES):
        col = s[:, t][live[:, t]]
        if col.size and int(np.median(col)) == per_tile[t]:
            okcols += 1
    check("per-column shifts recovered", okcols >= es.TILES - 1,
          f"{okcols}/{es.TILES} columns exact")

    print("unrelated images — must not be reported as a deep scene")
    # Take 97 produced 31 frames whose tiles pinned at -40 and +40 at once, on
    # dark frames, and read as 80 px of parallax: geometry 0.75 m from the eye,
    # in space. That is the correlator failing and returning the edge of its
    # search window. It has to be distinguishable from an answer.
    other = synth(base.shape[0], base.shape[1], seed=99)
    s, e, r = es.tile_shifts(base, other)
    textured = e >= es.MIN_ENERGY
    sat = textured & (np.abs(s) == es.MAX_SHIFT)
    good = textured & ~sat & (r >= es.MIN_R)
    check("unrelated pair yields no confident tile", int(good.sum()) == 0,
          f"{int(good.sum())} of {int(textured.sum())} textured passed "
          f"r>={es.MIN_R}, {int(sat.sum())} saturated")

    print("guards")
    s, e, r = es.tile_shifts(np.zeros_like(base), np.zeros_like(base))
    check("a black frame yields no textured tile",
          not bool((e >= es.MIN_ENERGY).any()))

    print()
    if FAIL:
        print(f"FAIL  {len(FAIL)} check(s): {', '.join(FAIL)}")
        return 1
    print("ok    every control passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
