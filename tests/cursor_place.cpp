// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Where the cursor goes, and what its bytes mean. Neither needs a GPU.
//
// These two are worth a test because both failures are invisible in review and
// unmistakable on screen, and because each would cost a run of X4 to find:
//
//   * the format mapping decides whether X4's blue cross comes out blue or
//     orange -- SDL names packed formats MSB-first and Vulkan names them in
//     memory order, so the two spellings of the same bytes look like opposites;
//   * the hot spot decides whether the pointer points at what X4 hit-tests.
//     Take 94 saw four different hot spots on four cursors, up to (12,19), so
//     ignoring it misses by up to 31 px -- far enough to click the wrong thing,
//     close enough to look approximately right.
//
// Self-checking like view_math: no arguments, non-zero on the first bad case.
// The embedded shaders are checked here too, because a stale generated header
// is exactly the sort of thing that builds and links and then draws nothing.
// The overlay header logs, so it needs a tag like its two real consumers do.
// Nothing under test here logs, but the macro has to resolve to compile.
#define X4VR_LOG_TAG "test"
#include "../common/x4vr_env.hpp"
#include "../layer/x4vr_cursor_draw.hpp"

using namespace x4vr;

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_bad = 0;

static void ok(const char *what, bool cond) {
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        g_bad++;
}

static void near_eq(const char *what, float got, float want) {
    const bool good = std::fabs(got - want) < 1e-5f;
    printf("%-4s %s (got %f want %f)\n", good ? "ok" : "FAIL", what, got, want);
    if (!good)
        g_bad++;
}

