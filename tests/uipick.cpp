// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HUD_PICK, checked without X4 (x4vr_uipick.hpp):
//   * the sites against the real binary, a changed byte refused where it is;
//   * the stub EXECUTED, entered the way 0xa40af3 is -- rdi = rbx = the
//     presentation, xmm1 = (cx, cy) as X4's movd/pinsrd leave it -- with
//     X4's je stood in for by code that records what X4 would see: ZF (the
//     d78 test its je reads), xmm1, obj+0xdd8 and r11. Off: X4's values. On:
//     the add for every kind, the scale only for the two perspective kinds,
//     each counted.
// Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_uipick.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// The ELF reader of tests/extview.cpp.
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
};

// What X4's je would find.
struct alignas(16) Seen {
    float xmm1[4];
    uint64_t r11;
    float dd8[2];
    uint8_t zf;
};

extern "C" void ui_enter(const void *stub, void *obj, int32_t cx, int32_t cy,
                         Seen *out, uint64_t r11);
asm(".text\n"
    ".globl ui_enter\n"
    "ui_enter:\n"
    "  push %rbx\n  push %r12\n"
    "  mov %rdi, %rax\n  mov %rsi, %rdi\n  mov %rsi, %rbx\n"
    "  mov %r8, %r12\n  mov %r9, %r11\n"
    "  movd %edx, %xmm1\n  pinsrd $1, %ecx, %xmm1\n"
    "  pcmpeqd %xmm0, %xmm0\n"
    "  call *%rax\n"
    "  pop %r12\n  pop %rbx\n  ret\n");

// sete %al; mov %al,0x20(%r12); movups %xmm1,(%r12); mov %r11,0x10(%r12);
// mov 0xdd8(%rbx),%rax; mov %rax,0x18(%r12); ret
static const uint8_t kStandInJe[] = {
    0x0f, 0x94, 0xc0, 0x41, 0x88, 0x44, 0x24, 0x20, 0x41, 0x0f, 0x11, 0x0c,
    0x24, 0x4d, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x8b, 0x83, 0xd8, 0x0d, 0x00,
    0x00, 0x49, 0x89, 0x44, 0x24, 0x18, 0xc3};
static_assert(offsetof(Seen, r11) == 16 && offsetof(Seen, dd8) == 24 &&
                  offsetof(Seen, zf) == 32,
              "the stand-in's displacements");

