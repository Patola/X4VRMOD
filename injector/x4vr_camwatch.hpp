// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_camwatch.hpp — WHO writes X4's camera orientation? (read-only)
//
// **Why this exists.** Head-look moves X4's camera through MOUSELOOK, which
// kills cockpit hover; Patola wants the view moved "by our own means". Take
// 315's value scan (x4vr_camscan.hpp) found no slot storing the free-look
// angle in step with the camera, and the disassembly of GetCameraRotation's
// internal call said why: the camera keeps its orientation as 3x3 MATRICES in
// per-frame buffers inside the camera object, and the free-look state that
// produces them lives in the controller that WRITES them.
//
// So instead of guessing the representation, catch the writer. A hardware
// WRITE watchpoint (perf_event_open, PERF_TYPE_BREAKPOINT, user space only —
// perf_event_paranoid 2 allows it on our own process) on the matrix's first
// row reports every store to it: the instruction, the thread, the registers
// at that moment and a slice of the stack. The instruction's function is the
// controller, and its disassembly names where the state lives.
//
// Nothing here runs in X4's threads. Each watched thread gets its own event
// and ring buffer; the kernel fills them, and OUR thread reads them. X4 sees a
// debug trap per store, which the kernel absorbs.
//
// Anchors are decoded from X4's own code and the whole fetch prologue must
// match byte for byte, so a different X4 build refuses to arm instead of
// watching a wrong address.
#pragma once

#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/x4vr_reloc.hpp"

