// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HEADLOOK_EXTERNAL, checked without X4 (x4vr_extview.hpp):
//   * the sites against the real binary, a changed byte refused where it is;
//   * the stub EXECUTED with X4's own compose behind it (the very bytes
//     verify_sites checks, copied into an executable page), entered the way
//     0x192a295 enters it -- rbx the camera, rbp `out`, xmm6/7/5/8 the
//     pivot -- through a frame built like 0x1929f70's, so X4's epilogue
//     returns here. The result is compared with the product written out:
//     (H x own) x pivot for the player's camera outside mode 0, X4's own
//     own x pivot in every other case;
//   * the angle composed is the angle declared: with the orbit factored
//     out, the camera's forward is H's;
//   * the double buffer, the period bookkeeping.
// Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_camwrite.hpp"
#include "../injector/x4vr_csmfit.hpp"
#include "../injector/x4vr_extview.hpp"
#include "../injector/x4vr_foot.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// The ELF reader of tests/foot.cpp: X4 is non-PIE, a VA maps through the
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

// A transform as X4 stores it: translation, then rows 0..2, 4 floats each.
struct alignas(16) Mat {
    float m[16];
};
static Mat transform(float yaw, float pitch, float x, float y, float z) {
    Mat t{};
    camwrite::rows_for(yaw, pitch, t.m + 4);
    t.m[0] = x, t.m[1] = y, t.m[2] = z;
    return t;
}
// X4's composition: `a` applied first, then `b` (row vectors).
static Mat compose(const Mat &a, const Mat &b) {
    Mat o{};
    for (int c = 0; c < 3; c++) {
        o.m[c] = b.m[c];
        for (int k = 0; k < 3; k++)
            o.m[c] += a.m[k] * b.m[4 + 4 * k + c];
    }
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float v = 0.f;
            for (int k = 0; k < 3; k++)
                v += a.m[4 + 4 * r + k] * b.m[4 + 4 * k + c];
            o.m[4 + 4 * r + c] = v;
        }
    return o;
}
// Lanes 0-2 of the translation and the three rows; X4 leaves lane 3 as junk.
static float max_diff(const Mat &a, const Mat &b) {
    float d = 0.f;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++)
            d = std::fmax(d, std::fabs(a.m[4 * r + c] - b.m[4 * r + c]));
    return d;
}

// 0x1929f70's frame: push r13, r12, rbp, rbx; sub $0x58. Then rbx = the
// camera, rbp = `out`, xmm6/7/5/8 = the pivot's rows 0/1/2 and translation,
// as the tail at 0x192a284 leaves them, and a jmp into the stub. X4's
// epilogue (add $0x58; pop rbx, rbp, r12, r13; ret) returns to our caller.
extern "C" void ext_enter(const void *stub, uintptr_t camera, float *out,
                          const float *pivot);
asm(".text\n"
    ".globl ext_enter\n"
    "ext_enter:\n"
    "  push %r13\n  push %r12\n  push %rbp\n  push %rbx\n"
    "  sub $0x58, %rsp\n"
    "  mov %rsi, %rbx\n  mov %rdx, %rbp\n"
    "  movaps 0x10(%rcx), %xmm6\n  movaps 0x20(%rcx), %xmm7\n"
    "  movaps 0x30(%rcx), %xmm5\n  movaps 0x00(%rcx), %xmm8\n"
    "  jmp *%rdi\n");