int main(int argc, char **argv) {
    // --emit <path>: the stub's bytes for objdump, placed at 0x400000.
    if (argc == 3 && !strcmp(argv[1], "--emit")) {
        uint8_t code[uipick::kStubMax];
        const size_t n = uipick::make_stub(code, 0x400000, 0x1111111111111111ull,
                                           uipick::kCont);
        FILE *f = fopen(argv[2], "wb");
        if (!n || !f || fwrite(code, 1, n, f) != n)
            return 1;
        fclose(f);
        printf("%zu bytes\n", n);
        return 0;
    }

    // 0. The sites, against the real binary.
    {
        static Elf elf;
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        if (!elf.load(x4)) {
            printf("SKIP   no X4 binary at %s — the patch sites are unchecked\n", x4);
        } else {
            auto rd = [](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
            uintptr_t bad = 0;
            check(uipick::verify_sites(rd, &bad),
                  "0xa40ad0's entry, its 3D-placed test and both callers "
                  "match X4 9.00, their jumps landing where read");
            for (const uipick::Site *s : {&uipick::kEntry, &uipick::kFlat,
                                          &uipick::kCaller1, &uipick::kCaller2}) {
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
                snprintf(what, sizeof what,
                         "one byte changed at %#lx -> refused there",
                         (unsigned long)flip);
                check(!uipick::verify_sites(rd2, &bad) && bad == flip, what);
            }
        }
    }

    // 1. The patch bytes and their reach.
    {
        uint8_t p[uipick::kPatchLen];
        const uintptr_t near_stub = 0x3e0000;
        check(uipick::patch_bytes(near_stub, p) && p[0] == 0xe9 &&
                  uipick::kPatch + 5 + (intptr_t)uipick::rel32_at(p, 1) == near_stub,
              "jmp rel32 from 0xa40af3 lands on a stub below X4's image");
        bool int3 = true;
        for (size_t i = 5; i < uipick::kPatchLen; i++)
            int3 = int3 && p[i] == 0xcc;
        check(int3, "the sixteen bytes after the jmp are int3");
        check(!uipick::patch_bytes(0x7f0000000000ull, p),
              "a stub out of rel32 reach is refused, not truncated");
        uint8_t code[uipick::kStubMax];
        check(uipick::make_stub(code, 0x7f0000000000ull, 0, uipick::kCont) == 0,
              "a stub whose jmp back cannot reach 0xa40b08 is refused");
    }

    // 2. The stub, executed.
    {
        uint8_t *page = (uint8_t *)mmap(nullptr, 4096,
                                        PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            check(false, "an executable page for the stub");
            return 1;
        }
        memcpy(page + 2048, kStandInJe, sizeof kStandInJe);
        static uipick::Slot slot;
        uint8_t code[uipick::kStubMax];
        const size_t len = uipick::make_stub(code, (uintptr_t)page, (uintptr_t)&slot,
                                             (uintptr_t)(page + 2048));
        check(len > 0 && len <= uipick::kStubMax, "the stub fits its buffer");
        memcpy(page, code, len);

        alignas(16) static uint8_t obj[0x1000];
        auto kind = [&](bool own_camera, bool placed) {
            memset(obj, 0, sizeof obj);
            obj[uipick::kOwnCamera] = own_camera;
            obj[uipick::kPlaced] = placed;
        };
        const uint64_t sentinel = 0x5a5aa5a5deadbeefull;
        auto run = [&](int32_t cx, int32_t cy) {
            Seen s;
            memset(&s, 0x77, sizeof s);
            ui_enter(page, obj, cx, cy, &s, sentinel);
            return s;
        };
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        auto stored = [&](const Seen &s, float x, float y) {
            return near(s.xmm1[0], x) && near(s.xmm1[1], y) && s.xmm1[2] == 0.f &&
                   s.xmm1[3] == 0.f && near(s.dd8[0], x) && near(s.dd8[1], y) &&
                   s.r11 == sentinel;
        };

        // Off: what X4 stores, the flags X4's je reads.
        slot.on.store(0);
        kind(true, false);
        Seen s = run(250, -400);
        check(stored(s, 250.f, -400.f) && s.zf == 0,
              "off, own-camera kind: X4's floats stored, ZF clear (je not taken)");
        kind(false, true);
        s = run(-7, 9);
        check(stored(s, -7.f, 9.f) && s.zf == 1,
              "off, 3D-placed kind: X4's floats, ZF set (je to the placed test)");
        check(slot.calls.load() == 2 && slot.scaled.load() == 0 &&
                  slot.flat.load() == 0,
              "off: two calls, nothing scaled or shifted");

        // On: add the ring's nose, scale the perspective kinds by 1/A.
        const float inv = uipick::unscale_of(0.5252f);
        slot.add.store(uipick::pack2(-731.f, 418.f));
        slot.scale.store(uipick::pack2(inv, inv));
        slot.on.store(1);
        kind(true, false);
        s = run(250, -400);
        char what[160];
        snprintf(what, sizeof what,
                 "on, own-camera kind: ((250-731), (-400+418)) x %.4f stored, "
                 "ZF clear",
                 inv);
        check(stored(s, (250.f - 731.f) * inv, (-400.f + 418.f) * inv) && s.zf == 0,
              what);
        kind(false, true);
        s = run(250, -400);
        check(stored(s, (250.f - 731.f) * inv, (-400.f + 418.f) * inv) && s.zf == 1,
              "on, 3D-placed kind: shifted and scaled, ZF set");
        kind(false, false);
        s = run(250, -400);
        check(stored(s, 250.f - 731.f, -400.f + 418.f) && s.zf == 1,
              "on, flat kind: shifted only, never scaled");
        check(slot.calls.load() == 5 && slot.scaled.load() == 2 &&
                  slot.flat.load() == 1,
              "on: counted -- two scaled, one flat");

        // The HUD at A: a pointer on the drawn element hits its layout spot.
        slot.add.store(0);
        kind(false, true);
        s = run(257, 306);
        snprintf(what, sizeof what,
                 "take 368's monitor: the pointer at its drawn (+257, +306) "
                 "reaches X4's (%.0f, %.0f), where it took the click (+489, +583)",
                 s.dd8[0], s.dd8[1]);
        check(std::fabs(s.dd8[0] - 489.f) < 3.f && std::fabs(s.dd8[1] - 583.f) < 3.f,
              what);

        check(uipick::unscale_of(0.f) == 1.f && uipick::unscale_of(NAN) == 1.f &&
                  uipick::unscale_of(1.5f) == 1.f,
              "an absent or unusable canvas factor scales by 1");
        munmap(page, 4096);
    }

    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
