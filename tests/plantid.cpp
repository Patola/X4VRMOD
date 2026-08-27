// Task #73 -- the open-loop identification sequence, checked before it is run.
//
// Two things have to be true before a take is spent on it:
//
//   1. The sequence never drives X4's camera into its clamp during a MEASURED
//      phase. If it did, the tail of every large element would record "the
//      camera stopped delivering" -- which is the exact conclusion the run
//      exists to test, arriving as an artefact of the instrument. This is not
//      hypothetical: the first draft used a fixed 10-step sustain, which is
//      119.6 deg of travel for the largest command against 60 deg of runway.
//
//   2. The data it produces can actually tell the candidate plants apart. A
//      sequence that yields the same numbers for an integrator and a smoother
//      would be a run that cannot fail, and this project has paid for several
//      of those.
#include "../common/x4vr_plantid.hpp"

#include <cmath>
#include <cstdio>
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

static constexpr float kGain = 0.115f;
static constexpr float kClamp = 65.0f;

// The candidate plants. Each takes a commanded delta in degrees and advances
// its own camera angle, clamped the way X4 clamps.
struct Plant {
    virtual ~Plant() = default;
    virtual void step(double cmd_deg) = 0;
    virtual double angle() const = 0;
};

// What tests/servo_lag.cpp assumed: the command IS the motion.
struct Integrator : Plant {
    double a = 0.0;
    void step(double c) override {
        a += c;
        a = a > kClamp ? kClamp : (a < -kClamp ? -kClamp : a);
    }
    double angle() const override { return a; }
};

// Velocity is smoothed toward the command; position integrates velocity. This
// coasts when the command stops, which is what take 212's 77% hinted at.
struct Smoother : Plant {
    double a = 0.0, v = 0.0, k;
    explicit Smoother(double kk) : k(kk) {}
    void step(double c) override {
        v += k * (c - v);
        a += v;
        a = a > kClamp ? kClamp : (a < -kClamp ? -kClamp : a);
    }
    double angle() const override { return a; }
};

// A hard per-step ceiling and nothing else -- the "anglespeed cap" story that
// take 212's 541 deg/s already refuted, kept as a negative control.
struct Capped : Plant {
    double a = 0.0, cap;
    explicit Capped(double c) : cap(c) {}
    void step(double c) override {
        c = c > cap ? cap : (c < -cap ? -cap : c);
        a += c;
        a = a > kClamp ? kClamp : (a < -kClamp ? -kClamp : a);
    }
    double angle() const override { return a; }
};

struct Trace {
    std::vector<double> cmd, cam;
    std::vector<x4vr::PlantPhase> phase;
    std::vector<int> size, meas;
    double worst_measured_abs = 0.0;
    int steps = 0, resets = 0;
};

static Trace run_sequence(Plant &p, int passes) {
    x4vr::PlantId pid;
    pid.passes = passes;
    Trace t;
    for (int guard = 0; guard < 200000 && !pid.finished; guard++) {
        const double before = p.angle();
        const x4vr::PlantStep ps =
            x4vr::plant_id_step(pid, (float)before, true, kGain);
        const double cmd = -ps.dx * (double)kGain; // sign_yaw is -1
        if (ps.measuring) {
            t.cmd.push_back(cmd);
            t.cam.push_back(before);
            t.phase.push_back(pid.phase);
            t.size.push_back(pid.size_counts());
            t.meas.push_back(1);
            const double m = std::fabs(before);
            if (m > t.worst_measured_abs)
                t.worst_measured_abs = m;
        } else {
            t.resets++;
        }
        p.step(cmd);
        t.steps++;
    }
    return t;
}

