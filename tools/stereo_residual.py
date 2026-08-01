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


def disparity_field(a, b, tile, lo, hi):
    """Best horizontal shift per tile, by normalized cross-correlation,
    searched over `lo..hi` only.

    NCC rather than absolute difference: NCC is invariant to an affine
    intensity change, so a surface one eye lights twice as brightly still
    correlates with itself and gets a correct shift, instead of being thrown
    out -- which is precisely the tile this tool exists to find.

    Tiles are searched over the whole frame. An earlier version skipped
    everything within the search radius of either edge, which on a 1408-wide
    image with a 400px search left only the middle 39% under examination.

    The `lo..hi` window is what keeps NCC honest. A starship hull is a
    repeating texture, so a tile will happily correlate at 0.8 against a
    *different* panel 200px away. `main` derives the window from the geometry
    (see `plausible_window`) and every match outside it is a false one.
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
            for s in range(lo, hi + 1, 2):
                xs = x - s
                if xs < 0 or xs + tile > w:
                    continue
                c = ncc(a[y : y + tile, xs : xs + tile], ref)
                if c > cc[iy, ix]:
                    cc[iy, ix], sh[iy, ix] = c, s
    return sh, cc


def plausible_window(sh, cc, dmax, good=0.7):
    """The range of horizontal shifts the stereo geometry can actually produce.

    X4's projection is reverse-Z infinite-far, so screen disparity is
    `W/2 * sx * d / z_v` px -- monotonic in depth, bounded by the near plane,
    and **the same sign for every tile in the frame**. With the layer's logged
    `ipd=0.064 sx=1.3333 near=0.1` at 1408px that is 30.04/z_v px, so 0 at
    infinity and 300px at the near plane, never negative and never positive,
    only one of the two.

    Which one depends on which array layer is the left eye, so rather than
    assume it, take the sign from the confidently-matched tiles.

    NOT from their median. This is a space game: most of the frame is stars and
    nebula at infinity, where the disparity genuinely IS zero, so the median is
    zero, the window collapses to {0}, and the search switches itself off --
    which is exactly what it did on takes 70 and 72, reporting p5/p50/p95 =
    0/0/0 on frames whose cockpit visibly shifts between the eyes. The
    information is in the tail, where the near geometry is, so read the tail:
    whichever end reaches further from zero is the direction the eyes are
    offset. At IPD=0 both ends are zero, the window is {0}, and the negative
    control still reads 0.0%.
    """
    s = sh[cc >= good]
    if not s.size:
        return 0, 0
    lo, hi = np.percentile(s, 2), np.percentile(s, 98)
    if abs(lo) > abs(hi) + 1:
        return -dmax, 0
    if abs(hi) > abs(lo) + 1:
        return 0, dmax
    return 0, 0


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


def brightness_ratio(a, b, shp, tile, min_level):
    """Per-tile right/left brightness, each tile compared at its own shift."""
    h, w = a.shape
    ratio = np.full(shp.shape, np.nan, np.float32)
    for iy in range(shp.shape[0]):
        for ix in range(shp.shape[1]):
            y, x = iy * tile, ix * tile
            xs = min(max(x - int(shp[iy, ix]), 0), w - tile)
            m0 = a[y : y + tile, xs : xs + tile].mean()
            m1 = b[y : y + tile, x : x + tile].mean()
            if max(m0, m1) >= min_level:
                ratio[iy, ix] = m1 / max(m0, 1e-6)
    return ratio


def defect_fraction(ratio, rel):
    """Fraction of judged tiles that are both differently lit and confidently
    matched -- the number the verdict rests on."""
    ok = ~np.isnan(ratio)
    off = (ratio > RATIO_FLAG) | (ratio < 1.0 / RATIO_FLAG)
    return 100.0 * (ok & off & rel).sum() / max(ok.sum(), 1)


# Lit in one eye, dark in the other. Absolute levels, because a ratio is
# meaningless against near-black and this is the one case where "how dark" is
# the whole question.
DARK, LIT = 12.0, 40.0


def unmatched_dark(a, b, shp, tile):
    """Pixels that are dark in one eye and lit in the other, after alignment.

    This exists because the ratio verdict above CANNOT see this defect. That
    verdict is `bad & rel`, and `rel` means NCC >= 0.7 -- but a surface whose
    shadowed half is flat black has nothing to correlate with, so it fails the
    gate and is dropped before its brightness is ever compared. Take 69's
    cockpit rim was lit in the right eye and dark in the left across 280px, and
    the tile verdict counted exactly none of it.

    So: no confidence gate. Warp by the propagated disparity -- the shadowed
    patch has lit, textured neighbours that did match, and it lies on their
    surface -- then compare absolute levels.

    A shadow lies ON a surface, so it must move by that surface's disparity.
    Anything left over after warping is the two eyes disagreeing about where
    the shadow falls, which is the defect this file spent takes 56-69 failing
    to measure.
    """
    h, w = a.shape
    warp = np.zeros_like(a)
    for iy in range(shp.shape[0]):
        for ix in range(shp.shape[1]):
            y, x = iy * tile, ix * tile
            xs = min(max(x - int(shp[iy, ix]), 0), w - tile)
            warp[y : y + tile, x : x + tile] = a[y : y + tile, xs : xs + tile]
    return (warp < DARK) & (b >= LIT), (b < DARK) & (warp >= LIT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("layer0")
    ap.add_argument("layer1")
    ap.add_argument("--tile", type=int, default=64)
    # 320px is just past the 300px the near plane allows at the logged
    # ipd/sx/near. Raising it only admits more false matches.
    ap.add_argument("--max", type=int, default=320, dest="maxshift")
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

    # Pass 1 only establishes which way the eyes are offset. Pass 2 is the
    # measurement, searched over shifts the geometry can actually produce.
    sh0, cc0 = disparity_field(a, b, T, -args.maxshift, args.maxshift)
    lo, hi = plausible_window(sh0, cc0, args.maxshift)
    sh, cc = disparity_field(a, b, T, lo, hi)
    shp, rel = propagate(sh, cc)

    ratio = brightness_ratio(a, b, shp, T, args.min_level)
    shp0, rel0 = propagate(sh0, cc0)
    loose = defect_fraction(brightness_ratio(a, b, shp0, T, args.min_level), rel0)

    ok = ~np.isnan(ratio)
    if not ok.any():
        sys.exit("no tile bright enough to judge -- lower --min-level")
    r = ratio[ok]
    print(f"tiles          {int(ok.sum())} judged of {sh.size} "
          f"({T}px, whole frame, shifts {lo:+d}..{hi:+d}px)")
    print(f"               {int(rel.sum())} matched confidently (NCC>=0.7), "
          f"the rest took a neighbour's disparity")
    print(f"               unconstrained +-{args.maxshift}px search would say "
          f"{loose:.1f}% -- the excess is repeating hull matching itself")
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

    # Second, independent verdict: content present in one eye and absent from
    # the other. Reported separately because the tile verdict above is
    # structurally blind to it -- see unmatched_dark().
    dl, dr = unmatched_dark(a, b, shp, T)
    pl, pr = 100.0 * dl.mean(), 100.0 * dr.mean()
    print(f"one-eye-dark   {pl:.2f}% of pixels lit in the right eye and dark "
          f"in the left,\n               {pr:.2f}% the other way "
          f"(asymmetry {pl / max(pr, 1e-9):.1f}x)")
    if max(pl, pr) >= 0.5 and max(pl, pr) / max(min(pl, pr), 1e-9) >= 1.5:
        print("               a shadow lies ON a surface and must move by that "
              "surface's\n               disparity -- a one-sided excess means "
              "the two eyes disagree\n               about where it falls. "
              "Crop the worst blobs and look.")

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
