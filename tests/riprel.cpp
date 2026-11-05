// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The IsHUDActive teardown guard's decoder, checked without X4 running.
//
// The guard is only as good as the address it reads, and a decoder that
// accepts the wrong instruction reads a wild pointer inside X4 on every tick.
// So the refusals are planted as carefully as the accepts, and -- when the game
// is installed -- the real IsHUDActive prologue is decoded from the binary on
// disk and must land on the global gdb showed in the take-313-era cores
// (0x3db6e48). Self-checking: no runner script.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../injector/x4vr_riprel.hpp"
#include "x4_path.hpp"

static int g_fail = 0;

static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// Lay out `mov disp32(%rip),%rdx` at buf[at], with its target at buf[slot].
static void emit(uint8_t *buf, int at, int slot, uint8_t rex = 0x48,
                 uint8_t op = 0x8b, uint8_t modrm = 0x15) {
    buf[at] = rex;
    buf[at + 1] = op;
    buf[at + 2] = modrm;
    const int32_t disp = slot - (at + 7);
    memcpy(buf + at + 3, &disp, 4);
}

static std::string run(const std::string &cmd) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p)
        return out;
    char b[512];
    while (fgets(b, sizeof b, p))
        out += b;
    pclose(p);
    return out;
}

int main() {
    alignas(8) uint8_t buf[256] = {};

    emit(buf, 64, 200);
    check(x4vr::riprel_rdx_load_slot(buf + 64) ==
              reinterpret_cast<const void *const *>(buf + 200),
          "forward displacement lands on the slot");

    emit(buf, 128, 16);
    check(x4vr::riprel_rdx_load_slot(buf + 128) ==
              reinterpret_cast<const void *const *>(buf + 16),
          "negative displacement lands on the slot");

    // A NULL in the slot is what the guard exists to see.
    const void *null_ptr = nullptr;
    memcpy(buf + 200, &null_ptr, sizeof null_ptr);
    emit(buf, 64, 200);
    check(*x4vr::riprel_rdx_load_slot(buf + 64) == nullptr,
          "reads the NULL the slot holds");

    // Refusals: each byte of the form is load-bearing.
    emit(buf, 64, 200, 0x49); // REX.WB: loads into r10, not rdx
    check(!x4vr::riprel_rdx_load_slot(buf + 64), "refuses a different REX");
    emit(buf, 64, 200, 0x48, 0x8d); // lea, not mov: an address, not a load
    check(!x4vr::riprel_rdx_load_slot(buf + 64), "refuses lea");
    emit(buf, 64, 200, 0x48, 0x8b, 0x05); // mov ...(%rip),%rax
    check(!x4vr::riprel_rdx_load_slot(buf + 64), "refuses a different register");
    emit(buf, 64, 200, 0x48, 0x8b, 0x55); // mov disp8(%rbp),%rdx: not rip
    check(!x4vr::riprel_rdx_load_slot(buf + 64), "refuses a non-RIP base");
    check(!x4vr::riprel_rdx_load_slot(nullptr), "refuses a null function");

    // The general form, for GetCameraRotation's `mov disp32(%rip),%rax`.
    emit(buf, 64, 200, 0x48, 0x8b, 0x05);
    check(x4vr::riprel_mov_load_slot(buf + 64, 0) ==
              reinterpret_cast<const void *const *>(buf + 200),
          "rax form decodes when asked for rax");
    check(!x4vr::riprel_mov_load_slot(buf + 64, 2),
          "rax form is refused when asked for rdx");
    check(!x4vr::riprel_mov_load_slot(buf + 64, 8), "refuses reg 8 (needs REX.R)");

    // The real prologue, when the game and binutils are both here.
    const char *x4 = x4_test_binary();
    FILE *f = fopen(x4, "rb");
    if (!f) {
        printf("skip  real IsHUDActive prologue — no X4 at %s\n", x4);
    } else {
        fclose(f);
        const std::string nm =
            run(std::string("nm -D --defined-only '") + x4 +
                "' 2>/dev/null | awk '$3==\"IsHUDActive\"{print $1}'");
        const unsigned long addr = strtoul(nm.c_str(), nullptr, 16);
        if (!addr) {
            printf("skip  real IsHUDActive prologue — nm could not find it\n");
        } else {
            char cmd[512];
            snprintf(cmd, sizeof cmd,
                     "objdump -d --start-address=0x%lx --stop-address=0x%lx "
                     "'%s' 2>/dev/null | grep -E '^ +[0-9a-f]+:' | head -1",
                     addr, addr + 7, x4);
            const std::string line = run(cmd);
            // "  a431d0:\t48 8b 15 71 3c 37 03 \tmov ...": pull the seven bytes.
            uint8_t code[7] = {};
            int got = 0;
            const char *p = strchr(line.c_str(), ':');
            while (p && got < 7) {
                while (*p == ':' || *p == ' ' || *p == '\t')
                    p++;
                char *end = nullptr;
                const unsigned long v = strtoul(p, &end, 16);
                if (end != p + 2)
                    break;
                code[got++] = (uint8_t)v;
                p = end;
            }
            if (got != 7) {
                printf("skip  real IsHUDActive prologue — objdump gave %d byte(s)\n",
                       got);
            } else {
                // Decode against a copy placed at a known host address, then
                // translate the host offset back into X4's address space.
                const void *const *s = x4vr::riprel_rdx_load_slot(code);
                check(s != nullptr,
                      "real IsHUDActive starts with mov disp32(%rip),%rdx");
                if (s) {
                    const unsigned long target =
                        addr + (unsigned long)((const uint8_t *)s - code);
                    printf("      decoded global at 0x%lx\n", target);
                    check(target == 0x3db6e48,
                          "real prologue decodes to the global the cores NULLed");
                }
            }
        }
    }

    // GetCameraRotation+0xd: the load of the object the camera hangs off,
    // which the camera-state scan starts from. gdb and objdump both put its
    // global at 0x3db6948.
    if (FILE *g = fopen(x4, "rb")) {
        fclose(g);
        const std::string nm2 =
            run(std::string("nm -D --defined-only '") + x4 +
                "' 2>/dev/null | awk '$3==\"GetCameraRotation\"{print $1}'");
        const unsigned long a2 = strtoul(nm2.c_str(), nullptr, 16);
        if (a2) {
            char cmd[512];
            snprintf(cmd, sizeof cmd,
                     "objdump -d --start-address=0x%lx --stop-address=0x%lx "
                     "'%s' 2>/dev/null | grep -E '^ +[0-9a-f]+:' | head -1",
                     a2 + 0xd, a2 + 0xd + 7, x4);
            const std::string line = run(cmd);
            uint8_t code[7] = {};
            int got = 0;
            const char *p = strchr(line.c_str(), ':');
            while (p && got < 7) {
                while (*p == ':' || *p == ' ' || *p == '\t')
                    p++;
                char *end = nullptr;
                const unsigned long v = strtoul(p, &end, 16);
                if (end != p + 2)
                    break;
                code[got++] = (uint8_t)v;
                p = end;
            }
            if (got == 7) {
                const void *const *s = x4vr::riprel_mov_load_slot(code, 0);
                check(s != nullptr,
                      "real GetCameraRotation+0xd is mov disp32(%rip),%rax");
                if (s) {
                    const unsigned long target =
                        a2 + 0xd + (unsigned long)((const uint8_t *)s - code);
                    printf("      decoded global at 0x%lx\n", target);
                    check(target == 0x3db6948,
                          "GetCameraRotation's root global is 0x3db6948");
                }
            } else {
                printf("skip  GetCameraRotation+0xd — objdump gave %d byte(s)\n",
                       got);
            }
        }
    }

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
