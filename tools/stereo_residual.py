#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Find surfaces the two eyes light differently, on a pair of probe dumps.

Two metrics have already failed at this question, and both failed the same way.

The layer's probe reports `l1/l0`, the ratio of whole-frame means. Takes 56-66
were scored on it. It cannot work: two *correctly* offset views of an
asymmetric scene have different whole-frame means, because the shift moves
bright content in at one edge and dark out at the other.

The first version of this tool then aligned each tile and compared brightness
-- and reported a median. Take 67's frame has a wing whose top surface is
**2.4x brighter in the right eye**, and the median said 1.000, because the
effect covers a few percent of the frame and a median is exactly the statistic
that discards it. Worse, it selected tiles by absolute residual, which drops a
differently-lit tile from the average meant to detect it.

So this version:

  - matches by **normalized cross-correlation**, which is invariant to an
    affine intensity change, so a surface lit twice as brightly still finds its
    own disparity instead of being discarded;
  - **propagates** disparity from confident neighbours into tiles that lost
    correlation, because a washed-out surface loses texture as well as
    brightness, and it sits next to tiles that did match;
  - reports the **tail and the total area**, never a bare median, and names the
    worst offenders with coordinates so they can be cropped and looked at.

Usage:

    tools/stereo_residual.py <layer0.ppm> <layer1.ppm> [--tile 64] [--max 400]
    tools/stereo_residual.py <layer0.ppm> <layer1.ppm> --png out-prefix

Inputs are what `X4VR_MV_DUMP=<prefix> X4VR_MV_DUMP_IMG=<serial>` writes:
`<prefix>-img<serial>-n<seq>-layer{0,1}.ppm`, 8-bit binary P6.

