// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_riprel.hpp — find the global an X4 accessor reads, from its own bytes.
//
// **Why this exists: X4's crash on exit was ours.** "X4 segfaults on exit in
// EVERY run" sat in the docs for weeks as harmless teardown. Both surviving
// cores have the same stack:
//
//     #0 IsHUDActive     (X4)
//     #1 headlook_tick   (libx4vr_inject.so)
//     #2 SDL_PumpEvents  (libx4vr_inject.so, our interposer)
//
// and the fault registers say exactly why: `rdx = 0` at
//
//     IsHUDActive:    mov    0x3373c71(%rip),%rdx     48 8b 15 <disp32>
//     IsHUDActive+7:  movzbl 0x418(%rdx),%eax         <- SEGV_MAPERR
//
// X4 NULLs the HUD global during shutdown and keeps pumping events, and our
// tick latched "once seen alive it stays alive" (true all through play, false
// at exit). Take 313 is the control: head-look OFF, so headlook_tick never
// reached the call, and it is the one run with no X4 core.
//
// The guard reads the same pointer IsHUDActive is about to dereference and
// skips the call when it is NULL. The address is DECODED from the function's
// first instruction rather than hard-coded, and the decode refuses anything
// but the exact `mov disp32(%rip),%rdx` form -- so an X4 update that changes
// the prologue makes the guard say so instead of reading a wild address.
#pragma once

#include <cstdint>
#include <cstring>

namespace x4vr {

// If `fn` begins with `mov disp32(%rip),%rdx` (REX.W 8B /r, ModRM 0x15), the
// address of the 8-byte slot it loads; otherwise nullptr. RIP-relative
// displacements count from the END of the instruction, which is 7 bytes long.
inline const void *const *riprel_rdx_load_slot(const void *fn) {
    if (!fn)
        return nullptr;
    const uint8_t *p = static_cast<const uint8_t *>(fn);
    if (p[0] != 0x48 || p[1] != 0x8b || p[2] != 0x15)
        return nullptr;
    int32_t disp;
    memcpy(&disp, p + 3, sizeof disp);
    return reinterpret_cast<const void *const *>(p + 7 + disp);
}

} // namespace x4vr
