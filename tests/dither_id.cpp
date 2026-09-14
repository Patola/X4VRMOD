// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// **Task #75: measuring X4's latency band during ORDINARY PLAY.**
//
// Takes 290a/290b measured it open-loop at ~40-165 ms, about 90-100 ms of
// jitter. That needed `plantid`, which takes the servo out of the loop for four
// minutes and cannot run while anyone is playing. And the session that refutes
// the model -- 285b-s1, calm at 68.6 Hz where s2 went wild at 70.7 with every
// other measured variable identical -- differs, if it differs at all, in exactly
// this band.
//
// **Closed-loop data cannot give it by itself.** The command is computed from
// the camera, so correlating one against the other measures the controller as
// much as the plant. What makes identification valid is an **exogenous** signal:
// a dither the servo does not know it is sending.
//
// **The relation being inverted, written down because getting it wrong cost four
// estimators.** X4 is an integrator with delayed delivery: a command DELTA u[j]
// issued at tick j arrives at tick j + L_j, so
//
//     delta cam[i] = sum_k P[k] * u[i-k]
//
// The convolution is against the **delta** sequence, not against position. The
// instrument therefore has to be a white **delta**. Four earlier attempts
// injected the DIFFERENCE of a white position sequence, to keep the accumulated
// offset bounded; that difference propagated into the input correlation `a`, and
// deconvolving a difference is an integration -- the recovered tail drifted
// -0.04, -0.05, -0.07, -0.08 over successive lags and jittered bands collapsed.
//
// **A white delta does not need differencing to stay bounded, because the servo
// is a regulator.** It observes the camera and drives it back to the head, so the
// dither's accumulated offset is suppressed rather than free-running -- the one
// thing that made a white delta look impossible was wrong. The cap below makes
// that guarantee explicit instead of relying on the loop: a draw that would take
// the accumulated offset outside the band is reflected. That is what makes this a
// BOUNDED PRBS in position while leaving the delta white almost everywhere, and
// how often the reflection fires is measured rather than assumed rare.
//
// **RANGE, and it is a real limit rather than a caveat.** The deconvolution needs
// the camera to be inside its clamp: past a mean delay of ~6 ticks this servo
// slams between X4's stops (tests/servo_rate.cpp), and a clamp is the one
// nonlinearity no deconvolution sees through. Validated to a mean of 4 ticks,
// degrading beyond -- a planted 2-8 band reads 7.20 where 5.00 is true, and the
// recovered MASS goes to 1.09 where every in-range case sits at 1.00-1.02. That
// mass is the instrument's own statement that it is out of range, and a run whose
// mass drifts must not be read.
//
// What that means for #75: the band measured open-loop in takes 290a/290b is
// ~40-165 ms. At the drain rate near 28 Hz that is 1.1-4.6 ticks and IN range; at
// 70 Hz it is 2.7-11.3 ticks and is NOT. So this measures a calm drain-rate
// session; it cannot yet read one at 90 Hz, which is exactly where the loop is
// already unstable for reasons that have nothing to do with the instrument.
#include "../common/x4vr_headlook.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <random>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) { g_fail++; std::printf("FAIL  %s\n", what); }
    else     { std::printf("ok    %s\n", what); }
}

struct Dither {
    uint16_t lfsr = 0xACE1u;
    int counts, cap, acc = 0, worst = 0;
    long draws = 0, reflected = 0;
    Dither(int c, int k) : counts(c), cap(k) {}
    int next_delta() {
        const uint16_t bit = (uint16_t)(((lfsr >> 0) ^ (lfsr >> 2) ^
                                         (lfsr >> 3) ^ (lfsr >> 5)) & 1u);
        lfsr = (uint16_t)((lfsr >> 1) | (bit << 15));
        int v = (lfsr & 1u) ? counts : -counts;
        draws++;
        if (acc + v > cap || acc + v < -cap) { v = -v; reflected++; }
        acc += v;
        if (std::abs(acc) > worst) worst = std::abs(acc);
        return v;
    }
};