namespace x4vr {
namespace camwatch {

// ---------------------------------------------------------------- anchors
//
// GetCameraRotation+0x5d: `40 e8 rel32` (a REX-prefixed call) into the fetch
// function, which on X4 9.00 is 0x1925bf0. Its first 162 bytes, verbatim:
// the buffer-index globals, the buffer stride (idx*11*64 = 0x2c0), the inline
// small-vector (capacity 8 at +0x290, data at +0x10), the count at +0x2d0, the
// 0x50-byte entries keyed at +0, and the row loads at +0x20/+0x30/+0x40.
inline constexpr uint8_t kFetchSig[162] = {
    0x64, 0x8b, 0x04, 0x25, 0xe0, 0xd9, 0xff, 0xff, 0x49, 0x89, 0xf9, 0x49,
    0x89, 0xf0, 0x8b, 0x3d, 0x28, 0x18, 0x55, 0x02, 0x48, 0x89, 0xd6, 0x85,
    0xc0, 0x74, 0x06, 0x8b, 0x3d, 0x1f, 0x18, 0x55, 0x02, 0x48, 0x8d, 0x04,
    0xbf, 0x48, 0x8d, 0x04, 0x47, 0x48, 0xc1, 0xe0, 0x06, 0x49, 0x8d, 0x44,
    0x01, 0x30, 0x48, 0x83, 0xb8, 0x90, 0x02, 0x00, 0x00, 0x08, 0x48, 0x8d,
    0x50, 0x10, 0x74, 0x04, 0x48, 0x8b, 0x50, 0x10, 0x48, 0x8d, 0x04, 0xbf,
    0x48, 0x8d, 0x04, 0x47, 0x48, 0xc1, 0xe0, 0x06, 0x49, 0x8b, 0x84, 0x01,
    0xd0, 0x02, 0x00, 0x00, 0x48, 0x8d, 0x0c, 0x80, 0x48, 0xc1, 0xe1, 0x04,
    0x48, 0x01, 0xd1, 0x48, 0x39, 0xd1, 0x0f, 0x84, 0xf4, 0x00, 0x00, 0x00,
    0x48, 0x89, 0xd0, 0xeb, 0x1c, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x40, 0x00, 0x48, 0x83, 0xc0, 0x50,
    0x48, 0x39, 0xc1, 0x0f, 0x84, 0xdb, 0x00, 0x00, 0x00, 0x48, 0x39, 0x30,
    0x75, 0xee, 0x41, 0x0f, 0x28, 0x08, 0x0f, 0x28, 0x58, 0x30, 0x0f, 0x28,
    0x68, 0x20, 0x0f, 0x28, 0x50, 0x40};

// The layout the signature pins. Named so a reader need not re-derive them
// from the bytes above.
inline constexpr uintptr_t kBufBase = 0x30;      // lea 0x30(%r9,idx*0x2c0)
inline constexpr uintptr_t kBufStride = 0x2c0;   // idx*5*2... then <<6
inline constexpr uintptr_t kCapOff = 0x290;      // cmpq $0x8,0x290(buf)
inline constexpr uint64_t kInlineCap = 8;
inline constexpr uintptr_t kDataOff = 0x10;      // inline data, or pointer
inline constexpr uintptr_t kCountOff = 0x2d0;    // from cam + idx*0x2c0
inline constexpr uintptr_t kEntryStride = 0x50;  // add $0x50,%rax
inline constexpr uintptr_t kRow0 = 0x20;         // movaps 0x20(%rax)

struct Anchors {
    bool ok = false;
    uintptr_t fetch = 0, g0 = 0, g1 = 0;
    const char *why = "not decoded";
};

// `read(va, out, n)` returns false for anything it cannot read: the injector
// reads its own mapping, the test reads the ELF file on disk.
template <class Reader>
Anchors decode_anchors(uintptr_t get_camera_rotation, Reader read) {
    Anchors a;
    uint8_t call[6];
    if (!get_camera_rotation || !read(get_camera_rotation + 0x5d, call, 6)) {
        a.why = "GetCameraRotation unreadable";
        return a;
    }
    if (call[0] != 0x40 || call[1] != 0xe8) {
        a.why = "GetCameraRotation+0x5d is not the REX call into the fetch";
        return a;
    }
    int32_t rel;
    memcpy(&rel, call + 2, 4);
    a.fetch = get_camera_rotation + 0x63 + (intptr_t)rel;
    uint8_t sig[sizeof kFetchSig];
    if (!read(a.fetch, sig, sizeof sig)) {
        a.why = "fetch function unreadable";
        return a;
    }
    if (memcmp(sig, kFetchSig, sizeof sig) != 0) {
        a.why = "fetch prologue differs from X4 9.00's — another build; "
                "refusing to aim at a guessed layout";
        return a;
    }
    int32_t d0, d1;
    memcpy(&d0, sig + 0x10, 4);
    memcpy(&d1, sig + 0x1d, 4);
    a.g0 = a.fetch + 0x14 + (intptr_t)d0;
    a.g1 = a.fetch + 0x21 + (intptr_t)d1;
    a.ok = true;
    a.why = "decoded and signature-verified";
    return a;
}

struct Target {
    uintptr_t addr = 0; // the watched 8 bytes (a rotation row's x,y)
    uint32_t buf = 0;   // hop 1: which per-frame buffer; hop 2: target index
    char label[24] = {}; // hop 2: "controller", "child0".. -- empty in hop 1
};

// The key-0 entry's row 0, in EVERY buffer that looks valid (up to four, the
// number of hardware watch slots per thread) -- not just the two the index
// globals name right now. If X4 triple-buffers, the writer fills a buffer
// neither global points at yet, and watching only the named ones would report
// a clean null over a store that happened next door. A buffer is valid if its
// count is sane and it holds a key-0 entry; the index globals are returned for
// the log. This is the entry GetCameraRotation reads (it passes key 0).
template <class Reader>
std::vector<Target> targets(uintptr_t cam, const Anchors &a, Reader read,
                            uint32_t *named0 = nullptr,
                            uint32_t *named1 = nullptr) {
    std::vector<Target> out;
    if (!a.ok || !cam)
        return out;
    uint32_t idx[2] = {~0u, ~0u};
    read(a.g0, &idx[0], 4);
    read(a.g1, &idx[1], 4);
    if (named0)
        *named0 = idx[0];
    if (named1)
        *named1 = idx[1];
    for (uint32_t k = 0; k < 4; k++) {
        const uintptr_t buf = cam + kBufBase + k * kBufStride;
        uint64_t cap = 0, count = 0;
        uintptr_t data = 0;
        if (!read(buf + kCapOff, &cap, 8) ||
            !read(cam + k * kBufStride + kCountOff, &count, 8))
            continue;
        if (count == 0 || count > 64 || cap < count)
            continue;
        if (cap == kInlineCap)
            data = buf + kDataOff;
        else if (!read(buf + kDataOff, &data, 8) || !data)
            continue;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t key = 1;
            if (!read(data + i * kEntryStride, &key, 8))
                break;
            if (key == 0) {
                out.push_back({data + i * kEntryStride + kRow0, k});
                break;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- hop 2
//
// Take 316 found ONE writer of the matrix above: a per-frame snapshot
// (0x192a310) that asks 0x1929f70 for the camera's transform. Read statically
// from there: for the player's camera, 0x1929f70 takes the controller at
// camera+0x18 and composes its local transform, which for the controller class
// seen in take 316 (vtable 0x3b81fe0) is 0x191d0f0 at vtable+0x1b0. That
// function starts from the controller's STORED matrix (+0x10..+0x40) and
// composes a CHAIN of child controllers (pointer vector +0xa0..+0xa8), each
// with its own stored matrix. The free-look is in one of those. Hop 2 watches
// the controller's and the first children's stored row 0 and reports which
// moves with the head, and who writes it.
//
// Every offset is tied to X4's bytes: the sites below must match exactly, and
// the controller's vtable+0x1b0 must BE 0x191d0f0, or hop 2 refuses -- a
// different controller class could lay its chain out differently.
struct Site {
    uintptr_t va;
    uint8_t n;
    uint8_t bytes[12]; // 12: a movss with SIB and disp32 is 10 bytes
};
inline constexpr Site kHop2Sites[] = {
    // 0x1929f70: `mov 0x18(%rbx),%r12` -- the controller, both branches.
    {0x1929ff6, 4, {0x4c, 0x8b, 0x63, 0x18}},
    {0x192a2f9, 4, {0x4c, 0x8b, 0x63, 0x18}},
    // 0x191d0f0: the chain's end and begin.
    {0x191d17e, 7, {0x4c, 0x8b, 0xa5, 0xa8, 0x00, 0x00, 0x00}},
    {0x191d185, 7, {0x4c, 0x8b, 0xbd, 0xa0, 0x00, 0x00, 0x00}},
    // 0x191d0f0: the stored matrix, translation +0x10 then row 0 +0x20.
    {0x191d158, 4, {0x0f, 0x28, 0x45, 0x10}},
    {0x191d15f, 5, {0x66, 0x0f, 0x6f, 0x45, 0x20}},
    // The base compose every child goes through: row 0 at +0x20 there too.
    {0x19186ac, 5, {0x66, 0x0f, 0x6f, 0x47, 0x20}},
};
inline constexpr uintptr_t kControllerOff = 0x18;
inline constexpr uintptr_t kChainBegin = 0xa0, kChainEnd = 0xa8;
inline constexpr uintptr_t kStoredRow0 = 0x20;
inline constexpr uintptr_t kVtLocal = 0x1b0, kVtCompose = 0x138;
inline constexpr uintptr_t kLocalTransformFn = 0x191d0f0;

template <class Reader>
bool verify_sites(const Site *sites, size_t n, Reader read, uintptr_t *bad) {
    for (size_t i = 0; i < n; i++) {
        const Site &st = sites[i];
        uint8_t b[sizeof st.bytes];
        if (st.n > sizeof st.bytes || !read(st.va, b, st.n) ||
            memcmp(b, st.bytes, st.n) != 0) {
            if (bad)
                *bad = st.va;
            return false;
        }
    }
    return true;
}

template <class Reader> bool verify_hop2_sites(Reader read, uintptr_t *bad) {
    return verify_sites(kHop2Sites, sizeof kHop2Sites / sizeof kHop2Sites[0],
                        read, bad);
}

struct Chain {
    bool ok = false;
    const char *why = "not read";
    uintptr_t controller = 0, vtable = 0, local_fn = 0;
    struct Child {
        uintptr_t obj = 0, vtable = 0, compose = 0, local = 0;
    };
    std::vector<Child> children;
};

template <class Reader> Chain read_chain(uintptr_t cam, Reader read) {
    Chain c;
    if (!cam || !read(cam + kControllerOff, &c.controller, 8) || !c.controller) {
        c.why = "no controller at camera+0x18";
        return c;
    }
    if (!read(c.controller, &c.vtable, 8) || !c.vtable ||
        !read(c.vtable + kVtLocal, &c.local_fn, 8)) {
        c.why = "controller vtable unreadable";
        return c;
    }
    // local_fn is this build's own address, read from the object.
    if (c.local_fn != reloc::va(kLocalTransformFn)) {
        c.why = "controller class differs from take 316's (its vtable+0x1b0 is "
                "not 0x191d0f0) — the chain layout is unverified, refusing";
        return c;
    }
    uintptr_t b = 0, e = 0;
    if (!read(c.controller + kChainBegin, &b, 8) ||
        !read(c.controller + kChainEnd, &e, 8) || e < b || (e - b) % 8 ||
        (e - b) / 8 > 32) {
        c.why = "child vector unreadable or implausible";
        return c;
    }
    for (uintptr_t p = b; p < e; p += 8) {
        Chain::Child ch;
        if (!read(p, &ch.obj, 8) || !ch.obj || !read(ch.obj, &ch.vtable, 8))
            continue;
        read(ch.vtable + kVtCompose, &ch.compose, 8);
        read(ch.vtable + kVtLocal, &ch.local, 8);
        c.children.push_back(ch);
    }
    c.ok = true;
    c.why = "controller class verified, chain read";
    return c;
}

// Row 0 of the controller's stored matrix, then of the first children, up to
// the four debug registers a thread has.
inline std::vector<Target> chain_targets(const Chain &c) {
    std::vector<Target> out;
    if (!c.ok)
        return out;
    Target t;
    t.addr = c.controller + kStoredRow0;
    t.buf = 0;
    snprintf(t.label, sizeof t.label, "controller");
    out.push_back(t);
    for (size_t i = 0; i < c.children.size() && out.size() < 4; i++) {
        Target u;
        u.addr = c.children[i].obj + kStoredRow0;
        u.buf = (uint32_t)(i + 1);
        snprintf(u.label, sizeof u.label, "child%u", (unsigned)(i & 0xff));
        out.push_back(u);
    }
    return out;
}

// ---------------------------------------------------------------- hop 2b
//
// Take 317: the controller's stored matrix is the camera's G-force SWAY, not
// the free-look -- it never rotated more than ~9 degrees while the head swung
// the camera through 131 -- and both children are fixed rotations. What 0x191d0f0
// has left is a transform on the object at global 0x3db83c8 (translation
// +0x5e0, rotation rows +0x5f0/+0x600/+0x610), whose Euler angles it extracts,
// scales by a factor, and composes. Not the head tracker: none of X4's
// head-tracker exports reads that global (they use 0x3db8938/0x3daab80); of
// the camera exports only GetFollowCameraBasePos does. Hop 2b watches that
// transform, with the sway matrix kept as the reference.
inline constexpr Site kHop2bSites[] = {
    // `mov 0x249b2b7(%rip),%r14` -- the settings global, 0x3db83c8.
    {0x191d10a, 7, {0x4c, 0x8b, 0x35, 0xb7, 0xb2, 0x49, 0x02}},
    // `movss 0x5f4(%r14)`, `movss 0x604(%r14)`: the rotation rows it reads.
    {0x191d22f, 8, {0xf3, 0x41, 0x0f, 0x10, 0x86, 0xf4, 0x05, 0x00}},
    {0x191d238, 8, {0xf3, 0x41, 0x0f, 0x10, 0x8e, 0x04, 0x06, 0x00}},
    // `movaps 0x5e0(%r14)`: the translation it adds.
    {0x191d39d, 8, {0x41, 0x0f, 0x28, 0x86, 0xe0, 0x05, 0x00, 0x00}},
};
inline constexpr uintptr_t kSettingsPos = 0x5e0, kSettingsRow0 = 0x5f0;

template <class Reader> bool verify_hop2b_sites(Reader read, uintptr_t *bad) {
    return verify_sites(kHop2bSites, sizeof kHop2bSites / sizeof kHop2bSites[0],
                        read, bad);
}

// The global's address, from the displacement in the verified instruction.
inline uintptr_t settings_global() {
    int32_t d;
    memcpy(&d, kHop2bSites[0].bytes + 3, 4);
    return kHop2bSites[0].va + 7 + (intptr_t)d;
}

inline std::vector<Target> settings_targets(uintptr_t obj, const Chain &c) {
    std::vector<Target> out;
    if (!obj)
        return out;
    Target r;
    r.addr = obj + kSettingsRow0;
    r.buf = 0;
    snprintf(r.label, sizeof r.label, "settings.rot");
    out.push_back(r);
    Target p;
    p.addr = obj + kSettingsPos;
    p.buf = 1;
    snprintf(p.label, sizeof p.label, "settings.pos");
    out.push_back(p);
    if (c.ok) {
        Target w;
        w.addr = c.controller + kStoredRow0;
        w.buf = 2;
        snprintf(w.label, sizeof w.label, "controller"); // the sway, reference
        out.push_back(w);
    }
    return out;
}

// ---------------------------------------------------------------- hop 3
//
// Take 318: the settings object is STATIC -- a constant 0.14 deg rotation and a
// zero translation -- and the controller's matrix is the sway again, 19 deg at
// most while the camera turned a full circle. Nothing 0x191d0f0 builds is the
// free-look. But 0x1929f70 does not stop at the controller: after composing it
// it composes ONE more transform, the camera object's OWN, at camera+0x5a0
// (translation) and +0x5b0/+0x5c0/+0x5d0 (rotation rows), on every path
// including the cockpit's (mode 0). Hop 2 read only the controller, and take
// 315's value scan stopped at the camera's first KB -- 0x5a0 is past it.
inline constexpr Site kHop3Sites[] = {
    // 0x1929f70: the player's-camera test, camera == root+0x3e8.
    {0x192a027, 7, {0x48, 0x3b, 0x98, 0xe8, 0x03, 0x00, 0x00}},
    // 0x1929f70: the mode switch, camera+0x880.
    {0x1929f9d, 6, {0x8b, 0x83, 0x80, 0x08, 0x00, 0x00}},
    // 0x1929f70: the camera's own transform, composed last -- translation,
    // then the three rotation rows.
    {0x192a15e, 7, {0x0f, 0x28, 0x9b, 0xa0, 0x05, 0x00, 0x00}},
    {0x192a165, 7, {0x0f, 0x28, 0x93, 0xb0, 0x05, 0x00, 0x00}},
    {0x192a16c, 7, {0x0f, 0x28, 0x8b, 0xc0, 0x05, 0x00, 0x00}},
    {0x192a173, 7, {0x0f, 0x28, 0x83, 0xd0, 0x05, 0x00, 0x00}},
};
inline constexpr uintptr_t kCamPos = 0x5a0, kCamRow0 = 0x5b0, kCamMode = 0x880;

template <class Reader> bool verify_hop3_sites(Reader read, uintptr_t *bad) {
    return verify_sites(kHop3Sites, sizeof kHop3Sites / sizeof kHop3Sites[0],
                        read, bad);
}

// The camera's own rotation and translation; the controller's sway matrix as
// the reference when its class is verified (it is the census's positive
// control: take 318 saw it written and moving, so a silent census is the
// instrument, not X4).
inline std::vector<Target> camera_local_targets(uintptr_t cam, const Chain &c) {
    std::vector<Target> out;
    if (!cam)
        return out;
    Target r;
    r.addr = cam + kCamRow0;
    r.buf = 0;
    snprintf(r.label, sizeof r.label, "camera.rot");
    out.push_back(r);
    Target p;
    p.addr = cam + kCamPos;
    p.buf = 1;
    snprintf(p.label, sizeof p.label, "camera.pos");
    out.push_back(p);
    if (c.ok) {
        Target w;
        w.addr = c.controller + kStoredRow0;
        w.buf = 2;
        snprintf(w.label, sizeof w.label, "controller"); // the sway, reference
        out.push_back(w);
    }
    return out;
}

// Is what was read a live transform? Twelve floats in the census's layout
// (rows of x,y,z,w). For a rotation target all three rows are the matrix; for
// a translation target (".pos") the translation comes first and the next two
// rows are the object's first two rotation rows. Take 318's census read the
// controller after X4 freed it at exit: an all-zero matrix, whose trace gives
// acos(-1/2) = 120 deg, and the scorer called the sway "FOLLOWS THE CAMERA and
// spans its range" on that one read.
inline bool rows_orthonormal(const float *a, const float *b, float tol) {
    const float na = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    const float nb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::fabs(na - 1.f) < tol && std::fabs(nb - 1.f) < tol &&
           std::fabs(d) < tol;
}
inline bool live_transform(const float *rows, bool is_pos) {
    constexpr float tol = 0.05f;
    if (is_pos)
        return rows_orthonormal(rows + 4, rows + 8, tol);
    if (!rows_orthonormal(rows, rows + 4, tol) ||
        !rows_orthonormal(rows + 4, rows + 8, tol) ||
        !rows_orthonormal(rows, rows + 8, tol))
        return false;
    const float det = rows[0] * (rows[5] * rows[10] - rows[6] * rows[9]) -
                      rows[1] * (rows[4] * rows[10] - rows[6] * rows[8]) +
                      rows[2] * (rows[4] * rows[9] - rows[5] * rows[8]);
    return std::fabs(det - 1.f) < tol;
}

// The rotation angle of a 3x3 (rows of 4), and of X4's camera from its yaw and
// pitch. With roll zero the angle of Ry*Rx and of Rx*Ry is the same, so no
// convention has to be guessed: cos = (cos y + cos p + cos y cos p - 1) / 2.
// The free-look matrix, if this is it, has the camera's angle; the sway and a
// static matrix do not.
inline float rotation_angle_deg(const float *rows) {
    const float tr = rows[0] + rows[5] + rows[10];
    const float c = std::fmax(-1.f, std::fmin(1.f, (tr - 1.f) * 0.5f));
    return std::acos(c) * 57.2957795130823f;
}
inline float yaw_pitch_angle_deg(float yaw_rad, float pitch_rad) {
    const float cy = std::cos(yaw_rad), cp = std::cos(pitch_rad);
    const float c =
        std::fmax(-1.f, std::fmin(1.f, (cy + cp + cy * cp - 1.f) * 0.5f));
    return std::acos(c) * 57.2957795130823f;
}

// ---------------------------------------------------------------- samples
//
// x86-64 perf register numbering: AX BX CX DX SI DI BP SP IP = 0..8, then
// FLAGS CS SS DS ES FS GS = 9..15 (DS..GS are refused for 64-bit), R8..R15 =
// 16..23. Seventeen registers, in ascending bit order in the record.
inline constexpr uint64_t kRegsMask = 0x1ffull | (0xffull << 16);
inline constexpr int kNumRegs = 17;
inline const char *reg_name(int i) {
    static const char *n[kNumRegs] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                      "rbp", "rsp", "rip", "r8",  "r9",  "r10",
                                      "r11", "r12", "r13", "r14", "r15"};
    return i >= 0 && i < kNumRegs ? n[i] : "?";
}
inline constexpr uint32_t kStackBytes = 256;

struct Sample {
    uint64_t ip = 0;   // AFTER the store: data breakpoints trap post-write
    uint32_t pid = 0, tid = 0;
    uint64_t abi = 0;
    uint64_t regs[kNumRegs] = {};
    uint32_t stack_len = 0;
    uint8_t stack[kStackBytes] = {};
};

// One PERF_RECORD_SAMPLE body (after the header), laid out for
// IP | TID | REGS_USER | STACK_USER. Pure, so the layout is tested without a
// kernel.
inline bool parse_sample(const uint8_t *p, size_t n, Sample *s) {
    size_t o = 0;
    auto take = [&](void *dst, size_t k) {
        if (o + k > n)
            return false;
        memcpy(dst, p + o, k);
        o += k;
        return true;
    };
    if (!take(&s->ip, 8) || !take(&s->pid, 4) || !take(&s->tid, 4) ||
        !take(&s->abi, 8))
        return false;
    if (s->abi && !take(s->regs, 8 * kNumRegs))
        return false;
    uint64_t size = 0;
    if (!take(&size, 8))
        return false;
    if (size) {
        const size_t keep = size < kStackBytes ? (size_t)size : kStackBytes;
        if (o + size > n)
            return false;
        memcpy(s->stack, p + o, keep);
        o += size;
        uint64_t dyn = 0;
        if (!take(&dyn, 8))
            return false;
        s->stack_len = (uint32_t)(dyn < keep ? dyn : keep);
    }
    return true;
}

inline std::vector<pid_t> threads_of_self() {
    std::vector<pid_t> out;
    if (DIR *d = opendir("/proc/self/task")) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
                out.push_back((pid_t)atoi(e->d_name));
        closedir(d);
    }
    return out;
}

// ---------------------------------------------------------------- the watch
//
// **Two kinds of event, because locked memory is the scarce thing.** A ring
// buffer is locked memory, and an unprivileged user gets perf_event_mlock_kb
// (516 KB here) per CPU plus RLIMIT_MEMLOCK. One 68 KB ring per X4 thread per
// target would run out partway through X4's threads and silently leave the
// rest unwatched. So every thread gets a COUNTING event (no ring, no memory,
// just the hardware counter), and only threads whose counter moves -- the
// writers -- are upgraded to a SAMPLING event with a ring. The count carried
// out of the counting event keeps stores() exact across the upgrade.
class WriteWatch {
public:
    // Power of two. A sample is ~450 bytes (17 registers, 256 bytes of
    // stack), so 64 KB holds ~140 between polls. It WILL overflow on a burst,
    // and the kernel only writes a LOST record when there is room for the next
    // record -- tests/camwatch.cpp caught 74 of 1000 stores with "lost 0". So
    // the exact number of stores comes from the counters (stores()), and
    // samples are what we could keep.
    static constexpr size_t kDataPages = 16;

    struct Ev {
        int fd = -1;
        uint8_t *mm = nullptr;
        size_t len = 0;
        pid_t tid = 0;
        Target target;
        bool sampling = false;
    };

    ~WriteWatch() { disarm_all(); }

    // 0 on success, else errno. ENOSPC: no free debug register on that
    // thread. EPERM from mmap: the locked-memory allowance is spent.
    int arm(pid_t tid, const Target &t, bool sampling) {
        perf_event_attr a;
        memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = PERF_TYPE_BREAKPOINT;
        a.bp_type = HW_BREAKPOINT_W;
        a.bp_addr = t.addr;
        a.bp_len = HW_BREAKPOINT_LEN_8;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        if (sampling) {
            a.sample_period = 1;
            a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
                            PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
            a.sample_regs_user = kRegsMask;
            a.sample_stack_user = kStackBytes;
            a.wakeup_events = 1;
        }
        const int fd = (int)syscall(SYS_perf_event_open, &a, tid, -1, -1,
                                    PERF_FLAG_FD_CLOEXEC);
        if (fd < 0)
            return errno;
        Ev e;
        e.fd = fd;
        e.tid = tid;
        e.target = t;
        e.sampling = sampling;
        if (sampling) {
            const size_t page = (size_t)sysconf(_SC_PAGESIZE);
            e.len = (1 + kDataPages) * page;
            void *mm = mmap(nullptr, e.len, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
            if (mm == MAP_FAILED) {
                const int err = errno;
                close(fd);
                return err;
            }
            e.mm = (uint8_t *)mm;
        }
        ev_.push_back(e);
        return 0;
    }

    bool armed_on(pid_t tid) const {
        for (const Ev &e : ev_)
            if (e.tid == tid)
                return true;
        return false;
    }

    // Counting events whose counter moved: the threads that WRITE a target.
    std::vector<std::pair<pid_t, Target>> writers_to_upgrade() const {
        std::vector<std::pair<pid_t, Target>> out;
        for (const Ev &e : ev_)
            if (!e.sampling && count_of(e) > 0)
                out.push_back({e.tid, e.target});
        return out;
    }

    // Replace (tid, target)'s counting event with a sampling one, carrying
    // its count so stores() stays exact. The counting event is closed FIRST:
    // two events on one address would spend two of the thread's four debug
    // registers. 0 or errno; on failure the store count is still carried.
    int upgrade(pid_t tid, const Target &t) {
        for (size_t i = 0; i < ev_.size(); i++) {
            Ev &e = ev_[i];
            if (e.sampling || e.tid != tid || e.target.addr != t.addr)
                continue;
            carry_ += count_of(e);
            close(e.fd);
            ev_.erase(ev_.begin() + (long)i);
            return arm(tid, t, true);
        }
        return ENOENT;
    }

    void disarm_all() {
        for (Ev &e : ev_) {
            carry_ += count_of(e);
            if (e.mm)
                munmap(e.mm, e.len);
            if (e.fd >= 0)
                close(e.fd);
        }
        ev_.clear();
    }

    size_t events() const { return ev_.size(); }
    size_t sampling_events() const {
        size_t n = 0;
        for (const Ev &e : ev_)
            n += e.sampling;
        return n;
    }
    uint64_t lost() const { return lost_; }

    // Every store the hardware saw, sampled or not, including events already
    // closed: read() on a perf event returns its count.
    uint64_t stores() const {
        uint64_t sum = carry_;
        for (const Ev &e : ev_)
            sum += count_of(e);
        return sum;
    }

    // Drain every ring; f(const Sample&, const Target&) for each store.
    template <class F> void poll(F f) {
        const size_t page = (size_t)sysconf(_SC_PAGESIZE);
        const size_t size = kDataPages * page;
        std::vector<uint8_t> rec;
        for (Ev &e : ev_) {
            if (!e.sampling)
                continue;
            auto *mp = (perf_event_mmap_page *)e.mm;
            const uint64_t head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
            uint64_t tail = mp->data_tail;
            const uint8_t *data = e.mm + page;
            while (tail + sizeof(perf_event_header) <= head) {
                perf_event_header h;
                copy_ring(data, size, tail, &h, sizeof h);
                if (h.size < sizeof h || tail + h.size > head)
                    break;
                rec.resize(h.size - sizeof h);
                copy_ring(data, size, tail + sizeof h, rec.data(), rec.size());
                if (h.type == PERF_RECORD_SAMPLE) {
                    Sample s;
                    if (parse_sample(rec.data(), rec.size(), &s))
                        f(s, e.target);
                } else if (h.type == PERF_RECORD_LOST && rec.size() >= 16) {
                    uint64_t n;
                    memcpy(&n, rec.data() + 8, 8);
                    lost_ += n;
                }
                tail += h.size;
            }
            __atomic_store_n(&mp->data_tail, tail, __ATOMIC_RELEASE);
        }
    }

private:
    static uint64_t count_of(const Ev &e) {
        uint64_t v = 0;
        return read(e.fd, &v, sizeof v) == (ssize_t)sizeof v ? v : 0;
    }

    static void copy_ring(const uint8_t *data, size_t size, uint64_t at,
                          void *dst, size_t n) {
        const size_t off = (size_t)(at & (size - 1));
        const size_t first = n < size - off ? n : size - off;
        memcpy(dst, data + off, first);
        if (first < n)
            memcpy((uint8_t *)dst + first, data, n - first);
    }

    std::vector<Ev> ev_;
    uint64_t lost_ = 0, carry_ = 0;
};

} // namespace camwatch
} // namespace x4vr
