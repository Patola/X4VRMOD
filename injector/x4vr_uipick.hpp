// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_uipick.hpp -- X4VR_HUD_PICK: X4's HUD and its other perspective UI
// take clicks where they are DRAWN (Patola, after take 368, option A).
//
// **Why.** The canvas map (task #30/#40/#82) draws X4's perspective UI --
// the cockpit HUD, its monitors, the conversation choices -- at about half
// size about the view's centre (A = 0.5252), and leaves the orthographic UI
// (object markers) alone. X4 does not know: it hit-tests its UI at its own
// scale. In menus the cursor is drawn through the same map, so drawn cursor
// and hit point agree; in the cockpit the cursor is drawn raw (where X4 picks
// the WORLD, take 314), so a HUD element answers only at 1/A = 1.9x its drawn
// offset (take 368: the alert monitor ignored clicks at +212..+267,+260..+359
// and took +489,+583; Escape took -103,+674 drawn just below the centre).
//
// **Where.** 0xa2ba18 hands X4's centred pointer (the mouse object's
// +0x30/+0x38) to every UI presentation, calling 0xa40ad0(obj, cx, cy) at
// 0xa2bab8 and 0xa2bb47 -- its only two callers. 0xa40ad0 stores the pointer
// as floats at obj+0xdd8, then maps it into the presentation by type:
//   * obj+0x83 == 5: from the player's look, not the pointer (left alone,
//     it branches away before the patch);
//   * obj+0xd78: a presentation with its own perspective camera (tan of its
//     field, obj+0x8);
//   * obj+0xde3: a presentation placed in 3D, hit by a ray from X4's camera;
//   * neither: flat, the pointer as it is.
// The two perspective kinds are what the canvas map scales; the flat one is
// what it leaves. Every hover and click of the UI reads obj+0xdd8 (65 sites).
//
// **What.** The 21 bytes at 0xa40af3 (movq %xmm1,%xmm1; cmpb $0,0xd78(%rdi);
// cvtdq2ps %xmm1,%xmm1; movlps %xmm1,0xdd8(%rbx)) become `jmp stub` + int3s;
// the stub ends by repeating the cmpb and jumping to X4's `je` at 0xa40b08,
// which therefore sees X4's own flags. Nothing in .text jumps into the 21
// bytes (objdump, 2026-10-30). While the slot is on:
//     pointer += add                (the steering ring's nose, X4VR_STEER_PICK)
//     pointer *= scale  if d78/de3  (1/A: the HUD answers where it is drawn)
// Off, the stub stores exactly what X4 would. xmm0 is free (0xa40ad0 takes
// no float argument and overwrites xmm0 on every path before reading it);
// r11 is saved.
//
// Self-contained so tests/uipick.cpp can check it: the sites against the
// real binary, the stub EXECUTED.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x4vr::uipick {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0xa40ad0 up to X4's je at 0xa40b08: push; movd %esi,%xmm1; push;
// pinsrd $1,%edx,%xmm1; mov %rdi,%rbx; sub $0x88,%rsp; cmpb $5,0x83(%rdi);
// je 0xa40f20; then THE patched 21 bytes; then je 0xa40be8.
inline constexpr uint8_t kEntryBytes[] = {
    0x55, 0x66, 0x0f, 0x6e, 0xce, 0x53, 0x66, 0x0f, 0x3a, 0x22, 0xca, 0x01,
    0x48, 0x89, 0xfb, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x80, 0xbf,
    0x83, 0x00, 0x00, 0x00, 0x05, 0x0f, 0x84, 0x2d, 0x04, 0x00, 0x00, 0xf3,
    0x0f, 0x7e, 0xc9, 0x80, 0xbf, 0x78, 0x0d, 0x00, 0x00, 0x00, 0x0f, 0x5b,
    0xc9, 0x0f, 0x13, 0x8b, 0xd8, 0x0d, 0x00, 0x00, 0x0f, 0x84, 0xda, 0x00,
    0x00, 0x00};
// 0xa40be8: the 3D-placed test, cmpb $0,0xde3(%rdi); movaps %xmm1,(%rsp);
// je 0xa40bd7 (flat: return with obj+0xdd8 as stored).
inline constexpr uint8_t kFlatBytes[] = {0x80, 0xbf, 0xe3, 0x0d, 0x00, 0x00, 0x00,
                                         0x0f, 0x29, 0x0c, 0x24, 0x74, 0xe2};