int main(int argc, char **argv) {
    // --emit <path>: the stub's bytes, placeholders for the injector's own
    // addresses, for objdump -D -b binary -mi386:x86-64.
    if (argc == 3 && !strcmp(argv[1], "--emit")) {
        extview::StubAddrs a{};
        a.root_slot = extview::kRootGlobal;
        a.player_cam = extview::kPlayerCamOff;
        a.mode = extview::kCamMode;
        a.own = extview::kCamOwn;
        a.rows_slot = 0x1111111111111111ull;
        a.applied = 0x2222222222222222ull;
        a.after_loads = extview::kAfterLoads;
        a.x4_compose = extview::kComposeVa;
        uint8_t code[extview::kStubMax];
        const size_t n = extview::make_stub(code, a);
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(code, 1, n, f) != n)
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
            check(extview::verify_sites(rd, &bad),
                  "the mode dispatch, the player test, modes 2-5 with their "
                  "tail and the compose match X4 9.00");
            for (const extview::Site *s : {&extview::kDispatch, &extview::kPlayerTest,
                                           &extview::kModes, &extview::kCompose}) {
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
                check(!extview::verify_sites(rd2, &bad) && bad == flip, what);
            }
            // The rel32 the patch replaces is X4's, landing on the compose.
            int32_t rel = 0;
            elf.read(extview::kTailRel32, &rel, 4);
            check(extview::kTailJmpEnd + (intptr_t)rel == extview::kComposeVa,
                  "control: X4's jmp at 0x192a295 lands on 0x192a15e");
            uint8_t op = 0;
            elf.read(extview::kTailJmp, &op, 1);
            check(op == 0xe9, "the patched instruction is a jmp rel32 (e9)");
            uintptr_t first = 0;
            size_t count = 0;
            foot::pages_of(extview::kTailRel32, 4, 4096, &first, &count);
            check(first == 0x192a000 && count == 1,
                  "the rel32 (0x192a296..0x192a299) lies in one page");
            // A stub just below X4's image is within reach; the disp
            // arithmetic lands on it.
            int32_t d = 0;
            const uintptr_t near_stub = 0x3e0000;
            check(csmfit::disp_for(extview::kTailJmp, 5, near_stub, &d) &&
                      extview::kTailJmpEnd + (intptr_t)d == near_stub,
                  "a stub below X4's image is in rel32 reach of the jmp");
        }
    }

    // 1. The stub, executed with X4's compose behind it.
    {
        uint8_t *page = (uint8_t *)mmap(nullptr, 4096,
                                        PROT_READ | PROT_WRITE | PROT_EXEC,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            check(false, "an executable page for the stub");
            return 1;
        }
        memcpy(page, extview::kComposeBytes, sizeof extview::kComposeBytes);
        static extview::Slot slot;
        static uint8_t root[0x400];
        static uintptr_t root_slot = 0;
        alignas(16) static uint8_t camera[0x900];
        alignas(16) static uint8_t other[0x900];
        const uintptr_t cam = (uintptr_t)camera;
        memcpy(root + extview::kPlayerCamOff, &cam, 8);

        extview::StubAddrs a{};
        a.root_slot = (uintptr_t)&root_slot;
        a.player_cam = extview::kPlayerCamOff;
        a.mode = extview::kCamMode;
        a.own = extview::kCamOwn;
        a.rows_slot = (uintptr_t)&slot.cur;
        a.applied = (uintptr_t)&slot.applied;
        a.after_loads = (uintptr_t)page + (extview::kAfterLoads - extview::kComposeVa);
        a.x4_compose = (uintptr_t)page;
        uint8_t code[extview::kStubMax];
        const size_t len = extview::make_stub(code, a);
        check(len == 269, "the stub is 269 bytes");
        memcpy(page + 1024, code, len);
        const void *stub = page + 1024;

        // X4's orbit (58 m back, turned) on a pivot somewhere in the sector.
        const Mat own = transform(50.f, -10.f, 3.f, 4.f, -58.f);
        const Mat pivot = transform(15.f, 5.f, 100.f, 200.f, 300.f);
        auto set_cam = [&](uint8_t *c, int32_t mode, const Mat &o) {
            memcpy(c + extview::kCamMode, &mode, 4);
            memcpy(c + extview::kCamOwn, o.m, sizeof o.m);
        };
        auto run = [&](uint8_t *c) {
            Mat out{};
            for (float &v : out.m)
                v = -777.f; // nothing left from a previous case
            ext_enter(stub, (uintptr_t)c, out.m, pivot.m);
            return out;
        };
        const float head_yaw = 30.f, head_pitch = -20.f;
        char what0[160];
        float rows[12] = {};
        camwrite::rows_for(head_yaw, head_pitch, rows);
        Mat head{};
        memcpy(head.m + 4, rows, sizeof rows);
        const Mat want_head = compose(compose(head, own), pivot);
        const Mat want_x4 = compose(own, pivot);
        snprintf(what0, sizeof what0,
                 "control: the head-turned and X4's products differ by %.2f "
                 "(the 1e-3 comparisons below can fail)",
                 max_diff(want_head, want_x4));
        check(max_diff(want_head, want_x4) > 0.1f, what0);

        root_slot = (uintptr_t)root;
        slot.publish(rows);
        uint64_t n0 = slot.applied.load();
        set_cam(camera, 2, own);
        Mat out = run(camera);
        char what[200];
        snprintf(what, sizeof what,
                 "mode 2 (F2/F3), the player's camera: (H x own) x pivot "
                 "(max diff %.2g), composed once",
                 max_diff(out, want_head));
        check(max_diff(out, want_head) < 1e-3f && slot.applied.load() == n0 + 1,
              what);
        set_cam(camera, 4, own);
        out = run(camera);
        check(max_diff(out, want_head) < 1e-3f && slot.applied.load() == n0 + 2,
              "mode 4 (F4/F6): the same");
        // The translation is the orbit's: the head turns about the eye.
        check(std::fabs(out.m[0] - want_x4.m[0]) < 1e-3f &&
                  std::fabs(out.m[1] - want_x4.m[1]) < 1e-3f &&
                  std::fabs(out.m[2] - want_x4.m[2]) < 1e-3f,
              "the camera stays where X4's orbit put it (only its rows turn)");

        n0 = slot.applied.load();
        set_cam(camera, 0, own);
        out = run(camera);
        check(max_diff(out, want_x4) < 1e-3f && slot.applied.load() == n0,
              "mode 0 (cockpit, walking through the foot stub): X4's own "
              "compose, nothing counted");
        set_cam(other, 2, own);
        out = run(other);
        check(max_diff(out, want_x4) < 1e-3f && slot.applied.load() == n0,
              "another camera in mode 2: X4's own compose");
        set_cam(camera, 2, own);
        slot.withdraw();
        out = run(camera);
        check(max_diff(out, want_x4) < 1e-3f && slot.applied.load() == n0,
              "rows withdrawn: X4's own compose");
        const Mat x4_out = out; // X4's own compose, executed
        slot.publish(rows);
        root_slot = 0;
        out = run(camera);
        check(max_diff(out, want_x4) < 1e-3f && slot.applied.load() == n0,
              "no root (load, teardown): X4's own compose");
        root_slot = (uintptr_t)root;

        // Head level and centred: bit for bit X4's picture (lanes 0-2; X4's
        // lane 3 is junk either way).
        float ident[12] = {};
        camwrite::rows_for(0.f, 0.f, ident);
        slot.publish(ident);
        out = run(camera);
        bool same = slot.applied.load() == n0 + 1;
        for (int r = 0; r < 4; r++)
            same = same && !memcmp(out.m + 4 * r, x4_out.m + 4 * r, 12);
        check(same, "head centred: composed, and bit-identical to X4's own "
                    "compose");

        // The angle composed is the angle declared: factor the orbit and the
        // pivot out and the camera's rows are H's, so its forward has the
        // head's yaw and pitch.
        const Mat unit = transform(0.f, 0.f, 0.f, 0.f, 0.f);
        slot.publish(rows);
        {
            alignas(16) Mat o = transform(0.f, 0.f, 0.f, 0.f, 0.f);
            Mat res{};
            set_cam(camera, 2, o);
            ext_enter(stub, cam, res.m, unit.m);
            float y = 0.f, p = 0.f;
            const bool ok = foot::head_angles_of_rows(res.m + 4, &y, &p);
            snprintf(what, sizeof what,
                     "orbit and pivot identity: the camera looks at yaw %.3f "
                     "pitch %.3f, the head's %.0f %.0f",
                     y, p, head_yaw, head_pitch);
            check(ok && std::fabs(y - head_yaw) < 1e-3f &&
                      std::fabs(p - head_pitch) < 1e-3f,
                  what);
        }
        {
            // With X4's orbit turned, the head is relative to the ORBIT:
            // out.R x own.R^T is H. Composing H after the orbit instead
            // (own x H) would not give this, the rotations do not commute.
            Mat res{};
            set_cam(camera, 2, own);
            ext_enter(stub, cam, res.m, unit.m);
            Mat rel{};
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) {
                    float v = 0.f;
                    for (int k = 0; k < 3; k++)
                        v += res.m[4 + 4 * r + k] * own.m[4 + 4 * c + k];
                    rel.m[4 + 4 * r + c] = v;
                }
            float y = 0.f, p = 0.f;
            const bool ok = foot::head_angles_of_rows(rel.m + 4, &y, &p);
            snprintf(what, sizeof what,
                     "orbit turned 50/-10: relative to the orbit the camera "
                     "looks at %.3f %.3f, the head's",
                     y, p);
            check(ok && std::fabs(y - head_yaw) < 1e-3f &&
                      std::fabs(p - head_pitch) < 1e-3f,
                  what);
            const Mat wrong = compose(own, head);
            check(max_diff(compose(wrong, unit), res) > 0.05f,
                  "control: the head composed AFTER the orbit would differ");
        }
        // X4's state is untouched: the camera's own transform as it was.
        check(!memcmp(camera + extview::kCamOwn, own.m, sizeof own.m),
              "the camera's own transform (X4's orbit) is never written");
        munmap(page, 4096);
    }

    // 2. The double buffer and the rows' layout.
    {
        extview::Slot s;
        float r1[12] = {}, r2[12] = {};
        camwrite::rows_for(10.f, 5.f, r1);
        camwrite::rows_for(-40.f, 20.f, r2);
        check(s.cur.load() == nullptr, "nothing published at start");
        s.publish(r1);
        extview::HeadRows *a = s.cur.load();
        s.publish(r2);
        extview::HeadRows *b = s.cur.load();
        check(a && b && a != b, "two publications fill the two buffers");
        bool laid = true;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int l = 0; l < 4; l++)
                    laid = laid && b->v[3 * i + j][l] == r2[4 * i + j];
        check(laid, "h[i][j] broadcast to the four lanes of vector 3i+j");
        s.publish(r1);
        check(s.cur.load() == a, "the third publication reuses the first buffer");
        s.withdraw();
        check(s.cur.load() == nullptr, "withdrawn: NULL, the stub composes nothing");
    }

    // 3. The period: declared only once the stub has composed.
    {
        using P = extview::Period;
        P p;
        check(p.step(false, 5) == P::kNone && !p.on, "no external view: nothing");
        check(p.step(true, 5) == P::kStarted && p.on && !p.declared,
              "an external view: started, not declared");
        check(p.step(true, 5) == P::kNone && !p.declared,
              "the stub has not composed yet: still not declared");
        check(p.step(true, 6) == P::kDeclared && p.declared,
              "the stub composed: declared");
        check(p.step(true, 9) == P::kNone && p.declared && p.composed == 4,
              "stays declared, counting");
        check(p.step(false, 9) == P::kEnded && !p.on && !p.declared &&
                  p.composed == 4,
              "left: ended, 4 compositions reported");
        check(p.step(true, 9) == P::kStarted && p.periods == 2 && !p.declared,
              "the next external view starts undeclared from the new count");
        check(p.step(true, 9) == P::kNone && !p.declared,
              "a camera the stub never reaches is never declared");
    }

    printf(g_fail ? "%d case(s) FAILED\n" : "all cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
