// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_SHADOWFOV_AB=<seconds> -- #70's suspect, tested by switching it.
//
// #70: at our wide field the cockpit shadows fade as the head turns; at X4VR_FOV
// <= 1.24 they hold (take 320). Take 223 found every shadow-configured camera
// slot pinned at sx 1.33333 -- X4's DEFAULT field, 73.74 deg -- while the camera
// we see follows <fov>. Read statically (docs/x4-quirks.md, "#70, the field X4
// sizes things by"):
//
//   * the camera constructor sets BOTH camera+0x874 and camera+0x890 to the
//     constant at 0x2e2cbc4 = 1.2870022 rad = 73.74 deg, and nothing else ever
//     writes +0x890;
//   * the FOV option multiplies: +0x874 = max(option, 0.01) x +0x890 -- so
//     +0x874 is the field we render (1.4917 x 73.74 = 110 deg) and +0x890
//     stays the bare default;
//   * a view is built from the BASE (0x19276fc -> view+0xcc8 -> 0x21bc1b0,
//     which builds a projection), and 0x1164da3 takes tan(base/2) right after
//     the camera-rotation fetch -- systems sized by 73.74 while the picture is
//     110. That is the pinned population's field exactly.
//
// The test: in alternating phases, widen the base to the rendered field (FIX)
// or leave it at X4's default (CONTROL), and let Patola watch the shadows switch
// -- a transition within one run, which is what his eye is reliable for. The
// effective field is re-asserted too, because X4's setter recomputes it as
// option x base on a camera re-init and would otherwise compound to 164 deg.
//
// Every offset is tied to X4 9.00's bytes and the live values must read as the
// layout says before anything is written; a mismatch refuses.
#pragma once

#include <cmath>
#include <cstdint>

#include "x4vr_camwatch.hpp"

namespace x4vr {
namespace shadowfov {

using camwatch::Site;

inline constexpr Site kSites[] = {
    // The camera constructor: effective and base both from the default.
    {0x1dcca31, 8, {0xf3, 0x0f, 0x11, 0x83, 0x74, 0x08, 0x00, 0x00}},
    {0x1dcca39, 8, {0xf3, 0x0f, 0x11, 0x83, 0x90, 0x08, 0x00, 0x00}},
    // The setter: effective = option x base.
    {0x138ba03, 9, {0xf3, 0x41, 0x0f, 0x59, 0x89, 0x90, 0x08, 0x00, 0x00}},
    {0x138ba0c, 9, {0xf3, 0x41, 0x0f, 0x11, 0x89, 0x74, 0x08, 0x00, 0x00}},
    // A view built from the BASE: camera+0x890 -> view+0xcc8.
    {0x19276fc, 9, {0xf3, 0x41, 0x0f, 0x10, 0x86, 0x90, 0x08, 0x00, 0x00}},
    {0x1927708, 8, {0xf3, 0x0f, 0x11, 0x83, 0xc8, 0x0c, 0x00, 0x00}},
    // A frustum extent from the BASE, right after the camera-rotation fetch.
    {0x1164da3, 8, {0xf3, 0x0f, 0x10, 0x83, 0x90, 0x08, 0x00, 0x00}},
};
inline constexpr uintptr_t kEffective = 0x874, kBase = 0x890;
inline constexpr uintptr_t kDefaultAddr = 0x2e2cbc4;
inline constexpr float kDefault = 1.2870022f; // rad, 73.74 deg

template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    return camwatch::verify_sites(kSites, sizeof kSites / sizeof kSites[0], read,
                                  bad);
}

// The field we render, from the FOV option the injector serves.
inline float rendered(float option) {
    return (option > 0.01f ? option : 0.01f) * kDefault;
}

// Does what we read match the layout before we touch it? The base must be X4's
// default or the widened value we wrote; the effective must be option x
// default (or, transiently, the compounded value X4's setter produces from a
// widened base -- which is what `restore` is for).
struct Read {
    float base, effective;
};
inline bool layout_ok(const Read &r, float option) {
    const float want = rendered(option);
    const bool base_ok = std::fabs(r.base - kDefault) < 1e-4f ||
                         std::fabs(r.base - want) < 1e-4f;
    const bool eff_ok = std::fabs(r.effective - want) < 1e-3f ||
                        std::fabs(r.effective - option * want) < 1e-3f;
    return base_ok && eff_ok;
}

// What to write this tick, if anything. FIX widens the base to the rendered
// field; CONTROL puts X4's default back. The effective is restored to the
// rendered field whenever it has drifted (X4's setter on a camera re-init).
struct Action {
    bool write_base = false, write_effective = false;
    float base = 0.f, effective = 0.f;
};
inline Action decide(const Read &r, float option, bool fix) {
    Action a;
    const float want = rendered(option);
    const float base_target = fix ? want : kDefault;
    if (std::fabs(r.base - base_target) > 1e-5f) {
        a.write_base = true;
        a.base = base_target;
    }
    if (std::fabs(r.effective - want) > 1e-4f) {
        a.write_effective = true;
        a.effective = want;
    }
    return a;
}

// Phase k of a period: CONTROL first, so the first thing seen is X4 as it is.
inline bool fix_phase(double elapsed_s, double period_s) {
    if (period_s <= 0.0)
        return false;
    return ((long)(elapsed_s / period_s) & 1) != 0;
}

} // namespace shadowfov
} // namespace x4vr
