// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HEADLOOK_WRITE=1 -- head-look WITHOUT MOUSELOOK. The first thing in this
// project that WRITES X4's state; everything before it read.
//
// Head-look turned X4's camera by holding X4's MOUSELOOK key and pushing mouse
// deltas, and holding MOUSELOOK kills cockpit hover (takes 312/313). Patola:
// "We need some way to skip X4's mouse-look entirely and change the viewport
// by our own means." Take 319 found where the free-look lives and 0x1933d00,
// read statically, says how X4 treats it (docs/x4-quirks.md, "0x1933d00,
// read"):
//
//   * the free-look state IS the camera's own local matrix, camera+0x5b0..
//     +0x5d0 -- X4 extracts yaw/pitch from it every frame (0xb08290) and
//     rebuilds it (0x1e02200); there is no separate angle variable;
//   * with the mouse-look flag clear, a follower (0x1e8c260 -> 0x1a482d0)
//     moves the angle toward numpad x max, limited by `anglespeed` -- which
//     treats |anglespeed| < 1e-4 as "desired velocity zero": the angle holds.
//
// So with the x4vrmod_headlook extension (anglespeed 0), a matrix written here
// stays: X4 extracts it, the follower leaves it, X4 rebuilds the same matrix.
// Absolute, no servo loop, no key, no synthetic mouse -- and take 315 showed X4
// picks correctly through a camera turned without MOUSELOOK.
//
// Everything is tied to X4 9.00's bytes and to live values; any mismatch
// refuses rather than writes. Self-contained so tests/camwrite.cpp can check
// it without X4.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "x4vr_camwatch.hpp"

