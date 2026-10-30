// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HUD_MOUSE, checked without X4 (x4vr_uimouse.hpp):
//   * the sites against the real binary (the feed, both its paths, the
//     setter, the input engine's slots 2/3; the layer pick's mouse load, its
//     mode test and the render engine's slot 14), a changed byte refused
//     where it is;
//   * the FEED stub executed as 0xa70725 enters it -- xmm0/xmm1 the centred
//     pointer, rbx the object, ebp the flag, r12 the setter -- with X4's OWN
//     setter bytes (0x2317e00, position-independent) as the callee: off,
//     X4's values; on, shifted by the ring and NOT scaled (take 374);
//   * the LAYER stub executed as 0xa24e79 enters it -- the mouse at (%rsp),
//     r14 the presentation: X4's three instructions replayed (xmm0 zeroed,
//     edx = +0xda8, xmm7 = the mouse); on, xmm7 scaled per axis; r11 kept,
//     the continuation reached.
// Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_uimouse.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

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

// The object X4's setter writes into, and what the continuation records.
struct alignas(16) Obj {
    uint8_t raw[0xc0];
};

// Enter as 0xa70725: rbx obj, ebp flag, r12 setter, xmm0/xmm1, r11.
extern "C" void mouse_enter(const void *stub, Obj *obj, uint32_t flag,
                            const void *setter, const float *xy, uint64_t r11);
asm(".text\n"
    ".globl mouse_enter\n"
    "mouse_enter:\n"
    "  push %rbx\n  push %rbp\n  push %r12\n"
    "  mov %rdi, %rax\n  mov %rsi, %rbx\n  mov %edx, %ebp\n  mov %rcx, %r12\n"
    "  movss (%r8), %xmm0\n  movss 4(%r8), %xmm1\n  mov %r9, %r11\n"
    "  call *%rax\n"
    "  pop %r12\n  pop %rbp\n  pop %rbx\n  ret\n");
// The feed's continuation: mov %r11,0xb8(%rbx); ret
static const uint8_t kCont[] = {0x4c, 0x89, 0x9b, 0xb8, 0x00, 0x00, 0x00, 0xc3};

// What the layer stub leaves for X4 at 0xa24e89.
struct LayerOut {
    float xy[2];     // xmm7, the mouse the ray is built from
    uint32_t xmm0lo; // must be 0 (X4's pxor)
    uint32_t edx;    // the presentation's +0xda8
    uint64_t r11;
};
// Enter as 0xa24e79: the mouse at (%rsp), r14 the presentation, xmm0 dirty.
// The continuation in the page records and unwinds this frame.
extern "C" void layer_enter(const void *stub, const void *r14, const float *xy,
                            uint64_t r11, LayerOut *out);
asm(".text\n"
    ".globl layer_enter\n"
    "layer_enter:\n"
    "  push %rbx\n  push %rbp\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
    "  mov %rsi, %r14\n  mov %rcx, %r11\n  mov %r8, %r12\n"
    "  pcmpeqd %xmm0, %xmm0\n  pcmpeqd %xmm7, %xmm7\n"
    "  sub $8, %rsp\n  mov (%rdx), %rax\n  mov %rax, (%rsp)\n"
    "  jmp *%rdi\n");
// movq %xmm7,(%r12); movd %xmm0,8(%r12); mov %edx,12(%r12); mov %r11,16(%r12);
// add $8,%rsp; pop r15..rbx; ret
static const uint8_t kLayerCont[] = {
    0x66, 0x41, 0x0f, 0xd6, 0x3c, 0x24, 0x66, 0x41, 0x0f, 0x7e, 0x44, 0x24,
    0x08, 0x41, 0x89, 0x54, 0x24, 0x0c, 0x4d, 0x89, 0x5c, 0x24, 0x10, 0x48,
    0x83, 0xc4, 0x08, 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5d,
    0x5b, 0xc3};

