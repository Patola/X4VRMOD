#!/usr/bin/env python3
"""Did the UI move by the canvas disparity, and did nothing else move?

Usage:  tools/canvas_shift_map.py <layer0.ppm> <layer1.ppm> [expected_px]

Written BEFORE the take that produces its input, so nothing here can be tuned
to the answer. It tests P100: with X4VR_CANVAS_M set, the UI moves as a rigid
whole and the rest of the frame does not.

The measurement is blockwise and reports the whole distribution, never a mean.
Two of this project's metrics have already reported "no defect" on frames that
were visibly wrong -- a 2.4x per-eye shading error survived two aggregates --
and a mean over this frame would be the same mistake a third time: the UI is a
small fraction of the pixels, so the correct answer (a large shift on a few
blocks, zero on most) and total failure (zero everywhere) have nearly the same
average.

What a healthy run reads, stated before any run has been scored:

    a peak at 0 px          the world. In space at default IPD the per-eye
                            offset is 30/z px, so anything past ~30 m is
                            sub-pixel and lands in the 0 bin.
    a peak at 2s px         the UI, where s is the per-eye NDC shift the log
                            reports. Between the two layers the UI moves by
                            TWICE the per-eye figure: layer 0 got +s and
                            layer 1 got -s.
    nothing in between      a spread of intermediate shifts would mean the UI
                            is being sheared with depth rather than
                            translated, i.e. it took K_world and not the
                            canvas.

The failure this is built to catch is the quiet one: every block reads 0,
which is a frame identical to the pre-canvas one and looks entirely correct.
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


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    a, b = gray(load_ppm(argv[1])), gray(load_ppm(argv[2]))
    if a.shape != b.shape:
        print(f"the two layers differ in size: {a.shape} vs {b.shape}")
        return 2
    expected = float(argv[3]) if len(argv) > 3 else None

    h, w = a.shape
    # Search wide enough to see a wrong answer, not just the right one. If the
    # UI took K_world instead of the canvas it could land anywhere, and a
    # search clamped to the expected value would report "not found" for both
    # "did not move" and "moved to the wrong place".
    span = max(64, int(abs(expected) * 3) if expected else 64)
    shifts, weak, flat = [], 0, 0
    for y0 in range(0, h - BLOCK + 1, BLOCK):
        for x0 in range(span, w - BLOCK - span + 1, BLOCK):
            if a[y0:y0 + BLOCK, x0:x0 + BLOCK].std() < MIN_STD:
                flat += 1
                continue
            dx, c = best_shift(a, b, x0, y0, -span, span)
            if c < MIN_NCC:
                weak += 1
                continue
            shifts.append((dx, c, x0, y0))

    print(f"{w}x{h}, {BLOCK}px blocks: {len(shifts)} matched, "
          f"{weak} below ncc {MIN_NCC}, {flat} too flat to match")
    if not shifts:
        print("VERDICT: nothing to measure -- was the frame still loading?")
        return 1

    dxs = np.array([d for d, _, _, _ in shifts])
    print("\nshift histogram (px, layer0 -> layer1):")
    for dx in sorted(set(dxs.tolist())):
        n = int((dxs == dx).sum())
        print(f"  {dx:+5d} px  {n:4d} block(s)  {'#' * min(n, 60)}")

    at_zero = int((np.abs(dxs) <= 1).sum())
    print(f"\n{at_zero}/{len(dxs)} blocks are within 1 px of zero "
          f"(the world at space distances)")

    if expected is None:
        print("\nNo expected shift given, so this is a description, not a "
              "verdict. Pass 2*s in pixels -- twice the per-eye figure the "
              "log reports, because layer 0 got +s and layer 1 got -s.")
        return 0

    # Tolerance is a pixel, not a percentage: the canvas is a constant offset,
    # so it either lands where the arithmetic says or the arithmetic is wrong.
    moved = np.abs(dxs - expected) <= 1.0
    n_moved = int(moved.sum())
    between = int(((np.abs(dxs) > 1) & (np.abs(dxs - expected) > 1)).sum())
    print(f"{n_moved}/{len(dxs)} blocks are within 1 px of the expected "
          f"{expected:+.1f} px (the canvas)")
    print(f"{between}/{len(dxs)} blocks are at neither -- these are the ones "
          f"to look at")

    print()
    if n_moved == 0:
        print("VERDICT: NO CANVAS. Nothing moved by the expected amount. If "
              "the log also says variants were built and swapped, the shift "
              "reached the modules but not the pixels.")
        return 1
    if between > n_moved:
        print("VERDICT: SHEARED, NOT TRANSLATED. More blocks sit at "
              "intermediate shifts than at the canvas distance, which is what "
              "a depth-scaled K looks like -- the UI took K_world.")
        return 1
    print("VERDICT: CANVAS. A population at the expected shift and a "
          "population at zero, with little between: the UI translated as a "
          "rigid whole and the world did not follow it.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
