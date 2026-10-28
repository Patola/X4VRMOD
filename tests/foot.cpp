// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_FOOTLOG, checked without X4: the angles read back from X4's matrix
// convention (camwrite::rows_for, which take 319's matrices fixed), a rotation
// yaw/pitch cannot see (roll) still counted, freed memory refused, the window's
// net/travel across the +-180 seam, a camera change not read as one huge step,
// and the window line's shape that score_run.py parses. Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_camwrite.hpp"
#include "../injector/x4vr_foot.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}
static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// A full matrix (translation + 3 rows) in X4's convention.
static void mat(float yaw, float pitch, float x, float y, float z, float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = x;
    m[1] = y;
    m[2] = z;
    camwrite::rows_for(yaw, pitch, m + 4);
}

// --emit <scenario>: a census log made by the real accumulation and the real
// formatters, for tests/run-census-scorer.sh to plant -- so the scorer's
// regex is checked against what the injector prints, not a hand copy.
struct Sim {
    double t = 999000.0;
    foot::Sample s, prev;
    bool have_prev = false;
    foot::Window w;
    float ly = 0, lp = 0, by = 0, bp = 0, ry = 0, rp = 0, hy = 0, hp = 0;
    void line(const char *body) { printf("[%12.3f] inject  %s\n", t, body); }
    // One second of 20 samples: per-second increments of each angle.
    void second(double mdx, double mdy, float dhy, float dhp, float dly,
                float dlp, float dby, float dbp, float dry, float drp) {
        for (int i = 0; i < 20; i++) {
            t += 0.05;
            hy += dhy / 20; hp += dhp / 20;
            ly += dly / 20; lp += dlp / 20;
            by += dby / 20; bp += dbp / 20;
            ry += dry / 20; rp += drp / 20;
            s.local_ok = s.base_ok = s.rot_ok = s.head_ok = true;
            mat(ly, lp, 0.f, 0.f, 0.f, s.local);
            mat(by, bp, 2.f, 1.f, -3.f, s.base);
            s.rot_yaw = foot::wrap180(ry);
            s.rot_pitch = rp;
            s.head_yaw = hy;
            s.head_pitch = hp;
            if (!have_prev || !foot::same_regime(prev, s)) {
                char b[768];
                foot::format_regime(b, sizeof b, s);
                line(b);
            }
            prev = s;
            have_prev = true;
            w.add(t, s);
        }
        w.mouse_dx = mdx;
        w.mouse_dy = mdy;
        w.mouse_n = (mdx != 0.0 || mdy != 0.0) ? 30 : 0;
        char b[1024];
        foot::format_window(b, sizeof b, w, 1.0);
        line(b);
        w.restart(t);
    }
};

static int emit(const char *scenario) {
    Sim m;
    m.line("foot: census ON — read only; a transition line per camera regime, "
           "a window line per second; sampled BEFORE head-look's write");
    // Seated: the head turns and head-look's write carries the local matrix
    // and GetCameraRotation with it; the cockpit pointer moves nothing.
    m.s.cam = 0x7f0000001000;
    m.s.attached = 0x7f0000002000;
    m.s.attached_vt = 0x3b80000;
    m.s.mode = 0;
    m.s.piloting = m.s.occupied = true;
    m.s.hud = true;
    const float head[] = {8, -12, 6, -9, 14, -7};
    for (float d : head)
        m.second(0, 0, d, 0, d, 0, 0, 0, d, 0);
    const double cur[] = {200, -150, 90, -300};
    for (double d : cur)
        m.second(d, d / 2, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!strcmp(scenario, "seated"))
        return 0;
    // Off the seat: another mode, another attached object.
    m.s.mode = 1;
    m.s.piloting = false;
    m.s.attached = 0x7f0000003000;
    m.s.attached_vt = 0x3c00000;
    m.s.relative = true;
    if (!strcmp(scenario, "patched")) {
        // B: X4VR_HEADLOOK_FOOT. The mouse turns the body (GetCameraRotation's
        // yaw); its pitch never reaches X4 (zeroed); the head turns
        // GetCameraRotation and the written local matrix.
        const double bx[] = {300, -500, 150, -200, 400, -100, 250, -350};
        for (double d : bx) {
            const float yaw = (float)(-0.1 * d);
            m.w.mouse_pitch_zeroed = 12;
            m.second(d, 0, 0, 0, 0, 0, 0, 0, yaw, 0);
        }
        const float turn[] = {10, -14, 9, -11, 12, -8};
        for (float d : turn)
            m.second(0, 0, d, d / 3, d, d / 3, 0, 0, d, d / 3);
        return 0;
    }
    const bool moves = !strcmp(scenario, "follows");
    const bool dropped = !strcmp(scenario, "dropped");
    const double dx[] = {300, -500, 150, -200, 400, -100, 250, -350};
    for (double d : dx) {
        const float yaw = moves ? (float)(-0.1 * d) : 0.f;
        m.w.mouse_dropped = dropped ? 25 : 0;
        m.second(dropped ? 0 : d, 0, 0, 0, 0, 0, yaw, 0, yaw, 0);
    }
    const double dy[] = {100, -200, 150, -50, 80, -120};
    for (double d : dy) {
        const float pitch = moves ? (float)(-0.05 * d) : 0.f;
        m.second(0, d, 0, 0, 0, pitch, 0, 0, 0, pitch);
    }
    const float turn[] = {10, -14, 9, -11, 12, -8};
    for (float d : turn)
        m.second(0, 0, d, d / 3, 0, 0, 0, 0, 0, 0);
    return 0;
}