// 0xa2baae and 0xa2bb3d: the two callers, edx = [rsp+0x30] (centred y),
// esi = r13d (centred x), call 0xa40ad0.
inline constexpr uint8_t kCaller1Bytes[] = {0x8b, 0x54, 0x24, 0x30, 0x44, 0x89,
                                            0xee, 0x48, 0x89, 0xef, 0xe8, 0x13,
                                            0x50, 0x01, 0x00};
inline constexpr uint8_t kCaller2Bytes[] = {0x8b, 0x54, 0x24, 0x30, 0x44, 0x89,
                                            0xee, 0x48, 0x89, 0xdf, 0xe8, 0x84,
                                            0x4f, 0x01, 0x00};

inline constexpr Site kEntry = {0xa40ad0, sizeof kEntryBytes, kEntryBytes};
inline constexpr Site kFlat = {0xa40be8, sizeof kFlatBytes, kFlatBytes};
inline constexpr Site kCaller1 = {0xa2baae, sizeof kCaller1Bytes, kCaller1Bytes};
inline constexpr Site kCaller2 = {0xa2bb3d, sizeof kCaller2Bytes, kCaller2Bytes};

constexpr uintptr_t kFunc = 0xa40ad0;
constexpr uintptr_t kPatch = 0xa40af3; // 21 bytes become jmp stub + int3s
constexpr size_t kPatchLen = 21;
constexpr uintptr_t kCont = 0xa40b08;  // X4's je on the d78 flag
constexpr uint32_t kOwnCamera = 0xd78, kPlaced = 0xde3, kPointer = 0xdd8;

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

inline int32_t rel32_at(const uint8_t *bytes, size_t off) {
    int32_t r;
    memcpy(&r, bytes + off, 4);
    return r;
}

// Every byte the patch depends on, and what the jumps decode to: type 5
// leaves before the patch, X4's je goes to the 3D-placed test, that test's
// je returns, and both callers call 0xa40ad0. `*bad` names the first
// difference.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kEntry, &kFlat, &kCaller1, &kCaller2})
        if (!same_bytes(read, *s, bad))
            return false;
    struct Want {
        uintptr_t end, target;
        int32_t rel;
    };
    const Want w[] = {
        {0xa40af3, 0xa40f20, rel32_at(kEntryBytes, 0x1f)},
        {0xa40b0e, 0xa40be8, rel32_at(kEntryBytes, 0x3a)},
        {0xa40bf5, 0xa40bd7, (int8_t)kFlatBytes[12]},
        {0xa2babd, kFunc, rel32_at(kCaller1Bytes, 11)},
        {0xa2bb4c, kFunc, rel32_at(kCaller2Bytes, 11)},
    };
    for (const Want &x : w)
        if (x.end + (intptr_t)x.rel != x.target) {
            *bad = x.end;
            return false;
        }
    if (kPatch + kPatchLen != kCont || kEntry.va + kEntry.n != kCont + 6) {
        *bad = kPatch;
        return false;
    }
    return true;
}

// What the stub reads and counts. `add` and `scale` are two floats each,
// packed so one 8-byte store publishes a pair; `on` gates both. `calls`
// counts every pass, `scaled` the passes that scaled a perspective
// presentation, `flat` those that only added to a flat one.
struct alignas(16) Slot {
    std::atomic<uint64_t> add{0};   // x, y in X4's window pixels
    std::atomic<uint64_t> scale{0}; // x, y factors
    std::atomic<uint32_t> on{0};
    uint32_t pad = 0;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> scaled{0};
    std::atomic<uint64_t> flat{0};
};
static_assert(offsetof(Slot, scale) == 8 && offsetof(Slot, on) == 16 &&
                  offsetof(Slot, calls) == 24 && offsetof(Slot, scaled) == 32 &&
                  offsetof(Slot, flat) == 40,
              "the stub's displacements");

inline uint64_t pack2(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return (uint64_t)x | ((uint64_t)y << 32);
}

// The scale for the canvas map's x/y factors: 1/a, or 1 for a factor that
// is not a usable contraction (absent, non-finite, or not in (0, 1]).
inline float unscale_of(float a) {
    return (a > 0.05f && a <= 1.0f) ? 1.0f / a : 1.0f;
}

