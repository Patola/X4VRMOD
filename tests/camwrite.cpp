// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_HEADLOOK_WRITE's arithmetic and its refusals, checked without X4.
//
// The matrix convention is checked against X4's OWN stored matrices: take
// 319's thirteen raw samples of camera+0x5b0. Each row set is reduced to the
// two angles it encodes and rebuilt with rows_for(); all nine entries must
// come back. Two parameters reproducing nine numbers, thirteen times over a
// full turn, is the test -- a convention fitted by eye to one sample would
// fail it. The code sites are read from the real binary when present.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include "../injector/x4vr_camwrite.hpp"

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

// Take 319's `camwatch hop3 sample [camera.rot]` lines: GetCameraRotation's
// yaw (r.yaw, degrees, sway included) and X4's stored rows, as logged.
struct Sample {
    float cam_yaw;
    float r[9];
};
static const Sample kTake319[] = {
    {2.06f, {0.9996f, 0.0000f, -0.0300f, -0.0158f, 0.8504f, -0.5259f, 0.0255f, 0.5261f, 0.8500f}},
    {-76.20f, {0.2268f, 0.0000f, 0.9739f, 0.4332f, 0.8956f, -0.1009f, -0.8723f, 0.4448f, 0.2031f}},
    {-29.33f, {0.8766f, 0.0000f, 0.4812f, 0.0819f, 0.9854f, -0.1492f, -0.4742f, 0.1702f, 0.8638f}},
    {172.67f, {-0.9882f, 0.0000f, -0.1534f, -0.0228f, 0.9889f, 0.1467f, 0.1517f, 0.1484f, -0.9772f}},
    {35.16f, {0.7690f, 0.0000f, -0.6393f, -0.0224f, 0.9994f, -0.0269f, 0.6389f, 0.0350f, 0.7685f}},
    {30.11f, {0.8345f, 0.0000f, -0.5510f, 0.0578f, 0.9945f, 0.0875f, 0.5480f, -0.1048f, 0.8299f}},
    {-33.83f, {0.8125f, 0.0000f, 0.5830f, -0.0087f, 0.9999f, 0.0122f, -0.5829f, -0.0150f, 0.8124f}},
    {37.10f, {0.7939f, 0.0000f, -0.6080f, -0.1250f, 0.9787f, -0.1632f, 0.5951f, 0.2055f, 0.7770f}},
    {-48.61f, {0.6790f, 0.0000f, 0.7341f, 0.0418f, 0.9984f, -0.0387f, -0.7330f, 0.0570f, 0.6779f}},
    {3.99f, {0.9982f, 0.0000f, -0.0596f, 0.0365f, 0.7906f, 0.6112f, 0.0471f, -0.6123f, 0.7892f}},
    {27.94f, {0.8464f, 0.0000f, -0.5325f, -0.1343f, 0.9677f, -0.2135f, 0.5153f, 0.2522f, 0.8190f}},
    {30.11f, {0.8464f, 0.0000f, -0.5325f, -0.1042f, 0.9807f, -0.1657f, 0.5222f, 0.1957f, 0.8300f}},
    {29.41f, {0.8908f, 0.0000f, -0.4543f, -0.0889f, 0.9807f, -0.1744f, 0.4455f, 0.1957f, 0.8736f}},
};

