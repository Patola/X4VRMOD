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
//   2. **Never command past the clamp.** X4 stops at **+-65.00 deg yaw and
//      +-35.00 deg pitch**, which is what `yaw_limit_deg`/`pitch_limit_deg`
//      below use, and they are X4's OWN numbers: `libraries/parameters.xml`
//      declares `<playercamera><lookaround><internal maxpitch="35"
//      maxyaw="65" .../>`. (This line said 56.5, from take 117, for long after
//      the constants were corrected.)
//
//      **The evidence is the OVERSHOOT, not the maximum.** Across 2398 live
//      `camread` samples the largest |X4 yaw| is 65.00 -- but the largest
//      |command| is also 65.00, so that pair on its own is circular: the
//      servo never asks for more. What is not circular is that 303 samples
//      show X4 going PAST the command and still pinning to exactly 65.00
//      (furthest: X4 65.00 against a command of 53.04), and 256 do the same in
//      pitch (35.00 against 26.83). A ceiling that is merely ours would be
//      sailed through by an overshoot. Commanding further would
//      wind the estimate up past where X4 actually is, and the view would then
//      lag by the whole accumulated excess on the way back. Clamping our own
//      target is the entire fix, and it needs no readback and no event
//      detection.
//   3. **Recentre events zero the estimate, not the head.** Seat changes and
//      save loads put X4 back at centre while our estimate is non-zero (both
//      measured in-game). The estimate must be reset from outside; the
//      arithmetic cannot see it happen.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace x4vr {

// A head orientation reduced to the two angles X4 can be driven in. Roll is
// dropped on purpose: free-look has no roll axis, so carrying it would invite
// someone to try.
struct HeadAngles {
    float yaw_deg = 0.0f;   // + = turning left (right-handed, Y up)
    float pitch_deg = 0.0f; // + = looking up
    // How much of the yaw reading to believe, 0..1, fading to zero as the head
    // approaches vertical. NOT a flag, and that matters: the flag it replaces
    // switched yaw tracking off in one step, so crossing the region froze the
    // yaw and re-crossing resumed it — the "jumps of rotations" Patola hit at
    // 85 deg of pitch in take 204.
    //
    // **Quaternions do not remove this.** X4's mouselook is a two-axis yaw/
    // pitch channel, so whatever representation the pose arrives in has to
    // land on (yaw, pitch), and that map is degenerate at the poles however it
    // is computed. Near vertical a tiny head wobble genuinely does swing the
    // yaw through a large angle, and the honest response is to stop believing
    // it — smoothly, rather than at a cliff.
    float yaw_weight = 1.0f;
    // Kept for callers that only need the yes/no. Derived, never set alone.
    bool yaw_reliable = true;
    // Rotation about the forward axis, + = tilting the head to the LEFT ear.
    // NOT for driving X4 -- free-look has no roll axis and never will. This
    // exists because the STEREO needs it: see head_angles() and task #77.
    // Shares yaw's singularity, and yaw_weight is the guard for both.
    float roll_deg = 0.0f;
};

