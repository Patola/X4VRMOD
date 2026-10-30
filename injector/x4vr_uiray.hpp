// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_uiray.hpp -- X4VR_HUD_RAY: X4's UI takes hover and clicks where it is
// DRAWN (Patola, option A after take 368; the route found in take 370).
//
// **Why.** The canvas map draws X4's perspective UI -- the cockpit HUD, its
// monitors, the conversation choices, the minimap -- at A (0.5252) about the
// view's centre; X4 does not know. In the cockpit the cursor is drawn raw,
// so a HUD element answered only at 1/A = 1.9x its drawn offset (take 368).
//
// **Where X4 hit-tests its UI (take 370).** Not through the pointer it hands
// its presentations (take 369 scaled every one of those and moved nothing):
// the hover and the click each build the world pick ray (0x1d6e080, its
// output pointer in rbp) and cast it TWICE through 0x1143b60 -- with filter
// 0x18 for the world (hit record +0x6c50 hover, +0x6c90 click) and 0x17 for
// the UI (+0x6bd0 hover, +0x6c10 click). Take 370's hover log: the UI record
// held a component at distance 0 exactly while a comms choice highlighted,
// at about 2x its drawn offset; the world record held the Cobra.
//
// **What.** Two stubs:
//   A, in 0x1d6e080 at 0x1d6e2b8, where xmm0 = sx*row0, xmm4 = sy*row1 and
//      xmm1 = (1,1,1,0)*row2 (X4's ray before its two addps): while on, it
//      also builds the UI ray d = Sx*xmm0 + Sy*xmm4 + xmm1 -- the same ray
//      through the pointer divided by the canvas factors -- normalises it as
//      X4 does (x,y,z length) and stores d*K (K = X4's 100000, the length the
//      callers' direction has) with rbp, the ray's output pointer, as its
//      key. Then X4's two addps; back at 0x1d6e2be. Uses xmm5-xmm7 (dead
//      from there on every path; xmm2 is read at 0x1d6e463 and left alone).
//   B, at both UI casts (0x137a1d0 hover, 0x1372a0b click): `mov %r15,%rcx;
//      push $0x17` -- rcx is the direction argument, r15 the buffer the
//      caller handed 0x1d6e080 (0x1379b9c, 0x13724d9). While on and the key
//      matches r15, rcx points at the UI direction instead. The world casts
//      are not touched, so ships and stations stay where they are.
// X4VR_STEER_PICK's shift is already in sx, sy (its stub runs earlier in
// 0x1d6e080), so in steering mode the UI ray goes through the ring / A.
//
// Self-contained so tests/uiray.cpp can check it: the sites against the real
// binary, both stubs EXECUTED.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x4vr::uiray {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0x1d6e080's prologue: ..., mov %rsi,%rbp (the output), ..., the frame cache.
inline constexpr uint8_t kRayEntryBytes[] = {
    0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x49, 0x89, 0xd4, 0x55, 0x48, 0x89,
    0xf5, 0x53, 0x48, 0x83, 0xc4, 0x80, 0x8b, 0x05, 0x58, 0xc6, 0x03, 0x02,
    0x39, 0x87, 0xa8, 0x6a, 0x00, 0x00, 0x0f, 0x84, 0x1c, 0x03, 0x00, 0x00};
// 0x1d6e29f: mulps 0x50(%rsp),%xmm0; movlhps; movaps 0x2e27370,%xmm1;
// mulps 0x60(%rsp),%xmm4; mulps 0x70(%rsp),%xmm1; THE two addps (0x1d6e2b8);
// movaps %xmm0,%xmm1; mulps %xmm0,%xmm1 (the length that follows).
inline constexpr uint8_t kRayDirBytes[] = {
    0x0f, 0x59, 0x44, 0x24, 0x50, 0x0f, 0x16, 0xe1, 0x0f, 0x28, 0x0d, 0xc2,
    0x90, 0x0b, 0x01, 0x0f, 0x59, 0x64, 0x24, 0x60, 0x0f, 0x59, 0x4c, 0x24,
    0x70, 0x0f, 0x58, 0xc4, 0x0f, 0x58, 0xc1, 0x0f, 0x28, 0xc8, 0x0f, 0x59,
    0xc8};
