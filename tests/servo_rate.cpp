// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// **Task #75: the servo steps faster than the plant reports.**
//
// Takes 285b and 287b slammed X4's camera between its +-65/+-35 clamps. The
// cause is not the one committed in 296abdf -- see docs/x4-quirks.md for that
// withdrawal -- and it is not the event channel, the servo arithmetic, or a
// stale off-thread read, all three refuted earlier. What the logs DO show is a
// rate mismatch:
//
//   X4's camera readback changes at 30-60 Hz. X4VR_TICK_HZ drives the servo at
//   90. head_look_observe() overwrites cmd with the readback EVERY tick, so on
//   the ticks between two camera updates the servo re-observes a reading it has
//   already acted on, recomputes the same error, and sends the same correction
//   again.
//
// M commands therefore land per intended correction, and the loop gain is M*kp
// rather than kp. The servo has no memory of what is in flight because observe
// destroys it.
//
// Measured from the poselag stream, servo ticks per camera update:
//
//   takes 278-284, 287a (no tick driver)  median 1.0   p90 1-2   max 4-6
//   takes 285b, 287b   (X4VR_TICK_HZ=90)  median 1.0   p90 2     max 7-11
//
// and camera updates moving more than 20 deg, which is the slam itself:
//
//   278 0.1%   279 0.0%   280 0.0%   281 0.0%   282 0.5%   283 0.2%   284 0.0%
//   287a 0.0%  285b-s1 0.0%   |   285b-s2 7.1%   285b 4.4%   287b 7.5%
//
// The three on the right are exactly the sessions Patola reported as wild, and
// 285b-s1 -- same binary, same knobs, same tick driver, no wild report -- reads
// 0.0%. The statistic agrees with his eye on 4 of 4 labelled sessions, so it is
// the statistic this file reports too: a model that reproduces the defect has
// to reproduce the NUMBER the log measures, not merely look unstable.
//
// **P288.1, committed before running this: the rate mismatch alone does NOT
// reproduce the slam.** With M commands per observation the error evolves as
// e(1 - M*kp), which diverges only at M*kp > 2 -- M > 8 at kp = 0.25. The
// measured median M is 1-2 and the maximum ever seen is 11, so if this
// prediction holds, the mechanism needs something this model does not have and
// the next step is to find it rather than to tune kp. The sweep below reports
// the threshold M so the prediction is checked against a number rather than
// asserted.
//
// **P288.2: with the head held STILL the model will settle.** Patola held his
// head still for several seconds mid-oscillation and it did not cure it. A loop
// whose instability is internal keeps ringing with a frozen target; one that is
// merely chasing a moving head calms down. If the model settles where the game
// did not, this mechanism is refuted by his observation alone.
//
// Nothing here changes shipping code. It drives the real head_look_step() and
// head_look_observe() against a plant, exactly as tests/servo_lag.cpp does --
// but servo_lag's plant steps once per servo step, which is the assumption this
// file exists to break, so it cannot express the question at all.
#include "../common/x4vr_headlook.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        g_fail++;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

// X4's camera through the mouse channel.
//
// Deltas are integrated the instant they arrive -- every mouse event X4 dequeues
// moves the camera, and take 287's census established that it dequeues ours 1:1
// with no backlog. What lags is the REPORT: GetCameraRotation returns a value
// that only refreshes on X4's own camera tick, which the poselag stream measures
// at 30-60 Hz against a 90 Hz servo.
//
// The clamp is applied per delta rather than to the sum, because X4's is a
// position limit on a camera that is already there, not arithmetic on a batch.
struct Plant {
    float yaw = 0.0f, pitch = 0.0f;     // where the camera actually is
    float pub_yaw = 0.0f, pub_pitch = 0.0f; // what GetCameraRotation returns
    float yaw_limit = 65.0f, pitch_limit = 35.0f;
    float gain = 0.115f;                // X4's true deg per count
    int period = 1;                     // servo steps per camera update
    int k = 0;

    // **Take 288 measured this, open-loop, and it is the whole plant.**
    //
    //   impulse response  +0: 0.00  +1: 0.16  +2: 0.15  +3: 3.45  +4..+7: 0.00
    //   delivery          99.6-99.7% at every size from 2 to 104 counts
    //
    // 92% of the response arrives in ONE tick, three ticks late, and exactly
    // nothing arrives after it. That is dead time on a faithful integrator --
    // not momentum, not a cap, not a nonlinearity. Everything the servo sends
    // is delivered, whole, `delay` ticks later.
    int delay = 3;
    std::vector<float> pipe_y, pipe_p;

    void arm() {
        pipe_y.assign((size_t)delay, 0.0f);
        pipe_p.assign((size_t)delay, 0.0f);
    }

