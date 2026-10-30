// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_uimouse.hpp -- X4VR_HUD_MOUSE: X4's UI library takes the mouse where
// the HUD is DRAWN (option A; the route take 373's hardware watch found).
//
// **Why.** The canvas map draws X4's perspective UI at A (0.5252) about the
// view's centre (#76: X4 lays its UI out for its default field and draws it
// with the world's, 1.904x out; the map pulls it back). X4 hit-tests its
// widgets at its own scale, so with the cursor drawn raw (the cockpit) a HUD
// element answered only at 1.904x its drawn offset. Takes 369, 371 and 372
// corrected the presentations' local pointer (Lua's GetLocalMousePosition),
// X4's UI cast, and both: the highlight never moved.
//
// **Where (take 373).** A read/write hardware watch on X4's pointer named a
// per-frame reader the static search had missed: 0xa704a0, slot 2 of the
// vtable at 0x3c4f690, reads the centred pointer (the mouse object's
// +0x38/+0x30, or the last click's +0x48/+0x40 while a button is held),
// converts it to floats in xmm0 (x) and xmm1 (y), and at 0xa70725 calls slot
// 3 of the same object -- 0x2317e00, the UI library's mouse setter
// (`unpcklps; mov %sil,0xa9(%rdi); movq $0,0x10(%rdi); movlps %xmm0,8(%rdi)`).
// That is the mouse every widget hover and click is tested against.
//
// **What.** The 8 bytes at 0xa70725 (`mov %ebp,%esi; mov %rbx,%rdi; call
// *%r12`) become `jmp stub` + int3s; only 0xa70725 itself is a jump target
// (from 0xa70859), nothing jumps inside. While on, the stub adds the steering
// ring's shift and multiplies by the canvas map's inverse factors, then makes
// X4's call itself and jumps to 0xa7072d. The stack is as X4 left it at the
// call (r11 is pushed and popped before it). Off, X4's floats pass untouched.
//
// Self-contained so tests/uimouse.cpp can check it: the sites against the
// real binary, the stub EXECUTED with X4's own setter bytes behind it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace x4vr::uimouse {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0xa704a0: the prologue, rbx = the object, r14 = [*0x3daaaf8 + 0x64b0]
// (the mouse object), none -> 0xa70930.
inline constexpr uint8_t kEntryBytes[] = {
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x55, 0x53, 0x48, 0x89,
    0xfb, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x40, 0xa6, 0x33, 0x03,
    0x4c, 0x8b, 0xb0, 0xb0, 0x64, 0x00, 0x00, 0x4d, 0x85, 0xf6, 0x0f, 0x84,
    0x68, 0x04, 0x00, 0x00};
// 0xa706fa: the button-held path, last click's +0x48 (y) and +0x40 (x) to
// floats, then THE patched 8 bytes at 0xa70725 and X4's next instructions.
inline constexpr uint8_t kClickPathBytes[] = {
    0x4c, 0x89, 0xf7, 0xff, 0x50, 0x48, 0x66, 0x0f, 0xef, 0xc9, 0x4c, 0x89,
    0xf7, 0xf3, 0x0f, 0x2a, 0xc8, 0x49, 0x8b, 0x06, 0xf3, 0x0f, 0x11, 0x4c,
    0x24, 0x04, 0xff, 0x50, 0x40, 0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x10,
    0x4c, 0x24, 0x04, 0xf3, 0x0f, 0x2a, 0xc0, 0x89, 0xee, 0x48, 0x89, 0xdf,
    0x41, 0xff, 0xd4, 0x31, 0xd2, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x48};
// 0xa7082e: the live path, +0x38 (y) and +0x30 (x) to floats, jmp 0xa70725.
inline constexpr uint8_t kLivePathBytes[] = {
    0x4c, 0x89, 0xf7, 0xff, 0x50, 0x38, 0x66, 0x0f, 0xef, 0xc9, 0x4c, 0x89,
    0xf7, 0xf3, 0x0f, 0x2a, 0xc8, 0x49, 0x8b, 0x06, 0xf3, 0x0f, 0x11, 0x4c,
    0x24, 0x04, 0xff, 0x50, 0x30, 0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x10,
    0x4c, 0x24, 0x04, 0xf3, 0x0f, 0x2a, 0xc0, 0xe9, 0xc7, 0xfe, 0xff, 0xff};
// 0x2317e00: the UI library's setter, the call's target through slot 3.
inline constexpr uint8_t kSetterBytes[] = {
    0x0f, 0x14, 0xc1, 0x40, 0x88, 0xb7, 0xa9, 0x00, 0x00, 0x00, 0x48, 0xc7,
    0x47, 0x10, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x13, 0x47, 0x08, 0xc3};

inline constexpr Site kEntry = {0xa704a0, sizeof kEntryBytes, kEntryBytes};
inline constexpr Site kClickPath = {0xa706fa, sizeof kClickPathBytes, kClickPathBytes};
inline constexpr Site kLivePath = {0xa7082e, sizeof kLivePathBytes, kLivePathBytes};
inline constexpr Site kSetter = {0x2317e00, sizeof kSetterBytes, kSetterBytes};

constexpr uintptr_t kPatch = 0xa70725; // 8 bytes
constexpr size_t kPatchLen = 8;
constexpr uintptr_t kCont = 0xa7072d;
constexpr uintptr_t kVtable = 0x3c4f690;  // slot 2 = 0xa704a0, slot 3 = the setter
constexpr uintptr_t kSetter2 = 0x2317e00;