// 0x1d6e37d: movss 0x2d67c18,%xmm1 (K); shufps; mulps; addps 0x40(%rsp)
// (the origin); movaps %xmm0,(%rbp) -- the point the callers subtract the
// origin from, so their direction is n*K.
inline constexpr uint8_t kRayOutBytes[] = {
    0xf3, 0x0f, 0x10, 0x0d, 0x93, 0x98, 0xff, 0x00, 0x0f, 0xc6, 0xc9, 0x00,
    0x0f, 0x59, 0xc1, 0x0f, 0x58, 0x44, 0x24, 0x40, 0x0f, 0x29, 0x45, 0x00};
// The hover: lea 0x10(%rsp),%r15 ... mov %r15,%rsi ... call 0x1d6e080.
inline constexpr uint8_t kHoverKeyBytes[] = {
    0x4c, 0x8d, 0x7c, 0x24, 0x10, 0x4c, 0x89, 0xea, 0x64, 0xf2, 0x0f, 0x10,
    0x04, 0x25, 0xd8, 0xd8, 0xff, 0xff, 0xe8, 0xde, 0x03, 0x5b, 0x00, 0x0f,
    0x28, 0x44, 0x24, 0x30, 0x31, 0xc9, 0x4c, 0x89, 0xea, 0x4c, 0x89, 0xfe,
    0x48, 0x89, 0xdf, 0x0f, 0x29, 0x44, 0x24, 0x20, 0xe8, 0xd4, 0x44, 0x9f,
    0x00};
// The hover's UI cast: ... mov %r15,%rcx; push $0x17 (0x137a1d0) ...
// lea 0x6bd0(%rbx),%rsi ... call 0x1143b60.
inline constexpr uint8_t kHoverUiBytes[] = {
    0x48, 0x8d, 0x54, 0x24, 0x20, 0x6a, 0x01, 0x4c, 0x89, 0xf9, 0x6a, 0x17,
    0x48, 0x8d, 0xb3, 0xd0, 0x6b, 0x00, 0x00, 0x41, 0xb9, 0x01, 0x00, 0x00,
    0x00, 0x48, 0x89, 0xef, 0x6a, 0x01, 0x50, 0x6a, 0x00, 0x6a, 0x00, 0xe8,
    0x6f, 0x99, 0xdc, 0xff};
// The click: mov %r15,%rsi; mov $1,%ecx (the last click) ... call 0x1d6e080.
inline constexpr uint8_t kClickKeyBytes[] = {
    0x0f, 0x28, 0x44, 0x24, 0x30, 0x4c, 0x89, 0xea, 0x4c, 0x89, 0xfe, 0xb9,
    0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0x0f, 0x29, 0x44, 0x24, 0x20,
    0xe8, 0x92, 0xbb, 0x9f, 0x00};
// The click's UI cast: mov %r15,%rcx; push $0x17 (0x1372a0b) ...
// lea 0x6c10(%rbx),%rsi ... call 0x1143b60.
inline constexpr uint8_t kClickUiBytes[] = {
    0x48, 0x8d, 0x54, 0x24, 0x20, 0x6a, 0x01, 0x4c, 0x89, 0xf9, 0x6a, 0x17,
    0x48, 0x8d, 0xb3, 0x10, 0x6c, 0x00, 0x00, 0x41, 0xb9, 0x01, 0x00, 0x00,
    0x00, 0x4c, 0x89, 0xe7, 0x6a, 0x01, 0x50, 0x6a, 0x01, 0x6a, 0x00, 0xe8,
    0x34, 0x11, 0xdd, 0xff};

inline constexpr Site kRayEntry = {0x1d6e080, sizeof kRayEntryBytes, kRayEntryBytes};
inline constexpr Site kRayDir = {0x1d6e29f, sizeof kRayDirBytes, kRayDirBytes};
inline constexpr Site kRayOut = {0x1d6e37d, sizeof kRayOutBytes, kRayOutBytes};
inline constexpr Site kHoverKey = {0x1379b7b, sizeof kHoverKeyBytes, kHoverKeyBytes};
inline constexpr Site kHoverUi = {0x137a1c9, sizeof kHoverUiBytes, kHoverUiBytes};
inline constexpr Site kClickKey = {0x13724d1, sizeof kClickKeyBytes, kClickKeyBytes};
inline constexpr Site kClickUi = {0x1372a04, sizeof kClickUiBytes, kClickUiBytes};

