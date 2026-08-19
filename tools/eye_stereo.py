#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Judge a stereo pair from the present dumps, per region rather than in bulk.

Task #32. The check this replaces asked whether layer 1 held roughly as much
light as layer 0, and takes 41-44 showed that a metric of that shape cannot tell
correct stereo from broken: a bit-exact copy of layer 0 scores a perfect 1.00,
and so does a correct right eye. The ratio was never measuring stereo.

What distinguishes them is not how much the two images differ but *how the
difference is distributed*:

    identical copy        every region aligns best at shift 0, and r = 1.000
    uniform translation   every region aligns at the same nonzero shift
    true stereo           regions align at DIFFERENT shifts, because the shear
                          displaces by sx*(ipd/2)/z and z varies across a frame

So the number to report is the **spread** of per-region shifts, not their mean.
A single global shift, however large, is a slid image and not a stereo pair --
which is exactly the failure `make_eye_shear`'s comment warns about, "a plain
clip-space translation would slide the whole image uniformly and produce no
depth cue at all".

Two things this deliberately does NOT do:

  * It does not fail a frame for being identical. X4's UI is mono by
    construction -- unsheared passes bind the unpatched module and both layers
    receive the same pixels -- so a menu or splash frame *should* be identical,
    and the six dumps on disk from take 103 are exactly that. Identical is a
    finding to report with the frame's brightness beside it, not a verdict.
  * It does not average over frames. One flight frame carries the answer and a
    dozen loading frames do not; blending them is how a real signal gets
    divided by the number of frames that had nothing in them.