int main() {
    static Elf elf;
    const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
    const bool have = elf.load(x4);
    if (!have) {
        printf("SKIP   no X4 binary at %s — the patch sites are unchecked\n", x4);
    } else {
        auto rd = [](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
        uintptr_t bad = 0;
        check(uimouse::verify_sites(rd, &bad),
              "the feed (0xa704a0, both paths, the setter, slots 2/3) and the "
              "layer pick (0xa24d2f's mouse load, 0xa24e66's mode test, the "
              "render engine's slot 14) match X4 9.00");
        for (const uimouse::Site *s :
             {&uimouse::kEntry, &uimouse::kClickPath, &uimouse::kLivePath,
              &uimouse::kSetter, &uimouse::kLayerMouse, &uimouse::kLayer}) {
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
            check(!uimouse::verify_sites(rd2, &bad) && bad == flip, what);
        }
    }

    uint8_t *page = (uint8_t *)mmap(nullptr, 8192, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        check(false, "an executable page");
        return 1;
    }
    // X4's own setter, copied from the binary when present.
    uint8_t setter[sizeof uimouse::kSetterBytes];
    memcpy(setter, uimouse::kSetterBytes, sizeof setter);
    if (have)
        elf.read(uimouse::kSetter2, setter, sizeof setter);
    memcpy(page + 2048, setter, sizeof setter);
    memcpy(page + 3072, kCont, sizeof kCont);
    memcpy(page + 7168, kLayerCont, sizeof kLayerCont);
    static uimouse::Slot slot;
    uint8_t code[uimouse::kStubMax];
    const size_t len = uimouse::make_stub(code, (uintptr_t)page, (uintptr_t)&slot,
                                          (uintptr_t)(page + 3072));
    check(len > 0 && len <= uimouse::kStubMax, "the feed stub fits its buffer");
    memcpy(page, code, len);
    const size_t llen = uimouse::make_layer_stub(code, (uintptr_t)(page + 4096),
                                                 (uintptr_t)&slot,
                                                 (uintptr_t)(page + 7168));
    check(llen > 0 && llen <= uimouse::kStubMax, "the layer stub fits its buffer");
    memcpy(page + 4096, code, llen);

    const uint64_t sentinel = 0x5a5aa5a5deadbeefull;
    auto run = [&](float x, float y, uint32_t flag, Obj *o) {
        memset(o, 0x77, sizeof *o);
        const float xy[2] = {x, y};
        mouse_enter(page, o, flag, page + 2048, xy, sentinel);
        float got[2];
        memcpy(got, o->raw + 8, 8);
        uint64_t r11;
        memcpy(&r11, o->raw + 0xb8, 8);
        return std::make_tuple(got[0], got[1], o->raw[0xa9], r11);
    };
    static Obj o;
    const float inv = uimouse::unscale_of(0.5252f);
    slot.add.store(uimouse::pack2(-731.f, 418.f));
    slot.scale.store(uimouse::pack2(inv, inv));
    slot.on.store(0);
    auto [x0, y0, f0, r0] = run(257.f, 306.f, 1, &o);
    check(x0 == 257.f && y0 == 306.f && f0 == 1 && r0 == sentinel,
          "feed off: X4's setter stores X4's floats and flag; r11 kept, the "
          "continuation reached");
    check(slot.calls.load() == 1 && slot.fed.load() == 0, "feed off: counted, not fed");

    slot.add.store(uimouse::pack2(0.f, 0.f));
    slot.on.store(1);
    auto [x1, y1, f1, r1] = run(257.f, 306.f, 0, &o);
    check(x1 == 257.f && y1 == 306.f && f1 == 0 && r1 == sentinel,
          "feed on, mouse mode: every layer gets the pointer as it is — the "
          "feed no longer scales (take 374's markers)");
    slot.add.store(uimouse::pack2(-731.f, 418.f));
    auto [x2, y2, f2, r2] = run(250.f, -400.f, 1, &o);
    check(x2 == 250.f - 731.f && y2 == -400.f + 418.f && f2 == 1 && r2 == sentinel,
          "feed on, steering: the pointer plus the ring's shift, unscaled");
    check(slot.calls.load() == 3 && slot.fed.load() == 2, "feed on: counted as fed");

    // The layer stub. The presentation's +0xda8 is its viewport height.
    static uint8_t pres[0x1000];
    const uint32_t vh = 1234;
    memcpy(pres + 0xda8, &vh, 4);
    auto lrun = [&](float x, float y) {
        LayerOut out;
        memset(&out, 0x55, sizeof out);
        const float xy[2] = {x, y};
        layer_enter(page + 4096, pres, xy, sentinel, &out);
        return out;
    };
    slot.scale.store(uimouse::pack2(inv, inv));
    slot.on.store(0);
    LayerOut l0 = lrun(257.f, 306.f);
    check(l0.xy[0] == 257.f && l0.xy[1] == 306.f && l0.xmm0lo == 0 && l0.edx == vh &&
              l0.r11 == sentinel,
          "layer off: X4's three instructions — xmm7 the mouse, xmm0 zero, edx "
          "the viewport height; r11 kept, 0xa24e89 reached");
    check(slot.persp.load() == 1 && slot.scaled.load() == 0,
          "layer off: counted, not scaled");
    slot.on.store(1);
    LayerOut l1 = lrun(257.f, 306.f);
    char what[200];
    snprintf(what, sizeof what,
             "layer on: take 368's monitor drawn at (+257, +306) builds its "
             "perspective ray at (%.0f, %.0f), where X4 took the click "
             "(+489, +583)",
             l1.xy[0], l1.xy[1]);
    check(std::fabs(l1.xy[0] - 489.f) < 3.f && std::fabs(l1.xy[1] - 583.f) < 3.f &&
              l1.xmm0lo == 0 && l1.edx == vh && l1.r11 == sentinel,
          what);
    slot.scale.store(uimouse::pack2(2.f, 0.5f));
    LayerOut l2 = lrun(100.f, -60.f);
    check(l2.xy[0] == 200.f && l2.xy[1] == -30.f,
          "layer on: x by the first factor, y by the second");
    check(slot.persp.load() == 3 && slot.scaled.load() == 2,
          "layer on: counted as scaled");

    uint8_t p[uimouse::kPatchLen];
    check(uimouse::patch_bytes(0x3e0000, p) && p[0] == 0xe9 && p[5] == 0xcc &&
              p[7] == 0xcc,
          "0xa70725: jmp to a stub below X4's image, then int3s");
    check(!uimouse::patch_bytes(0x7f0000000000ull, p), "feed out of reach: refused");
    uint8_t lp[uimouse::kLayerPatchLen];
    check(uimouse::layer_patch_bytes(0x3e0000, lp) && lp[0] == 0xe9 && lp[5] == 0xcc &&
              lp[15] == 0xcc,
          "0xa24e79: jmp to a stub below X4's image, then 11 int3s");
    check(!uimouse::layer_patch_bytes(0x7f0000000000ull, lp),
          "layer out of reach: refused");
    munmap(page, 8192);
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
