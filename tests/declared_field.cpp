// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// common/x4vr_field.hpp: the part of X4's picture a headset's own field falls on, and how the
// runtime's eyes stand to each other.
#include "../common/x4vr_field.hpp"

#include <cstdio>

static int fails;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}
static const float K = 0.01745329251994330f;

int main() {
    using namespace x4vr;
    const float half = 1.4917f * 73.7399f * 0.5f * K; // X4VR_FOV's 110 degrees
    auto rect = [&](float l, float r, float u, float d, int size = 2816, float h = 0.f) {
        const float v[4] = {l * K, r * K, u * K, d * K};
        return field_rect(h > 0.f ? h * K : half, size, size, v);
    };
    // The Reverb G2 through SteamVR (issue #3): inside on every side.
    const FieldRect g0 = rect(-52.67f, 41.47f, 46.32f, -46.28f);
    const FieldRect g1 = rect(-41.47f, 52.67f, 46.32f, -46.28f);
    check(g0.ok && g1.ok, "the Reverb G2's field lies inside X4's");
    check(g0.x == 115 && g0.x + g0.w == 2280, "...eye 0's columns");
    check(g1.x == 2816 - 2280 && g1.x + g1.w == 2816 - 115, "...eye 1's are its mirror image");
    check(g0.left <= -52.67f * K && g0.right >= 41.47f * K && g0.up >= 46.32f * K &&
              g0.down <= -46.28f * K,
          "...never less than the runtime asked for");
    check(std::fabs(g0.left + 52.67f * K) < 0.06f * K && std::fabs(g0.right - 41.47f * K) < 0.06f * K &&
              std::fabs(g0.up - 46.32f * K) < 0.06f * K,
          "...and no more than a pixel's worth past it");
    // The same direction lands on the same pixel either way: what changes is what's declared.
    {
        const float t = std::tan(half);
        auto full = [&](float deg) { return (std::tan(deg * K) + t) / (2.f * t) * 2816.f; };
        auto part = [&](float deg, const FieldRect &r) {
            const float a = std::tan(r.left), b = std::tan(r.right);
            return r.x + (std::tan(deg * K) - a) / (b - a) * r.w;
        };
        float worst = 0.f;
        for (float deg = -50.f; deg <= 41.f; deg += 1.f)
            worst = std::max(worst, std::fabs(full(deg) - part(deg, g0)));
        check(worst < 0.01f, "...every direction on the pixel X4 drew it at");
        // The control: the whole image taken for the headset's field, as a runtime that ignored
        // the declaration would show it: straight ahead lands 12 degrees out.
        const float a = std::tan(-52.67f * K), b = std::tan(41.47f * K);
        const float mid = std::atan((a + b) * 0.5f) / K;
        check(mid < -11.f && mid > -13.f, "...where ignoring it would put straight ahead 12 deg out");
    }
    // Quest 2 (issue #4) inside; the Quest 3 and the Steam Frame reach past X4's 110 degrees.
    check(rect(-52.f, 45.f, 48.f, -50.f).ok, "a narrower headset's field is covered");
    check(!rect(-59.09f, 50.47f, 49.85f, -59.73f).ok, "the Steam Frame's is not: declared as it was");
    check(rect(-59.09f, 50.47f, 49.85f, -59.73f, 2816, 66.37f).ok,
          "...until X4 draws wider (X4VR_FOV=1.80, as in take 458)");
    check(!rect(41.f, -52.f, 46.f, -46.f).ok && !rect(-52.f, 41.f, -46.f, 46.f).ok,
          "a field turned inside out is refused");
    check(!rect(-52.f, 41.f, 46.f, -46.f, 0).ok, "no image, no part of one");
    const FieldRect all = rect(-55.f, 55.f, 55.f, -55.f, 2816, 55.f);
    check(all.ok && all.x == 0 && all.y == 0 && all.w == 2816 && all.h == 2816,
          "X4's own field is the whole image");

    const float id[4] = {0.f, 0.f, 0.f, 1.f};
    const float l[3] = {-0.0317f, 1.1f, 0.2f}, r[3] = {0.0317f, 1.1f, 0.2f};
    EyePair p = eye_pair(id, id, l, r);
    check(p.deg < 1e-3f && std::fabs(p.at[0][0] + 0.0317f) < 1e-6f &&
              std::fabs(p.at[1][0] - 0.0317f) < 1e-6f && std::fabs(p.at[1][1]) < 1e-6f,
          "parallel eyes, 63.4 mm apart");
    // Eye 1 turned 10 degrees outwards (a yaw to the right is negative about +y).
    const float s = std::sin(-5.f * K), c = std::cos(-5.f * K);
    const float out[4] = {0.f, s, 0.f, c};
    p = eye_pair(id, out, l, r);
    check(std::fabs(p.deg - 10.f) < 1e-3f && std::fabs(p.yaw + 10.f) < 1e-3f &&
              std::fabs(p.pitch) < 1e-3f && std::fabs(p.roll) < 1e-3f,
          "canted eyes say so, and on which axis");
    // The head turned 90 degrees: positions come back in the head's own axes.
    const float sy = std::sin(45.f * K), cy = std::cos(45.f * K);
    const float q[4] = {0.f, sy, 0.f, cy};
    const float l2[3] = {0.f, 0.f, 0.0317f}, r2[3] = {0.f, 0.f, -0.0317f};
    p = eye_pair(q, q, l2, r2);
    check(p.deg < 1e-3f && std::fabs(p.at[0][0] + 0.0317f) < 1e-5f &&
              std::fabs(p.at[1][0] - 0.0317f) < 1e-5f && std::fabs(p.at[0][2]) < 1e-5f,
          "a turned head: still eye 0 on the left");
    p = eye_pair(q, q, r2, l2);
    check(p.at[0][0] > 0.03f, "...and swapped eyes show");

    printf(fails ? "\n%d check(s) failed\n" : "\nall passed\n", fails);
    return fails != 0;
}
