// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_uimouse.hpp -- X4VR_HUD_MOUSE: X4's UI takes the mouse where it is
// DRAWN -- the perspective HUD at the canvas map's scale, the orthographic
// markers as they are, both at the steering ring in steering mode.
//
// **Why.** The canvas map draws X4's perspective UI at A (0.5252) about the
// view's centre (#76: X4 lays its UI out for its default field and draws it
// with the world's, 1.904x out; the map pulls it back) and leaves the
// orthographic UI -- the object markers -- alone (#82's w-gate). X4
// hit-tests at its own scale, so with the cursor drawn raw (the cockpit) a
// HUD element answered only at 1.904x its drawn offset. Takes 369, 371 and
// 372 corrected the presentations' local pointer (Lua's
// GetLocalMousePosition), X4's UI cast, and both: the highlight never moved.
//
// **Where the mouse enters (take 373).** A read/write hardware watch on X4's
// pointer named 0xa704a0, slot 2 of UI::XAnark::AnarkInputEngine (vtable
// 0x3c4f690; the base is Anark::CInputEngine, whose slot 2 is
// `lea 8(%rdi),%rax` -- GetMouseState). X4's override reads the centred
// pointer (the mouse object's +0x38/+0x30, or the last click's +0x48/+0x40
// while a button is held), converts it to floats xmm0 (x) and xmm1 (y), and
// at 0xa70725 calls slot 3 -- 0x2317e00, the setter (`unpcklps; mov
// %sil,0xa9(%rdi); movq $0,0x10(%rdi); movlps %xmm0,8(%rdi)`). It is the
// only mouse Anark has: one per frame, for every scene.
//
// **Take 374: one mouse, two kinds of UI.** Scaling that mouse by 1/A made
// the HUD answer where drawn (Patola: the alert, its options, the mail icon)
// and broke what is drawn at full size: away from the centre, clicks and
// highlights on objects landed only about half way out. So the scale must be
// per layer, not per frame.
//
// **Where the layers split (read 2026-10-30).** X4's scene manager
// (0xa583a0) hands the copied mouse to each scene's pick (Anark::CScene slot
// 7, 0x2322dc0), which walks the scene's layers (0x2322350) and calls the
// render engine's slot 14 for each -- X4's own layer pick, 0xa24b50 (vtable
// 0x3c4f588). That function reads the mouse from the pick state into its
// locals (0xa24d3f: [rsp] = x,y), asks the presentation for its viewport and
// projection mode, and builds the pick ray:
//   * mode 1 (0xa25000): an orthographic ray, its origin moved by the mouse
//     -- the layers drawn with w == 1, which the canvas map leaves alone;
//   * otherwise (0xa24e79): a perspective ray, its direction the mouse times
//     tan of the presentation's field (obj+0x8, X4's default) -- the layers
//     drawn with w != 1, which the canvas map scales by A.
//   * The player's-look kind (obj+0x83 == 5) branches away earlier and uses
//     X4's own aim point, not the mouse.
//
// **What.** Two patches, one slot:
//   * 0xa70725, 8 bytes (`mov %ebp,%esi; mov %rbx,%rdi; call *%r12`) ->
//     `jmp feed stub`: while on, the steering ring's shift is added to the
//     mouse every layer gets; then X4's call, on X4's stack, and back to
//     0xa7072d.
//   * 0xa24e79, 16 bytes (`pxor %xmm0,%xmm0; mov 0xda8(%r14),%edx; movq
//     (%rsp),%xmm7`) -> `jmp layer stub`: the stub replays them, and while
//     on multiplies xmm7 -- the mouse this perspective layer's ray is built
//     from -- by 1/A, then jumps to 0xa24e89. The orthographic branch and
//     the locals other code reads are X4's.
// Nothing jumps inside either range (objdump, 2026-10-30). In the layer
// stub xmm6 is scratch: X4 overwrites it at 0xa24eb5 before reading it.
//
// Self-contained so tests/uimouse.cpp can check it: the sites against the
// real binary, both stubs EXECUTED.
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
// 0xa24d2f, the layer pick: rax = the pick state; rsi = its +0x10 (the
// mouse, x and y) to [rsp]; x to [rsp+0x20], y to [rsp+0x24].
inline constexpr uint8_t kLayerMouseBytes[] = {
    0x48, 0x8b, 0x44, 0x24, 0x08, 0x4c, 0x8d, 0xac, 0x24, 0xa0, 0x00, 0x00,
    0x00, 0x4c, 0x89, 0xef, 0x48, 0x8b, 0x70, 0x10, 0xf3, 0x0f, 0x10, 0x78,
    0x10, 0x48, 0x89, 0x34, 0x24, 0x48, 0x8b, 0x74, 0x24, 0x10, 0xf3, 0x0f,
    0x11, 0x7c, 0x24, 0x20, 0xf3, 0x0f, 0x10, 0x78, 0x14, 0x48, 0x81, 0xc6,
    0x88, 0x00, 0x00, 0x00, 0xf3, 0x0f, 0x11, 0x7c, 0x24, 0x24};
