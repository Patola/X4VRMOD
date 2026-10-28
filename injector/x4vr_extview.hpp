// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_extview.hpp -- X4VR_HEADLOOK_EXTERNAL: the head turns X4's external
// views (F2/F3/F4/F6), so they are the whole scene and not a window fixed in
// front (Patola, take 344; route chosen 2026-10-28: "I will prefer the code
// patch, like walking's").
//
// **Where.** 0x1929f70 evaluates a camera each frame. Modes 2-5 (F2/F3 =
// mode 2 on the ship or the target, F4/F6 = mode 4, take 350's census) take
// 0x192a228: the pivot (component 0x57) is evaluated into `out`, then the
// tail at 0x192a284 reloads `out`'s rows into xmm6/7/5/8 and `jmp 0x192a15e`
// composes the camera's own transform (+0x5a0..+0x5d0: X4's orbit) onto it.
// The camera's own transform is X4's state -- the orbit the numpad drives
// (take 352) -- so the head never goes there.
//
// **What.** That one jmp's rel32 is retargeted to a stub of ours. For the
// player's camera in a mode other than 0, with head rows published, the stub
// multiplies the head's rotation H in BEFORE the orbit, in the camera's own
// frame:
//
//     own'.R = H.R x own.R      own'.t = own.t
//
// (X4's row-vector convention: out = own x pivot, row i of a product is
// sum_j a[i][j] * b.row_j.) It leaves own'.t in xmm3 and own'.R's rows in
// xmm2/xmm1/xmm0 -- exactly what X4's own four loads at 0x192a15e leave --
// and jumps to 0x192a17a, X4's compose right after those loads. Anything
// else jumps to 0x192a15e: X4's compose, unchanged. X4's memory is never
// written; the camera's own transform stays X4's.
//
// **Mode 0 is excluded in the stub itself.** The walking stub
// (x4vr_foot.hpp) reaches 0x192a284 too, in mode 0, where head-look already
// writes the camera's own transform; a second H there would double it.
//
// **Declared only once composed.** The stub counts every composition
// (`applied`). The injector publishes the rows at the start of a period and
// tells the layer to declare the head's angle only after the count has
// moved: a frame the stub never touched (another path, a pivot X4 returns
// early for) is not declared as head-turned.
//
// Self-contained so tests/extview.cpp can check it: the sites against the
// real binary, and the stub EXECUTED with X4's own compose behind it.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x4vr::extview {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0x1929f9d: mov 0x880(%rbx),%eax; mov %rsi,%rbp; mov %rdx,%r12;
// cmp $5,%eax; jg 0x192a2a0; cmp $1,%eax; jg 0x192a228 -- rbx is the camera,
// rbp `out`, and modes 2-5 go to 0x192a228.
inline constexpr uint8_t kDispatchBytes[] = {
    0x8b, 0x83, 0x80, 0x08, 0x00, 0x00, 0x48, 0x89, 0xf5, 0x49, 0x89, 0xd4,
    0x83, 0xf8, 0x05, 0x0f, 0x8f, 0xee, 0x02, 0x00, 0x00, 0x83, 0xf8, 0x01,
    0x0f, 0x8f, 0x6d, 0x02, 0x00, 0x00};
// 0x192a003: mov 0x3db6948(%rip),%rax; ...; cmp 0x3e8(%rax),%rbx; je --
// X4's own test that a camera is the player's, the one the stub repeats.
inline constexpr uint8_t kPlayerTestBytes[] = {
    0x48, 0x8b, 0x05, 0x3e, 0xc9, 0x48, 0x02, 0x48, 0x8b, 0x93, 0x80, 0x07,
    0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0xb6, 0x02, 0x00, 0x00, 0x48,
    0x39, 0x90, 0x38, 0x02, 0x00, 0x00, 0x0f, 0x84, 0xb6, 0x02, 0x00, 0x00,
    0x48, 0x3b, 0x98, 0xe8, 0x03, 0x00, 0x00, 0x0f, 0x84, 0xa9, 0x02, 0x00,
    0x00};
