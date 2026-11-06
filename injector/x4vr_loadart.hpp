// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_loadart.hpp -- X4VR_LOADING_ART: X4's loading screen shows the mod's art in a VR
// session (Patola asked for it, and approved this patch, 2026-11-06).
//
// **Why a patch.** X4 builds assets/textures/ui/loadingscreens/x4_<shape> from the window's
// aspect at 0x2022e00 and asks the lookup at 0x1067980 for it: the base game, then each
// extension in load order, the last that has the file winning. But that call's last
// argument asks for Egosoft's extensions only: 0x1067a9c skips any whose record has +0xc8
// clear, the field X4's own Lua export calls `egosoftextension`. So our extension's
// ext_01.cat was never looked at, whatever the load order (takes 408, 409).
//
// **What.** One byte: the `push 1` at 0x202317b, that argument, becomes `push 0`. The lookup
// has no other caller but the second call just below it, for the _beta pictures when
// beta.dat exists, which stays as it is: a beta build shows Egosoft's beta art. Other
// extensions' art now counts too; ours loads after the DLCs, and a mod that loads after
// ours showing its own art hurts nothing.
//
// Setting +0xc8 on our own record would need no code patch, but X4 reads that flag elsewhere
// too (0xfb4cf4, 0x106caea, the Lua export), and the extension isn't Egosoft's.
//
// Self-contained so the launcher can run the same checks over the file.
#pragma once

#include <cstddef>
#include <cstdint>

namespace x4vr::loadart {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0x202317b: the first call's arguments, last to first, and the call: `push 1` (Egosoft's
// only), the path's length, "dds jpg" in r8, the result in rdi, `push 0`, `push 1` (load
// order), the extension list in rsi, `call 0x1067980`.
inline constexpr uint8_t kCallBytes[] = {
    0x6a, 0x01, 0x48, 0x89, 0xc2, 0x4c, 0x8d, 0x05, 0xe6, 0xe9, 0xdf, 0x00, 0x4c, 0x89, 0xef, 0x6a,
    0x00, 0x6a, 0x01, 0x48, 0x8b, 0x35, 0xeb, 0x74, 0xd8, 0x01, 0xe8, 0xe6, 0x47, 0x04, 0xff};
inline constexpr Site kCall = {0x202317b, sizeof kCallBytes, kCallBytes};

// 0x10679ca: the lookup keeps that argument, +0x20 on its stack, at -0x10b(%rbp).
inline constexpr uint8_t kArgBytes[] = {0x0f, 0xb6, 0x45, 0x20, 0x4c, 0x89, 0x85, 0xd0, 0xfe,
                                        0xff, 0xff, 0x88, 0x85, 0xf5, 0xfe, 0xff, 0xff};
inline constexpr Site kArg = {0x10679ca, sizeof kArgBytes, kArgBytes};

// 0x1067a93: set, an extension with +0xc8 clear is skipped (0x1067bf5, the next one).
inline constexpr uint8_t kFilterBytes[] = {0x80, 0xbd, 0xf5, 0xfe, 0xff, 0xff, 0x00, 0x74,
                                           0x0e, 0x41, 0x80, 0xbe, 0xc8, 0x00, 0x00, 0x00,
                                           0x00, 0x0f, 0x84, 0x4b, 0x01, 0x00, 0x00};
inline constexpr Site kFilter = {0x1067a93, sizeof kFilterBytes, kFilterBytes};

// The file types the first call passes: only the picture lookup asks for these.
inline constexpr uint8_t kTypesBytes[] = {'d', 'd', 's', ' ', 'j', 'p', 'g', 0};
inline constexpr Site kTypes = {0x2e21b6d, sizeof kTypesBytes, kTypesBytes};

// The byte that changes: the immediate of the push at kCall, 1 to 0.
inline constexpr uintptr_t kPatch = kCall.va + 1;
inline constexpr uint8_t kPatched = 0;

template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kCall, &kArg, &kFilter, &kTypes}) {
        uint8_t b[64];
        if (s->n > sizeof b || !read(s->va, b, s->n)) {
            *bad = s->va;
            return false;
        }
        for (uint8_t i = 0; i < s->n; i++)
            if (b[i] != s->bytes[i]) {
                *bad = s->va + i;
                return false;
            }
    }
    return true;
}

} // namespace x4vr::loadart
