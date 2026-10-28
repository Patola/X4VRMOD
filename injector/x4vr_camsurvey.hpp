// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_CAMSURVEY=1 -- which camera does X4 draw F6 (the cinematic view)
// through? READ only; nothing is written or called.
//
// Take 354: in F6 the player's camera (root+0x3e8) stayed in mode 0 with
// its rotation frozen, X4's view said "cockpit", the HUD was down with no
// menu open -- so X4 drew F6 through another camera. This census finds every
// object of the camera's class that X4's game root (0x3db6948) and its
// render-side object (0x3daa888, which SetSceneCameraActive hands the scene
// camera root+0x3e0) hold, and logs each second, per camera:
//
//   * its mode (+0x880), what it hangs from (+0x780) and its pivot slot
//     (+0x788);
//   * its own transform (+0x5a0..), as the walking census does;
//   * the pose X4 caches for it -- the entry "relative to nothing" that
//     GetCameraRotation reads through 0x1925bf0, the camera's world pose.
//
// The camera whose cached pose moves while F6 is up is the one F6 renders
// through. The cache's layout comes from 0x1925bf0 and is checked byte for
// byte before anything is read (verify_sites).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x4vr::camsurvey {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// 0x1925bf0..0x1925c86: the cache lookup. The buffer index from 0x3e7742c
// (or 0x3e77430 when a thread-local flag is set), the buffer at
// cam + idx*0x2c0 + 0x30: capacity +0x290 (8 = the entries inline at +0x10,
// else a pointer there), count +0x2a0; 0x50-byte entries, the first quad
// the frame they are relative to.
inline constexpr uint8_t kLookupBytes[] = {
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
    0x75, 0xee, 0x41, 0x0f, 0x28, 0x08};
// SetSceneCameraActive (0xc03c60): mov root(%rip),%rax; test; je;
// mov 0x3e0(%rax),%rsi; test; je; movzbl %dil,%edx (REX 40); mov
// 0x3daa888(%rip),%rdi
// -- the scene camera and the render-side object it is handed to.
inline constexpr uint8_t kSceneBytes[] = {
    0x48, 0x8b, 0x05, 0xe1, 0x2c, 0x1b, 0x03, 0x48, 0x85, 0xc0, 0x74, 0x24,
    0x48, 0x8b, 0xb0, 0xe0, 0x03, 0x00, 0x00, 0x48, 0x85, 0xf6, 0x74, 0x18,
    0x40, 0x0f, 0xb6, 0xd7, 0x48, 0x8b, 0x3d, 0x05, 0x6c, 0x1a, 0x03};
inline constexpr Site kLookup = {0x1925bf0, sizeof kLookupBytes, kLookupBytes};
inline constexpr Site kScene = {0xc03c60, sizeof kSceneBytes, kSceneBytes};

constexpr uintptr_t kRootGlobal = 0x3db6948;
constexpr uintptr_t kRenderGlobal = 0x3daa888;
constexpr uintptr_t kIndexMain = 0x3e7742c;
constexpr uintptr_t kIndexOther = 0x3e77430;
constexpr uintptr_t kPlayerCamOff = 0x3e8;
constexpr uintptr_t kSceneCamOff = 0x3e0;
constexpr uintptr_t kCamMode = 0x880;
constexpr uintptr_t kCamAttached = 0x780;
constexpr uintptr_t kCamPivotSlot = 0x788;
constexpr uintptr_t kCamOwn = 0x5a0;
constexpr uintptr_t kBufStride = 0x2c0;
constexpr uintptr_t kBufBase = 0x30;
constexpr uintptr_t kBufCap = 0x290, kBufData = 0x10, kBufCount = 0x2a0;
constexpr uintptr_t kEntrySize = 0x50, kEntryXform = 0x10;
constexpr uint64_t kMaxEntries = 64; // the parent chain is a handful deep