// 0x192a228..0x192a29a: modes 2-5. The pivot (0x189d8f0(+0x788, 0x57, 1))
// evaluated into `out` by 0x18a0090, X4's early return when it is the
// root's +0x448 object, and the tail: movaps 0x10/0x20/0x30/0x0(%rbp) ->
// xmm6/7/5/8; jmp 0x192a15e -- the jmp whose rel32 is patched.
inline constexpr uint8_t kModesBytes[] = {
    0xba, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x57, 0x00, 0x00, 0x00, 0x48, 0x89,
    0xcf, 0xf2, 0x0f, 0x11, 0x44, 0x24, 0x08, 0xe8, 0xb0, 0x36, 0xf7, 0xff,
    0xf2, 0x0f, 0x10, 0x44, 0x24, 0x08, 0x48, 0x85, 0xc0, 0x49, 0x89, 0xc5,
    0x74, 0x36, 0x4c, 0x39, 0xe0, 0x74, 0x10, 0x31, 0xc9, 0x4c, 0x89, 0xe2,
    0x48, 0x89, 0xee, 0x48, 0x89, 0xc7, 0xe8, 0x2d, 0x5e, 0xf7, 0xff, 0x48,
    0x8b, 0x15, 0xde, 0xc6, 0x48, 0x02, 0x48, 0x8d, 0x82, 0x48, 0x04, 0x00,
    0x00, 0x48, 0x85, 0xd2, 0x48, 0x8d, 0x15, 0x6d, 0xf9, 0x5b, 0x02, 0x48,
    0x0f, 0x44, 0xc2, 0x4c, 0x39, 0x28, 0x74, 0x92, 0x0f, 0x28, 0x75, 0x10,
    0x0f, 0x28, 0x7d, 0x20, 0x0f, 0x28, 0x6d, 0x30, 0x44, 0x0f, 0x28, 0x45,
    0x00, 0xe9, 0xc4, 0xfe, 0xff, 0xff};
// 0x192a15e..0x192a221: the compose and the function's epilogue. The four
// loads of the camera's own transform (t -> xmm3, rows -> xmm2/xmm1/xmm0),
// then, from 0x192a17a, a 4x4 product reading only xmm0-xmm3 and
// xmm5-xmm8, writing `out` (rbp) and returning. The stub enters at
// 0x192a17a, so every byte of it is checked.
inline constexpr uint8_t kComposeBytes[] = {
    0x0f, 0x28, 0x9b, 0xa0, 0x05, 0x00, 0x00, 0x0f, 0x28, 0x93, 0xb0, 0x05,
    0x00, 0x00, 0x0f, 0x28, 0x8b, 0xc0, 0x05, 0x00, 0x00, 0x0f, 0x28, 0x83,
    0xd0, 0x05, 0x00, 0x00, 0x0f, 0x28, 0xe3, 0x44, 0x0f, 0x28, 0xcb, 0x0f,
    0xc6, 0xe3, 0xc0, 0x44, 0x0f, 0xc6, 0xcb, 0xd5, 0x0f, 0x59, 0xe6, 0x0f,
    0xc6, 0xdb, 0xea, 0x44, 0x0f, 0x59, 0xcf, 0x0f, 0x59, 0xdd, 0x41, 0x0f,
    0x58, 0xe1, 0x0f, 0x58, 0xdc, 0x0f, 0x28, 0xe2, 0x0f, 0xc6, 0xe2, 0x00,
    0x0f, 0x59, 0xe6, 0x41, 0x0f, 0x58, 0xd8, 0x0f, 0x29, 0x5d, 0x00, 0x0f,
    0x28, 0xda, 0x0f, 0xc6, 0xda, 0x55, 0x0f, 0x59, 0xdf, 0x0f, 0xc6, 0xd2,
    0xaa, 0x0f, 0x59, 0xd5, 0x0f, 0x58, 0xdc, 0x0f, 0x58, 0xd3, 0x0f, 0x28,
    0xd9, 0x0f, 0xc6, 0xd9, 0x00, 0x0f, 0x59, 0xde, 0x0f, 0x29, 0x55, 0x10,
    0x0f, 0x28, 0xd1, 0x0f, 0xc6, 0xd1, 0x55, 0x0f, 0x59, 0xd7, 0x0f, 0xc6,
    0xc9, 0xaa, 0x0f, 0x59, 0xcd, 0x0f, 0x58, 0xd3, 0x0f, 0x58, 0xca, 0x0f,
    0x28, 0xd0, 0x0f, 0xc6, 0xd0, 0x00, 0x0f, 0x59, 0xd6, 0x0f, 0x29, 0x4d,
    0x20, 0x0f, 0x28, 0xc8, 0x0f, 0xc6, 0xc8, 0x55, 0x0f, 0x59, 0xcf, 0x0f,
    0xc6, 0xc0, 0xaa, 0x0f, 0x59, 0xc5, 0x0f, 0x58, 0xca, 0x0f, 0x58, 0xc1,
    0x0f, 0x29, 0x45, 0x30, 0x48, 0x83, 0xc4, 0x58, 0x5b, 0x5d, 0x41, 0x5c,
    0x41, 0x5d, 0xc3};

