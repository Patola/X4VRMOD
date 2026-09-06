// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The dump writers' pixel transforms, rewritten for speed — so the only thing
// that matters is that the BYTES DID NOT CHANGE.
//
// Take 259 measured the flush at 2378 ms/frame with OTHER = 0: eye write 1098,
// coverage 1076, ui write 203. Both hot terms read the write-combined readback
// buffer one byte at a time. The fix copies each layer into RAM once and does
// the per-pixel work there, and folds the UI coverage count into the pass that
// was already happening.
//
// **A performance change that alters output is a correctness bug wearing a
// stopwatch.** So every case here runs the ORIGINAL per-pixel code, kept below
// verbatim as `ref_*`, and requires the new implementation to agree byte for
// byte and count for count. The reference is the old code rather than a fresh
// derivation on purpose: the claim under test is "identical to what take 259
// produced", not "correct in the abstract".
#include "../common/x4vr_dumpio.hpp"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

static int fails = 0, checks = 0;

static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

// ---- the code as it stood before, verbatim from layer/x4vr_layer.cpp ------
static void ref_rgb(std::vector<uint8_t> &rgb, const uint8_t *px, uint32_t w,
                    uint32_t h, bool bgra) {
    rgb.assign((size_t)w * h * 3, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            for (uint32_t c = 0; c < 3; c++)
                rgb[((size_t)y * w + x) * 3 + c] = bgra ? p[2 - c] : p[c];
        }
}

static void ref_rgba(std::vector<uint8_t> &rgba, const uint8_t *px, uint32_t w,
                     uint32_t h, bool bgra) {
    rgba.assign((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            uint8_t *o = &rgba[((size_t)y * w + x) * 4];
            for (uint32_t c = 0; c < 3; c++)
                o[c] = bgra ? p[2 - c] : p[c];
            o[3] = p[3];
        }
}

// The coverage loop the flush used to run as a second pass.
static void ref_cov(const uint8_t *src, uint32_t w, uint32_t h, uint64_t *nz,
                    uint64_t *na) {
    *nz = *na = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *q = src + i * 4;
        if (q[0] | q[1] | q[2])
            (*nz)++;
        if (q[3])
            (*na)++;
    }
}

int main() {
    // Sizes chosen to catch an index that assumes a square or a power of two.
    // 1408x1408 is the real one; the rest are awkward on purpose.
    const uint32_t sizes[][2] = {{1408, 1408}, {1, 1},   {1, 7},
                                 {7, 1},       {13, 5},  {64, 63},
                                 {255, 3}};
    unsigned seed = 20260906u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (uint8_t)(seed >> 24);
    };

    for (bool bgra : {false, true}) {
        for (const auto &wh : sizes) {
            const uint32_t w = wh[0], h = wh[1];
            std::vector<uint8_t> px((size_t)w * h * 4);
            // Mixed content: random, plus deliberate zeros so the coverage
            // criteria are actually exercised in both directions.
            for (size_t i = 0; i < px.size(); i++)
                px[i] = rnd();
            for (size_t i = 0; i < px.size(); i += 4)
                if ((i / 4) % 3 == 0)
                    px[i] = px[i + 1] = px[i + 2] = 0;   // colour-blank pixel
            for (size_t i = 3; i < px.size(); i += 4)
                if ((i / 4) % 5 == 0)
                    px[i] = 0;                            // alpha-blank pixel

            char tag[128];

            // --- 4ch -> 3ch --------------------------------------------
            std::vector<uint8_t> a, b, src;
            ref_rgb(a, px.data(), w, h, bgra);
            x4vr::dump::snapshot(src, px.data(), px.size());
            x4vr::dump::to_rgb(b, src.data(), w, h, bgra);
            snprintf(tag, sizeof tag, "to_rgb %ux%u bgra=%d identical", w, h,
                     (int)bgra);
            ok(a == b, tag);

            // --- 4ch -> 4ch + coverage in one pass ----------------------
            std::vector<uint8_t> c, d;
            ref_rgba(c, px.data(), w, h, bgra);
            const x4vr::dump::Coverage got =
                x4vr::dump::to_rgba_counted(d, src.data(), w, h, bgra);
            snprintf(tag, sizeof tag, "to_rgba %ux%u bgra=%d identical", w, h,
                     (int)bgra);
            ok(c == d, tag);

            uint64_t nz = 0, na = 0;
            ref_cov(px.data(), w, h, &nz, &na);
            snprintf(tag, sizeof tag,
                     "coverage %ux%u bgra=%d colour %llu == %llu", w, h,
                     (int)bgra, (unsigned long long)got.colour,
                     (unsigned long long)nz);
            ok(got.colour == nz, tag);
            snprintf(tag, sizeof tag,
                     "coverage %ux%u bgra=%d alpha %llu == %llu", w, h,
                     (int)bgra, (unsigned long long)got.alpha,
                     (unsigned long long)na);
            ok(got.alpha == na, tag);

            // The counts must be over the SOURCE, so they cannot depend on
            // the channel order. A run's logged coverage would otherwise
            // change meaning between builds.
            std::vector<uint8_t> e;
            const x4vr::dump::Coverage flip =
                x4vr::dump::to_rgba_counted(e, src.data(), w, h, !bgra);
            snprintf(tag, sizeof tag,
                     "coverage %ux%u is order-independent", w, h);
            ok(flip.colour == got.colour && flip.alpha == got.alpha, tag);
        }
    }

    // --- the reused thread_local buffers must not leak between images -----
    // The writers keep their scratch vectors across calls. A smaller image
    // following a larger one must not inherit the tail of the larger, and a
    // larger one must not read a stale short buffer.
    {
        std::vector<uint8_t> scratch, out_big, out_small, ref;
        std::vector<uint8_t> big((size_t)64 * 64 * 4, 0xAB);
        std::vector<uint8_t> small((size_t)8 * 8 * 4, 0x11);
        x4vr::dump::snapshot(scratch, big.data(), big.size());
        x4vr::dump::to_rgb(out_big, scratch.data(), 64, 64, true);
        x4vr::dump::snapshot(scratch, small.data(), small.size());
        x4vr::dump::to_rgb(out_small, scratch.data(), 8, 8, true);
        ok(scratch.size() == small.size(),
           "snapshot shrinks to the new image, no stale tail");
        ref_rgb(ref, small.data(), 8, 8, true);
        ok(out_small == ref, "small image after a large one is correct");
        ok(out_small.size() == (size_t)8 * 8 * 3,
           "output resized down, not left long");
    }

    // --- degenerate inputs must not crash ---------------------------------
    {
        std::vector<uint8_t> s, o;
        x4vr::dump::snapshot(s, nullptr, 0);
        ok(s.empty(), "zero-byte snapshot is empty, not a crash");
        x4vr::dump::to_rgb(o, s.data(), 0, 0, true);
        ok(o.empty(), "zero-pixel to_rgb is empty");
        const x4vr::dump::Coverage z =
            x4vr::dump::to_rgba_counted(o, s.data(), 0, 0, true);
        ok(o.empty() && z.colour == 0 && z.alpha == 0,
           "zero-pixel to_rgba_counted is empty with zero coverage");
    }

    printf("%s  dumpio: %d check(s), %d failure(s)\n", fails ? "FAIL" : "ok  ",
           checks, fails);
    return fails ? 1 : 0;
}
