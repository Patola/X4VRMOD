// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_headlook.hpp — head pose to X4 free-look, the arithmetic.
//
// Pure and dependency-free so it can be tested without X4, a headset or a GPU.
// The plumbing around it (the share channel, the SDL hooks) is elsewhere; what
// is here is the part that has to be *right*, because every failure mode #33
// has is in this arithmetic rather than in the wiring.
//
// **The channel is relative and there is no readback.** X4 takes free-look from
// INPUT_RANGE_MOUSELOOK_YAW/PITCH, which are mouse-axis deltas, and nothing
// tells us the angle it ended up at. That would normally mean drift, and drift
// with no way to correct it. Take 115 is what makes it workable: a held offset
// repeated *to the pixel* across eight dumps spanning ~13 s, so X4 integrates
// exactly and never settles or decays. `angle = k * sum(deltas)` therefore
// holds open-loop and indefinitely, and we can carry our own estimate of where
// X4 is pointing without ever asking.
//
// Three things follow, and each is a test below:
//
//   1. **Integrate what was SENT, not what was wanted.** Mouse deltas are whole
//      counts, so the command is rounded. Adding the *desired* angle to the
//      estimate would accumulate the rounding residue forever -- a slow drift
//      built by the very code that exists to avoid drift.
//   2. **Never command past the clamp.** X4 stops at +56.5 deg (take 117,
//      thirteen dumps of exactly zero at snr ~625). Commanding further would
//      wind the estimate up past where X4 actually is, and the view would then
//      lag by the whole accumulated excess on the way back. Clamping our own
//      target is the entire fix, and it needs no readback and no event
//      detection.
//   3. **Recentre events zero the estimate, not the head.** Seat changes and
//      save loads put X4 back at centre while our estimate is non-zero (both
//      measured in-game). The estimate must be reset from outside; the
//      arithmetic cannot see it happen.
#pragma once

#include <cmath>

namespace x4vr {

// A head orientation reduced to the two angles X4 can be driven in. Roll is
// dropped on purpose: free-look has no roll axis, so carrying it would invite
// someone to try.
struct HeadAngles {
    float yaw_deg = 0.0f;   // + = turning left (right-handed, Y up)
    float pitch_deg = 0.0f; // + = looking up
};

// OpenXR poses are right-handed, Y up, -Z forward. Decompose the forward axis
// rather than converting to Euler angles generally: we want "where is the nose
// pointing", which is exactly the forward vector, and a general decomposition
// would drag roll back in through gimbal choices nobody needs to reason about.
inline HeadAngles head_angles(float qx, float qy, float qz, float qw) {
    // forward = q * (0, 0, -1) * q^-1
    const float fx = -2.0f * (qx * qz + qw * qy);
    const float fy = -2.0f * (qy * qz - qw * qx);
    const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    HeadAngles a;
    a.yaw_deg = std::atan2(-fx, -fz) * 57.2957795130823f;
    const float s = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    a.pitch_deg = std::asin(s) * 57.2957795130823f;
    return a;
}

struct Delta {
    int dx = 0;
    int dy = 0;
    bool clamped = false; // the head asked for more than X4 will give
};

struct HeadLook {
    // Degrees of X4 camera rotation per mouse count. NOT known: take 115's
    // +24.05 deg came from an uncalibrated hand push, so it anchors nothing.
    // The first synthesis run measures it -- command a known count, read the
    // angle back with camera_rotation.py --integrate -- and until then this is
    // a placeholder the log must state as such.
    float gain_deg_per_count = 0.05f;

    // Measured, take 117: yaw stops dead at +56.5 deg.
    float yaw_limit_deg = 56.5f;
    // NOT measured. Pitch sat at +19 deg for the whole of take 117 and was
    // never walked to its limit, so this is deliberately conservative rather
    // than a number pretending to be a measurement.
    float pitch_limit_deg = 40.0f;

    // Below this the head is holding still and commanding anything would just
    // inject tracker noise into the camera.
    float dead_zone_deg = 0.15f;

    float sign_yaw = 1.0f;   // set from the calibration run, not guessed
    float sign_pitch = 1.0f;

    // Where we believe X4 is pointing. Advanced only by what we actually sent.
    float cmd_yaw_deg = 0.0f;
    float cmd_pitch_deg = 0.0f;
};

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// One frame: given where the head is, what to send X4 and what we now believe.
inline Delta head_look_step(HeadLook &s, const HeadAngles &head) {
    Delta d;
    if (!(s.gain_deg_per_count > 0.0f))
        return d; // uncalibrated: send nothing rather than something arbitrary

    const float want_yaw = clampf(head.yaw_deg, -s.yaw_limit_deg, s.yaw_limit_deg);
    const float want_pitch =
        clampf(head.pitch_deg, -s.pitch_limit_deg, s.pitch_limit_deg);
    d.clamped = want_yaw != head.yaw_deg || want_pitch != head.pitch_deg;

    const float err_yaw = want_yaw - s.cmd_yaw_deg;
    const float err_pitch = want_pitch - s.cmd_pitch_deg;

    if (std::fabs(err_yaw) >= s.dead_zone_deg)
        d.dx = (int)std::lround(s.sign_yaw * err_yaw / s.gain_deg_per_count);
    if (std::fabs(err_pitch) >= s.dead_zone_deg)
        d.dy = (int)std::lround(s.sign_pitch * err_pitch / s.gain_deg_per_count);

    // Integrate what was SENT. Rounding to whole counts leaves a residue, and
    // folding the *wanted* angle in here instead would accumulate it forever.
    s.cmd_yaw_deg += s.sign_yaw * (float)d.dx * s.gain_deg_per_count;
    s.cmd_pitch_deg += s.sign_pitch * (float)d.dy * s.gain_deg_per_count;
    return d;
}

// X4 has put its own camera back to centre -- a seat change or a save load,
// both measured in-game. Our estimate has to follow it there, and no amount of
// arithmetic here can notice: it must be called from whatever detects the
// event.
inline void head_look_recentre(HeadLook &s) {
    s.cmd_yaw_deg = 0.0f;
    s.cmd_pitch_deg = 0.0f;
}

} // namespace x4vr