    // Returns true on the ticks where the readback refreshes.
    bool step(float sign_yaw, int dx, float sign_pitch, int dy) {
        pipe_y.push_back(sign_yaw * (float)dx * gain);
        pipe_p.push_back(sign_pitch * (float)dy * gain);
        const float dy_now = pipe_y.front(), dp_now = pipe_p.front();
        pipe_y.erase(pipe_y.begin());
        pipe_p.erase(pipe_p.begin());
        yaw = x4vr::clampf(yaw + dy_now, -yaw_limit, yaw_limit);
        pitch = x4vr::clampf(pitch + dp_now, -pitch_limit, pitch_limit);
        if (++k < period)
            return false;
        k = 0;
        pub_yaw = yaw;
        pub_pitch = pitch;
        return true;
    }
};

struct Result {
    double slam_pct = 0.0;   // camera updates moving >20 deg, the log's statistic
    double max_move = 0.0;   // largest single camera update, deg
    double max_excursion = 0.0;
    int reversals = 0;       // sign changes of camera velocity between updates
    double rms_err = 0.0;
};

// `traj` returns the head angles at step i. The servo runs at a fixed rate; the
// plant's report refreshes every `period` steps.
template <class F>
static Result drive(int period, float kp, float lead, float plant_gain, F traj,
                    int steps, int delay = 0) {
    // **Default 0, deliberately.** Sections 1-4 below and the P288.1/P288.2
    // assertions were all taken against a plant with NO dead time, before take
    // 288 measured any. Changing this default would silently re-run them against
    // a different plant and leave their comments describing a result that no
    // longer exists. Section 5 passes the measured delay explicitly.
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f; // what the SERVO believes
    s.servo_kp = kp;
    s.lead_scale = lead;
    Plant p;
    p.period = period;
    p.gain = plant_gain;            // what X4 actually does
    p.delay = delay;
    p.arm();

    Result r;
    int updates = 0;
    double prev_pub_yaw = 0.0, prev_vel = 0.0, err2 = 0.0;
    for (int i = 0; i < steps; i++) {
        // The injector's order, and it matters: observe BEFORE step, so the
        // servo acts on this tick's reading. On the ticks where the plant did
        // not refresh, this re-pins cmd to a reading already acted on -- which
        // is the whole mechanism under test.
        x4vr::head_look_observe(s, p.pub_yaw, p.pub_pitch);

        const x4vr::HeadAngles head = traj(i);
        const x4vr::Delta d = x4vr::head_look_step(s, head);

        if (p.step(s.sign_yaw, d.dx, s.sign_pitch, d.dy)) {
            const double move = std::fabs((double)p.pub_yaw - prev_pub_yaw);
            const double vel = (double)p.pub_yaw - prev_pub_yaw;
            if (updates > 0) {
                if (move > 20.0)
                    r.slam_pct += 1.0;
                r.max_move = std::fmax(r.max_move, move);
                if (updates > 1 && vel * prev_vel < 0.0)
                    r.reversals++;
            }
            prev_vel = vel;
            prev_pub_yaw = p.pub_yaw;
            updates++;
        }
        r.max_excursion = std::fmax(r.max_excursion, std::fabs((double)p.yaw));
        const double e = (double)head.yaw_deg - (double)p.yaw;
        err2 += e * e;
    }
    if (updates > 1)
        r.slam_pct = 100.0 * r.slam_pct / (double)(updates - 1);
    r.rms_err = std::sqrt(err2 / (double)steps);
    return r;
}

// A head that turns, the way a player looking around a cockpit does. Amplitude
// and rate are taken from take 287b's own `head` column: +-3 to +-20 deg of yaw
// at 30-60 deg/s, well inside X4's clamp, so nothing here asks for the stop.
static x4vr::HeadAngles turning(int i, double hz, double amp) {
    const double t = (double)i / 90.0;
    x4vr::HeadAngles h;
    h.yaw_deg = (float)(amp * std::sin(2.0 * M_PI * hz * t));
    h.pitch_deg = (float)(0.5 * amp * std::sin(2.0 * M_PI * hz * t * 0.7));
    h.yaw_weight = 1.0f;
    h.yaw_reliable = true;
    return h;
}


// The measured plant: a faithful integrator whose latency JITTERS over a band
// fixed in milliseconds. Arrival order is preserved -- X4 applies mouse deltas
// in the order it dequeues them -- so a command drawn a shorter delay than its
// predecessor lands with it, never before it.
struct Jit { double slam = 0; double max_exc = 0; int reversals = 0; };

