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
// **Take 353: never reached.** For the player's camera X4 left before that
// jmp, through the early return at 0x192a27f, so a second site covers it
// (stub B, below); both count which path the camera takes.
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

// What the stubs read: `cur`, the rows in effect or NULL (nothing composed),
// and their counters: `applied`, bumped at every composition; `orbit_seen`
// and `early_seen`, bumped whenever the player's camera outside mode 0
// reaches stub A (the orbit compose) or takes X4's early return (stub B),
// rows or not -- which path X4 takes is the measurement take 353 lacked.
// Two buffers, so the injector never writes the rows a stub may be
// reading: it fills the other one and then swaps the pointer.
struct Slot {
    HeadRows buf[2];
    std::atomic<HeadRows *> cur{nullptr};
    std::atomic<uint64_t> applied{0};
    std::atomic<uint64_t> orbit_seen{0};
    std::atomic<uint64_t> early_seen{0};
    int next = 0;
    void publish(const float rows[12]) {
        broadcast_rows(rows, &buf[next]);
        cur.store(&buf[next], std::memory_order_release);
        next ^= 1;
    }
    void withdraw() { cur.store(nullptr, std::memory_order_release); }
};
static_assert(sizeof(std::atomic<HeadRows *>) == 8, "the stubs load 8 bytes");
static_assert(sizeof(std::atomic<uint64_t>) == 8, "the stubs increment 8 bytes");

struct StubAddrs {
    uintptr_t root_slot;   // kRootGlobal
    uint32_t player_cam;   // kPlayerCamOff
    uint32_t mode;         // kCamMode
    uint32_t own;          // kCamOwn
    uintptr_t rows_slot;   // &Slot::cur
    uintptr_t applied;     // &Slot::applied
    uintptr_t seen;        // &Slot::orbit_seen (A) / &Slot::early_seen (B)
    uintptr_t after_loads; // A: kAfterLoads
    uintptr_t x4_compose;  // A: kComposeVa
    uintptr_t ret;         // B: kReturnVa
    uintptr_t cont;        // B: kEarlyEnd
};

// The byte emitter both stubs are written with; forward jumps are recorded
// and fixed up to a label once it is known.
struct Emit {
    uint8_t *out;
    size_t i = 0;
    size_t fix[8] = {};
    int target[8] = {};
    int nfix = 0;
    size_t label[4] = {0, 0, 0, 0};
    explicit Emit(uint8_t *o) : out(o) {}
    void b(uint8_t v) { out[i++] = v; }
    void d32(uint32_t v) {
        memcpy(out + i, &v, 4);
        i += 4;
    }
    void q(uint64_t v) {
        memcpy(out + i, &v, 8);
        i += 8;
    }
    void jcc(uint8_t cc, int to) { // 0f 8x rel32 to label `to`
        b(0x0f), b(cc);
        fix[nfix] = i;
        target[nfix++] = to;
        d32(0);
    }
    void mov_r11(uint64_t v) { b(0x49), b(0xbb), q(v); }
    void jmp_abs(uint64_t v) { mov_r11(v), b(0x41), b(0xff), b(0xe3); }
    void lock_inc(uint64_t at) { mov_r11(at), b(0xf0), b(0x49), b(0xff), b(0x03); }
    // The player's camera, outside mode 0; else jump to label `no`.
    void player_gates(const StubAddrs &a, int no) {
        mov_r11(a.root_slot);
        b(0x4d), b(0x8b), b(0x1b); // mov r11, [r11]
        b(0x4d), b(0x85), b(0xdb); // test r11, r11
        jcc(0x84, no);             // je no
        b(0x49), b(0x3b), b(0x9b), d32(a.player_cam); // cmp rbx, [r11+d32]
        jcc(0x85, no);                                 // jne no
        b(0x83), b(0xbb), d32(a.mode), b(0x00);        // cmp dword [rbx+d32], 0
        jcc(0x84, no);                                 // je no
    }
    // r11 = the rows in effect; NULL -> label `no`.
    void rows_gate(const StubAddrs &a, int no) {
        mov_r11(a.rows_slot);
        b(0x4d), b(0x8b), b(0x1b); // mov r11, [r11]
        b(0x4d), b(0x85), b(0xdb); // test r11, r11
        jcc(0x84, no);             // je no
    }
    // xmm2/xmm1/xmm0 = H x (xmm4, xmm10, xmm11): row i of the product is
    // h[i][0]*row0 + h[i][1]*row1 + h[i][2]*row2, with r11 at the rows.
    void head_times_rows() {
        const uint8_t dst[3] = {2, 1, 0}; // X4's registers for rows 0/1/2
        for (uint32_t r = 0; r < 3; r++) {
            const uint8_t d = dst[r];
            b(0x41), b(0x0f), b(0x10), b((uint8_t)(0x83 | d << 3)),
                d32(48 * r);                                   // movups xmmd, [r11+]
            b(0x0f), b(0x59), b((uint8_t)(0xc4 | d << 3));     // mulps xmmd, xmm4
            b(0x45), b(0x0f), b(0x10), b(0xa3), d32(48 * r + 16); // movups xmm12
            b(0x45), b(0x0f), b(0x59), b(0xe2);                // mulps xmm12, xmm10
            b(0x41), b(0x0f), b(0x58), b((uint8_t)(0xc4 | d << 3)); // addps xmmd, xmm12
            b(0x45), b(0x0f), b(0x10), b(0xa3), d32(48 * r + 32); // movups xmm12
            b(0x45), b(0x0f), b(0x59), b(0xe3);                // mulps xmm12, xmm11
            b(0x41), b(0x0f), b(0x58), b((uint8_t)(0xc4 | d << 3)); // addps xmmd, xmm12
        }
    }
    void here(int l) { label[l] = i; }
    size_t finish() {
        for (int k = 0; k < nfix; k++) {
            const int32_t rel = (int32_t)(label[target[k]] - (fix[k] + 4));
            memcpy(out + fix[k], &rel, 4);
        }
        return i;
    }
};

