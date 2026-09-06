// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_UI_DUMP = "PERIOD[:BURST[:GAPMS]]"
//
// Parsed here rather than in three separate lambdas inside the layer, because
// three parsers over one string is three chances for them to disagree about
// what a malformed spec means -- and because nothing in tests/ could reach
// them where they were.
//
// **This grammar has already cost a take.** Take 258 ran `600:6`, whose third
// field did not exist, so a burst took six CONSECUTIVE presents: 8.5 ms apart
// at the 117 presents/s that run measured. `tools/hud_slide.py` needs 15..300
// px of image motion and at 12.8 px/deg that is 1.2..23 deg, so a pair needed
// a 138 deg/s head turn to clear the floor. All 130 pairs were rejected as
// quantisation and the run could not have succeeded at any head speed. The
// spacing was never a decision, it was a default nobody had written down.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace x4vr {

struct DumpSpec {
    uint64_t period = 0;    // presents between bursts; 0 = feature off
    uint32_t burst = 1;     // frames per burst; 1 = the pre-burst behaviour
    uint32_t gap_ms = 150;  // wall-clock ms BETWEEN frames of a burst
    // Whether each field was actually present in the string, as opposed to
    // taking its default. A run's log should be able to say "you did not
    // choose this" rather than presenting a default as a decision.
    bool had_burst = false, had_gap = false;
    // Set when a field was present but not a number we could use. The caller
    // reports it; parsing silently past a typo is how `--max-frames 40`
    // truncated 93 frames and called it complete.
    bool malformed = false;
};

// `spec` is the raw environment value, or null.
//
// The default gap is 150 ms and NOT zero. Zero is reachable by writing it,
// which is the point: a run that wants adjacent presents can have them, but
// it has to say so, because that is the setting that cannot measure anything.
inline DumpSpec parse_dump_spec(const char *spec) {
    DumpSpec s;
    if (!spec || !*spec)
        return s;
    char *end = nullptr;
    const long p = strtol(spec, &end, 10);
    if (end == spec) {          // no leading number at all
        s.malformed = true;
        return s;
    }
    s.period = p > 0 ? (uint64_t)p : 0;

    const char *c = strchr(spec, ':');
    if (!c)
        return s;
    if (!c[1]) {                // a trailing ':' with nothing after it
        s.malformed = true;
        return s;
    }
    end = nullptr;
    const long b = strtol(c + 1, &end, 10);
    if (end == c + 1)
        s.malformed = true;
    else {
        s.had_burst = true;
        s.burst = b > 0 ? (uint32_t)b : 1u;
    }

    const char *c2 = strchr(c + 1, ':');
    if (!c2)
        return s;
    if (!c2[1]) {
        s.malformed = true;
        return s;
    }
    end = nullptr;
    const long g = strtol(c2 + 1, &end, 10);
    if (end == c2 + 1)
        s.malformed = true;
    else {
        s.had_gap = true;
        // Negative is refused rather than clamped: it means the spec was not
        // what the author thought, and 0 has a real meaning here.
        s.gap_ms = g >= 0 ? (uint32_t)g : 150u;
        if (g < 0)
            s.malformed = true;
    }
    return s;
}

} // namespace x4vr
