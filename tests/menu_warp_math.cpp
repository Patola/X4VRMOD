// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The menu warp's arithmetic (layer/x4vr_menu_warp.hpp), with no GPU.
//
// The oracle is plain trigonometry written out here, not the header's own
// quaternion code: a head turned right by t looks along a direction whose
// tangent in X4's view is tan(t). The negative control swaps the two poses and
// must land on the OTHER side, so a matrix built the wrong way round fails
// rather than passing as a symmetric picture.
//
// Given the .spv files as arguments, it also checks that the embedded shaders
// are those files, so the layer cannot draw yesterday's shader.
#define X4VR_LOG_TAG "test"
#include "../layer/x4vr_menu_warp.hpp"

#include <cstdio>
#include <vector>

using namespace x4vr;

static int g_bad = 0;

static void ok(const char *what, bool cond) {
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        g_bad++;
}

static void near(const char *what, double got, double want, double tol) {
    const bool good = std::fabs(got - want) <= tol;
    printf("%-4s %s (got %.5f want %.5f)\n", good ? "ok" : "FAIL", what, got,
           want);
    if (!good)
        g_bad++;
}

// A turn about the view's up axis. Positive turns forward (-Z) towards -X, so
// a head turned RIGHT by t is yaw(-t).
static void yaw(float deg, float q[4]) {
    const float h = 0.5f * deg / 57.2957795f;
    q[0] = 0.f;
    q[1] = std::sin(h);
    q[2] = 0.f;
    q[3] = std::cos(h);
}

// About the view's right axis: positive turns forward towards +Y, up.
static void pitch(float deg, float q[4]) {
    const float h = 0.5f * deg / 57.2957795f;
    q[0] = std::sin(h);
    q[1] = 0.f;
    q[2] = 0.f;
    q[3] = std::cos(h);
}

static bool same_words(const char *path, const uint32_t *words, size_t bytes) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    std::vector<uint32_t> v(bytes / 4 + 1);
    const size_t n = fread(v.data(), 1, v.size() * 4, f);
    fclose(f);
    return n == bytes && std::memcmp(v.data(), words, bytes) == 0;
}

