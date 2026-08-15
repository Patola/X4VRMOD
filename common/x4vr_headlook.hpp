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
    // False when the head is looking near-vertically and yaw is a singularity.
    // See head_angles(); the caller must hold its previous yaw rather than
    // follow this one.
    bool yaw_reliable = true;
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

    // **Yaw is a singularity when the nose points at the ceiling.** Both fx and
    // fz collapse to zero there, so atan2 returns whatever the noise in the
    // tracker happens to be, swinging through the whole circle for a head that
    // is barely moving.
    //
    // Take 128 found it the expensive way: Patola looked up at the roof, the
    // garbage yaw drove the estimate somewhere arbitrary, and it STAYED there --
    // he came back to level and the world was leaned down and to the left.
    // Clamping the commanded angle does not help, because the corrupt value is
    // inside the clamp.
    const float horiz = std::sqrt(fx * fx + fz * fz);
    a.yaw_reliable = horiz > 0.09f; // ~85 deg of pitch
    return a;
}

// The inverse of head_angles(): the orientation X4's camera is at, given the
// yaw and pitch we drove it to. The layer needs this because the composition
// layer must declare the pose the image was RENDERED from -- see the submission
// site for why identity was right until head-look existed and is wrong now.
inline void quat_of_angles(float yaw_deg, float pitch_deg, float q[4]) {
    const float cy = std::cos(yaw_deg * 0.008726646f);   // half angle, degrees
    const float sy = std::sin(yaw_deg * 0.008726646f);
    const float cp = std::cos(pitch_deg * 0.008726646f);
    const float sp = std::sin(pitch_deg * 0.008726646f);
    q[0] = cy * sp;  // x
    q[1] = sy * cp;  // y
    q[2] = -sy * sp; // z
    q[3] = cy * cp;  // w
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
    // Pitch has its OWN gain. Take 135 read X4's camera back directly and the
    // two axes disagree by 7x: at the same moment we believed 40.46 deg of yaw
    // and 9.43 of pitch, X4 reported 0.796 of the yaw and 0.109 of the pitch.
    // X4 scales its mouse axes separately, so a single number was always going
    // to be wrong on one of them. Zero means "use the yaw gain", which keeps
    // every existing caller behaving as before.
    float gain_pitch_deg_per_count = 0.0f;

    // **Both measured directly, from X4's own readback.** Take 136 drove the
    // camera into its stops on both axes and GetCameraRotation reported exactly
    // +-65.00 and +-35.00 -- round numbers, so these are X4's configured limits
    // rather than an artefact.
    //
    // Both previous values were wrong, and in opposite directions. Yaw was
    // 56.5, inferred in take 117 by integrating image correlation, which
    // under-read it by 8.5 deg and cost that much range for nothing. Pitch was
    // 40, an admitted placeholder, which over-read it by 5 and let the servo
    // push against a wall. Direct measurement beat both, which is the argument
    // for the readback in miniature.
    float yaw_limit_deg = 65.0f;
    float pitch_limit_deg = 35.0f;

    // Below this the head is holding still and commanding anything would just
    // inject tracker noise into the camera.
    float dead_zone_deg = 0.15f;

    // **Proportional gain of the servo, and it must be well under 1.**
    //
    // Take 136 closed the loop with an implicit Kp of 1.0 -- the whole error
    // corrected in a single frame -- and X4 span so fast Patola could not read
    // the screen. The log shows why: at head +3.22 with the camera at -65, the
    // step commanded -593 counts, slammed into X4's clamp at +65, and reversed.
    // A controller cannot apply a full correction every frame when the thing it
    // corrects takes more than a frame to respond; it just re-sends the
    // correction before the last one has landed.
    float servo_kp = 0.25f;

    // Hard ceiling on how far one frame may command, whatever the error. The
    // clamp turns overshoot into a bounce, so this is what keeps a single bad
    // observation from crossing the entire range before the next one arrives.
    float max_step_deg = 12.0f;

    // **Both negative, and take 126 is why.** head_angles() reports +yaw as
    // turning LEFT and +pitch as looking UP, which is right for a right-handed
    // Y-up frame. Mouse axes disagree with both: positive xrel turns the view
    // RIGHT, and positive yrel is DOWN because screen y grows downward. So a
    // left head-turn was commanding a right camera-turn, and Patola saw the
    // cabin rotate a further 30 degrees the same way he had turned instead of
    // holding still.
    //
    // Neither test in this file could catch it. They check that the estimate
    // converges and that inverting the sign inverts the command -- both true
    // under either convention. Only the headset knew, which is why the
    // convention is now written down here rather than implied by a default.
    float sign_yaw = -1.0f;
    float sign_pitch = -1.0f;

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

    const float want_yaw =
        head.yaw_reliable ? clampf(head.yaw_deg, -s.yaw_limit_deg, s.yaw_limit_deg)
                          : s.cmd_yaw_deg;
    const float want_pitch =
        clampf(head.pitch_deg, -s.pitch_limit_deg, s.pitch_limit_deg);
    d.clamped = want_yaw != head.yaw_deg || want_pitch != head.pitch_deg;

    const float err_yaw = want_yaw - s.cmd_yaw_deg;
    const float err_pitch = want_pitch - s.cmd_pitch_deg;

    // A head looking straight up has no meaningful yaw, so hold the last one
    // rather than chase a number that is pure noise.
    const float gp = s.gain_pitch_deg_per_count > 0.0f
                         ? s.gain_pitch_deg_per_count
                         : s.gain_deg_per_count;
    const float step_yaw =
        clampf(s.servo_kp * err_yaw, -s.max_step_deg, s.max_step_deg);
    const float step_pitch =
        clampf(s.servo_kp * err_pitch, -s.max_step_deg, s.max_step_deg);
    if (head.yaw_reliable && std::fabs(err_yaw) >= s.dead_zone_deg)
        d.dx = (int)std::lround(s.sign_yaw * step_yaw / s.gain_deg_per_count);
    if (std::fabs(err_pitch) >= s.dead_zone_deg)
        d.dy = (int)std::lround(s.sign_pitch * step_pitch / gp);

    // Integrate what was SENT. Rounding to whole counts leaves a residue, and
    // folding the *wanted* angle in here instead would accumulate it forever.
    s.cmd_yaw_deg += s.sign_yaw * (float)d.dx * s.gain_deg_per_count;
    s.cmd_pitch_deg += s.sign_pitch * (float)d.dy * gp;
    return d;
}

