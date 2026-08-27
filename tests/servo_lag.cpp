// Task #73 -- the head-look lag, and the arithmetic that removes it.
//
// **The lag was measured before any of this was written**, from logs already on
// disk. Takes 209/210/211 print X4's own camera readback beside the head angle
// (`camread:`), so the lag is the slope of tracking error against head rate:
//
//   take 209   85 Hz pose rate   yaw 43.8 ms   pitch 38.7 ms   (~240 fps)
//   take 210   85 Hz pose rate   yaw 41.4 ms   pitch 42.1 ms   (~120 fps)
//   take 211   26.6 Hz           yaw 160.4 ms  pitch 203.8 ms  (~20 fps)
//
// tau SCALES with the pose period -- 3.3 to 5.4 times it -- which is what rules
// out every fixed-latency story: tracker latency, our render cost, and X4's own
// `anglespeed`/`angleacc` would each hold tau constant in milliseconds.
// (1 - kp)/kp is 3.0 at kp = 0.25, and that is the whole explanation: a
// proportional servo tracking a moving target sits behind it forever.
//
// This file drives the REAL head_look_step against a model of X4's camera and
// checks four things:
//   1. it reproduces the measured lag, so the model is not a story;
//   2. it reproduces take 136 -- kp 1.0 spinning uncontrollably -- which is the
//      only independent evidence the plant model is shaped right;
//   3. the lead removes the lag at BOTH frame rates without retuning;
//   4. every way the lead can go wrong is bounded: the yaw wrap, the pole, a
//      corrupt pose, and a mid-run toggle.
#include "../common/x4vr_headlook.hpp"

#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

static int failures = 0;

static void check(bool ok, const char *what, const char *detail = "") {
    if (!ok) {
        failures++;
        std::printf("  FAIL %s %s\n", what, detail);
    } else {
        std::printf("  ok   %s %s\n", what, detail);
    }
}

// X4's camera as seen through the mouse channel: deltas accumulate, after some
// number of frames of transport delay. The delay is UNKNOWN in the real game,
// which is why every claim below is checked across a range of it.
struct Plant {
    double angle = 0.0;
    std::deque<double> pipe;
    explicit Plant(int delay) : pipe((size_t)delay, 0.0) {}
    void step(double d) {
        pipe.push_back(d);
        angle += pipe.front();
        pipe.pop_front();
    }
};

struct Cfg {
    float kp = 0.25f;
    int delay = 1;
    float lead = 0.0f;   // lead_scale
    double noise = 0.0;  // tracker noise, degrees RMS
};

struct Out {
    double lag_T = 0;    // steady-state lag, in units of the step period
    double jitter = 0;   // camera rate RMS -- what a player actually sees
    double max_step = 0; // largest single camera move
};

template <class F>
static Out drive(const Cfg &c, double T, F traj, int steps, double rate_ref) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f; // take 211's calibrated gain
    s.servo_kp = c.kp;
    s.lead_scale = c.lead;
    s.yaw_limit_deg = 1e6f; // no clamp: isolate the servo from #68
    s.pitch_limit_deg = 1e6f;
    Plant p(c.delay);
    std::mt19937 rng(20250827u);
    std::normal_distribution<double> nd(0.0, c.noise);

    std::vector<double> errs, rates;
    double last = 0.0;
    Out o;
    for (int i = 0; i < steps; i++) {
        const double head = traj(i);
        const double meas = head + (c.noise > 0 ? nd(rng) : 0.0);
        // **Error recorded BEFORE the step, because that is what the in-run
        // instrument records.** X4's readback at the moment of a sample is the
        // camera as it stood before this tick's command, so `lagsample` logs
        // the pre-step error and score_run.py regresses on it.
        //
        // The distinction is not cosmetic -- it is a whole frame of correction.
        // Pre-step, the steady ramp error is rate*T/kp = 4T; post-step it is
        // (1-kp) times that, 3T. Measuring one here and the other in the run
        // would have set the acceptance threshold for a quantity the run never
        // reports. Caught by the two disagreeing on identical data: 0.06x here
        // against 0.27x from the scorer.
        const double err = head - p.angle;
        x4vr::head_look_observe(s, (float)p.angle, 0.0f);
        x4vr::HeadAngles ha;
        ha.yaw_deg = (float)meas;
        ha.pitch_deg = 0.0f;
        ha.yaw_weight = 1.0f;
        const x4vr::Delta d = x4vr::head_look_step(s, ha);
        p.step(s.sign_yaw * (double)d.dx * s.gain_deg_per_count);
        const double moved = std::fabs(p.angle - last);
        if (i > steps / 2) {
            errs.push_back(err);
            rates.push_back(moved / T);
            o.max_step = std::max(o.max_step, moved);
        }
        last = p.angle;
    }
    double m = 0;
    for (double e : errs)
        m += e;
    m /= (double)errs.size();
    o.lag_T = rate_ref != 0 ? m / (rate_ref * T) : 0.0;
    double cm = 0;
    for (double r : rates)
        cm += r;
    cm /= (double)rates.size();
    double cv = 0;
    for (double r : rates)
        cv += (r - cm) * (r - cm);
    o.jitter = std::sqrt(cv / (double)rates.size());
    return o;
}

