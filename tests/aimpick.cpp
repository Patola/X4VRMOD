// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_STEER_PICK, checked without X4 (x4vr_aimpick.hpp):
//   * the sites against the real binary, a changed byte refused where it is,
//     and the patch's 11 bytes and its reach;
//   * the stub EXECUTED, entered the way 0x1d6e0e1 is -- r13d and ebx the
//     centred pointer -- with X4's continuation stood in for by code that
//     records what X4 would read next: the two centred values, xmm0, xmm2,
//     and r11 (saved by the stub). No shift: X4's values; a shift: added,
//     counted;
//   * the pixel arithmetic: the shift is NDC * window / 2, so the shifted
//     pointer's NDC is the pointer's plus the nose's -- where the ring is
//     drawn.
// Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_aimpick.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// The ELF reader of tests/extview.cpp: X4 is non-PIE, a VA maps through the
// PT_LOAD headers.
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

// What X4's continuation would find: the centred pointer, xmm0, xmm2, r11.
struct alignas(16) Seen {
    int32_t x, y, pad[2];
    float xmm0[4];
    float xmm2[4];
    uint64_t r11;
};

// Enter the stub as 0x1d6e0e1 is entered: r13d/ebx the centred pointer,
// xmm0/xmm2 holding junk X4's two replayed instructions must overwrite, r11
// a sentinel. The stand-in continuation (bytes in the stub's page) stores
// them through r12 and returns here.
extern "C" void aim_enter(const void *stub, int32_t cx, int32_t cy, Seen *out,
                          uint64_t r11);
asm(".text\n"
    ".globl aim_enter\n"
    "aim_enter:\n"
    "  push %rbx\n  push %r12\n  push %r13\n"
    "  mov %rcx, %r12\n  mov %esi, %r13d\n  mov %edx, %ebx\n"
    "  mov %r8, %r11\n"
    "  pcmpeqd %xmm0, %xmm0\n  pcmpeqd %xmm2, %xmm2\n"
    "  call *%rdi\n"
    "  pop %r13\n  pop %r12\n  pop %rbx\n"
    "  ret\n");

// mov %r13d,(%r12); mov %ebx,4(%r12); movups %xmm0,16(%r12);
// movups %xmm2,32(%r12); mov %r11,48(%r12); ret
static const uint8_t kStandInCont[] = {
    0x45, 0x89, 0x2c, 0x24, 0x41, 0x89, 0x5c, 0x24, 0x04, 0x41, 0x0f, 0x11,
    0x44, 0x24, 0x10, 0x41, 0x0f, 0x11, 0x54, 0x24, 0x20, 0x4d, 0x89, 0x5c,
    0x24, 0x30, 0xc3};
static_assert(offsetof(Seen, xmm0) == 16 && offsetof(Seen, xmm2) == 32 &&
                  offsetof(Seen, r11) == 48,
              "the stand-in's displacements");

