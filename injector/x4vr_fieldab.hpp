// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_FIELD_AB=<seconds> -- #70's knee, switched live inside one run.
//
// Takes 226-228 found the cockpit shadows clean at X4VR_FOV <= 1.24 and fading
// above ~1.28 -- across RUNS, which Patola's eye is not reliable for. Take 322's
// CPU suspect (the camera's base field, camera+0x890) was refuted. This puts
// the knee inside ONE run: phase A renders at X4VR_FOV (the wide field), phase
// B at X4VR_FIELD_B (1.24 by default), alternating, so he watches the shadows
// switch -- and the layer reads, per phase, the projection X4 actually drew
// with (M_projection) beside the one its shadow lookup inverts
// (M_invprojection_uj): if the picture's field and the shadows' field part
// company in phase A, the defect is a matrix the shader can be given.
//
// The lever is X4's EFFECTIVE field, camera+0x874 = option x 73.74 deg: the
// constructor sets it, the FOV setter recomputes it, and 0x1926f80 reads it for
// the projection. Flatscreen only: in VR the headset is told one field at start,
// so a live switch would show the world at the wrong size in one phase.
//
// Phases start only once GAME TIME runs with the cockpit camera up -- take 322
// armed on the load screen, where the camera, its parameters and the HUD
// already exist. Every phase's value is read back each tick; the scorer judges
// a phase by what X4 rendered, not by what was written (take 322's arms
// collapsed while the instrument reported both).
#pragma once

#include <cmath>
#include <cstdint>

#include "x4vr_shadowfov.hpp"

namespace x4vr {
namespace fieldab {

using camwatch::Site;
using shadowfov::kDefault;
using shadowfov::kEffective;

// The effective field's writers and readers, byte for byte.
inline constexpr Site kSites[] = {
    // Constructor and setter: the only writers (shadowfov's sites 0 and 3).
    {0x1dcca31, 8, {0xf3, 0x0f, 0x11, 0x83, 0x74, 0x08, 0x00, 0x00}},
    {0x138ba0c, 9, {0xf3, 0x41, 0x0f, 0x11, 0x89, 0x74, 0x08, 0x00, 0x00}},
    // 0x1926f80, the field X4 returns for the projection: both reads.
    {0x1926fb9, 8, {0xf3, 0x0f, 0x10, 0x8b, 0x74, 0x08, 0x00, 0x00}},
    {0x1927010, 8, {0xf3, 0x0f, 0x10, 0x83, 0x74, 0x08, 0x00, 0x00}},
};

template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    return camwatch::verify_sites(kSites, sizeof kSites / sizeof kSites[0], read,
                                  bad);
}

// The effective field for an option, as X4's setter computes it.
inline float field_of(float option) { return shadowfov::rendered(option); }

// The projection's sx for a field, square render: 1 / tan(field / 2).
inline float sx_of(float option) { return 1.0f / std::tan(field_of(option) * 0.5f); }

// Phase k: A (the wide field, X4 as configured) first, then B, alternating.
inline bool phase_b(double elapsed_s, double period_s) {
    if (period_s <= 0.0)
        return false;
    return ((long)(elapsed_s / period_s) & 1) != 0;
}

// What to write this tick: the phase's field, whenever the read differs.
struct Action {
    bool write = false;
    float value = 0.f;
};
inline Action decide(float read, float option_a, float option_b, bool b) {
    Action a;
    const float want = field_of(b ? option_b : option_a);
    if (std::fabs(read - want) > 1e-5f) {
        a.write = true;
        a.value = want;
    }
    return a;
}

// Is the camera's field what X4 should have set before anything was written?
inline bool layout_ok(float read, float option_a) {
    return std::fabs(read - field_of(option_a)) < 1e-3f;
}

// "Playing": game time advancing. X4's clock (GetCurrentGameTime) holds still
// while loading and in pause; it steps once per frame, so equal readings
// between frames are normal. Running = it has advanced by `need_s` without
// standing still for more than `gap_s` of wall time. A stall or a clock that
// went BACKWARDS (a new game loaded) starts the count over.
struct GameClock {
    double last_game = -1.0;  // game time at its last change
    double last_change = 0.0; // wall time of that change
    double ran = 0.0;         // game seconds advanced in the current stretch
    bool running = false;
};
inline bool game_clock_step(GameClock &c, double game_s, double wall_s,
                            double need_s, double gap_s) {
    if (c.last_game < 0.0) {
        c.last_game = game_s;
        c.last_change = wall_s;
    } else if (game_s > c.last_game) {
        const double dg = game_s - c.last_game;
        c.ran = (wall_s - c.last_change <= gap_s) ? c.ran + dg : dg;
        c.last_game = game_s;
        c.last_change = wall_s;
    } else if (game_s < c.last_game) {
        c.ran = 0.0;
        c.last_game = game_s;
        c.last_change = wall_s;
    } else if (wall_s - c.last_change > gap_s) {
        c.ran = 0.0;
    }
    c.running = c.ran >= need_s;
    return c.running;
}

} // namespace fieldab
} // namespace x4vr