// The stub's two exits, as thunks that say which one was taken. Reached by a
// jmp from inside the stub, so their `ret` returns to run_stub's call.
extern "C" int foot_compose_thunk();
extern "C" int foot_ret_thunk();
asm(".text\n"
    ".globl foot_compose_thunk\nfoot_compose_thunk:\n  mov $1, %eax\n  ret\n"
    ".globl foot_ret_thunk\nfoot_ret_thunk:\n  mov $2, %eax\n  ret\n");

// Call the stub with rbx = `camera`, as X4's 0x1929f70 has it. Below the red
// zone, since the call pushes a return address.
static int run_stub(void *stub, uintptr_t camera) {
    int r;
    asm volatile("sub $128, %%rsp\n\tcall *%1\n\tadd $128, %%rsp"
                 : "=a"(r)
                 : "r"(stub), "b"(camera)
                 : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
                   "memory", "cc");
    return r;
}

// The ELF reader of tests/steer.cpp: X4 is non-PIE, a VA maps through the
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

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--emit"))
        return emit(argv[2]);

    // 0. X4VR_HEADLOOK_FOOT's sites, against the real binary.
    {
        static Elf elf;
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        if (!elf.load(x4)) {
            printf("SKIP   no X4 binary at %s — the patch sites are unchecked\n", x4);
        } else {
            auto rd = [](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
            uintptr_t bad = 0;
            check(foot::verify_skip_sites(rd, &bad),
                  "the walking branch, the local tail and the compose match X4 9.00");
            // One byte changed in each site: refused, and named.
            for (const foot::CodeBytes *c :
                 {&foot::kSkipBranch, &foot::kLocalTail, &foot::kLocalCompose}) {
                const uintptr_t flip = c->va + c->n / 2;
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
                check(!foot::verify_skip_sites(rd2, &bad) && bad == flip, what);
            }
            // The patch's arithmetic: the new rel32, decoded from the jne's end,
            // lands on the tail; the old one on the return.
            const int32_t want = foot::rel32_to(foot::kSkipJneEnd, foot::kLocalTailVa);
            check((uint32_t)want == 0xffffff8bu &&
                      foot::kSkipJneEnd + (intptr_t)want == foot::kLocalTailVa,
                  "the patched rel32 is 0xffffff8b and lands on 0x192a284");
            int32_t old_rel = 0;
            elf.read(foot::kSkipRel32, &old_rel, 4);
            check(foot::kSkipJneEnd + (intptr_t)old_rel == foot::kSkipTarget,
                  "control: X4's own rel32 lands on the return 0x192a216");
            // B's second patch: the controller-NULL exit and what the stub
            // relies on.
            bad = 0;
            check(foot::verify_null_exit_sites(rd, &bad),
                  "v2: the NULL-controller exit, the player-camera test, the "
                  "return, the tail and the compose match X4 9.00");
            for (const foot::CodeBytes *c :
                 {&foot::kFnHead, &foot::kNullExit, &foot::kPlayerCamCmp,
                  &foot::kReturn}) {
                const uintptr_t flip = c->va + c->n / 2;
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
                         "v2: one byte changed at %#lx -> refused there",
                         (unsigned long)flip);
                check(!foot::verify_null_exit_sites(rd2, &bad) && bad == flip, what);
            }
            // The pages the patch writes: take 337 refused because the rel32
            // crosses a page; both must be made writable.
            {
                uintptr_t first = 0;
                size_t count = 0;
                foot::pages_of(foot::kNullRel32, 4, 4096, &first, &count);
                check(first == 0x1929000 && count == 2,
                      "v2: the je's rel32 crosses 0x192a000 -> two pages");
                foot::pages_of(foot::kSkipRel32, 4, 4096, &first, &count);
                check(first == 0x192a000 && count == 1,
                      "control: a rel32 inside one page -> one page");
                foot::pages_of(0x1fff, 1, 4096, &first, &count);
                check(first == 0x1000 && count == 1,
                      "control: the last byte of a page -> that page alone");
            }
            int32_t null_rel = 0;
            elf.read(foot::kNullRel32, &null_rel, 4);
            check(foot::kNullJeEnd + (intptr_t)null_rel == foot::kReturn.va,
                  "control: X4's NULL-controller je lands on the return");
        }
    }
    // 0b. B's second patch: the stub's own bytes, EXECUTED.
    {
        void *page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            check(false, "an executable page for the stub");
        } else {
            static uint8_t fake_root[0x400];
            static uintptr_t root_slot = 0;
            const uintptr_t player_cam = 0x7f00dead1000;
            memcpy(fake_root + foot::kPlayerCamOff, &player_cam, 8);
            uint8_t stub[foot::kStubLen];
            foot::make_stub(stub, (uintptr_t)&root_slot,
                            (uintptr_t)&foot_compose_thunk,
                            (uintptr_t)&foot_ret_thunk,
                            (uint32_t)foot::kPlayerCamOff);
            memcpy(page, stub, sizeof stub);
            root_slot = (uintptr_t)fake_root;
            check(run_stub(page, player_cam) == 1,
                  "stub: the player's camera -> compose its own transform");
            check(run_stub(page, player_cam + 0x80) == 2,
                  "stub: any other camera -> X4's own return");
            root_slot = 0;
            check(run_stub(page, player_cam) == 2,
                  "stub: no root (teardown) -> X4's own return");
            munmap(page, 4096);
        }
    }

    // 0c. patch_code across a page boundary in a read+execute mapping -- the
    // shape of take 337's refusal (0x1929fff..0x192a002).
    {
        const long pg = sysconf(_SC_PAGESIZE);
        uint8_t *two = (uint8_t *)mmap(nullptr, 2 * (size_t)pg,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (two == MAP_FAILED) {
            check(false, "two pages for patch_code");
        } else {
            memset(two, 0xcc, 2 * (size_t)pg);
            mprotect(two, 2 * (size_t)pg, PROT_READ | PROT_EXEC);
            const int32_t v = (int32_t)0x8bffffff;
            char err[200] = "";
            const bool ok = foot::patch_code((uintptr_t)two + (uintptr_t)pg - 1,
                                             &v, 4, err, sizeof err);
            int32_t got = 0;
            memcpy(&got, two + pg - 1, 4);
            check(ok && got == v && two[pg - 2] == 0xcc && two[pg + 3] == 0xcc,
                  "patch_code writes 4 bytes across a page boundary of a "
                  "read+execute mapping, and nothing around them");
            // Given back read+execute: a write now would fault, so ask the
            // kernel -- /proc/self/maps -- instead of trying.
            bool rx = false;
            if (FILE *f = fopen("/proc/self/maps", "r")) {
                char ln[512];
                while (fgets(ln, sizeof ln, f)) {
                    unsigned long lo = 0, hi = 0;
                    char perm[8] = "";
                    if (sscanf(ln, "%lx-%lx %7s", &lo, &hi, perm) == 3 &&
                        lo <= (unsigned long)two &&
                        (unsigned long)two + 2 * (unsigned long)pg <= hi)
                        rx = !strcmp(perm, "r-xp");
                }
                fclose(f);
            }
            check(rx, "both pages left read+execute (/proc/self/maps: r-xp)");
            munmap(two, 2 * (size_t)pg);
        }
    }

    // 1. The inverse of rows_for, over the range head-look writes.
    {
        float worst = 0.f;
        bool all = true;
        for (float y = -179.f; y <= 179.f; y += 7.f)
            for (float p = -75.f; p <= 75.f; p += 5.f) {
                float r[12];
                camwrite::rows_for(y, p, r);
                float yy = 0.f, pp = 0.f;
                all = all && foot::head_angles_of_rows(r, &yy, &pp);
                worst = std::fmax(worst, std::fabs(foot::wrap180(yy - y)));
                worst = std::fmax(worst, std::fabs(pp - p));
            }
        char what[128];
        snprintf(what, sizeof what,
                 "angles read back from rows_for over yaw +-179, pitch +-75 "
                 "(worst %.5f deg)", worst);
        check(all && worst < 1e-3f, what);
    }
    // 2. Not a unit forward axis: refused, not read as an angle.
    {
        float r[12] = {};
        float y = 0.f, p = 0.f;
        check(!foot::head_angles_of_rows(r, &y, &p), "all-zero rows (freed) refused");
        camwrite::rows_for(20.f, 10.f, r);
        r[8] *= 1.5f;
        check(!foot::head_angles_of_rows(r, &y, &p), "a stretched forward axis refused");
    }
    // 3. delta_angle_deg: a yaw step, and a ROLL that yaw/pitch cannot see.
    {
        float a[12], b[12];
        camwrite::rows_for(10.f, 0.f, a);
        camwrite::rows_for(25.f, 0.f, b);
        check(near(foot::delta_angle_deg(a, b), 15.f, 1e-2f), "a 15 deg yaw step reads 15");
        check(near(foot::delta_angle_deg(a, a), 0.f, 0.05f), "no motion reads ~0");
        // Roll by 30 deg about the forward (row 2) axis: rows 0 and 1 turn,
        // row 2 stays -- so yaw/pitch read no motion at all.
        float c[12];
        memcpy(c, a, sizeof c);
        const float cr = std::cos(30.f * 0.0174533f), sr = std::sin(30.f * 0.0174533f);
        for (int k = 0; k < 3; k++) {
            c[0 + k] = cr * a[0 + k] + sr * a[4 + k];
            c[4 + k] = -sr * a[0 + k] + cr * a[4 + k];
        }
        float y0, p0, y1, p1;
        foot::head_angles_of_rows(a, &y0, &p0);
        foot::head_angles_of_rows(c, &y1, &p1);
        check(near(y0, y1, 1e-3f) && near(p0, p1, 1e-3f),
              "control: a pure roll leaves yaw/pitch unchanged");
        check(near(foot::delta_angle_deg(a, c), 30.f, 0.05f),
              "and the geodesic angle still reads it (30 deg)");
    }
    // 4. A window: local yaw ramps 170 -> -170 through the seam, base static,
    //    the head turns left 12 deg. Net local yaw +20, not -340.
    {
        foot::Window w;
        double t = 100.0;
        for (int i = 0; i <= 20; i++, t += 0.05) {
            foot::Sample s;
            s.cam = 0x1000;
            s.mode = 1;
            s.local_ok = s.base_ok = s.head_ok = s.rot_ok = true;
            mat(foot::wrap180(170.f + (float)i), 5.f, 0.f, 1.7f, 0.f, s.local);
            mat(0.f, 0.f, 3.f, 0.f, -2.f, s.base);
            s.head_yaw = 0.6f * (float)i;
            s.head_pitch = 0.f;
            s.rot_yaw = foot::wrap180(170.f + (float)i);
            s.rot_pitch = 5.f;
            w.add(t, s);
        }
        check(near(w.local.ang.net_yaw, 20.f, 1e-2f),
              "local yaw net +20 across the +-180 seam (not -340)");
        check(near(w.local.ang.travel_yaw, 20.f, 1e-2f), "local yaw travel 20");
        check(w.local.moved == 20, "local moved in all 20 steps");
        check(w.base.moved == 0 && near(w.base.rot_travel, 0.f, 0.05f) &&
                  near(w.base.pos_max, 0.f, 1e-6f),
              "control: the static base reads no rotation and no displacement");
        check(near(w.head.net_yaw, 12.f, 1e-3f), "head net yaw 12");
        check(near(w.rot.net_yaw, 20.f, 1e-2f), "GetCameraRotation net yaw 20");

        // The line, and what score_run.py's regex keys on.
        w.mouse_n = 42;
        w.mouse_dx = -350.0;
        w.mouse_dy = 12.0;
        char line[1024];
        foot::format_window(line, sizeof line, w, 1.0);
        printf("       %s\n", line);
        check(strstr(line, "foot: window 1.00 s n 21 mode 1 piloting 0 occupied 0 "
                           "view cockpit hud 0 rel 0 | mouse n 42 dx -350 dy 12 dropped 0 pitchzero 0 |") != nullptr,
              "window line head (mode, flags, mouse)");
        check(strstr(line, "| local net 20.00 0.00 travel 20.00 0.00 at -170.0 5.0 rot ") != nullptr,
              "window line's local column");
        check(strstr(line, "moved 0 pos 0.000") != nullptr, "window line's base column");

        // A restart keeps the rows: the next window's first step is counted.
        w.restart(t);
        foot::Sample s;
        s.cam = 0x1000;
        s.mode = 1;
        s.local_ok = true;
        mat(-169.f, 5.f, 0.f, 1.7f, 0.f, s.local);
        w.add(t + 0.05, s);
        check(w.local.moved == 1 && near(w.local.ang.net_yaw, 1.f, 1e-2f),
              "after a restart the first step from the last window is counted");
    }
    // 5. A camera change: the matrices are forgotten, not differenced.
    {
        foot::Window w;
        foot::Sample s;
        s.cam = 0x1000;
        s.local_ok = true;
        mat(0.f, 0.f, 0.f, 0.f, 0.f, s.local);
        w.add(1.0, s);
        w.forget_matrices();
        s.cam = 0x2000;
        mat(90.f, 0.f, 5.f, 0.f, 0.f, s.local);
        w.add(1.05, s);
        check(w.local.moved == 0 && near(w.local.ang.net_yaw, 0.f, 1e-6f) &&
                  near(w.local.pos_max, 0.f, 1e-6f),
              "a new camera is not read as a 90 deg step and a 5 m jump");
    }
    // 6. Regimes.
    {
        foot::Sample a, b;
        a.cam = b.cam = 0x1000;
        a.mode = b.mode = 0;
        a.piloting = b.piloting = true;
        b.rot_yaw = 40.f;
        b.view = 1;
        check(foot::same_regime(a, b), "a turned camera or another view is the same regime");
        b.piloting = false;
        check(!foot::same_regime(a, b), "leaving the seat is a new regime");
        b = a;
        b.mode = 1;
        check(!foot::same_regime(a, b), "a mode change is a new regime");
    }
    // 7. On-foot menus (takes 338, 340): when X4 freeing the pointer counts.
    {
        using M = foot::FootMenu;
        // step(off_seat, foot, rel, t, ab)
        // Stand-up: on foot with the pointer already free, then X4 takes it.
        M m;
        bool any = false;
        for (int i = 0; i < 20; i++)
            any |= m.step(true, true, false, 1.0 + 0.01 * i, true) != M::kNone;
        any |= m.step(true, true, true, 1.3, true) != M::kNone;
        check(!any && m.openings == 0,
              "menus: standing up with the pointer free is no opening");

        // A real opening: ON -> off, held 50 ms.
        m.step(true, true, true, 2.000, true);
        check(m.step(true, true, false, 2.010, true) == M::kNone,
              "menus: the edge alone does not open");
        check(m.step(true, true, false, 2.040, true) == M::kNone && !m.open,
              "menus: 30 ms after the edge, still not open");
        check(m.step(true, true, false, 2.061, true) == M::kOpened && m.openings == 1 &&
                  m.canvas && m.draw_canvas() && !m.resumed,
              "menus: 51 ms after the edge it opens, opening 1 = arm A (canvas)");
        check(m.step(true, true, false, 3.0, true) == M::kNone && m.open,
              "menus: stays open while the pointer is free");
        check(m.step(true, true, true, 4.0, true) == M::kClosed && !m.draw_canvas(),
              "menus: X4 taking the pointer back closes it");

        // Take 338's HUD-down: pointer freed, HUD down 6 ms later, and X4
        // takes the pointer back before the HUD comes up again.
        m.step(true, true, true, 5.000, true);
        m.step(true, true, false, 5.010, true);
        m.step(true, false, false, 5.016, true);
        bool opened = false;
        for (int i = 0; i < 100; i++)
            opened |= m.step(true, false, false, 5.02 + 0.01 * i, true) == M::kOpened;
        opened |= m.step(true, false, true, 6.05, true) == M::kOpened;
        for (int i = 0; i < 20; i++)
            opened |= m.step(true, true, true, 6.1 + 0.01 * i, true) == M::kOpened;
        check(!opened && m.openings == 1,
              "menus: a pointer freed 6 ms before the HUD goes down is no opening");

        // The A/B alternates; without it every opening is canvas.
        m.step(true, true, true, 7.0, true);
        m.step(true, true, false, 7.01, true);
        m.step(true, true, false, 7.07, true);
        check(m.openings == 2 && m.open && !m.canvas && !m.draw_canvas(),
              "menus: opening 2 = arm B (raw)");
        m.step(true, true, true, 8.0, true);
        m.step(true, true, false, 8.01, true);
        m.step(true, true, false, 8.07, true);
        check(m.openings == 3 && m.canvas, "menus: opening 3 = arm A again");

        // Take 340's vendor: the dialog, then the purchase list (full-screen,
        // HUD down), then Cancel back to the dialog with the pointer free.
        M v;
        v.step(true, true, true, 10.0, false);
        v.step(true, true, false, 10.01, false);
        check(v.step(true, true, false, 10.07, false) == M::kOpened && !v.resumed,
              "menus: the vendor's dialog opens");
        check(v.step(true, false, false, 11.0, false) == M::kClosed,
              "menus: the purchase list (HUD down) closes it");
        v.step(true, false, false, 20.0, false);
        check(v.step(true, true, false, 20.01, false) == M::kNone,
              "menus: back from the list, not before the hold");
        check(v.step(true, true, false, 20.07, false) == M::kOpened &&
                  v.resumed && v.draw_canvas() && v.openings == 2,
              "menus: back at the dialog with the pointer free it opens again (resumed)");

        // Take 339's map on foot: opened while walking (no menu open), and
        // the HUD came up 0.9 s before X4 took the pointer back.
        M w;
        w.step(true, true, true, 50.0, false);
        w.step(true, false, false, 51.0, false);     // the map: HUD down
        bool map_opened = false;
        for (int i = 0; i < 90; i++)
            map_opened |= w.step(true, true, false, 60.0 + 0.01 * i, false) == M::kOpened;
        map_opened |= w.step(true, true, true, 60.9, false) == M::kOpened;
        check(!map_opened && w.openings == 0,
              "menus: the HUD back up after the map, pointer free 0.9 s, is no opening");

        // Sitting down forgets an interrupted menu, and standing up is still
        // no opening.
        v.step(false, false, false, 30.0, false);
        bool again = false;
        for (int i = 0; i < 20; i++)
            again |= v.step(true, true, false, 31.0 + 0.01 * i, false) == M::kOpened;
        check(!again && !v.open,
              "menus: after sitting down, standing up again is no opening");
        check(v.step(true, true, true, 32.0, false) == M::kNone &&
                  v.step(true, true, false, 32.01, false) == M::kNone &&
                  v.step(true, true, false, 32.07, false) == M::kOpened,
              "menus: and the first menu after it opens as before");

        M n;
        for (int k = 0; k < 2; k++) {
            n.step(true, true, true, 40.0 + k, false);
            n.step(true, true, false, 40.01 + k, false);
            n.step(true, true, false, 40.07 + k, false);
        }
        check(n.openings == 2 && n.draw_canvas(),
              "menus: without the A/B the second opening is canvas too");
        check(n.step(false, false, false, 43.0, false) == M::kClosed,
              "menus: leaving the foot closes it");
    }

    printf("\n%s\n", g_fail ? "SOME CASES FAILED" : "all cases passed");
    return g_fail ? 1 : 0;
}
