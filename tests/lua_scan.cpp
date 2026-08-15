// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The Lua chunk scan, checked without X4.
//
// One thing here is a genuine hazard rather than a formality: a Lua chunk is a
// buffer with an explicit length and is **not** guaranteed to be
// NUL-terminated. Reaching for strstr() would run off the end of whatever X4
// handed us, in X4's own address space, on a code path that runs for every
// chunk the game loads. So the scan is length-bounded and the test drives it
// against an unterminated buffer on purpose.
#include <cstdio>
#include <cstring>
#include <string>

#include "../injector/x4vr_lua.hpp"

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        g_fail++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}

int main() {
    {
        const char *src = "local c = GetCameraRotation()\nreturn c";
        const size_t n = strlen(src);
        check(x4vr::lua_chunk_mentions(src, n, "GetCamera"),
              "finds a needle in the middle of a chunk");
        check(x4vr::lua_chunk_mentions(src, n, "local"),
              "finds one at the very start");
        check(!x4vr::lua_chunk_mentions(src, n, "SetCamera"),
              "does not invent one that is absent");
        check(!x4vr::lua_chunk_mentions(src, n, ""),
              "an empty needle matches nothing rather than everything");
        check(!x4vr::lua_chunk_mentions(nullptr, 10, "Camera"),
              "a null buffer is not dereferenced");
    }

    {
        // The hazard. `buf` is deliberately NOT NUL-terminated, and the bytes
        // after it spell a needle that must not be found: a length-unaware scan
        // would report a hit that exists only past the end of the buffer.
        char pad[64];
        memset(pad, 'x', sizeof(pad));
        memcpy(pad, "local a = 1", 11);
        memcpy(pad + 11, "SetCamera", 9); // beyond the length we will pass
        check(!x4vr::lua_chunk_mentions(pad, 11, "SetCamera"),
              "does not read past the buffer length");
        check(x4vr::lua_chunk_mentions(pad, 11, "local a"),
              "but still finds what is inside it");
        check(!x4vr::lua_chunk_mentions(pad, 3, "local a"),
              "a needle longer than the buffer is not a match");
    }

    {
        // Chunk names carry Lua's source prefix; the path is what identifies it.
        check(!strcmp(x4vr::lua_chunk_name("@ui/core/init.lua"), "ui/core/init.lua"),
              "strips the '@' file prefix");
        check(!strcmp(x4vr::lua_chunk_name("=[C]"), "[C]"),
              "strips the '=' literal prefix");
        check(!strcmp(x4vr::lua_chunk_name("plain"), "plain"),
              "leaves an unprefixed name alone");
        check(!strcmp(x4vr::lua_chunk_name(nullptr), "(anonymous)"),
              "names a null chunk rather than crashing on it");
        check(!strcmp(x4vr::lua_chunk_name(""), "(anonymous)"),
              "and an empty one too");
    }

    {
        size_t n = 0;
        const x4vr::LuaProbe *p = x4vr::lua_probes(&n);
        check(n > 0 && p, "there are probes to run");
        bool every_one_explains_itself = true;
        for (size_t i = 0; i < n; i++)
            if (!p[i].needle || !*p[i].needle || !p[i].why || !*p[i].why)
                every_one_explains_itself = false;
        // A probe without a stated reason is a probe nobody can judge the
        // result of, which is how a hit list turns into a guess list.
        check(every_one_explains_itself, "every probe states what a hit means");
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
