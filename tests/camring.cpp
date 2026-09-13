// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The staleness matcher, driven against series whose staleness is known by
// construction. This is the header the injector uses, not a copy of it.
//
// The question it has to answer in take 286 is whether an off-thread
// GetCameraRotation returns a value X4 has already moved past. An instrument
// that cannot be shown to report a KNOWN lag correctly would answer it either
// way and mean nothing, so each case below plants a lag and checks it comes
// back.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../common/x4vr_camring.hpp"

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        g_fail++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}

// A camera sweeping at `rate` deg/s, sampled at 90 Hz, pushed into the ring.
// Returns the value and time of the sample `stale_frames` back from the newest.
struct Fed { float y_now, y_stale; double t_now; };
static Fed feed(x4vr::CamRing &r, float rate, int frames, int stale_frames) {
    Fed f{};
    const double dt = 1.0 / 90.0;
    for (int i = 0; i < frames; i++) {
        const double t = i * dt;
        const float y = (float)(rate * t);
        r.push(y, t);
        if (i == frames - 1) { f.y_now = y; f.t_now = t; }
        if (i == frames - 1 - stale_frames) f.y_stale = y;
    }
    return f;
}

int main() {
    const double frame = 1.0 / 90.0;

    // ---- the CONTROL: a current reading, matched with the newest entry
    // excluded, must come back at about one frame. Without this the measurement
    // below is a bare number of milliseconds with nothing to be read against.
    {
        x4vr::CamRing r;
        const Fed f = feed(r, 200.0f, 100, 0);
        double lag = 0.0; float spread = 0.0f, resid = 0.0f;
        const bool ok = r.age_of(f.y_now, f.t_now, 1, &lag, &spread, &resid);
        check(ok, "control: a moving camera is measurable at all");
        check(std::fabs(lag - frame) < 0.5 * frame,
              "control: a CURRENT reading matches one frame back, as it must");
        printf("      control lag %.2f ms (one frame is %.2f ms), residual "
               "%.3f deg, spread %.1f deg\n", lag * 1e3, frame * 1e3, resid,
               spread);
    }

    // ---- the MEASUREMENT: a reading planted N frames stale must report N
    // frames. Several depths, because an estimator that is right at one lag and
    // saturates beyond it would pass a single case and mislead on the run.
    {
        printf("      planted staleness vs reported, camera at 200 deg/s:\n");
        float worst = 0.0f;
        int refused_beyond = 0;
        for (int n : {0, 1, 4, 9, 18, 45}) {
            x4vr::CamRing r;
            const Fed f = feed(r, 200.0f, 120, n);
            double lag = 0.0; float spread = 0.0f, resid = 0.0f;
            const bool ok = r.age_of(f.y_stale, f.t_now, 0, &lag, &spread,
                                     &resid);
            const double want = n * frame;
            // Beyond the 0.30 s window the answer is not available, and the
            // instrument says so instead of pinning to the oldest entry. Stating
            // the range is part of the measurement, not a caveat on it.
            const bool in_range = want <= 0.30;
            printf("        planted %5.1f ms -> reported %5.1f ms  (%s%s)\n",
                   want * 1e3, lag * 1e3, ok ? "measured" : "REFUSED",
                   in_range ? "" : ", beyond the 300 ms window — expected");
            if (!in_range) {
                if (!ok)
                    refused_beyond++;
                continue;
            }
            if (ok)
                worst = std::fmax(worst, (float)std::fabs(lag - want));
            else
                worst = 1e9f; // an in-range depth must not be refused
        }
        check(refused_beyond == 1,
              "a staleness beyond the window is REFUSED, not pinned to the "
              "oldest entry");
        check(worst < 0.5f * (float)frame,
              "every planted staleness comes back within half a frame");
        printf("      worst error across all depths: %.2f ms\n", worst * 1e3);
    }

    // ---- it must REFUSE a camera too still to match, rather than report noise
    {
        x4vr::CamRing r;
        feed(r, 0.5f, 120, 0); // 0.5 deg/s: a whole window spans 0.67 deg
        double lag = 0.0; float spread = 0.0f, resid = 0.0f;
        const bool ok = r.age_of(0.3f, 120.0 / 90.0, 0, &lag, &spread, &resid);
        check(!ok, "a near-static camera is REFUSED, not reported as lag 0");
        printf("      spread was %.2f deg against a %.1f deg floor\n", spread,
               x4vr::CamRing::kMinSpread);
    }

    // ---- fast motion must NOT manufacture a lag
    //
    // This is the alternative the run has to rule out: if the camera really is
    // slewing at ~1000 deg/s and our read is current, the matcher must say so.
    // If speed alone produced a lag, every fast moment would read as stale and
    // the instrument would confirm whatever it was pointed at.
    {
        printf("      a CURRENT reading at rising speeds (must stay ~0):\n");
        float worst = 0.0f;
        for (float rate : {50.0f, 200.0f, 600.0f, 1200.0f}) {
            x4vr::CamRing r;
            const Fed f = feed(r, rate, 120, 0);
            double lag = 0.0; float spread = 0.0f, resid = 0.0f;
            if (r.age_of(f.y_now, f.t_now, 0, &lag, &spread, &resid)) {
                printf("        %6.0f deg/s -> lag %5.2f ms\n", rate, lag * 1e3);
                worst = std::fmax(worst, (float)std::fabs(lag));
            }
        }
        check(worst < 0.5f * (float)frame,
              "speed alone never manufactures a lag — the alternative stays "
              "distinguishable");
    }

    // ---- a SWEEPING camera, where values recur
    //
    // Every case above ramps monotonically, so each value appears once and the
    // match cannot be ambiguous. A real camera sweeps back and forth: take
    // 285b's oscillated between X4's two clamps many times a second, so any
    // given angle appears again and again across the window and the matcher can
    // lock onto an occurrence from an earlier sweep. That would report a
    // spuriously LARGE lag — which is the very answer the run is looking for,
    // so it must be shown not to happen before the run, not explained after it.
    {
        printf("      a sweeping camera (values recur), current reading:\n");
        const double dt = 1.0 / 90.0;
        float worst = 0.0f;
        int measured = 0, refused = 0;
        for (float hz : {0.5f, 2.0f, 5.0f}) {
            x4vr::CamRing r;
            float y = 0.0f; double t = 0.0;
            for (int i = 0; i < 400; i++) {
                t = i * dt;
                y = 65.0f * std::sin(2.0f * 3.14159265f * hz * (float)t);
                r.push(y, t);
            }
            double lag = 0.0; float spread = 0.0f, resid = 0.0f;
            if (r.age_of(y, t, 1, &lag, &spread, &resid)) {
                printf("        %.1f Hz sweep -> lag %6.2f ms (one frame is "
                       "%.2f ms), residual %.3f deg\n", hz, lag * 1e3,
                       frame * 1e3, resid);
                worst = std::fmax(worst, (float)std::fabs(lag));
                measured++;
            } else {
                refused++;
            }
        }
        check(measured > 0, "a sweeping camera is measurable at all");
        // Deliberately loose: the point is not that recurrence is harmless but
        // that it does not fabricate the tens of milliseconds the run is
        // hunting. Anything near a frame is fine; 40 ms would not be.
        check(worst < 5.0f * (float)frame,
              "value recurrence does NOT fabricate a large lag — a current "
              "reading still reads as current on a sweeping camera");
        printf("      worst lag on a sweeping camera: %.2f ms (%d measured, "
               "%d refused)\n", worst * 1e3, measured, refused);
    }

    // ---- an empty or barely-filled ring reports nothing
    {
        x4vr::CamRing r;
        double lag = 0.0; float spread = 0.0f, resid = 0.0f;
        check(!r.age_of(10.0f, 1.0, 0, &lag, &spread, &resid),
              "an empty ring is refused, not answered");
        for (int i = 0; i < 4; i++)
            r.push((float)i * 5.0f, i / 90.0);
        check(!r.age_of(10.0f, 1.0, 0, &lag, &spread, &resid),
              "a ring with too few entries is refused");
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
