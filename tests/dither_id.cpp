// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// **Task #75: measuring X4's latency band during ORDINARY PLAY.**
//
// Takes 290a/290b measured it open-loop: ~40-165 ms, about 90-100 ms of jitter.
// That needed `plantid`, which takes the servo out of the loop for four minutes
// and cannot run while anyone is playing. And the one session that refutes the
// model -- 285b-s1, calm at 68.6 Hz where s2 went wild at 70.7 with every other
// measured variable identical -- differs, if it differs at all, in exactly this
// band. So it has to be measurable with the servo running.
//
// **Closed-loop data cannot give it, and this project has the scars.** The
// command is computed from the camera position, so correlating one against the
// other measures the controller as much as the plant; `x4vr_plantid.hpp`'s
// header lists the two explanations that died of it in one day.
//
// What makes it valid is an **exogenous** signal: a small dither the servo does
// not know it is sending. It is drawn from an LFSR, so it is white and
// independent of the head, the camera and the command. Cross-correlating it
// against the camera's motion recovers the plant's impulse response *through*
// the closed loop -- the standard closed-loop identification result -- and the
// width of that response is the jitter band.
//
// **STATUS: the DELAY recovers, the BAND WIDTH does not. No take yet.**
//
// The signal is demonstrably there -- with a planted delay D the two correlation
// sequences satisfy b[L] = a[L-D] to four decimals, visible by inspection. What
// is not solved is turning that into a width. Four estimators, four failures,
// each diagnosed and none of them patched over:
//
//   naive corr(dither, camera)     recovers P/(1+PC), not P: the servo sees the
//                                  dither as camera motion it did not ask for and
//                                  corrects it, so its reaction is inside the
//                                  answer. Planted 5.00 came back as 11.35, and a
//                                  band of width ZERO reported a spread of 4.10.
//   deconvolution, instrument =    a differenced white sequence is MA(1), not
//   the injected DELTA             white, so `a` is not causal and forward
//                                  substitution from L=0 is invalid.
//   deconvolution, instrument =    correct in principle. But the injection is
//   the white OFFSET               itself a first difference, so `a` inherits
//                                  that shape and deconvolving a difference is
//                                  an INTEGRATION: the tail drifts, -0.04, -0.05,
//                                  -0.07, -0.08 over successive lags.
//   match filter b against a       fixed delays recover with a constant +1.2 bias
//                                  and a correct ZERO width -- but jittered bands
//                                  collapse, which is the only quantity wanted.
//
// Holding each dither level past the lag range, to separate `a`'s two lobes, made
// it worse rather than better.
//
// So this file asserts what is true and states what is not. **A run will not be
// handed over until the width case passes**, because the rule this project runs
// on is that the acceptance check must be shown to discriminate before the take,
// and here it demonstrably does not.
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

// The dither, exactly as the injector will generate it: a 16-bit LFSR giving a
// 0/1 sequence, scaled to `counts`. The injector sends the DIFFERENCE between
// consecutive values, so the accumulated position offset is bounded by `counts`
// rather than random-walking away.
struct Dither {
    uint16_t lfsr = 0xACE1u;
    int counts;
    int prev = 0;
    explicit Dither(int c) : counts(c) {}
    int next_offset() {
        const uint16_t bit = (uint16_t)(((lfsr >> 0) ^ (lfsr >> 2) ^
                                         (lfsr >> 3) ^ (lfsr >> 5)) & 1u);
        lfsr = (uint16_t)((lfsr >> 1) | (bit << 15));
        return (lfsr & 1u) ? counts : 0;
    }
};

// X4 as takes 288/290 measured it: a faithful integrator whose latency jitters
// over a band, with arrival order preserved.
struct Plant {
    float yaw = 0.0f, pub = 0.0f;
    float limit = 65.0f, gain = 0.115f;
    std::vector<std::pair<int, float>> q;
    int last_due = -1;
    std::mt19937 rng{20260914u};
    int lo, hi;
    Plant(int l, int h) : lo(l), hi(h) {}
    void step(int i, float deg) {
        std::uniform_int_distribution<int> jit(lo, hi);
        int due = i + jit(rng);
        if (due < last_due)
            due = last_due;
        last_due = due;
        q.emplace_back(due, deg);
        float land = 0.0f;
        while (!q.empty() && q.front().first <= i) {
            land += q.front().second;
            q.erase(q.begin());
        }
        yaw = x4vr::clampf(yaw + land, -limit, limit);
        pub = yaw;
    }
};

struct Recovered {
    bool ok = false;
    double mean_lag = 0.0, spread_lag = 0.0;
};

// Match filter: b = a (*) P, so correlating b against a gives P smoothed by a's
// own autocorrelation. The peak LOCATION survives that smoothing; the width does
// not, which is exactly the limitation recorded above.
static Recovered recover(const std::vector<int> &off, const std::vector<double> &u,
                         const std::vector<double> &dc, int ML) {
    Recovered r;
    const size_t n = off.size();
    if (n < 500 || u.size() != n || dc.size() != n)
        return r;
    double m = 0.0;
    for (size_t i = 0; i < n; i++) m += off[i];
    m /= (double)n;
    double var = 0.0;
    for (size_t i = 0; i < n; i++) var += (off[i] - m) * (off[i] - m);
    if (var < 1e-9)
        return r; // no dither: report nothing, invent nothing
    std::vector<double> a((size_t)ML + 1, 0.0), b((size_t)ML + 1, 0.0);
    for (int L = 0; L <= ML; L++) {
        double ua = 0.0, ub = 0.0;
        for (size_t i = (size_t)L; i < n; i++) {
            const double w = off[i - (size_t)L] - m;
            ua += w * u[i];
            ub += w * dc[i];
        }
        a[(size_t)L] = ua;
        b[(size_t)L] = ub;
    }
    std::vector<double> R((size_t)ML + 1, 0.0);
    for (int L = 0; L <= ML; L++) {
        double acc = 0.0;
        for (int k = 0; k + L <= ML; k++) acc += a[(size_t)k] * b[(size_t)(k + L)];
        R[(size_t)L] = acc;
    }
    double tot = 0.0, m1 = 0.0;
    for (int L = 0; L <= ML; L++) if (R[(size_t)L] > 0.0) { tot += R[(size_t)L]; m1 += L * R[(size_t)L]; }
    if (tot <= 0.0) return r;
    r.mean_lag = m1 / tot;
    double m2 = 0.0;
    for (int L = 0; L <= ML; L++) if (R[(size_t)L] > 0.0)
        m2 += R[(size_t)L] * (L - r.mean_lag) * (L - r.mean_lag);
    r.spread_lag = std::sqrt(m2 / tot);
    r.ok = true;
    return r;
}