int main(int argc, char **argv) {
    const float k = 57.2957795f;
    const WarpTan field = warp_tan(-55.f / k, 55.f / k, 55.f / k, -55.f / k);
    const float W = 4000.f, H = 4000.f;
    const float id[4] = {0, 0, 0, 1};

    // ---- no turn: every pixel is itself -----------------------------------
    {
        float m[9];
        warp_matrix(id, id, m);
        double worst = 0;
        for (float y = 0.5f; y < H; y += 333.f)
            for (float x = 0.5f; x < W; x += 333.f) {
                float sx = 0, sy = 0;
                if (!warp_source_px(m, field, field, W, H, W, H, x, y, &sx, &sy))
                    worst = 1e9;
                worst = std::fmax(worst, std::fmax(std::fabs(sx - x), std::fabs(sy - y)));
            }
        ok("unturned, every sampled pixel lands on itself within 0.001 px",
           worst < 1e-3);
        near("unturned, the turn is 0 deg", warp_angle_deg(id, id), 0.0, 1e-3);
    }

    // ---- the head turned right by 30 deg, X4 rendered ahead ----------------
    {
        float shown[4];
        yaw(-30.f, shown);
        float m[9];
        warp_matrix(id, shown, m);
        float sx = 0, sy = 0;
        const bool in = warp_source_px(m, field, field, W, H, W, H, W / 2, H / 2,
                                       &sx, &sy);
        ok("turned right 30: the middle of the view is in X4's picture", in);
        const double want =
            (std::tan(30.0 / k) - field.l) / (field.r - field.l) * W;
        near("turned right 30: the middle of the view is tan(30) right in X4's",
             sx, want, 0.05);
        near("turned right 30: and at X4's middle height", sy, H / 2, 0.05);
        ok("turned right 30: that is RIGHT of X4's middle", sx > W / 2 + 100);
        near("turned right 30: the turn is 30 deg", warp_angle_deg(id, shown), 30.0,
             1e-2);

        // The negative control: the poses the wrong way round.
        float mw[9];
        warp_matrix(shown, id, mw);
        float wx = 0, wy = 0;
        warp_source_px(mw, field, field, W, H, W, H, W / 2, H / 2, &wx, &wy);
        ok("swapped poses land LEFT of the middle — the order is not symmetric",
           wx < W / 2 - 100);
    }

    // ---- pitch up 20: the middle of the view is above X4's -------------------
    {
        float shown[4];
        pitch(20.f, shown);
        float m[9];
        warp_matrix(id, shown, m);
        float sx = 0, sy = 0;
        warp_source_px(m, field, field, W, H, W, H, W / 2, H / 2, &sx, &sy);
        const double want = (field.u - std::tan(20.0 / k)) / (field.u - field.d) * H;
        near("pitched up 20: the middle of the view is tan(20) up in X4's", sy,
             want, 0.05);
        ok("pitched up 20: that is ABOVE X4's middle (smaller row)", sy < H / 2 - 100);
        near("pitched up 20: and at X4's middle column", sx, W / 2, 0.05);
    }

    // ---- X4 rendered turned too: only the difference counts ----------------
    {
        float rendered[4], shown[4], rel[4];
        yaw(-10.f, rendered);
        yaw(-40.f, shown);
        yaw(-30.f, rel);
        float a[9], b[9];
        warp_matrix(rendered, shown, a);
        warp_matrix(id, rel, b);
        double worst = 0;
        for (int i = 0; i < 9; i++)
            worst = std::fmax(worst, std::fabs(a[i] - b[i]));
        ok("rendered 10 right, shown 40 right == rendered ahead, shown 30 right",
           worst < 1e-5);
    }

    // ---- behind X4's eye ---------------------------------------------------
    {
        float shown[4];
        yaw(180.f, shown);
        float m[9];
        warp_matrix(id, shown, m);
        float sx = 0, sy = 0;
        ok("turned round: the middle of the view is behind X4's eye",
           !warp_source_px(m, field, field, W, H, W, H, W / 2, H / 2, &sx, &sy));
    }

    // ---- an asymmetric field and a smaller target --------------------------
    {
        const WarpTan src = warp_tan(-54.f / k, 40.f / k, 44.f / k, -55.f / k);
        float m[9];
        warp_matrix(id, id, m);
        float sx = 0, sy = 0;
        // The target is half the size over the same field: a pixel at (x, y)
        // of the target is at (2x, 2y) in the source.
        warp_source_px(m, src, src, W / 2, H / 2, W, H, 1000.f, 700.f, &sx, &sy);
        near("same field, half the pixels: x scales by 2", sx, 2000.0, 1e-2);
        near("same field, half the pixels: y scales by 2", sy, 1400.0, 1e-2);
    }

    // ---- bytes in, bytes out -------------------------------------------------
    ok("UNORM eye into an sRGB swapchain: decode, the view encodes",
       warp_flags(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB) ==
           kWarpDecodeSource);
    ok("UNORM into UNORM: decode and encode",
       warp_flags(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM) ==
           (kWarpDecodeSource | kWarpEncodeResult));
    ok("sRGB into sRGB: both views do it",
       warp_flags(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB) == 0);

    // ---- the push block ----------------------------------------------------
    {
        float m[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        const WarpPush p = warp_push(m, field, field, {4000, 3000}, {2000, 1000},
                                     1, kWarpDecodeSource);
        ok("push: M's rows, the layer in m0.w and the flags in m1.w",
           p.m0[0] == 1 && p.m0[2] == 3 && p.m1[1] == 5 && p.m2[2] == 9 &&
               p.m0[3] == 1.f && p.m1[3] == 1.f);
        ok("push: target then source extents",
           p.ext[0] == 4000 && p.ext[1] == 3000 && p.ext[2] == 2000 &&
               p.ext[3] == 1000);
    }

    if (argc >= 3) {
        ok("the embedded vertex shader is layer/menu_warp.vert.spv",
           same_words(argv[1], kMenuWarpVert, sizeof kMenuWarpVert));
        ok("the embedded fragment shader is layer/menu_warp.frag.spv",
           same_words(argv[2], kMenuWarpFrag, sizeof kMenuWarpFrag));
    } else {
        printf("skip the .spv comparison: pass the two .spv paths\n");
    }

    printf("%s\n", g_bad ? "FAILED" : "all passed");
    return g_bad ? 1 : 0;
}