numpy only; `--png` also needs Pillow and writes a ratio map.
"""

import argparse
import sys

import numpy as np

# A tile counts as differently lit past this ratio. 1.25 is well clear of the
# few-percent spread that correct stereo shows at occlusion boundaries, and far
# under the 2.4x that take 67 found, so it is not tuned to catch one frame.
RATIO_FLAG = 1.25


def load_ppm(path):
    """Read a binary P6 PPM. Comments would shift the pixel offset, so they are
    parsed rather than assumed absent."""
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


def ncc(u, v):
    u = u - u.mean()
    v = v - v.mean()
    d = np.sqrt((u * u).sum() * (v * v).sum())
    return float((u * v).sum() / d) if d > 1e-6 else 0.0


def disparity_field(a, b, tile, maxshift):
    """Best horizontal shift per tile, by normalized cross-correlation.

    NCC rather than absolute difference: NCC is invariant to an affine
    intensity change, so a surface one eye lights twice as brightly still
    correlates with itself and gets a correct shift, instead of being thrown
    out -- which is precisely the tile this tool exists to find.

    Tiles are searched over the whole frame. The previous version skipped
    everything within `maxshift` of either edge, which on a 1408-wide image
    with a 400px search left only the middle 39% under examination.
    """
    h, w = a.shape
    ny, nx = h // tile, w // tile
    sh = np.zeros((ny, nx), np.float32)
    cc = np.full((ny, nx), -2.0, np.float32)
    for iy in range(ny):
        for ix in range(nx):
            y, x = iy * tile, ix * tile
            ref = b[y : y + tile, x : x + tile]
            if ref.std() < 3:
                continue  # flat: NCC is noise here, let a neighbour decide
            for s in range(-maxshift, maxshift + 1, 2):
                xs = x - s
                if xs < 0 or xs + tile > w:
                    continue
                c = ncc(a[y : y + tile, xs : xs + tile], ref)
                if c > cc[iy, ix]:
                    cc[iy, ix], sh[iy, ix] = c, s
    return sh, cc


def propagate(sh, cc, good=0.7):
    """Fill unconfident tiles from confident neighbours.

    A surface that is blown out in one eye loses texture as well as gaining
    brightness, so it can lose correlation too. Its disparity is still that of
    the surface around it. Dropping such tiles is how the first version of this
    tool reported "no difference" on a frame containing a 2.4x one.
    """
    ny, nx = sh.shape
    out = sh.copy()
    rel = cc >= good
    if not rel.any():
        return out, rel
    gmed = float(np.median(sh[rel]))
    for iy in range(ny):
        for ix in range(nx):
            if rel[iy, ix]:
                continue
            ys = slice(max(0, iy - 1), iy + 2)
            xs = slice(max(0, ix - 1), ix + 2)
            near = sh[ys, xs][rel[ys, xs]]
            out[iy, ix] = float(np.median(near)) if near.size else gmed
    return out, rel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layer0")
    ap.add_argument("layer1")
    ap.add_argument("--tile", type=int, default=64)
    ap.add_argument("--max", type=int, default=400, dest="maxshift")
    ap.add_argument("--min-level", type=float, default=8.0,
                    help="ignore tiles darker than this in both eyes")
    ap.add_argument("--png", metavar="PREFIX", help="write a ratio map")
    args = ap.parse_args()

    a = load_ppm(args.layer0).mean(axis=2)
    b = load_ppm(args.layer1).mean(axis=2)
    if a.shape != b.shape:
        sys.exit(f"size mismatch: {a.shape} vs {b.shape}")
    h, w = a.shape
    T = args.tile

    sh, cc = disparity_field(a, b, T, args.maxshift)
    shp, rel = propagate(sh, cc)

    ratio = np.full(sh.shape, np.nan, np.float32)
    lvl = np.zeros(sh.shape, np.float32)
    for iy in range(sh.shape[0]):
        for ix in range(sh.shape[1]):
            y, x = iy * T, ix * T
            s = int(shp[iy, ix])
            xs = min(max(x - s, 0), w - T)
            m0 = a[y : y + T, xs : xs + T].mean()
            m1 = b[y : y + T, x : x + T].mean()
            lvl[iy, ix] = max(m0, m1)
            if max(m0, m1) >= args.min_level:
                ratio[iy, ix] = m1 / max(m0, 1e-6)

    ok = ~np.isnan(ratio)
    if not ok.any():
        sys.exit("no tile bright enough to judge -- lower --min-level")
    r = ratio[ok]
    print(f"tiles          {int(ok.sum())} judged of {sh.size} "
          f"({T}px, search +-{args.maxshift}px, whole frame)")
    print(f"               {int(rel.sum())} matched confidently (NCC>=0.7), "
          f"the rest took a neighbour's disparity")
    print(f"whole-frame    l1/l0 = {b.mean() / max(a.mean(), 1e-9):.4f}   "
          "<- the probe's number; not a verdict")
    print(f"disparity      p5/p50/p95 = {np.percentile(shp, 5):.0f}/"
          f"{np.percentile(shp, 50):.0f}/{np.percentile(shp, 95):.0f} px")
    print(f"tile ratio     p1/p50/p99 = {np.percentile(r, 1):.3f}/"
          f"{np.percentile(r, 50):.3f}/{np.percentile(r, 99):.3f}")

    off = (ratio > RATIO_FLAG) | (ratio < 1.0 / RATIO_FLAG)
    bad = ok & off
    # The verdict rests on this narrower set: tiles whose *structure* matched
    # confidently at the chosen shift. A tile can be bright in one eye and dark
    # in the other because the other eye sees past a silhouette -- that is
    # occlusion, and it is correct. It cannot also correlate at 0.7, because
    # there would be nothing there to correlate with. So a confident match plus
    # a brightness mismatch is the same surface lit two ways, and nothing else.
    sure = bad & rel
    frac = 100.0 * bad.sum() / max(ok.sum(), 1)
    fsure = 100.0 * sure.sum() / max(ok.sum(), 1)
    print(f"flagged        {int(bad.sum())} tiles ({frac:.1f}% of judged) "
          f"differ by more than {RATIO_FLAG:g}x")
    print(f"  of those     {int(sure.sum())} ({fsure:.1f}% of judged) also "
          "matched confidently -- same surface, cannot be occlusion")

    if bad.any():
        idx = np.argsort(np.where(sure, -np.abs(np.nan_to_num(ratio) - 1), 0).ravel())
        print("worst offenders (crop these and look):")
        for k in idx[: min(8, int(bad.sum()))]:
            iy, ix = divmod(int(k), sh.shape[1])
            print(f"   x={ix * T:4d}-{ix * T + T - 1:4d} y={iy * T:4d}-"
                  f"{iy * T + T - 1:4d}   shift={int(shp[iy, ix]):+4d}px  "
                  f"NCC={cc[iy, ix]:+.2f}  L1/L0={ratio[iy, ix]:.2f}")

    # The verdict is area-based on purpose. A median cannot see an effect that
    # covers a few percent of the frame, and a few percent of the frame is
    # exactly what a wrongly-lit surface looks like.
    if fsure >= 2.0:
        print(f"\nVERDICT  {fsure:.1f}% of judged tiles are lit differently in "
              "the two eyes.\n         Same surface, same place, different "
              "brightness -- this is a shading\n         defect, not parallax.")
    else:
        print("\nVERDICT  no meaningful area is lit differently; what differs "
              "between the\n         eyes is where things are, not how they "
              "are lit.")

    if args.png:
        from PIL import Image

        big = np.repeat(np.repeat(np.nan_to_num(ratio, nan=1.0), T, 0), T, 1)
        rgb = np.zeros((h, w, 3), np.uint8)
        rgb[..., 1] = np.clip((big - 1.0) * 255, 0, 255)  # green: right brighter
        rgb[..., 0] = np.clip((1.0 / np.maximum(big, 1e-6) - 1.0) * 255, 0, 255)
        Image.fromarray(rgb).save(f"{args.png}-ratio.png")
        print(f"\nwrote {args.png}-ratio.png  "
              "(green = right eye brighter, red = left eye brighter)")


if __name__ == "__main__":
    main()