inline constexpr Site kDispatch = {0x1929f9d, sizeof kDispatchBytes, kDispatchBytes};
inline constexpr Site kPlayerTest = {0x192a003, sizeof kPlayerTestBytes, kPlayerTestBytes};
inline constexpr Site kModes = {0x192a228, sizeof kModesBytes, kModesBytes};
inline constexpr Site kCompose = {0x192a15e, sizeof kComposeBytes, kComposeBytes};

constexpr uintptr_t kModesJg = 0x1929fb5;    // 0f 8f <rel32>: modes 2-5
constexpr uintptr_t kModesEntry = 0x192a228;
constexpr uintptr_t kTailJmp = 0x192a295;    // e9 <rel32>: THE patched jmp
constexpr uintptr_t kTailJmpEnd = 0x192a29a;
constexpr uintptr_t kTailRel32 = kTailJmp + 1;
constexpr uintptr_t kComposeVa = 0x192a15e;  // X4's four loads, then the product
constexpr uintptr_t kAfterLoads = 0x192a17a; // the product, after the loads
constexpr uintptr_t kRootGlobal = 0x3db6948;
constexpr uint32_t kPlayerCamOff = 0x3e8;
constexpr uint32_t kCamMode = 0x880;
constexpr uint32_t kCamOwn = 0x5a0; // translation; rows at +0x10/+0x20/+0x30

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

// Every byte the patch and the stub depend on, as X4 9.00 has them, and the
// decoded targets: modes 2-5 reach the tail, the tail's jmp lands on the
// compose (X4's own rel32, not yet patched), the player test reads the root
// global. `*bad` names the first difference.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kDispatch, &kPlayerTest, &kModes, &kCompose})
        if (!same_bytes(read, *s, bad))
            return false;
    int32_t r = 0;
    memcpy(&r, kDispatchBytes + (kModesJg + 2 - kDispatch.va), 4);
    if (kModesJg + 6 + (intptr_t)r != kModesEntry) {
        *bad = kModesJg;
        return false;
    }
    memcpy(&r, kModesBytes + (kTailRel32 - kModes.va), 4);
    if (kTailJmpEnd + (intptr_t)r != kComposeVa) {
        *bad = kTailJmp;
        return false;
    }
    memcpy(&r, kPlayerTestBytes + 3, 4);
    if (kPlayerTest.va + 7 + (intptr_t)r != kRootGlobal) {
        *bad = kPlayerTest.va;
        return false;
    }
    if (kAfterLoads - kComposeVa != 28) { // the four movaps: 7 bytes each
        *bad = kAfterLoads;
        return false;
    }
    return true;
}