// Stub A, at the orbit compose (0x192a295's jmp). Entered with rbx = the
// camera, rbp = `out`, xmm6/7/5/8 = the pivot's rows and translation (live:
// X4's product reads them). Uses r11 (never used in 0x1929f70) and
// xmm0-xmm4, xmm10-xmm12 (X4's product writes xmm4 and xmm9 before reading
// them and never reads xmm10+); flags are dead.
//
//     player gates (root, rbx == [root+0x3e8], [rbx+0x880] != 0) else x4
//     lock inc orbit_seen
//     rows gate (r11 = rows) else x4
//     movaps xmm4/xmm10/xmm11, own rows 0/1/2
//     xmm2/xmm1/xmm0 = H x own's rows
//     movaps xmm3, own translation
//     lock inc applied ; jmp after_loads   ; X4's product, our rows
//   x4:
//     jmp x4_compose                       ; X4's compose, untouched
constexpr size_t kStubMax = 320;
inline size_t make_stub(uint8_t out[kStubMax], const StubAddrs &a) {
    Emit e{out};
    enum { X4 = 0 };
    e.player_gates(a, X4);
    e.lock_inc(a.seen);
    e.rows_gate(a, X4);
    e.b(0x0f), e.b(0x28), e.b(0xa3), e.d32(a.own + 0x10);          // movaps xmm4
    e.b(0x44), e.b(0x0f), e.b(0x28), e.b(0x93), e.d32(a.own + 0x20); // xmm10
    e.b(0x44), e.b(0x0f), e.b(0x28), e.b(0x9b), e.d32(a.own + 0x30); // xmm11
    e.head_times_rows();
    e.b(0x0f), e.b(0x28), e.b(0x9b), e.d32(a.own); // movaps xmm3, own.t
    e.lock_inc(a.applied);
    e.jmp_abs(a.after_loads);
    e.here(X4);
    e.jmp_abs(a.x4_compose);
    return e.finish();
}

