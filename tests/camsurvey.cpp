// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_CAMSURVEY, checked without X4 (x4vr_camsurvey.hpp): the cache lookup
// and SetSceneCameraActive against the real binary, a changed byte refused
// where it is; the cached pose read from a planted camera, its entries inline
// and on the heap; a buffer that is not believable refused; the object scan
// finds exactly the camera-class pointers. Self-checking.
#include <cstdio>
#include <cstring>
#include <vector>

#include "../injector/x4vr_camsurvey.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// The ELF reader of tests/foot.cpp.
struct Elf {
    std::vector<uint8_t> d;
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        d.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        const bool ok = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        return ok && d.size() > 64;
    }
    bool read(uintptr_t va, void *out, size_t n) const {
        uint64_t phoff;
        uint16_t phentsize, phnum;
        memcpy(&phoff, &d[0x20], 8);
        memcpy(&phentsize, &d[0x36], 2);
        memcpy(&phnum, &d[0x38], 2);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint8_t *p = &d[phoff + (size_t)i * phentsize];
            uint32_t type;
            uint64_t off, vaddr, filesz;
            memcpy(&type, p, 4);
            memcpy(&off, p + 8, 8);
            memcpy(&vaddr, p + 16, 8);
            memcpy(&filesz, p + 32, 8);
            if (type == 1 && va >= vaddr && va + n <= vaddr + filesz) {
                memcpy(out, &d[off + (va - vaddr)], n);
                return true;
            }
        }
        return false;
    }
    // Inside a writable PT_LOAD's memory (its .bss included, which has no
    // bytes in the file).
    bool in_data(uintptr_t va, size_t n) const {
        uint64_t phoff;
        uint16_t phentsize, phnum;
        memcpy(&phoff, &d[0x20], 8);
        memcpy(&phentsize, &d[0x36], 2);
        memcpy(&phnum, &d[0x38], 2);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint8_t *p = &d[phoff + (size_t)i * phentsize];
            uint32_t type, flags;
            uint64_t vaddr, memsz;
            memcpy(&type, p, 4);
            memcpy(&flags, p + 4, 4);
            memcpy(&vaddr, p + 16, 8);
            memcpy(&memsz, p + 40, 8);
            if (type == 1 && (flags & 2) && va >= vaddr && va + n <= vaddr + memsz)
                return true;
        }
        return false;
    }
};

// Our own memory, read the way the injector reads X4's.
static bool mem_read(uintptr_t va, void *o, size_t n) {
    if (!va)
        return false;
    memcpy(o, (const void *)va, n);
    return true;
}