// 0xa24e66: cmp $1,%r13d (the projection mode); movss 0x10(%rsp),%xmm4;
// movaps %xmm0,%xmm5 (tan of the field); je 0xa25000 (orthographic); THE
// patched 16 bytes at 0xa24e79; mov $0x4533ae0,%rax at 0xa24e89.
inline constexpr uint8_t kLayerBytes[] = {
    0x41, 0x83, 0xfd, 0x01, 0xf3, 0x0f, 0x10, 0x64, 0x24, 0x10, 0x0f, 0x28,
    0xe8, 0x0f, 0x84, 0x87, 0x01, 0x00, 0x00, 0x66, 0x0f, 0xef, 0xc0, 0x41,
    0x8b, 0x96, 0xa8, 0x0d, 0x00, 0x00, 0xf3, 0x0f, 0x7e, 0x3c, 0x24, 0x48,
    0xc7, 0xc0, 0xe0, 0x3a, 0x53, 0x04};

inline constexpr Site kEntry = {0xa704a0, sizeof kEntryBytes, kEntryBytes};
inline constexpr Site kClickPath = {0xa706fa, sizeof kClickPathBytes, kClickPathBytes};
inline constexpr Site kLivePath = {0xa7082e, sizeof kLivePathBytes, kLivePathBytes};
inline constexpr Site kSetter = {0x2317e00, sizeof kSetterBytes, kSetterBytes};
inline constexpr Site kLayerMouse = {0xa24d2f, sizeof kLayerMouseBytes,
                                     kLayerMouseBytes};
inline constexpr Site kLayer = {0xa24e66, sizeof kLayerBytes, kLayerBytes};

constexpr uintptr_t kPatch = 0xa70725; // 8 bytes
constexpr size_t kPatchLen = 8;
constexpr uintptr_t kCont = 0xa7072d;
constexpr uintptr_t kVtable = 0x3c4f690;  // slot 2 = 0xa704a0, slot 3 = the setter
constexpr uintptr_t kSetter2 = 0x2317e00;
constexpr uintptr_t kLayerPatch = 0xa24e79; // 16 bytes
constexpr size_t kLayerPatchLen = 16;
constexpr uintptr_t kLayerCont = 0xa24e89;
constexpr uintptr_t kLayerOrtho = 0xa25000;
constexpr uintptr_t kLayerPick = 0xa24b50;
constexpr uintptr_t kRenderVtable = 0x3c4f588; // slot 14 = the layer pick

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

// Every byte the patches depend on; the live path's jmp lands on the feed
// patch; the input engine's slots 2 and 3 are the feed and the setter; the
// render engine's slot 14 is the layer pick; the mode test's je goes to the
// orthographic branch.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kEntry, &kClickPath, &kLivePath, &kSetter, &kLayerMouse,
                          &kLayer})
        if (!same_bytes(read, *s, bad))
            return false;
    int32_t rel;
    memcpy(&rel, kLivePathBytes + sizeof kLivePathBytes - 4, 4);
    if (kLivePath.va + kLivePath.n + (intptr_t)rel != kPatch ||
        kClickPath.va + 43 != kPatch) {
        *bad = kPatch;
        return false;
    }
    memcpy(&rel, kLayerBytes + 15, 4);
    if (kLayer.va + 19 + (intptr_t)rel != kLayerOrtho ||
        kLayer.va + 19 != kLayerPatch ||
        kLayerPatch + kLayerPatchLen != kLayerCont) {
        *bad = kLayerPatch;
        return false;
    }
    uint64_t slot2 = 0, slot3 = 0, slot14 = 0;
    if (!read(kVtable + 0x10, &slot2, 8) || !read(kVtable + 0x18, &slot3, 8) ||
        slot2 != kEntry.va || slot3 != kSetter2) {
        *bad = kVtable;
        return false;
    }
    if (!read(kRenderVtable + 0x70, &slot14, 8) || slot14 != kLayerPick) {
        *bad = kRenderVtable;
        return false;
    }
    return true;
}