// X4 as takes 288/290 measured it: a faithful integrator whose latency jitters
// over a band, with arrival order preserved.
struct Plant {
    float yaw = 0.0f, pub = 0.0f, limit = 65.0f;
    std::vector<std::pair<int, float>> q;
    int last_due = -1;
    std::mt19937 rng{20260914u};
    std::uniform_int_distribution<int> jit;
    Plant(int lo, int hi) : jit(lo, hi) {}
    void step(int i, float deg) {
        int due = i + jit(rng);
        if (due < last_due) due = last_due;
        last_due = due;
        q.emplace_back(due, deg);
        float land = 0.0f;
        while (!q.empty() && q.front().first <= i) {
            land += q.front().second; q.erase(q.begin());
        }
        yaw = x4vr::clampf(yaw + land, -limit, limit);
        pub = yaw;
    }
};

struct Recovered {
    bool ok = false;
    double mean_lag = 0.0, spread_lag = 0.0, total = 0.0;
};

// b = a (*) P with `a` causal and a[0] dominant, so P by forward substitution.
//
// `a` is the dither against the plant's TOTAL input and `b` against its output,
// so the servo's reaction to the dither appears on both sides and divides out.
// Correlating against the output alone recovers P/(1+PC) instead, which is how
// the first attempt turned a planted 5.00 into 11.35.
static Recovered recover(const std::vector<int> &v, const std::vector<double> &u,
                         const std::vector<double> &dc, int ML) {
    Recovered r;
    const size_t n = v.size();
    if (n < 500 || u.size() != n || dc.size() != n) return r;
    double var = 0.0;
    for (size_t i = 0; i < n; i++) var += (double)v[i] * (double)v[i];
    if (var < 1e-9) return r; // no dither: report nothing, invent nothing
    std::vector<double> a((size_t)ML + 1, 0.0), b((size_t)ML + 1, 0.0), P((size_t)ML + 1, 0.0);
    for (int L = 0; L <= ML; L++) {
        double ua = 0.0, ub = 0.0;
        for (size_t i = (size_t)L; i < n; i++) {
            const double w = (double)v[i - (size_t)L];
            ua += w * u[i]; ub += w * dc[i];
        }
        a[(size_t)L] = ua / var; b[(size_t)L] = ub / var;
    }
    if (std::fabs(a[0]) < 1e-9) return r;
    for (int L = 0; L <= ML; L++) {
        double acc = b[(size_t)L];
        for (int k = 0; k < L; k++) acc -= a[(size_t)(L - k)] * P[(size_t)k];
        P[(size_t)L] = acc / a[0];
    }
    // **Taps below a fraction of the peak are noise, and summing them was a
    // systematic bias, not a wobble.** Every positive tap counted meant a thin
    // tail of near-zero values at large lags, each contributing its own large L
    // to the first moment and its large (L-mean)^2 to the second: planted bands
    // came back +1.5 ticks in the mean and +2.3 in the spread, every one of them
    // in the same direction. A real impulse response is concentrated; 5% of the
    // peak is the floor below which a tap says nothing.
    double peak = 0.0;
    for (int L = 0; L <= ML; L++) peak = std::fmax(peak, P[(size_t)L]);
    const double floor_ = 0.05 * peak;
    double tot = 0.0, m1 = 0.0;
    for (int L = 0; L <= ML; L++) if (P[(size_t)L] > floor_) { tot += P[(size_t)L]; m1 += L * P[(size_t)L]; }
    if (tot < 1e-6) return r;
    r.mean_lag = m1 / tot;
    double m2 = 0.0;
    for (int L = 0; L <= ML; L++) if (P[(size_t)L] > floor_)
        m2 += P[(size_t)L] * (L - r.mean_lag) * (L - r.mean_lag);
    r.spread_lag = std::sqrt(m2 / tot);
    r.total = tot; r.ok = true;
    return r;
}

struct Stats { double rms_extra = 0.0; int worst_acc = 0; double reflect_pct = 0.0; };