template <class Reader> bool same_bytes(Reader read, const Site &s, uintptr_t *bad) {
    uint8_t b[256];
    if (s.n > sizeof b || !read(s.va, b, s.n)) {
        *bad = s.va;
        return false;
    }
    for (uint8_t i = 0; i < s.n; i++)
        if (b[i] != s.bytes[i]) {
            *bad = s.va + i;
            return false;
        }
    return true;
}

// Every byte the patch depends on; the live path's jmp lands on the patch;
// the vtable's slots 2 and 3 are this function and the setter.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kEntry, &kClickPath, &kLivePath, &kSetter})
        if (!same_bytes(read, *s, bad))
            return false;
    int32_t rel;
    memcpy(&rel, kLivePathBytes + sizeof kLivePathBytes - 4, 4);
    if (kLivePath.va + kLivePath.n + (intptr_t)rel != kPatch ||
        kClickPath.va + 43 != kPatch) {
        *bad = kPatch;
        return false;
    }
    uint64_t slot2 = 0, slot3 = 0;
    if (!read(kVtable + 0x10, &slot2, 8) || !read(kVtable + 0x18, &slot3, 8) ||
        slot2 != kEntry.va || slot3 != kSetter2) {
        *bad = kVtable;
        return false;
    }
    return true;
}

// What the stub reads and counts: `add` and `scale` as float pairs, `on`;
// `calls` every pass, `scaled` the passes that moved the mouse.
struct alignas(16) Slot {
    std::atomic<uint64_t> add{0};   // 0: x, y (X4's window pixels)
    std::atomic<uint64_t> scale{0}; // 8: x, y
    std::atomic<uint32_t> on{0};    // 16
    uint32_t pad = 0;
    std::atomic<uint64_t> scaled{0}; // 24
    std::atomic<uint64_t> calls{0};  // 32
};
static_assert(offsetof(Slot, scale) == 8 && offsetof(Slot, on) == 16 &&
                  offsetof(Slot, scaled) == 24 && offsetof(Slot, calls) == 32,
              "the stub's displacements");

inline uint64_t pack2(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return (uint64_t)x | ((uint64_t)y << 32);
}
inline float unscale_of(float a) {
    return (a > 0.05f && a <= 1.0f) ? 1.0f / a : 1.0f;
}

//     push %r11 ; mov $slot,%r11
//     lock incq 32(%r11)                     calls
//     cmpl $0,16(%r11) ; je 1f
//     lock incq 24(%r11)                     scaled
//     addss (%r11),%xmm0 ; addss 4(%r11),%xmm1      + the ring's shift
//     mulss 8(%r11),%xmm0 ; mulss 12(%r11),%xmm1    x 1/A
// 1:  pop %r11
//     mov %ebp,%esi ; mov %rbx,%rdi ; call *%r12    X4's call, its stack
//     jmp 0xa7072d
constexpr size_t kStubMax = 96;
inline size_t make_stub(uint8_t out[kStubMax], uintptr_t stub_va, uintptr_t slot,
                        uintptr_t cont) {
    size_t i = 0;
    auto b = [&](std::initializer_list<uint8_t> v) {
        for (uint8_t x : v)
            out[i++] = x;
    };
    b({0x41, 0x53});                         // push %r11
    b({0x49, 0xbb});                         // mov $slot,%r11
    memcpy(out + i, &slot, 8);
    i += 8;
    b({0xf0, 0x49, 0xff, 0x43, 0x20});       // lock incq 32(%r11)
    b({0x41, 0x83, 0x7b, 0x10, 0x00});       // cmpl $0,16(%r11)
    b({0x74});                               // je 1f
    const size_t j = i++;
    b({0xf0, 0x49, 0xff, 0x43, 0x18});       // lock incq 24(%r11)
    b({0xf3, 0x41, 0x0f, 0x58, 0x03});       // addss (%r11),%xmm0
    b({0xf3, 0x41, 0x0f, 0x58, 0x4b, 0x04}); // addss 4(%r11),%xmm1
    b({0xf3, 0x41, 0x0f, 0x59, 0x43, 0x08}); // mulss 8(%r11),%xmm0
    b({0xf3, 0x41, 0x0f, 0x59, 0x4b, 0x0c}); // mulss 12(%r11),%xmm1
    out[j] = (uint8_t)(i - (j + 1));
    b({0x41, 0x5b});                         // 1: pop %r11
    b({0x89, 0xee});                         // mov %ebp,%esi
    b({0x48, 0x89, 0xdf});                   // mov %rbx,%rdi
    b({0x41, 0xff, 0xd4});                   // call *%r12
    b({0xe9});                               // jmp cont
    const int64_t rel = (int64_t)cont - (int64_t)(stub_va + i + 4);
    const int32_t r32 = (int32_t)rel;
    memcpy(out + i, &r32, 4);
    i += 4;
    return rel == (int64_t)r32 ? i : 0;
}

// The 8 bytes at 0xa70725: jmp stub, then int3s.
inline bool patch_bytes(uintptr_t stub_va, uint8_t out[kPatchLen]) {
    const int64_t rel = (int64_t)stub_va - (int64_t)(kPatch + 5);
    if (rel != (int64_t)(int32_t)rel)
        return false;
    const int32_t r32 = (int32_t)rel;
    out[0] = 0xe9;
    memcpy(out + 1, &r32, 4);
    memset(out + 5, 0xcc, kPatchLen - 5);
    return true;
}

} // namespace x4vr::uimouse
