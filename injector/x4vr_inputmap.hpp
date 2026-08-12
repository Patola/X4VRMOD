// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_inputmap.hpp — X4's inputmap.xml, forked and rebound for head-look.
//
// #33 drives X4's own free-look from the head pose. X4 gates free-look behind
// a held state, `INPUT_STATE_CAMERA_MOUSELOOK`, bound by default to
// shift+middle-mouse, and takes the look itself from two relative ranges:
//
//   <state id="INPUT_STATE_CAMERA_MOUSELOOK" source="INPUT_SOURCE_MOUSEBUTTONS"
//          code="INPUT_MOUSEBUTTON_MIDDLE_SHIFT"/>
//   <range id="INPUT_RANGE_MOUSELOOK_YAW"  source="INPUT_SOURCE_MOUSEAXES" .../>
//   <range id="INPUT_RANGE_MOUSELOOK_PITCH" source="INPUT_SOURCE_MOUSEAXES" .../>
//
// We rebind only the *state*, and only to a key nothing else can produce. The
// ranges stay on the mouse axes, because that is the channel we feed.
//
// **Why a rebind rather than synthesising shift+middle-mouse.** Patola's
// objection, and it is correct: those go through X4's own binding interpreter,
// where shift is half the standard combos and middle-mouse is zoom. Holding
// them for a session would corrupt every one of those. A binding X4 knows but
// no keyboard can emit collides with nothing.
//
// **Why HOLD and not toggle="1".** X4 honours `toggle="1"` even on bindings its
// UI does not offer it for, and latched free-look does work — the view stops
// recentring entirely. But the latch *desynchronises*: measured in-game, after
// a save reload X4's input state still reads ON while the camera has stopped
// listening, and it takes an off/on cycle to recover. Same for the loot magnet.
// A latch that silently stops working is the worst failure available here,
// because our integrator keeps counting and the view jumps by the whole
// accumulated error when it re-syncs.
//
// A held key is self-healing by construction: re-assert it every frame and
// after a reload X4 simply sees it held again. Note the two are **mutually
// exclusive** — re-asserting a `toggle="1"` binding every frame would flip it
// on and off forever — which is why the rewrite below emits no `toggle`
// attribute and strips any that was there.
#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace x4vr {

// The key head-look holds. F13-F15 are in X4's keycode vocabulary (confirmed
// by reading the 584 INPUT_KEYCODE_* strings out of the X4 binary) and absent
// from the shipped inputmap. No physical keyboard has them, so the player
// cannot press one by accident and we never have to share it.
//
// Deliberately a *plain* key. X4 encodes modifier combinations as their own
// codes -- INPUT_KEYCODE_J_CONTROL, INPUT_MOUSEBUTTON_MIDDLE_SHIFT -- so
// picking one of those would mean holding the modifier too, which is the exact
// problem the rebind exists to avoid.
inline const char *headlook_code() {
    static const char *code = [] {
        const char *e = getenv("X4VR_HEADLOOK_KEY");
        return (e && *e) ? e : "INPUT_KEYCODE_F13";
    }();
    return code;
}

// True if `path`'s basename is inputmap.xml or inputmap_<N>.xml. X4 keeps five
// numbered profiles alongside the active one and we do not know from here
// which it will read, so all of them are served.
inline bool is_x4_inputmap(const char *path) {
    if (!path)
        return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    if (strncmp(base, "inputmap", 8) != 0)
        return false;
    const char *p = base + 8;
    if (*p == '_') {
        p++;
        if (!isdigit((unsigned char)*p))
            return false;
        while (isdigit((unsigned char)*p))
            p++;
    }
    return strcmp(p, ".xml") == 0;
}

// .../inputmap.xml -> .../inputmap-x4vrmod.xml (and inputmap_2-x4vrmod.xml).
// Must not itself satisfy is_x4_inputmap(), or serving it would recurse.
inline std::string inputmap_profile_path(const char *path) {
    std::string p = path ? path : "";
    const size_t dot = p.rfind(".xml");
    if (dot == std::string::npos)
        return {};
    p.replace(dot, 4, "-x4vrmod.xml");
    return p;
}

// Rewrite every binding of `id` to exactly ONE hold binding on source/code.
// Returns how many bindings were found; 0 means nothing was changed.
//
// **Every** binding, not the first. X4's format aliases freely -- in Patola's
// own file INPUT_STATE_LOOTMAGNET and INPUT_STATE_MATCH_SPEED each appear
// twice, on different sources -- and first-match on an aliased binding is a
// mistake this project has already made more than once. A leftover alias would
// leave the original shift+middle-mouse path live alongside ours.
//
// The replacement is built from scratch rather than edited in place, so any
// `toggle="1"` on the original is dropped. That matters concretely: the
// experiment that settled the design left one on CAMERA_MOUSELOOK in the
// player's file, and inheriting it would invert the semantics we just chose.
inline int set_binding(std::string &xml, const char *id, const char *source,
                       const char *code, std::string *old_desc = nullptr) {
    const std::string needle = std::string("id=\"") + id + "\"";
    int found = 0;
    size_t pos = 0;
    while (true) {
        const size_t at = xml.find(needle, pos);
        if (at == std::string::npos)
            break;
        const size_t start = xml.rfind('<', at);
        const size_t end = xml.find("/>", at);
        if (start == std::string::npos || end == std::string::npos)
            break;
        // Guard against a non-self-closing element: if another tag opens
        // before this one closes, we are not looking at what we think.
        const size_t next_open = xml.find('<', at);
        if (next_open != std::string::npos && next_open < end)
            break;
        const size_t stop = end + 2;

        if (found == 0) {
            size_t n = start + 1;
            while (n < xml.size() && !isspace((unsigned char)xml[n]))
                n++;
            const std::string tag = xml.substr(start + 1, n - start - 1);
            if (old_desc)
                *old_desc = xml.substr(start, stop - start);
            const std::string repl = "<" + tag + " id=\"" + id +
                                     "\" source=\"" + source + "\" code=\"" +
                                     code + "\"/>";
            xml.replace(start, stop - start, repl);
            pos = start + repl.size();
        } else {
            // Drop the alias with its indentation and line break, so the file
            // stays well-formed and exactly one binding survives.
            size_t from = start;
            while (from > 0 && (xml[from - 1] == ' ' || xml[from - 1] == '\t'))
                from--;
            size_t to = stop;
            if (to < xml.size() && xml[to] == '\r')
                to++;
            if (to < xml.size() && xml[to] == '\n')
                to++;
            xml.erase(from, to - from);
            pos = from;
        }
        found++;
    }
    return found;
}

// The one rewrite head-look needs. Separate from set_binding so the policy --
// which binding, which key, hold not toggle -- lives in one place and the
// mechanism stays general.
inline int bind_headlook(std::string &xml, std::string *old_desc = nullptr) {
    return set_binding(xml, "INPUT_STATE_CAMERA_MOUSELOOK",
                       "INPUT_SOURCE_KEYBOARD", headlook_code(), old_desc);
}

} // namespace x4vr