template <class Reader> bool same_bytes(Reader read, const Site &s, uintptr_t *bad) {
    uint8_t b[160];
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

inline uintptr_t riprel(const Site &s, size_t at, size_t len) {
    int32_t d = 0;
    memcpy(&d, s.bytes + at + len - 4, 4);
    return s.va + at + len + (intptr_t)d;
}

// SetSceneCameraActive alone: the root global and its scene camera at
// +0x3e0. X4VR_HEADLOOK_EXTERNAL reads that slot (take 355: non-NULL exactly
// while F6, the cinematic view, is up).
template <class Reader> bool verify_scene_site(Reader read, uintptr_t *bad) {
    if (!same_bytes(read, kScene, bad))
        return false;
    if (riprel(kScene, 0, 7) != kRootGlobal) {
        *bad = kScene.va;
        return false;
    }
    uint32_t off = 0;
    memcpy(&off, kSceneBytes + 15, 4);
    if (off != kSceneCamOff) {
        *bad = kScene.va + 12;
        return false;
    }
    return true;
}

// The bytes, and what they decode to: the two index globals, the root
// global, root+0x3e0 and the render-side global.
template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kLookup, &kScene})
        if (!same_bytes(read, *s, bad))
            return false;
    if (riprel(kLookup, 0x0e, 6) != kIndexMain) {
        *bad = kLookup.va + 0x0e;
        return false;
    }
    if (riprel(kLookup, 0x1b, 6) != kIndexOther) {
        *bad = kLookup.va + 0x1b;
        return false;
    }
    if (riprel(kScene, 0, 7) != kRootGlobal) {
        *bad = kScene.va;
        return false;
    }
    uint32_t off = 0;
    memcpy(&off, kSceneBytes + 15, 4);
    if (off != kSceneCamOff) {
        *bad = kScene.va + 12;
        return false;
    }
    if (riprel(kScene, 0x1c, 7) != kRenderGlobal) {
        *bad = kScene.va + 0x1c;
        return false;
    }
    return true;
}

// The camera's cached pose relative to `rel` (0: the world entry
// GetCameraRotation reads) in buffer `idx`: translation, then three rows, 16
// floats. False when there is no such entry or the buffer is not believable.
template <class Reader>
bool cached_pose(Reader read, uintptr_t cam, uint32_t idx, uintptr_t rel,
                 float out[16]) {
    if (idx > 4)
        return false;
    const uintptr_t buf = cam + (uintptr_t)idx * kBufStride + kBufBase;
    uint64_t cap = 0, count = 0;
    uintptr_t data = 0;
    if (!read(buf + kBufCap, &cap, 8) || !read(buf + kBufCount, &count, 8))
        return false;
    if (count == 0 || count > kMaxEntries || count > cap)
        return false;
    if (cap == 8)
        data = buf + kBufData;
    else if (!read(buf + kBufData, &data, 8) || !data)
        return false;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t e[kEntrySize];
        if (!read(data + i * kEntrySize, e, sizeof e))
            return false;
        uintptr_t r = 0;
        memcpy(&r, e, 8);
        if (r == rel) {
            memcpy(out, e + kEntryXform, 16 * sizeof(float));
            return true;
        }
    }
    return false;
}

// A believable heap pointer: aligned, above the low pages and X4's image,
// inside user space.
inline bool plausible(uintptr_t p) {
    return p && (p & 7) == 0 && p > 0x10000000 && p < 0x800000000000ull;
}

// Every offset in `blob` (a container object's first `n` bytes) holding an
// object whose vtable is one of `vts` (`nvt` of them). At most `max`.
template <class Reader>
int find_objects(Reader read, const uint8_t *blob, size_t n, const uintptr_t *vts,
                 int nvt, uint32_t *offs, int max) {
    int k = 0;
    for (size_t off = 0; off + 8 <= n && k < max; off += 8) {
        uintptr_t p = 0;
        memcpy(&p, blob + off, 8);
        if (!plausible(p))
            continue;
        uintptr_t vt = 0;
        if (!read(p, &vt, 8))
            continue;
        for (int v = 0; v < nvt; v++)
            if (vts[v] && vt == vts[v]) {
                offs[k++] = (uint32_t)off;
                break;
            }
    }
    return k;
}

} // namespace x4vr::camsurvey