constexpr uintptr_t kRayFunc = 0x1d6e080;
constexpr uintptr_t kCast = 0x1143b60;
constexpr uintptr_t kPatchA = 0x1d6e2b8; // 6 bytes: the two addps
constexpr size_t kPatchALen = 6;
constexpr uintptr_t kContA = 0x1d6e2be;
constexpr uintptr_t kPatchHover = 0x137a1d0; // 5 bytes: mov %r15,%rcx; push $0x17
constexpr uintptr_t kPatchClick = 0x1372a0b;
constexpr size_t kPatchBLen = 5;
constexpr uintptr_t kRayLength = 0x2d67c18; // X4's K, 100000

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

// Every byte both stubs depend on and what the calls decode to: both callers
// call 0x1d6e080 with r15 as its output, both UI casts call 0x1143b60, the
// ray's K is read where X4 reads it. `*bad` names the first difference.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kRayEntry, &kRayDir, &kRayOut, &kHoverKey, &kHoverUi,
                          &kClickKey, &kClickUi})
        if (!same_bytes(read, *s, bad))
            return false;
    struct Want {
        uintptr_t end, target;
        int32_t rel;
    };
    const Want w[] = {
        {0x1379bac, kRayFunc, rel32_at(kHoverKeyBytes, 45)},
        {0x13724ee, kRayFunc, rel32_at(kClickKeyBytes, 25)},
        {0x137a1f1, kCast, rel32_at(kHoverUiBytes, 36)},
        {0x1372a2c, kCast, rel32_at(kClickUiBytes, 36)},
        {0x1d6e385, kRayLength, rel32_at(kRayOutBytes, 4)},
    };
    for (const Want &x : w)
        if (x.end + (intptr_t)x.rel != x.target) {
            *bad = x.end;
            return false;
        }
    // The patched spots are where the bytes say they are.
    if (kRayDir.va + 25 != kPatchA || kHoverUi.va + 7 != kPatchHover ||
        kClickUi.va + 7 != kPatchClick) {
        *bad = kPatchA;
        return false;
    }
    return true;
}

// What the stubs read and count. `dir` is the UI direction (x, y, z, 0) * K,
// `key` the 0x1d6e080 output it was built for; `scale` packs the canvas
// map's inverse factors (x, y); `k` is X4's ray length, read from X4 when
// patching; `on` gates both stubs. `built` counts stub A's UI rays,
// `swapped` the UI casts that took one, `passed` those left on X4's ray.
struct alignas(16) Slot {
    float dir[4] = {0.f, 0.f, 0.f, 0.f};
    std::atomic<uint64_t> key{0};   // 16
    std::atomic<uint64_t> scale{0}; // 24
    std::atomic<uint32_t> on{0};    // 32
    float k = 0.f;                  // 36
    std::atomic<uint64_t> built{0};   // 40
    std::atomic<uint64_t> swapped{0}; // 48
    std::atomic<uint64_t> passed{0};  // 56
};
static_assert(offsetof(Slot, key) == 16 && offsetof(Slot, scale) == 24 &&
                  offsetof(Slot, on) == 32 && offsetof(Slot, k) == 36 &&
                  offsetof(Slot, built) == 40 && offsetof(Slot, swapped) == 48 &&
                  offsetof(Slot, passed) == 56,
              "the stubs' displacements");

inline uint64_t pack2(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return (uint64_t)x | ((uint64_t)y << 32);
}

// The inverse of a canvas factor; 1 for one that is not a usable
// contraction (absent, non-finite, or not in (0, 1]).
inline float unscale_of(float a) {
    return (a > 0.05f && a <= 1.0f) ? 1.0f / a : 1.0f;
}

