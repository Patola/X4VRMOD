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
                                  {0, -35}, {65.0f, 0}, {25, -15}, {-70, 12}};
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
                                  {0, -35},  {65.0f, 0}, {25, -15}, {-56, 39}};
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
        // Spinning past the back: the head crosses +180 and atan2 reports
        // -179. Unwrapped that is an error of -358 and the servo drives a full
        // reverse spin, which is exactly what a player turning their chair
        // would hit. The short way is 2 deg.
        {
            x4vr::HeadLook spin;
            spin.gain_deg_per_count = 0.115f;
            spin.yaw_limit_deg = 180.0f;
            spin.pitch_limit_deg = 70.0f;
            for (int i = 0; i < 400; i++)
                x4vr::head_look_step(spin, {179.0f, 0.0f, true});
            const float before = spin.cmd_yaw_deg;
            const x4vr::Delta d = x4vr::head_look_step(spin, {-179.0f, 0.0f, true});
            check(std::fabs(before - 179.0f) < 2.0f,
                  "spin: servo reaches +179 before the crossing");
            // The short way is NEGATIVE-going by 2 deg, so a handful of counts.
            // The long way would be ~358 deg, i.e. a step pinned at max.
            check(std::abs(d.dx) < 40,
                  "spin: crossing +-180 takes the short way, not a reverse spin");
            printf("      crossing 180: cmd %.1f -> step dx=%d\n", before, d.dx);
        }
        // The defect take 204 hit: the yaw gate was a cliff, so crossing it
        // froze the yaw and re-crossing resumed it -- "jumps of rotations".
        // Sweep pitch through the fade with a constant yaw error and assert
        // the command never jumps between adjacent samples.
        {
            int prev = 0; int worst = 0; bool first = true;
            for (float pitch = 60.0f; pitch <= 89.0f; pitch += 0.25f) {
                // forward vector for this pitch, yaw fixed at 30 deg
                const float p = pitch * 3.14159265f / 180.0f;
                const float y = 30.0f * 3.14159265f / 180.0f;
                const float fy = std::sin(p), ch = std::cos(p);
                const float fx = -ch * std::sin(y), fz = -ch * std::cos(y);
                // reconstruct HeadAngles the way head_angles() would
                x4vr::HeadAngles a;
                a.yaw_deg = std::atan2(-fx, -fz) * 57.2957795130823f;
                a.pitch_deg = std::asin(fy) * 57.2957795130823f;
                const float horiz = std::sqrt(fx * fx + fz * fz);
                float t = (horiz - 0.09f) / (0.342f - 0.09f);
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                a.yaw_weight = t * t * (3.0f - 2.0f * t);
                a.yaw_reliable = a.yaw_weight > 0.0f;
                x4vr::HeadLook s2;
                s2.gain_deg_per_count = 0.115f;
                s2.yaw_limit_deg = 180.0f;
                s2.pitch_limit_deg = 89.0f;
                const x4vr::Delta d = x4vr::head_look_step(s2, a);
                if (!first && std::abs(d.dx - prev) > worst)
                    worst = std::abs(d.dx - prev);
                prev = d.dx; first = false;
            }
            check(worst <= 4,
                  "pitch sweep through the fade makes no yaw jump");
            printf("      worst adjacent dx change across 60-89 deg: %d\n",
                   worst);
        }
        // The weight must be RECOVERABLE FROM PITCH ALONE, because that is
        // all the injector has: only yaw and pitch cross the shared block, and
        // when the weight lived solely inside head_angles() the injector's
        // brace-init left it at 1.0 and the guard never applied in a real run.
        // If these two ever disagree, the injector is silently unguarded again.
        {
            float worst = 0.0f;
            for (float p = 0.0f; p <= 89.0f; p += 0.5f) {
                const float pr = p * 3.14159265358979f / 180.0f;
                const float fy = std::sin(pr), ch = std::cos(pr);
                const float fx = -ch * 0.5f, fz = -ch * 0.8660254f;
                const x4vr::HeadAngles a = x4vr::head_angles(
                    /*qx*/ 0, 0, 0, 1);  // unused; build the weight directly
                (void)a;
                const float from_vec = x4vr::yaw_weight_of_horiz(
                    std::sqrt(fx * fx + fz * fz));
                const float from_pitch = x4vr::yaw_weight_of_pitch_deg(p);
                worst = std::max(worst, std::fabs(from_vec - from_pitch));
                (void)fy;
            }
            check(worst < 1e-4f,
                  "yaw weight from pitch matches the forward vector");
            printf("      worst weight mismatch over 0-89 deg: %.2e\n", worst);
        }
        x4vr::HeadLook mild;
        for (int i = 0; i < 500; i++)
            x4vr::head_look_step(mild, {65.0f, 0.0f}); // exactly at the clamp (65, not 56.5)
        int mild_steps = 0;
        while (std::fabs(mild.cmd_yaw_deg) > 0.25f && mild_steps < 200) {
            x4vr::head_look_step(mild, {0.0f, 0.0f});
            mild_steps++;
        }
        check(steps_to_return <= mild_steps + 1,
              "a 170 deg excursion returns no slower than one at the 65 deg clamp");
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

    // ---- the stall watch must not mistake success for failure
    //
    // This is the test the shipped version needed and did not have. A converged
    // servo commands little and moves little, which the first implementation
    // read as a stall: it stood down after 0.66 s of perfect tracking and
    // thrashed 23 times in one run, so the cockpit never tracked at all.
    {
        x4vr::StallWatch w;
        // A converged servo: tiny commands, tiny motion, for a long time.
        int events = 0;
        for (int i = 0; i < 2000; i++)
            events += (stall_watch_step(w, 0.03f, 0.02f) != 0);
        check(events == 0 && !w.stalled,
              "a converged servo never stalls, however long it runs");

        // A real stall: commanding hard into something that does not move.
        // It re-stalls after each retry probe, which is intended -- but the
        // backoff must keep that RARE. Over 300 frames of a dead camera a
        // fixed-interval retry would stall ~4 times; the doubling keeps it to 2.
        x4vr::StallWatch v;
        int in = 0;
        for (int i = 0; i < 300; i++)
            if (stall_watch_step(v, 1.0f, 0.0f) == 1)
                in++;
        check(v.stalled && in >= 1, "commanding into a dead camera stalls");
        check(in <= 3, "and the backoff keeps re-stalling rare");
        printf("      stalls in 300 frames of a dead camera: %d\n", in);

        // And it comes back when the camera moves on its own.
        int out = 0;
        for (int i = 0; i < 300; i++)
            if (stall_watch_step(v, 0.0f, 0.5f) == -1)
                out++;
        check(!v.stalled && out == 1, "and resumes once when it answers again");

        // A slow window with almost no command must not decide anything: the
        // ambiguous case has to stay ambiguous rather than guess.
        x4vr::StallWatch q;
        for (int i = 0; i < 600; i++)
            stall_watch_step(q, 0.01f, 0.0f);
        check(!q.stalled, "a window with too little command decides nothing");

        // **A stall must expire.** Standing down releases the key and stops
        // commanding, so the camera cannot move on its own -- take 140 stalled
        // in the menu and was still stalled in the cockpit, tracking dead. A
        // recovery condition the recovery itself prevents is a trap door.
        x4vr::StallWatch t;
        for (int i = 0; i < 300; i++)
            stall_watch_step(t, 1.0f, 0.0f);
        check(t.stalled, "stalled against a dead camera");
        // The servo keeps WANTING to move while stood down -- steering is off,
        // but the error is still there -- so the watch keeps seeing commands.
        // What it must never do is stay stalled forever when the camera cannot
        // move on its own, which is precisely take 140.
        int retried = 0;
        for (int i = 0; i < 8000 && retried < 3; i++)
            if (stall_watch_step(t, 1.0f, 0.0f) == -1)
                retried++;
        check(retried >= 3, "and retries, so a dead context is never permanent");
        printf("      retries observed while nothing ever answers: %d\n",
               retried);
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

    // ---- task #77: roll comes back out of the quaternion ------------------
    //
    // head_angles() dropped roll on purpose for as long as X4's camera was the
    // only consumer, because free-look has no roll axis. The STEREO needs it:
    // the disparity is baked along image-x and the compositor rotates the image
    // by the head's full orientation, so sin(roll) of it goes vertical.
    //
    // Round-tripped against quaternions built the other way, at every
    // combination rather than one convenient angle -- a sign error or an axis
    // swap survives a single case easily, and this one has to be exact because
    // the eye offset is turned by whatever comes out.
    {
        auto q_of = [](float yaw, float pitch, float roll, float q[4]) {
            const float d = 0.008726646f; // half angle, degrees
            const float cy = std::cos(yaw * d), sy = std::sin(yaw * d);
            const float cp = std::cos(pitch * d), sp = std::sin(pitch * d);
            const float cr = std::cos(roll * d), sr = std::sin(roll * d);
            const float a[4] = {0, sy, 0, cy};   // yaw about +Y
            const float b[4] = {sp, 0, 0, cp};   // pitch about +X
            const float c[4] = {0, 0, -sr, cr};  // roll about -Z (forward)
            auto mul = [](const float x[4], const float y[4], float o[4]) {
                o[0] = x[3]*y[0] + x[0]*y[3] + x[1]*y[2] - x[2]*y[1];
                o[1] = x[3]*y[1] - x[0]*y[2] + x[1]*y[3] + x[2]*y[0];
                o[2] = x[3]*y[2] + x[0]*y[1] - x[1]*y[0] + x[2]*y[3];
                o[3] = x[3]*y[3] - x[0]*y[0] - x[1]*y[1] - x[2]*y[2];
            };
            float t[4];
            mul(a, b, t);
            mul(t, c, q);
        };
        int bad = 0, n = 0;
        for (float yaw : {0.0f, 30.0f, -50.0f})
            for (float pitch : {0.0f, 20.0f, -35.0f})
                for (float roll : {0.0f, 15.0f, -45.0f, 90.0f}) {
                    float q[4];
                    q_of(yaw, pitch, roll, q);
                    const x4vr::HeadAngles a =
                        x4vr::head_angles(q[0], q[1], q[2], q[3]);
                    n++;
                    if (!near(a.roll_deg, roll, 0.05f) ||
                        !near(a.yaw_deg, yaw, 0.05f) ||
                        !near(a.pitch_deg, pitch, 0.05f))
                        bad++;
                }
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "yaw/pitch/roll round-trip exactly (%d of %d)", n - bad, n);
        check(bad == 0, buf);

        // Level head, no roll -- the case that must not acquire one by
        // accident, because every existing run is this case and a spurious
        // roll would rotate the disparity of a head that is not tilted.
        float q[4];
        q_of(25.0f, -10.0f, 0.0f, q);
        const x4vr::HeadAngles lvl = x4vr::head_angles(q[0], q[1], q[2], q[3]);
        check(near(lvl.roll_deg, 0.0f, 1e-3f),
              "a level head reports exactly zero roll");

        // At the pole yaw and roll are the same rotation and the split is
        // arbitrary. yaw_weight is the existing guard and must be the one that
        // fires -- roll does not get a second, differently-spelled predicate.
        q_of(0.0f, 89.5f, 30.0f, q);
        const x4vr::HeadAngles pole = x4vr::head_angles(q[0], q[1], q[2], q[3]);
        check(pole.yaw_weight == 0.0f,
              "at the pole the existing yaw guard is what fires");
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
