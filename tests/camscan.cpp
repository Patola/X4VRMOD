// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The camera-state scan, checked without X4.
//
// What the scan must NOT do is as important as what it must: report a slot
// that matched once by coincidence, report anything when the camera never
// moved, or lose the real slot to a tolerance that is too tight for the way
// X4 derives its angle. Each of those is planted here. Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../injector/x4vr_camscan.hpp"

using x4vr::camscan::Region;
using x4vr::camscan::Scanner;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

static const double kD2R = 1.0 / 57.29577951308232;

static void put_f(Region &r, size_t off, float v) { memcpy(&r.bytes[off], &v, 4); }
static void put_d(Region &r, size_t off, double v) { memcpy(&r.bytes[off], &v, 8); }

// Two regions of noise-free zeros with the state planted where the X4 side
// would find it, and decoys that coincide with the angle exactly once.
static std::vector<Region> world(double yaw_deg, double pitch_deg) {
    std::vector<Region> w(2);
    w[0].name = "cam";
    w[1].name = "cam->[0x58]";
    w[0].bytes.assign(512, 0);
    w[1].bytes.assign(512, 0);
    // The state: yaw in DEGREES as float, pitch in RADIANS negated as double
    // -- two different conventions, so neither is found by luck.
    put_f(w[0], 0x40, (float)yaw_deg);
    put_d(w[1], 0x88, -pitch_deg * kD2R);
    // A matrix-like slot (sin of yaw) that tracks the camera but is not an
    // angle in either unit: must never survive.
    put_f(w[0], 0x100, (float)std::sin(yaw_deg * kD2R));
    // A constant 30.0 that equals the yaw ONLY at the first sample.
    put_f(w[0], 0x180, 30.0f);
    return w;
}

int main() {
    // ---- the camera never leaves +-5 degrees: nothing may be reported ----
    {
        Scanner s;
        for (int i = 0; i < 200; i++) {
            const double y = 3.0 * std::sin(i * 0.1);
            s.observe(world(y, 1.0), y * kD2R, 1.0 * kD2R);
        }
        check(s.discoveries() == 0, "still camera: never discovers");
        check(s.survivors().empty(), "still camera: reports nothing");
        check(s.yaw_span_deg() < 7.0, "still camera: states its small range");
    }

    // ---- a sweep: the planted state must survive, decoys must not -------
    {
        Scanner s;
        int step = 0;
        for (double y = 30.0; y > -60.0; y -= 1.7, step++) {
            const double p = 20.0 * std::sin(step * 0.13);
            s.observe(world(y, p), y * kD2R, p * kD2R);
        }
        const auto sv = s.survivors();
        bool yaw_found = false, pitch_found = false, decoy = false;
        for (const auto &c : sv) {
            if (c.region == 0 && c.off == 0x40 && c.axis == 0 && c.deg == 1 &&
                c.sign == 1 && c.dbl == 0)
                yaw_found = true;
            else if (c.region == 1 && c.off == 0x88 && c.axis == 1 &&
                     c.deg == 0 && c.sign == -1 && c.dbl == 1)
                pitch_found = true;
            else
                decoy = true;
        }
        check(yaw_found, "sweep: finds yaw, float degrees, at cam+0x40");
        // Pitch is 0 at the first sample, when yaw is already 30: the case a
        // single shared discovery missed.
        check(pitch_found, "sweep: finds pitch, double radians negated, "
                           "although it was ~0 when yaw was discovered");
        check(!decoy, "sweep: no decoy survives (constant, sin, coincidence)");
        if (!sv.empty())
            printf("      top: %s\n", s.describe(sv[0]).c_str());
        check(s.describe(sv.empty() ? x4vr::camscan::Cand{} : sv[0])
                      .find("cam") != std::string::npos,
              "describe() names the path, not a heap address");
    }

    // ---- the state is somewhere the scan does not look -----------------
    {
        Scanner s;
        for (double y = -40.0; y < 40.0; y += 2.3) {
            std::vector<Region> w = world(0.0, 0.0);
            // Wipe the planted slots: the real state is elsewhere.
            put_f(w[0], 0x40, 0.0f);
            put_d(w[1], 0x88, 0.0);
            put_f(w[0], 0x100, (float)std::sin(y * kD2R));
            s.observe(w, y * kD2R, 0.0);
        }
        check(s.survivors().empty(), "state elsewhere: reports nothing");
        check(s.yaw_span_deg() > 70.0,
              "state elsewhere: yet says the camera DID sweep (a real null)");
    }

    // ---- unreadable regions count as neither hit nor miss --------------
    {
        Scanner s;
        int step = 0;
        for (double y = 20.0; y < 80.0; y += 1.3, step++) {
            std::vector<Region> w = world(y, 0.0);
            if (step % 5 == 3)
                w[0].bytes.clear(); // a read that failed this sample
            s.observe(w, y * kD2R, 0.0);
        }
        bool yaw_found = false;
        for (const auto &c : s.survivors())
            if (c.region == 0 && c.off == 0x40 && c.axis == 0)
                yaw_found = true;
        check(yaw_found, "failed reads: the real slot survives them");
    }

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
