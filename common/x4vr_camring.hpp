// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// **Task #75: is a camera reading STALE, or is the camera simply fast?**
//
// Take 285b read X4's camera from two threads on the same frame -- ours from the
// tick thread, the layer's at the first render pass -- and they disagreed by up
// to 59.588 deg during the oscillation while agreeing to 1.5 deg outside it.
// That comparison cannot choose between two explanations with opposite fixes:
//
//   staleness   our read returns a value X4 has already moved past, so the servo
//               steers on a camera wrong by tens of degrees
//   fast motion the camera genuinely slews at ~1000 deg/s in bursts and the two
//               threads sample either side of one
//
// Both predict a disagreement the size of the jump. What separates them is
// matching a reading against a TIME SERIES rather than against one other
// sample: fast motion spreads the series' values apart and makes the match
// SHARPER, but it cannot make the matched entry OLD. Only staleness does.
//
// This header exists so the matcher can be driven offline against series whose
// staleness is known by construction. Testing a copy of it would only prove the
// copy works.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace x4vr {

struct CamRing {
    static constexpr int N = 128;
    // Below this the window's values are too alike for a match to mean
    // anything -- with a static camera every entry matches equally well and the
    // reported age is whichever one happened to win a tie. Refused, not
    // reported.
    static constexpr float kMinSpread = 3.0f; // deg

    std::atomic<uint32_t> w{0};
    float yaw[N] = {};
    double t[N] = {};

    // Written ONLY by X4's own thread.
    void push(float y, double now) {
        const uint32_t i = w.load(std::memory_order_relaxed);
        yaw[i % N] = y;
        t[i % N] = now;
        w.store(i + 1, std::memory_order_release);
    }

    // How old is the reading `y`, taken at `now`, against the recorded series?
    //
    // `skip_newest` is what makes the control possible: a reading taken on X4's
    // own thread, matched against the ring minus the entry it just wrote, should
    // come back at about one poll period. That is what "current" looks like
    // through this matcher, and without it a lag in milliseconds has no scale.
    //
    // Range: N entries at X4's poll rate, so roughly 1.4 s at 90 Hz. A lag at
    // the far end of that is "older than the window" and is not a measurement.
    // **Closest-in-value is WRONG, and confidently so.** A sweeping camera
    // revisits every angle: driven with a monotonic ramp the closest match is
    // unambiguous, but on a 2 Hz sweep a reading that is perfectly CURRENT
    // matched an occurrence half a second earlier and reported 500 ms of lag —
    // 1200 ms at 5 Hz. Take 285b's camera oscillated between X4's clamps many
    // times a second, so that version of this function would have reported
    // enormous staleness whatever the truth was, and confirmed the hypothesis it
    // was built to test.
    //
    // So: scan from NEWEST backwards and take the first entry that matches
    // within a tolerance, rather than the best match anywhere in the window. A
    // current reading then stops at the newest occurrence; only a genuinely old
    // value walks further back. The tolerance is derived from how far the camera
    // moves between samples, because that is the distance a current reading can
    // legitimately differ by — a fixed tolerance would refuse everything when
    // the camera is fast and accept everything when it is slow.
    //
    // `max_age_s` bounds both the search and the answer: a lag can only be
    // reported up to this, and beyond it the reading is refused rather than
    // pinned to the oldest entry.
    bool age_of(float y, double now, int skip_newest, double *lag_s,
                float *spread_deg, float *resid_deg,
                double max_age_s = 0.30) const {
        const uint32_t wn = w.load(std::memory_order_acquire);
        const int have = (int)(wn < (uint32_t)N ? wn : (uint32_t)N) - skip_newest;
        if (have < 8)
            return false;

        float lo = 1e30f, hi = -1e30f, step_sum = 0.0f;
        int step_n = 0, in_window = 0;
        float prev = 0.0f;
        bool have_prev = false;
        for (int k = skip_newest; k < have + skip_newest; k++) {
            const uint32_t idx = (wn - 1 - (uint32_t)k) % (uint32_t)N;
            if (now - t[idx] > max_age_s)
                break;
            in_window++;
            const float v = yaw[idx];
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
            if (have_prev) {
                step_sum += std::fabs(v - prev);
                step_n++;
            }
            prev = v;
            have_prev = true;
        }
        if (spread_deg)
            *spread_deg = (in_window > 0) ? hi - lo : 0.0f;
        if (in_window < 8 || hi - lo < kMinSpread)
            return false;

        const float mean_step = step_n ? step_sum / (float)step_n : 0.0f;
        const float tol = std::fmax(0.25f, 1.5f * mean_step);
        // Once inside the tolerance, keep walking back while the residual is
        // still IMPROVING and stop at the local minimum. Returning the first
        // entry within tolerance instead reported every planted lag one frame
        // early — a systematic 11 ms bias, which is the same order as the
        // staleness being hunted and would have been read as signal.
        bool found = false;
        float best = 0.0f;
        double best_t = 0.0;
        for (int k = skip_newest; k < have + skip_newest; k++) {
            const uint32_t idx = (wn - 1 - (uint32_t)k) % (uint32_t)N;
            if (now - t[idx] > max_age_s)
                break;
            const float d = std::fabs(yaw[idx] - y);
            if (!found) {
                if (d <= tol) {
                    found = true;
                    best = d;
                    best_t = t[idx];
                }
                continue;
            }
            if (d < best) {
                best = d;
                best_t = t[idx];
                continue;
            }
            break; // past the minimum; an older occurrence is a different sweep
        }
        if (found) {
            if (resid_deg)
                *resid_deg = best;
            if (lag_s)
                *lag_s = now - best_t;
            return true;
        }
        // Nothing in the window resembles this reading. That is not a lag
        // measurement and must not be reported as one.
        return false;
    }
};

} // namespace x4vr