struct Emit {
    uint8_t *out;
    size_t i = 0;
    explicit Emit(uint8_t *o) : out(o) {}
    void b(uint8_t v) { out[i++] = v; }
    template <class... T> void bs(T... v) { (b((uint8_t)v), ...); }
    void q(uint64_t v) {
        memcpy(out + i, &v, 8);
        i += 8;
    }
    size_t jcc8(uint8_t op) { // returns the rel8's offset
        b(op);
        return i++;
    }
    void here(size_t at) { out[at] = (uint8_t)(i - (at + 1)); }
    bool jmp32(uintptr_t self_va, uintptr_t to) {
        b(0xe9);
        const int64_t rel = (int64_t)to - (int64_t)(self_va + i + 4);
        const int32_t r32 = (int32_t)rel;
        memcpy(out + i, &r32, 4);
        i += 4;
        return rel == (int64_t)r32;
    }
};

//     push %r11 ; mov $slot,%r11
//     cmpl $0,32(%r11) ; je 1f
//     lock incq 40(%r11)                              built
//     movq 24(%r11),%xmm7                             (Sx, Sy)
//     movaps %xmm7,%xmm5 ; shufps $0,%xmm5,%xmm5 ; mulps %xmm0,%xmm5
//     shufps $0x55,%xmm7,%xmm7 ; mulps %xmm4,%xmm7
//     addps %xmm7,%xmm5 ; addps %xmm1,%xmm5            d = Sx*xmm0 + Sy*xmm4 + xmm1
//     insertps $0x08,%xmm5,%xmm5                      w = 0
//     movaps %xmm5,%xmm6 ; mulps %xmm6,%xmm6           squares
//     movaps %xmm6,%xmm7 ; shufps $0x55,%xmm7,%xmm7 ; addss %xmm7,%xmm6
//     movaps %xmm5,%xmm7 ; mulps %xmm7,%xmm7 ; shufps $0xaa,%xmm7,%xmm7
//     addss %xmm7,%xmm6 ; sqrtss %xmm6,%xmm6           |d| (x, y, z)
//     movss 36(%r11),%xmm7 ; divss %xmm6,%xmm7         K / |d|
//     shufps $0,%xmm7,%xmm7 ; mulps %xmm7,%xmm5
//     movaps %xmm5,(%r11) ; mov %rbp,16(%r11)          dir, key
// 1:  pop %r11
//     addps %xmm4,%xmm0 ; addps %xmm1,%xmm0            X4's two
//     jmp 0x1d6e2be
constexpr size_t kStubMax = 160;
inline size_t make_stub_a(uint8_t out[kStubMax], uintptr_t stub_va, uintptr_t slot,
                          uintptr_t cont) {
    Emit e{out};
    e.bs(0x41, 0x53);             // push %r11
    e.bs(0x49, 0xbb), e.q(slot);  // mov $slot,%r11
    e.bs(0x41, 0x83, 0x7b, 0x20, 0x00); // cmpl $0,32(%r11)
    const size_t off = e.jcc8(0x74);    // je 1f
    e.bs(0xf0, 0x49, 0xff, 0x43, 0x28); // lock incq 40(%r11)
    e.bs(0xf3, 0x41, 0x0f, 0x7e, 0x7b, 0x18); // movq 24(%r11),%xmm7
    e.bs(0x0f, 0x28, 0xef);             // movaps %xmm7,%xmm5
    e.bs(0x0f, 0xc6, 0xed, 0x00);       // shufps $0,%xmm5,%xmm5
    e.bs(0x0f, 0x59, 0xe8);             // mulps %xmm0,%xmm5
    e.bs(0x0f, 0xc6, 0xff, 0x55);       // shufps $0x55,%xmm7,%xmm7
    e.bs(0x0f, 0x59, 0xfc);             // mulps %xmm4,%xmm7
    e.bs(0x0f, 0x58, 0xef);             // addps %xmm7,%xmm5
    e.bs(0x0f, 0x58, 0xe9);             // addps %xmm1,%xmm5
    e.bs(0x66, 0x0f, 0x3a, 0x21, 0xed, 0x08); // insertps $8,%xmm5,%xmm5
    e.bs(0x0f, 0x28, 0xf5);             // movaps %xmm5,%xmm6
    e.bs(0x0f, 0x59, 0xf6);             // mulps %xmm6,%xmm6
    e.bs(0x0f, 0x28, 0xfe);             // movaps %xmm6,%xmm7
    e.bs(0x0f, 0xc6, 0xff, 0x55);       // shufps $0x55,%xmm7,%xmm7
    e.bs(0xf3, 0x0f, 0x58, 0xf7);       // addss %xmm7,%xmm6
    e.bs(0x0f, 0x28, 0xfd);             // movaps %xmm5,%xmm7
    e.bs(0x0f, 0x59, 0xff);             // mulps %xmm7,%xmm7
    e.bs(0x0f, 0xc6, 0xff, 0xaa);       // shufps $0xaa,%xmm7,%xmm7
    e.bs(0xf3, 0x0f, 0x58, 0xf7);       // addss %xmm7,%xmm6
    e.bs(0xf3, 0x0f, 0x51, 0xf6);       // sqrtss %xmm6,%xmm6
    e.bs(0xf3, 0x41, 0x0f, 0x10, 0x7b, 0x24); // movss 36(%r11),%xmm7
    e.bs(0xf3, 0x0f, 0x5e, 0xfe);       // divss %xmm6,%xmm7
    e.bs(0x0f, 0xc6, 0xff, 0x00);       // shufps $0,%xmm7,%xmm7
    e.bs(0x0f, 0x59, 0xef);             // mulps %xmm7,%xmm5
    e.bs(0x41, 0x0f, 0x29, 0x2b);       // movaps %xmm5,(%r11)
    e.bs(0x49, 0x89, 0x6b, 0x10);       // mov %rbp,16(%r11)
    e.here(off);
    e.bs(0x41, 0x5b);                   // 1: pop %r11
    e.bs(0x0f, 0x58, 0xc4);             // addps %xmm4,%xmm0
    e.bs(0x0f, 0x58, 0xc1);             // addps %xmm1,%xmm0
    return e.jmp32(stub_va, cont) ? e.i : 0;
}