// Overshoot on a step input. This is where raising kp goes wrong, and a mean
// lag cannot see it.
static double overshoot_pct(const Cfg &c, double T, double amp) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f;
    s.servo_kp = c.kp;
    s.lead_scale = c.lead;
    s.yaw_limit_deg = 1e6f;
    s.pitch_limit_deg = 1e6f;
    Plant p(c.delay);
    double peak = 0.0;
    for (int i = 0; i < 400; i++) {
        x4vr::head_look_observe(s, (float)p.angle, 0.0f);
        x4vr::HeadAngles ha;
        ha.yaw_deg = (float)(i < 5 ? 0.0 : amp);
        ha.yaw_weight = 1.0f;
        const x4vr::Delta d = x4vr::head_look_step(s, ha);
        p.step(s.sign_yaw * (double)d.dx * s.gain_deg_per_count);
        if (i >= 5)
            peak = std::max(peak, p.angle);
    }
    (void)T;
    return (peak - amp) / amp * 100.0;
}

int main() {
    const double T85 = 1.0 / 85.0, T27 = 1.0 / 26.6;
    auto ramp = [](double rate, double T) {
        return [rate, T](int i) { return rate * T * (double)i; };
    };
    char buf[256];

    std::printf("1. the model reproduces the measured lag\n");
    {
        const Out a = drive({0.25f, 1, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        const Out b = drive({0.25f, 1, 0.0f, 0.0}, T27, ramp(60.0, T27), 4000, 60.0);
        const double ms85 = a.lag_T * T85 * 1000.0, ms27 = b.lag_T * T27 * 1000.0;
        std::snprintf(buf, sizeof buf,
                      "85 Hz %.1f ms (4T = %.1f, logs 38.7-43.8), 26.6 Hz "
                      "%.0f ms (4T = %.0f, logs 160-204)",
                      ms85, 4000.0 * T85, ms27, 4000.0 * T27);
        // **4T, derived, not fitted.** Steady state on a ramp gives
        // e = e(1-kp) + rate*T, so e = rate*T/kp = 4T*rate at kp = 0.25.
        //
        // The stored logs read 38.7-43.8 ms against the 47 this predicts, and
        // the gap is expected in that direction: their head rate came from
        // samples 350 ms apart, and noise in a regressor ATTENUATES the slope.
        // So the logs are a lower bound on the true lag, not a disagreement.
        check(b.lag_T > 3.6 && b.lag_T < 4.4, "lag is 4 T at both rates", buf);
        check(ms85 > 40.0 && ms85 < 55.0, "85 Hz lag is 4T", buf);
    }

    std::printf("2. the harness reproduces take 136 (kp 1.0 was unusable)\n");
    {
        const Out k1 = drive({1.0f, 1, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        const Out k25 = drive({0.25f, 1, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        std::snprintf(buf, sizeof buf, "kp 1.0 jitter %.1f deg/s vs kp 0.25 %.1f",
                      k1.jitter, k25.jitter);
        check(k1.jitter > 5.0 * k25.jitter, "kp 1.0 makes the camera thrash", buf);
        const double os = overshoot_pct({1.0f, 1, 0.0f, 0.0}, T85, 20.0);
        std::snprintf(buf, sizeof buf, "%.0f%% overshoot on a 20 deg step", os);
        check(os > 40.0, "kp 1.0 overshoots badly", buf);
    }

    std::printf("3. raising kp is the WRONG fix -- priced, not asserted\n");
    for (float kp : {0.35f, 0.5f, 0.7f}) {
        const double os = overshoot_pct({kp, 1, 0.0f, 0.0}, T85, 20.0);
        const Out r = drive({kp, 1, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        std::printf("       kp %.2f: lag %5.1f ms, overshoot %+6.1f%%\n", kp,
                    r.lag_T * T85 * 1000.0, os);
    }
    {
        // The reason the gain cannot be trusted: the answer depends on a plant
        // delay nobody has measured.
        const double d1 = overshoot_pct({0.5f, 1, 0.0f, 0.0}, T85, 20.0);
        const double d3 = overshoot_pct({0.5f, 3, 0.0f, 0.0}, T85, 20.0);
        std::snprintf(buf, sizeof buf, "kp 0.5 overshoots %.0f%% at delay 1 but "
                                       "%.0f%% at delay 3", d1, d3);
        check(d3 > 2.0 * d1, "kp's safety depends on the UNKNOWN plant delay", buf);
    }

    std::printf("4. the lead takes the lag from 4 T to 1 T, at both rates\n");
    // Derived the same way: with the lead, e = 0.75 e + 0.25 rate T, so the
    // steady pre-step error is rate*T -- ONE frame time, whatever the frame
    // rate, and a 4x reduction rather than the cancellation an earlier draft of
    // this file claimed. That draft measured post-step and flattered itself.
    for (auto pr : {std::pair<double, double>{85.0, T85},
                    std::pair<double, double>{26.6, T27}})
        for (double rate : {60.0, 240.0}) {
            const Out r =
                drive({0.25f, 1, 1.0f, 0.0}, pr.second, ramp(rate, pr.second),
                      4000, rate);
            const double ms = r.lag_T * pr.second * 1000.0;
            std::snprintf(buf, sizeof buf,
                          "%.1f Hz at %.0f deg/s: %.2f ms = %.2f T (1 T = "
                          "%.1f ms)", pr.first, rate, ms, r.lag_T,
                          pr.second * 1000.0);
            check(r.lag_T > 0.8 && r.lag_T < 1.25, "lead leaves exactly 1 T", buf);
        }

    std::printf("4b. a REAL head accelerates, and that caps what a velocity "
                "lead can do\n");
    {
        // **The ramp above is the lead's best case and must not be the only
        // case.** A constant rate is exactly what a velocity feed-forward
        // cancels; a head that turns and stops has acceleration the lead does
        // not model, and the residual is second order. Measuring only the ramp
        // is how an instrument comes to pass in its simplest configuration and
        // nowhere else -- twice already in this project.
        //
        // These frequencies are what a head does: 0.35 Hz is a slow sweep
        // across the cockpit, 1.3 Hz a quick glance.
        for (auto pr : {std::pair<double, double>{85.0, T85},
                        std::pair<double, double>{26.6, T27}}) {
            const double T = pr.second;
            auto sway = [T](int i) {
                const double t = (double)i * T;
                return 60.0 * std::sin(0.35 * t) + 25.0 * std::sin(1.30 * t);
            };
            // Lag against a non-constant rate is not a single slope, so compare
            // the RMS tracking error with and without the lead -- the quantity
            // a player experiences either way.
            double e_idle = 0.0, e_armed = 0.0;
            for (int arm = 0; arm < 2; arm++) {
                x4vr::HeadLook s;
                s.gain_deg_per_count = 0.115f;
                s.lead_scale = arm ? 1.0f : 0.0f;
                s.yaw_limit_deg = 1e6f;
                s.pitch_limit_deg = 1e6f;
                Plant p(1);
                double acc = 0.0;
                int n = 0;
                const int steps = (int)(120.0 / T);
                for (int i = 0; i < steps; i++) {
                    const double pre = sway(i) - p.angle;
                    x4vr::head_look_observe(s, (float)p.angle, 0.0f);
                    x4vr::HeadAngles ha;
                    ha.yaw_deg = (float)sway(i);
                    ha.yaw_weight = 1.0f;
                    const x4vr::Delta d = x4vr::head_look_step(s, ha);
                    p.step(s.sign_yaw * (double)d.dx * s.gain_deg_per_count);
                    if (i > steps / 4) {
                        acc += pre * pre; // pre-step, as the instrument reports
                        n++;
                    }
                }
                (arm ? e_armed : e_idle) = std::sqrt(acc / (double)n);
            }
            std::snprintf(buf, sizeof buf,
                          "%.1f Hz: RMS error %.2f -> %.2f deg (%.2fx)",
                          pr.first, e_idle, e_armed, e_armed / e_idle);
            // **This is where take 212's acceptance threshold comes from.**
            // The ramp predicts 1T/4T = 0.25x exactly; a head that accelerates
            // leaves a second-order residual the velocity lead cannot model, so
            // the achievable ratio is a little worse. score_run.py fails above
            // 0.45x -- clear of what is measured here, and still less than half
            // the 1.00x a lead that never reached the servo would produce.
            check(e_armed < 0.40 * e_idle, "lead removes most of the error", buf);
        }
    }

    std::printf("5. the lead is robust to the plant delay it cannot know\n");
    // The lead is computed from the head alone, so it cannot know the plant --
    // and does not need to. What must hold is that the residual stays at ~1 T
    // however many frames X4 takes to answer, since that number is unmeasured
    // and a fix that depended on it would be a fix that can fail in the field.
    for (int d = 0; d <= 3; d++) {
        const Out r = drive({0.25f, d, 1.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        const double ms = r.lag_T * T85 * 1000.0;
        const Out u = drive({0.25f, d, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        std::snprintf(buf, sizeof buf, "delay %d: %.2f ms = %.2f T, against "
                                       "%.2f T unled", d, ms, r.lag_T, u.lag_T);
        check(r.lag_T > 0.8 && r.lag_T < 1.3, "lead holds at 1 T", buf);
    }

    std::printf("6. lead_scale 0 is EXACTLY the old behaviour\n");
    {
        const Out a = drive({0.25f, 1, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        const Out b = drive({0.25f, 1, 0.0f, 0.0}, T85, ramp(60.0, T85), 4000, 60.0);
        check(a.lag_T == b.lag_T, "control arm is deterministic", "");
        // The A/B's control arm must be the shipped servo, bit for bit, or the
        // comparison measures two changes at once.
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        x4vr::HeadAngles ha;
        ha.yaw_deg = 30.0f;
        ha.yaw_weight = 1.0f;
        const x4vr::Delta d = x4vr::head_look_step(s, ha);
        std::snprintf(buf, sizeof buf, "dx %d (0.25*30/0.115 = %.0f)", d.dx,
                      0.25 * 30.0 / 0.115);
        check(d.dx == -65, "unled first step is unchanged", buf);
    }

    std::printf("7. the lead's failure modes are bounded\n");
    {
        // (a) The +-180 crossing. A raw difference would read -358 deg of
        // "motion" and command a full reverse spin -- the same defect the error
        // term already carries a wrap for, and the reason this is checked here.
        x4vr::HeadLook s;
        s.gain_deg_per_count = 0.115f;
        s.lead_scale = 1.0f;
        s.yaw_limit_deg = 180.0f;
        x4vr::HeadAngles ha;
        ha.yaw_weight = 1.0f;
        ha.yaw_deg = 179.0f;
        x4vr::head_look_observe(s, 179.0f, 0.0f);
        x4vr::head_look_step(s, ha);
        ha.yaw_deg = -179.0f; // 2 deg further, across the wrap
        x4vr::head_look_observe(s, 179.0f, 0.0f);
        const x4vr::Delta d = x4vr::head_look_step(s, ha);
        // The head moved +2 deg (179 -> -179 is a crossing, not a reversal), so
        // the lead is +6 and the target is -173. From the camera at 179 that is
        // +8 the short way: step 2 deg, dx -17.
        //
        // **Asserted tightly on purpose.** Unwrapped, the lead would be -1074
        // clipped to -45, the target would hit the -180 stop, and the error
        // would come out as +1 -- dx -2. Both are negative, so a sign test
        // passes either way and proves nothing. The value is the only thing
        // that separates them.
        std::snprintf(buf, sizeof buf,
                      "dx %d across the +-180 crossing (unwrapped would be -2)",
                      d.dx);
        check(d.dx <= -12 && d.dx >= -22, "wrapped lead steps the short way", buf);

        // (b) The lead's own cap holds. max_step_deg is lifted here so the step
        // clamp cannot mask the thing under test: with both at their shipped
        // values the step saturates first and this check passes without ever
        // observing the lead -- which is how it was written the first time.
        x4vr::HeadLook s2;
        s2.gain_deg_per_count = 0.115f;
        s2.lead_scale = 1.0f;
        s2.yaw_limit_deg = 1e6f;
        s2.max_step_deg = 1e6f;
        x4vr::HeadAngles h2;
        h2.yaw_weight = 1.0f;
        h2.yaw_deg = 0.0f;
        x4vr::head_look_step(s2, h2);
        h2.yaw_deg = 90.0f; // a 90 deg jump in one pose: tracker glitch
        const x4vr::Delta dg = x4vr::head_look_step(s2, h2);
        // Same glitch with the lead off: the difference is the lead alone.
        x4vr::HeadLook s2n = s2;
        s2n.lead_scale = 0.0f;
        s2n.cmd_yaw_deg = 0.0f;
        s2n.last_head_yaw_deg = 0.0f;
        const x4vr::Delta dn = x4vr::head_look_step(s2n, h2);
        const double led = ((double)dn.dx - (double)dg.dx) * 0.115 / 0.25;
        std::snprintf(buf, sizeof buf, "lead contributed %.1f deg, cap %.1f "
                                       "(unled dx %d, led dx %d)",
                      led, s2.lead_max_deg, dn.dx, dg.dx);
        check(led > 0.9 * s2.lead_max_deg && led <= s2.lead_max_deg + 0.5,
              "glitch lead is capped, and the cap is what bound it", buf);

        // And the shipped configuration bounds the COMMAND regardless, which is
        // the claim that actually matters: max_step_deg clamps after the error
        // is formed, so no lead can drive more than one step.
        x4vr::HeadLook s2s;
        s2s.gain_deg_per_count = 0.115f;
        s2s.lead_scale = 1.0f;
        s2s.yaw_limit_deg = 1e6f;
        x4vr::HeadAngles h2s;
        h2s.yaw_weight = 1.0f;
        h2s.yaw_deg = 0.0f;
        x4vr::head_look_step(s2s, h2s);
        h2s.yaw_deg = 90.0f;
        const x4vr::Delta ds = x4vr::head_look_step(s2s, h2s);
        const double cmd_deg = std::fabs((double)ds.dx * 0.115);
        std::snprintf(buf, sizeof buf, "commanded %.1f deg, max_step %.1f",
                      cmd_deg, s2s.max_step_deg);
        check(cmd_deg <= s2s.max_step_deg + 0.2,
              "a glitch still cannot command past max_step_deg", buf);

        // (c) Near vertical the yaw derivative is noise squared. The weight has
        // to reach the lead, not just the step.
        x4vr::HeadLook s3;
        s3.gain_deg_per_count = 0.115f;
        s3.lead_scale = 1.0f;
        s3.yaw_limit_deg = 1e6f;
        x4vr::HeadAngles h3;
        h3.pitch_deg = 88.0f;
        h3.yaw_weight = x4vr::yaw_weight_of_pitch_deg(88.0f);
        h3.yaw_deg = 0.0f;
        x4vr::head_look_step(s3, h3);
        h3.yaw_deg = 40.0f; // the pole swinging yaw around
        const x4vr::Delta dp = x4vr::head_look_step(s3, h3);
        std::snprintf(buf, sizeof buf, "weight %.4f at 88 deg -> dx %d",
                      h3.yaw_weight, dp.dx);
        check(dp.dx == 0, "the pole fades the lead too", buf);

        // (d) A mid-run toggle must not difference against a stale head. This
        // is what the A/B in take 212 does every 20 s.
        x4vr::HeadLook s4;
        s4.gain_deg_per_count = 0.115f;
        s4.yaw_limit_deg = 1e6f;
        x4vr::HeadAngles h4;
        h4.yaw_weight = 1.0f;
        s4.lead_scale = 0.0f;
        for (int i = 0; i < 50; i++) { // 50 deg travelled with the lead OFF
            h4.yaw_deg = (float)i;
            x4vr::head_look_observe(s4, h4.yaw_deg, 0.0f);
            x4vr::head_look_step(s4, h4);
        }
        s4.lead_scale = 1.0f; // armed mid-run
        h4.yaw_deg = 50.0f;
        x4vr::head_look_observe(s4, 49.0f, 0.0f);
        const x4vr::Delta dt = x4vr::head_look_step(s4, h4);
        std::snprintf(buf, sizeof buf, "dx %d on the first armed pose", dt.dx);
        check(std::abs(dt.dx) < 60, "toggling on does not jolt", buf);
    }

    std::printf("8. THE PRICE: the lead amplifies tracker noise\n");
    {
        auto still = [](int) { return 0.0; };
        const Out a = drive({0.25f, 1, 0.0f, 0.05}, T85, still, 4000, 0.0);
        const Out b = drive({0.25f, 1, 1.0f, 0.05}, T85, still, 4000, 0.0);
        std::printf("       head STILL, 0.05 deg noise: unled %.2f deg/s, "
                    "led %.2f deg/s\n", a.jitter, b.jitter);
        // Recorded as a bound, not a pass/fail: the real tracker's noise is not
        // 0.05 deg by assumption, and take 212's `lagfit still` measures it.
        // What must hold is that it stays inside the range the run can judge.
        std::snprintf(buf, sizeof buf, "led jitter %.2f deg/s at 0.05 deg noise",
                      b.jitter);
        check(b.jitter < 20.0, "noise gain is bounded, and measured in-run", buf);
    }

    std::printf(failures ? "\nFAILED (%d)\n" : "\nAll servo-lag checks passed\n",
                failures);
    return failures ? 1 : 0;
}