// ---- Site B: X4's early return (take 353) ---------------------------------
//
// Take 353: two external periods, 57 s, the stub above composed nothing,
// while GetCameraRotation followed the orbit exactly (net -17.70/-14.80
// against the camera's own -18.07/-14.83). So X4 composes the orbit for
// the player's camera somewhere other than 0x192a15e. The one exit of the
// modes 2-5 path before that jmp is at 0x192a27f: `cmp %r13,(%rax); je
// 0x192a216` -- return with `out` = the pivot alone when the pivot is
// [root+0x448], the object X4's external-view switch (0x1d7dd51, 0x1d7e373
// via 0x1938b30) stores in the camera's +0x788. The reading: that object
// carries the camera's pose, orbit included, and X4 returns it as is.
//
// Those five bytes become `jmp stub B` (nothing jumps into them, all of
// .text scanned). Stub B repeats X4's comparison; not equal -> 0x192a284,
// X4's own continuation. Equal -> for the player's camera outside mode 0
// with rows published, `out`'s rows become H x out's rows, in place; then
// X4's return either way. The early return's `out` is the camera's pose
// relative to the caller's frame, so H turns it about the eye, as stub A
// does with the orbit.
inline constexpr uint8_t kEarlyBytes[] = {0x4c, 0x39, 0x28, 0x74, 0x92};
constexpr uintptr_t kEarlyCmp = 0x192a27f; // cmp %r13,(%rax); je 0x192a216
constexpr uintptr_t kEarlyEnd = 0x192a284;
constexpr uintptr_t kReturnVa = 0x192a216;

inline bool early_site_ok(uintptr_t *bad) {
    // Inside kModes, so verify_sites has checked the bytes; here what they
    // mean: the je lands on the return, the next instruction is the tail.
    if (memcmp(kModesBytes + (kEarlyCmp - kModes.va), kEarlyBytes, 5) != 0) {
        *bad = kEarlyCmp;
        return false;
    }
    const int8_t rel = (int8_t)kEarlyBytes[4];
    if (kEarlyEnd + rel != kReturnVa || kEarlyEnd != kEarlyCmp + 5) {
        *bad = kEarlyCmp + 3;
        return false;
    }
    return true;
}

//     cmp  [rax], r13 ; jne cont           ; X4's comparison, verbatim
//     player gates else ret
//     lock inc early_seen
//     rows gate (r11 = rows) else ret
//     movaps xmm4/xmm10/xmm11, out rows 0/1/2 ([rbp+0x10/0x20/0x30])
//     xmm2/xmm1/xmm0 = H x out's rows
//     movaps [rbp+0x10/0x20/0x30], xmm2/xmm1/xmm0
//     lock inc applied
//   ret:  jmp 0x192a216                     ; X4's return
//   cont: jmp 0x192a284                     ; X4's continuation
inline size_t make_early_stub(uint8_t out[kStubMax], const StubAddrs &a) {
    Emit e{out};
    enum { RET = 0, CONT = 1 };
    e.b(0x4c), e.b(0x39), e.b(0x28); // cmp [rax], r13
    e.jcc(0x85, CONT);               // jne cont
    e.player_gates(a, RET);
    e.lock_inc(a.seen);
    e.rows_gate(a, RET);
    e.b(0x0f), e.b(0x28), e.b(0x65), e.b(0x10);         // movaps xmm4, [rbp+0x10]
    e.b(0x44), e.b(0x0f), e.b(0x28), e.b(0x55), e.b(0x20); // movaps xmm10, [rbp+0x20]
    e.b(0x44), e.b(0x0f), e.b(0x28), e.b(0x5d), e.b(0x30); // movaps xmm11, [rbp+0x30]
    e.head_times_rows();
    e.b(0x0f), e.b(0x29), e.b(0x55), e.b(0x10); // movaps [rbp+0x10], xmm2
    e.b(0x0f), e.b(0x29), e.b(0x4d), e.b(0x20); // movaps [rbp+0x20], xmm1
    e.b(0x0f), e.b(0x29), e.b(0x45), e.b(0x30); // movaps [rbp+0x30], xmm0
    e.lock_inc(a.applied);
    e.here(RET);
    e.jmp_abs(a.ret);
    e.here(CONT);
    e.jmp_abs(a.cont);
    return e.finish();
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