static void session(int lo, int hi, int counts, double HZ, double secs,
                    std::vector<int> &off, std::vector<double> &u,
                    std::vector<double> &dc) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f;
    s.servo_kp = 0.25f;
    s.lead_scale = 1.0f;
    Plant p(lo, hi);
    Dither d(counts);
    double prev = 0.0;
    const int N = (int)(HZ * secs);
    for (int i = 0; i < N; i++) {
        x4vr::head_look_observe(s, p.pub, 0.0f);
        x4vr::HeadAngles h;
        const double t = i / HZ;
        h.yaw_deg = (float)(20.0 * std::sin(2.0 * M_PI * 0.3 * t));
        h.pitch_deg = 0.0f;
        h.yaw_weight = 1.0f;
        const x4vr::Delta dd = x4vr::head_look_step(s, h);
        // Added AFTER the servo steps, so the estimate never contains it: a
        // disturbance, not a command.
        const int o = d.next_offset();
        const int inj = o - d.prev;
        d.prev = o;
        const double ud = (double)(s.sign_yaw * (float)(dd.dx + inj) * s.gain_deg_per_count);
        p.step(i, (float)ud);
        off.push_back(o);
        u.push_back(ud);
        dc.push_back((double)p.pub - prev);
        prev = (double)p.pub;
    }
}

int main() {
    const double HZ = 70.0;
    std::printf("Closed loop throughout: real head_look_step, head at 0.3 Hz / "
                "20 deg, kp 0.25.\n\n");

    std::printf("1. a FIXED delay — recovered, with a constant bias\n");
    std::printf("%10s %12s %12s %10s\n", "planted", "recovered", "bias", "width");
    double b0 = 0.0, worst_bias_spread = 0.0, worst_width = 0.0;
    bool first = true;
    for (int D : {2, 3, 5, 7}) {
        std::vector<int> off; std::vector<double> u, dc;
        session(D, D, 3, HZ, 300.0, off, u, dc);
        const Recovered r = recover(off, u, dc, 28);
        const double bias = r.mean_lag - D;
        if (first) { b0 = bias; first = false; }
        worst_bias_spread = std::fmax(worst_bias_spread, std::fabs(bias - b0));
        worst_width = std::fmax(worst_width, r.spread_lag);
        std::printf("%10d %12.2f %12.2f %10.2f\n", D, r.mean_lag, bias, r.spread_lag);
    }
    std::printf("   bias varies by %.2f ticks across depths and a zero-width "
                "band reports %.2f —\n   neither is usable.\n",
                worst_bias_spread, worst_width);

    std::printf("\n2. dither OFF — the recovery must report nothing\n");
    {
        std::vector<int> off; std::vector<double> u, dc;
        session(3, 3, 0, HZ, 300.0, off, u, dc);
        const Recovered r = recover(off, u, dc, 28);
        std::printf("   ok=%d\n", r.ok ? 1 : 0);
        check(!r.ok, "with no dither it REFUSES rather than building a band out "
                     "of the loop itself");
    }

    std::printf("\n3. a JITTERED band — NOT RECOVERED, and this is the blocker\n");
    std::printf("%10s %10s %12s %10s %12s\n", "planted", "true mean", "recovered",
                "true sd", "recovered");
    for (auto bd : {std::pair<int,int>{1,5}, {2,6}, {3,9}}) {
        std::vector<int> off; std::vector<double> u, dc;
        session(bd.first, bd.second, 3, HZ, 300.0, off, u, dc);
        const Recovered r = recover(off, u, dc, 28);
        const int k = bd.second - bd.first + 1;
        std::printf("%6d-%-3d %10.2f %12.2f %10.2f %12.2f\n", bd.first, bd.second,
                    0.5 * (bd.first + bd.second), r.mean_lag,
                    std::sqrt((k * k - 1.0) / 12.0), r.spread_lag);
    }
    std::printf("   ^ the means are wrong by several ticks and the widths "
                "collapse.\n     **No take until this table is right.**\n");

    // **One assertion, and it is the only claim this file can make.** Everything
    // above is printed diagnosis, not a check -- asserting on a number I cannot
    // yet recover would be a guard that cannot fail, which this project has paid
    // for more than once. The suite stays green because nothing here pretends to
    // work; the banner at the top says what does not.
    std::printf("\n** NOT A MEASUREMENT YET. ** The exogenous-dither route is "
                "sound in principle —\n   b[L] = a[L-D] is visible to four "
                "decimals — but no estimator built so far\n   turns it into a "
                "band. Sections 1 and 3 are evidence of that, not of a\n   "
                "working instrument. Do not spend a take on this.\n");
    std::printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