template <class F>
static Jit run_jitter(double tick_hz, int lo, int hi, F traj, double seconds) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f;
    s.servo_kp = 0.25f;
    s.lead_scale = 1.0f;
    Plant p;
    p.period = 1;
    p.delay = 0;
    p.arm();
    std::mt19937 rng(20260913u);
    std::uniform_int_distribution<int> jit(lo, hi);
    std::vector<std::pair<int, float>> q;
    int last_due = -1, updates = 0;
    double prev_pub = 0.0, prev_vel = 0.0;
    Jit r;
    const int steps = (int)(tick_hz * seconds);
    for (int i = 0; i < steps; i++) {
        x4vr::head_look_observe(s, p.pub_yaw, p.pub_pitch);
        const x4vr::Delta d = x4vr::head_look_step(s, traj(i));
        int due = i + jit(rng);
        if (due < last_due) due = last_due;
        last_due = due;
        q.emplace_back(due, s.sign_yaw * (float)d.dx * s.gain_deg_per_count);
        float land = 0.0f;
        for (size_t k = 0; k < q.size();) {
            if (q[k].first <= i) { land += q[k].second; q.erase(q.begin() + (long)k); }
            else break;
        }
        p.yaw = x4vr::clampf(p.yaw + land, -p.yaw_limit, p.yaw_limit);
        p.pub_yaw = p.yaw;
        const double vel = (double)p.pub_yaw - prev_pub;
        if (updates > 0) {
            if (std::fabs(vel) > 20.0) r.slam += 1.0;
            if (updates > 1 && vel * prev_vel < 0.0) r.reversals++;
        }
        prev_vel = vel;
        prev_pub = p.pub_yaw;
        updates++;
        r.max_exc = std::fmax(r.max_exc, std::fabs((double)p.yaw));
    }
    r.slam = updates > 1 ? 100.0 * r.slam / (double)(updates - 1) : 0.0;
    return r;
}

