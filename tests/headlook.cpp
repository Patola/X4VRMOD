// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The head-look arithmetic, checked without X4, a headset or a GPU.
//
// Every failure mode #33 has lives in this file's subject rather than in the
// wiring around it, and each one below was a real worry before it was a test:
// rounding residue accumulating into the drift the design exists to avoid,
// integrator windup past a clamp we cannot see X4 hit, and a recentre event
// that leaves our estimate pointing somewhere X4 is not.
#include <cmath>
#include <cstdio>

#include "../common/x4vr_headlook.hpp"

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        g_fail++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// Quaternion for a yaw then a pitch, in the same right-handed Y-up frame
// OpenXR reports. Written from the definition rather than by calling the code
// under test, so the two can disagree.
static void q_of(float yaw_deg, float pitch_deg, float &x, float &y, float &z,
                 float &w) {
    const float cy = std::cos(yaw_deg * 0.008726646f);   // half angle, to rad
    const float sy = std::sin(yaw_deg * 0.008726646f);
    const float cp = std::cos(pitch_deg * 0.008726646f);
    const float sp = std::sin(pitch_deg * 0.008726646f);
    // q = yaw(Y) * pitch(X)
    w = cy * cp;
    x = cy * sp;
    y = sy * cp;
    z = -sy * sp;
}

