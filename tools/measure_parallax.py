# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#!/usr/bin/env python3
"""Phase 4a regression check: does the per-eye shear fall off with depth?

Usage:  python3 tools/measure_parallax.py <mono.png> <sheared.png>

Capture both frames from the SAME save at the same camera pose, one run with
X4VR_EYE unset (no shader patching at all) and one with X4VR_EYE=right. The
starfield region doubles as the alignment check: if the camera poses match it
correlates at ~1.0 with zero shift, and any non-zero star shift means the two
loads did not line up, not that parallax reached infinity.

Prediction (committed before the measurement was run):
    HUD           0 px      screen space, excluded from the shear
    starfield     0 px      infinite depth  -> sx*dx/z -> 0
    cockpit    -190..-375   near geometry   -> strong parallax

A uniform clip-space translation would move ALL of these by the same amount.
Only 1/z parallax makes near geometry move while the stars stay put.
"""
import numpy as np
from PIL import Image

# 1408x1408 per eye is what this project happens to run at; it is not a
# property of the mod. The headset decides the eye size, it need not be square,
# and it will differ per user -- so the half-width comes from the image being
# measured, never from a constant.
SX, DX = 0.889, 0.15


def predicted_shift_times_z(width_px):
    """`shift_px * z` for an eye `width_px` wide. Half-width, because the
    projection's x scale is relative to the half-extent of the frame."""
    return SX * DX * (width_px / 2.0)


def gray(p):
    return np.asarray(Image.open(p).convert("L"), dtype=np.float32)


def ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-6 else -1.0


def shift_of(ref, tgt, box, lo=-500, hi=120):
    x0, y0, x1, y1 = box
    patch = ref[y0:y1, x0:x1]
    best = (0, -1.0)
    for dx in range(lo, hi):
        sx_ = x0 + dx
        if sx_ < 0 or sx_ + (x1 - x0) > tgt.shape[1]:
            continue
        c = ncc(patch, tgt[y0:y1, sx_:sx_ + (x1 - x0)])
        if c > best[1]:
            best = (dx, c)
    return best


REGIONS = [
    # name,                       x0    y0    x1    y1     kind
    ("starfield (top right)",   2250,   80, 2760,  430, "far"),
    ("starfield (upper left)",    60,   80,  520,  400, "far"),
    ("HUD radar (screen space)",1300, 1160, 1520, 1350, "ui"),
    ("HUD bars (screen space)", 1870,  560, 2060,  800, "ui"),
    ("canopy strut (mid)",       900,  400, 1260,  620, "near"),
    ("left console (near)",      150,  950,  700, 1260, "near"),
    ("right hull panel (near)", 1900,  950, 2500, 1260, "near"),
]


def main():
    import sys
    a = sys.argv[1] if len(sys.argv) > 1 else "mono_game.png"
    b = sys.argv[2] if len(sys.argv) > 2 else "v2_game.png"
    mono, sheared = gray(a), gray(b)
    # From the image, not a constant: the eye size is the headset's to decide.
    pred = predicted_shift_times_z(mono.shape[1])

    print(f"{'region':30s} {'shift':>7s} {'corr':>6s} {'implied z':>11s}")
    print("-" * 60)
    out = {}
    for name, x0, y0, x1, y1, kind in REGIONS:
        dx, c = shift_of(mono, sheared, (x0, y0, x1, y1))
        z = pred / abs(dx) if dx else float("inf")
        zs = f"{z:8.2f} m" if dx else "       inf"
        flag = "" if c > 0.5 else "   (low conf)"
        print(f"{name:30s} {dx:>7d} {c:>6.3f} {zs}{flag}")
        out.setdefault(kind, []).append((name, dx, c))

    print()
    far = [d for _, d, c in out.get("far", []) if c > 0.5]
    ui = [d for _, d, c in out.get("ui", []) if c > 0.5]
    near = [d for _, d, c in out.get("near", []) if c > 0.5]

    if far:
        print(f"far  (stars, z->inf) : {far}  -> expect ~0")
    if ui:
        print(f"ui   (screen space)  : {ui}  -> expect exactly 0")
    if near:
        print(f"near (cockpit)       : {near}  -> expect -190..-375")

    print()
    if not (far and near):
        print("VERDICT: inconclusive - not enough confident matches.")
        return
    spread = max(abs(min(near)), 0) - max(abs(d) for d in far)
    if abs(np.mean(far)) <= 3 and abs(np.mean(near)) >= 30:
        print("VERDICT: depth-dependent. Near geometry is displaced while the")
        print("         starfield is not -> this is 1/z parallax, not a")
        print(f"         uniform translation. (separation {spread:.0f} px)")
    elif abs(np.mean(near) - np.mean(far)) < 10:
        print("VERDICT: UNIFORM SHIFT - everything moved together. The shear")
        print("         is behaving as a translation; Phase 4a is WRONG.")
    else:
        print("VERDICT: ambiguous, needs a closer look.")


if __name__ == "__main__":
    main()
