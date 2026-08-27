// Task #73 -- open-loop identification of X4's free-look camera.
//
// **Why this exists.** Take 212 proved the servo commands exactly what it is
// asked to and that X4's camera does not follow it: delivery falls from 95% to
// 71% as the command grows, and splits on direction -- 87-126% when continuing
// the current motion against 21-49% when reversing it. Everything about that
// was inferred THROUGH a closed loop, which sets the command from the error it
// is trying to measure. Two explanations built on it died the same day: a
// 200 deg/s cap (the camera reaches 541) and a reversal-only story (steady
// turns read 0.99x too).
//
// So stop inferring. Drive the plant with a known signal and watch.
//
// The sequence is fixed, deterministic and self-describing, and the head is not
// in the loop at all -- the player's head can do anything without affecting a
// single command. Each element is:
//
//   RESET    steer to a start angle, so the burst has runway before X4's
//            clamp. Closed-loop, and NOT part of the measurement.
//   SETTLE   command nothing; confirm the camera is actually still
//   IMPULSE  ONE command of the test size
//   COAST    command nothing and watch. This is the dynamics: an integrator
//            stops dead, a smoother decays, momentum overshoots.
//   SUSTAIN  the same command repeatedly. This is the steady-state gain, and
//            where a velocity cap would appear.
//   DECAY    command nothing again, from a moving start rather than a still one
//
// Sizes span the whole usable range and both directions, because the direction
// asymmetry is the one thing take 212 established beyond doubt.
#pragma once

#include <cmath>

namespace x4vr {

enum class PlantPhase { Reset, Settle, Impulse, Coast, Sustain, Decay, Done };

inline const char *plant_phase_name(PlantPhase p) {
    switch (p) {
    case PlantPhase::Reset: return "reset";
    case PlantPhase::Settle: return "settle";
    case PlantPhase::Impulse: return "impulse";
    case PlantPhase::Coast: return "coast";
    case PlantPhase::Sustain: return "sustain";
    case PlantPhase::Decay: return "decay";
    default: return "done";
    }
}

struct PlantId {
    // Test sizes in mouse counts. 104 counts is 11.96 deg at the calibrated
    // 0.115 deg/count -- the servo's own max_step_deg, so the top of the range
    // is exactly what the real servo can ask for and no more.
    static constexpr int kSizes[7] = {2, 4, 8, 16, 32, 64, 104};
    static constexpr int kNumSizes = 7;
    static constexpr int kElements = kNumSizes * 2; // each size, both directions

    // **Runway.** A sustained burst travels a long way, and X4's clamp at 65
    // would silently truncate the tail of the largest elements -- turning a
    // plant measurement into a clamp measurement. Starting each element on the
    // far side gives 60 deg of room and keeps every sample inside the linear
    // part of the range.
    float start_angle_deg = 30.0f;  // signed against the test direction
    float reset_tolerance_deg = 3.0f;
    int max_reset_steps = 90;

    int settle_steps = 8;
    int coast_steps = 14;   // long enough to see a decay, not just a stop
    int decay_steps = 14;
    // **The sustain length must depend on the size, or the largest elements
    // measure X4's clamp instead of its response.** Ten steps of the biggest
    // command is 119.6 deg of travel against 60 deg of runway: the tail of
    // every large element would have run into the stop at 65 and been recorded
    // as "the camera stopped delivering", which is precisely the conclusion
    // this run exists to test. Caught by tests/plantid.cpp before it ran.
    float sustain_travel_deg = 55.0f;
    int sustain_min = 3, sustain_max = 30;
    int passes = 3;         // repeats, so a single glitchy element cannot set
                            // the answer

    // --- live state -------------------------------------------------------
    int pass = 0;
    int element = 0;
    PlantPhase phase = PlantPhase::Reset;
    int k = 0;              // step within the phase
    bool finished = false;

