// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HEAD_6DOF's arithmetic (x4vr_head6.hpp): the room's axes to X4's and
// back, and the limit. Self-checking.
#include <cmath>
#include <cstdio>

#include "../common/x4vr_head6.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

int main() {
    {
        // Leaning forward in the room is -z (OpenXR); in X4 it is +z, the
        // way camwrite::rows_for's yaw 0 looks.
        const float lean_fwd[3] = {0.f, 0.f, -0.10f};
        float x[3];
        head6::to_x4(lean_fwd, x);
        check(x[0] == 0.f && x[1] == 0.f && x[2] == 0.10f,
              "a 10 cm lean forward -> X4 +z 0.10");
        const float right_up[3] = {0.05f, 0.03f, 0.f};
        head6::to_x4(right_up, x);
        check(x[0] == 0.05f && x[1] == 0.03f && x[2] == 0.f,
              "right and up keep their signs");
        float back[3];
        const float v[3] = {0.07f, -0.02f, 0.11f};
        head6::to_x4(v, x);
        head6::from_x4(x, back);
        check(back[0] == v[0] && back[1] == v[1] && back[2] == v[2],
              "from_x4 undoes to_x4 (what the layer declares is what it sent)");
    }
    {
        float v[3] = {0.3f, 0.4f, 0.f}; // 0.5 m
        head6::clamp_len(v, 0.35f);
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        check(std::fabs(l - 0.35f) < 1e-5f && std::fabs(v[0] / v[1] - 0.75f) < 1e-5f,
              "0.5 m held at 0.35 m, in the same direction");
        float w[3] = {0.1f, -0.1f, 0.05f};
        head6::clamp_len(w, 0.35f);
        check(w[0] == 0.1f && w[1] == -0.1f && w[2] == 0.05f,
              "inside the limit -> unchanged");
        float n[3] = {NAN, 0.1f, 0.1f};
        head6::clamp_len(n, 0.35f);
        check(n[0] == 0.f && n[1] == 0.f && n[2] == 0.f,
              "a lost pose (NaN) -> zero, never a jump");
        float z[3] = {0.1f, 0.1f, 0.1f};
        head6::clamp_len(z, 0.f);
        check(z[0] == 0.f && z[1] == 0.f && z[2] == 0.f, "limit 0 -> zero");
    }
    printf(g_fail ? "%d case(s) FAILED\n" : "all cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