//     movq %xmm1,%xmm1 ; cvtdq2ps %xmm1,%xmm1     X4's, the cmpb moved below
//     push %r11 ; mov $slot,%r11
//     lock incq 24(%r11)                         calls
//     cmpl $0,16(%r11) ; je 2f                   off: X4's own values
//     movq (%r11),%xmm0 ; addps %xmm0,%xmm1      += add
//     movq 8(%r11),%xmm0
//     cmpb $0,0xd78(%rdi) ; jne 1f
//     cmpb $0,0xde3(%rdi) ; je 3f
// 1:  mulps %xmm0,%xmm1 ; lock incq 32(%r11) ; jmp 2f    perspective: *= scale
// 3:  lock incq 40(%r11)                                 flat: add only
// 2:  pop %r11
//     movlps %xmm1,0xdd8(%rbx)                   X4's store
//     cmpb $0,0xd78(%rdi)                        X4's flags for its je
//     jmp 0xa40b08
constexpr size_t kStubMax = 128;
inline size_t make_stub(uint8_t out[kStubMax], uintptr_t stub_va, uintptr_t slot,
                        uintptr_t cont) {
    size_t i = 0;
    auto b = [&](uint8_t v) { out[i++] = v; };
    auto q = [&](uint64_t v) {
        memcpy(out + i, &v, 8);
        i += 8;
    };
    auto cmpb = [&](uint32_t off) { // cmpb $0, off(%rdi)
        b(0x80), b(0xbf);
        memcpy(out + i, &off, 4);
        i += 4;
        b(0x00);
    };
    b(0xf3), b(0x0f), b(0x7e), b(0xc9);          // movq %xmm1,%xmm1
    b(0x0f), b(0x5b), b(0xc9);                   // cvtdq2ps %xmm1,%xmm1
    b(0x41), b(0x53);                            // push %r11
    b(0x49), b(0xbb), q(slot);                   // mov $slot,%r11
    b(0xf0), b(0x49), b(0xff), b(0x43), b(0x18); // lock incq 24(%r11)
    b(0x41), b(0x83), b(0x7b), b(0x10), b(0x00); // cmpl $0,16(%r11)
    b(0x74);                                     // je 2f
    const size_t j_off = i++;
    b(0xf3), b(0x41), b(0x0f), b(0x7e), b(0x03); // movq (%r11),%xmm0
    b(0x0f), b(0x58), b(0xc8);                   // addps %xmm0,%xmm1
    b(0xf3), b(0x41), b(0x0f), b(0x7e), b(0x43), b(0x08); // movq 8(%r11),%xmm0
    cmpb(kOwnCamera);
    b(0x75);                                     // jne 1f
    const size_t j_scale = i++;
    cmpb(kPlaced);
    b(0x74);                                     // je 3f
    const size_t j_flat = i++;
    out[j_scale] = (uint8_t)(i - (j_scale + 1));
    b(0x0f), b(0x59), b(0xc8);                   // 1: mulps %xmm0,%xmm1
    b(0xf0), b(0x49), b(0xff), b(0x43), b(0x20); // lock incq 32(%r11)
    b(0xeb);                                     // jmp 2f
    const size_t j_done = i++;
    out[j_flat] = (uint8_t)(i - (j_flat + 1));
    b(0xf0), b(0x49), b(0xff), b(0x43), b(0x28); // 3: lock incq 40(%r11)
    out[j_off] = (uint8_t)(i - (j_off + 1));
    out[j_done] = (uint8_t)(i - (j_done + 1));
    b(0x41), b(0x5b);                            // 2: pop %r11
    b(0x0f), b(0x13), b(0x8b);                   // movlps %xmm1,0xdd8(%rbx)
    const uint32_t ptr = kPointer;
    memcpy(out + i, &ptr, 4);
    i += 4;
    cmpb(kOwnCamera);                            // X4's flags for its je
    b(0xe9);                                     // jmp cont
    const int64_t rel = (int64_t)cont - (int64_t)(stub_va + i + 4);
    const int32_t r32 = (int32_t)rel;
    memcpy(out + i, &r32, 4);
    i += 4;
    return rel == (int64_t)r32 ? i : 0; // 0: cont out of rel32 reach
}

// The 21 bytes written at 0xa40af3: jmp stub, then int3s.
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

} // namespace x4vr::uipick