int main() {
    {
        static Elf elf;
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        if (!elf.load(x4)) {
            printf("SKIP   no X4 binary at %s — the sites are unchecked\n", x4);
        } else {
            auto rd = [](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
            uintptr_t bad = 0;
            check(camsurvey::verify_sites(rd, &bad),
                  "the cache lookup and SetSceneCameraActive match X4 9.00, "
                  "decoding to the index globals, the root, +0x3e0 and the "
                  "render-side global");
            for (const camsurvey::Site *s : {&camsurvey::kLookup, &camsurvey::kScene}) {
                const uintptr_t flip = s->va + s->n / 2;
                auto rd2 = [flip](uintptr_t va, void *o, size_t n) {
                    if (!elf.read(va, o, n))
                        return false;
                    if (flip >= va && flip < va + n)
                        ((uint8_t *)o)[flip - va] ^= 0x01;
                    return true;
                };
                bad = 0;
                char what[96];
                snprintf(what, sizeof what, "one byte changed at %#lx -> refused there",
                         (unsigned long)flip);
                check(!camsurvey::verify_sites(rd2, &bad) && bad == flip, what);
            }
            check(elf.in_data(camsurvey::kIndexMain, 4) &&
                      elf.in_data(camsurvey::kIndexOther, 4) &&
                      elf.in_data(camsurvey::kRenderGlobal, 8) &&
                      elf.in_data(camsurvey::kRootGlobal, 8),
                  "the globals it reads lie in X4's writable data");
        }
    }

    // A planted camera: buffer 1 with three entries relative to a zone, a
    // sector and nothing (the world), stored inline (capacity 8).
    {
        alignas(16) static uint8_t cam[0x1000];
        memset(cam, 0, sizeof cam);
        const uint32_t idx = 1;
        const uintptr_t buf = (uintptr_t)cam + idx * camsurvey::kBufStride +
                              camsurvey::kBufBase;
        auto put_entry = [](uint8_t *e, uintptr_t rel, float base) {
            memcpy(e, &rel, 8);
            float x[16];
            for (int i = 0; i < 16; i++)
                x[i] = base + (float)i;
            memcpy(e + camsurvey::kEntryXform, x, sizeof x);
        };
        uint64_t cap = 8, count = 3;
        memcpy((void *)(buf + camsurvey::kBufCap), &cap, 8);
        memcpy((void *)(buf + camsurvey::kBufCount), &count, 8);
        uint8_t *data = (uint8_t *)(buf + camsurvey::kBufData);
        put_entry(data, 0x7f0000001000, 100.f);
        put_entry(data + 0x50, 0x7f0000002000, 200.f);
        put_entry(data + 0xa0, 0, 300.f);
        float out[16] = {};
        check(camsurvey::cached_pose(mem_read, (uintptr_t)cam, idx, 0, out) &&
                  out[0] == 300.f && out[15] == 315.f,
              "inline entries: the world entry (relative to nothing) found");
        check(camsurvey::cached_pose(mem_read, (uintptr_t)cam, idx, 0x7f0000002000, out) &&
                  out[0] == 200.f,
              "inline entries: an entry relative to a given frame found");
        check(!camsurvey::cached_pose(mem_read, (uintptr_t)cam, 0, 0, out),
              "another buffer, empty: refused");
        check(!camsurvey::cached_pose(mem_read, (uintptr_t)cam, idx, 0x7f0000009000, out),
              "no entry relative to that frame: refused");
        // The same entries on the heap (capacity 16, a pointer at +0x10).
        static uint8_t heap[0x50 * 16];
        memcpy(heap, data, 0x50 * 3);
        cap = 16;
        const uintptr_t hp = (uintptr_t)heap;
        memcpy((void *)(buf + camsurvey::kBufCap), &cap, 8);
        memcpy((void *)(buf + camsurvey::kBufData), &hp, 8);
        check(camsurvey::cached_pose(mem_read, (uintptr_t)cam, idx, 0, out) &&
                  out[0] == 300.f,
              "heap entries: the world entry found through the pointer");
        count = 17;
        memcpy((void *)(buf + camsurvey::kBufCount), &count, 8);
        check(!camsurvey::cached_pose(mem_read, (uintptr_t)cam, idx, 0, out),
              "a count above the capacity: refused");
        count = 1000;
        cap = 2000;
        memcpy((void *)(buf + camsurvey::kBufCount), &count, 8);
        memcpy((void *)(buf + camsurvey::kBufCap), &cap, 8);
        check(!camsurvey::cached_pose(mem_read, (uintptr_t)cam, idx, 0, out),
              "a count no parent chain has (1000): refused");
    }

    // The object scan: two camera-class objects among a heap pointer of
    // another class, a misaligned value and small integers.
    {
        alignas(16) static uintptr_t cam_a[4], cam_b[4], other[4];
        const uintptr_t vt_cam = 0x3d0aaa0, vt_other = 0x3c00000;
        cam_a[0] = vt_cam;
        cam_b[0] = vt_cam;
        other[0] = vt_other;
        uint8_t blob[0x80] = {};
        auto at = [&](size_t off, uintptr_t v) { memcpy(blob + off, &v, 8); };
        at(0x08, (uintptr_t)other);
        at(0x10, (uintptr_t)cam_a);
        at(0x18, 42);
        at(0x20, (uintptr_t)cam_a + 3);
        at(0x40, (uintptr_t)cam_b);
        const uintptr_t vts[2] = {vt_cam, 0};
        uint32_t offs[8];
        const bool heap_ok = camsurvey::plausible((uintptr_t)cam_a);
        if (!heap_ok) {
            printf("SKIP   this process's statics are below the plausible range\n");
        } else {
            const int k = camsurvey::find_objects(mem_read, blob, sizeof blob, vts, 2, offs, 8);
            check(k == 2 && offs[0] == 0x10 && offs[1] == 0x40,
                  "the scan finds the two camera-class objects and nothing else");
            const uintptr_t vts2[2] = {vt_cam, vt_other};
            check(camsurvey::find_objects(mem_read, blob, sizeof blob, vts2, 2, offs, 8) == 3,
                  "with the second vtable (the scene camera's) it finds three");
            check(camsurvey::find_objects(mem_read, blob, sizeof blob, vts, 2, offs, 1) == 1,
                  "the scan stops at its limit");
        }
    }

    printf(g_fail ? "%d case(s) FAILED\n" : "all cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
