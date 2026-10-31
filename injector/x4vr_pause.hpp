// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_pause.hpp — what a pause of X4 reads as (takes 383–385). Pure, so
// tests/pause.cpp replays the sequences the takes logged.
//
// The ESC menu pauses X4 and is a full-screen menu: PAUSED, the window in
// front. The Pause key pauses with no menu at all: the view under it (the
// cockpit, or on foot) stays immersive, as F2/F4 always did. Two holds:
//   * a fresh pause is decided once IsFullscreenMenuDisplayed has been asked
//     twice since it began -- the ESC menu opens in the pump that pauses,
//     after head-look's tick has read the pause (take 383);
//   * a paused state with no menu up counts as the Pause key only once none
//     has been up for kSettle checks (~0.1 s) -- between two menus X4 has
//     none up for one check (take 385: 26 ms, 38 label flips in a session).
// Without the full-screen query (`known` false) every pause is PAUSED, as
// before take 384.
#pragma once
#include <cstdint>

namespace x4vr {
namespace pause {

constexpr uint64_t kSettle = 8;

enum class Kind {
    NotPaused,
    KeyPause,  // the Pause key: the view under it
    MenuPause, // a full-screen menu (the ESC menu): PAUSED
    Undecided, // fresh, not yet asked twice: the view it was
};

struct Decider {
    bool was_paused = false;
    uint64_t seq0 = 0;

    // `seq`: full-screen checks so far; `down_seq`: the check at which the
    // last full-screen menu went down.
    Kind step(bool paused, bool known, bool menu_up, uint64_t seq,
              uint64_t down_seq) {
        if (paused && !was_paused)
            seq0 = seq;
        was_paused = paused;
        if (!paused)
            return Kind::NotPaused;
        if (!known)
            return Kind::MenuPause;
        if (seq < seq0 + 2)
            return Kind::Undecided;
        if (!menu_up && seq >= down_seq + kSettle)
            return Kind::KeyPause;
        return Kind::MenuPause;
    }
};

} // namespace pause
} // namespace x4vr
