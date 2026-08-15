// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_lua.hpp — what to report from X4's Lua, and why this is reconnaissance.
//
// #33 works but drives **open-loop**: we command X4's camera through synthesised
// input and cannot ask where it ended up. Every defect still standing is that
// one defect wearing different clothes — the gain is unmeasured, the clamp winds
// up for real, and a seat change or save load recentres X4 while our estimate
// does not follow. A readback makes all three self-correcting; a direct camera
// write skips the channel entirely. Either would come from here.
//
// **Route 1 only.** `ui/core/lualibs/` holds `.so` files with `.sig` companions
// and `extensions/` registers a mod; both would flag a save as modified, and
// Patola's saves are clean and must stay that way. Interposing LuaJIT touches no
// game file at all.
//
// The reconnaissance is cheap because X4 hands us the source. Every chunk it
// loads passes through `luaL_loadbuffer` as a plain buffer, so the same hook
// that will one day inject can, today, say *which chunk mentions the camera*.
// That is the question the whole route turns on and it costs one run.
//
// Feasibility was established from the binary before any of this was written:
// X4 links `libluajit-5.1.so.2` **dynamically**, from `/usr/lib`, and imports 80
// Lua symbols including `luaL_loadbuffer`, `luaL_loadfile`, `luaL_loadstring`,
// `lua_load`, `lua_pcall` and `luaopen_ffi`. `nm -D` first, per the standing
// rule — and per its harder sibling, an import is not a call, so the hook logs
// whether it fires at all before anything is built on it.
#pragma once

#include <cstddef>
#include <cstring>

namespace x4vr {

// What a camera readback would look like in X4's own vocabulary. Deliberately
// mixed case and mixed specificity: X4's UI Lua calls into C through FFI, so the
// declarations carry C names, while the Lua side uses its own. A hit is a
// pointer to a chunk worth extracting, not an answer.
struct LuaProbe {
    const char *needle;
    const char *why;
};

inline const LuaProbe *lua_probes(size_t *n) {
    static const LuaProbe probes[] = {
        {"Camera", "any camera API at all — the broadest net"},
        {"camera", "same, lower case for Lua-side locals and fields"},
        {"cockpit", "the view #33 drives; take 117's clamp lives in one"},
        {"Cockpit", "same, C/FFI spelling"},
        {"FreeLook", "the exact feature we synthesise input for"},
        {"freelook", "same, Lua spelling"},
        {"MouseLook", "INPUT_STATE_CAMERA_MOUSELOOK's likely C name"},
        {"GetCamera", "a getter is the readback the open loop needs"},
        {"SetCamera", "a setter is the direct write that skips input"},
        {"Rotation", "how an orientation would be named"},
        {"Quaternion", "or how it would be typed"},
        {"FieldOfView", "adjacent, and confirms the camera block is reachable"},
    };
    *n = sizeof(probes) / sizeof(probes[0]);
    return probes;
}

// Does this buffer mention `needle`? The buffer is Lua source, not a C string:
// it has an explicit length and is **not** guaranteed to be NUL-terminated, so
// strstr() on it would run off the end. Chunks are large and there are many, so
// this is a plain scan rather than anything clever — it runs once per chunk at
// load time, not per frame.
inline bool lua_chunk_mentions(const char *buf, size_t sz, const char *needle) {
    if (!buf || !needle)
        return false;
    const size_t n = strlen(needle);
    if (n == 0 || sz < n)
        return false;
    for (size_t i = 0; i + n <= sz; i++)
        if (memcmp(buf + i, needle, n) == 0)
            return true;
    return false;
}

// Lua chunk names arrive with a source prefix: '@' for a file, '=' for a
// caller-supplied literal name, and neither for a string chunk. The prefix is
// noise in a log and the path is what identifies the chunk.
inline const char *lua_chunk_name(const char *name) {
    if (!name || !*name)
        return "(anonymous)";
    if (*name == '@' || *name == '=')
        return name + 1;
    return name;
}

} // namespace x4vr
