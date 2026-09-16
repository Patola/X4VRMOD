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

    // ---- take 285b: a plant that delivers in BURSTS, not every tick
    //
    // The half-strength plant above is sluggish but delivers something every
    // single tick, so it cannot express the failure X4VR_TICK_HZ=90 produced.
    // X4 consumes queued mouse events in its own event loop, not on our
    // cadence: during take 285b's episode the camera moved on 24% of ticks
    // (against 70-86% outside it) and then jumped by EXACT multiples of
    // max_step -- 3x, 4x and 5x of 11.918 deg -- because three to five
    // maximum-size commands landed in one frame.
    //
    // This plant models exactly that and nothing else: commands accumulate, and
    // every `burst` ticks the whole accumulation is applied at once. In between,
    // `head_look_observe` keeps reporting the unchanged angle -- which is what
    // makes the servo re-command an error it has already answered.
    //
    // Written to FAIL first. Under the drain-driven tick, burst == 1 by
    // construction (the tick was the drain), and that arm must stay clean, or
    // the harness is not reproducing a regression but a pre-existing fault.
    // A 2x2, not a single arm. Take 285b ran with X4VR_HEADLOOK_LEAD=1.0, and
    // the lead reads head MOTION -- so a parked head produces no lead at all and
    // a one-armed test would have silently measured the wrong servo. Varying
    // burst and lead independently is the only way to say which one it is, or
    // whether it takes both.
    {
        struct Result { float worst_jump, worst_err, worst_cam; int reversals; };
        auto run = [](int burst, float lead, float amp = 45.0f) {
            x4vr::HeadLook s;
            s.gain_deg_per_count = 0.115f;
            s.lead_scale = lead;
            float actual = 0.0f, pending = 0.0f, prev = 0.0f;
            Result r{0.0f, 0.0f, 0.0f, 0};
            int last_sign = 0;
            for (int i = 0; i < 900; i++) {
                // A head sweeping +-45 deg at 0.5 Hz, sampled at 90 Hz: about
                // 1.6 deg per tick at the fastest, which is an ordinary look
                // around and well inside X4's 65 deg clamp. Nothing here asks
                // for an excursion.
                const float head =
                    amp * std::sin(2.0f * 3.14159265f * 0.5f * (float)i / 90.0f);
                x4vr::head_look_observe(s, actual, 0.0f);
                const x4vr::Delta d = x4vr::head_look_step(s, {head, 0.0f});
                pending += s.sign_yaw * (float)d.dx * s.gain_deg_per_count;
                if ((i % burst) == (burst - 1)) {
                    actual = x4vr::clampf(actual + pending, -65.0f, 65.0f);
                    pending = 0.0f;
                }
                const float jump = std::fabs(actual - prev);
                if (i > 90) { // one full sweep to settle
                    r.worst_jump = std::fmax(r.worst_jump, jump);
                    r.worst_err = std::fmax(r.worst_err,
                                            std::fabs(actual - head));
                    r.worst_cam = std::fmax(r.worst_cam, std::fabs(actual));
                    const int sgn = actual > prev ? 1 : (actual < prev ? -1 : 0);
                    if (sgn && last_sign && sgn != last_sign && jump > 20.0f)
                        r.reversals++;
                    if (sgn)
                        last_sign = sgn;
                }
                prev = actual;
            }
            return r;
        };

        // The amplitude is the third variable, and it is not a free parameter:
        // take 285b's own `headlook` lines show the head reaching -70.0..+63.9,
        // i.e. PAST X4's 65 deg clamp. At +-45 the camera never reaches the
        // clamp at all, and a sweep that cannot reach it cannot reproduce a
        // failure whose signature is cmd pinned at +-53.04 = +-(65 - max_step).
        printf("      burst x lead x amplitude, head sweeping at 0.5 Hz:\n");
        printf("        %-6s %-6s %-6s %10s %10s %10s %10s\n",
               "burst", "lead", "amp", "worst_err", "worst_jump", "max|cam|",
               "revs");
        Result g[2][2][2];
        const int bursts[2] = {1, 4};
        const float leads[2] = {0.0f, 1.0f};
        const float amps[2] = {45.0f, 70.0f};
        for (int bi = 0; bi < 2; bi++)
            for (int li = 0; li < 2; li++)
                for (int ai = 0; ai < 2; ai++) {
                    g[bi][li][ai] = run(bursts[bi], leads[li], amps[ai]);
                    printf("        %-6d %-6.1f %-6.0f %10.2f %10.2f %10.2f "
                           "%10d\n", bursts[bi], leads[li], amps[ai],
                           g[bi][li][ai].worst_err, g[bi][li][ai].worst_jump,
                           g[bi][li][ai].worst_cam, g[bi][li][ai].reversals);
                }

        // **This harness was written to REPRODUCE take 285b and does not.**
        //
        // It was built first, before any fix, precisely so that the mechanism
        // could be confirmed before a fix was designed around it. All eight
        // cells are stable: the worst is 11.03 deg of error and 9.77 deg jumps,
        // against a real episode of 59.6 deg jumps and sustained clamp-to-clamp
        // reversals. The reproduction FAILED, and that is the finding.
        //
        // What it rules out is worth more than what it was meant to show:
        // burst delivery, the #73 lead, and driving the camera into X4's 65 deg
        // clamp are all individually and jointly innocent, across the ranges
        // take 285b actually exercised. So the oscillation is **not in the servo
        // arithmetic**, and the in-flight-command compensation I had proposed
        // would not have fixed it — it would have cost a take to learn that.
        //
        // These now assert the invariant that IS true, so the file keeps its
        // value: if a future change to the servo makes burst delivery unstable,
        // this catches it offline. They are not a reproduction and must not be
        // cited as one.
        for (int bi = 0; bi < 2; bi++)
            for (int li = 0; li < 2; li++)
                for (int ai = 0; ai < 2; ai++)
                    check(g[bi][li][ai].reversals == 0 &&
                              g[bi][li][ai].worst_jump < 3.0f * 11.0f,
                          "servo is stable under burst delivery "
                          "(burst/lead/amp grid)");
        check(g[1][1][1].worst_cam >= 64.9f,
              "the +-70 deg arm really does drive the camera into X4's clamp, "
              "so the grid covers the regime take 285b ran in");
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

    // ---- task #74: the head shift lands in the HEAD's axes ----------------
    //
    // The direction is the whole content of this function, and "does it move"
    // would pass on the forward rotation as readily as the inverse -- so the
    // test is built the way the requirement is stated: **leaning left must
    // read as left at every heading.**
    //
    // A vector that IS head-left by construction is rotated out into world
    // space with the forward rotation, handed to head_shift, and must come
    // back as head-left. Using the wrong rotation inside makes this fail at
    // every non-zero heading, and pass at zero -- which is exactly the case a
    // level-headed test would have covered and nothing else.
    {
        printf("\n#74: does the head shift land in the head's own axes?\n");
        auto q_of = [](float yaw, float pitch, float roll, float q[4]) {
            const float d = 0.008726646f;
            const float cy = std::cos(yaw * d), sy = std::sin(yaw * d);
            const float cp = std::cos(pitch * d), sp = std::sin(pitch * d);
            const float cr = std::cos(roll * d), sr = std::sin(roll * d);
            const float a[4] = {0, sy, 0, cy};
            const float b[4] = {sp, 0, 0, cp};
            const float c[4] = {0, 0, -sr, cr};
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
        // FORWARD rotation: head axes -> world. The counterpart of what
        // head_shift undoes, written out separately so the test does not
        // prove a function against itself.
        auto to_world = [](const float q[4], const float v[3], float o[3]) {
            const float ux = q[0], uy = q[1], uz = q[2], w = q[3];
            const float tx = 2.0f * (uy * v[2] - uz * v[1]);
            const float ty = 2.0f * (uz * v[0] - ux * v[2]);
            const float tz = 2.0f * (ux * v[1] - uy * v[0]);
            o[0] = v[0] + w * tx + (uy * tz - uz * ty);
            o[1] = v[1] + w * ty + (uz * tx - ux * tz);
            o[2] = v[2] + w * tz + (ux * ty - uy * tx);
        };
        int bad = 0, n = 0;
        const float axes[3][3] = {{0.12f, 0, 0}, {0, 0.08f, 0}, {0, 0, 0.05f}};
        for (float yaw : {0.0f, 37.0f, -80.0f, 150.0f})
            for (float pitch : {0.0f, 25.0f, -40.0f})
                for (float roll : {0.0f, 30.0f, -55.0f})
                    for (const auto &want : axes) {
                        float q[4];
                        q_of(yaw, pitch, roll, q);
                        float w[3];
                        to_world(q, want, w);
                        const x4vr::HeadShift got =
                            x4vr::head_shift(q[0], q[1], q[2], q[3],
                                             w[0], w[1], w[2]);
                        n++;
                        if (!near(got.x, want[0], 1e-5f) ||
                            !near(got.y, want[1], 1e-5f) ||
                            !near(got.z, want[2], 1e-5f))
                            bad++;
                    }
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "a head-frame shift survives the round trip through "
                      "world space (%d of %d)", n - bad, n);
        check(bad == 0, buf);

        // ---- the neck model: a PURE ROTATION must shift nothing -----------
        //
        // Take 231 is this case. Head translation worked, and then rolling
        // either way raised Patola toward the roof, pitching either way did
        // the same, and yawing either way pushed him back from whatever he
        // was looking at. Direction-independent, so the term is EVEN in the
        // angle: the eye midpoint orbits a rotation centre it does not sit on,
        // and resolving that arc into head axes leaves h*(1-cos) up and
        // -f*(1-cos) back.
        //
        // Nothing before this asserted it, because every earlier case fed a
        // displacement directly and never asked what a rotation ALONE does.
        // The requirement is zero, exactly -- not small -- since a wrong pivot
        // sign doubles the effect instead of removing it.
        {
            const float pivot[3] = {0.0f, 0.10f, 0.08f};
            int bad = 0, n = 0;
            float worst = 0.0f;
            for (float yaw : {0.0f, 20.0f, -45.0f, 90.0f})
                for (float pitch : {0.0f, 15.0f, -35.0f})
                    for (float roll : {0.0f, 25.0f, -60.0f}) {
                        float q[4];
                        q_of(yaw, pitch, roll, q);
                        // Where the eye midpoint is after this rotation, given
                        // a rotation centre held fixed at the world origin.
                        float eye[3];
                        to_world(q, pivot, eye);
                        // What the layer computes: eye - R*pivot, which must
                        // land back on the stationary centre.
                        const x4vr::HeadShift back = x4vr::head_to_world(
                            q[0], q[1], q[2], q[3], pivot[0], pivot[1],
                            pivot[2]);
                        const float dx = eye[0] - back.x;
                        const float dy = eye[1] - back.y;
                        const float dz = eye[2] - back.z;
                        const x4vr::HeadShift got = x4vr::head_shift(
                            q[0], q[1], q[2], q[3], dx, dy, dz);
                        const float e = std::fmax(
                            std::fabs(got.x),
                            std::fmax(std::fabs(got.y), std::fabs(got.z)));
                        worst = std::fmax(worst, e);
                        n++;
                        if (e > 1e-5f)
                            bad++;
                    }
            char nb[144];
            std::snprintf(nb, sizeof nb,
                          "a pure head rotation shifts NOTHING once the pivot "
                          "is removed (%d of %d, worst %.2e m)", n - bad, n,
                          worst);
            check(bad == 0, nb);

            // And the defect is REAL without the model, or this is guarding
            // nothing. Same rotations, pivot NOT removed: the residual has to
            // be big enough that Patola would feel it, which he did.
            float peak = 0.0f;
            for (float ang : {25.0f, 45.0f, 60.0f}) {
                float q[4];
                q_of(0.0f, ang, 0.0f, q);
                float eye[3];
                to_world(q, pivot, eye);
                const x4vr::HeadShift got = x4vr::head_shift(
                    q[0], q[1], q[2], q[3], eye[0] - pivot[0],
                    eye[1] - pivot[1], eye[2] - pivot[2]);
                peak = std::fmax(peak, std::fabs(got.y));
            }
            char pb[128];
            std::snprintf(pb, sizeof pb,
                          "without the model a pure pitch raises the head by "
                          "%.1f mm -- the take 231 defect", peak * 1000.0f);
            check(peak > 0.005f, pb);
        }

        // Length is preserved -- it is a rotation, and a version that scaled
        // would still pass a direction-only check at unit length.
        float q[4];
        q_of(41.0f, -17.0f, 22.0f, q);
        const x4vr::HeadShift s =
            x4vr::head_shift(q[0], q[1], q[2], q[3], 0.3f, -0.2f, 0.1f);
        const float len = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
        check(near(len, std::sqrt(0.3f*0.3f + 0.2f*0.2f + 0.1f*0.1f), 1e-5f),
              "the shift is rotated, not rescaled");

        // Identity head: the shift passes through untouched. This is the case
        // every level-headed run exercises, and the one that cannot tell the
        // inverse from the forward rotation -- recorded as such so nobody
        // reads a green here as covering the direction.
        const x4vr::HeadShift id = x4vr::head_shift(0, 0, 0, 1, 0.4f, 0.5f, 0.6f);
        check(near(id.x, 0.4f, 1e-6f) && near(id.y, 0.5f, 1e-6f) &&
                  near(id.z, 0.6f, 1e-6f),
              "identity head passes the shift through (proves NO direction)");
    }

    // ---- task #57: which pose the UI quad must declare --------------------
    //
    // **This is the arithmetic I got wrong twice and guessed at a third time.**
    // It is here so it can never be re-derived in prose again.
    //
    // The quad carries an image X4 rendered through its camera at pose C. What
    // the player sees is the quad's declared pose composed with wherever the
    // content sits INSIDE that image:
    //
    //     displayed = R(Q) * (content, in camera coordinates)
    //
    // and the two content classes sit in different places, which is the whole
    // of #57's title:
    //
    //     world-anchored (the cockpit HUD)   R(C)^-1   -- it counter-rotates
    //     screen-locked  (menus, the map)    identity  -- it stays centred
    //
    // Take 242 ran Q = identity and Patola measured the HUD at twice the head
    // angle. The lines below say why, and they say it in the yaw/pitch/roll
    // pattern he reported -- yaw and pitch broken, roll clean -- which is the
    // signature that makes this the right explanation rather than a fitting one.
    {
        auto qmul = [](const float a[4], const float b[4], float o[4]) {
            o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
            o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
            o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
            o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
        };
        auto qconj = [](const float a[4], float o[4]) {
            o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3];
        };

        int bad_world = 0, bad_ident = 0, bad_roll = 0, bad_screen = 0, n = 0;
        for (float cy : {0.0f, 12.0f, -25.0f, 47.0f, -60.0f})
            for (float cp : {0.0f, 8.0f, -15.0f, 30.0f}) {
                // Where X4's camera is, and therefore what layer A declares.
                float R[4];
                x4vr::quat_of_angles(cy, cp, R);
                // World-anchored content, expressed in that camera's frame.
                float content[4];
                qconj(R, content);
                n++;

                // Q = C -- the fix. R * R^-1 is identity for ANY yaw/pitch, so
                // this needs no small-angle argument and no commutation
                // argument: the HUD lands dead ahead of the SHIP and stays
                // there while the head moves.
                float shown[4];
                qmul(R, content, shown);
                x4vr::HeadAngles a =
                    x4vr::head_angles(shown[0], shown[1], shown[2], shown[3]);
                if (!near(a.yaw_deg, 0.0f, 0.05f) ||
                    !near(a.pitch_deg, 0.0f, 0.05f))
                    bad_world++;

                // Q = identity -- take 242. The content keeps its own
                // counter-rotation, so it shows up on the far side from the
                // head by the whole camera angle: 2C of apparent motion.
                a = x4vr::head_angles(content[0], content[1], content[2],
                                      content[3]);
                const bool opposite =
                    (cy == 0.0f || a.yaw_deg * cy < 0.0f) &&
                    (cp == 0.0f || a.pitch_deg * cp < 0.0f);
                if (!opposite)
                    bad_ident++;
                // ...and OFF the axes it also acquires a roll, which I
                // asserted was zero and it is not. R^-1 is pitch^-1 * yaw^-1,
                // the opposite order to the yaw-then-pitch the readback
                // assumes, and the two disagree by a genuine rotation: 26.6 deg
                // at yaw -60 / pitch 30, not a rounding artefact.
                //
                // **This is why take 242's roll check passed.** Patola tested
                // roll by rolling his head, which is yaw ~ pitch ~ 0 -- the one
                // row of this table where the cross-term is exactly zero. The
                // measurement was sound and the configuration hid the defect.
                // Nobody has yet seen a HUD roll while turning AND looking up,
                // because nothing has asked for that combination on purpose.
                //
                // The fix removes it outright: R * R^-1 is identity on all
                // three axes, which is the arm asserted above.
                const bool on_axis = (cy == 0.0f || cp == 0.0f);
                if (on_axis != near(a.roll_deg, 0.0f, 0.05f))
                    bad_roll++;

                // Screen-locked content under the SAME fix follows the camera,
                // because that is what screen-locked means. Asserted, not
                // hoped: if X4's menus turn out to want the ship's frame too,
                // one texture cannot serve both and #57 grows a second part
                // (P243.4). This line is what would have to change.
                a = x4vr::head_angles(R[0], R[1], R[2], R[3]);
                if (!near(a.yaw_deg, cy, 0.05f) ||
                    !near(a.pitch_deg, cp, 0.05f))
                    bad_screen++;
            }

        // **Take 243a: the orientation above is only half a placement.**
        //
        // A quad is a rectangle CENTRED at pose.position and turned about that
        // centre by pose.orientation. Turning it does not move the centre, so
        // content drawn in the middle of the texture keeps its world direction
        // under every orientation -- which is exactly the content whose
        // direction the fix was meant to change. 243a declared the pose, the
        // panel spun in place, and Patola saw no difference at all.
        //
        // The panel has to go where X4's image plane was: zm along the
        // CAMERA's forward axis, orbiting the neutral point. That is what
        // moves the centre, and the centre is what carries the content.
        {
            int bad_centre = 0, bad_spin = 0, m = 0;
            for (float cy : {12.0f, -25.0f, 47.0f})
                for (float cp : {0.0f, 8.0f, -15.0f}) {
                    float R[4];
                    x4vr::quat_of_angles(cy, cp, R);
                    const float zm = 1.4f;
                    m++;
                    // Where the panel's centre ends up once it orbits, read
                    // back as a direction. It must point along the camera's
                    // own forward axis -- the same angles, not merely "some
                    // different place".
                    const x4vr::HeadShift off =
                        x4vr::head_to_world(R[0], R[1], R[2], R[3], 0, 0, -zm);
                    const float len = std::sqrt(off.x*off.x + off.y*off.y +
                                                off.z*off.z);
                    const x4vr::HeadAngles at = x4vr::head_angles(
                        R[0], R[1], R[2], R[3]);
                    // direction of the offset, in the same yaw/pitch terms
                    const float yaw = std::atan2(-off.x, -off.z) * 57.2957795f;
                    const float pitch =
                        std::asin(off.y / (len > 0 ? len : 1)) * 57.2957795f;
                    if (!near(len, zm, 1e-4f) || !near(yaw, at.yaw_deg, 0.05f) ||
                        !near(pitch, at.pitch_deg, 0.05f))
                        bad_centre++;
                    // ...and the take-243a arrangement, at the SAME camera
                    // angle: centre pinned at (0, 0, -zm) whatever the
                    // orientation. Its direction is dead ahead, so the two
                    // placements differ by the whole camera angle -- which is
                    // the difference 243a did not make. Asserted as a contrast
                    // between the two, because "the centre is straight ahead"
                    // on its own is true by construction and would be a check
                    // that cannot fail.
                    const float spin_yaw =
                        std::atan2(-0.0f, -(-zm)) * 57.2957795f;
                    const float spin_pitch = 0.0f;
                    if (!near(yaw - spin_yaw, at.yaw_deg, 0.05f) ||
                        !near(pitch - spin_pitch, at.pitch_deg, 0.05f))
                        bad_spin++;
                }
            char b2[120];
            std::snprintf(b2, sizeof b2,
                          "#57: the panel CENTRE orbits onto the camera axis "
                          "(%d of %d)", m - bad_centre, m);
            check(bad_centre == 0, b2);
            std::snprintf(b2, sizeof b2,
                          "#57: orbited vs 243a's pinned centre differ by the "
                          "whole camera angle (%d of %d)", m - bad_spin, m);
            check(bad_spin == 0, b2);
        }

        char buf[112];
        std::snprintf(buf, sizeof buf,
                      "#57: declaring the scene pose pins the HUD to the ship "
                      "(%d of %d)", n - bad_world, n);
        check(bad_world == 0, buf);
        std::snprintf(buf, sizeof buf,
                      "#57: identity puts it on the OPPOSITE side — take 242 "
                      "(%d of %d)", n - bad_ident, n);
        check(bad_ident == 0, buf);
        check(bad_roll == 0,
              "#57: identity also ROLLED the HUD off-axis (0 on-axis, 26.6 deg "
              "at -60/30) — the fix is exact on all three");
        std::snprintf(buf, sizeof buf,
                      "#57: screen-locked content follows the camera (%d of %d)",
                      n - bad_screen, n);
        check(bad_screen == 0, buf);
    }

    // ---- #75 take 294: the read-back plausibility guard
    //
    // The guard exists because ONE tick of X4 answering 0,0 put the camera
    // 100 deg off the head for 4.6 s. But a guard is only worth having if it
    // refuses the fault AND passes honest motion, so both are asserted here.
    // The negative controls matter more than the positive one: a guard that
    // refuses everything would look identical on the fault case alone.
    {
        // The measured sequence, verbatim from /tmp/x4vr-take294.log at
        // t=920748.8-920749.0. head_yaw, x4_yaw.
        const float seq[][2] = {{16.852f, 16.387f},  {17.954f, -0.000f},
                                {18.176f, 104.123f}, {18.346f, 105.727f},
                                {18.379f, 119.043f}, {18.317f, 93.751f},
                                {18.046f, 81.747f},  {17.488f, 76.218f}};
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        x4vr::head_look_observe(s, seq[0][1], 0.0f); // prime
        float worst_cmd = 0.0f;
        for (size_t i = 1; i < sizeof seq / sizeof seq[0]; i++) {
            x4vr::head_look_observe(s, seq[i][1], 0.0f);
            x4vr::head_look_step(s, {seq[i][0], 0.0f});
            worst_cmd = std::fmax(worst_cmd, std::fabs(s.cmd_yaw_deg));
        }
        check(s.observe_refused >= 6,
              "#75: the take-294 read-back sequence is refused");
        check(s.observe_resyncs == 0,
              "#75: ...and the 7-sample fault fits inside the refusal budget");
        check(worst_cmd < 40.0f,
              "#75: the command never follows the read-back to 119 deg");
        printf("      take-294 replay: %llu refused, worst command %.2f deg "
               "(was 107.08 unguarded)\n",
               (unsigned long long)s.observe_refused, worst_cmd);
    }
    {
        // NEGATIVE CONTROL 1 -- an honest fast sweep must not be refused. This
        // is the case the guard could plausibly break: during the dead time
        // cmd runs ahead of the read-back, and if that gap reached the
        // threshold the servo would start disbelieving its own success.
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        float cam = 0.0f;
        float inflight[3] = {0, 0, 0}; // take 291's measured 3-tick dead time
        for (int i = 0; i < 400; i++) {
            const float head = 70.0f * std::sin(i * 0.05f); // ~250 deg/s peak
            x4vr::head_look_observe(s, cam, 0.0f);
            const x4vr::Delta d = x4vr::head_look_step(s, {head, 0.0f});
            cam += inflight[0];
            inflight[0] = inflight[1];
            inflight[1] = inflight[2];
            inflight[2] = s.sign_yaw * (float)d.dx * s.gain_deg_per_count;
        }
        check(s.observe_refused == 0,
              "#75: an honest 250 deg/s sweep with 3 ticks of dead time is "
              "never refused (the guard's false-positive case)");
        printf("      honest sweep: %llu refused of 400\n",
               (unsigned long long)s.observe_refused);
    }
    {
        // NEGATIVE CONTROL 2 -- the head past X4's stop. cmd walks out to
        // yaw_limit while the read-back pins at 65, so the gap is a standing
        // 20 deg. If the threshold were set at the gap rather than above it,
        // every clamped moment would be read as a fault.
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        s.yaw_limit_deg = 85.0f;
        for (int i = 0; i < 200; i++) {
            x4vr::head_look_observe(s, 65.0f, 0.0f); // X4 pinned at its stop
            x4vr::head_look_step(s, {85.0f, 0.0f});  // head asks for more
        }
        check(s.observe_refused == 0,
              "#75: a head held past X4's stop is not a fault (yaw_limit 85 "
              "against a 65 deg stop)");
    }
    {
        // The budget must RUN OUT. A guard that never yields would strand the
        // servo pushing at a camera that has genuinely moved -- the map-view
        // accelerator again.
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        x4vr::head_look_observe(s, 0.0f, 0.0f);
        for (int i = 0; i < 40; i++)
            x4vr::head_look_observe(s, 120.0f, 0.0f); // X4 really is elsewhere
        check(s.observe_resyncs >= 1 && s.cmd_yaw_deg > 119.0f,
              "#75: the refusal budget runs out and the servo resynchronises");
        printf("      sustained move: %llu refused, %llu resync(s), cmd %.1f\n",
               (unsigned long long)s.observe_refused,
               (unsigned long long)s.observe_resyncs, s.cmd_yaw_deg);
    }
    {
        // And it must be switchable off, or every earlier take stops being
        // reproducible.
        x4vr::HeadLook s;
        s.observe_max_jump_deg = 0.0f;
        x4vr::head_look_observe(s, 0.0f, 0.0f);
        x4vr::head_look_observe(s, 119.043f, 0.0f);
        check(s.observe_refused == 0 && s.cmd_yaw_deg > 119.0f,
              "#75: observe_max_jump_deg = 0 restores the unguarded behaviour");
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