//     mov %r15,%rcx                                   X4's
//     push %r11 ; mov $slot,%r11
//     cmpl $0,32(%r11) ; je 2f
//     cmp %r15,16(%r11) ; jne 2f                       built for this ray?
//     lea (%r11),%rcx                                 the UI direction
//     lock incq 48(%r11) ; jmp 3f                     swapped
// 2:  lock incq 56(%r11)                              passed
// 3:  pop %r11
//     push $0x17                                      X4's
//     jmp back
inline size_t make_stub_b(uint8_t out[kStubMax], uintptr_t stub_va, uintptr_t slot,
                          uintptr_t back) {
    Emit e{out};
    e.bs(0x4c, 0x89, 0xf9);             // mov %r15,%rcx
    e.bs(0x41, 0x53);                   // push %r11
    e.bs(0x49, 0xbb), e.q(slot);        // mov $slot,%r11
    e.bs(0x41, 0x83, 0x7b, 0x20, 0x00); // cmpl $0,32(%r11)
    const size_t off = e.jcc8(0x74);    // je 2f
    e.bs(0x4d, 0x39, 0x7b, 0x10);       // cmp %r15,16(%r11)
    const size_t miss = e.jcc8(0x75);   // jne 2f
    e.bs(0x49, 0x8d, 0x0b);             // lea (%r11),%rcx
    e.bs(0xf0, 0x49, 0xff, 0x43, 0x30); // lock incq 48(%r11)
    const size_t done = e.jcc8(0xeb);   // jmp 3f
    e.here(off);
    e.here(miss);
    e.bs(0xf0, 0x49, 0xff, 0x43, 0x38); // 2: lock incq 56(%r11)
    e.here(done);
    e.bs(0x41, 0x5b);                   // 3: pop %r11
    e.bs(0x6a, 0x17);                   // push $0x17
    return e.jmp32(stub_va, back) ? e.i : 0;
}

// `len` bytes at `site`: jmp stub, then int3s.
inline bool patch_bytes(uintptr_t site, size_t len, uintptr_t stub_va, uint8_t *out) {
    const int64_t rel = (int64_t)stub_va - (int64_t)(site + 5);
    if (rel != (int64_t)(int32_t)rel || len < 5)
        return false;
    const int32_t r32 = (int32_t)rel;
    out[0] = 0xe9;
    memcpy(out + 1, &r32, 4);
    memset(out + 5, 0xcc, len - 5);
    return true;
}

} // namespace x4vr::uiray
