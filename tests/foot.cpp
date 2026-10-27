// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_FOOTLOG, checked without X4: the angles read back from X4's matrix
// convention (camwrite::rows_for, which take 319's matrices fixed), a rotation
// yaw/pitch cannot see (roll) still counted, freed memory refused, the window's
// net/travel across the +-180 seam, a camera change not read as one huge step,
// and the window line's shape that score_run.py parses. Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../injector/x4vr_camwrite.hpp"
#include "../injector/x4vr_foot.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// A full matrix (translation + 3 rows) in X4's convention.
static void mat(float yaw, float pitch, float x, float y, float z, float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = x;
    m[1] = y;
    m[2] = z;
    camwrite::rows_for(yaw, pitch, m + 4);
}

// --emit <scenario>: a census log made by the real accumulation and the real
// formatters, for tests/run-census-scorer.sh to plant -- so the scorer's
// regex is checked against what the injector prints, not a hand copy.
struct Sim {
    double t = 999000.0;
    foot::Sample s, prev;
    bool have_prev = false;
    foot::Window w;
    float ly = 0, lp = 0, by = 0, bp = 0, ry = 0, rp = 0, hy = 0, hp = 0;
    void line(const char *body) { printf("[%12.3f] inject  %s\n", t, body); }
    // One second of 20 samples: per-second increments of each angle.
    void second(double mdx, double mdy, float dhy, float dhp, float dly,
                float dlp, float dby, float dbp, float dry, float drp) {
        for (int i = 0; i < 20; i++) {
            t += 0.05;
            hy += dhy / 20; hp += dhp / 20;
            ly += dly / 20; lp += dlp / 20;
            by += dby / 20; bp += dbp / 20;
            ry += dry / 20; rp += drp / 20;
            s.local_ok = s.base_ok = s.rot_ok = s.head_ok = true;
            mat(ly, lp, 0.f, 0.f, 0.f, s.local);
            mat(by, bp, 2.f, 1.f, -3.f, s.base);
            s.rot_yaw = foot::wrap180(ry);
            s.rot_pitch = rp;
            s.head_yaw = hy;
            s.head_pitch = hp;
            if (!have_prev || !foot::same_regime(prev, s)) {
                char b[768];
                foot::format_regime(b, sizeof b, s);
                line(b);
            }
            prev = s;
            have_prev = true;
            w.add(t, s);
        }
        w.mouse_dx = mdx;
        w.mouse_dy = mdy;
        w.mouse_n = (mdx != 0.0 || mdy != 0.0) ? 30 : 0;
        char b[1024];
        foot::format_window(b, sizeof b, w, 1.0);
        line(b);
        w.restart(t);
    }
};

