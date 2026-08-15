#!/usr/bin/env python3
"""Guard-band cost curve for layer-side head rotation (#33).

Head rotation applied in the layer is a homography -- x' = K R K^-1 x -- which
is *exact* for a pure rotation about the projection centre, at every depth. The
only thing that limits it is coverage: a ray the runtime's eye frustum wants
must have been rendered by X4, and X4 renders one frustum per eye with no idea
the head moved. So the head-rotation range is bought with render area, and this
computes the price.

Everything here is measured, nothing is assumed:

  * per-eye frusta      take 112 xr-probe, Quest 3 over WiVRn (KEY_FOV0/KEY_FOV1)
  * X4's FOV mapping    horizontal FOV deg = X4VR_FOV * 73.7399 (docs/known-good-runs.md:260)
  * current knob        X4VR_FOV=1.4917, eye render 1408x1408 (take 114d onward)

The self-check is the point of trusting the rest: with a zero head-rotation
budget the symmetric model must reproduce X4VR_FOV=1.4917, because that is the
value takes 114d..154 actually ran. A model that cannot reproduce the known-good
state does not get to price the unknown one.

Usage:  tools/guard_band.py [--max-deg 34] [--step 2.5]
"""

import argparse
import math

# --- measured inputs -------------------------------------------------------

# (angleLeft, angleRight, angleUp, angleDown) in degrees, as OpenXR reports
# them: left and down are negative. Take 112, KEY_FOV0 / KEY_FOV1.
EYE_FOV_DEG = [
    (-54.0, 40.0, 44.0, -55.0),   # view 0
    (-40.0, 54.0, 44.0, -55.0),   # view 1
]

X4_FOV_DEG_PER_UNIT = 73.7399     # horizontal FOV = X4VR_FOV * this
BASE_EYE_PX = 1408                # X4VR_RES, per eye, square
BASE_X4VR_FOV = 1.4917            # the knob every take since 114d has run


def boundary_rays(fov_deg, per_edge=96):
    """Unit rays along the frustum boundary, densely enough that the bounding
    extent of the rotated pyramid is found rather than assumed.

    Sampling the four corners alone would be a guess about which ray ends up
    extreme after an arbitrary rotation; edges are cheap, so walk them.
    """
    l, r, u, d = (math.tan(math.radians(a)) for a in fov_deg)
    pts = []
    for i in range(per_edge + 1):
        t = i / per_edge
        pts.append((l + (r - l) * t, u))          # top edge
        pts.append((l + (r - l) * t, d))          # bottom edge
        pts.append((l, d + (u - d) * t))          # left edge
        pts.append((r, d + (u - d) * t))          # right edge
    # tangent-plane point (x, y) at z = 1 -> direction
    return [(x, y, 1.0) for x, y in pts]


def rot_yaw_pitch(yaw_deg, pitch_deg):
    """Head rotation, yaw about y then pitch about x, in the eye's frame."""
    cy, sy = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    cp, sp = math.cos(math.radians(pitch_deg)), math.sin(math.radians(pitch_deg))
    # R = Rx(pitch) * Ry(yaw), applied to a column vector
    return (
        (cy,        0.0,  sy),
        (sp * sy,   cp,  -sp * cy),
        (-cp * sy,  sp,   cp * cy),
    )


def apply(R, v):
    return tuple(sum(R[i][j] * v[j] for j in range(3)) for i in range(3))


def _axis_steps(budget_deg, grid):
    if budget_deg <= 0 or grid < 2:
        return [0.0]
    return [-budget_deg + 2 * budget_deg * i / (grid - 1) for i in range(grid)]