static Stats session(int lo, int hi, int counts, int cap, double HZ, double secs,
                     std::vector<int> &v, std::vector<double> &u,
                     std::vector<double> &dc) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f; s.lead_scale = 1.0f;
    Plant p(lo, hi);
    Dither d(counts, cap);
    double prev = 0.0, extra2 = 0.0;
    const int N = (int)(HZ * secs);
    for (int i = 0; i < N; i++) {
        x4vr::head_look_observe(s, p.pub, 0.0f);
        x4vr::HeadAngles h;
        const double t = i / HZ;
        h.yaw_deg = (float)(20.0 * std::sin(2.0 * M_PI * 0.3 * t));
        h.pitch_deg = 0.0f; h.yaw_weight = 1.0f;
        const x4vr::Delta dd = x4vr::head_look_step(s, h);
        // **Added AFTER the servo has stepped**, so the estimate never contains
        // it: a disturbance, not a command. The servo sees it only as camera
        // motion it did not ask for, and regulates it away like any other.
        const int inj = counts ? d.next_delta() : 0;
        const double ud = (double)(s.sign_yaw * (float)(dd.dx + inj) * s.gain_deg_per_count);
        p.step(i, (float)ud);
        v.push_back(inj); u.push_back(ud);
        dc.push_back((double)p.pub - prev); prev = (double)p.pub;
        const double err = (double)p.pub - (double)h.yaw_deg;
        extra2 += err * err;
    }
    Stats st;
    st.rms_extra = std::sqrt(extra2 / (double)N);
    st.worst_acc = d.worst;
    st.reflect_pct = d.draws ? 100.0 * (double)d.reflected / (double)d.draws : 0.0;
    return st;
}

// Emit the injector's exact log format, so score_run.py's recovery -- a SECOND
// implementation, in Python -- can be driven against a planted band. The C++
// recovery above being right says nothing about the Python one, and the Python
// one is what reads the take.
// **`play` is take 291's regime, and its absence is why this file certified a
// statistic the take could not use.** Every case here drove a 20 deg 0.3 Hz sine
// at a metronome-uniform 70 Hz -- a servo so quiet the recovery returned a
// planted spread exactly. Take 291 was ordinary play: the player's own steering
// carried 51x the instrument's variance and the tick wandered from 9 to 51 ms.
// Under THOSE conditions the tap spread stops tracking the plant's width, and
// nothing here said so. Measured on take 291 and reproduced by these numbers:
//
//              |dx| median   |dx| p99   var(servo)   tick p05..p95
//   take 291         3          71         205        9 .. 51 ms
//
// Run it in the configuration the real measurement uses. That rule has now cost
// this project an instrument three times.
struct Regime {
    bool play = false;          // wandering tick + steering that reaches the stop
    double hz = 70.0;
};