// How much of the yaw reading to believe, from cos(pitch).
//
// A pure function of pitch, deliberately: the LAYER computes the head angles
// and only yaw/pitch cross the shared block, so the injector rebuilds
// HeadAngles from those two numbers alone. When the weight lived only inside
// head_angles() the injector's brace-init left it at its default 1.0 and the
// guard was never active in a real run at ALL -- not the fade, and not the
// hard `yaw_reliable` cliff before it. It ran in tests and nowhere else, which
// is why take 204 gimbal-locked: the servo was chasing raw noisy yaw all the
// way to vertical with nothing damping it.
//
// horiz is cos(pitch) exactly -- the forward vector is unit length, so
// sqrt(fx^2+fz^2) == sqrt(1 - fy^2) == cos(asin(fy)) -- and that is what makes
// the injector able to recover it without a wider channel.
//
// Full confidence to 70 deg of pitch (0.342), none at 84.8 (0.09). Smoothstep,
// so the derivative is continuous at both ends and not just the value.
inline float yaw_weight_of_horiz(float horiz) {
    const float lo = 0.09f, hi = 0.342f;
    float t = (horiz - lo) / (hi - lo);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

inline float yaw_weight_of_pitch_deg(float pitch_deg) {
    return yaw_weight_of_horiz(std::cos(pitch_deg * 0.01745329251994330f));
}

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
    a.yaw_weight = yaw_weight_of_horiz(std::sqrt(fx * fx + fz * fz));
    a.yaw_reliable = a.yaw_weight > 0.0f;

    // **Roll, which this function dropped for six months and should not have.**
    //
    // The comment above says a general Euler decomposition "would drag roll back
    // in through gimbal choices nobody needs to reason about", and that was
    // right for driving X4: free-look has no roll axis, so there was nothing to
    // send it to. But the STEREO needs it. The disparity is baked along image-x
    // and the compositor rotates the image by the head's full orientation, so
    // at roll phi a sin(phi) share of the disparity becomes vertical, which the
    // eye cannot fuse (task #77 -- Patola found it at 90 deg by closing one eye
    // at a time, and it is already a quarter of the disparity at 15).
    //
    // Measured about the FORWARD axis, against the roll-free frame, so it is
    // exactly the angle the eye offset has to be turned by:
    //
    //   u   = the head's own up,  q * (0,1,0)
    //   r0  = normalise(f x worldUp)   -- the roll-free right
    //   u0  = r0 x f                   -- the roll-free up
    //   roll = atan2(u . r0, u . u0)
    //
    // Degenerate exactly where yaw is, and for the same reason: pointing at the
    // ceiling, yaw and roll are the same rotation and the split between them is
    // arbitrary. yaw_weight already fades that region and is reused rather than
    // given a second spelling -- one predicate, one place.
    const float ux = 2.0f * (qx * qy - qw * qz);
    const float uy = 1.0f - 2.0f * (qx * qx + qz * qz);
    const float uz = 2.0f * (qy * qz + qw * qx);
    // r0 = f x worldUp, with worldUp = (0,1,0): (fy*0 - fz*1, fz*0 - fx*0, fx*1 - fy*0)
    float r0x = -fz, r0y = 0.0f, r0z = fx;
    const float r0n = std::sqrt(r0x * r0x + r0z * r0z);
    if (r0n > 1e-6f) {
        r0x /= r0n;
        r0z /= r0n;
        // u0 = r0 x f
        const float u0x = r0y * fz - r0z * fy;
        const float u0y = r0z * fx - r0x * fz;
        const float u0z = r0x * fy - r0y * fx;
        a.roll_deg = std::atan2(ux * r0x + uy * r0y + uz * r0z,
                                ux * u0x + uy * u0y + uz * u0z) *
                     57.2957795130823f;
    }
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

    // **Task #73: the lag is arithmetic, not latency, and this is the fix.**
    //
    // A proportional servo tracking a MOVING target sits behind it forever, by
    // a fixed fraction of its own step period. That is the defining property of
    // a type-0 loop, not a defect in the tuning. Measured across takes
    // 209/210/211 -- from `camread`, no run spent on it -- the lag was 41 ms at
    // an 85 Hz pose rate and 160-204 ms at 26.6 Hz. It SCALES with the period,
    // which is what rules out tracker latency, our render cost, and X4's own
    // `anglespeed`/`angleacc`: a fixed delay would not.
    //
    // In units of the step period the six measurements were 3.3 to 5.4, and
    // (1 - kp)/kp is exactly 3.0 at kp = 0.25.
    //
    // So aim at where the head WILL be rather than where it is. The lead is
    // (1/kp - 1) times the head's motion since the last pose -- a difference of
    // ANGLES, carrying no clock and no dt, which is why it needs no retuning
    // when the frame rate collapses. tests/servo_lag.cpp measures 36.8 -> 1.5 ms
    // at 85 Hz and 112 -> -0.5 ms at 26.6 Hz against this very function.
    //
    // **Feed-forward, not a gain change.** It is outside the loop, so it cannot
    // affect stability. Raising kp instead was measured and rejected: kp 0.5
    // buys 12 ms and costs +24.8% step overshoot, and X4's true input-to-camera
    // delay is unknown, so a gain that depends on it can fail in the field. The
    // same harness reports kp 1.0 as never settling with 46 deg/s of jitter --
    // take 136, which Patola saw as the screen spinning too fast to read.
    //
    // 0 disables it exactly, so every earlier run remains reproducible and the
    // A/B has a true control arm.
    float lead_scale = 0.0f;
    // Ceiling on the lead, in degrees.
    //
    // **This is a sanity bound, NOT the thing that bounds the damage.** A
    // corrupt pose can only ever command max_step_deg, because the step is
    // clamped to it after the error is formed -- so the guard against a glitch
    // was already there, and giving this the same value only broke honest
    // motion.
    //
    // It was 12 for exactly that mistaken reason and tests/servo_lag.cpp caught
    // it: at 26.6 Hz a 240 deg/s turn moves 9 deg between poses, so the correct
    // lead is 27, and clipping it at 12 left 63 ms of the lag in place at the
    // one frame rate the fix is most needed. A fixed angle cannot separate "a
    // fast head at a low frame rate" from "a bad sample" -- only a RATE can,
    // and the lead is deliberately dt-free. So this is set where no plausible
    // head reaches it: 45 deg is a 240 deg/s turn sampled at 16 Hz.
    float lead_max_deg = 45.0f;
    // The previous pose, for the difference above. NaN means "no previous" --
    // the first pose after arming or a recentre leads by nothing rather than by
    // the whole angle from zero.
    float last_head_yaw_deg = std::numeric_limits<float>::quiet_NaN();
    float last_head_pitch_deg = std::numeric_limits<float>::quiet_NaN();
};

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// One frame: given where the head is, what to send X4 and what we now believe.
inline Delta head_look_step(HeadLook &s, const HeadAngles &head) {
    Delta d;
    if (!(s.gain_deg_per_count > 0.0f))
        return d; // uncalibrated: send nothing rather than something arbitrary

    // **#73's lead: aim where the head will be, not where it is.**
    //
    // The head's motion since the last pose, times (1/kp - 1), is exactly the
    // distance a proportional servo would otherwise stay behind. See lead_scale
    // for the measurement this comes from.
    float lead_yaw = 0.0f, lead_pitch = 0.0f;
    if (s.lead_scale > 0.0f && s.servo_kp > 0.0f) {
        const float k = s.lead_scale * (1.0f / s.servo_kp - 1.0f);
        if (!std::isnan(s.last_head_yaw_deg)) {
            // **Wrapped, for the same reason err_yaw below is.** This is a NEW
            // subtraction of two yaws, so it carries the ±180 defect in its own
            // right: a head crossing the back would otherwise show a -358 deg
            // "motion" and the lead would command a full reverse spin -- the
            // failure the error term was already fixed for, reintroduced one
            // function later. Weighted too, because near vertical the yaw is
            // noise and its derivative is worse noise.
            float dy = head.yaw_deg - s.last_head_yaw_deg;
            while (dy > 180.0f)
                dy -= 360.0f;
            while (dy < -180.0f)
                dy += 360.0f;
            lead_yaw = clampf(k * dy * head.yaw_weight, -s.lead_max_deg,
                              s.lead_max_deg);
        }
        if (!std::isnan(s.last_head_pitch_deg))
            lead_pitch = clampf(k * (head.pitch_deg - s.last_head_pitch_deg),
                                -s.lead_max_deg, s.lead_max_deg);
    }
    // **Outside the branch on purpose.** The A/B in take 212 toggles lead_scale
    // mid-run; if this only advanced while the lead was armed, the first pose
    // after re-arming would difference against a head position from twenty
    // seconds ago and command the clamp.
    s.last_head_yaw_deg = head.yaw_deg;
    s.last_head_pitch_deg = head.pitch_deg;

    // Always the head's own yaw now. Substituting cmd_yaw when the reading was
    // unreliable made the error exactly zero, which is a step change in the
    // command the instant the gate flips; the weight below fades the step
    // instead, and reaches the same zero without the edge.
    const float want_yaw =
        clampf(head.yaw_deg + lead_yaw, -s.yaw_limit_deg, s.yaw_limit_deg);
    const float want_pitch =
        clampf(head.pitch_deg + lead_pitch, -s.pitch_limit_deg,
               s.pitch_limit_deg);
    // The LEAD must not be reported as the head hitting X4's stop -- it is our
    // own anticipation, and `clamped` drives the scorer's clamp measurement.
    d.clamped = clampf(head.yaw_deg, -s.yaw_limit_deg, s.yaw_limit_deg) !=
                    head.yaw_deg ||
                clampf(head.pitch_deg, -s.pitch_limit_deg, s.pitch_limit_deg) !=
                    head.pitch_deg;

    // **Yaw error takes the SHORT way round.**
    //
    // head_angles() gets yaw from atan2, so it lives in (-180, 180] and a head
    // that turns past the back jumps from +179 to -179. Unwrapped, that is an
    // error of -358 and the servo drives the camera all the way back the other
    // way at full step -- a whole reverse spin, exactly at the point a player
    // turning their chair reaches.
    //
    // Inert below a 90 deg limit: with want and cmd both inside +-65, the
    // error cannot reach 180 and this never fires. It only matters once the
    // clamp is opened up, which is why it was never seen before.
    float err_yaw = want_yaw - s.cmd_yaw_deg;
    while (err_yaw > 180.0f)
        err_yaw -= 360.0f;
    while (err_yaw < -180.0f)
        err_yaw += 360.0f;
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
    // Weighted, so the yaw command fades out as the head nears vertical
    // instead of being cut off. At weight 1 -- anything below 70 deg of pitch,
    // which is every take before 204 -- this is arithmetically identical to
    // what it replaced.
    if (head.yaw_weight > 0.0f && std::fabs(err_yaw) >= s.dead_zone_deg)
        d.dx = (int)std::lround(s.sign_yaw * step_yaw * head.yaw_weight /
                                s.gain_deg_per_count);
    if (std::fabs(err_pitch) >= s.dead_zone_deg)
        d.dy = (int)std::lround(s.sign_pitch * step_pitch / gp);

    // Integrate what was SENT. Rounding to whole counts leaves a residue, and
    // folding the *wanted* angle in here instead would accumulate it forever.
    s.cmd_yaw_deg += s.sign_yaw * (float)d.dx * s.gain_deg_per_count;
    s.cmd_pitch_deg += s.sign_pitch * (float)d.dy * gp;
    // Kept in the same (-180, 180] range the head angle uses, so the two are
    // comparable next frame. Without this the estimate drifts out of range and
    // the wrap above starts correcting against a number the head can never
    // report.
    while (s.cmd_yaw_deg > 180.0f)
        s.cmd_yaw_deg -= 360.0f;
    while (s.cmd_yaw_deg < -180.0f)
        s.cmd_yaw_deg += 360.0f;
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

    // **A stall must expire.** The first version resumed only when the camera
    // moved by itself -- but standing down releases the key and stops
    // commanding, so nothing CAN move it. Take 140 stalled during the menu at
    // startup and was still stalled in the cockpit minutes later, with head
    // tracking simply dead. A recovery condition that the recovery itself makes
    // impossible is a trap door.
    //
    // So a stall times out and we try again. The interval doubles up to a cap,
    // so a genuinely dead context settles into a rare, brief probe rather than a
    // constant one, and a live context is picked up within a second.
    int retry_frames = 90;
    int retry_cap_frames = 600;
    int stalled_for = 0;
    int retry_at = 0;

    float cmd_sum = 0.0f, obs_sum = 0.0f;
    int win = 0;
    bool stalled = false;
};

