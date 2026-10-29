// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HEAD_6DOF's limit (x4vr_head6.hpp). Self-checking.
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
