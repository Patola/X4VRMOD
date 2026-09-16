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

#include <algorithm>
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
    // **p99 of the per-update move, and its absence is why section 8 read
    // `period` as harmless.** Every statistic above is blind in the same way the
    // LOG's slam counter was: `max_excursion` is dominated by the 20 deg the head
    // is asking for and only moves once the loop reaches a stop, and `slam_pct`
    // cannot fire below 20 deg per update. Take 285b's episode separates from its
    // own calm windows at 44.69 deg against 4.76 on exactly this statistic, so it
    // is the one the model has to report to be talking about the same thing.
    // Fixed in tools/wildness.py first and left broken here, one file over.
    double p99_move = 0.0;
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
    std::vector<double> moves;
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
                moves.push_back(move);
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
    if (!moves.empty()) {
        std::sort(moves.begin(), moves.end());
        r.p99_move = moves[(size_t)(0.99 * (double)(moves.size() - 1))];
    }
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
    // **Reported as p99 |dyaw| over its BASELINE, because `period` moves both.**
    // A camera sampled every `period` ticks shows `period` times as much motion
    // per update from smooth tracking alone -- the head asks for at most
    // amp*2*pi*hz = 62.8 deg/s, so a perfectly stable loop reads
    // 62.8*period/90 deg per update. Reading the raw p99 would credit `period`
    // with an instability that is really just a coarser sample. The ratio is 1.0
    // for a loop that is merely tracking, whatever the period, and grows only
    // when the camera is moving further than it was asked to.
    std::printf("%8s", "delay");
    for (int p : {1, 2, 3}) std::printf("    period %d", p);
    std::printf("   <- p99 |dyaw| / smooth-tracking baseline\n");
    int clamp_at[4] = {0, 0, 0, 0};
    double ratio[4][9] = {{0}};
    for (int delay : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        std::printf("%8d", delay);
        for (int p : {1, 2, 3}) {
            const Result r = drive(p, 0.25f, 1.0f, 0.115f,
                                   [](int i) { return turning(i, 0.5, 20.0); },
                                   90 * 20, delay);
            const double base = 20.0 * 2.0 * M_PI * 0.5 * (double)p / 90.0;
            ratio[p][delay] = r.p99_move / base;
            std::printf("   %9.2f", ratio[p][delay]);
            if (r.max_excursion >= 64.9 && !clamp_at[p]) clamp_at[p] = delay;
        }
        std::printf("\n");
    }
    std::printf("  first dead time reaching the clamp: period 1 -> %d, "
                "period 2 -> %d, period 3 -> %d\n",
                clamp_at[1], clamp_at[2], clamp_at[3]);

    // **First refuted, then found — and the difference was the statistic.**
    //
    // This section originally read `max_excursion` and concluded that period does
    // not interact with dead time, because the clamp is reached at 6 whatever the
    // period. That is still true and `clamp_at` still says so. But max excursion
    // is dominated by the 20 deg the head is asking for and only moves once the
    // loop reaches a stop, so it cannot see a loop that is oscillating and has not
    // got there yet -- the same blindness that made the LOG's slam counter read
    // 0.0% on take 285b-s1 and label it calm. I found that in the log, fixed it in
    // tools/wildness.py, and left the identical defect here, one file over.
    // [[x4vr-fixed-here-broken-next-door]]
    //
    // With p99 over its baseline the mechanism is visible: at dead time 5, just
    // under the clamp, the ratio grows 1.15 -> 1.56 -> 2.25 with period, and
    // period 3 passes "moving twice as far as it was asked" one whole tick of dead
    // time before period 1 does.
    int cross[4] = {0, 0, 0, 0};
    for (int p : {1, 2, 3})
        for (int d = 0; d <= 8 && !cross[p]; d++)
            if (ratio[p][d] >= 2.0) cross[p] = d;
    std::printf("  ratio first reaches 2.0 at dead time: period 1 -> %d, "
                "period 2 -> %d, period 3 -> %d\n", cross[1], cross[2], cross[3]);
    check(ratio[1][5] < ratio[2][5] && ratio[2][5] < ratio[3][5],
          "period DOES amplify the oscillation — at the dead time just below the "
          "clamp the normalised p99 grows monotonically with it");
    check(cross[3] < cross[1],
          "period lowers the dead time at which the camera starts moving further "
          "than it was asked to — the boundary moves, which max_excursion could "
          "not see");

    // **And it is nowhere near big enough, which is the open work.** Over 491
    // windows, holding the tick rate and varying only ticks-per-observation takes
    // the wild share from 2% to 97%; holding ticks-per-observation and varying the
    // tick rate leaves it flat (3/0/0/4, and 92/97/100). So in the game `period`
    // is the dominant variable and the delay in ticks is nearly irrelevant. Here
    // it is the reverse: dead time decides, and tripling the period buys one tick.
    // Asserting the gap keeps it from being quietly closed by a model that
    // overclaims later.
    check(cross[1] - cross[3] <= 2,
          "the model's period effect is still only ~1 tick of dead time, against "
          "data where period moves the wild share 2% -> 97% with the tick rate "
          "held — the model reproduces the MECHANISM but not its WEIGHT, and that "
          "gap is unexplained");

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
    const double d291 = (70.0 + 0.01) / tick291, d292 = (70.0 + 16.79) / tick292;
    std::printf("\n  measured operating points against the %d-tick threshold:\n",
                clamp_at[1]);
    std::printf("    take 291  (70.00 + 0.01) ms / %.1f ms = %.1f ticks  %s\n",
                tick291, d291, d291 < clamp_at[1] ? "calm side" : "wild side");
    std::printf("    take 292  (70.00 + 16.79) ms / %.1f ms = %.1f ticks  %s\n",
                tick292, d292, d292 < clamp_at[1] ? "calm side" : "wild side");
    check(d291 < clamp_at[1] && d292 >= clamp_at[1],
          "total delay in TICKS, including the measured queue time, puts 291 and "
          "292 on opposite sides of the threshold — the queue term is what the "
          "model was missing, not a new mechanism");

    // ---- 9. the measured operating points, in a TIME base -----------------
    //
    // Sections 1-8 count in ticks, so the tick rate is a free parameter and the
    // queue time has nowhere to live. This one takes **four measured numbers per
    // take and invents none**: the servo rate, X4's camera publish rate, the
    // transport delay, and the time our events spend in X4's queue.
    //
    //                tick     camera   transport   queue    Patola
    //   take 291   38.5 Hz   34.5 Hz     70 ms    0.01 ms   calm
    //   take 292   75.8 Hz   45.5 Hz     70 ms   16.79 ms   WILD
    //
    // The queue term is structural rather than a tuning choice. Without
    // X4VR_TICK_HZ the injector calls headlook_tick() from INSIDE its
    // SDL_PeepEvents interception, so the event is pushed while X4 is in its own
    // drain loop and the same loop consumes it -- 0.01 ms. With X4VR_TICK_HZ the
    // servo runs on our thread and the event waits for X4's next drain, which
    // `pumprate` measures as CLUSTERED, so the wait is far longer than half a
    // drain period: 19.89 ms median against the 6.26 a uniform drain predicts.
    //
    // **A queue model was refuted before this was written.** "The event waits for
    // the next drain at rate D" predicts 12.85 ms for 291 (measured 0.01) and
    // 6.26 ms for 292 (measured 19.89), and correlates with the per-window
    // measurement at -0.05 and +0.04. So the queue time is taken here as a
    // measured input, not derived from a mechanism I could not confirm.
    std::printf("\n9. the measured operating points, in a time base\n");
    struct Point {
        const char *name; double tick_hz, cam_hz, transport_ms, queue_ms;
        bool wild;
    };
    const Point pts[2] = {
        {"take 291", 38.5, 34.5, 70.0,  0.01, false},
        {"take 292", 75.8, 45.5, 70.0, 16.79, true},
    };
    // **Two clamps, because the run says 65 is the wrong one.** The dither stream
    // has take 292's camera reaching 79.64 deg and a single update moving 81.57,
    // neither of which a 65 deg stop can produce -- and `<internal>` declares
    // `maxyaw="65" trackmaxyaw="85"`. Running both makes that a test rather than
    // a remark: if 85 is the operative limit the excursion should land near the
    // measured 79.64, and if it does not, trackmaxyaw is not the explanation.
    std::printf("%10s %6s %9s %9s %9s %10s %10s\n", "take", "limit",
                "delay tk", "slam%", "max move", "max |cam|", "reversals");
    bool ok291 = false, ok292 = false;
    double exc85_292 = 0.0, move85_292 = 0.0;
    for (float limit : {65.0f, 85.0f})
    for (const Point &pt : pts) {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f; s.lead_scale = 1.0f;
        const double dt = 1.0 / pt.tick_hz;
        const double lat = (pt.transport_ms + pt.queue_ms) / 1000.0;
        const double cam_dt = 1.0 / pt.cam_hz;
        std::vector<std::pair<double, float>> inflight; // (due seconds, degrees)
        float yaw = 0.0f, pub = 0.0f, prev_pub = 0.0f;
        double next_pub = cam_dt, prev_vel = 0.0;
        int updates = 0, slams = 0, reversals = 0;
        double max_move = 0.0, max_exc = 0.0;
        const int steps = (int)(pt.tick_hz * 20.0);
        for (int i = 0; i < steps; i++) {
            const double t = i * dt;
            x4vr::head_look_observe(s, pub, 0.0f);
            const x4vr::HeadAngles h = turning(i, 0.5 / (pt.tick_hz / 90.0), 20.0);
            const x4vr::Delta d = x4vr::head_look_step(s, h);
            inflight.emplace_back(t + lat,
                                  s.sign_yaw * (float)d.dx * 0.115f);
            // everything whose time has come, integrated at the clamp
            for (size_t k = 0; k < inflight.size();) {
                if (inflight[k].first <= t) {
                    yaw = x4vr::clampf(yaw + inflight[k].second, -limit, limit);
                    inflight.erase(inflight.begin() + (long)k);
                } else { k++; }
            }
            if (t >= next_pub) {                 // X4 publishes on its OWN grid
                next_pub += cam_dt;
                pub = yaw; updates++;
                const double mv = std::fabs((double)pub - prev_pub);
                if (mv > max_move) max_move = mv;
                if (mv > 20.0) slams++;
                const double vel = (double)pub - prev_pub;
                if (vel * prev_vel < 0.0) reversals++;
                if (std::fabs(vel) > 1e-6) prev_vel = vel;
                prev_pub = pub;
                if (std::fabs((double)pub) > max_exc) max_exc = std::fabs((double)pub);
            }
        }
        const double slam_pct = updates ? 100.0 * slams / updates : 0.0;
        std::printf("%10s %6.0f %9.2f %8.1f%% %9.2f %10.2f %10d\n", pt.name,
                    (double)limit,
                    (pt.transport_ms + pt.queue_ms) / 1000.0 * pt.tick_hz,
                    slam_pct, max_move, max_exc, reversals);
        if (limit > 70.0f && pt.wild) { exc85_292 = max_exc; move85_292 = max_move; }
        if (limit > 70.0f) continue;   // the pass/fail pair is judged at 65
        if (!pt.wild) ok291 = max_exc < 64.9;
        else          ok292 = max_exc >= 64.9;
    }

    // **This is the falsifiable form of the account in section 8.** There the
    // numbers were compared to a threshold by hand; here the real
    // head_look_step() runs against them and either reproduces Patola's two
    // reports or does not. Every input was measured for another purpose.
    check(ok291, "at take 291's measured rates the model stays off the clamp, "
                 "as Patola reported");
    check(ok292, "at take 292's measured rates the model reaches the clamp — if "
                 "this fails, the queue term does NOT explain take 292 and the "
                 "account in section 8 is wrong");

    // **The limit-85 row is NOT evidence for trackmaxyaw, and the check that
    // said so could not fail.** The model pins at 65.00 given 65 and 85.00 given
    // 85: it saturates at whatever stop it is handed, so asserting the result
    // falls "near the measurement" tested my choice of constant. Hand it 100 and
    // it reads 100.
    //
    // What the LOG says, with no model involved: take 292's camera reached
    // **79.64 deg** and one update moved **81.57**. That excludes a 65 deg stop
    // arithmetically — a position limit cannot be exceeded by 14.6 deg — and it
    // is consistent with `trackmaxyaw="85"`, which the real camera never reached.
    // Consistent is not confirmed, and this model cannot get past that, because a
    // run that pins at its clamp says nothing about where the clamp is.
    std::printf("  take 292 measured max |cam| 79.64, max move 81.57\n");
    std::printf("  model pins at its own stop: %.2f at limit 85, and 65.00 at "
                "limit 65 — it saturates at whichever it is given, so it cannot "
                "locate X4's\n", exc85_292);
    check(79.64 > 65.0,
          "the MEASURED 79.64 deg excludes maxyaw=65 as the operative stop in "
          "take 292 — arithmetic on the log, not a model output");
    check(move85_292 < 40.0,
          "the model does NOT reproduce the measured 81.57 deg single update at "
          "either limit — something about take 292's excursion is still "
          "unexplained, and this records that rather than hiding it");

    // ---- 10. the servo overshoots its OWN limit, and X4 used to hide it ----
    //
    // Every take before 292 ran with X4's stock 65 deg clamp, which is the same
    // number as `yaw_limit_deg`. A camera stopped by X4 and a camera stopped by
    // us look identical, so three months of takes could not see that the servo
    // sails past its own target. With #68's extension loaded the wall is gone
    // and X4's readback reaches 79-82 deg against a servo asking for at most 65.
    //
    // The plant here is deliberately UNCLAMPED, which is the extended path, and
    // the servo keeps its 65. Anything past 65 is ours.
    std::printf("\n10. overshoot past the servo's OWN limit, plant unclamped\n");
    std::printf("%8s %12s %12s %12s\n", "delay", "max |cam|", "overshoot",
                "in flight");
    double over_at3 = 0.0;
    for (int delay : {0, 1, 2, 3, 4, 5, 6}) {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f; s.lead_scale = 1.0f;
        s.yaw_limit_deg = 65.0f;
        // A head that parks well past the limit, which is what asks the servo to
        // sit ON its stop -- the condition the real takes spend 3-4% of their
        // samples in.
        std::vector<float> pipe((size_t)delay, 0.0f);
        float cam = 0.0f, worst = 0.0f;
        double flight = 0.0;
        const int steps = 90 * 12;
        for (int i = 0; i < steps; i++) {
            x4vr::head_look_observe(s, cam, 0.0f);
            x4vr::HeadAngles h;
            h.yaw_deg = 80.0f;            // past the servo's 65 on purpose
            h.pitch_deg = 0.0f; h.yaw_weight = 1.0f; h.yaw_reliable = true;
            const x4vr::Delta d = x4vr::head_look_step(s, h);
            pipe.push_back(s.sign_yaw * (float)d.dx * 0.115f);
            cam += pipe.front();
            pipe.erase(pipe.begin());
            if (std::fabs(cam) > worst) worst = std::fabs(cam);
            if (i == steps / 2) {         // command sitting in the pipe, mid-run
                flight = 0.0;
                for (float v : pipe) flight += std::fabs(v);
            }
        }
        std::printf("%8d %12.2f %12.2f %12.2f\n", delay, worst, worst - 65.0,
                    flight);
        if (delay == 3) over_at3 = worst - 65.0;
    }
    std::printf("  measured, X4's readback against a servo asking 65: "
                "take 292 reached 76.79 (+11.79), take 293 78.79 (+13.79)\n");

    // **The claim is that dead time alone produces it.** If the model overshoots
    // by nothing at the measured 3-tick delay, the mechanism is something else
    // and this section says so rather than the docs assuming it.
    check(over_at3 > 1.0,
          "the servo overshoots its own limit at the measured 3-tick dead time — "
          "commands already in flight keep arriving after the target is reached");
    check(over_at3 < 30.0,
          "and by an amount of the same order as the 11.8-13.8 deg X4 actually "
          "reported, not a runaway");


    // ---- 11. the tick period WANDERS, and every servo constant rides on it --
    //
    // Take 294 measured the servo's own iteration period at p10 9, p50 15,
    // p90 40, p99 56 ms -- a 4.4x spread, because a tick happens only when a
    // drain poll happens to find a new pose. servo_kp and max_step_deg are both
    // PER TICK, so the loop is a different controller from one second to the
    // next: lag is (1/kp - 1)*dt, slew is max_step/dt, and overshoot is
    // 0.5*(dead_time_s/dt)*max_step -- the first two worsen as dt grows and the
    // third worsens as it shrinks.
    //
    // **The intervals below are REAL**, a contiguous 300-tick window lifted from
    // /tmp/x4vr-take294.log and chosen because its p10/p50/p90 match the whole
    // run's (10/16/40 against 9/15/40). Contiguous on purpose: the polls arrive
    // in bursts and a resampled or synthesised sequence would lose the serial
    // structure that makes the spread bite.
    {
    static const unsigned char kTick294Ms[] = {
    21, 10, 13, 20, 22, 30, 21, 10, 10, 11, 10, 10, 11, 10, 10, 21, 11, 16, 30, 10, 10, 11, 21, 13,
    30, 20, 10, 11, 10, 11, 10, 10, 11, 20, 11, 20, 30, 10, 10, 11, 11, 10, 11, 21, 10, 11, 19, 30,
    11, 10, 11, 10, 12, 11, 10, 23, 15, 29, 18,  8,  7, 10, 21, 11, 10, 20, 30, 10, 11, 10, 11, 22,
    20, 39, 26, 22, 40, 11, 10, 11, 11, 11, 10, 22, 10, 11, 21, 30, 34, 32, 14, 27, 12, 12, 11, 10,
     9, 11, 21, 10, 21, 12, 40, 11, 10, 10, 11, 21, 10, 20, 31, 10, 10, 11, 11, 10, 22, 12, 31, 21,
    11, 19, 30, 11, 10, 11, 21, 13, 31, 21, 10, 13, 31, 21, 10, 10, 11, 10, 11, 11, 10, 11, 11, 10,
    11, 21, 12, 20, 21, 13, 31, 20, 11, 11, 10, 11, 11, 11, 11, 10, 11, 11, 11, 13, 20, 21, 11, 11,
    10, 11, 10, 11, 11, 10, 11, 20, 18, 24, 17, 10, 10, 22, 18, 29, 11, 10, 10, 21, 10, 13, 29, 11,
    10, 11, 11, 10, 11, 10, 11, 11, 21, 13, 21, 11, 11, 11, 11, 11, 21, 18, 13, 20, 24, 31, 21, 11,
    10, 11, 11, 11, 10, 11, 11, 11, 10, 22, 19, 34, 34, 40, 66, 43, 33, 44, 34, 46, 43, 46, 34, 24,
    32, 34, 33, 45, 17, 44, 39, 34, 39, 38, 33, 34, 34, 33, 33, 34, 33, 32, 34, 45, 45, 33, 33, 32,
    34, 33, 34, 33, 34, 40, 48, 41, 38, 32, 46, 40, 45, 37, 55, 34, 33, 34, 45, 22, 40, 27, 34, 44,
    45, 27, 55, 34, 44, 45, 33, 22, 23, 56, 45, 45,
    };
    const int NT = (int)(sizeof kTick294Ms / sizeof kTick294Ms[0]);
    std::printf("\n11. take 294's REAL tick sequence, rate fix off vs on\n");
    std::printf("%22s %10s %10s %12s %12s\n", "", "lag ms", "rms deg",
                "tremble p90", "overshoot");
    double lag_off = 0.0, lag_on = 0.0, trem_off = 0.0, trem_on = 0.0;
    for (int fix = 0; fix < 2; fix++) {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        s.servo_kp = 0.25f;
        s.yaw_limit_deg = 85.0f;
        if (fix)
            s.rate_fix_tau_ms = 80.0f;   // 1/(0.25*50) — today's equivalent
        // 3 ticks of dead time, take 291's measurement, held in WALL CLOCK so
        // it does not silently change with the tick rate the way a fixed pipe
        // depth would -- that would build the answer into the harness.
        const double kDeadS = 3.0 / 50.0;
        std::vector<std::pair<double, float>> pipe;   // (arrival time, step)
        double t = 0.0, cam = 0.0, err_sq = 0.0, worst_over = 0.0;
        std::vector<double> lagsum, tremble;
        double prev_err = 0.0;
        int n = 0;
        for (int rep = 0; rep < 8; rep++) {
            for (int i = 0; i < NT; i++) {
                const double dt = kTick294Ms[i] / 1000.0;
                t += dt;
                // A head sweeping at a realistic rate -- 0.35 Hz, +-60 deg, so
                // the peak is ~130 deg/s, inside take 294's measured band.
                const double head = 60.0 * std::sin(2.0 * 3.14159265 * 0.35 * t);
                while (!pipe.empty() && pipe.front().first <= t) {
                    cam += pipe.front().second;
                    pipe.erase(pipe.begin());
                }
                x4vr::head_look_observe(s, (float)cam, 0.0f);
                x4vr::HeadAngles h;
                h.yaw_deg = (float)head; h.pitch_deg = 0.0f;
                h.yaw_weight = 1.0f; h.yaw_reliable = true;
                const x4vr::Delta d = x4vr::head_look_step(s, h, (float)dt);
                pipe.push_back({t + kDeadS,
                                s.sign_yaw * (float)d.dx * 0.115f});
                const double e = cam - head;
                err_sq += e * e;
                if (rep) {                       // let the loop settle first
                    tremble.push_back(std::fabs(e - prev_err));
                    if (std::fabs(cam) > 85.0)
                        worst_over = std::fmax(worst_over, std::fabs(cam) - 85.0);
                }
                prev_err = e;
                n++;
            }
        }
        // Lag in ms: the error of a proportional servo tracking a sinusoid is
        // (rate * lag), so dividing the rms error by the rms head rate gives a
        // TIME -- the same quantity the take measures by cross-correlation, and
        // it is derived here rather than fitted.
        double rate_sq = 0.0;
        for (int i = 0; i < 2000; i++) {
            const double tt = i * 0.001;
            const double r = 60.0 * 2.0 * 3.14159265 * 0.35
                             * std::cos(2.0 * 3.14159265 * 0.35 * tt);
            rate_sq += r * r;
        }
        const double rms_err = std::sqrt(err_sq / n);
        const double rms_rate = std::sqrt(rate_sq / 2000);
        const double lag_ms = 1000.0 * rms_err / rms_rate;
        std::sort(tremble.begin(), tremble.end());
        const double tp90 = tremble[(size_t)(0.90 * tremble.size())];
        std::printf("%22s %10.1f %10.2f %12.3f %12.2f\n",
                    fix ? "rate fix ON" : "today (off)", lag_ms, rms_err, tp90,
                    worst_over);
        if (fix) { lag_on = lag_ms; trem_on = tp90; }
        else     { lag_off = lag_ms; trem_off = tp90; }
    }
    // **The check I first wrote here FAILED, and it was the check that was
    // wrong, not the fix.** I asserted a quarter off the lag. It delivers 14%,
    // and the arithmetic says it should:
    //
    //   lag = (1/kp_eff - 1) * dt,  and kp_eff = kp * dt * nominal_hz
    //      => lag = 1/(kp*nominal_hz) - dt  =  tau - dt
    //
    // With the nominal set to today's mean rate, tau is 1/(0.25*50) = 80 ms --
    // today's lag. So the rate fix makes the loop the SAME controller at every
    // tick rate; it does not by itself make it a faster one. Consistency and
    // speed are two knobs and this is the first.
    //
    // Keeping the failed prediction visible rather than quietly retuning the
    // threshold: [[optimise-one-axis-move-the-error]] is exactly this mistake
    // one level up.
    check(lag_on <= lag_off,
          "11: the rate fix does not make the lag worse");
    check(trem_on <= trem_off,
          "11: ...and does not make the tremble worse");
    std::printf("      lag %.1f -> %.1f ms (%.0f%% of today), tremble p90 "
                "%.3f -> %.3f deg\n", lag_off, lag_on,
                100.0 * lag_on / lag_off, trem_off, trem_on);

    // **What the fix ACTUALLY buys: the loop stops depending on the tick.**
    // The claim is consistency, so the measurement is the SPREAD of the error
    // across tick-period bins, not its mean.
    {
        std::printf("      per-tick-period tremble p90, deg:\n");
        std::printf("      %10s %12s %12s\n", "tick ms", "off", "ON");
        double spread[2] = {0.0, 0.0};
        double lo[2] = {1e9, 1e9}, hi[2] = {0.0, 0.0};
        for (int fix = 0; fix < 2; fix++) {
            std::vector<double> byb[4];
            x4vr::HeadLook s;
            s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f;
            s.yaw_limit_deg = 85.0f;
            if (fix) s.rate_fix_tau_ms = 80.0f;
            const double kDeadS = 3.0 / 50.0;
            std::vector<std::pair<double, float>> pipe;
            double t = 0.0, cam = 0.0, prev_err = 0.0;
            for (int rep = 0; rep < 8; rep++)
                for (int i = 0; i < NT; i++) {
                    const double dt = kTick294Ms[i] / 1000.0;
                    t += dt;
                    const double head =
                        60.0 * std::sin(2.0 * 3.14159265 * 0.35 * t);
                    while (!pipe.empty() && pipe.front().first <= t) {
                        cam += pipe.front().second;
                        pipe.erase(pipe.begin());
                    }
                    x4vr::head_look_observe(s, (float)cam, 0.0f);
                    x4vr::HeadAngles h;
                    h.yaw_deg = (float)head; h.pitch_deg = 0.0f;
                    h.yaw_weight = 1.0f; h.yaw_reliable = true;
                    const x4vr::Delta d =
                        x4vr::head_look_step(s, h, (float)dt);
                    pipe.push_back({t + kDeadS,
                                    s.sign_yaw * (float)d.dx * 0.115f});
                    const double e = cam - head;
                    if (rep) {
                        const int b = kTick294Ms[i] < 12 ? 0
                                    : kTick294Ms[i] < 18 ? 1
                                    : kTick294Ms[i] < 28 ? 2 : 3;
                        byb[b].push_back(std::fabs(e - prev_err));
                    }
                    prev_err = e;
                }
            for (int b = 0; b < 4; b++) {
                if (byb[b].size() < 30) continue;
                std::sort(byb[b].begin(), byb[b].end());
                const double v = byb[b][(size_t)(0.90 * byb[b].size())];
                lo[fix] = std::fmin(lo[fix], v);
                hi[fix] = std::fmax(hi[fix], v);
            }
            spread[fix] = hi[fix] / std::fmax(lo[fix], 1e-9);
        }
        std::printf("      %10s %12.2fx %11.2fx   (max bin / min bin)\n",
                    "spread", spread[0], spread[1]);
        check(spread[1] < spread[0],
              "11: the rate fix FLATTENS the tremble across tick periods — "
              "that is the property it exists for");
    }

    // **And where the LAG knob now lives.** With the fix on, the two per-tick
    // constants collapse into one time constant, tau = 1/(kp * nominal_hz), so
    // the nominal IS the lag dial and it is in units that mean something.
    // This is the table take 295's arm B is chosen from.
    {
        std::printf("      tau sweep, slew held at 600 deg/s:\n");
        std::printf("      %10s %10s %10s %12s\n", "-", "tau ms",
                    "lag ms", "overshoot");
        for (double tau_ms : {80.0, 57.0, 40.0, 29.0}) {
            x4vr::HeadLook s;
            s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f;
            s.yaw_limit_deg = 85.0f;
            s.rate_fix_tau_ms = (float)tau_ms;
            const double kDeadS = 3.0 / 50.0;
            std::vector<std::pair<double, float>> pipe;
            double t = 0.0, cam = 0.0, err_sq = 0.0, over = 0.0;
            int n = 0;
            for (int rep = 0; rep < 8; rep++)
                for (int i = 0; i < NT; i++) {
                    const double dt = kTick294Ms[i] / 1000.0;
                    t += dt;
                    const double head =
                        60.0 * std::sin(2.0 * 3.14159265 * 0.35 * t);
                    while (!pipe.empty() && pipe.front().first <= t) {
                        cam += pipe.front().second;
                        pipe.erase(pipe.begin());
                    }
                    x4vr::head_look_observe(s, (float)cam, 0.0f);
                    x4vr::HeadAngles h;
                    h.yaw_deg = (float)head; h.pitch_deg = 0.0f;
                    h.yaw_weight = 1.0f; h.yaw_reliable = true;
                    const x4vr::Delta d =
                        x4vr::head_look_step(s, h, (float)dt);
                    pipe.push_back({t + kDeadS,
                                    s.sign_yaw * (float)d.dx * 0.115f});
                    if (rep) {
                        const double e = cam - head;
                        err_sq += e * e; n++;
                        if (std::fabs(cam) > 60.5)
                            over = std::fmax(over, std::fabs(cam) - 60.0);
                    }
                }
            const double rms = std::sqrt(err_sq / n);
            // **93.27, not 131.9.** The first draft divided by the PEAK head
            // rate here and by the RMS in the table above, so the same setting
            // read 56.9 ms in one place and 78.1 in the other. Two routes to one
            // quantity disagreeing means one of them is wrong
            // ([[three-contradictory-measurements]]); rms of A*cos is A/sqrt(2),
            // and 60*2*pi*0.35/sqrt(2) = 93.27 deg/s.
            const double kRmsRate = 60.0 * 2.0 * 3.14159265 * 0.35 / 1.41421356;
            std::printf("      %10.0f %10.1f %10.1f %12.2f\n",
                        1000.0 / (0.25 * tau_ms) * 1000.0 / 1000.0, tau_ms,
                        1000.0 * rms / kRmsRate, over);
        }
    }

    // NEGATIVE CONTROL: at a STEADY tick equal to the nominal, the fix must be
    // arithmetically inert. If it changes anything here it is not correcting
    // the wander, it is just a different tuning wearing the wander's name.
    {
        double got[2];
        for (int fix = 0; fix < 2; fix++) {
            x4vr::HeadLook s;
            s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f;
            s.yaw_limit_deg = 85.0f;
            if (fix) s.rate_fix_tau_ms = 80.0f;
            double cam = 0.0, acc = 0.0;
            std::vector<float> pipe(3, 0.0f);
            for (int i = 0; i < 1200; i++) {
                const double head = 60.0 * std::sin(2.0 * 3.14159265 * 0.35
                                                    * i * 0.02);
                x4vr::head_look_observe(s, (float)cam, 0.0f);
                x4vr::HeadAngles h;
                h.yaw_deg = (float)head; h.pitch_deg = 0.0f;
                h.yaw_weight = 1.0f; h.yaw_reliable = true;
                const x4vr::Delta d = x4vr::head_look_step(s, h, 0.020f);
                pipe.push_back(s.sign_yaw * (float)d.dx * 0.115f);
                cam += pipe.front();
                pipe.erase(pipe.begin());
                acc += std::fabs(cam - head);
            }
            got[fix] = acc;
        }
        check(std::fabs(got[0] - got[1]) < 1e-6 * std::fabs(got[0]) + 1e-6,
              "11: at a steady 50 Hz — the nominal — the fix is EXACTLY inert");
        std::printf("      steady 50 Hz: off %.4f, on %.4f (identical)\n",
                    got[0], got[1]);
    }
    }


    // ---- 12. TAKE 295: the regime section 11 never entered -----------------
    //
    // Section 11 drove a smooth 0.35 Hz sweep whose error never reached
    // max_step/kp, so the step NEVER SATURATED and the ceiling was never
    // exercised. The fix passed, shipped into take 295, and the frustum span:
    // ARM_B commanded 32.77 deg in a tick against a design ceiling of 12, lag
    // went to 448-2072 ms against the control arm's 41-153, and 39 of the run's
    // 40 saturated-with-head-centred commands were in it.
    //
    // [[test-the-fix-where-it-can-act]]. The head here MOVES -- fast sweeps that
    // drive the error well past max_step/kp -- because that is the only regime
    // in which a step ceiling means anything at all.
    {
    std::printf("\n12. the SATURATING regime, which is where take 295 broke\n");
    std::printf("%22s %12s %12s %12s\n", "", "worst step", "slew deg/s",
                "worst |cam|");
    double worst_step[3] = {0, 0, 0}, worst_slew[3] = {0, 0, 0},
           worst_cam_[3] = {0, 0, 0};
    for (int cfg = 0; cfg < 3; cfg++) {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f;
        s.yaw_limit_deg = 85.0f;
        const char *name = "today (fix off)";
        if (cfg == 1) {
            // Take 295's ARM_B, reconstructed: tau 40 ms AND the slew ceiling
            // dragged to 1200 by the single knob.
            s.rate_fix_tau_ms = 40.0f;
            s.rate_fix_max_rate_deg_s = 1200.0f;
            name = "295 ARM_B (coupled)";
        } else if (cfg == 2) {
            // The same speed-up with the slew left where it was.
            s.rate_fix_tau_ms = 40.0f;
            s.rate_fix_max_rate_deg_s = 600.0f;
            name = "tau 40, slew 600";
        }
        const double kDeadS = 0.060;
        std::vector<std::pair<double, float>> pipe;
        double t = 0.0, cam = 0.0, worst_cam = 0.0, wstep = 0.0, wslew = 0.0;
        for (int i = 0; i < 6000; i++) {
            // Deliberately harsh: 1.4 Hz, +-70 deg => ~615 deg/s peak, which is
            // a real snap-glance and the thing take 293's protocol produced.
            const double dt = (i % 5 == 0) ? 0.040 : 0.012;  // the real spread
            t += dt;
            const double head = 70.0 * std::sin(2.0 * 3.14159265 * 1.4 * t);
            while (!pipe.empty() && pipe.front().first <= t) {
                cam += pipe.front().second;
                pipe.erase(pipe.begin());
            }
            x4vr::head_look_observe(s, (float)cam, 0.0f);
            x4vr::HeadAngles h;
            h.yaw_deg = (float)head; h.pitch_deg = 0.0f;
            h.yaw_weight = 1.0f; h.yaw_reliable = true;
            const x4vr::Delta d = x4vr::head_look_step(s, h, (float)dt);
            const double st = std::fabs((double)d.dx * 0.115);
            wstep = std::fmax(wstep, st);
            wslew = std::fmax(wslew, st / dt);
            pipe.push_back({t + kDeadS, s.sign_yaw * (float)d.dx * 0.115f});
            worst_cam = std::fmax(worst_cam, std::fabs(cam));
        }
        worst_step[cfg] = wstep;
        worst_slew[cfg] = wslew;
        worst_cam_[cfg] = worst_cam;
        std::printf("%22s %12.2f %12.0f %12.2f\n", name, wstep, wslew,
                    worst_cam);
    }
    check(worst_slew[1] > 1.15 * worst_slew[0],
          "12: the COUPLED setting take 295 ran drives the camera FASTER than "
          "today — 1198 against 997 deg/s, the coupling defect, offline");
    check(worst_slew[2] <= worst_slew[0],
          "12: separating tau from the slew ceiling puts the slew back");
    // **And the second check I wrote here failed too, for a REASON that
    // matters more than the fix.** I expected separation to remove the
    // excursion; it does not. tau 40 ms drives kp_eff to its 0.60 ceiling, and
    // a gain that high against a 60 ms dead time overshoots whatever the slew
    // limit is: worst |cam| 96.49 against today's 81.42.
    //
    // A proportional loop with transport delay is only stable while its time
    // constant stays comfortably ABOVE the dead time. The dead time is 60 ms,
    // so tau 40 was already past the margin before the coupling made it worse.
    // Section 11's sweep could not see this because it never saturated.
    check(worst_cam_[2] > worst_cam_[0],
          "12: tau 40 ms is UNSAFE at a 60 ms dead time even with the slew "
          "separated — recorded as a finding, not a bug to tune away");

    // So: where does tau become safe? Swept in the regime that can answer.
    std::printf("      tau in the SATURATING regime, slew fixed at 600 deg/s:\n");
    std::printf("      %10s %12s %12s %14s\n", "tau ms", "worst |cam|",
                "vs 85 limit", "verdict");
    for (double tau_ms : {200.0, 140.0, 110.0, 80.0, 60.0, 40.0}) {
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f;
        s.yaw_limit_deg = 85.0f;
        s.rate_fix_tau_ms = (float)tau_ms;
        s.rate_fix_max_rate_deg_s = 600.0f;
        const double kDeadS = 0.060;
        std::vector<std::pair<double, float>> pipe;
        double t = 0.0, cam = 0.0, wc = 0.0, err = 0.0;
        int n = 0;
        for (int i = 0; i < 6000; i++) {
            const double dt = (i % 5 == 0) ? 0.040 : 0.012;
            t += dt;
            const double head = 70.0 * std::sin(2.0 * 3.14159265 * 1.4 * t);
            while (!pipe.empty() && pipe.front().first <= t) {
                cam += pipe.front().second;
                pipe.erase(pipe.begin());
            }
            x4vr::head_look_observe(s, (float)cam, 0.0f);
            x4vr::HeadAngles h;
            h.yaw_deg = (float)head; h.pitch_deg = 0.0f;
            h.yaw_weight = 1.0f; h.yaw_reliable = true;
            const x4vr::Delta d = x4vr::head_look_step(s, h, (float)dt);
            pipe.push_back({t + kDeadS, s.sign_yaw * (float)d.dx * 0.115f});
            wc = std::fmax(wc, std::fabs(cam));
            const double want = cam > 85.0 ? 85.0 : (cam < -85.0 ? -85.0 : head);
            err += std::fabs(cam - want); n++;
            (void)want;
        }
        std::printf("      %10.0f %12.2f %12.2f %14s\n", tau_ms, wc, wc - 85.0,
                    wc <= 86.0 ? "clean" : (wc <= 92.0 ? "marginal" : "SPINS"));
    }

    // ---- REMOVING THE FRESHNESS GATE ------------------------------------
    //
    // `if (!fresh) return;` makes a tick happen only when a drain poll finds a
    // NEW pose. Measured: the servo iterates at 41.9 Hz (take 295) and 52.2
    // (294) while the drain runs at 84.2 and 103.7 and the layer publishes at
    // ~89.7. So roughly half the available iteration rate is left on the floor
    // by our own gate, not by anything X4 does.
    //
    // Modelled honestly, which means modelling what the gate actually costs and
    // what removing it actually buys:
    //
    //   * the servo ticks at the DRAIN rate, not the publish rate;
    //   * the head only updates at the PUBLISH rate, so an ungated tick
    //     sometimes sees a repeat — that is not a bug, a proportional servo
    //     simply applies another correction toward the same target;
    //   * the dead time is in WALL CLOCK and barely moves, so doubling the
    //     iteration rate DOUBLES dead_time_ticks — and with a per-tick step
    //     ceiling that doubles the motion in flight. This is the term that
    //     killed X4VR_TICK_HZ, and the model has to be able to show it.
    std::printf("\n      removing `if (!fresh) return;` — saturating regime\n");
    std::printf("      %26s %9s %9s %12s %10s\n", "", "tick ms", "lag ms",
                "worst |cam|", "verdict");
    double gate_cam[4] = {0, 0, 0, 0};
    for (int cfg = 0; cfg < 4; cfg++) {
        const bool ungated = (cfg & 1) != 0;
        const bool ratefix = (cfg & 2) != 0;
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f;
        s.yaw_limit_deg = 85.0f;
        if (ratefix) {
            s.rate_fix_tau_ms = 110.0f;          // the sweep's safe value
            s.rate_fix_max_rate_deg_s = 600.0f;
        }
        // Take 295's measured rates. Ungated ticks at the drain, gated at the
        // fresh rate; the head is sampled-and-held at the publish rate either
        // way, so the ungated arm really does see repeats.
        const double tick = ungated ? 1.0 / 84.2 : 1.0 / 41.9;
        const double pub = 1.0 / 89.7;
        // Removing the gate shortens the dead time by about half a tick — our
        // command reaches X4's queue sooner on average — and by nothing else.
        // Anything larger would be the harness inventing the answer.
        const double kDeadS = 0.060 - 0.5 * (1.0 / 41.9 - tick);
        std::vector<std::pair<double, float>> pipe;
        double t = 0.0, cam = 0.0, wc = 0.0, err_sq = 0.0, held = 0.0,
               next_pub = 0.0;
        int n = 0;
        for (int i = 0; i < 12000; i++) {
            t += tick;
            const double head = 70.0 * std::sin(2.0 * 3.14159265 * 1.4 * t);
            if (t >= next_pub) { held = head; next_pub = t + pub; }
            while (!pipe.empty() && pipe.front().first <= t) {
                cam += pipe.front().second;
                pipe.erase(pipe.begin());
            }
            x4vr::head_look_observe(s, (float)cam, 0.0f);
            x4vr::HeadAngles h;
            h.yaw_deg = (float)held; h.pitch_deg = 0.0f;   // the HELD pose
            h.yaw_weight = 1.0f; h.yaw_reliable = true;
            const x4vr::Delta d = x4vr::head_look_step(s, h, (float)tick);
            pipe.push_back({t + kDeadS, s.sign_yaw * (float)d.dx * 0.115f});
            if (i > 2000) {
                wc = std::fmax(wc, std::fabs(cam));
                const double e = cam - head;
                err_sq += e * e; n++;
            }
        }
        const double kRms = 70.0 * 2.0 * 3.14159265 * 1.4 / 1.41421356;
        char name[64];
        std::snprintf(name, sizeof name, "%s, ratefix %s",
                      ungated ? "UNGATED" : "gated (today)",
                      ratefix ? "tau110" : "off");
        gate_cam[cfg] = wc;
        std::printf("      %26s %9.1f %9.1f %12.2f %10s\n", name,
                    1000.0 * tick, 1000.0 * std::sqrt(err_sq / n) / kRms, wc,
                    wc <= 86.0 ? "clean" : (wc <= 92.0 ? "marginal" : "SPINS"));
    }
    check(gate_cam[1] > gate_cam[0],
          "12: removing the gate with TODAY's per-tick constants makes the "
          "excursion worse — doubling the iteration rate doubles the motion in "
          "flight, which is what X4VR_TICK_HZ already demonstrated in game");
    check(gate_cam[3] <= gate_cam[2] + 0.5,
          "12: ...but with the rate fix on, the step ceiling is per SECOND, so "
          "the same change costs nothing");
    }

    std::printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