// The head's rotation as the stub reads it: h[i][j] broadcast to all four
// lanes of vector 3*i + j, so the stub needs no shuffles. 16-aligned, though
// the stub loads with movups.
struct alignas(16) HeadRows {
    float v[9][4];
};
// `rows` is 12 floats in camwrite::rows_for's layout (x, y, z, w per row).
inline void broadcast_rows(const float rows[12], HeadRows *out) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int l = 0; l < 4; l++)
                out->v[3 * i + j][l] = rows[4 * i + j];
}

// What the stub reads: `cur`, the rows in effect or NULL (nothing composed),
// and `applied`, bumped by the stub at every composition. Two buffers, so
// the injector never writes the rows the stub may be reading: it fills the
// other one and then swaps the pointer.
struct Slot {
    HeadRows buf[2];
    std::atomic<HeadRows *> cur{nullptr};
    std::atomic<uint64_t> applied{0};
    int next = 0;
    void publish(const float rows[12]) {
        broadcast_rows(rows, &buf[next]);
        cur.store(&buf[next], std::memory_order_release);
        next ^= 1;
    }
    void withdraw() { cur.store(nullptr, std::memory_order_release); }
};
static_assert(sizeof(std::atomic<HeadRows *>) == 8, "the stub loads 8 bytes");
static_assert(sizeof(std::atomic<uint64_t>) == 8, "the stub increments 8 bytes");

struct StubAddrs {
    uintptr_t root_slot;   // kRootGlobal
    uint32_t player_cam;   // kPlayerCamOff
    uint32_t mode;         // kCamMode
    uint32_t own;          // kCamOwn
    uintptr_t rows_slot;   // &Slot::cur
    uintptr_t applied;     // &Slot::applied
    uintptr_t after_loads; // kAfterLoads
    uintptr_t x4_compose;  // kComposeVa
};