static void emit(int lo, int hi, int counts, Regime rg, double secs,
                 const char *path) {
    x4vr::HeadLook s;
    s.gain_deg_per_count = 0.115f; s.servo_kp = 0.25f; s.lead_scale = 1.0f;
    Plant p(lo, hi);
    Dither d(counts, 100000);
    // Separate stream from the plant's jitter, so the tick and the latency are
    // independent -- moving two things at once is how the last control confirmed
    // a hypothesis it should have refuted.
    std::mt19937 trng{777u};
    std::uniform_real_distribution<double> tj(9.0, 51.0);
    FILE *f = std::fopen(path, "w");
    if (!f) { std::printf("  FAIL cannot write %s\n", path); g_fail++; return; }
    double t = 100.0;
    std::fprintf(f, "[ %11.3f] inject  env: run = X4VR_TAKE=SYNTH "
                    "X4VR_HEADLOOK_GAIN=0.115 X4VR_DITHER=%d ./launch/"
                    "x4vr-launch.sh\n", t, counts);
    std::fprintf(f, "[ %11.3f] layer   mv: X4 uses vkCreateRenderPass (v1)\n", t);
    const int N = (int)(rg.hz * secs);
    double tt = 0.0;
    for (int i = 0; i < N; i++) {
        const float cam_before = p.pub;   // read at the TOP of the tick
        x4vr::head_look_observe(s, p.pub, 0.0f);
        x4vr::HeadAngles h;
        // The quiet case is unchanged, so the four original plants still read
        // exactly what they read before.
        // Play is not a sine. Take 291's servo sat at |dx| median 3 and p99 71 --
        // nearly still, with rare fast sweeps. A continuous sine big enough to
        // reach that p99 sits at the saturation stop the whole time (measured:
        // var 3249 against the real 205). So: slow drift, plus a look-away-and-
        // back burst that returns to where it started.
        double burst = 0.0;
        if (rg.play) {
            const double ph = std::fmod(tt, 4.0);
            if (ph < 0.26) burst = 26.0 * std::sin(M_PI * ph / 0.26);
        }
        h.yaw_deg = rg.play
            ? (float)(20.0 * std::sin(2.0 * M_PI * 0.07 * tt) +
                       8.0 * std::sin(2.0 * M_PI * 0.28 * tt + 1.1) + burst)
            : (float)(20.0 * std::sin(2.0 * M_PI * 0.3 * tt));
        h.pitch_deg = 0.0f; h.yaw_weight = 1.0f;
        const x4vr::Delta dd = x4vr::head_look_step(s, h);
        const int dv = d.next_delta();
        const int dx = dd.dx + dv;
        const double dt = rg.play ? tj(trng) / 1000.0 : 1.0 / rg.hz;
        t += dt; tt += dt;
        std::fprintf(f, "[ %11.3f] inject  dither: v=%+d dx=%+d cam=%.4f\n",
                     t, dv, dx, (double)cam_before);
        p.step(i, s.sign_yaw * (float)dx * s.gain_deg_per_count);
    }
    std::fclose(f);
}

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--emit") {
        const std::string dir = argv[2];
        // The last two are the REFUSAL cases, and they are what this file was
        // missing. A wandering tick plus steering hard enough to reach X4's 65
        // deg stop (measured: 0.1% and 0.4% of samples sitting on it) puts the
        // loop somewhere the deconvolution has no business reporting from -- the
        // plant is not linear at a clamp. The instrument must say so instead of
        // returning a number, which is exactly what it failed to do on take 291.
        struct { int lo, hi; bool play; const char *name; } cases[6] = {
            {2, 2, false, "fixed2"},    {3, 3, false, "fixed3"},
            {2, 6, false, "band2-6"},   {1, 7, false, "band1-7"},
            {4, 4, true,  "slam-fixed"},{1, 7, true,  "slam-band"}};
        for (auto &c : cases) {
            const std::string path = dir + "/dither-" + c.name + ".log";
            Regime rg; rg.play = c.play; rg.hz = 70.0;
            emit(c.lo, c.hi, 2, rg, 300.0, path.c_str());
            std::printf("wrote %s\n", path.c_str());
        }
        return 0;
    }
    const double HZ = 70.0;
    // **No cap.** Reflecting a draw to bound the accumulated offset destroys the
    // one property the instrument needs: at +-3 counts inside +-6 the reflection
    // fired on 12.6% of draws, the sequence stopped being white, and a planted
    // fixed delay came back 2.51 instead of 2.00 with a spread of 3.33 instead of
    // 0.00. Without it the same case is EXACT. The accumulated injection does
    // random-walk -- 141 counts at amplitude 1 -- but that counter is not the
    // camera: the servo regulates the camera, and section 3 measures what the
    // view actually costs, which is a few tenths of a degree.
    const int CAP = 100000;
    (void)CAP;
    std::printf("Closed loop throughout: real head_look_step, head at 0.3 Hz / "
                "20 deg, kp 0.25.\nPRBS: white +-N counts per tick. No cap — see "
                "the note at CAP for why one\nruins the instrument it was meant "
                "to protect.\n\n");

    std::printf("1. PLANTED bands recovered through the running servo\n");
    std::printf("%12s %10s %11s %10s %11s %8s\n", "planted", "true mean",
                "recovered", "true sd", "recovered", "sum P");
    double worst_mean = 0.0, worst_sd = 0.0, worst_mass = 0.0;
    // **Only bands the LOOP can survive.** tests/servo_rate.cpp measures this
    // servo reaching X4's clamp at a mean delay of 6 ticks; past that the camera
    // slams between the stops and the clamp is a nonlinearity no deconvolution
    // can see through. Bands of mean 6+ are therefore out of range for this
    // instrument, not merely hard -- and that is the honest scope, because the
    // session it has to measure (285b-s1) is a CALM one. A run that goes wild is
    // a run this cannot read, and `sum P` says so.
    for (auto bd : {std::pair<int,int>{2,2}, {3,3}, {1,5}, {2,6}, {1,7}}) {
        std::vector<int> v; std::vector<double> u, dc;
        session(bd.first, bd.second, 3, CAP, HZ, 300.0, v, u, dc);
        const Recovered r = recover(v, u, dc, 28);
        const double tm = 0.5 * (bd.first + bd.second);
        const int k = bd.second - bd.first + 1;
        const double ts = std::sqrt((k * k - 1.0) / 12.0);
        std::printf("%8d-%-3d %10.2f %11.2f %10.2f %11.2f %8.2f\n", bd.first,
                    bd.second, tm, r.mean_lag, ts, r.spread_lag, r.total);
        worst_mean = std::fmax(worst_mean, std::fabs(r.mean_lag - tm));
        worst_sd = std::fmax(worst_sd, std::fabs(r.spread_lag - ts));
        worst_mass = std::fmax(worst_mass, std::fabs(r.total - 1.0));
    }
    std::printf("   worst error: mean %.2f ticks, spread %.2f ticks, mass %.2f\n",
                worst_mean, worst_sd, worst_mass);
    check(worst_mass < 0.05,
          "sum P stays near 1 — the plant delivers everything, so a recovered "
          "mass far from unity means the deconvolution drifted and the answer "
          "must not be read");
    check(worst_mean < 1.5, "the MEAN comes back within 1.5 ticks at every depth");
    check(worst_sd < 1.5,
          "the SPREAD comes back within 1.5 ticks — a wide band is "
          "distinguishable from a narrow one, which is the whole question");

    std::printf("\n2. dither OFF — the recovery must report nothing\n");
    {
        std::vector<int> v; std::vector<double> u, dc;
        session(2, 6, 0, CAP, HZ, 300.0, v, u, dc);
        const Recovered r = recover(v, u, dc, 28);
        std::printf("   ok=%d\n", r.ok ? 1 : 0);
        check(!r.ok, "with no dither it REFUSES rather than building a band out "
                     "of the loop itself");
    }

    // This goes into a player's headset, so the amplitude comes from a table and
    // the accumulated offset is MEASURED rather than asserted to be small.
    std::printf("\n3. amplitude: what is needed, and what it costs the view\n");
    std::printf("%8s %9s %10s %9s %12s %11s %10s\n", "counts", "degrees",
                "mean lag", "spread", "worst accum", "added rms", "reflected");
    int smallest = 0;
    for (int c : {1, 2, 3, 5}) {
        std::vector<int> v; std::vector<double> u, dc;
        const Stats st = session(2, 6, c, CAP, HZ, 300.0, v, u, dc);
        const Recovered r = recover(v, u, dc, 28);
        std::printf("%8d %9.3f %10.2f %9.2f %8d cts %11.3f %9.1f%%\n", c,
                    c * 0.115, r.mean_lag, r.spread_lag, st.worst_acc,
                    st.rms_extra, st.reflect_pct);
        const bool good = r.ok && std::fabs(r.mean_lag - 4.0) < 1.5 &&
                          std::fabs(r.spread_lag - 1.71) < 1.5;
        if (good && !smallest) smallest = c;
    }
    if (smallest)
        std::printf("   smallest amplitude that recovers the 2-6 band: %d "
                    "count(s) = %.3f deg\n", smallest, smallest * 0.115);
    check(smallest > 0,
          "some amplitude recovers the band, and the table says which is the "
          "smallest — the run does not have to guess");

    std::printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