int main() {
    std::printf("A head turning at 20 deg amplitude, 0.5 Hz, servo at 90 Hz.\n");
    std::printf("`period` is servo steps per camera update: 1 is every take\n");
    std::printf("before the tick driver, 2 is take 287b's median.\n\n");
    std::printf("%8s %9s %10s %10s %10s %10s\n", "period", "loop gain",
                "slam%", "max move", "max |cam|", "reversals");

    std::vector<Result> by_period;
    for (int period : {1, 2, 3, 4, 6, 8, 11, 16}) {
        const Result r = drive(period, 0.25f, 1.0f, 0.115f,
                               [](int i) { return turning(i, 0.5, 20.0); },
                               90 * 20);
        by_period.push_back(r);
        std::printf("%8d %9.2f %9.1f%% %9.2f %10.2f %10d\n", period,
                    period * 0.25, r.slam_pct, r.max_move, r.max_excursion,
                    r.reversals);
    }

    // **The control, and it has to pass before any other line here means
    // anything.** Every take without the tick driver measured a median of one
    // servo step per camera update and 0.0-0.5% slams. If the model slams at
    // period 1 it is not a model of those takes, and its verdict at period 2
    // would be a property of the model rather than of X4.
    check(by_period[0].slam_pct == 0.0 && by_period[0].max_move < 20.0,
          "control: at ONE step per camera update the model is calm, as takes "
          "278-284 and 287a measured");

    // P288.1: the measured median is 1-2 and the all-time maximum 11.
    check(by_period[1].slam_pct == 0.0,
          "P288.1: at take 287b's MEDIAN period of 2 the model does not slam");

    int threshold = 0;
    const int periods[] = {1, 2, 3, 4, 6, 8, 11, 16};
    for (size_t i = 0; i < by_period.size(); i++)
        if (by_period[i].slam_pct > 0.0) { threshold = periods[i]; break; }
    if (threshold)
        std::printf("\n  first period that slams: %d (loop gain %.2f). The log's "
                    "median is 1-2 and its all-time maximum is 11.\n", threshold,
                    threshold * 0.25);
    else
        std::printf("\n  NO period up to 16 slams. The rate mismatch cannot "
                    "produce this defect at any rate the game reaches.\n");

    // **P288.2: a still head.** Patola held his head still mid-oscillation for
    // several seconds and it did not cure it. Start the plant already thrown to
    // the clamp, freeze the head, and see whether the loop rings on its own.
    std::printf("\n2. the head held STILL, camera starting at the +65 clamp\n");
    {
        for (int period : {1, 2, 4, 8}) {
            x4vr::HeadLook s;
            s.gain_deg_per_count = 0.115f;
            s.servo_kp = 0.25f;
            s.lead_scale = 1.0f;
            Plant p;
            p.period = period;
            p.delay = 0; // P288.2 was taken with no dead time; see drive()
            p.arm();
            p.yaw = p.pub_yaw = 65.0f; // thrown there by whatever started it
            double worst_late = 0.0;
            int updates = 0;
            double prev = 65.0;
            for (int i = 0; i < 90 * 6; i++) {
                x4vr::head_look_observe(s, p.pub_yaw, p.pub_pitch);
                x4vr::HeadAngles h; // frozen, looking slightly left
                h.yaw_deg = 5.0f;
                h.pitch_deg = 0.0f;
                h.yaw_weight = 1.0f;
                const x4vr::Delta d = x4vr::head_look_step(s, h);
                if (p.step(s.sign_yaw, d.dx, s.sign_pitch, d.dy)) {
                    // Only the last second: the first is the legitimate
                    // recovery from the clamp, which is not the defect.
                    if (i > 90 * 5 && updates)
                        worst_late = std::fmax(worst_late,
                                               std::fabs((double)p.pub_yaw - prev));
                    prev = p.pub_yaw;
                    updates++;
                }
            }
            std::printf("   period %2d: after 5 s of a frozen head the camera "
                        "still moves %.3f deg per update, and sits at %.2f\n",
                        period, worst_late, p.yaw);
            if (period == 2)
                check(worst_late < 0.5,
                      "P288.2: at the measured period the model SETTLES with a "
                      "still head — the game did not, so this mechanism does "
                      "not explain Patola's observation");
        }
    }

    // **Vary them independently.** A gain mismatch and a rate mismatch both
    // multiply the correction, so sweeping them together would confirm whichever
    // one I happened to believe. The servo believes 0.115 deg/count throughout;
    // only what X4 does with a count changes here, at period 1, so nothing in
    // this block can be attributed to the rate.
    std::printf("\n3. a GAIN mismatch instead, at one step per camera update\n");
    std::printf("%12s %11s %10s %10s\n", "X4 deg/count", "vs believed", "slam%",
                "max move");
    double gain_threshold = 0.0;
    for (double g : {0.115, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5}) {
        const Result r = drive(1, 0.25f, 1.0f, (float)g,
                               [](int i) { return turning(i, 0.5, 20.0); },
                               90 * 20);
        std::printf("%12.3f %10.1fx %9.1f%% %9.2f\n", g, g / 0.115, r.slam_pct,
                    r.max_move);
        if (r.slam_pct > 0.0 && gain_threshold == 0.0)
            gain_threshold = g;
    }
    if (gain_threshold > 0.0)
        std::printf("\n  a gain mismatch of %.1fx is enough on its own, with the "
                    "rates matched.\n", gain_threshold / 0.115);

    // The lead is #73's feed-forward, closed as NOT the jitter by take 276's
    // in-run A/B. Re-checked here for free rather than assumed: if it mattered
    // to this defect, that closure would have to reopen.
    std::printf("\n4. the lead, which take 276 closed as not the jitter\n");
    for (float lead : {0.0f, 1.0f}) {
        const Result r = drive(2, 0.25f, lead, 0.115f,
                               [](int i) { return turning(i, 0.5, 20.0); },
                               90 * 20);
        std::printf("   lead_scale %.1f: slam %.1f%%, max move %.2f deg, "
                    "rms error %.2f deg\n", lead, r.slam_pct, r.max_move,
                    r.rms_err);
    }

    // ---- 5. the plant take 288 actually measured -------------------------
    //
    // Sections 1-4 ran against a plant with no dead time, because none had been
    // measured. Take 288 drove X4 open-loop and the answer was not the rate
    // mismatch and not momentum: **a faithful integrator with a transport delay
    // of 3 ticks.** 99.6% delivered at every size from 2 to 104 counts, 92% of
    // the impulse response in one tick three ticks late, and exactly 0.000
    // after it.
    //
    // A proportional controller against dead time is the textbook oscillator,
    // and this one has a second multiplier on top: head_look_observe() re-pins
    // the estimate to the published reading every tick, so on the ticks between
    // two camera updates the servo re-sends a correction already in flight.
    //
    // **The delay is fixed in TIME, not in ticks -- 3 ticks at take 288's 30 Hz
    // is 99 ms.** One run at one rate cannot separate "99 ms" from "3 frames",
    // and that distinction decides whether raising X4VR_TICK_HZ multiplies the
    // dead time or leaves it alone. The sweep below therefore reports the
    // threshold in TICKS and says what each rate would imply, rather than
    // asserting a scaling this run cannot support.
    std::printf("\n5. the measured plant: an integrator with dead time\n");
    std::printf("%8s %8s %10s %10s %10s %10s\n", "delay", "period", "slam%",
                "max move", "max |cam|", "reversals");
    // **max |cam| and reversals, not slam%% alone.** At one tick per camera
    // update a single update can only move max_step_deg = 12 deg, so the log's
    // ">20 deg per update" statistic CANNOT fire however violently the loop
    // oscillates -- and it read 0.0%% on a run that was pinned to both clamps
    // with 113 reversals. A statistic that cannot express the failure is not a
    // measurement of it; see [[statistic-cannot-express-the-alternative]].
    int reaches_clamp_at = 0;
    for (int delay : {0, 1, 2, 3, 4, 5, 6, 9}) {
        const Result r = drive(1, 0.25f, 1.0f, 0.115f,
                               [](int i) { return turning(i, 0.5, 20.0); },
                               90 * 20, delay);
        std::printf("%8d %8d %9.1f%% %9.2f %10.2f %10d\n", delay, 1,
                    r.slam_pct, r.max_move, r.max_excursion, r.reversals);
        if (r.max_excursion >= 64.9 && !reaches_clamp_at)
            reaches_clamp_at = delay;
    }

    // The model has to be ABLE to produce the failure, or nothing it says about
    // the measured point means anything.
    check(reaches_clamp_at > 0,
          "the model can reach the clamp at SOME dead time — otherwise this "
          "section could only ever report calm");

    // **The measured point is calm, and that is the finding.** Take 288 read 3
    // ticks of dead time at its 30 Hz tick rate. At kp = 0.25 that is well
    // inside stability, so dead time at the rate it was measured does NOT
    // explain takes 285b/287b either. Asserted so a future change that makes it
    // slam gets noticed rather than welcomed.
    {
        const Result r = drive(1, 0.25f, 1.0f, 0.115f,
                               [](int i) { return turning(i, 0.5, 20.0); },
                               90 * 20, 3);
        check(r.max_excursion < 30.0 && r.reversals == 0,
              "at the MEASURED dead time of 3 ticks the loop is calm — so this "
              "is not the mechanism at the rate it was measured");
    }

    std::printf("\n  first dead time that reaches the clamp: %d tick(s), at "
                "kp = 0.25.\n", reaches_clamp_at);

    // **P289, and it is the one question left.** The delay is 3 ticks at take
    // 288's 30.3 Hz tick rate. Whether that is 99 ms of wall clock or 3 of X4's
    // frames cannot be told from one run at one rate -- and it decides
    // everything, because the servo's stability depends on the dead time in
    // TICKS:
    //
    //   fixed in TIME    99 ms is 3 ticks at 30 Hz, 5 at 50, 9 at 90. Raising
    //                    X4VR_TICK_HZ walks the loop straight past the
    //                    threshold above, and that ordering matches all four
    //                    labelled sessions: 287a (50 Hz, drain) calm, 285b-s1/
    //                    s2 and 287b (90 Hz) wild.
    //   fixed in FRAMES  3 ticks at every rate, the loop never crosses, and the
    //                    oscillation has a cause still unfound.
    //
    // One plantid run at X4VR_TICK_HZ=90 separates them: the transport delay
    // comes back as 9 ticks or as 3. Nothing else in the run needs to change.
    std::printf("\n  P289: if the 3-tick delay is fixed in TIME (99 ms at take "
                "288's 30.3 Hz),\n        it is ~9 ticks at X4VR_TICK_HZ=90 — "
                "past the %d above.\n        If it is fixed in FRAMES it stays "
                "3 and this is not the cause.\n        One plantid run at 90 Hz "
                "reports which.\n", reaches_clamp_at);

    // ---- 6. the plant's latency JITTERS, and that is not a fixed delay ----
    //
    // Takes 290a/290b measured X4's input-to-camera latency per impulse, in
    // milliseconds, independent of the sampling bucket:
    //
    //   290a (28.2 Hz tick)   median 108.8 ms   p10 39.0   p90 139.5   max 162
    //   290b (75.1 Hz tick)   median  52.8 ms   p10 38.5   p90 128.0   max 167
    //
    // The medians differ because 290a's buckets are 35 ms and 290b's are 11, but
    // the SPREAD agrees: 100.5 ms against 89.5 ms, p90-p10, with no dependence on
    // command size (corr +0.03 and -0.18 across 2 to 104 counts). So the plant's
    // latency is not a constant to be compensated -- it wanders over roughly
    // 40-165 ms, and the servo cannot know where in that range any given command
    // will land.
    //
    // Section 5 swept a FIXED delay and found the loop calm to 5 ticks and at the
    // clamp by 6. In ticks, the measured range is:
    //
    //   at 29 Hz (34.5 ms ticks)   1.2 - 4.8 ticks   -- never reaches 6
    //   at 90 Hz (11.1 ms ticks)   3.6 - 14.9 ticks  -- past 6 for most of it
    //
    // **P291, committed before this runs: a delay jittering over the measured
    // range destabilises the loop at 90 Hz and not at 29 Hz.** If both stay calm,
    // jitter is not the mechanism either and section 5's threshold is the wrong
    // lens. If BOTH go unstable, the tick rate is not what separates the wild
    // sessions from the calm ones and the 4-of-4 correlation was luck.
    std::printf("\n6. the MEASURED latency, which jitters over 40-165 ms\n");
    std::printf("%10s %9s %9s %10s %10s %10s\n", "tick Hz", "delay lo",
                "delay hi", "slam%", "max |cam|", "reversals");
    struct Arm { double hz; const char *note; };
    // **The rates are taken from windows where the servo was actually
    // WORKING** -- steering above 50% and deltas going out above 5 Hz. Measured
    // over all windows instead, takes 280-284 looked like calm 30-60 Hz
    // sessions; they have no such window at all, so they never exercised the
    // servo and cannot discriminate anything. And take 287a, which I listed as
    // a calm 50 Hz arm, ran at 28.0 Hz where it mattered.
    const Arm arms[] = {{28.0, "287a: calm"}, {50.0, "no take ran here"},
                        {68.6, "285b-s1: not reported wild"},
                        {70.7, "285b-s2 / 287b: WENT WILD"}};
    bool wild_at_90 = false, calm_at_29 = false;
    for (const Arm &a : arms) {
        const double tick_ms = 1000.0 / a.hz;
        const int lo = (int)std::floor(40.0 / tick_ms);
        const int hi = (int)std::ceil(165.0 / tick_ms);
        // A jittering delay must NOT reorder: X4 applies mouse deltas in the
        // order it dequeues them. Each command gets a due tick, clamped to be no
        // earlier than the previous one's, and everything due is released.
        x4vr::HeadLook s2;
        s2.gain_deg_per_count = 0.115f;
        s2.servo_kp = 0.25f;
        s2.lead_scale = 1.0f;
        Plant p;
        p.period = 1;
        p.delay = 0;
        p.arm();
        std::mt19937 rng(20260913u);
        std::uniform_int_distribution<int> jit(lo, hi);
        std::vector<std::pair<int, float>> q; // (due tick, degrees)
        int last_due = -1;
        double prev_pub = 0.0, prev_vel = 0.0;
        int updates = 0, reversals = 0;
        double max_move = 0.0, max_exc = 0.0, slam = 0.0;
        const int steps = (int)(a.hz * 20.0);
        for (int i = 0; i < steps; i++) {
            x4vr::head_look_observe(s2, p.pub_yaw, p.pub_pitch);
            const x4vr::HeadAngles head = turning(i, 0.5, 20.0);
            const x4vr::Delta d = x4vr::head_look_step(s2, head);
            int due = i + jit(rng);
            if (due < last_due)
                due = last_due;
            last_due = due;
            q.emplace_back(due, s2.sign_yaw * (float)d.dx * s2.gain_deg_per_count);
            float land = 0.0f;
            for (size_t k = 0; k < q.size();) {
                if (q[k].first <= i) { land += q[k].second; q.erase(q.begin() + (long)k); }
                else break;          // ordered, so the first not-due ends it
            }
            p.yaw = x4vr::clampf(p.yaw + land, -p.yaw_limit, p.yaw_limit);
            p.pub_yaw = p.yaw;
            const double vel = (double)p.pub_yaw - prev_pub;
            const double move = std::fabs(vel);
            if (updates > 0) {
                if (move > 20.0) slam += 1.0;
                max_move = std::fmax(max_move, move);
                if (updates > 1 && vel * prev_vel < 0.0) reversals++;
            }
            prev_vel = vel;
            prev_pub = p.pub_yaw;
            updates++;
            max_exc = std::fmax(max_exc, std::fabs((double)p.yaw));
        }
        slam = updates > 1 ? 100.0 * slam / (double)(updates - 1) : 0.0;
        std::printf("%10.1f %9d %9d %9.1f%% %10.2f %10d   %s\n", a.hz, lo, hi,
                    slam, max_exc, reversals, a.note);
        if (a.hz >= 70.0 && max_exc >= 64.9) wild_at_90 = true;
        if (a.hz <= 29.0 && max_exc < 40.0) calm_at_29 = true;
    }
    // **"every pre-driver session was calm" is NOT supported and is not
    // claimed.** Takes 280-284 have no window where the servo was working, so
    // they are silent on this, not evidence for it. The only measured calm arm
    // with the servo actually running is take 287a, at 28.0 Hz.
    check(calm_at_29,
          "P291: at 28.0 Hz the jittering delay stays calm, which is where take "
          "287a ran with the servo working and the camera fine");
    check(wild_at_90,
          "P291: at ~70 Hz the SAME jitter reaches X4's clamp — the rate at "
          "which 285b-s2 and 287b went wild, and the only variable that moved");

    // ---- 7. how much head motion does the model NEED? ---------------------
    //
    // Take 285b-s1 ran at 68.6 Hz -- the same rate as the two wild sessions --
    // and was not reported wild. Section 6 calls it 14.8%. So either its head
    // was too quiet to excite the loop, or the model is simply too eager.
    //
    // Measured from `camread`, whose cadence is `% 30` only and therefore does
    // NOT oversample the wild moments the way `headlook` does (it fires on
    // `% 30 || clamped`), head angular rate over each session:
    //
    //   287a     calm, 28 Hz    p50   3.9   p90  56.3   max 113.9 deg/s
    //   285b-s1  calm, 68.6 Hz  p50   6.2   p90  69.4   max 117.5
    //   285b-s2  WILD, 70.7 Hz  p50  33.4   p90 103.7   max 161.9
    //   287b     WILD, 69.7 Hz  p50   5.3   p90  87.7   max 160.6
    //
    // **287b went wild with a QUIETER head than s1's** -- p50 5.3 against 6.2,
    // p90 87.7 against 69.4 only at the tail. So "s1 was not excited enough" is
    // not obviously true, and the peak-rate split (118 vs 161) is a threshold
    // fitted to four points, which is not evidence.
    //
    // **P292, committed before this runs: the model's threshold at 68.6 Hz lies
    // BELOW 117 deg/s, so it over-predicts s1 and the jitter model is
    // incomplete.** The excitation used in section 6 already peaks at only
    // 62.8 deg/s (20 deg at 0.5 Hz) and that was enough. If the threshold comes
    // out above 117 the prediction is refuted and s1 is explained by excitation
    // after all -- which would be the better outcome, and is why it is written
    // down before the number exists.
    //
    // Amplitude is held at 20 deg and the FREQUENCY swept, so the excursion
    // stays well inside X4's clamp and only the rate varies. One variable.
    std::printf("\n7. the excitation the model needs, at each measured rate\n");
    std::printf("%12s", "peak deg/s");
    const double rates[] = {28.0, 68.6};
    for (double hz : rates) std::printf("%14.1f Hz", hz);
    std::printf("\n");
    double thresh[2] = {0.0, 0.0};
    for (double peak : {5.0, 10.0, 20.0, 40.0, 62.8, 90.0, 118.0, 160.0, 220.0}) {
        std::printf("%12.1f", peak);
        for (int a = 0; a < 2; a++) {
            const double f = peak / (20.0 * 2.0 * M_PI); // amp 20 deg
            const double tick_ms = 1000.0 / rates[a];
            const Jit r = run_jitter(rates[a], (int)std::floor(40.0 / tick_ms),
                                     (int)std::ceil(165.0 / tick_ms),
                                     [f](int i) { return turning(i, f, 20.0); },
                                     20.0);
            std::printf("   %6.1f%% %6.1f", r.slam, r.max_exc);
            if (r.max_exc >= 64.9 && thresh[a] == 0.0) thresh[a] = peak;
        }
        std::printf("\n");
    }
    std::printf("%12s", "");
    for (int a = 0; a < 2; a++) std::printf("   %-13s", "slam%  max|cam|");
    std::printf("\n\n  threshold (first peak rate reaching the clamp): "
                "28.0 Hz -> %.0f deg/s, 68.6 Hz -> %.0f deg/s\n",
                thresh[0], thresh[1]);
    std::printf("  measured peaks: 287a 113.9, 285b-s1 117.5 (both CALM) | "
                "285b-s2 161.9, 287b 160.6 (both WILD)\n");

    // **The control: a head that does not move at all.** A model that slams
    // into the clamp with a FROZEN head is not "marginally unstable and needing
    // excitation" -- it is an unconditional oscillator, and then excitation can
    // never be the reason one session stayed calm. Run beside a FIXED delay of
    // the same mean, because if both behave alike the jitter is doing no work
    // and section 5's threshold on the MEAN is the whole story.
    std::printf("\n   control — a frozen head, and jitter against a fixed mean:\n");
    auto still = [](int) {
        x4vr::HeadAngles h;
        h.yaw_deg = 5.0f; h.pitch_deg = 0.0f; h.yaw_weight = 1.0f;
        return h;
    };
    for (double hz : {28.0, 68.6}) {
        const double tick_ms = 1000.0 / hz;
        const int lo = (int)std::floor(40.0 / tick_ms), hi = (int)std::ceil(165.0 / tick_ms);
        const int mean = (lo + hi) / 2;
        const Jit j = run_jitter(hz, lo, hi, still, 20.0);
        const Jit f = run_jitter(hz, mean, mean, still, 20.0);
        std::printf("     %5.1f Hz  jitter %d-%d: slam %5.1f%% max %5.2f | "
                    "FIXED %d: slam %5.1f%% max %5.2f\n", hz, lo, hi, j.slam,
                    j.max_exc, mean, f.slam, f.max_exc);
    }

    check(thresh[1] > 0.0 && thresh[1] < 117.0,
          "P292: at 68.6 Hz the model goes unstable BELOW s1's measured peak of "
          "117.5 deg/s — so it over-predicts s1, and excitation does not "
          "explain why that session stayed calm");

    // ---- 8. the two together, which is the combination nothing swept ------
    //
    // **Section 1 swept `period` at dead time 0 and needed 11 to slam. Section 5
    // swept dead time at `period` 1 and needed 6. Take 292 ran at neither: about
    // 1.7 steps per camera update AND about 5 ticks of dead time, and it was
    // violently unstable.** Each factor was cleared on its own and the pair was
    // never tried, which is how a mechanism survives two sweeps that both name it
    // innocent. They are not independent — `period` multiplies the loop gain
    // because head_look_observe() re-pins to an unchanged reading and re-sends a
    // correction already in flight, and dead time is what that extra gain then
    // rings against.
    //
    // Measured operating points, from the `dither` stream of each take:
    //
    //   take 291  tick 38.5 Hz  camera 34.5 Hz  period 1.12  delay ~3 ticks  CALM
    //   take 292  tick 75.8 Hz  camera 45.5 Hz  period 1.67  delay ~5 ticks  WILD
    //
    // `period` is an int here, so 1 and 2 bracket both takes.
    std::printf("\n8. period AND dead time together — the pair neither sweep "
                "tried\n");
    std::printf("%8s", "delay");
    for (int p : {1, 2, 3}) std::printf("   period %d", p);
    std::printf("      <- max |cam| deg, 65.00 is the clamp\n");
    int clamp_at[4] = {0, 0, 0, 0};
    for (int delay : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        std::printf("%8d", delay);
        for (int p : {1, 2, 3}) {
            const Result r = drive(p, 0.25f, 1.0f, 0.115f,
                                   [](int i) { return turning(i, 0.5, 20.0); },
                                   90 * 20, delay);
            std::printf("   %8.2f", r.max_excursion);
            if (r.max_excursion >= 64.9 && !clamp_at[p]) clamp_at[p] = delay;
        }
        std::printf("\n");
    }
    std::printf("  first dead time reaching the clamp: period 1 -> %d, "
                "period 2 -> %d, period 3 -> %d\n",
                clamp_at[1], clamp_at[2], clamp_at[3]);

    // **REFUTED, by the check written to be able to fail.** The boundary does not
    // move: 6 at period 1, 6 at period 2, 6 at period 3. Re-sending a correction
    // already in flight does NOT lower the dead time the loop tolerates, so it is
    // not what made take 292 wild, and section 1's gain multiplication does not
    // survive contact with a plant that has dead time. The story this section was
    // written to support is wrong and the assertion now records that.
    check(clamp_at[1] == clamp_at[2] && clamp_at[2] == clamp_at[3],
          "period does NOT interact with dead time — the clamp boundary is the "
          "same at 1, 2 and 3 steps per camera update, which refutes the "
          "re-issued-command mechanism");

    // What is left is the plain sum, in TICKS, against section 5's threshold --
    // and the term that was missing is not a new mechanism but a measured one.
    // `evflow` puts the time our motion events spend in X4's queue at 0.01 ms in
    // take 291 and **16.8 ms** in take 292, because at 72 Hz we push faster than
    // X4's drain takes them.
    //
    // **Restricted to windows that actually pushed motion**, because the summary
    // line averages over idle ones too and prints a queue time in windows
    // reporting `motion pushed 0.0 Hz` — its `max` is a running maximum that
    // never resets. The restriction makes the gap larger, not smaller: 0.01 ms
    // over 140 motion windows in 291 against 16.79 mean / 19.89 median over 149
    // in 292. Checked before the account below was built on it.
    //
    //   take 291  tick 26.0 ms   70 ms transport + 0.01 ms queue = 2.7 ticks  CALM
    //   take 292  tick 13.2 ms   70 ms transport + 16.4 ms queue = 6.5 ticks  WILD
    //
    // Opposite sides of the 6 above. **This is an account, not a test**: it was
    // assembled after seeing which take went wild, and every number in it was
    // measured for another purpose. It earns a prediction, not a conclusion.
    const double tick291 = 26.0, tick292 = 13.2;
    const double d291 = (70.0 + 0.01) / tick291, d292 = (70.0 + 16.43) / tick292;
    std::printf("\n  measured operating points against the %d-tick threshold:\n",
                clamp_at[1]);
    std::printf("    take 291  (70.00 + 0.01) ms / %.1f ms = %.1f ticks  %s\n",
                tick291, d291, d291 < clamp_at[1] ? "calm side" : "wild side");
    std::printf("    take 292  (70.00 + 16.43) ms / %.1f ms = %.1f ticks  %s\n",
                tick292, d292, d292 < clamp_at[1] ? "calm side" : "wild side");
    check(d291 < clamp_at[1] && d292 >= clamp_at[1],
          "total delay in TICKS, including the measured queue time, puts 291 and "
          "292 on opposite sides of the threshold — the queue term is what the "
          "model was missing, not a new mechanism");

    std::printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