"""
import glob
import re
import sys

try:
    import numpy as np
except ImportError:  # scoring a log must not depend on numpy being installed
    np = None

# Shifts searched, in pixels. The eye shear puts (W/2)*sx*(ipd/2)/z px on each
# eye, which at 1408px, sx=0.75405 and ipd=0.064 is 17.0/z -- so +-40 covers
# everything from about 0.4 m outwards, and anything beyond it is closer to the
# eye than a cockpit panel.
MAX_SHIFT = 40

# A tile with no texture correlates with anything. This is the fraction of the
# brightest tile's variance a tile must carry to get a vote; empty space and
# unlit hull both fall below it.
MIN_ENERGY = 0.02

# A tile must actually match at its best shift to vote. Take 97's dumps are why:
# 31 of its 253 frames came back with tiles pinned at -40 AND +40 at once, r
# around 0.78, on frames of mean luma 9. That is the correlator finding nothing
# and returning the edge of its search window, and read as a measurement it says
# "80 px of parallax", i.e. geometry 0.75 m from the eye, in a frame of distant
# space. A number that means "I failed" must not be reported in the same units
# as the answer.
MIN_R = 0.90

TILES = 8


def read_ppm(path):
    """Binary PPM or PNG. The layer writes PNG since take 176.

    This function and the glob below both said `.ppm`, so the format change
    silently turned every present dump invisible: take 177 FAILED with "no
    present dumps were found" while 20 of them sat on disk. An intent gate that
    fires on the analyser's own staleness accuses the run of a fault it does not
    have -- exactly the failure mode the gate exists to prevent.
    """
    if path.lower().endswith(".png"):
        from PIL import Image
        # Same contract as the PPM branch below: float32 HxWx3, NOT a tuple.
        # Returning the wrong shape here would have been caught by the first
        # frame, which is why this is asserted against a real dump in
        # tests/run-eye-stereo-format.sh rather than reasoned about.
        return np.asarray(Image.open(path).convert("RGB"),
                          dtype=np.float32)
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError(f"{path}: not a binary PPM")
    tok, i, n = [], 2, len(data)
    while len(tok) < 3:
        while i < n and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < n and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < n and not data[j:j + 1].isspace():
            j += 1
        tok.append(int(data[i:j]))
        i = j
    i += 1  # single whitespace byte after maxval
    w, h, _ = tok
    px = np.frombuffer(data[i:i + w * h * 3], dtype=np.uint8)
    if px.size != w * h * 3:
        raise ValueError(f"{path}: truncated, want {w * h * 3} bytes")
    return px.reshape(h, w, 3).astype(np.float32)


def luma(im):
    return im[..., 0] * 0.2126 + im[..., 1] * 0.7152 + im[..., 2] * 0.0722


def tile_shifts(a, b, tiles=TILES, max_shift=MAX_SHIFT):
    """Per-tile best horizontal shift of b against a, and each tile's energy.

    Returns (shifts, energy, r_at_best) as tiles x tiles arrays. The compared
    region is fixed while b slides across it, so every shift scores the same
    pixels of a and the tiles mean the same thing at every offset.

    Sign: a positive shift means the content sits FURTHER RIGHT in b than in a.
    The window below is `b[m+s : w-m+s]` and not `b[m-s : w-m-s]`, which was the
    first version and returned every displacement negated. The controls in
    tests/eye_stereo_selftest.py caught it because they slide a known amount and
    check the value, not just its magnitude -- and a sign error here is not
    cosmetic: it reports the eyes swapped, which is a defect this project has
    already spent takes on from the other direction.
    """
    h, w = a.shape
    m = max_shift
    x = a[:, m:w - m]
    th, tw = h // tiles, x.shape[1] // tiles
    x = x[:th * tiles, :tw * tiles]

    def blocks(v):
        return v.reshape(tiles, th, tiles, tw).transpose(0, 2, 1, 3) \
                .reshape(tiles, tiles, -1)

    xb = blocks(x)
    xb = xb - xb.mean(axis=2, keepdims=True)
    xv = (xb * xb).sum(axis=2)

    best_r = np.full((tiles, tiles), -2.0)
    best_s = np.zeros((tiles, tiles), dtype=int)
    for s in range(-max_shift, max_shift + 1):
        y = b[:, m + s:w - m + s]
        y = y[:th * tiles, :tw * tiles]
        yb = blocks(y)
        yb = yb - yb.mean(axis=2, keepdims=True)
        yv = (yb * yb).sum(axis=2)
        denom = np.sqrt(xv * yv)
        with np.errstate(invalid="ignore", divide="ignore"):
            r = np.where(denom > 0, (xb * yb).sum(axis=2) / denom, -2.0)
        better = r > best_r
        best_r = np.where(better, r, best_r)
        best_s = np.where(better, s, best_s)
    return best_s, xv / xv.max() if xv.max() > 0 else xv, best_r


def judge_pair(p0, p1):
    """One frame. Returns a dict, or raises on unreadable input."""
    l0, l1 = read_ppm(p0), read_ppm(p1)
    if l0.shape != l1.shape:
        return {"error": f"{l0.shape} vs {l1.shape} — the two layers are not "
                         f"the same size"}
    y0, y1 = luma(l0), luma(l1)
    out = {"w": l0.shape[1], "h": l0.shape[0],
           "mean0": float(y0.mean()), "mean1": float(y1.mean()),
           "identical": bool(np.array_equal(l0, l1))}
    if out["identical"]:
        return out
    s, e, r = tile_shifts(y0, y1)
    textured = e >= MIN_ENERGY
    # Saturated at the edge of the search window means no match was found, not
    # a match that far away. Counted and reported, never silently dropped.
    saturated = textured & (np.abs(s) == MAX_SHIFT)
    weak = textured & ~saturated & (r < MIN_R)
    live = textured & ~saturated & (r >= MIN_R)
    out["tiles"] = int(live.sum())
    out["textured"] = int(textured.sum())
    out["saturated"] = int(saturated.sum())
    out["weak"] = int(weak.sum())
    if live.any():
        sv = s[live]
        out["shift_median"] = float(np.median(sv))
        out["shift_min"] = int(sv.min())
        out["shift_max"] = int(sv.max())
        out["spread"] = int(sv.max() - sv.min())
        out["r_median"] = float(np.median(r[live]))
        # Counted per tile, not per frame. Two tiles reading +3 in a frame whose
        # other tiles read -30 is correlation noise on a repeating texture or an
        # object edge; the eyes being swapped would put EVERY tile on the wrong
        # side. A verdict about the whole pipeline must not turn on an extremum.
        out["pos"] = int((sv > 0).sum())
        out["neg"] = int((sv < 0).sum())
    return out


def find_pairs(prefix):
    """(layer0, layer1) paths for each present dump, in frame order."""
    pairs = []
    # Both formats: PNG since take 176, PPM for every dump taken before it.
    layer0 = (glob.glob(f"{prefix}-present-n*-layer0.png") +
              glob.glob(f"{prefix}-present-n*-layer0.ppm"))
    for p0 in sorted(layer0,
                     key=lambda s: int(re.search(r"-n(\d+)-", s).group(1))):
        p1 = p0.replace("-layer0.", "-layer1.")
        if glob.os.path.exists(p1):
            pairs.append((p0, p1))
    return pairs


SUMMARISE_OVER = 8


def summarise(pairs, emit, w, sx, ipd):
    """Tally many frames, then print the deepest few in full."""
    rows = []
    for p0, p1 in pairs:
        try:
            d = judge_pair(p0, p1)
        except (OSError, ValueError) as exc:
            emit(f"eye  {p0}: unreadable — {exc}")
            continue
        d["n"] = int(re.search(r"-n(\d+)-", p0).group(1))
        rows.append(d)
    ident = [d for d in rows if d.get("identical")]
    meas = [d for d in rows if d.get("spread") is not None]
    mute = [d for d in rows
            if not d.get("identical") and d.get("spread") is None]
    flat = [d for d in meas if d["spread"] == 0]
    deep = sorted((d for d in meas if d["spread"] > 0),
                  key=lambda d: -d["spread"])
    emit(f"eye  {len(rows)} present dump(s): {len(ident)} bit-identical, "
         f"{len(flat)} measured with no parallax, {len(deep)} with parallax, "
         f"{len(mute)} with nothing alignable")
    if not deep:
        emit("warn  no frame in this run shows depth-varying parallax — every "
             "measurable one is flat, which is a duplicated left eye unless the "
             "whole session was distant geometry")
        return len(rows)
    for d in deep[:3]:
        far = max(abs(d["shift_min"]), abs(d["shift_max"]))
        z = depth_for(far, w or d["w"], sx, ipd)
        emit(f"eye  frame {d['n']}: {d['tiles']} matched tile(s), shift "
             f"{d['shift_min']:+d}..{d['shift_max']:+d} px (spread "
             f"{d['spread']}), r={d['r_median']:.4f}, luma {d['mean0']:.1f}"
             + (f", nearest ~{z:.1f} m" if z else ""))
    # The sign is the eye order, and getting it backwards is a defect that looks
    # like working stereo until someone wears it. A near object sits LEFT in the
    # right eye, so with layer 0 as the left eye every shift should be <= 0.
    pos = sum(d.get("pos", 0) for d in deep)
    neg = sum(d.get("neg", 0) for d in deep)
    if pos and pos > 0.05 * (pos + neg):
        emit(f"warn  {pos} of {pos + neg} displaced tile(s) move RIGHT in "
             f"layer 1 — with layer 0 as the left eye a nearer object must move "
             f"left, so either the eyes are swapped or the shear's sign is")
    elif pos:
        emit(f"eye  {neg} displaced tile(s) move left and {pos} move right "
             f"({pos / (pos + neg):.1%}) — layer 0 = left eye with the shear "
             f"the right way round, the rest is correlation noise on repeating "
             f"texture and object edges")
    else:
        emit(f"eye  all {neg} displaced tile(s) move left in layer 1, which is "
             f"layer 0 = left eye with the shear the right way round")
    return len(rows)


def depth_for(shift_px, w, sx, ipd):
    """View depth implied by a layer0->layer1 displacement, in metres.

    Per eye the shear moves a point by (w/2)*sx*(ipd/2)/z px, so between the two
    layers it is twice that: z = w*sx*ipd / (2*d). At 1408 px, sx=1.3333 and
    ipd=0.064 that is 60.07/d, which is the `disparity_px * z = 60.07` constant
    recorded for the start menu -- so this function and that note have to agree,
    and take 103's dumps are where they were checked against each other.
    """
    if not shift_px or not sx or not ipd or not w:
        return None
    return w * sx * ipd / (2.0 * abs(shift_px))


def report(prefix, emit=print, w=None, sx=None, ipd=None):
    """Print the per-frame verdicts. Returns the number of frames judged."""
    if np is None:
        emit("eye  numpy is not installed, so the present dumps cannot be "
             "read — the log alone cannot judge the right eye")
        return 0
    pairs = find_pairs(prefix)
    if not pairs:
        return 0
    # Take 97 wrote 253 of these. Printed one line each they would bury the rest
    # of the score, and averaged into one number they would do what every
    # aggregate in this project has done -- a session is mostly loading frames,
    # and dividing a real signal by the number of frames that had nothing in
    # them is how the 2.4x shading error went unreported. So: a tally, plus the
    # frames that carry the most depth, printed individually.
    if len(pairs) > SUMMARISE_OVER:
        return summarise(pairs, emit, w, sx, ipd)
    for p0, p1 in pairs:
        n = re.search(r"-n(\d+)-", p0).group(1)
        try:
            d = judge_pair(p0, p1)
        except (OSError, ValueError) as exc:
            emit(f"eye  frame {n}: unreadable — {exc}")
            continue
        if "error" in d:
            emit(f"eye  frame {n}: {d['error']}")
        elif d["identical"]:
            emit(f"eye  frame {n}: layer 1 is a bit-exact copy of layer 0 "
                 f"(mean luma {d['mean0']:.2f}) — correct for a UI-only or "
                 f"loading frame, broken for one with world geometry in it")
        elif "spread" in d:
            # The largest displacement is the nearest thing in the frame, and
            # that is the number #25 cares about -- so it is quoted in metres
            # when the run recorded enough to convert it.
            far = max(abs(d["shift_min"]), abs(d["shift_max"]))
            z = depth_for(far, w or d["w"], sx, ipd)
            near = f", nearest ~{z:.1f} m" if z else ""
            drop = ""
            if d["saturated"] or d["weak"]:
                drop = (f" [{d['saturated']} tile(s) hit the ±{MAX_SHIFT} px "
                        f"search limit, {d['weak']} matched below r={MIN_R}, "
                        f"none of them counted]")
            emit(f"eye  frame {n}: {d['tiles']} matched tile(s) of "
                 f"{d['textured']} textured{drop}, shift "
                 f"{d['shift_min']:+d}..{d['shift_max']:+d} px "
                 f"(median {d['shift_median']:+.0f}, spread {d['spread']}), "
                 f"r={d['r_median']:.4f}, mean luma {d['mean0']:.2f}/"
                 f"{d['mean1']:.2f}{near}")
            if d["spread"] == 0 and d["shift_median"] == 0:
                # Not the same defect as a slide, and saying "slid image" here
                # was wrong: this is a pair with no measurable parallax, which
                # for a frame of distant geometry is the correct answer. At
                # 1408 px and sx=1.3333 one pixel is already 60 m away.
                emit(f"eye  frame {n}: no measurable parallax — every textured "
                     f"region aligns at 0, which is correct for geometry beyond "
                     f"about {depth_for(1, w or d['w'], sx, ipd):.0f} m and "
                     f"wrong for anything nearer"
                     if sx and ipd else
                     f"eye  frame {n}: no measurable parallax — every textured "
                     f"region aligns at 0")
            elif d["spread"] == 0:
                emit(f"warn  frame {n}: every textured region aligns at the "
                     f"same nonzero shift ({d['shift_median']:+.0f}) — that is "
                     f"a slid image, not a stereo pair; depth would have to be "
                     f"constant across the whole frame")
        else:
            emit(f"eye  frame {n}: nothing alignable — {d.get('textured', 0)} "
                 f"textured tile(s), {d.get('saturated', 0)} at the search "
                 f"limit, {d.get('weak', 0)} below r={MIN_R}, mean luma "
                 f"{d['mean0']:.2f}. No measurement, and not a verdict either")
    return len(pairs)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        print("usage: eye_stereo.py <dump-prefix>   "
              "(the value of X4VR_MV_DUMP)")
        sys.exit(2)
    sys.exit(0 if report(sys.argv[1]) else 1)