// Feed one frame. Returns +1 on the transition into a stall, -1 on the
// transition out, 0 otherwise.
inline int stall_watch_step(StallWatch &w, float commanded_deg,
                            float observed_deg) {
    if (w.stalled && ++w.stalled_for >= w.retry_at) {
        w.stalled = false;
        w.stalled_for = 0;
        w.cmd_sum = w.obs_sum = 0.0f;
        w.win = 0;
        return -1; // probe again; if it is still dead this costs one window
    }
    w.cmd_sum += std::fabs(commanded_deg);
    w.obs_sum += std::fabs(observed_deg);
    if (++w.win < w.window_frames)
        return 0;
    int event = 0;
    if (!w.stalled && w.cmd_sum > w.min_cmd_deg &&
        w.obs_sum < w.max_obs_frac * w.cmd_sum) {
        w.stalled = true;
        w.stalled_for = 0;
        w.retry_at = w.retry_at ? std::min(w.retry_at * 2, w.retry_cap_frames)
                                : w.retry_frames;
        event = 1;
    } else if (w.stalled && w.obs_sum > w.resume_obs_deg) {
        w.stalled = false;
        w.retry_at = 0; // it answered: forget the backoff
        event = -1;
    } else if (!w.stalled) {
        w.retry_at = 0;
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

// **Task #74: the head's displacement, resolved into the head's OWN axes.**
//
// The eye offset is applied in the camera's frame, so a room-space vector would
// have to be re-resolved every time the head turned -- lean left, then turn 90
// degrees, and an unrotated shift would push the view sideways in a direction
// that no longer means "left". Rotating by the inverse of the head orientation
// is what makes "I leaned left" mean the same thing at every heading.
//
// The caller supplies the displacement already measured from a neutral point;
// recentring, clamping and scaling all stay in the layer, because doing any of
// them here as well would put one number in two places.
struct HeadShift {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Head axes -> world. The inverse of head_shift, and it exists for #74's neck
// model: the pivot offset is stated in head axes and has to be rotated OUT to
// be subtracted from a world-space eye position.
//
// tests/headlook.cpp deliberately keeps its own copy of this arithmetic rather
// than calling here, so the round-trip case is an independent oracle instead of
// a function checked against itself.
inline HeadShift head_to_world(float qx, float qy, float qz, float qw, float vx,
                               float vy, float vz) {
    const float tx = 2.0f * (qy * vz - qz * vy);
    const float ty = 2.0f * (qz * vx - qx * vz);
    const float tz = 2.0f * (qx * vy - qy * vx);
    HeadShift r;
    r.x = vx + qw * tx + (qy * tz - qz * ty);
    r.y = vy + qw * ty + (qz * tx - qx * tz);
    r.z = vz + qw * tz + (qx * ty - qy * tx);
    return r;
}

inline HeadShift head_shift(float qx, float qy, float qz, float qw, float dx,
                            float dy, float dz) {
    // v' = v + 2w(u x v) + 2u x (u x v), with u the CONJUGATE vector part --
    // that is the inverse rotation, world-into-head, not head-into-world.
    // Getting it the other way round would still look like tracking, because
    // it moves when the head moves; it would just move wrongly the moment the
    // head is not facing forward, which is exactly the case a level-headed
    // test never reaches.
    const float ux = -qx, uy = -qy, uz = -qz;
    const float tx = 2.0f * (uy * dz - uz * dy);
    const float ty = 2.0f * (uz * dx - ux * dz);
    const float tz = 2.0f * (ux * dy - uy * dx);
    HeadShift r;
    r.x = dx + qw * tx + (uy * tz - uz * ty);
    r.y = dy + qw * ty + (uz * tx - ux * tz);
    r.z = dz + qw * tz + (ux * ty - uy * tx);
    return r;
}

} // namespace x4vr