static std::vector<uint32_t> load_spv(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return {};
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> v((size_t)n / 4);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n)
        v.clear();
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    // ---- the pixel format -------------------------------------------------
    // 0x16362004 is what take 94 read off the channel, and what SDL3's own
    // header prints for SDL_PIXELFORMAT_ARGB8888. Its memory order on a
    // little-endian machine is B,G,R,A.
    ok("ARGB8888 into a UNORM eye is B8G8R8A8_UNORM",
       cursor_pixel_format(0x16362004u, VK_FORMAT_B8G8R8A8_UNORM) ==
           VK_FORMAT_B8G8R8A8_UNORM);
    ok("ARGB8888 into an SRGB eye is B8G8R8A8_SRGB",
       cursor_pixel_format(0x16362004u, VK_FORMAT_B8G8R8A8_SRGB) ==
           VK_FORMAT_B8G8R8A8_SRGB);
    // The eye's *channel order* must not leak into the cursor's: the cursor's
    // bytes are whatever SDL wrote, no matter what the swapchain looks like.
    ok("an R8G8B8A8 eye does not re-order an ARGB8888 cursor",
       cursor_pixel_format(0x16362004u, VK_FORMAT_R8G8B8A8_UNORM) ==
           VK_FORMAT_B8G8R8A8_UNORM);
    ok("ABGR8888 into a UNORM eye is R8G8B8A8_UNORM",
       cursor_pixel_format(0x16762004u, VK_FORMAT_B8G8R8A8_UNORM) ==
           VK_FORMAT_R8G8B8A8_UNORM);
    // A refusal, not a guess. Drawing an unknown layout with swapped channels
    // would look like a bug in the capture rather than in the mapping.
    ok("an unknown SDL format is refused",
       cursor_pixel_format(0x12345678u, VK_FORMAT_B8G8R8A8_UNORM) ==
           VK_FORMAT_UNDEFINED);
    ok("sRGB detection covers both channel orders",
       cursor_is_srgb(VK_FORMAT_B8G8R8A8_SRGB) &&
           cursor_is_srgb(VK_FORMAT_R8G8B8A8_SRGB) &&
           !cursor_is_srgb(VK_FORMAT_B8G8R8A8_UNORM));

    // ---- placement --------------------------------------------------------
    // A 32x32 cursor with hot spot (0,0) at the exact centre of a 1408 eye.
    // Its top-left corner is the pointer, so the quad starts at the centre and
    // extends down and right.
    {
        const CursorRect r = cursor_rect(704.f, 704.f, 0, 0, 32, 32, 1408, 1408);
        ok("centre, hot (0,0): on screen", r.onscreen);
        near_eq("x0 is NDC 0 at the centre", r.x0, 0.f);
        near_eq("y0 is NDC 0 at the centre", r.y0, 0.f);
        near_eq("x1 is 32px right of centre", r.x1, 2.f * 736.f / 1408.f - 1.f);
    }
    // The same pointer position with take 94's (12,19) hot spot. The *pointer*
    // has not moved; the bitmap has, by exactly the hot spot.
    {
        const CursorRect a = cursor_rect(704.f, 704.f, 0, 0, 32, 32, 1408, 1408);
        const CursorRect b =
            cursor_rect(704.f, 704.f, 12, 19, 32, 32, 1408, 1408);
        near_eq("hot x shifts the quad left by 12 px", b.x0 - a.x0,
                -2.f * 12.f / 1408.f);
        near_eq("hot y shifts the quad up by 19 px", b.y0 - a.y0,
                -2.f * 19.f / 1408.f);
        near_eq("the quad keeps its width", (b.x1 - b.x0) - (a.x1 - a.x0), 0.f);
    }
    // Corners map to the NDC corners. This is the check that would catch a
    // y-flip: Vulkan's NDC y and the framebuffer's both run downward, so
    // y=0 is -1 and y=height is +1.
    {
        const CursorRect r = cursor_rect(0.f, 0.f, 0, 0, 32, 32, 1408, 1408);
        near_eq("top-left x is -1", r.x0, -1.f);
        near_eq("top-left y is -1 (no flip)", r.y0, -1.f);
    }
    // Not square, and not 1408. The eye is whatever the headset asks for.
    {
        const CursorRect r = cursor_rect(960.f, 540.f, 16, 16, 32, 32, 1920, 1080);
        ok("a 1920x1080 eye places on screen", r.onscreen);
        near_eq("x0 on a 1920-wide eye", r.x0, 2.f * (960.f - 16.f) / 1920.f - 1.f);
        near_eq("y0 on a 1080-tall eye", r.y0, 2.f * (540.f - 16.f) / 1080.f - 1.f);
    }
    // Off-screen is refused rather than drawn clipped, so the frame pays
    // nothing for a pointer that is not in it.
    {
        ok("wholly past the right edge is refused",
           !cursor_rect(1500.f, 700.f, 0, 0, 32, 32, 1408, 1408).onscreen);
        ok("wholly above the top edge is refused",
           !cursor_rect(700.f, -40.f, 0, 0, 32, 32, 1408, 1408).onscreen);
        ok("straddling the left edge still draws",
           cursor_rect(4.f, 700.f, 20, 0, 32, 32, 1408, 1408).onscreen);
        ok("a zero-sized eye is refused, not divided by",
           !cursor_rect(0.f, 0.f, 0, 0, 32, 32, 0, 0).onscreen);
        ok("a zero-sized cursor is refused",
           !cursor_rect(700.f, 700.f, 0, 0, 0, 0, 1408, 1408).onscreen);
    }

    // ---- knob semantics ---------------------------------------------------
    // The injector decides whether to hide the compositor's pointer by reading
    // the same variables the layer reads. If the two ever disagree about what
    // "on" means, a run configured one way behaves the other way in one of the
    // components -- so the rule is pinned here rather than trusted to two
    // matching hand-written expressions.
    unsetenv("X4VR_TEST_KNOB");
    ok("unset takes the caller's default (on)",
       env_on("X4VR_TEST_KNOB", true));
    ok("unset takes the caller's default (off)",
       !env_on("X4VR_TEST_KNOB", false));
    setenv("X4VR_TEST_KNOB", "", 1);
    ok("empty is treated as unset", env_on("X4VR_TEST_KNOB", true) &&
                                        !env_on("X4VR_TEST_KNOB", false));
    setenv("X4VR_TEST_KNOB", "0", 1);
    ok("\"0\" is off whatever the default",
       !env_on("X4VR_TEST_KNOB", true) && !env_on("X4VR_TEST_KNOB", false));
    setenv("X4VR_TEST_KNOB", "1", 1);
    ok("\"1\" is on whatever the default",
       env_on("X4VR_TEST_KNOB", true) && env_on("X4VR_TEST_KNOB", false));
    setenv("X4VR_TEST_KNOB", "yes", 1);
    ok("anything not starting with 0 is on", env_on("X4VR_TEST_KNOB", false));
    unsetenv("X4VR_TEST_KNOB");

    // ---- the embedded shaders --------------------------------------------
    ok("the vertex module carries the SPIR-V magic",
       sizeof(kCursorQuadVert) > 20 && kCursorQuadVert[0] == 0x07230203u);
    ok("the fragment module carries the SPIR-V magic",
       sizeof(kCursorQuadFrag) > 20 && kCursorQuadFrag[0] == 0x07230203u);
    // The generated header must still match the .spv it was generated from,
    // or the layer draws yesterday's shader. Skipped rather than failed when
    // the files are not passed, so the binary stays runnable on its own.
    if (argc > 2) {
        const std::vector<uint32_t> v = load_spv(argv[1]), f = load_spv(argv[2]);
        ok("cursor_shaders.hpp matches cursor_quad.vert.spv",
           !v.empty() && v.size() * 4 == sizeof(kCursorQuadVert) &&
               memcmp(v.data(), kCursorQuadVert, sizeof(kCursorQuadVert)) == 0);
        ok("cursor_shaders.hpp matches cursor_quad.frag.spv",
           !f.empty() && f.size() * 4 == sizeof(kCursorQuadFrag) &&
               memcmp(f.data(), kCursorQuadFrag, sizeof(kCursorQuadFrag)) == 0);
    } else {
        printf("skip the generated header is not compared (pass the two .spv "
               "paths to compare)\n");
    }

    printf("\n%s\n", g_bad ? "SOME CASES FAILED" : "all cases passed");
    return g_bad ? 1 : 0;
}