int main(int argc, char **argv) {
    // --emit <path>: the stub's bytes for objdump -D -b binary -mi386:x86-64,
    // placed at 0x400000 with a placeholder slot.
    if (argc == 3 && !strcmp(argv[1], "--emit")) {
        uint8_t code[aimpick::kStubMax];
        const size_t n = aimpick::make_stub(code, 0x400000, 0x1111111111111111ull,
                                            aimpick::kConst, aimpick::kCont);
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
            check(aimpick::verify_sites(rd, &bad),
                  "the pick ray's live path, merge point, last-click and "
                  "no-mouse paths and the use of r13d/ebx match X4 9.00, "
                  "their jumps landing on 0x1d6e0e1");
            for (const aimpick::Site *s :
                 {&aimpick::kLive, &aimpick::kMerge, &aimpick::kClick,
                  &aimpick::kNoMouse, &aimpick::kUse}) {
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
                check(!aimpick::verify_sites(rd2, &bad) && bad == flip, what);
            }
            // The constant X4's movaps loads, so the replay loads the same.
            float k[4] = {};
            check(elf.read(aimpick::kConst, k, 16) && k[0] == 0.f && k[1] == 1.f &&
                      k[2] == 0.f && k[3] == 0.f,
                  "0x2e245e0, the movaps's operand, is (0, 1, 0, 0)");
            // The ray divides by window * 0.5 (0x2e25da4): a pixel is
            // NDC * window / 2, the unit shift_px works in.
            float half = 0.f;
            check(elf.read(0x2e25da4, &half, 4) && half == 0.5f,
                  "the ray's window factor at 0x2e25da4 is 0.5");
        }
    }

    // 1. The patch bytes and their reach.
    {
        uint8_t p[aimpick::kPatchLen];
        const uintptr_t near_stub = 0x3e0000;
        check(aimpick::patch_bytes(near_stub, p) && p[0] == 0xe9 &&
                  aimpick::kPatch + 5 + (intptr_t)aimpick::rel_at(p, 1) == near_stub,
              "jmp rel32 from 0x1d6e0e1 lands on a stub below X4's image");
        bool int3 = true;
        for (size_t i = 5; i < aimpick::kPatchLen; i++)
            int3 = int3 && p[i] == 0xcc;
        check(int3, "the six bytes after the jmp are int3");
        check(!aimpick::patch_bytes(0x7f0000000000ull, p),
              "a stub out of rel32 reach is refused, not truncated");
        uint8_t code[aimpick::kStubMax];
        check(aimpick::make_stub(code, 0x7f0000000000ull, 0, aimpick::kConst,
                                 aimpick::kCont) == 0,
              "a stub whose jmp back cannot reach 0x1d6e0ec is refused");
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
        memcpy(page + 2048, kStandInCont, sizeof kStandInCont);
        static aimpick::Slot slot;
        alignas(16) static const float konst[4] = {0.f, 1.f, 0.f, 0.f};
        uint8_t code[aimpick::kStubMax];
        const size_t len = aimpick::make_stub(code, (uintptr_t)page, (uintptr_t)&slot,
                                              (uintptr_t)konst,
                                              (uintptr_t)(page + 2048));
        check(len > 0 && len <= aimpick::kStubMax, "the stub fits its buffer");
        memcpy(page, code, len);

        const uint64_t sentinel = 0x5a5aa5a5deadbeefull;
        auto run = [&](int32_t cx, int32_t cy) {
            Seen s;
            memset(&s, 0x77, sizeof s);
            aim_enter(page, cx, cy, &s, sentinel);
            return s;
        };
        auto replayed = [&](const Seen &s) {
            return s.xmm0[0] == 0.f && s.xmm0[1] == 0.f && s.xmm0[2] == 0.f &&
                   s.xmm0[3] == 0.f && !memcmp(s.xmm2, konst, 16) &&
                   s.r11 == sentinel;
        };

        // No shift: X4's own values, counted as a call, not as shifted.
        slot.shift.store(0);
        Seen s = run(123, -45);
        check(s.x == 123 && s.y == -45, "no shift: the centred pointer is X4's");
        check(replayed(s), "no shift: xmm0 = 0, xmm2 = X4's constant, r11 kept");
        check(slot.calls.load() == 1 && slot.shifted.load() == 0,
              "no shift: one call, none shifted");

        // A shift: the nose 30 deg left of the gaze at X4's field puts it to
        // the right and, looking up, below -- both signs exercised.
        slot.shift.store(aimpick::pack(-731, 418));
        s = run(123, -45);
        check(s.x == 123 - 731 && s.y == -45 + 418,
              "a shift (-731, +418) is added to the centred x and y");
        check(replayed(s), "with a shift: xmm0, xmm2 and r11 as X4 leaves them");
        check(slot.calls.load() == 2 && slot.shifted.load() == 1,
              "with a shift: counted as a call and as shifted");

        // Only y non-zero still shifts: the gate is on the whole 64 bits.
        slot.shift.store(aimpick::pack(0, -9));
        s = run(0, 0);
        check(s.x == 0 && s.y == -9 && slot.shifted.load() == 2,
              "a shift with x = 0 and y = -9 is applied");

        // Withdrawn: back to X4's own.
        slot.shift.store(0);
        s = run(-1408, 1407);
        check(s.x == -1408 && s.y == 1407 && slot.shifted.load() == 2,
              "withdrawn: X4's values again, the shifted count unchanged");
        munmap(page, 4096);
    }

    // 3. The pixel arithmetic and the packing.
    {
        int32_t dx = 0, dy = 0;
        check(aimpick::shift_px(0.5f, -0.25f, 2816, 2816, &dx, &dy) && dx == 704 &&
                  dy == -352,
              "NDC (0.5, -0.25) in a 2816 window is (704, -352) px");
        // The shifted pointer's NDC is the pointer's plus the nose's: where
        // the layer draws the ring (px + nose * w / 2, x4vr_cursor_draw.hpp).
        const int w = 2816, h = 2816;
        const float nx = 0.3127f, ny = -0.618f;
        const int32_t px = 250, py = -400; // centred pointer
        check(aimpick::shift_px(nx, ny, w, h, &dx, &dy), "a nose in front");
        const float ndc_x = (float)(px + dx) / (0.5f * (float)w);
        const float ndc_y = (float)(py + dy) / (0.5f * (float)h);
        const float want_x = (float)px / (0.5f * (float)w) + nx;
        const float want_y = (float)py / (0.5f * (float)h) + ny;
        char what[160];
        snprintf(what, sizeof what,
                 "the ray's NDC is the pointer's plus the nose's (error %.5f, "
                 "%.5f; half a pixel is %.5f)",
                 ndc_x - want_x, ndc_y - want_y, 1.f / (float)w);
        check(std::fabs(ndc_x - want_x) <= 1.f / (float)w &&
                  std::fabs(ndc_y - want_y) <= 1.f / (float)h,
              what);
        check(!aimpick::shift_px(NAN, 0.f, w, h, &dx, &dy) &&
                  !aimpick::shift_px(0.f, 0.f, 0, h, &dx, &dy),
              "a non-finite nose or an unknown window: no shift");
        int32_t ux = 0, uy = 0;
        aimpick::unpack(aimpick::pack(-2000, 1999), &ux, &uy);
        check(ux == -2000 && uy == 1999, "pack/unpack keep both signs");
        check(aimpick::pack(0, 0) == 0, "a zero shift packs to 0 (the stub's gate)");
    }

    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