// The stub. Entered with rbx = the camera, rbp = `out`, xmm6/7/5/8 = the
// pivot's rows and translation (live: X4's product reads them). Uses r11
// (never used in 0x1929f70) and xmm0-xmm4, xmm10-xmm12 (X4's product writes
// xmm4 and xmm9 before reading them and never reads xmm10+); flags are dead.
//
//     mov  r11, root_slot ; mov r11, [r11] ; test r11, r11 ; je x4
//     cmp  rbx, [r11+0x3e8] ; jne x4            ; the player's camera
//     cmp  dword [rbx+0x880], 0 ; je x4         ; never mode 0
//     mov  r11, &cur ; mov r11, [r11] ; test r11, r11 ; je x4
//     movaps xmm4/xmm10/xmm11, own rows 0/1/2
//     xmm2/xmm1/xmm0 = h[i][0]*row0 + h[i][1]*row1 + h[i][2]*row2, i = 0/1/2
//     movaps xmm3, own translation
//     mov  r11, &applied ; lock inc qword [r11]
//     mov  r11, after_loads ; jmp r11            ; X4's product, our rows
//   x4:
//     mov  r11, x4_compose ; jmp r11             ; X4's compose, untouched
// 269 bytes as written; the buffer has room, and the stub page is 4096.
constexpr size_t kStubMax = 320;
inline size_t make_stub(uint8_t out[kStubMax], const StubAddrs &a) {
    size_t i = 0;
    auto b = [&](uint8_t v) { out[i++] = v; };
    auto d32 = [&](uint32_t v) {
        memcpy(out + i, &v, 4);
        i += 4;
    };
    auto q = [&](uint64_t v) {
        memcpy(out + i, &v, 8);
        i += 8;
    };
    size_t to_x4[4];
    int nj = 0;
    auto jcc_x4 = [&](uint8_t cc) { // 0f 8x rel32, fixed up below
        b(0x0f), b(cc);
        to_x4[nj++] = i;
        d32(0);
    };
    b(0x49), b(0xbb), q(a.root_slot); //    mov r11, imm64
    b(0x4d), b(0x8b), b(0x1b);        //    mov r11, [r11]
    b(0x4d), b(0x85), b(0xdb);        //    test r11, r11
    jcc_x4(0x84);                     //    je x4
    b(0x49), b(0x3b), b(0x9b), d32(a.player_cam); // cmp rbx, [r11+d32]
    jcc_x4(0x85);                                 // jne x4
    b(0x83), b(0xbb), d32(a.mode), b(0x00);       // cmp dword [rbx+d32], 0
    jcc_x4(0x84);                                 // je x4
    b(0x49), b(0xbb), q(a.rows_slot); //    mov r11, imm64
    b(0x4d), b(0x8b), b(0x1b);        //    mov r11, [r11]
    b(0x4d), b(0x85), b(0xdb);        //    test r11, r11
    jcc_x4(0x84);                     //    je x4
    b(0x0f), b(0x28), b(0xa3), d32(a.own + 0x10);            // movaps xmm4
    b(0x44), b(0x0f), b(0x28), b(0x93), d32(a.own + 0x20);   // movaps xmm10
    b(0x44), b(0x0f), b(0x28), b(0x9b), d32(a.own + 0x30);   // movaps xmm11
    const uint8_t dst[3] = {2, 1, 0}; // X4's registers for own's rows 0/1/2
    for (uint32_t r = 0; r < 3; r++) {
        const uint8_t d = dst[r];
        b(0x41), b(0x0f), b(0x10), b((uint8_t)(0x83 | d << 3)),
            d32(48 * r);                                  // movups xmmd, [r11+]
        b(0x0f), b(0x59), b((uint8_t)(0xc4 | d << 3));    // mulps xmmd, xmm4
        b(0x45), b(0x0f), b(0x10), b(0xa3), d32(48 * r + 16); // movups xmm12
        b(0x45), b(0x0f), b(0x59), b(0xe2);               // mulps xmm12, xmm10
        b(0x41), b(0x0f), b(0x58), b((uint8_t)(0xc4 | d << 3)); // addps xmmd, xmm12
        b(0x45), b(0x0f), b(0x10), b(0xa3), d32(48 * r + 32); // movups xmm12
        b(0x45), b(0x0f), b(0x59), b(0xe3);               // mulps xmm12, xmm11
        b(0x41), b(0x0f), b(0x58), b((uint8_t)(0xc4 | d << 3)); // addps xmmd, xmm12
    }
    b(0x0f), b(0x28), b(0x9b), d32(a.own);  // movaps xmm3, own translation
    b(0x49), b(0xbb), q(a.applied);         // mov r11, imm64
    b(0xf0), b(0x49), b(0xff), b(0x03);     // lock inc qword [r11]
    b(0x49), b(0xbb), q(a.after_loads);     // mov r11, imm64
    b(0x41), b(0xff), b(0xe3);              // jmp r11
    const size_t x4 = i;
    b(0x49), b(0xbb), q(a.x4_compose);      // x4: mov r11, imm64
    b(0x41), b(0xff), b(0xe3);              //     jmp r11
    for (int k = 0; k < nj; k++) {
        const int32_t rel = (int32_t)(x4 - (to_x4[k] + 4));
        memcpy(out + to_x4[k], &rel, 4);
    }
    static_assert(kStubMax >= 269, "the stub is 269 bytes");
    return i;
}

// One external period: the rows are published while `want` holds; the
// head is DECLARED (the layer told the picture is head-turned) only once
// the stub has composed them, `applied` past its value at the start.
struct Period {
    bool on = false, declared = false;
    uint64_t base = 0, periods = 0, composed = 0;
    enum Event { kNone, kStarted, kDeclared, kEnded };
    Event step(bool want, uint64_t applied) {
        if (!want) {
            if (!on)
                return kNone;
            on = declared = false;
            composed = applied - base;
            return kEnded;
        }
        if (!on) {
            on = true;
            declared = false;
            base = applied;
            periods++;
            return kStarted;
        }
        composed = applied - base;
        if (!declared && composed) {
            declared = true;
            return kDeclared;
        }
        return kNone;
    }
};

} // namespace x4vr::extview
