// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_CSM_COVER, checked without X4: X4's cascade-fit formula against take
// 323's measured cascade-0 boxes, the divisor arithmetic, the four sites
// against the real binary, and the patch routine run for real -- X4's own
// instruction bytes executed on this CPU before and after their displacement
// is retargeted. Self-checking.
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../injector/x4vr_csmfit.hpp"
#include "x4_path.hpp"

using namespace x4vr::csmfit;

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

static const double kDefaultField = 1.2870022; // rad, X4's 73.74 deg
static double field_of(double option) { return option * kDefaultField; }

int main() {
    const double far0 = 3.0 * kFarScale;
    // ---- X4's formula reproduces take 323's cascade-0 boxes -------------
    struct {
        double option, measured;
        const char *what;
    } m[] = {{1.4917, 0.47213, "110 deg (phase A)"},
             {1.24, 0.38799, "91.44 deg (phase B)"},
             {1.0, 0.30841, "73.74 deg (the menu)"}};
    for (auto &x : m) {
        const double sx = 1.0 / radius(kNear, far0, field_of(x.option), 1.0, 1.0);
        char what[160];
        snprintf(what, sizeof what,
                 "X4's fit at %s: cascade-0 sx %.5f, measured %.5f", x.what, sx,
                 x.measured);
        check(std::fabs(sx - x.measured) < 2e-4, what);
    }
    // the threshold X4 derives from the same sphere, logged in takes 190-202
    const double r110 = radius(kNear, far0, field_of(1.4917), 1.0, 1.0);
    const double r737 = radius(kNear, far0, field_of(1.0), 1.0, 1.0);
    check(std::fabs((kNear + far0) / 2 + std::sqrt(3.0) * r737 - 7.2) < 0.05 &&
              std::fabs((kNear + far0) / 2 + std::sqrt(3.0) * r110 - 5.25) < 0.05,
          "threshold 0 = mid + sqrt(3) r: 7.2 at 73.74 deg, 5.2 at 110 deg");

    // ---- coverage and divisors -------------------------------------------
    const double f110 = field_of(1.4917);
    check(std::fabs(coverage(1.0, f110, 1.0) - 0.245) < 1e-3,
          "X4's own divisor at 110 deg: coverage 0.245");
    check(std::fabs(coverage(1.0, field_of(1.1111), 1.0) - 0.663) < 1e-3,
          "X4's own at vanilla's 81.9 deg: coverage 0.663");
    const double d = divisor(0.663, f110, 1.0);
    printf("      divisor for 0.663 at 110 deg: %.5f\n", d);
    check(std::fabs(d - 0.3698) < 1e-3, "rho 0.663 at 110 deg: divisor 0.3698");
    check(std::fabs(coverage(d, f110, 1.0) - 0.663) < 1e-9,
          "coverage(divisor(rho)) == rho");
    const double fix_sx = 1.0 / radius(kNear, far0, f110, d, d);
    printf("      FIX cascade-0 sx at 110 deg: %.5f (box %.2f m)\n", fix_sx,
           2.0 / fix_sx);
    check(std::fabs(fix_sx - 0.22954) < 2e-4, "FIX at 0.663: cascade-0 sx 0.22954");
    const double a = 1920.0 / 1080.0;
    check(std::fabs(coverage(divisor(0.5, f110, a), f110, a) - 0.5) < 1e-9 &&
              std::fabs(coverage(divisor(0.5, f110, 1.0 / a), f110, 1.0 / a) -
                        0.5) < 1e-9,
          "non-square aspect: each axis reaches rho");

    // ---- the four sites, against the real binary --------------------------
    const char *path = x4_test_binary();
    Elf elf;
    if (elf.load(path)) {
        bool all = true, targets = true;
        for (auto &fs : kSites) {
            uint8_t b[12];
            if (!elf.read(fs.site.va, b, fs.site.n) ||
                memcmp(b, fs.site.bytes, fs.site.n) != 0)
                all = false;
            if (rip_target(fs.site.va, fs.site.n, fs.site.bytes) !=
                (fs.slot ? kAspectY : kAspectX))
                targets = false;
        }
        check(all, "the fit's four reads match X4's bytes");
        check(targets, "each read points at its aspect global (slot 0 x, 1 y)");
        float def[2];
        check(elf.read(kAspectDefaultAddr, def, sizeof def) && def[0] == 1.0f &&
                  def[1] == 1.0f,
              "X4's default aspect constant is (1, 1)");
        float c0;
        check(elf.read(kCascade0Far, &c0, 4) == false,
              "cascade 0's far end is runtime state (.bss), read live");
    } else {
        printf("SKIP  %s not readable — the site checks need X4\n", path);
    }
    bool one_page = true;
    for (auto &fs : kSites)
        if ((fs.site.va & ~0xfffUL) != 0x2095000UL)
            one_page = false;
    check(one_page, "all four sites share page 0x2095000 (one mprotect)");
    int32_t disp;
    check(disp_for(0x2095000, 8, 0x3f0000, &disp) &&
              !disp_for(0x2095000, 8, 0x2095000UL + 0x90000000UL, &disp),
          "rel32 reach: below X4's base yes, 2.25 GB away no");

    // The injector's own choice for X4: a page just below X4's image base
    // (0x400000), reachable from all four real sites. This process is PIE, so
    // that range is free here as it is in X4.
    {
        uintptr_t va[kNumSites];
        for (size_t i = 0; i < kNumSites; i++)
            va[i] = kSites[i].site.va;
        void *p = alloc_near(0x3f0000, 0x7c00000, va, kNumSites, 8);
        bool reach = p != nullptr;
        for (size_t i = 0; p && i < kNumSites; i++)
            reach = reach && disp_for(va[i], 8, (uintptr_t)p + 4, &disp);
        printf("      the injector's page for X4: %p\n", p);
        check(reach, "alloc_near(0x3f0000, ...) gives a page all four X4 sites reach");
        if (p)
            munmap(p, (size_t)sysconf(_SC_PAGESIZE));
    }

    // ---- the patch, executed -----------------------------------------------
    // A code page holding X4's own bytes for sites 1 (xmm0) and 0 (xmm1),
    // each made a callable function returning the float it reads as a double.
    const long page = sysconf(_SC_PAGESIZE);
    uint8_t *code = (uint8_t *)mmap(nullptr, (size_t)page, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // "X4's globals": a data page next to it.
    volatile float *globals =
        (volatile float *)alloc_near((uintptr_t)code - 0x10000,
                                     (uintptr_t)code + 0x10000,
                                     (const uintptr_t *)&code, 1, 8);
    if (code == MAP_FAILED || !globals) {
        check(false, "could not map the test pages");
        return 1;
    }
    globals[0] = 1.0f;  // X4's aspect x
    globals[1] = 1.25f; // X4's aspect y
    // fn0: cvtss2sd xmm0, [rip+d]; ret           (site 0x20953c8's bytes)
    // fn1: cvtss2sd xmm1, [rip+d]; movaps xmm0, xmm1; ret   (site 0x209539f's)
    uint8_t *f0 = code, *f1 = code + 16;
    memcpy(f0, kSites[1].site.bytes, 8);
    f0[8] = 0xc3;
    memcpy(f1, kSites[0].site.bytes, 8);
    f1[8] = 0x0f, f1[9] = 0x28, f1[10] = 0xc1, f1[11] = 0xc3;
    int32_t d0 = 0, d1 = 0;
    check(disp_for((uintptr_t)f0, 8, (uintptr_t)&globals[0], &d0) &&
              disp_for((uintptr_t)f1, 8, (uintptr_t)&globals[1], &d1),
          "the test's globals are within reach of its code");
    memcpy(f0 + 4, &d0, 4);
    memcpy(f1 + 4, &d1, 4);
    mprotect(code, (size_t)page, PROT_READ | PROT_EXEC);
    using Fn = double (*)();
    const Fn fn0 = (Fn)(void *)f0, fn1 = (Fn)(void *)f1;
    check(fn0() == 1.0 && fn1() == 1.25,
          "before: X4's bytes read X4's globals (1.0, 1.25)");

    volatile float *slots = (volatile float *)alloc_near(
        (uintptr_t)code - 0x20000, (uintptr_t)code + 0x20000,
        (const uintptr_t *)&code, 1, 8);
    check(slots && (uintptr_t)slots != (uintptr_t)globals,
          "alloc_near: a private page within reach");
    if (!slots)
        return 1;
    slots[0] = 0.3698f;
    slots[1] = 0.5f;
    Retarget r[] = {{(uintptr_t)f0, 8, (uintptr_t)&slots[0]},
                    {(uintptr_t)f1, 8, (uintptr_t)&slots[1]}};
    char err[200] = "";
    const bool ok = retarget(r, 2, err, sizeof err);
    if (!ok)
        printf("      retarget: %s\n", err);
    check(ok, "retarget: both displacements rewritten, page back to r-x");
    check(std::fabs(fn0() - 0.3698) < 1e-6 && fn1() == 0.5,
          "after: the same instructions read the private pair");
    slots[0] = 1.0f;
    check(fn0() == 1.0, "the private value changes live with no further patch");
    check(memcmp(f0, kSites[1].site.bytes, 4) == 0 &&
              memcmp(f1, kSites[0].site.bytes, 4) == 0,
          "only the displacement changed: prefix, opcode and ModRM intact");
    check(globals[0] == 1.0f && globals[1] == 1.25f,
          "X4's globals themselves untouched");

    Retarget far_r[] = {{(uintptr_t)f0, 8, (uintptr_t)f0 + 0x90000000UL}};
    check(!retarget(far_r, 1, err, sizeof err) && fn0() == 1.0,
          "a target out of rel32 reach refuses, and changes nothing");
    Retarget two_pages[] = {{(uintptr_t)f0, 8, (uintptr_t)&slots[0]},
                            {(uintptr_t)f0 + (uintptr_t)page, 8,
                             (uintptr_t)&slots[0]}};
    check(!retarget(two_pages, 2, err, sizeof err),
          "sites on different pages refuse");

    printf("\n%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
