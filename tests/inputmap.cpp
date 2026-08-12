// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The inputmap rewrite, checked without X4.
//
// Head-look rebinds X4's free-look state onto a key no keyboard can emit, and
// serves the result from memory so the player's own inputmap is never touched.
// Three things about that rewrite are load-bearing, and each has already gone
// wrong once somewhere in this project:
//
//   1. It must rewrite EVERY binding of the id, not the first. X4's format
//      aliases -- LOOTMAGNET and MATCH_SPEED each appear twice on different
//      sources in the real file -- and a surviving alias would leave the
//      original shift+middle-mouse path live next to ours.
//   2. It must emit no `toggle` attribute, and strip one that was there. The
//      in-game experiment that settled hold-over-toggle left a toggle="1" on
//      CAMERA_MOUSELOOK in the player's file; inheriting it would invert the
//      semantics we chose *because* the latch desynchronises on save load.
//   3. It must leave INPUT_RANGE_MOUSELOOK_YAW/PITCH alone. Those are the
//      channel we feed; rebinding them off the mouse axes would cut it.
//
// The fixture below is the shape of the real file, aliases and all.
#include <cstdio>
#include <string>

#include "../injector/x4vr_inputmap.hpp"

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        g_fail++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}

static bool has(const std::string &s, const char *needle) {
    return s.find(needle) != std::string::npos;
}

static size_t count(const std::string &s, const char *needle) {
    size_t n = 0, at = 0;
    const size_t len = strlen(needle);
    while ((at = s.find(needle, at)) != std::string::npos) {
        n++;
        at += len;
    }
    return n;
}

int main() {
    // Shaped like the real inputmap.xml, including the toggle Patola added by
    // hand during the experiment and a second, aliased binding.
    const std::string original =
        "<inputmap>\n"
        "  <state id=\"INPUT_STATE_LOOTMAGNET\" source=\"INPUT_SOURCE_KEYBOARD\""
        " toggle=\"1\" code=\"INPUT_KEYCODE_O\"/>\n"
        "  <state id=\"INPUT_STATE_CAMERA_MOUSELOOK\""
        " source=\"INPUT_SOURCE_MOUSEBUTTONS\" toggle=\"1\""
        " code=\"INPUT_MOUSEBUTTON_MIDDLE_SHIFT\"/>\n"
        "  <state id=\"INPUT_STATE_CAMERA_LEFT\" source=\"INPUT_SOURCE_KEYBOARD\""
        " code=\"INPUT_KEYCODE_NUMPAD4\"/>\n"
        "  <state id=\"INPUT_STATE_CAMERA_MOUSELOOK\""
        " source=\"INPUT_SOURCE_JOYBUTTONS\" code=\"INPUT_XBUTTON_LEFT_THUMB\"/>\n"
        "  <range id=\"INPUT_RANGE_MOUSELOOK_YAW\""
        " source=\"INPUT_SOURCE_MOUSEAXES\" code=\"INPUT_MOUSEAXIS_X\"/>\n"
        "  <range id=\"INPUT_RANGE_MOUSELOOK_PITCH\""
        " source=\"INPUT_SOURCE_MOUSEAXES\" code=\"INPUT_MOUSEAXIS_Y\"/>\n"
        "</inputmap>\n";

    {
        std::string xml = original;
        std::string old;
        const int n = x4vr::bind_headlook(xml, &old);

        check(n == 2, "both aliased CAMERA_MOUSELOOK bindings are found");
        check(count(xml, "INPUT_STATE_CAMERA_MOUSELOOK") == 1,
              "exactly one CAMERA_MOUSELOOK binding survives");
        check(has(xml, "INPUT_KEYCODE_F13"),
              "the survivor is bound to the head-look key");
        check(has(xml, "id=\"INPUT_STATE_CAMERA_MOUSELOOK\" "
                       "source=\"INPUT_SOURCE_KEYBOARD\""),
              "and to the keyboard source");
        check(!has(xml, "INPUT_MOUSEBUTTON_MIDDLE_SHIFT"),
              "the shift+middle-mouse path is gone, not merely shadowed");
        check(!has(xml, "INPUT_XBUTTON_LEFT_THUMB"),
              "the aliased joystick binding is gone too");
        check(has(old, "INPUT_MOUSEBUTTON_MIDDLE_SHIFT"),
              "the old binding is reported for the log");

        // The toggle must not survive on OUR binding -- but must survive on
        // everyone else's. Both halves matter: the first is the semantics we
        // chose, the second is the player's own setting.
        const size_t mouselook_at = xml.find("INPUT_STATE_CAMERA_MOUSELOOK");
        const size_t elem_end = xml.find("/>", mouselook_at);
        check(xml.find("toggle", mouselook_at) > elem_end,
              "no toggle on the head-look binding (hold, not latch)");
        check(has(xml, "INPUT_STATE_LOOTMAGNET\" source=\"INPUT_SOURCE_KEYBOARD\""
                       " toggle=\"1\""),
              "the player's own loot-magnet toggle is untouched");

        // The channel we feed must be intact.
        check(has(xml, "INPUT_RANGE_MOUSELOOK_YAW\" "
                       "source=\"INPUT_SOURCE_MOUSEAXES\""),
              "MOUSELOOK_YAW still reads the mouse axis");
        check(has(xml, "INPUT_RANGE_MOUSELOOK_PITCH\" "
                       "source=\"INPUT_SOURCE_MOUSEAXES\""),
              "MOUSELOOK_PITCH still reads the mouse axis");
        check(has(xml, "INPUT_KEYCODE_NUMPAD4"),
              "an unrelated binding is untouched");
        check(xml.find("</inputmap>") != std::string::npos &&
                  count(xml, "<state") + count(xml, "<range") == 5,
              "the file stays well formed: 6 elements in, 5 out");
    }

    {
        // Idempotent: serving the profile happens on every read, so a second
        // pass over an already-rewritten file must not disturb it.
        std::string once = original;
        x4vr::bind_headlook(once);
        std::string twice = once;
        const int n = x4vr::bind_headlook(twice);
        check(n == 1 && twice == once, "rewriting an already-bound file is a no-op");
    }

    {
        // A file with no such binding must be left exactly as it was, so the
        // caller can fall back to serving the untouched original.
        std::string xml = "<inputmap>\n  <state id=\"INPUT_STATE_OTHER\""
                          " source=\"INPUT_SOURCE_KEYBOARD\""
                          " code=\"INPUT_KEYCODE_Q\"/>\n</inputmap>\n";
        const std::string keep = xml;
        check(x4vr::bind_headlook(xml) == 0 && xml == keep,
              "a file without the binding is returned unchanged");
    }

    {
        check(x4vr::is_x4_inputmap("/x/inputmap.xml"), "inputmap.xml matches");
        check(x4vr::is_x4_inputmap("/x/inputmap_3.xml"), "inputmap_3.xml matches");
        check(!x4vr::is_x4_inputmap("/x/config.xml"), "config.xml does not");
        check(!x4vr::is_x4_inputmap("/x/inputmapfoo.xml"), "inputmapfoo.xml does not");
        // The profile must not look like an inputmap to our own path test, or
        // serving it would recurse forever -- the same trap config-x4vrmod.xml
        // was named to avoid.
        const std::string prof = x4vr::inputmap_profile_path("/x/inputmap_3.xml");
        check(prof == "/x/inputmap_3-x4vrmod.xml", "profile path is derived");
        check(!x4vr::is_x4_inputmap(prof.c_str()),
              "the profile does not match the interposer's own test");
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