int main() {
    char buf[256];
    x4vr::PlantId spec;

    std::printf("1. the sequence is bounded and complete\n");
    {
        Integrator plant;
        const Trace t = run_sequence(plant, spec.passes);
        std::snprintf(buf, sizeof buf, "%d ticks (%d reset), predicted %d "
                                       "measured",
                      t.steps, t.resets, x4vr::plant_id_total_steps(spec, kGain));
        check(t.steps > 0 && (int)t.cmd.size() ==
                                 x4vr::plant_id_total_steps(spec, kGain),
              "every element ran, and the predicted length is right", buf);

        // THE check this file exists for.
        std::snprintf(buf, sizeof buf,
                      "worst |camera| during a measured phase = %.1f deg, "
                      "X4 clamps at %.0f", t.worst_measured_abs, kClamp);
        check(t.worst_measured_abs < kClamp - 3.0,
              "no measured sample is near X4's clamp", buf);

        std::snprintf(buf, sizeof buf, "~%.0f s at 24 Hz plus %d reset ticks",
                      t.cmd.size() / 24.0, t.resets);
        check(t.cmd.size() / 24.0 < 400.0, "the run fits in one sitting", buf);

        // **The runway must be on the side the burst travels toward.** With it
        // on the wrong side the sequence still runs, still logs, and still
        // produces a delivery curve -- one shaped entirely by X4's clamp. The
        // only way to see it is to check the direction of travel explicitly.
        int wrong = 0, checked = 0;
        for (size_t i = 1; i < t.cmd.size(); i++) {
            if (t.phase[i - 1] != x4vr::PlantPhase::Sustain) continue;
            if (std::fabs(t.cmd[i - 1]) < 1e-9) continue;
            const double moved = t.cam[i] - t.cam[i - 1];
            if (std::fabs(moved) < 1e-9) continue;
            checked++;
            // A positive test size must move the camera NEGATIVE (sign_yaw is
            // -1). Anything else means the runway is behind the burst.
            //
            // Checking "moves toward centre" instead was wrong: a burst starts
            // at +27 and ends near -28, so its whole second half moves away
            // from centre by design. The invariant is about DIRECTION, not
            // distance from zero.
            if (moved * (double)t.size[i - 1] > 0) wrong++;
        }
        std::snprintf(buf, sizeof buf,
                      "%d of %d sustained steps moved the wrong way for their "
                      "sign", wrong, checked);
        check(checked > 0 && wrong == 0,
              "each burst travels across its runway, not off the end", buf);
    }

    std::printf("2. the sustain length adapts to the command size\n");
    for (int i = 0; i < x4vr::PlantId::kNumSizes; i++) {
        const int c = x4vr::PlantId::kSizes[i];
        const int n = spec.sustain_steps_for(c, kGain);
        const double travel = n * c * (double)kGain;
        std::snprintf(buf, sizeof buf, "%3d counts (%5.2f deg) -> %2d steps, "
                                       "%5.1f deg of travel",
                      c, c * kGain, n, travel);
        check(travel <= spec.sustain_travel_deg + 1e-3,
              "travel stays inside the runway", buf);
    }

    std::printf("3. THE POINT: the trace separates the candidate plants\n");
    {
        // For each plant, the two numbers the scorer will read: steady-state
        // delivery during SUSTAIN, and how far the camera coasts after the
        // command stops. If two different plants produce the same pair, this
        // run cannot answer anything.
        // **Per SIZE, not aggregated.** The staircase is the whole design: a
        // per-step cap shows as delivery COLLAPSING with size while a smoother
        // holds roughly flat. Averaging the sizes together throws that away --
        // it put an integrator at 100% and a hard cap at 78.6%, only 21 points
        // apart, because the capped sizes are the ones with fewest steps.
        struct Row { const char *name; double small, large, coast; };
        std::vector<Row> rows;
        Integrator integ;
        Smoother smooth(0.25);
        Capped cap(4.0);
        Plant *plants[3] = {&integ, &smooth, &cap};
        const char *names[3] = {"integrator", "smoother  ", "capped    "};
        for (int pi = 0; pi < 3; pi++) {
            const Trace t = run_sequence(*plants[pi], 1);
            double sc[2] = {0, 0}, sm[2] = {0, 0}; // [0]=small sizes, [1]=large
            double c0 = 0, c1 = 0;      // coast: first step after impulse
            for (size_t i = 1; i < t.cmd.size(); i++) {
                const double moved = t.cam[i] - t.cam[i - 1];
                if (t.phase[i - 1] == x4vr::PlantPhase::Sustain &&
                    std::fabs(t.cmd[i - 1]) > 1e-9) {
                    const int b = std::abs(t.size[i - 1]) >= 32 ? 1 : 0;
                    sc[b] += std::fabs(t.cmd[i - 1]);
                    sm[b] += std::fabs(moved);
                }
                // Coast step 1 vs coast step 2: how much motion survives with
                // nothing commanded.
                if (t.phase[i - 1] == x4vr::PlantPhase::Coast &&
                    std::fabs(t.cmd[i - 1]) < 1e-9 && i + 1 < t.cmd.size()) {
                    const double next = t.cam[i + 1] - t.cam[i];
                    if (std::fabs(moved) > 0.05) {
                        c0 += std::fabs(moved);
                        c1 += std::fabs(next);
                    }
                }
            }
            const double dsmall = sc[0] > 0 ? sm[0] / sc[0] : 0.0;
            const double dlarge = sc[1] > 0 ? sm[1] / sc[1] : 0.0;
            const double coast = c0 > 0 ? c1 / c0 : 0.0;
            rows.push_back({names[pi], dsmall, dlarge, coast});
            std::printf("       %s delivery: small %5.1f%%  large %5.1f%%  "
                        "(fade %5.2fx)   coast retains %5.1f%%\n",
                        names[pi], dsmall * 100, dlarge * 100,
                        dsmall > 0 ? dlarge / dsmall : 0.0, coast * 100);
        }
        check(std::fabs(rows[0].coast - rows[1].coast) > 0.30,
              "coast separates integrator from smoother", "");
        std::snprintf(buf, sizeof buf,
                      "integrator holds %.2fx across the staircase, a cap "
                      "fades to %.2fx",
                      rows[0].large / rows[0].small, rows[2].large / rows[2].small);
        check(rows[0].large / rows[0].small - rows[2].large / rows[2].small > 0.30,
              "the size staircase separates integrator from capped", buf);
        // And the triple as a whole must be unique per plant, not just each
        // number individually.
        bool unique = true;
        for (size_t i = 0; i < rows.size() && unique; i++)
            for (size_t j = i + 1; j < rows.size(); j++)
                if (std::fabs(rows[i].small - rows[j].small) < 0.15 &&
                    std::fabs(rows[i].large - rows[j].large) < 0.15 &&
                    std::fabs(rows[i].coast - rows[j].coast) < 0.15)
                    unique = false;
        check(unique, "no two candidate plants land on the same signature", "");
    }

    std::printf("4. the head is not in the loop anywhere\n");
    {
        // Same plant, same sequence, run twice -- there is no head input to
        // vary, so identical output is the claim that the player cannot
        // influence this measurement by moving.
        Integrator a, b;
        const Trace ta = run_sequence(a, 1);
        const Trace tb = run_sequence(b, 1);
        bool same = ta.cmd.size() == tb.cmd.size();
        for (size_t i = 0; same && i < ta.cmd.size(); i++)
            same = ta.cmd[i] == tb.cmd[i];
        check(same, "the command stream is fully determined by the sequence",
              "so nothing the player does can change it");
    }

    std::printf(failures ? "\nFAILED (%d)\n" : "\nAll plant-id checks passed\n",
                failures);
    return failures ? 1 : 0;
}