// What the stubs read and count: `add` and `scale` as float pairs, `on`.
// The feed stub counts `calls` every pass and `fed` the passes while on;
// the layer stub counts `persp` every perspective layer pick and `scaled`
// the ones it scaled.
struct alignas(16) Slot {
    std::atomic<uint64_t> add{0};   // 0: x, y (X4's window pixels)
    std::atomic<uint64_t> scale{0}; // 8: x, y
    std::atomic<uint32_t> on{0};    // 16
    uint32_t pad = 0;
    std::atomic<uint64_t> fed{0};    // 24
    std::atomic<uint64_t> calls{0};  // 32
    std::atomic<uint64_t> persp{0};  // 40
    std::atomic<uint64_t> scaled{0}; // 48
};
static_assert(offsetof(Slot, scale) == 8 && offsetof(Slot, on) == 16 &&
                  offsetof(Slot, fed) == 24 && offsetof(Slot, calls) == 32 &&
                  offsetof(Slot, persp) == 40 && offsetof(Slot, scaled) == 48,
              "the stubs' displacements");

inline uint64_t pack2(float a, float b) {
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return (uint64_t)x | ((uint64_t)y << 32);
}
inline float unscale_of(float a) {
    return (a > 0.05f && a <= 1.0f) ? 1.0f / a : 1.0f;
}

// The feed stub, at 0xa70725:
//     push %r11 ; mov $slot,%r11
//     lock incq 32(%r11)                     calls
//     cmpl $0,16(%r11) ; je 1f
//     lock incq 24(%r11)                     fed
//     addss (%r11),%xmm0 ; addss 4(%r11),%xmm1      + the ring's shift
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

// The layer stub, at 0xa24e79 (X4's rsp: the mouse is read before the push):
//     pxor %xmm0,%xmm0 ; mov 0xda8(%r14),%edx ; movq (%rsp),%xmm7   X4's three
//     push %r11 ; mov $slot,%r11
//     lock incq 40(%r11)                     persp
//     cmpl $0,16(%r11) ; je 1f
//     lock incq 48(%r11)                     scaled
//     movq 8(%r11),%xmm6 ; mulps %xmm6,%xmm7        x 1/A
// 1:  pop %r11
//     jmp 0xa24e89
inline size_t make_layer_stub(uint8_t out[kStubMax], uintptr_t stub_va,
                              uintptr_t slot, uintptr_t cont) {
    size_t i = 0;
    auto b = [&](std::initializer_list<uint8_t> v) {
        for (uint8_t x : v)
            out[i++] = x;
    };
    b({0x66, 0x0f, 0xef, 0xc0});                   // pxor %xmm0,%xmm0
    b({0x41, 0x8b, 0x96, 0xa8, 0x0d, 0x00, 0x00}); // mov 0xda8(%r14),%edx
    b({0xf3, 0x0f, 0x7e, 0x3c, 0x24});             // movq (%rsp),%xmm7
    b({0x41, 0x53});                               // push %r11
    b({0x49, 0xbb});                               // mov $slot,%r11
    memcpy(out + i, &slot, 8);
    i += 8;
    b({0xf0, 0x49, 0xff, 0x43, 0x28});             // lock incq 40(%r11)
    b({0x41, 0x83, 0x7b, 0x10, 0x00});             // cmpl $0,16(%r11)
    b({0x74});                                     // je 1f
    const size_t j = i++;
    b({0xf0, 0x49, 0xff, 0x43, 0x30});             // lock incq 48(%r11)
    b({0xf3, 0x41, 0x0f, 0x7e, 0x73, 0x08});       // movq 8(%r11),%xmm6
    b({0x0f, 0x59, 0xfe});                         // mulps %xmm6,%xmm7
    out[j] = (uint8_t)(i - (j + 1));
    b({0x41, 0x5b});                               // 1: pop %r11
    b({0xe9});                                     // jmp cont
    const int64_t rel = (int64_t)cont - (int64_t)(stub_va + i + 4);
    const int32_t r32 = (int32_t)rel;
    memcpy(out + i, &r32, 4);
    i += 4;
    return rel == (int64_t)r32 ? i : 0;
}

// `len` bytes at `site`: jmp stub, then int3s.
inline bool jmp_bytes(uintptr_t site, size_t len, uintptr_t stub_va, uint8_t *out) {
    const int64_t rel = (int64_t)stub_va - (int64_t)(site + 5);
    if (len < 5 || rel != (int64_t)(int32_t)rel)
        return false;
    const int32_t r32 = (int32_t)rel;
    out[0] = 0xe9;
    memcpy(out + 1, &r32, 4);
    memset(out + 5, 0xcc, len - 5);
    return true;
}
inline bool patch_bytes(uintptr_t stub_va, uint8_t out[kPatchLen]) {
    return jmp_bytes(kPatch, kPatchLen, stub_va, out);
}
inline bool layer_patch_bytes(uintptr_t stub_va, uint8_t out[kLayerPatchLen]) {
    return jmp_bytes(kLayerPatch, kLayerPatchLen, stub_va, out);
}

} // namespace x4vr::uimouse