namespace x4vr {
namespace camwrite {

using camwatch::Site;

// The code the write relies on, byte for byte. Each one names a fact the write
// depends on; a different build refuses.
inline constexpr Site kWriteSites[] = {
    // 0x1933d00: `mov 0x24845ed(%rip),%r12` -- the player-camera parameters
    // object, global 0x3db83c8 (decoded from this displacement).
    {0x1933dd4, 7, {0x4c, 0x8b, 0x25, 0xed, 0x45, 0x48, 0x02}},
    // The angles are EXTRACTED from the stored matrix: call 0xb08290.
    {0x19342f8, 5, {0xe8, 0x93, 0x3f, 0x1d, 0xff}},
    // ...and the matrix REBUILT from them: call 0x1e02200.
    {0x193452b, 5, {0xe8, 0xd0, 0xdc, 0x4c, 0x00}},
    // The mouse-look flag picks add-delta (no follower) or the follower.
    {0x19344a4, 8, {0x84, 0xdb, 0x0f, 0x84, 0x57, 0x1a, 0x00, 0x00}},
    // maxyaw +0x68c and maxpitch +0x688, read off r12.
    {0x1934490, 10, {0xf3, 0x41, 0x0f, 0x10, 0x9c, 0x24, 0x8c, 0x06, 0x00, 0x00}},
    {0x19344be, 10, {0xf3, 0x45, 0x0f, 0x10, 0x84, 0x24, 0x88, 0x06, 0x00, 0x00}},
    // The follower's angleacc +0x694 and anglespeed +0x690.
    {0x1935f0c, 10, {0xf3, 0x41, 0x0f, 0x10, 0x9c, 0x24, 0x94, 0x06, 0x00, 0x00}},
    {0x1935f1e, 10, {0xf3, 0x41, 0x0f, 0x10, 0x94, 0x24, 0x90, 0x06, 0x00, 0x00}},
    // 0x1a482d0: the zero-speed guard -- eps load, then `comiss; ja`.
    {0x1a482d0, 8, {0xf3, 0x0f, 0x10, 0x25, 0xa8, 0xda, 0x3d, 0x01}},
    {0x1a482e8, 5, {0x0f, 0x2f, 0xe0, 0x77, 0x73}},
    // The base the stored matrix is relative to: translation +0x6f0, row 0
    // +0x700 (rows +0x710/+0x720 follow).
    {0x193419a, 8, {0x45, 0x0f, 0x58, 0x8f, 0xf0, 0x06, 0x00, 0x00}},
    {0x1934104, 8, {0x41, 0x0f, 0x28, 0xaf, 0x00, 0x07, 0x00, 0x00}},
};

// In the parameters object: `<lookaround><internal>` in attribute order.
inline constexpr uintptr_t kMaxPitch = 0x688, kMaxYaw = 0x68c,
                           kAngleSpeed = 0x690, kAngleAcc = 0x694;
// In the camera object.
inline constexpr uintptr_t kBasePos = 0x6f0, kBaseRow0 = 0x700;

template <class Reader> bool verify_write_sites(Reader read, uintptr_t *bad) {
    return camwatch::verify_sites(kWriteSites,
                                  sizeof kWriteSites / sizeof kWriteSites[0],
                                  read, bad);
}

// The parameters global, from the displacement in the verified instruction.
inline uintptr_t params_global() {
    int32_t d;
    memcpy(&d, kWriteSites[0].bytes + 3, 4);
    return kWriteSites[0].va + 7 + (intptr_t)d;
}

// Is what X4 loaded what our extensions asked for? `p` is maxpitch, maxyaw,
// anglespeed, angleacc as read. The limits must equal the servo's (75/180 with
// x4vrmod_lookaround), in degrees or radians -- whichever X4 stores, said in
// `radians`; that match is also what confirms the field layout live. The speed
// must be zero (x4vrmod_headlook), or X4's follower pulls every write home.
struct ParamCheck {
    bool ok = false;
    bool radians = false;
    const char *why = "not checked";
};
inline ParamCheck check_params(const float p[4], float plim_deg, float ylim_deg) {
    ParamCheck c;
    constexpr float d2r = 0.01745329251994330f;
    const bool deg = std::fabs(p[0] - plim_deg) < 0.01f &&
                     std::fabs(p[1] - ylim_deg) < 0.01f;
    const bool rad = std::fabs(p[0] - plim_deg * d2r) < 1e-3f &&
                     std::fabs(p[1] - ylim_deg * d2r) < 1e-3f;
    if (!deg && !rad) {
        c.why = "maxpitch/maxyaw do not read as the limits our extension set "
                "(neither degrees nor radians) -- the layout is unverified or "
                "x4vrmod_lookaround is not in force";
        return c;
    }
    c.radians = rad && !deg;
    if (!(std::fabs(p[2]) < 1e-4f)) {
        c.why = "anglespeed is not zero -- neither the injector's per-mode "
                "speed nor x4vrmod_headlook holds it, so X4's follower would "
                "pull every write back toward centre";
        return c;
    }
    c.ok = true;
    c.why = "limits match the servo's and anglespeed is zero";
    return c;
}

// **The follower's speed, per camera mode** (take 351). x4vrmod_headlook
// used to zero `<lookaround><internal> anglespeed` for the whole session so a
// written angle stays. The same pair also drives the external views' orbit
// (read at 0x19352b3/0x19352bd into the follower wrapper 0x1e8c260): take 350
// had the numpad pitch F2/F3 once and then freeze, and take 351, with the
// extension not served, had them orbit again. So X4 now loads its own value,
// and the injector holds it at zero only while the camera is in mode 0 -- the
// cockpit and walking, where the write acts -- and gives X4 its value back in
// every other mode.
struct SpeedOwner {
    bool have = false;    // X4's own value has been read
    float x4_value = 0.f; // ...and is this
    uint64_t to_zero = 0, to_x4 = 0;
    // The speed to hold for camera mode `mode`, or a negative value when
    // nothing is to be written yet (X4's own value not read).
    float wanted(int32_t mode) const {
        if (!have)
            return -1.f;
        return mode == 0 ? 0.f : x4_value;
    }
    // Remember X4's own value from the first reading that is X4's: the load
    // passes through all-zero defaults, and a zero after that is ours (or a
    // mod's), which cannot be given back as "X4's".
    bool learn(float anglespeed) {
        if (have || !(std::fabs(anglespeed) >= 1e-4f))
            return false;
        have = true;
        x4_value = anglespeed;
        return true;
    }
};

// The base the stored matrix is relative to must be identity for a written
// R(yaw, pitch) to mean that angle: X4 extracts from stored x base^-1. `b` is
// 16 floats from camera+0x6f0: translation, then rows 0..2 (x, y, z, w each).
inline bool base_is_identity(const float b[16], float tol) {
    for (int i = 0; i < 3; i++)
        if (std::fabs(b[i]) > tol)
            return false;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            if (std::fabs(b[4 + 4 * r + c] - (r == c ? 1.f : 0.f)) > tol)
                return false;
    return true;
}

// X4's convention, fitted to take 319's raw pairs (docs, "0x1933d00, read"):
// with psi and theta the yaw and pitch GetCameraRotation reports (radians),
//
//   row0 = ( cos psi,             0,          -sin psi           )
//   row1 = (-sin psi * sin theta, cos theta,  -cos psi * sin theta)
//   row2 = ( sin psi * cos theta, sin theta,   cos psi * cos theta)
//
// Our head-look yaw is the NEGATED camera yaw (camread prints -r.yaw), pitch is
// the same sign. `rows` is 12 floats (x, y, z, w per row); w is left as it was.
inline void rows_for(float head_yaw_deg, float head_pitch_deg, float rows[12]) {
    constexpr float d2r = 0.01745329251994330f;
    const float psi = -head_yaw_deg * d2r, th = head_pitch_deg * d2r;
    const float cy = std::cos(psi), sy = std::sin(psi);
    const float cp = std::cos(th), sp = std::sin(th);
    rows[0] = cy;
    rows[1] = 0.f;
    rows[2] = -sy;
    rows[4] = -sy * sp;
    rows[5] = cp;
    rows[6] = -cy * sp;
    rows[8] = sy * cp;
    rows[9] = sp;
    rows[10] = cy * cp;
}

// The angle to write, from the head. Pitch clamped to the limit; yaw clamped
// (or, at a 180 limit, wrapped -- X4 wraps there, take 318) and FADED near
// the vertical the same way the servo's yaw is (yaw_weight, 1 when level, 0
// straight up or down where atan2's yaw is noise): the written yaw moves only
// that fraction of the way toward the head's, the short way round.
inline float wrap180(float d) {
    while (d > 180.f)
        d -= 360.f;
    while (d < -180.f)
        d += 360.f;
    return d;
}
inline void write_target(float prev_yaw_deg, float head_yaw_deg,
                         float head_pitch_deg, float yaw_weight,
                         float ylim_deg, float plim_deg, float *yaw_out,
                         float *pitch_out) {
    float y = wrap180(prev_yaw_deg +
                      yaw_weight * wrap180(head_yaw_deg - prev_yaw_deg));
    if (ylim_deg < 180.f)
        y = y < -ylim_deg ? -ylim_deg : (y > ylim_deg ? ylim_deg : y);
    float p = head_pitch_deg;
    p = p < -plim_deg ? -plim_deg : (p > plim_deg ? plim_deg : p);
    *yaw_out = y;
    *pitch_out = p;
}

} // namespace camwrite
} // namespace x4vr
