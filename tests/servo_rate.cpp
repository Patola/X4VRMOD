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

    // Returns true on the ticks where the readback refreshes.
    bool step(float sign_yaw, int dx, float sign_pitch, int dy) {
        yaw = x4vr::clampf(yaw + sign_yaw * (float)dx * gain, -yaw_limit,
                           yaw_limit);
        pitch = x4vr::clampf(pitch + sign_pitch * (float)dy * gain,
                             -pitch_limit, pitch_limit);
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
                    int steps) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f; // what the SERVO believes
    s.servo_kp = kp;
    s.lead_scale = lead;
    Plant p;
    p.period = period;
    p.gain = plant_gain;            // what X4 actually does

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

    std::printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