// Is the camera we command the camera we are reading?
//
// Outside the cockpit X4 reports 0.00,0.00 from GetCameraRotation while
// something else consumes the mouse deltas, so the servo pushes into a void and
// accelerates -- opening the map made the view spin.
//
// **The obvious test is worthless and was shipped once.** "We commanded
// something and the camera did not move" is precisely what a CONVERGED servo
// looks like with the head held still: it stood down after 0.66 s of perfect
// tracking, then thrashed 23 times in one run. What separates the two cases is
// the RATIO over a window -- a stall is having asked for a lot and received
// almost none, and a converged servo never asks for a lot.
struct StallWatch {
    int window_frames = 60;   // ~0.7 s at headset rate
    float min_cmd_deg = 15.0f;  // below this the window says nothing either way
    float max_obs_frac = 0.15f; // received less than this share of what we asked
    float resume_obs_deg = 2.0f; // moved on its own: a live context is back

    float cmd_sum = 0.0f, obs_sum = 0.0f;
    int win = 0;
    bool stalled = false;
};

// Feed one frame. Returns +1 on the transition into a stall, -1 on the
// transition out, 0 otherwise.
inline int stall_watch_step(StallWatch &w, float commanded_deg,
                            float observed_deg) {
    w.cmd_sum += std::fabs(commanded_deg);
    w.obs_sum += std::fabs(observed_deg);
    if (++w.win < w.window_frames)
        return 0;
    int event = 0;
    if (!w.stalled && w.cmd_sum > w.min_cmd_deg &&
        w.obs_sum < w.max_obs_frac * w.cmd_sum) {
        w.stalled = true;
        event = 1;
    } else if (w.stalled && w.obs_sum > w.resume_obs_deg) {
        w.stalled = false;
        event = -1;
    }
    w.cmd_sum = w.obs_sum = 0.0f;
    w.win = 0;
    return event;
}

// X4's camera, as X4 reports it. This is the whole point of the readback: the
// estimate stops being a belief and becomes an observation, so the gain only has
// to be roughly right, the clamp is seen rather than modelled, and a recentre
// on a seat change or save load corrects itself on the next frame instead of
// leaving a permanent offset.
inline void head_look_observe(HeadLook &s, float yaw_deg, float pitch_deg) {
    s.cmd_yaw_deg = yaw_deg;
    s.cmd_pitch_deg = pitch_deg;
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
