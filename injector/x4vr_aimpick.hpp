// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_aimpick.hpp -- X4VR_STEER_PICK: in mouse steering, X4 targets where
// the steering ring is drawn (Patola, after take 366, option (b): "the ring
// does everything", approved 2026-10-29).
//
// **Why a patch.** In steering mode the ring (x4vr_steer.hpp, A) is drawn at
// the nose plus the pointer's offset, fixed to the cockpit; X4 targets at
// the gaze plus that offset, so with the head turned by an angle, highlight
// and click land that angle away from the ring (take 366). X4 keeps ONE
// centred pointer -- `pointer - reference`, the mouse object's accessors
// +0x30/+0x38 -- and both the ship's steering (0x166d236) and the world
// pick ray (0x1d6e080) read it. So no change to the pointer or to its
// reference can move one without the other; the pick has to be told.
//
// **Where.** 0x1d6e080 builds the ray from X4's camera through the centred
// pointer. Its four callers: hover twice (0x1257ade, 0x1379ba7), click-select
// with the last-click position (0x13724e9, cl=1), IsPointingWithinAimingRange.
// Its three paths -- the live pointer, the last click, no mouse object --
// meet at 0x1d6e0e1 with r13d = centred x and ebx = centred y, in X4's window
// pixels (y down); the ray then takes them over (window * 0.5), so a pixel
// is NDC * window / 2.
//
// **What.** The 11 bytes there (`pxor %xmm0,%xmm0`; `movaps 0x2e245e0,%xmm2`)
// become `jmp stub` + six int3s. Only 0x1d6e0e1 itself is a jump target in
// .text (from 0x1d6e370 and 0x1d6e3b2; objdump of all of .text, 2026-10-29),
// so the int3s are never reached. The stub adds the published shift to r13d
// and ebx, counts, replays the two instructions and jumps to 0x1d6e0ec.
// Flags are dead there (`test %rax,%rax` at 0x1d6e10f sets them before any
// use); rax (X4's root, loaded on every path) is untouched; r11 is saved.
// A zero shift is X4's own code, byte for byte in effect.
//
// The shift is the nose's position in X4's window pixels -- the same numbers
// the ring is drawn with -- so the ray goes through the ring as drawn.
//
// Self-contained so tests/aimpick.cpp can check it: the sites against the
// real binary, the stub EXECUTED.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x4vr::aimpick {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0x1d6e0a4: the mouse object from [*0x3daaaf8 + 0x64b0] (none -> 0x1d6e3a8),
// cl set -> the last click (0x1d6e358), else centred x (+0x30) into r13d and
// centred y (+0x38) into ebx; then rax = X4's root (0x3db6948).
inline constexpr uint8_t kLiveBytes[] = {
    0x48, 0x8b, 0x05, 0x4d, 0xca, 0x03, 0x02, 0x48, 0x8b, 0x98, 0xb0, 0x64,
    0x00, 0x00, 0x48, 0x85, 0xdb, 0x0f, 0x84, 0xed, 0x02, 0x00, 0x00, 0x48,
    0x8b, 0x03, 0x48, 0x89, 0xdf, 0x84, 0xc9, 0x0f, 0x85, 0x8f, 0x02, 0x00,
    0x00, 0xff, 0x50, 0x30, 0x48, 0x89, 0xdf, 0x41, 0x89, 0xc5, 0x48, 0x8b,
    0x03, 0xff, 0x50, 0x38, 0x89, 0xc3, 0x48, 0x8b, 0x05, 0x67, 0x88, 0x04,
    0x02};
// 0x1d6e0e1: THE patched 11 bytes, then X4's own continuation up to the
// `test %rax,%rax` that sets the flags again -- nothing between reads them.
inline constexpr uint8_t kMergeBytes[] = {
    0x66, 0x0f, 0xef, 0xc0, 0x0f, 0x28, 0x15, 0xf4, 0x64, 0x0b, 0x01, 0x0f,
    0x29, 0x44, 0x24, 0x40, 0xf3, 0x0f, 0x10, 0x05, 0x07, 0x65, 0x0b, 0x01,
    0x0f, 0x29, 0x44, 0x24, 0x50, 0x0f, 0x28, 0x05, 0xeb, 0x64, 0x0b, 0x01,
    0x0f, 0x29, 0x54, 0x24, 0x60, 0x0f, 0x29, 0x44, 0x24, 0x70, 0x48, 0x85,
    0xc0};
