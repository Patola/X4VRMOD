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

// **Take 289: the sequence must run in the READBACK's time base, not the tick's.**
//
// Every phase below was counted in ticks -- IMPULSE one, COAST fourteen -- while
// the plant is only observable when X4's camera readback CHANGES. Take 288 ran
// 2.5 ticks per update and every command size delivered 99.6-99.7%. Take 289
// raised X4VR_TICK_HZ to 90; the readback rate barely moved (12.1 -> 16.0 per
// second), giving 5.7 ticks per update, and delivery came back 49.8 / 83.0 /
// 124.6%. **124.6% is impossible**, and the run was void.
//
// So the sequence now steps once per camera update and idles in between. One
// command per observation, phases counted in observations: the same experiment
// at any tick rate, which is exactly what P289 needs in order to compare two.
//
// **Calibrate exists because a value change is not a refresh.** During SETTLE
// and COAST the camera is genuinely still, so "the readback did not change" and
// "the readback did not refresh" are the same observation with opposite
// meanings. The ratio therefore has to be measured where the camera is
// definitely moving: one short phase, driving a constant command, counting ticks
// against changes. It is excluded from the measurement like Reset is.
enum class PlantPhase {
    Calibrate, Reset, Settle, Impulse, Coast, Sustain, Decay, Done
};

inline const char *plant_phase_name(PlantPhase p) {
    switch (p) {
    case PlantPhase::Calibrate: return "calibrate";
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

    // --- the readback's time base ------------------------------------------
    //
    // How many servo ticks pass per camera update, measured in Calibrate. The
    // measured phases step once per update and send nothing on the ticks
    // between, so the whole experiment is identical at 30 Hz and at 90.
    int calib_ticks = 40;       // ticks to spend measuring; ~1.3 s at 30 Hz
    int calib_counts = 16;      // command per calibrate tick: must MOVE the camera
    float ticks_per_update = 1.0f;
    int cal_t = 0, cal_cmp = 0, cal_changes = 0;
    float cal_last = 0.0f;
    bool cal_have = false;
    // Sub-tick counter: the sequence acts when this reaches ticks_per_update.
    float sub = 0.0f;

    // --- live state -------------------------------------------------------
    int pass = 0;
    int element = 0;
    PlantPhase phase = PlantPhase::Calibrate;
    int k = 0;              // step within the phase (in camera UPDATES)
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
    // **The phase that PRODUCED this dx**, which is not the same as the
    // phase the sequence is in afterwards. Take 213 logged the latter, so the
    // single impulse command every element makes arrived stamped `phase=coast`
    // and an analysis looking for `phase=impulse AND dx!=0` found nothing at
    // all. An instrument that reports a different branch than the one it took
    // is the same defect as 18 modules logging "World" after being dispatched
    // as UI.
    PlantPhase acted = PlantPhase::Calibrate;
    // **True only on the ticks where the sequence actually advanced.** The
    // injector logs these and skips the rest, so one logged line is one camera
    // update and the scorer's lag profile is indexed by OBSERVATION rather than
    // by tick. Logging the idle ticks would put the tick rate straight back into
    // the measurement, which is take 289's defect with extra steps.
    bool stepped = false;
};

inline PlantStep plant_id_step(PlantId &p, float cam_yaw_deg, bool have_cam,
                               float gain_deg_per_count) {
    PlantStep out;
    if (p.finished || p.phase == PlantPhase::Done)
        return out;

    out.acted = p.phase;

    // **Calibrate: how many ticks per camera update?** Driven with a constant
    // command so the camera is certainly moving and a value change therefore IS
    // a refresh. Excluded from the measurement.
    if (p.phase == PlantPhase::Calibrate) {
        out.stepped = true;
        // **Alternating, because a straight drive walks into X4's stop.** 40
        // ticks of 16 counts is 73.6 deg against a 65 deg clamp, and a clamped
        // camera stops changing — so the ratio this phase exists to measure
        // would read the clamp instead, silently, and every phase length after
        // it would be wrong. Reversing every 10 ticks bounds the excursion at
        // 18.4 deg from wherever the camera happens to start.
        const bool back = ((p.cal_t / 10) % 2) != 0;
        out.dx = back ? -p.calib_counts : p.calib_counts;
        if (have_cam) {
            // **Comparisons, not ticks, and the difference is not cosmetic.**
            // The first tick has nothing to compare against, so 40 ticks yield
            // 39 comparisons. Dividing ticks by changes gave 40/39 = 1.026 for a
            // readback that refreshes EVERY tick — and the fractional
            // accumulator below then skipped one step in every 39, smearing the
            // impulse response and turning a delayed integrator into "SMOOTHED /
            // MOMENTUM". The ratio has to be exactly 1.0 in that case, and only
            // comparisons/changes gives it.
            if (p.cal_have) {
                p.cal_cmp++;
                if (cam_yaw_deg != p.cal_last)
                    p.cal_changes++;
            }
            p.cal_last = cam_yaw_deg;
            p.cal_have = true;
        }
        if (++p.cal_t >= p.calib_ticks) {
            // Four changes is the floor for a ratio worth having; below it the
            // readback may simply not be live and 1.0 is the safe answer --
            // that is the old behaviour, which was correct whenever the tick
            // rate already matched the readback.
            p.ticks_per_update =
                (p.cal_changes >= 4 && p.cal_cmp > 0)
                    ? (float)p.cal_cmp / (float)p.cal_changes
                    : 1.0f;
            if (p.ticks_per_update < 1.0f)
                p.ticks_per_update = 1.0f;
            if (p.ticks_per_update > 16.0f)
                p.ticks_per_update = 16.0f; // a runaway ratio would stall the run
            p.sub = 0.0f;
            p.k = 0;
            p.phase = PlantPhase::Reset;
            out.phase_changed = true;
        }
        return out;
    }

    // **The throttle.** Every phase below counts in camera updates, so the
    // sequence acts once per `ticks_per_update` ticks and sends nothing in
    // between. Fractional, accumulated rather than rounded: a ratio of 2.5 must
    // average 2.5, and rounding it to 2 or 3 would bias every phase length by up
    // to 20% and put the tick rate back into the answer.
    p.sub += 1.0f;
    if (p.sub < p.ticks_per_update)
        return out;            // idle tick: dx 0, stepped false, nothing logged
    p.sub -= p.ticks_per_update;
    out.stepped = true;

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
}

// How many MEASURED SEQUENCE STEPS one full run takes — which is now camera
// updates, not ticks. Multiply by `ticks_per_update` for the tick cost and
// divide by the camera's update rate for the wall-clock cost; `plant_id_seconds`
// does that, so a run is priced before it is spent rather than discovered to be
// too short afterwards.
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

// Wall-clock cost, in seconds, at a given camera UPDATE rate.
//
// **The update rate, not the tick rate.** The sequence now advances once per
// update, so a run costs the same time whether the servo ticks at 30 Hz or 90 —
// which is the whole point, and also the thing that makes P289's two arms
// comparable. Take 288's readback changed 12.1 times a second and take 289's
// 16.0, so a full run is 200-265 s at these phase lengths: long, and the price
// of asking the question at a rate the plant can actually answer.
inline double plant_id_seconds(const PlantId &p, double camera_update_hz,
                               float gain_deg_per_count = 0.115f) {
    if (!(camera_update_hz > 0.0))
        return 0.0;
    return (double)plant_id_total_steps(p, gain_deg_per_count) /
           camera_update_hz;
}

} // namespace x4vr
