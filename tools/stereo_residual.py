#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Tell correct stereo apart from broken stereo, on a pair of probe dumps.

The layer's probe reports `l1/l0`, the ratio of whole-frame means. Takes 56
through 66 were scored on that number, and take 66 showed it cannot do the job:
two *correctly* offset views of the same scene have different whole-frame means
whenever the scene is left-right asymmetric, because the shift moves bright
content in at one edge and dark content out at the other. `l1/l0 = 1.846` on
X4's cockpit is what correct stereo looks like there, not a defect.

What separates the two cases is whether the eyes *shade* the same surface the
same way. So: find each tile's horizontal disparity by search, then compare
brightness after compensating for it.

  - Correct stereo -> aligned tiles match to within ~1%, and the leftover
    residual sits on occlusion edges, where one eye sees behind a silhouette
    the other does not. Nothing can align that; it is the parallax itself.
  - A shading defect -> aligned tiles still differ, and the residual fills
    interiors rather than tracing outlines.

Usage:

    tools/stereo_residual.py <layer0.ppm> <layer1.ppm> [--tile 64] [--max 400]
    tools/stereo_residual.py --png out-prefix <layer0.ppm> <layer1.ppm>

The inputs are what `X4VR_MV_DUMP=<prefix> X4VR_MV_DUMP_IMG=<serial>` writes:
`<prefix>-img<serial>-n<seq>-layer{0,1}.ppm`, 8-bit binary P6.

