// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HUD_RAY, checked without X4 (x4vr_uiray.hpp):
//   * the sites against the real binary, a changed byte refused where it is;
//   * stub A EXECUTED as 0x1d6e2b8 enters it -- xmm0 = sx*row0, xmm4 =
//     sy*row1, xmm1 = (1,1,1,0)*row2, rbp the output -- with X4's
//     continuation stood in for: X4's ray unchanged (xmm0), xmm2 kept (X4
//     reads it at 0x1d6e463), and, while on, the UI ray built through the
//     pointer / A, normalised and scaled by K, keyed to rbp;
//   * stub B EXECUTED as the UI casts enter it -- r15 the buffer, rax live --
//     with the cast stood in for: rcx the UI direction only while on and the
//     key matches, $0x17 pushed, rax and r11 kept.
// Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_uiray.hpp"

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

struct alignas(16) SeenA {
    float xmm0[4], xmm2[4];
    uint64_t r11, rbp;
};
struct SeenB {
    uint64_t pushed, rcx, rax, r11;
};

// Stub A's entry: xmm0, xmm4, xmm1, xmm2 from `in` (16 floats), rbp, r11.
extern "C" void ray_enter(const void *stub, const float *in, uint64_t rbp,
                          SeenA *out, uint64_t r11);
asm(".text\n"
    ".globl ray_enter\n"
    "ray_enter:\n"
    "  push %rbx\n  push %rbp\n  push %r12\n"
    "  mov %rdi, %rax\n"
    "  movups (%rsi), %xmm0\n  movups 16(%rsi), %xmm4\n"
    "  movups 32(%rsi), %xmm1\n  movups 48(%rsi), %xmm2\n"
    "  mov %rdx, %rbp\n  mov %rcx, %r12\n  mov %r8, %r11\n"
    "  call *%rax\n"
    "  pop %r12\n  pop %rbp\n  pop %rbx\n  ret\n");
// movups %xmm0,(%r12); movups %xmm2,0x10(%r12); mov %r11,0x20(%r12);
// mov %rbp,0x28(%r12); ret
static const uint8_t kStandInA[] = {0x41, 0x0f, 0x11, 0x04, 0x24, 0x41, 0x0f,
                                    0x11, 0x54, 0x24, 0x10, 0x4d, 0x89, 0x5c,
                                    0x24, 0x20, 0x49, 0x89, 0x6c, 0x24, 0x28,
                                    0xc3};
static_assert(offsetof(SeenA, xmm2) == 16 && offsetof(SeenA, r11) == 32 &&
                  offsetof(SeenA, rbp) == 40,
              "stand-in A's displacements");

// Stub B's entry: r15, rax (live in X4: pushed after), r11, rcx junk.
extern "C" void cast_enter(const void *stub, uint64_t r15, SeenB *out, uint64_t rax,
                           uint64_t r11);
asm(".text\n"
    ".globl cast_enter\n"
    "cast_enter:\n"
    "  push %r15\n  push %r12\n"
    "  mov %rdi, %r10\n  mov %rsi, %r15\n  mov %rdx, %r12\n"
    "  mov %rcx, %rax\n  mov %r8, %r11\n  mov $0x5555, %rcx\n"
    "  call *%r10\n"
    "  pop %r12\n  pop %r15\n  ret\n");
// pop %rdx; mov %rdx,(%r12); mov %rcx,8(%r12); mov %rax,0x10(%r12);
// mov %r11,0x18(%r12); ret
static const uint8_t kStandInB[] = {0x5a, 0x49, 0x89, 0x14, 0x24, 0x49, 0x89,
                                    0x4c, 0x24, 0x08, 0x49, 0x89, 0x44, 0x24,
                                    0x10, 0x4d, 0x89, 0x5c, 0x24, 0x18, 0xc3};