def required_extent(eye_fov, yaw_deg, pitch_deg, grid=5):
    """Tangent-space bounds (left, right, up, down) X4 must render for this eye
    so that every head rotation within the yaw/pitch budget box is covered.

    The union over the whole box is what matters, not one rotation -- the player
    passes through every intermediate pose, and the corner of the box is the
    expensive one because it stacks both angles onto the same corner ray.
    """
    rays = boundary_rays(eye_fov)
    lo_x = lo_y = math.inf
    hi_x = hi_y = -math.inf
    for yaw in _axis_steps(yaw_deg, grid):
        for pitch in _axis_steps(pitch_deg, grid):
            R = rot_yaw_pitch(yaw, pitch)
            for v in rays:
                x, y, z = apply(R, v)
                if z <= 1e-6:
                    return None          # ray at or past 90 deg: not renderable
                lo_x, hi_x = min(lo_x, x / z), max(hi_x, x / z)
                lo_y, hi_y = min(lo_y, y / z), max(hi_y, y / z)
    return lo_x, hi_x, hi_y, lo_y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-deg", type=float, default=34.0)
    ap.add_argument("--step", type=float, default=2.5)
    ap.add_argument("--pitch-deg", type=float, default=None,
                    help="fixed pitch budget; default is to track yaw")
    args = ap.parse_args()

    base_half_tan = math.tan(math.radians(BASE_X4VR_FOV * X4_FOV_DEG_PER_UNIT / 2))

    print("Guard-band cost for layer-side head rotation")
    print("  eye frusta   %s" % (EYE_FOV_DEG,))
    print("  baseline     X4VR_FOV=%.4f, %dx%d per eye\n"
          % (BASE_X4VR_FOV, BASE_EYE_PX, BASE_EYE_PX))
    print("Pixel counts hold tangent-space sample density constant, i.e. the")
    print("angular resolution on axis is preserved and only the covered angle")
    print("grows. 'sym' is what X4 renders today (one symmetric frustum per")
    print("eye); 'asym' is the same coverage with the per-eye off-axis")
    print("projection of #35, which needs no extra pixels to be worth having.\n")

    print("  pitch budget: %s\n"
          % ("tracks yaw" if args.pitch_deg is None
             else "fixed at %.1f deg" % args.pitch_deg))
    hdr = ("   yaw     X4VR_FOV   sym px/eye   sym area   asym px/eye  asym area")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))

    n = int(args.max_deg / args.step) + 1
    for i in range(n):
        b = i * args.step
        pb = b if args.pitch_deg is None else args.pitch_deg
        rows = [required_extent(f, b, pb) for f in EYE_FOV_DEG]
        if any(r is None for r in rows):
            print("  %5.1f    -- a ray reaches 90 deg: no planar render covers this --"
                  % b)
            continue

        # Symmetric square frustum: one half-angle must contain every edge of
        # every eye, because X4 has one FOV and the render is square.
        max_tan = max(max(abs(v) for v in r) for r in rows)
        half_deg = math.degrees(math.atan(max_tan))
        if half_deg >= 89.5:
            print("  %5.1f    -- required FOV reaches 180 deg --" % b)
            continue
        knob = 2 * half_deg / X4_FOV_DEG_PER_UNIT
        sym_px = BASE_EYE_PX * max_tan / base_half_tan
        sym_area = (sym_px / BASE_EYE_PX) ** 2

        # Per-eye off-axis: each edge sized to what that eye actually needs.
        # Both eyes come out the same by mirror symmetry; take the larger.
        asym = 0.0
        for l, r_, u, d in rows:
            w = (abs(l) + abs(r_)) / (2 * base_half_tan) * BASE_EYE_PX
            h = (abs(u) + abs(d)) / (2 * base_half_tan) * BASE_EYE_PX
            asym = max(asym, w * h)
            asym_wh = (w, h)
        asym_area = asym / (BASE_EYE_PX ** 2)

        print("  %5.1f    %7.4f   %5.0f sq     %6.2fx    %4.0fx%-4.0f    %6.2fx"
              % (b, knob, sym_px, sym_area, asym_wh[0], asym_wh[1], asym_area))

    # --- self-check --------------------------------------------------------
    rows = [required_extent(f, 0.0, 0.0) for f in EYE_FOV_DEG]
    max_tan = max(max(abs(v) for v in r) for r in rows)
    knob0 = 2 * math.degrees(math.atan(max_tan)) / X4_FOV_DEG_PER_UNIT
    ok = abs(knob0 - BASE_X4VR_FOV) < 0.002
    print("\n  self-check: zero-budget symmetric knob = %.4f, shipped = %.4f  %s"
          % (knob0, BASE_X4VR_FOV, "OK" if ok else "MISMATCH -- model is wrong"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