// 0x1d6e358: the last click's centred x (+0x40) and y (+0x48), the root,
// jmp 0x1d6e0e1.
inline constexpr uint8_t kClickBytes[] = {
    0xff, 0x50, 0x40, 0x48, 0x89, 0xdf, 0x41, 0x89, 0xc5, 0x48, 0x8b, 0x03,
    0xff, 0x50, 0x48, 0x89, 0xc3, 0x48, 0x8b, 0x05, 0xd8, 0x85, 0x04, 0x02,
    0xe9, 0x6c, 0xfd, 0xff, 0xff};
// 0x1d6e3a8: no mouse object (rbx == 0): the root, r13d = 0, jmp 0x1d6e0e1.
inline constexpr uint8_t kNoMouseBytes[] = {
    0x48, 0x8b, 0x05, 0x99, 0x85, 0x04, 0x02, 0x45, 0x31, 0xed, 0xe9, 0x2a,
    0xfd, 0xff, 0xff};
// 0x1d6e254: where the ray takes them: neg %ebx; cvtsi2ss %ebx,%xmm4;
// cvtsi2ss %r13d,%xmm1; mulss %xmm7,%xmm4.
inline constexpr uint8_t kUseBytes[] = {
    0xf7, 0xdb, 0x66, 0x0f, 0xef, 0xe4, 0x66, 0x0f, 0xef, 0xc9, 0xf3, 0x0f,
    0x2a, 0xe3, 0xf3, 0x41, 0x0f, 0x2a, 0xcd, 0xf3, 0x0f, 0x59, 0xe7};

inline constexpr Site kLive = {0x1d6e0a4, sizeof kLiveBytes, kLiveBytes};
inline constexpr Site kMerge = {0x1d6e0e1, sizeof kMergeBytes, kMergeBytes};
inline constexpr Site kClick = {0x1d6e358, sizeof kClickBytes, kClickBytes};
inline constexpr Site kNoMouse = {0x1d6e3a8, sizeof kNoMouseBytes, kNoMouseBytes};
inline constexpr Site kUse = {0x1d6e254, sizeof kUseBytes, kUseBytes};

constexpr uintptr_t kPatch = 0x1d6e0e1;    // 11 bytes become jmp stub + int3s
constexpr size_t kPatchLen = 11;
constexpr uintptr_t kCont = 0x1d6e0ec;     // X4's next instruction
constexpr uintptr_t kConst = 0x2e245e0;    // the movaps's operand (0,1,0,0)
constexpr uintptr_t kInputGlobal = 0x3daaaf8;
constexpr uintptr_t kClickVa = 0x1d6e358;
constexpr uintptr_t kNoMouseVa = 0x1d6e3a8;

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

inline int32_t rel_at(const uint8_t *bytes, size_t off) {
    int32_t r;
    memcpy(&r, bytes + off, 4);
    return r;
}

// Every byte the patch depends on, and what the jumps in them decode to:
// the three paths reach 0x1d6e0e1, the movaps reads 0x2e245e0, the mouse
// object comes from the input global. `*bad` names the first difference.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kLive, &kMerge, &kClick, &kNoMouse, &kUse})
        if (!same_bytes(read, *s, bad))
            return false;
    struct Want {
        uintptr_t insn_end, target;
        int32_t rel;
    };
    const Want w[] = {
        {kLive.va + 7, kInputGlobal, rel_at(kLiveBytes, 3)},
        {0x1d6e0bb, kNoMouseVa, rel_at(kLiveBytes, 0x1d6e0b7 - kLive.va)},
        {0x1d6e0c9, kClickVa, rel_at(kLiveBytes, 0x1d6e0c5 - kLive.va)},
        {kPatch + kPatchLen, kConst, rel_at(kMergeBytes, 7)},
        {0x1d6e375, kPatch, rel_at(kClickBytes, 25)},
        {0x1d6e3b7, kPatch, rel_at(kNoMouseBytes, 11)},
    };
    for (const Want &x : w)
        if (x.insn_end + (intptr_t)x.rel != x.target) {
            *bad = x.insn_end;
            return false;
        }
    // The live path falls through to the patch: its last instruction (the
    // root's load, 7 bytes) ends exactly there.
    if (kLive.va + kLive.n != kPatch) {
        *bad = kPatch;
        return false;
    }
    return true;
}

