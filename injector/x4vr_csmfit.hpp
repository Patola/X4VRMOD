// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_CSM_COVER=<rho> -- #70's fix: size X4's shadow cascades for the field
// we render.
//
// X4's cascade fit (0x2094fc0, docs/x4-quirks.md "#70's cause, read from X4's
// code") gives each cascade a bounding sphere over its slice [n, f] of view
// depth:
//
//     r^2 = ((f - n)/2)^2 + (f/2 * cot/ax)^2 + (f/2 * cot/ay)^2
//     cot = 1/tan(field/2),  ax, ay = X4's aspect globals [0x4533b00], [0x4533b04]
//
// The slice's real half-extent at f is f * a * tan(field/2): X4 uses the
// projection SCALE where the frustum needs its inverse, so the spheres shrink as
// the field widens. Coverage rho = (cot/a/2) / (a * tan) is 0.663 at vanilla's
// 81.9 deg and 0.245 at our 110 deg; the knee Patola found is ~0.45.
//
// The fit reads ax/ay through four instructions, `cvtss2sd xmmN,
// [rip+disp32]`, and nothing else in it depends on them. Retargeting those four
// displacements at a private pair of floats changes the fit and nothing else:
// X4's globals, and every projection that defaults to them, are untouched.
// Writing d = cot^2 / (2 * rho * a) into the private pair makes the sphere
// reach rho of the slice; writing X4's own ax/ay makes it X4's fit exactly.
//
// The patch happens once, in the injector's constructor, before any X4 code
// runs -- no thread can be executing the four instructions while they change.
// Each site's bytes, and the default aspect constant, must match X4 9.00 or
// nothing is written.
#pragma once

#include <algorithm>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "x4vr_camwatch.hpp"

namespace x4vr {
namespace csmfit {

using camwatch::Site;

// X4's aspect globals and their initial value (written at 0x2025686 from the
// constant (1.0, 1.0) at 0x2e24680; later from the render size, 0x2023623).
inline constexpr uintptr_t kAspectX = 0x4533b00, kAspectY = 0x4533b04;
inline constexpr uintptr_t kAspectDefaultAddr = 0x2e24680;
// Cascade 0's far end before the fit's x 1.02, and the fit's near (take 323's
// three fields fit n = 0.1).
inline constexpr uintptr_t kCascade0Far = 0x45343ec;
inline constexpr float kFarScale = 1.02f, kNear = 0.1f;

// The fit's four reads of the aspect globals. `slot` 0 = x (0x4533b00),
// 1 = y (0x4533b04).
struct FitSite {
    Site site;
    int slot;
};
inline constexpr FitSite kSites[] = {
    {{0x209539f, 8, {0xf3, 0x0f, 0x5a, 0x0d, 0x5d, 0xe7, 0x49, 0x02}}, 1},
    {{0x20953c8, 8, {0xf3, 0x0f, 0x5a, 0x05, 0x30, 0xe7, 0x49, 0x02}}, 0},
    {{0x2095554, 8, {0xf3, 0x0f, 0x5a, 0x15, 0xa4, 0xe5, 0x49, 0x02}}, 0},
    {{0x2095577, 8, {0xf3, 0x0f, 0x5a, 0x15, 0x85, 0xe5, 0x49, 0x02}}, 1},
};
inline constexpr size_t kNumSites = sizeof kSites / sizeof kSites[0];

// Where a RIP-relative instruction of `len` bytes, its disp32 in the last four,
// points.
inline uintptr_t rip_target(uintptr_t insn, size_t len, const uint8_t *bytes) {
    int32_t d;
    memcpy(&d, bytes + len - 4, 4);
    return insn + len + (intptr_t)d;
}

// Every read of the fit, by its bytes and by the global it reads, and X4's
// default aspect constant. Used by the patch and by the launcher, which runs it
// over the X4 file.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (size_t i = 0; i < kNumSites; i++) {
        const Site &st = kSites[i].site;
        uint8_t b[sizeof st.bytes];
        if (!read(st.va, b, st.n) || memcmp(b, st.bytes, st.n) != 0 ||
            rip_target(st.va, st.n, b) !=
                (kSites[i].slot ? kAspectY : kAspectX)) {
            *bad = st.va;
            return false;
        }
    }
    float def[2] = {0.f, 0.f};
    if (!read(kAspectDefaultAddr, def, sizeof def) || def[0] != 1.0f ||
        def[1] != 1.0f) {
        *bad = kAspectDefaultAddr;
        return false;
    }
    return true;
}

// The disp32 that makes that instruction point at `target`, if it can.
inline bool disp_for(uintptr_t insn, size_t len, uintptr_t target, int32_t *out) {
    const long long d = (long long)target - (long long)(insn + len);
    if (d < INT32_MIN || d > INT32_MAX)
        return false;
    *out = (int32_t)d;
    return true;
}