int main() {
    // ---- the pose decomposition
    {
        x4vr::HeadAngles a = x4vr::head_angles(0, 0, 0, 1);
        check(near(a.yaw_deg, 0, 1e-4f) && near(a.pitch_deg, 0, 1e-4f),
              "identity orientation is dead ahead");

        const float cases[][2] = {{0, 0},   {30, 0},   {-45, 0}, {0, 20},
                                  {0, -35}, {56.5f, 0}, {25, -15}, {-70, 12}};
        float worst = 0.0f;
        for (const auto &c : cases) {
            float x, y, z, w;
            q_of(c[0], c[1], x, y, z, w);
            a = x4vr::head_angles(x, y, z, w);
            worst = std::fmax(worst, std::fabs(a.yaw_deg - c[0]));
            worst = std::fmax(worst, std::fabs(a.pitch_deg - c[1]));
        }
        check(worst < 1e-3f, "yaw and pitch recovered from the quaternion");
        printf("      worst decomposition error %.2e deg\n", worst);
    }

    // ---- quat_of_angles must invert head_angles exactly
    //
    // The layer submits the composition layer's orientation from this, so a
    // sign error would be invisible in every test and obvious only in the
    // headset, as a world that tilts the wrong way when the head turns.
    {
        const float cases[][2] = {{0, 0},    {30, 0},  {-45, 0},  {0, 20},
                                  {0, -35},  {56.5f, 0}, {25, -15}, {-56, 39}};
        float worst = 0.0f;
        for (const auto &c : cases) {
            float q[4];
            x4vr::quat_of_angles(c[0], c[1], q);
            const x4vr::HeadAngles a = x4vr::head_angles(q[0], q[1], q[2], q[3]);
            worst = std::fmax(worst, std::fabs(a.yaw_deg - c[0]));
            worst = std::fmax(worst, std::fabs(a.pitch_deg - c[1]));
            // A quaternion the runtime will accept must be unit length.
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                                      q[3] * q[3]);
            check(std::fabs(n - 1.0f) < 1e-5f, "quat_of_angles returns a unit quaternion");
        }
        check(worst < 1e-3f, "quat_of_angles round-trips through head_angles");
        printf("      worst round-trip error %.2e deg\n", worst);
    }

    // ---- the ceiling singularity
    //
    // Take 128: looking at the roof made yaw garbage, the estimate followed it,
    // and it stayed wrong after the head came back level -- the world leaned.
    {
        // Straight up: forward is (0, 1, 0), so yaw is undefined.
        float q[4];
        x4vr::quat_of_angles(0.0f, 89.9f, q);
        const x4vr::HeadAngles up = x4vr::head_angles(q[0], q[1], q[2], q[3]);
        check(!up.yaw_reliable, "yaw is flagged unreliable near the ceiling");

        x4vr::HeadLook s;
        for (int i = 0; i < 100; i++)
            x4vr::head_look_step(s, {40.0f, 0.0f});
        const float parked = s.cmd_yaw_deg;
        check(near(parked, 40.0f, 0.2f), "parked at 40 deg of yaw");

        // Now look up, with a yaw that swings wildly as the tracker noises.
        for (int i = 0; i < 50; i++) {
            x4vr::HeadAngles bad{(i % 2) ? 170.0f : -170.0f, 89.9f, false};
            x4vr::head_look_step(s, bad);
        }
        check(near(s.cmd_yaw_deg, parked, 0.05f),
              "a wildly swinging unreliable yaw does not move the estimate");
        check(s.cmd_pitch_deg > 0.0f, "but pitch still follows the head up");

        // And a level head is trusted again.
        const x4vr::Delta d = x4vr::head_look_step(s, {0.0f, 0.0f});
        check(d.dx != 0, "yaw is commanded again once the head is level");
    }

    // ---- an uncalibrated gain sends nothing, rather than something arbitrary
    {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.0f;
        x4vr::Delta d = x4vr::head_look_step(s, {30.0f, 0.0f});
        check(d.dx == 0 && d.dy == 0 && s.cmd_yaw_deg == 0.0f,
              "gain of zero commands nothing and moves no estimate");
    }

    // ---- it converges, and to the right place
    {
        x4vr::HeadLook s;
        // 100 steps, not 10: with servo_kp at 0.25 a step closes a quarter of
        // the error, so settling takes longer by design. The tolerance is the
        // dead zone -- the servo deliberately stops inside it rather than
        // hunting on tracker noise -- and asserting tighter than that would be
        // asserting a bug.
        for (int i = 0; i < 100; i++)
            x4vr::head_look_step(s, {20.0f, -10.0f});
        check(near(s.cmd_yaw_deg, 20.0f, 0.2f) &&
                  near(s.cmd_pitch_deg, -10.0f, 0.2f),
              "the estimate converges on the head angle");
    }

    // ---- THE drift test: rounding residue must not accumulate
    //
    // A gain that divides badly into the step, driven back and forth for a
    // long time, is exactly the case where folding the *wanted* angle into the
    // estimate instead of the *sent* one would build a slow drift -- the very
    // failure the open-loop design exists to avoid.
    {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.037f;
        for (int i = 0; i < 20000; i++) {
            const float head = 25.0f * std::sin(i * 0.05f);
            x4vr::head_look_step(s, {head, 0.0f});
        }
        // Park it and let it settle, then compare against the truth.
        for (int i = 0; i < 50; i++)
            x4vr::head_look_step(s, {0.0f, 0.0f});
        check(std::fabs(s.cmd_yaw_deg) < 0.2f,
              "20000 reversals leave no accumulated drift");
        printf("      residual after 20000 reversals: %.4f deg\n", s.cmd_yaw_deg);
    }

    // ---- THE windup test: the clamp must not be commanded past
    {
        x4vr::HeadLook s;
        // Head goes far past what X4 will give, and stays there a long time.
        x4vr::Delta d{};
        for (int i = 0; i < 500; i++)
            d = x4vr::head_look_step(s, {170.0f, 0.0f});
        check(d.clamped, "the step reports that the head outran X4");
        check(near(s.cmd_yaw_deg, s.yaw_limit_deg, 0.2f),
              "the estimate stops at the clamp instead of winding up");
        check(s.cmd_yaw_deg <= s.yaw_limit_deg + 1e-3f,
              "and never goes past it, which is the whole of the windup fix");

        // Now the head comes back to centre. If the estimate had wound up to
        // 170 deg, this would spend ~113 deg of commands going nowhere and the
        // view would lag the head by that much the whole way back.
        // The bound is now set by servo_kp, not by windup: a quarter of the
        // error per step needs ~20 steps to cover 56 deg. What matters is that
        // it is a function of Kp and NOT of how far past the clamp the head
        // went -- 170 deg of excursion must cost no more than 57 does.
        int steps_to_return = 0;
        while (std::fabs(s.cmd_yaw_deg) > 0.25f && steps_to_return < 200) {
            x4vr::head_look_step(s, {0.0f, 0.0f});
            steps_to_return++;
        }
        x4vr::HeadLook mild;
        for (int i = 0; i < 500; i++)
            x4vr::head_look_step(mild, {56.5f, 0.0f}); // exactly at the clamp
        int mild_steps = 0;
        while (std::fabs(mild.cmd_yaw_deg) > 0.25f && mild_steps < 200) {
            x4vr::head_look_step(mild, {0.0f, 0.0f});
            mild_steps++;
        }
        check(steps_to_return <= mild_steps + 1,
              "a 170 deg excursion returns no slower than a 56.5 deg one");
        printf("      return from 170 deg: %d steps; from the clamp: %d\n",
               steps_to_return, mild_steps);
        printf("      steps to return from a 170 deg excursion: %d\n",
               steps_to_return);
    }

    // ---- the servo must not oscillate, which is what take 136 did
    //
    // Closed-loop, the estimate is REPLACED by an observation each frame. If the
    // step applies the whole error, the correction is re-sent before the camera
    // has finished responding and it rings -- X4 span fast enough that Patola
    // could not read the screen. This drives it against a plant that only ever
    // delivers HALF of what is commanded, which is the worst case for a
    // proportional loop, and requires it to settle rather than diverge.
    {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        float actual = -65.0f; // start pinned at the clamp, as take 136 did
        float worst = 0.0f;
        for (int i = 0; i < 200; i++) {
            x4vr::head_look_observe(s, actual, 0.0f);
            const x4vr::Delta d = x4vr::head_look_step(s, {5.0f, 0.0f});
            const float commanded =
                s.sign_yaw * (float)d.dx * s.gain_deg_per_count;
            actual += 0.5f * commanded;            // sluggish plant
            actual = x4vr::clampf(actual, -65.0f, 65.0f); // X4's own clamp
            if (i > 100)
                worst = std::fmax(worst, std::fabs(actual - 5.0f));
        }
        check(worst < 1.0f, "the servo settles on the head instead of ringing");
        printf("      steady-state error against a half-strength plant: "
               "%.3f deg\n", worst);
    }

    // ---- one frame cannot cross the range
    {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        x4vr::head_look_observe(s, -65.0f, 0.0f);
        const x4vr::Delta d = x4vr::head_look_step(s, {65.0f, 0.0f});
        const float commanded =
            std::fabs((float)d.dx * s.gain_deg_per_count);
        check(commanded <= s.max_step_deg + 0.5f,
              "a 130 deg error still commands at most max_step_deg");
        printf("      worst single-frame command: %.2f deg\n", commanded);
    }

    // ---- the dead zone holds still
    {
        x4vr::HeadLook s;
        s.cmd_yaw_deg = 10.0f;
        const x4vr::Delta d = x4vr::head_look_step(s, {10.05f, 0.0f});
        check(d.dx == 0 && s.cmd_yaw_deg == 10.0f,
              "sub-dead-zone head motion commands nothing");
    }

    // ---- the sign CONVENTION, not just its invertibility
    //
    // The previous test only checked that flipping the knob flips the command,
    // which was true while the default was wrong. This pins the direction:
    // head_angles() calls +yaw "left" and mouse +xrel turns right, so a
    // positive head yaw must command a NEGATIVE dx. Take 126 shipped the
    // opposite and the world rotated with the head instead of standing still.
    {
        x4vr::HeadLook s;
        const x4vr::Delta yaw = x4vr::head_look_step(s, {10.0f, 0.0f});
        check(yaw.dx < 0, "+yaw (head left) commands -dx (mouse left)");
        x4vr::HeadLook t;
        const x4vr::Delta pitch = x4vr::head_look_step(t, {0.0f, 10.0f});
        check(pitch.dy < 0, "+pitch (head up) commands -dy (mouse up)");
        check(s.cmd_yaw_deg > 0.0f && t.cmd_pitch_deg > 0.0f,
              "and the estimate still follows the head, not the mouse");
    }

    // ---- sign inversion is a knob, not a rewrite
    {
        x4vr::HeadLook a, b;
        b.sign_yaw = -a.sign_yaw; // relative to the default, whatever it is --
                                  // hardcoding -1 made this pass trivially once
                                  // -1 became the default, testing nothing.
        const x4vr::Delta da = x4vr::head_look_step(a, {15.0f, 0.0f});
        const x4vr::Delta db = x4vr::head_look_step(b, {15.0f, 0.0f});
        check(da.dx == -db.dx && da.dx != 0,
              "inverting sign_yaw inverts the command");
        check(near(a.cmd_yaw_deg, b.cmd_yaw_deg, 1e-4f),
              "and both still believe the camera went the same way");
    }

    // ---- recentre: X4 went to zero without telling us
    {
        x4vr::HeadLook s;
        for (int i = 0; i < 100; i++)
            x4vr::head_look_step(s, {30.0f, 0.0f});
        check(near(s.cmd_yaw_deg, 30.0f, 0.2f), "parked at 30 deg");
        x4vr::head_look_recentre(s);
        check(s.cmd_yaw_deg == 0.0f && s.cmd_pitch_deg == 0.0f,
              "a recentre event zeroes the estimate");
        // With the head still at 30, the next step must drive back out to 30
        // rather than assume it is already there.
        const x4vr::Delta d = x4vr::head_look_step(s, {30.0f, 0.0f});
        check(d.dx != 0, "and the next step drives back out to the head");
    }

    // ---- pitch is clamped by its own, unmeasured limit
    {
        x4vr::HeadLook s;
        for (int i = 0; i < 200; i++)
            x4vr::head_look_step(s, {0.0f, 89.0f});
        check(near(s.cmd_pitch_deg, s.pitch_limit_deg, 0.2f),
              "pitch stops at its own limit, not yaw's");
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