int main(int argc, char **argv) {
    // --emit A|B <path>: a stub's bytes for objdump, placed at 0x400000.
    if (argc == 4 && !strcmp(argv[1], "--emit")) {
        uint8_t code[uiray::kStubMax];
        const size_t n = argv[2][0] == 'B'
                             ? uiray::make_stub_b(code, 0x400000, 0x1111111111111111ull,
                                                  uiray::kPatchHover + 5)
                             : uiray::make_stub_a(code, 0x400000, 0x1111111111111111ull,
                                                  uiray::kContA);
        FILE *f = fopen(argv[3], "wb");
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
            check(uiray::verify_sites(rd, &bad),
                  "the ray's prologue, direction and output, both callers and "
                  "both UI casts match X4 9.00, their calls landing where read");
            for (const uiray::Site *s :
                 {&uiray::kRayEntry, &uiray::kRayDir, &uiray::kRayOut,
                  &uiray::kHoverKey, &uiray::kHoverUi, &uiray::kClickKey,
                  &uiray::kClickUi}) {
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
                check(!uiray::verify_sites(rd2, &bad) && bad == flip, what);
            }
            float k = 0.f;
            check(elf.read(uiray::kRayLength, &k, 4) && k == 100000.f,
                  "X4's ray length at 0x2d67c18 is 100000");
        }
    }

    uint8_t *page = (uint8_t *)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        check(false, "an executable page for the stubs");
        return 1;
    }
    static uiray::Slot slot;
    slot.k = 100000.f;
    const float inv = uiray::unscale_of(0.5252f);

    // 1. Stub A, executed.
    {
        memcpy(page + 3072, kStandInA, sizeof kStandInA);
        uint8_t code[uiray::kStubMax];
        const size_t len = uiray::make_stub_a(code, (uintptr_t)page, (uintptr_t)&slot,
                                              (uintptr_t)(page + 3072));
        check(len > 0 && len <= uiray::kStubMax, "stub A fits its buffer");
        memcpy(page, code, len);

        // A camera turned off the axes so every lane matters: rows of a
        // rotation (right, up, forward) and the pointer's sx, sy.
        const float yaw = 0.4f, pitch = -0.25f;
        const float r0[4] = {std::cos(yaw), 0.f, -std::sin(yaw), 0.f};
        const float r1[4] = {std::sin(yaw) * std::sin(pitch), std::cos(pitch),
                             std::cos(yaw) * std::sin(pitch), 0.f};
        const float r2[4] = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch),
                             std::cos(yaw) * std::cos(pitch), 0.f};
        const float sx = 0.31f, sy = -0.22f;
        alignas(16) float in[16];
        for (int l = 0; l < 4; l++) {
            in[l] = sx * r0[l];
            in[4 + l] = sy * r1[l];
            in[8 + l] = (l < 3 ? 1.f : 0.f) * r2[l];
            in[12 + l] = 7.f + (float)l; // xmm2: must survive
        }
        float x4d[4];
        for (int l = 0; l < 4; l++)
            x4d[l] = in[l] + in[4 + l] + in[8 + l];
        const uint64_t sentinel = 0x5a5aa5a5deadbeefull;
        alignas(16) static uint8_t outbuf[64];

        slot.on.store(0);
        slot.key.store(0);
        SeenA s{};
        ray_enter(page, in, (uint64_t)outbuf, &s, sentinel);
        bool same = true;
        for (int l = 0; l < 4; l++)
            same = same && std::fabs(s.xmm0[l] - x4d[l]) < 1e-6f &&
                   s.xmm2[l] == in[12 + l];
        check(same && s.r11 == sentinel && s.rbp == (uint64_t)outbuf,
              "off: X4's ray (xmm0) as X4 adds it, xmm2/r11/rbp kept");
        check(slot.built.load() == 0 && slot.key.load() == 0,
              "off: no UI ray built, the key untouched");

        slot.scale.store(uiray::pack2(inv, inv));
        slot.on.store(1);
        s = SeenA{};
        ray_enter(page, in, (uint64_t)outbuf, &s, sentinel);
        same = true;
        for (int l = 0; l < 4; l++)
            same = same && std::fabs(s.xmm0[l] - x4d[l]) < 1e-6f &&
                   s.xmm2[l] == in[12 + l];
        check(same && s.r11 == sentinel, "on: X4's own ray still unchanged");
        // The expected UI ray: through the pointer / A.
        float d[3], len2 = 0.f;
        for (int l = 0; l < 3; l++) {
            d[l] = inv * sx * r0[l] + inv * sy * r1[l] + r2[l];
            len2 += d[l] * d[l];
        }
        const float n = std::sqrt(len2);
        float err = 0.f;
        for (int l = 0; l < 3; l++)
            err = std::fmax(err, std::fabs(slot.dir[l] - d[l] / n * 100000.f));
        char what[160];
        snprintf(what, sizeof what,
                 "on: the UI ray through the pointer x%.4f, normalised x K "
                 "(max error %.3g of 100000), w = 0",
                 inv, err);
        check(err < 0.5f && slot.dir[3] == 0.f, what);
        check(slot.key.load() == (uint64_t)outbuf && slot.built.load() == 1,
              "on: keyed to the ray's output (rbp), counted");
        // In the camera's frame the UI ray's slope is 1/A times X4's.
        const float ux = (slot.dir[0] * r0[0] + slot.dir[1] * r0[1] + slot.dir[2] * r0[2]);
        const float uz = (slot.dir[0] * r2[0] + slot.dir[1] * r2[1] + slot.dir[2] * r2[2]);
        snprintf(what, sizeof what,
                 "on: in the camera's frame the UI ray's x slope is %.4f = "
                 "%.4f x the pointer's %.2f",
                 ux / uz, inv, sx);
        check(std::fabs(ux / uz - inv * sx) < 1e-4f, what);
    }

    // 2. Stub B, executed.
    {
        memcpy(page + 3584, kStandInB, sizeof kStandInB);
        uint8_t code[uiray::kStubMax];
        const size_t len = uiray::make_stub_b(code, (uintptr_t)(page + 1024),
                                              (uintptr_t)&slot,
                                              (uintptr_t)(page + 3584));
        check(len > 0 && len <= uiray::kStubMax, "stub B fits its buffer");
        memcpy(page + 1024, code, len);
        const uint64_t r15 = slot.key.load(); // the buffer stub A was keyed to
        const uint64_t rax = 0x1234567890abcdefull, r11 = 0xfeedfacecafebeefull;

        SeenB b{};
        slot.on.store(1);
        cast_enter(page + 1024, r15, &b, rax, r11);
        check(b.rcx == (uint64_t)slot.dir && b.pushed == 0x17 && b.rax == rax &&
                  b.r11 == r11,
              "on, key matches: rcx = the UI direction, $0x17 pushed, rax/r11 kept");
        cast_enter(page + 1024, r15 + 16, &b, rax, r11);
        check(b.rcx == r15 + 16 && b.pushed == 0x17,
              "on, another buffer: rcx = r15, X4's own ray");
        slot.on.store(0);
        cast_enter(page + 1024, r15, &b, rax, r11);
        check(b.rcx == r15 && b.pushed == 0x17 && b.rax == rax,
              "off: rcx = r15, X4's own ray");
        check(slot.swapped.load() == 1 && slot.passed.load() == 2,
              "counted: one swapped, two passed");
    }

    // 3. The patch bytes.
    {
        uint8_t p[8];
        check(uiray::patch_bytes(uiray::kPatchA, uiray::kPatchALen, 0x3e0000, p) &&
                  p[0] == 0xe9 && p[5] == 0xcc,
              "0x1d6e2b8: jmp to a stub below X4's image, then int3");
        check(uiray::patch_bytes(uiray::kPatchHover, uiray::kPatchBLen, 0x3e0000, p) &&
                  p[0] == 0xe9,
              "0x137a1d0: a jmp exactly over mov %r15,%rcx; push $0x17");
        check(!uiray::patch_bytes(uiray::kPatchA, 6, 0x7f0000000000ull, p),
              "a stub out of rel32 reach is refused");
        check(uiray::unscale_of(0.f) == 1.f && uiray::unscale_of(NAN) == 1.f,
              "an unusable canvas factor scales by 1");
    }
    munmap(page, 4096);

    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