// --- the arithmetic -------------------------------------------------------

inline double cot_half(double field) { return 1.0 / std::tan(field * 0.5); }

// The divisor that makes X4's fit reach `rho` of the slice on an axis whose
// aspect global is `a`: the fit uses cot/d where the slice needs 2*rho*a*tan.
inline double divisor(double rho, double field, double a) {
    const double c = cot_half(field);
    return c * c / (2.0 * rho * a);
}

// What reach a divisor gives (the inverse of `divisor`; d = a is X4's own).
inline double coverage(double d, double field, double a) {
    return (cot_half(field) / d) * 0.5 / (a * std::tan(field * 0.5));
}

// X4's sphere radius for one slice, with divisors dx, dy in place of ax, ay.
inline double radius(double n, double f, double field, double dx, double dy) {
    const double c = cot_half(field);
    const double h = (f - n) * 0.5, lx = f * 0.5 * c / dx, ly = f * 0.5 * c / dy;
    return std::sqrt(h * h + lx * lx + ly * ly);
}

// --- the patch ------------------------------------------------------------

// A page of our own within rel32 reach of every site: X4 is not PIE, so try
// just below its image base, then just above its highest mapping.
inline void *alloc_near(uintptr_t below, uintptr_t above, const uintptr_t *sites,
                        size_t n_sites, size_t site_len) {
    const long page = sysconf(_SC_PAGESIZE);
    auto reach = [&](uintptr_t a) {
        for (size_t i = 0; i < n_sites; i++) {
            int32_t d;
            if (!disp_for(sites[i], site_len, a, &d) ||
                !disp_for(sites[i], site_len, a + (uintptr_t)page - 8, &d))
                return false;
        }
        return true;
    };
    auto try_at = [&](uintptr_t a) -> void * {
        if (!reach(a))
            return nullptr;
        void *p = mmap((void *)a, (size_t)page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED)
            return nullptr;
        if ((uintptr_t)p != a) { // a kernel without NOREPLACE treats it as a hint
            munmap(p, (size_t)page);
            return nullptr;
        }
        return p;
    };
    for (uintptr_t a = below; a >= 0x10000 && a <= below; a -= 0x10000)
        if (void *p = try_at(a))
            return p;
    for (uintptr_t a = above; a < above + 0x40000000; a += 0x10000)
        if (void *p = try_at(a))
            return p;
    return nullptr;
}

// Rewrite the disp32 of each instruction to point at its new target. All the
// instructions must share one page (X4's four do: 0x2095000). The page is made
// writable for the copy and given back read+execute; `err` says what failed.
struct Retarget {
    uintptr_t insn;
    size_t len;
    uintptr_t target;
};
inline bool retarget(const Retarget *r, size_t n, char *err, size_t errlen) {
    const long page = sysconf(_SC_PAGESIZE);
    int32_t disp[8];
    if (n > 8) {
        snprintf(err, errlen, "too many sites");
        return false;
    }
    // Every page the sites touch: they share one on Steam's build, not on every build.
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (size_t i = 0; i < n; i++) {
        lo = std::min(lo, r[i].insn);
        hi = std::max(hi, r[i].insn + r[i].len);
        if (!disp_for(r[i].insn, r[i].len, r[i].target, &disp[i])) {
            snprintf(err, errlen, "%#lx out of rel32 reach of %#lx",
                     (unsigned long)r[i].target, (unsigned long)r[i].insn);
            return false;
        }
    }
    const uintptr_t pg = lo & ~(uintptr_t)(page - 1);
    const size_t span = ((hi + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1)) - pg;
    if (mprotect((void *)pg, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0 &&
        mprotect((void *)pg, span, PROT_READ | PROT_WRITE) != 0) {
        snprintf(err, errlen, "mprotect(rw) %#lx: %s", (unsigned long)pg,
                 strerror(errno));
        return false;
    }
    for (size_t i = 0; i < n; i++)
        memcpy((void *)(r[i].insn + r[i].len - 4), &disp[i], 4);
    if (mprotect((void *)pg, span, PROT_READ | PROT_EXEC) != 0) {
        snprintf(err, errlen, "mprotect(rx) %#lx: %s", (unsigned long)pg,
                 strerror(errno));
        return false;
    }
    for (size_t i = 0; i < n; i++)
        if (rip_target(r[i].insn, r[i].len, (const uint8_t *)r[i].insn) !=
            r[i].target) {
            snprintf(err, errlen, "read-back of %#lx does not point at %#lx",
                     (unsigned long)r[i].insn, (unsigned long)r[i].target);
            return false;
        }
    return true;
}

} // namespace csmfit
} // namespace x4vr