int main() {
    // ---- the convention against X4's own matrices ------------------------
    {
        constexpr float r2d = 57.2957795130823f;
        float worst = 0.f, worst_sign = 0.f;
        for (const Sample &s : kTake319) {
            // row0 = (cos psi, 0, -sin psi); row1.y = cos th; row2.y = sin th
            const float psi = std::atan2(-s.r[2], s.r[0]);
            const float th = std::atan2(s.r[7], s.r[4]);
            float rows[12] = {};
            rows[3] = rows[7] = rows[11] = 42.f; // w must survive
            camwrite::rows_for(-psi * r2d, th * r2d, rows);
            const int map[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
            for (int k = 0; k < 9; k++)
                worst = std::fmax(worst, std::fabs(rows[map[k]] - s.r[k]));
            if (rows[3] != 42.f || rows[7] != 42.f || rows[11] != 42.f)
                worst = 99.f;
            // The yaw SIGN against GetCameraRotation's: the stored matrix and
            // the camera differ by sway (a few degrees), never by a mirror.
            worst_sign = std::fmax(
                worst_sign, std::fabs(camwrite::wrap180(psi * r2d - s.cam_yaw)));
        }
        printf("      worst entry error %.5f, worst |psi - camera yaw| %.2f deg\n",
               worst, worst_sign);
        check(worst < 2e-3f,
              "rows_for rebuilds all 9 entries of all 13 of take 319's stored "
              "matrices from their 2 angles (w kept)");
        check(worst_sign < 6.f,
              "the stored yaw has GetCameraRotation's sign (within sway), so "
              "head yaw -> psi = -yaw is right");
        // And what the write produces is a rotation.
        float rows[12] = {};
        camwrite::rows_for(123.f, -40.f, rows);
        check(camwatch::live_transform(rows, false),
              "a written matrix is orthonormal with det 1");
        check(std::fabs(camwatch::rotation_angle_deg(rows) -
                        camwatch::yaw_pitch_angle_deg(-123.f / r2d, -40.f / r2d)) <
                  0.01f,
              "its angle is the camera angle the census would expect");
    }

    // ---- X4's loaded parameters -----------------------------------------
    {
        const float deg[4] = {75.f, 180.f, 0.f, 500.f};
        const float rad[4] = {75.f * 0.01745329f, 3.14159265f, 0.f, 8.72665f};
        const float spring[4] = {75.f, 180.f, 200.f, 500.f};
        const float stock[4] = {35.f, 65.f, 0.f, 500.f};
        const float shifted[4] = {180.f, 0.f, 500.f, 85.f}; // read 4 bytes late
        auto c = camwrite::check_params(deg, 75.f, 180.f);
        check(c.ok && !c.radians, "params in degrees, our limits, speed 0 -> ok");
        c = camwrite::check_params(rad, 75.f, 180.f);
        check(c.ok && c.radians, "params in radians -> ok, and says radians");
        check(!camwrite::check_params(spring, 75.f, 180.f).ok,
              "anglespeed 200 (x4vrmod_headlook not in force) -> REFUSED");
        check(!camwrite::check_params(stock, 75.f, 180.f).ok,
              "stock 35/65 limits (x4vrmod_lookaround not in force) -> REFUSED");
        check(!camwrite::check_params(shifted, 75.f, 180.f).ok,
              "a field layout off by one float -> REFUSED");
    }

    // ---- the base transform ---------------------------------------------
    {
        float b[16] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        check(camwrite::base_is_identity(b, 1e-3f), "identity base -> ok");
        b[1] = 0.05f;
        check(!camwrite::base_is_identity(b, 1e-3f),
              "a translated base -> REFUSED");
        b[1] = 0.f;
        b[5] = 0.9f;
        check(!camwrite::base_is_identity(b, 1e-3f), "a rotated base -> REFUSED");
    }

    // ---- the target ------------------------------------------------------
    {
        float y, p;
        camwrite::write_target(0.f, 40.f, 10.f, 1.f, 180.f, 75.f, &y, &p);
        check(std::fabs(y - 40.f) < 1e-4f && std::fabs(p - 10.f) < 1e-4f,
              "level head: the written angle IS the head's");
        camwrite::write_target(0.f, 40.f, 89.f, 0.f, 180.f, 75.f, &y, &p);
        check(std::fabs(y) < 1e-4f && std::fabs(p - 75.f) < 1e-4f,
              "straight up: yaw held (weight 0), pitch clamped to 75");
        camwrite::write_target(170.f, -170.f, 0.f, 1.f, 180.f, 75.f, &y, &p);
        check(std::fabs(y + 170.f) < 1e-4f,
              "across the back: -170, reached the short way");
        camwrite::write_target(170.f, -170.f, 0.f, 0.5f, 180.f, 75.f, &y, &p);
        check(std::fabs(std::fabs(y) - 180.f) < 1e-3f,
              "half weight across the back lands at 180, not at 0");
        camwrite::write_target(0.f, 100.f, 0.f, 1.f, 65.f, 35.f, &y, &p);
        check(std::fabs(y - 65.f) < 1e-4f, "a 65 limit clamps instead of wrapping");
    }

    // ---- the write mechanism: process_vm_writev on ourselves ---------------
    //
    // Every probe before this READ X4 through process_vm_readv. The write uses
    // the other half, on this kernel's settings (yama ptrace_scope 1): it must
    // land, and an unmapped address -- a camera X4 freed -- must come back as
    // an error, never a fault.
    {
        alignas(16) static float target[12] = {};
        float rows[12];
        for (int i = 0; i < 12; i++)
            rows[i] = 0.25f * (float)i;
        struct iovec l {rows, sizeof rows};
        struct iovec r {target, sizeof target};
        const ssize_t n = process_vm_writev(getpid(), &l, 1, &r, 1, 0);
        check(n == (ssize_t)sizeof rows && memcmp(rows, target, sizeof rows) == 0,
              "process_vm_writev on our own process lands the 48 bytes");
        void *gone = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        munmap(gone, 4096);
        struct iovec r2 {gone, sizeof rows};
        const ssize_t m = process_vm_writev(getpid(), &l, 1, &r2, 1, 0);
        check(m < 0, "a freed address is an ERROR (EFAULT), not a crash");
    }

    // ---- the code sites, against the real binary --------------------------
    {
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        Elf elf;
        if (!elf.load(x4)) {
            printf("skip  write sites — no X4 at %s\n", x4);
        } else {
            auto rd = [&](uintptr_t va, void *o, size_t n) {
                return elf.read(va, o, n);
            };
            uintptr_t bad = 0;
            check(camwrite::verify_write_sites(rd, &bad),
                  "every write site matches X4 9.00 byte for byte");
            check(camwatch::verify_hop3_sites(rd, &bad),
                  "and hop 3's camera layout still does");
            check(camwrite::params_global() == 0x3db83c8,
                  "the parameters global decodes to 0x3db83c8");
            auto bent = [&](uintptr_t va, void *o, size_t n) {
                if (!elf.read(va, o, n))
                    return false;
                if (va == 0x1935f1e)
                    ((uint8_t *)o)[6] ^= 0x04; // anglespeed +0x690 -> +0x694
                return true;
            };
            bad = 0;
            check(!camwrite::verify_write_sites(bent, &bad) && bad == 0x1935f1e,
                  "one changed byte refuses, and names the site");
        }
    }

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
