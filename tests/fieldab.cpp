// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_FIELD_AB, checked without X4: the effective-field sites against the
// real binary, the phase targets and their sx, what each tick writes, and the
// game-clock gate that keeps phases off the load screen (take 322 armed at
// 45% loaded). Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../injector/x4vr_fieldab.hpp"
#include "x4_path.hpp"

using namespace x4vr::fieldab;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

struct Elf {
    std::vector<uint8_t> d;
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        d.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        const bool ok = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        return ok && d.size() > 64;
    }
    bool read(uintptr_t va, void *out, size_t n) const {
        uint64_t phoff;
        uint16_t phentsize, phnum;
        memcpy(&phoff, &d[0x20], 8);
        memcpy(&phentsize, &d[0x36], 2);
        memcpy(&phnum, &d[0x38], 2);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint8_t *p = &d[phoff + (size_t)i * phentsize];
            uint32_t type;
            uint64_t off, vaddr, filesz;
            memcpy(&type, p, 4);
            memcpy(&off, p + 8, 8);
            memcpy(&vaddr, p + 16, 8);
            memcpy(&filesz, p + 32, 8);
            if (type == 1 && va >= vaddr && va + n <= vaddr + filesz) {
                memcpy(out, &d[off + (va - vaddr)], n);
                return true;
            }
        }
        return false;
    }
};


int main() {
    // ---- the two fields and the sx the layer should read -----------------
    check(std::fabs(sx_of(1.4917f) - 0.70024f) < 1e-4f,
          "phase A (1.4917): sx 0.70024, the rendering slots' value");
    check(std::fabs(sx_of(1.0f) - 1.33333f) < 1e-4f,
          "option 1.0: sx 1.33333, the pinned slots' value");
    printf("      phase B (1.24): %.2f deg, sx %.5f\n",
           field_of(1.24f) * 57.29578f, sx_of(1.24f));
    check(std::fabs(field_of(1.24f) * 57.29578f - 91.44f) < 0.01f,
          "phase B (1.24): 91.44 deg");

    // ---- phases and writes ----------------------------------------------
    check(!phase_b(0.0, 45.0) && phase_b(45.5, 45.0) && !phase_b(90.5, 45.0),
          "A first (X4 as configured), then B, alternating");
    Action a = decide(field_of(1.4917f), 1.4917f, 1.24f, false);
    check(!a.write, "phase A on X4's own field: nothing to write");
    a = decide(field_of(1.4917f), 1.4917f, 1.24f, true);
    check(a.write && std::fabs(a.value - field_of(1.24f)) < 1e-6f,
          "phase B: the narrow field written");
    a = decide(field_of(1.24f), 1.4917f, 1.24f, false);
    check(a.write && std::fabs(a.value - field_of(1.4917f)) < 1e-6f,
          "back to A: X4's wide field restored");
    check(layout_ok(field_of(1.4917f), 1.4917f) && !layout_ok(1.2870022f, 1.4917f),
          "arming needs the camera at option x default, not the bare default");

    // ---- the game clock -------------------------------------------------
    {
        GameClock c;
        double wall = 0.0;
        bool ran = false;
        // Load screen: game time frozen for 30 s of wall time.
        for (int i = 0; i < 300; i++, wall += 0.1)
            ran |= game_clock_step(c, 12.0, wall, 5.0, 1.0);
        check(!ran, "frozen game time (a load screen) never reads as running");
        // Play: game time advances with wall time, frame-stepped.
        double g = 12.0;
        int to_run = -1;
        for (int i = 0; i < 100; i++, wall += 0.1) {
            if (i % 2 == 0)
                g += 0.2; // steps every other tick, like a frame clock
            if (game_clock_step(c, g, wall, 5.0, 1.0) && to_run < 0)
                to_run = i;
        }
        check(to_run >= 45 && to_run <= 55,
              "running after ~5 s of play, not before");
        // Pause: frozen again past the gap -> stopped.
        bool still = true;
        for (int i = 0; i < 20; i++, wall += 0.1)
            still = game_clock_step(c, g, wall, 5.0, 1.0);
        check(!still, "a pause past the 1 s gap stops it");
        // A new game: the clock goes backwards; it must count from zero again.
        game_clock_step(c, 1.0, wall, 5.0, 1.0);
        wall += 0.1;
        check(!game_clock_step(c, 1.2, wall, 5.0, 1.0),
              "a clock that went backwards (a load) starts over");
    }

    // ---- the binary -------------------------------------------------------
    const char *x4 = x4_test_binary();
    Elf elf;
    if (!elf.load(x4)) {
        printf("skip  effective-field sites — no X4 at %s\n", x4);
    } else {
        auto rd = [&](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
        uintptr_t bad = 0;
        check(verify_sites(rd, &bad), "every effective-field site matches X4 9.00");
        auto bent = [&](uintptr_t va, void *o, size_t n) {
            if (!elf.read(va, o, n))
                return false;
            if (va == 0x1926fb9)
                ((uint8_t *)o)[4] ^= 0x04; // +0x874 -> +0x870
            return true;
        };
        bad = 0;
        check(!verify_sites(bent, &bad) && bad == 0x1926fb9,
              "one changed byte refuses, and names the site");
    }
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
