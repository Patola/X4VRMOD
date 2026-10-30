// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HUD_MOUSE, checked without X4 (x4vr_uimouse.hpp):
//   * the sites against the real binary (the function, both paths, the
//     setter, the vtable's two slots), a changed byte refused where it is;
//   * the stub EXECUTED as 0xa70725 enters it -- xmm0/xmm1 the centred
//     pointer, rbx the object, ebp the flag, r12 the setter -- with X4's OWN
//     setter bytes (0x2317e00, position-independent) as the callee: what X4's
//     UI library would store at +0x8 and +0xa9. Off: X4's values. On: shifted
//     and scaled. r11 kept, the continuation reached.
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
// The continuation: mov %r11,0xb8(%rbx); ret
static const uint8_t kCont[] = {0x4c, 0x89, 0x9b, 0xb8, 0x00, 0x00, 0x00, 0xc3};

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
              "0xa704a0, both paths, the setter and the vtable's slots 2/3 "
              "match X4 9.00, the live path's jmp landing on 0xa70725");
        for (const uimouse::Site *s : {&uimouse::kEntry, &uimouse::kClickPath,
                                       &uimouse::kLivePath, &uimouse::kSetter}) {
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

    uint8_t *page = (uint8_t *)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
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
    static uimouse::Slot slot;
    uint8_t code[uimouse::kStubMax];
    const size_t len = uimouse::make_stub(code, (uintptr_t)page, (uintptr_t)&slot,
                                          (uintptr_t)(page + 3072));
    check(len > 0 && len <= uimouse::kStubMax, "the stub fits its buffer");
    memcpy(page, code, len);

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
    slot.on.store(0);
    auto [x0, y0, f0, r0] = run(257.f, 306.f, 1, &o);
    check(x0 == 257.f && y0 == 306.f && f0 == 1 && r0 == sentinel,
          "off: X4's setter stores X4's floats and flag; r11 kept, the "
          "continuation reached");
    check(slot.calls.load() == 1 && slot.scaled.load() == 0, "off: counted, not scaled");

    const float inv = uimouse::unscale_of(0.5252f);
    slot.add.store(uimouse::pack2(0.f, 0.f));
    slot.scale.store(uimouse::pack2(inv, inv));
    slot.on.store(1);
    auto [x1, y1, f1, r1] = run(257.f, 306.f, 0, &o);
    char what[160];
    snprintf(what, sizeof what,
             "on: take 368's monitor drawn at (+257, +306) reaches X4's UI "
             "library at (%.0f, %.0f), where it answered (+489, +583)",
             x1, y1);
    check(std::fabs(x1 - 489.f) < 3.f && std::fabs(y1 - 583.f) < 3.f && f1 == 0 &&
              r1 == sentinel,
          what);
    slot.add.store(uimouse::pack2(-731.f, 418.f));
    auto [x2, y2, f2, r2] = run(250.f, -400.f, 1, &o);
    check(std::fabs(x2 - (250.f - 731.f) * inv) < 1e-2f &&
              std::fabs(y2 - (-400.f + 418.f) * inv) < 1e-2f && f2 == 1,
          "on, steering: (pointer + the ring's shift) x 1/A");
    check(slot.calls.load() == 3 && slot.scaled.load() == 2, "on: counted as scaled");

    uint8_t p[uimouse::kPatchLen];
    check(uimouse::patch_bytes(0x3e0000, p) && p[0] == 0xe9 && p[5] == 0xcc &&
              p[7] == 0xcc,
          "0xa70725: jmp to a stub below X4's image, then int3s");
    check(!uimouse::patch_bytes(0x7f0000000000ull, p), "out of reach: refused");
    munmap(page, 4096);
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