Only numpy is required. `--png` additionally needs Pillow, and writes the
residual map -- worth looking at, because "edges" and "interiors" is a
distinction the eye makes instantly and a scalar makes badly.
"""

import argparse
import sys

import numpy as np


def load_ppm(path):
    """Read a binary P6 PPM. The header is whitespace-delimited, three fields
    before the maxval, and the dumps carry no comments -- but a comment would
    silently shift the pixel offset, so reject anything that is not P6."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM (P6)")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while pos < len(data) and data[pos : pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    w, h, maxval = fields
    if maxval != 255:
        raise ValueError(f"{path}: maxval {maxval}, only 8-bit is written")
    px = np.frombuffer(data, dtype=np.uint8, count=w * h * 3, offset=pos + 1)
    return px.reshape(h, w, 3).astype(np.float32)


def analyse(a, b, tile, maxshift):
    """Per-tile horizontal disparity search, and brightness after compensation.

    Tiles within `maxshift` of either edge are skipped rather than clamped: a
    tile whose true match lies outside the frame would otherwise report the
    best *available* shift, which is a number with no meaning attached to it.
    """
    h, w = a.shape
    out = []
    for y in range(0, h - tile + 1, tile):
        for x in range(maxshift, w - tile - maxshift + 1, tile):
            ref = b[y : y + tile, x : x + tile]
            if ref.max() < 2 and a[y : y + tile, x : x + tile].max() < 2:
                continue  # both empty: any shift matches, so it votes on nothing
            best_s, best_r = 0, float("inf")
            for s in range(-maxshift, maxshift + 1, 2):
                r = np.abs(a[y : y + tile, x - s : x - s + tile] - ref).mean()
                if r < best_r:
                    best_s, best_r = s, r
            moved = a[y : y + tile, x - best_s : x - best_s + tile]
            # The non-circular test. Selecting tiles by low residual and then
            # reporting that they match in brightness is close to a tautology:
            # a tile that differs photometrically keeps a high residual and is
            # dropped from the very average meant to detect it. So fit a gain
            # too, and ask whether it *buys* anything. If one eye were darker,
            # scaling would collapse the residual. If the residual is
            # structural -- parallax, occlusion -- a gain cannot help and
            # usually hurts, because it distorts the parts that already agree.
            gain = float(np.sum(moved * ref) / max(np.sum(moved * moved), 1e-9))
            r_gain = np.abs(moved * gain - ref).mean()
            out.append(
                (
                    best_s,
                    moved.mean(),
                    ref.mean(),
                    best_r,
                    np.abs(a[y : y + tile, x : x + tile] - ref).mean(),
                    gain,
                    r_gain,
                )
            )
    return out


def residual_map(a, b, tile, maxshift, step=3):
    h, w = a.shape
    res = np.zeros((h, w), np.float32)
    for y in range(0, h - tile + 1, tile):
        for x in range(0, w - tile + 1, tile):
            ref = b[y : y + tile, x : x + tile]
            best = float("inf")
            for s in range(-maxshift, maxshift + 1, step):
                xs = x - s
                if xs < 0 or xs + tile > w:
                    continue
                r = np.abs(a[y : y + tile, xs : xs + tile] - ref).mean()
                best = min(best, r)
            res[y : y + tile, x : x + tile] = best
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layer0")
    ap.add_argument("layer1")
    ap.add_argument("--tile", type=int, default=64)
    ap.add_argument("--max", type=int, default=400, dest="maxshift")
    ap.add_argument("--png", metavar="PREFIX", help="also write a residual map")
    args = ap.parse_args()

    a = load_ppm(args.layer0).mean(axis=2)
    b = load_ppm(args.layer1).mean(axis=2)
    if a.shape != b.shape:
        sys.exit(f"size mismatch: {a.shape} vs {b.shape}")

    rows = analyse(a, b, args.tile, args.maxshift)
    if not rows:
        sys.exit("no tiles with content -- both layers look empty")
    sh = np.array([r[0] for r in rows], np.float32)
    m0 = np.array([r[1] for r in rows], np.float32)
    m1 = np.array([r[2] for r in rows], np.float32)
    res = np.array([r[3] for r in rows], np.float32)
    raw = np.array([r[4] for r in rows], np.float32)

    print(f"tiles          {len(rows)} of {a.shape[0]}x{a.shape[1]}, "
          f"{args.tile}px, search +-{args.maxshift}px")
    print(f"whole-frame    l1/l0 = {b.mean() / max(a.mean(), 1e-9):.4f}   "
          "<- the probe's number; not a verdict")
    print(f"disparity      p5/p50/p95 = "
          f"{np.percentile(sh, 5):.0f}/{np.percentile(sh, 50):.0f}/"
          f"{np.percentile(sh, 95):.0f} px, "
          f"{int(np.sum(np.abs(sh) >= args.maxshift - 2))} at the search bound")
    print(f"residual       {raw.mean():.2f} unaligned -> {res.mean():.2f} "
          f"aligned  ({100 * (1 - res.mean() / max(raw.mean(), 1e-9)):.1f}% "
          "explained by displacement)")
    for thr in (1.0, 2.0, 4.0):
        keep = res < thr
        if keep.sum() == 0:
            continue
        ratio = np.sum(m1[keep]) / max(np.sum(m0[keep]), 1e-9)
        print(f"  residual<{thr:<4g} {int(keep.sum()):4d} tiles   "
              f"aligned l1/l0 = {ratio:.4f}")

    gain = np.array([r[5] for r in rows], np.float32)
    rgain = np.array([r[6] for r in rows], np.float32)
    bought = 1.0 - rgain / np.maximum(res, 1e-9)
    print(f"gain test      per-tile gain p10/p50/p90 = "
          f"{np.percentile(gain, 10):.3f}/{np.percentile(gain, 50):.3f}/"
          f"{np.percentile(gain, 90):.3f}")
    print(f"               fitting a gain changes the residual by "
          f"{100 * np.median(bought):+.1f}% (median)")

    # The verdict rests on the gain test, not on the aligned-brightness average
    # above: that average is computed over tiles chosen for agreeing, and would
    # report agreement almost regardless.
    photometric = np.median(bought) > 0.15 and abs(np.median(gain) - 1.0) > 0.05
    if photometric:
        print("\nVERDICT  a per-tile gain absorbs much of the residual -- the "
              "eyes shade\n         the same surface differently. Look at the "
              "lighting.")
    else:
        print("\nVERDICT  a per-tile gain buys nothing, so the residual is "
              "structural, not\n         photometric: the eyes shade the same "
              "surface the same way and the\n         whole-frame ratio is "
              "displacement. This is correct stereo.")

    if args.png:
        from PIL import Image

        rm = residual_map(a, b, 32, min(args.maxshift, 300))
        Image.fromarray(np.clip(rm * 8, 0, 255).astype(np.uint8)).save(
            f"{args.png}-residual.png"
        )
        print(f"\nwrote {args.png}-residual.png -- outlines mean parallax, "
              "filled areas mean shading")


if __name__ == "__main__":
    main()
