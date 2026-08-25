// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// How far the head may rotate before the conjugation runs out of pixels.
//
// #33's layer-side rotation is a homography, x' = K R K^-1 x, exact for a pure
// rotation about the projection centre at every depth. Nothing limits it but
// COVERAGE: a ray the runtime's eye frustum wants must have been rendered by
// X4, and X4 renders one symmetric frustum per eye with no idea the head
// moved. So the range is bought with render area, and past a point it cannot
// be bought at all -- a ray that reaches 90 deg has no planar representation.
//
// This is the same containment test `tools/guard_band.py` uses to price the
// field, inverted: given the field X4 is ACTUALLY rendering, how much rotation
// still fits? The two implementations are independent and
// tests/guard_budget.cpp cross-checks this one against the Python's published
// numbers, because a solver agreeing with itself proves nothing.
//
// **The budget is not advisory.** Rotating further does not degrade
// gracefully; it samples outside what was drawn and returns black, and the
// declared pose would then claim the image came from somewhere it did not.
// Whatever residual the layer applies must be clamped to this, and the SAME
// clamped value must be what gets declared to the compositor.
#pragma once

#include <cmath>

namespace x4vr {

// Per-eye field as OpenXR reports it: left and down negative, radians.
struct EyeFov {
    float l = 0, r = 0, u = 0, d = 0;
};

namespace guard {

inline void rot_yaw_pitch(float yaw_rad, float pitch_rad, float m[9]) {
    const float cy = std::cos(yaw_rad), sy = std::sin(yaw_rad);
    const float cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
    // R = Rx(pitch) * Ry(yaw), applied to a column vector. Mirrors
    // guard_band.py's rot_yaw_pitch exactly; a different composition order
    // would give a different -- and differently wrong -- corner.
    m[0] = cy;       m[1] = 0.0f;  m[2] = sy;
    m[3] = sp * sy;  m[4] = cp;    m[5] = -sp * cy;
    m[6] = -cp * sy; m[7] = sp;    m[8] = cp * cy;
}

// Does every boundary ray of `eye`, rotated anywhere inside the yaw/pitch
// budget box, still land inside a symmetric field of half-tangent `half_tan`?
//
// The whole BOX, not one rotation: the player passes through every
// intermediate pose, and the corner is the expensive one because it stacks
// both angles onto the same ray. Edges are walked rather than just corners --
// which ray ends up extreme after an arbitrary rotation is not something to
// guess at.
inline bool covered(const EyeFov &eye, float half_tan, float yaw_budget_rad,
                    float pitch_budget_rad, int per_edge = 96, int grid = 5) {
    const float l = std::tan(eye.l), r = std::tan(eye.r);
    const float u = std::tan(eye.u), d = std::tan(eye.d);
    auto steps = [grid](float b, int i) {
        return b <= 0.0f || grid < 2 ? 0.0f
                                     : -b + 2.0f * b * (float)i / (grid - 1);
    };
    const int gy = (yaw_budget_rad <= 0.0f || grid < 2) ? 1 : grid;
    const int gp = (pitch_budget_rad <= 0.0f || grid < 2) ? 1 : grid;
    for (int iy = 0; iy < gy; iy++) {
        for (int ip = 0; ip < gp; ip++) {
            float R[9];
            rot_yaw_pitch(steps(yaw_budget_rad, iy), steps(pitch_budget_rad, ip),
                          R);
            for (int i = 0; i <= per_edge; i++) {
                const float t = (float)i / per_edge;
                const float pts[4][2] = {{l + (r - l) * t, u},
                                         {l + (r - l) * t, d},
                                         {l, d + (u - d) * t},
                                         {r, d + (u - d) * t}};
                for (const auto &p : pts) {
                    const float v[3] = {p[0], p[1], 1.0f};
                    float o[3];
                    for (int k = 0; k < 3; k++)
                        o[k] = R[k * 3] * v[0] + R[k * 3 + 1] * v[1] +
                               R[k * 3 + 2] * v[2];
                    // A ray at or past 90 deg has no planar representation at
                    // any field. Not "too expensive" -- impossible.
                    if (o[2] <= 1e-6f)
                        return false;
                    if (std::fabs(o[0] / o[2]) > half_tan ||
                        std::fabs(o[1] / o[2]) > half_tan)
                        return false;
                }
            }
        }
    }
    return true;
}

// The largest yaw=pitch budget, in DEGREES, that `half_tan` covers for both
// eyes. Bisection on a monotone predicate: widening the box only adds rays.
//
// Returns 0 when even a zero budget does not fit -- which is not a failure of
// this function but a statement that X4's field is already narrower than the
// headset's, and the honest response is a black edge, not a smaller clamp.
inline float max_budget_deg(const EyeFov *eyes, int count, float half_tan,
                            float search_max_deg = 45.0f) {
    const float kDeg = 0.01745329251994330f;
    auto fits = [&](float deg) {
        for (int e = 0; e < count; e++)
            if (!covered(eyes[e], half_tan, deg * kDeg, deg * kDeg))
                return false;
        return true;
    };
    if (!fits(0.0f))
        return 0.0f;
    float lo = 0.0f, hi = search_max_deg;
    if (fits(hi))
        return hi;
    for (int i = 0; i < 40; i++) {
        const float mid = 0.5f * (lo + hi);
        (fits(mid) ? lo : hi) = mid;
    }
    return lo;
}

// X4's own law, from the layer: full horizontal degrees = X4VR_FOV * 73.7399,
// so the half-angle is half of that and the symmetric half-tangent follows.
inline float half_tan_of_x4vr_fov(float fov) {
    const float kDeg = 0.01745329251994330f;
    return std::tan(0.5f * fov * 73.7399f * kDeg);
}

// What the conjugation should apply this frame, and whether the budget bit.
//
// Here rather than in the layer because the layer's copy cannot run headless
// -- it needs an XrSession -- and this is the arithmetic that decides both
// what the shader rotates by and what gets declared to the compositor. Those
// two must be the same number; making it one function is how that is enforced
// rather than remembered.
struct Residual {
    float yaw = 0.0f, pitch = 0.0f;
    bool clamped = false;
};

// `cmd_valid` false means the injector is not driving, so X4 rendered along
// its own axis and the command is zero -- the residual is then the whole head
// angle and the budget does all the work. Deliberately not a special case: the
// rule is the same, and "applied == declared" stays true either way.
inline Residual residual_of(float head_yaw_deg, float head_pitch_deg,
                            float cmd_yaw_deg, float cmd_pitch_deg,
                            bool cmd_valid, float budget_deg) {
    Residual out;
    if (budget_deg <= 0.0f)
        return out;                       // no range bought, none applied
    const float ry = head_yaw_deg - (cmd_valid ? cmd_yaw_deg : 0.0f);
    const float rp = head_pitch_deg - (cmd_valid ? cmd_pitch_deg : 0.0f);
    auto clamp = [budget_deg](float v) {
        return v < -budget_deg ? -budget_deg : (v > budget_deg ? budget_deg : v);
    };
    out.yaw = clamp(ry);
    out.pitch = clamp(rp);
    out.clamped = out.yaw != ry || out.pitch != rp;
    return out;
}

} // namespace guard
} // namespace x4vr
