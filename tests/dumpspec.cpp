// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_UI_DUMP's grammar, which has already cost a take.
//
// Take 258 ran `X4VR_UI_DUMP=600:6`. The third field did not exist, so a burst
// took six CONSECUTIVE presents -- 8.5 ms apart at the 117 presents/s that run
// measured. tools/hud_slide.py needs 15..300 px of image motion, which at
// 12.8 px/deg is 1.2..23 deg, so clearing the floor in 8.5 ms needed a
// 138 deg/s head turn. Every one of the 130 pairs was rejected as
// quantisation. The run could not have succeeded at ANY head speed, and
// nothing in its log said so, because the spacing was a default nobody had
// written down rather than a decision.
//
// So the two cases that matter most here are the take-258 string and the
// default it now takes.
#include "../common/x4vr_dumpspec.hpp"

#include <cstdio>
#include <cstring>

static int fails = 0, checks = 0;

static void eq(unsigned long long got, unsigned long long want,
               const char *what) {
    checks++;
    if (got != want) {
        fails++;
        printf("  FAIL %-46s got %llu want %llu\n", what, got, want);
    }
}
static void tf(bool got, bool want, const char *what) {
    checks++;
    if (got != want) {
        fails++;
        printf("  FAIL %-46s got %s want %s\n", what, got ? "true" : "false",
               want ? "true" : "false");
    }
}

int main() {
    using x4vr::parse_dump_spec;

    // ---- the string take 258 actually ran --------------------------------
    {
        const auto s = parse_dump_spec("600:6");
        eq(s.period, 600, "600:6 period");
        eq(s.burst, 6, "600:6 burst");
        tf(s.had_gap, false, "600:6 did NOT choose a gap");
        eq(s.gap_ms, 150, "600:6 gap defaults to 150 ms, not 0");
        tf(s.malformed, false, "600:6 is well formed");
    }
    // The default is the whole point: at 150 ms a pair is measurable for head
    // rates of 8..156 deg/s. At the old implicit 0 it was measurable at none.
    // If someone widens the default later, this is the line that argues.
    {
        const auto s = parse_dump_spec("600:6:150");
        eq(s.gap_ms, 150, "explicit 150 agrees with the default");
        tf(s.had_gap, true, "explicit 150 records that it was chosen");
    }
    // Zero must remain REACHABLE -- a run wanting adjacent presents can have
    // them, it just has to say so.
    {
        const auto s = parse_dump_spec("600:6:0");
        eq(s.gap_ms, 0, "600:6:0 means adjacent presents");
        tf(s.had_gap, true, "600:6:0 was chosen, not defaulted");
        tf(s.malformed, false, "600:6:0 is well formed");
    }

    // ---- shapes that existed before the third field ----------------------
    {
        const auto s = parse_dump_spec("600");
        eq(s.period, 600, "600 period");
        eq(s.burst, 1, "600 burst defaults to 1 (the pre-burst behaviour)");
        tf(s.had_burst, false, "600 did not choose a burst");
    }
    {
        const auto s = parse_dump_spec(nullptr);
        eq(s.period, 0, "null spec turns the feature off");
        tf(s.malformed, false, "null is not malformed, it is absent");
    }
    {
        const auto s = parse_dump_spec("");
        eq(s.period, 0, "empty spec turns the feature off");
        tf(s.malformed, false, "empty is not malformed, it is absent");
    }
    {
        const auto s = parse_dump_spec("0");
        eq(s.period, 0, "explicit 0 turns the feature off");
    }

    // ---- malformed. Each must be FLAGGED, never parsed past in silence ---
    // `--max-frames 40` silently truncating 93 frames and calling the result
    // complete is the failure this guards against.
    {
        const auto s = parse_dump_spec("abc");
        tf(s.malformed, true, "abc is flagged");
        eq(s.period, 0, "abc yields no period");
    }
    {
        const auto s = parse_dump_spec("600:");
        tf(s.malformed, true, "trailing colon is flagged");
    }
    {
        const auto s = parse_dump_spec("600:6:");
        tf(s.malformed, true, "trailing second colon is flagged");
    }
    {
        const auto s = parse_dump_spec("600:x");
        tf(s.malformed, true, "non-numeric burst is flagged");
        eq(s.burst, 1, "non-numeric burst falls back to 1");
    }
    {
        const auto s = parse_dump_spec("600:6:x");
        tf(s.malformed, true, "non-numeric gap is flagged");
        eq(s.gap_ms, 150, "non-numeric gap falls back to the default");
    }
    {
        const auto s = parse_dump_spec("600:6:-5");
        tf(s.malformed, true, "negative gap is flagged, not clamped silently");
        eq(s.gap_ms, 150, "negative gap falls back to the default");
    }
    {
        // A zero burst would make slots.assign(0) and the ring would never
        // fill, so the flush would never run and the take would produce
        // nothing while looking armed.
        const auto s = parse_dump_spec("600:0");
        eq(s.burst, 1, "burst 0 becomes 1 — a 0-slot ring never flushes");
    }

    // ---- the field order cannot be confused ------------------------------
    // Three separate lambdas used to read this string. The risk that replaced
    // is a single parser reading the fields in the wrong order, which no
    // individual field check would catch.
    {
        const auto s = parse_dump_spec("11:22:33");
        eq(s.period, 11, "field 1 is the period");
        eq(s.burst, 22, "field 2 is the burst");
        eq(s.gap_ms, 33, "field 3 is the gap");
    }

    printf("%s  dumpspec: %d check(s), %d failure(s)\n",
           fails ? "FAIL" : "ok  ", checks, fails);
    return fails ? 1 : 0;
}