    int size_counts() const {
        const int s = kSizes[element / 2];
        return (element % 2) == 0 ? s : -s;
    }
    // Start on the opposite side from where the burst travels.
    //
    // **A POSITIVE dx drives the camera NEGATIVE.** sign_yaw is -1 -- positive
    // mouse x turns the view right while +yaw is left -- so a positive test
    // size travels toward negative angles and its runway belongs on the
    // positive side. The obvious spelling is backwards, it put every element's
    // runway on the wrong side, and the camera walked into X4's 65 deg stop
    // from element 6 onward. tests/plantid.cpp caught it by watching an
    // integrator deliver 74.6% when an integrator delivers 100% by definition.
    float start_target_deg() const {
        return size_counts() > 0 ? start_angle_deg : -start_angle_deg;
    }
    // How many sustained commands this element may issue before its travel
    // would reach X4's clamp. Small commands get more steps, not fewer: they
    // need them to reach steady state at all.
    int sustain_steps_for(int counts, float gain_deg_per_count) const {
        const float mag = std::fabs((float)counts) * gain_deg_per_count;
        if (!(mag > 1e-4f))
            return sustain_max;
        int n = (int)(sustain_travel_deg / mag);
        return n < sustain_min ? sustain_min
                               : (n > sustain_max ? sustain_max : n);
    }
};

// One tick. Returns the mouse dx to send; `advance` is what the caller should
// log. Never touches the head.
struct PlantStep {
    int dx = 0;
    bool measuring = false; // false during reset: not part of the answer
    bool phase_changed = false;
};

inline PlantStep plant_id_step(PlantId &p, float cam_yaw_deg, bool have_cam,
                               float gain_deg_per_count) {
    PlantStep out;
    if (p.finished || p.phase == PlantPhase::Done)
        return out;

    const PlantPhase was = p.phase;
    switch (p.phase) {
    case PlantPhase::Reset: {
        // Closed-loop, deliberately: this is the only part that may use the
        // readback, and it is excluded from the measurement precisely because
        // it does.
        if (!have_cam) {
            // No readback means no way to know where the runway is. Waiting is
            // correct -- guessing would start the burst from an unknown angle
            // and every number after it would be untraceable.
            if (++p.k > p.max_reset_steps) {
                p.k = 0;
                p.phase = PlantPhase::Settle;
                out.phase_changed = true;
            }
            return out;
        }
        const float err = p.start_target_deg() - cam_yaw_deg;
        if (std::fabs(err) <= p.reset_tolerance_deg || ++p.k > p.max_reset_steps) {
            p.k = 0;
            p.phase = PlantPhase::Settle;
            out.phase_changed = true;
            return out;
        }
        float step = 0.25f * err;
        const float cap = 8.0f;
        step = step > cap ? cap : (step < -cap ? -cap : step);
        out.dx = (int)std::lround(-step / gain_deg_per_count);
        return out;
    }
    case PlantPhase::Settle:
        out.measuring = true;
        if (++p.k >= p.settle_steps) {
            p.k = 0;
            p.phase = PlantPhase::Impulse;
            out.phase_changed = true;
        }
        return out;
    case PlantPhase::Impulse:
        out.measuring = true;
        out.dx = p.size_counts();
        p.k = 0;
        p.phase = PlantPhase::Coast;
        out.phase_changed = true;
        return out;
    case PlantPhase::Coast:
        out.measuring = true;
        if (++p.k >= p.coast_steps) {
            p.k = 0;
            p.phase = PlantPhase::Sustain;
            out.phase_changed = true;
        }
        return out;
    case PlantPhase::Sustain:
        out.measuring = true;
        out.dx = p.size_counts();
        if (++p.k >= p.sustain_steps_for(p.size_counts(), gain_deg_per_count)) {
            p.k = 0;
            p.phase = PlantPhase::Decay;
            out.phase_changed = true;
        }
        return out;
    case PlantPhase::Decay:
        out.measuring = true;
        if (++p.k >= p.decay_steps) {
            p.k = 0;
            p.phase = PlantPhase::Reset;
            out.phase_changed = true;
            if (++p.element >= PlantId::kElements) {
                p.element = 0;
                if (++p.pass >= p.passes) {
                    p.finished = true;
                    p.phase = PlantPhase::Done;
                }
            }
        }
        return out;
    default:
        return out;
    }
    (void)was;
}

// How many ticks one full run takes, so the run can be priced before it is
// spent rather than discovered to be too short afterwards.
inline int plant_id_total_steps(const PlantId &p,
                                float gain_deg_per_count = 0.115f) {
    int per_pass = 0;
    for (int e = 0; e < PlantId::kElements; e++) {
        const int counts =
            (e % 2) == 0 ? PlantId::kSizes[e / 2] : -PlantId::kSizes[e / 2];
        per_pass += p.settle_steps + 1 + p.coast_steps + p.decay_steps +
                    p.sustain_steps_for(counts, gain_deg_per_count);
    }
    return p.passes * per_pass; // excludes the reset phases, which are closed
                                // loop and vary with where the camera happens
                                // to be
}

} // namespace x4vr