// What the stub reads and counts. `shift` packs the pixel shift as two
// int32 -- x low, y high -- so one 64-bit store publishes both; zero means
// X4's own targeting. `calls` counts every pass, `shifted` the passes that
// added a non-zero shift.
struct alignas(16) Slot {
    std::atomic<uint64_t> shift{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> shifted{0};
};
static_assert(sizeof(std::atomic<uint64_t>) == 8, "the stub reads 8 bytes");
static_assert(offsetof(Slot, calls) == 8 && offsetof(Slot, shifted) == 16,
              "the stub's displacements");

inline uint64_t pack(int32_t dx, int32_t dy) {
    return (uint64_t)(uint32_t)dx | ((uint64_t)(uint32_t)dy << 32);
}
inline void unpack(uint64_t v, int32_t *dx, int32_t *dy) {
    *dx = (int32_t)(uint32_t)(v & 0xffffffffu);
    *dy = (int32_t)(uint32_t)(v >> 32);
}

// The nose (eye NDC, x right, y down, as steer::nose_ndc gives it) in X4's
// window pixels, the unit of the centred pointer: NDC * window / 2, rounded.
// False -- no shift -- for a window not known yet or a non-finite nose.
inline bool shift_px(float nose_x, float nose_y, int win_w, int win_h,
                     int32_t *dx, int32_t *dy) {
    if (win_w < 2 || win_h < 2 || !std::isfinite(nose_x) || !std::isfinite(nose_y))
        return false;
    const float fx = nose_x * 0.5f * (float)win_w;
    const float fy = nose_y * 0.5f * (float)win_h;
    // A nose this far off the window is far behind the eye's edge; the
    // layer reports a nose behind the eye as none, so this only guards the
    // int conversion.
    if (std::fabs(fx) > 1e6f || std::fabs(fy) > 1e6f)
        return false;
    *dx = (int32_t)std::lround(fx);
    *dy = (int32_t)std::lround(fy);
    return true;
}

//     push %r11
//     mov  $slot, %r11
//     lock incq 8(%r11)                 ; calls
//     cmpq $0, (%r11) ; je 1f           ; no shift: X4's own values
//     lock incq 16(%r11)                ; shifted
//     add  (%r11), %r13d                ; centred x += dx
//     add  4(%r11), %ebx                ; centred y += dy
// 1:  mov  $0x2e245e0, %r11
//     movaps (%r11), %xmm2              ; X4's movaps, the operand absolute
//     pop  %r11
//     pxor %xmm0, %xmm0                 ; X4's pxor
//     jmp  0x1d6e0ec                    ; rel32 from where the stub sits
constexpr size_t kStubMax = 64;
inline size_t make_stub(uint8_t out[kStubMax], uintptr_t stub_va,
                        uintptr_t slot, uintptr_t constant, uintptr_t cont) {
    size_t i = 0;
    auto b = [&](uint8_t v) { out[i++] = v; };
    auto q = [&](uint64_t v) {
        memcpy(out + i, &v, 8);
        i += 8;
    };
    b(0x41), b(0x53);                        // push %r11
    b(0x49), b(0xbb), q(slot);               // mov $slot, %r11
    b(0xf0), b(0x49), b(0xff), b(0x43), b(0x08); // lock incq 8(%r11)
    b(0x49), b(0x83), b(0x3b), b(0x00);      // cmpq $0, (%r11)
    b(0x74);                                 // je 1f
    const size_t je = i++;
    b(0xf0), b(0x49), b(0xff), b(0x43), b(0x10); // lock incq 16(%r11)
    b(0x45), b(0x03), b(0x2b);               // add (%r11), %r13d
    b(0x41), b(0x03), b(0x5b), b(0x04);      // add 4(%r11), %ebx
    out[je] = (uint8_t)(i - (je + 1));
    b(0x49), b(0xbb), q(constant);           // mov $constant, %r11
    b(0x41), b(0x0f), b(0x28), b(0x13);      // movaps (%r11), %xmm2
    b(0x41), b(0x5b);                        // pop %r11
    b(0x66), b(0x0f), b(0xef), b(0xc0);      // pxor %xmm0, %xmm0
    b(0xe9);                                 // jmp cont
    const int64_t rel = (int64_t)cont - (int64_t)(stub_va + i + 4);
    const int32_t r32 = (int32_t)rel;
    memcpy(out + i, &r32, 4);
    i += 4;
    return rel == (int64_t)r32 ? i : 0; // 0: cont out of rel32 reach
}

// The 11 bytes written at 0x1d6e0e1: jmp stub, then int3s.
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

} // namespace x4vr::aimpick