static int emit(const char *scenario) {
    Sim m;
    m.line("foot: census ON — read only; a transition line per camera regime, "
           "a window line per second");
    // Seated: the head turns and head-look's write carries the local matrix
    // and GetCameraRotation with it; the cockpit pointer moves nothing.
    m.s.cam = 0x7f0000001000;
    m.s.attached = 0x7f0000002000;
    m.s.attached_vt = 0x3b80000;
    m.s.mode = 0;
    m.s.piloting = m.s.occupied = true;
    m.s.hud = true;
    const float head[] = {8, -12, 6, -9, 14, -7};
    for (float d : head)
        m.second(0, 0, d, 0, d, 0, 0, 0, d, 0);
    const double cur[] = {200, -150, 90, -300};
    for (double d : cur)
        m.second(d, d / 2, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!strcmp(scenario, "seated"))
        return 0;
    // Off the seat: another mode, another attached object.
    m.s.mode = 1;
    m.s.piloting = false;
    m.s.attached = 0x7f0000003000;
    m.s.attached_vt = 0x3c00000;
    m.s.relative = true;
    const bool moves = !strcmp(scenario, "follows");
    const double dx[] = {300, -500, 150, -200, 400, -100, 250, -350};
    for (double d : dx) {
        const float yaw = moves ? (float)(-0.1 * d) : 0.f;
        m.second(d, 0, 0, 0, 0, 0, yaw, 0, yaw, 0);
    }
    const double dy[] = {100, -200, 150, -50, 80, -120};
    for (double d : dy) {
        const float pitch = moves ? (float)(-0.05 * d) : 0.f;
        m.second(0, d, 0, 0, 0, pitch, 0, 0, 0, pitch);
    }
    const float turn[] = {10, -14, 9, -11, 12, -8};
    for (float d : turn)
        m.second(0, 0, d, d / 3, 0, 0, 0, 0, 0, 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--emit"))
        return emit(argv[2]);
    // 1. The inverse of rows_for, over the range head-look writes.
    {
        float worst = 0.f;
        bool all = true;
        for (float y = -179.f; y <= 179.f; y += 7.f)
            for (float p = -75.f; p <= 75.f; p += 5.f) {
                float r[12];
                camwrite::rows_for(y, p, r);
                float yy = 0.f, pp = 0.f;
                all = all && foot::head_angles_of_rows(r, &yy, &pp);
                worst = std::fmax(worst, std::fabs(foot::wrap180(yy - y)));
                worst = std::fmax(worst, std::fabs(pp - p));
            }
        char what[128];
        snprintf(what, sizeof what,
                 "angles read back from rows_for over yaw +-179, pitch +-75 "
                 "(worst %.5f deg)", worst);
        check(all && worst < 1e-3f, what);
    }
    // 2. Not a unit forward axis: refused, not read as an angle.
    {
        float r[12] = {};
        float y = 0.f, p = 0.f;
        check(!foot::head_angles_of_rows(r, &y, &p), "all-zero rows (freed) refused");
        camwrite::rows_for(20.f, 10.f, r);
        r[8] *= 1.5f;
        check(!foot::head_angles_of_rows(r, &y, &p), "a stretched forward axis refused");
    }
    // 3. delta_angle_deg: a yaw step, and a ROLL that yaw/pitch cannot see.
    {
        float a[12], b[12];
        camwrite::rows_for(10.f, 0.f, a);
        camwrite::rows_for(25.f, 0.f, b);
        check(near(foot::delta_angle_deg(a, b), 15.f, 1e-2f), "a 15 deg yaw step reads 15");
        check(near(foot::delta_angle_deg(a, a), 0.f, 0.05f), "no motion reads ~0");
        // Roll by 30 deg about the forward (row 2) axis: rows 0 and 1 turn,
        // row 2 stays -- so yaw/pitch read no motion at all.
        float c[12];
        memcpy(c, a, sizeof c);
        const float cr = std::cos(30.f * 0.0174533f), sr = std::sin(30.f * 0.0174533f);
        for (int k = 0; k < 3; k++) {
            c[0 + k] = cr * a[0 + k] + sr * a[4 + k];
            c[4 + k] = -sr * a[0 + k] + cr * a[4 + k];
        }
        float y0, p0, y1, p1;
        foot::head_angles_of_rows(a, &y0, &p0);
        foot::head_angles_of_rows(c, &y1, &p1);
        check(near(y0, y1, 1e-3f) && near(p0, p1, 1e-3f),
              "control: a pure roll leaves yaw/pitch unchanged");
        check(near(foot::delta_angle_deg(a, c), 30.f, 0.05f),
              "and the geodesic angle still reads it (30 deg)");
    }
    // 4. A window: local yaw ramps 170 -> -170 through the seam, base static,
    //    the head turns left 12 deg. Net local yaw +20, not -340.
    {
        foot::Window w;
        double t = 100.0;
        for (int i = 0; i <= 20; i++, t += 0.05) {
            foot::Sample s;
            s.cam = 0x1000;
            s.mode = 1;
            s.local_ok = s.base_ok = s.head_ok = s.rot_ok = true;
            mat(foot::wrap180(170.f + (float)i), 5.f, 0.f, 1.7f, 0.f, s.local);
            mat(0.f, 0.f, 3.f, 0.f, -2.f, s.base);
            s.head_yaw = 0.6f * (float)i;
            s.head_pitch = 0.f;
            s.rot_yaw = foot::wrap180(170.f + (float)i);
            s.rot_pitch = 5.f;
            w.add(t, s);
        }
        check(near(w.local.ang.net_yaw, 20.f, 1e-2f),
              "local yaw net +20 across the +-180 seam (not -340)");
        check(near(w.local.ang.travel_yaw, 20.f, 1e-2f), "local yaw travel 20");
        check(w.local.moved == 20, "local moved in all 20 steps");
        check(w.base.moved == 0 && near(w.base.rot_travel, 0.f, 0.05f) &&
                  near(w.base.pos_max, 0.f, 1e-6f),
              "control: the static base reads no rotation and no displacement");
        check(near(w.head.net_yaw, 12.f, 1e-3f), "head net yaw 12");
        check(near(w.rot.net_yaw, 20.f, 1e-2f), "GetCameraRotation net yaw 20");

        // The line, and what score_run.py's regex keys on.
        w.mouse_n = 42;
        w.mouse_dx = -350.0;
        w.mouse_dy = 12.0;
        char line[1024];
        foot::format_window(line, sizeof line, w, 1.0);
        printf("       %s\n", line);
        check(strstr(line, "foot: window 1.00 s n 21 mode 1 piloting 0 occupied 0 "
                           "view cockpit hud 0 rel 0 | mouse n 42 dx -350 dy 12 |") != nullptr,
              "window line head (mode, flags, mouse)");
        check(strstr(line, "| local net 20.00 0.00 travel 20.00 0.00 at -170.0 5.0 rot ") != nullptr,
              "window line's local column");
        check(strstr(line, "moved 0 pos 0.000") != nullptr, "window line's base column");

        // A restart keeps the rows: the next window's first step is counted.
        w.restart(t);
        foot::Sample s;
        s.cam = 0x1000;
        s.mode = 1;
        s.local_ok = true;
        mat(-169.f, 5.f, 0.f, 1.7f, 0.f, s.local);
        w.add(t + 0.05, s);
        check(w.local.moved == 1 && near(w.local.ang.net_yaw, 1.f, 1e-2f),
              "after a restart the first step from the last window is counted");
    }
    // 5. A camera change: the matrices are forgotten, not differenced.
    {
        foot::Window w;
        foot::Sample s;
        s.cam = 0x1000;
        s.local_ok = true;
        mat(0.f, 0.f, 0.f, 0.f, 0.f, s.local);
        w.add(1.0, s);
        w.forget_matrices();
        s.cam = 0x2000;
        mat(90.f, 0.f, 5.f, 0.f, 0.f, s.local);
        w.add(1.05, s);
        check(w.local.moved == 0 && near(w.local.ang.net_yaw, 0.f, 1e-6f) &&
                  near(w.local.pos_max, 0.f, 1e-6f),
              "a new camera is not read as a 90 deg step and a 5 m jump");
    }
    // 6. Regimes.
    {
        foot::Sample a, b;
        a.cam = b.cam = 0x1000;
        a.mode = b.mode = 0;
        a.piloting = b.piloting = true;
        b.rot_yaw = 40.f;
        b.view = 1;
        check(foot::same_regime(a, b), "a turned camera or another view is the same regime");
        b.piloting = false;
        check(!foot::same_regime(a, b), "leaving the seat is a new regime");
        b = a;
        b.mode = 1;
        check(!foot::same_regime(a, b), "a mode change is a new regime");
    }
    printf("\n%s\n", g_fail ? "SOME CASES FAILED" : "all cases passed");
    return g_fail ? 1 : 0;
}
