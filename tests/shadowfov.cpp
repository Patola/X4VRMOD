// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_SHADOWFOV_AB's decisions and its refusals, checked without X4: the
// field sites and X4's default-field constant against the real binary, the
// live-layout check, what each phase writes, and that the effective field can
// never compound past the rendered one. Self-checking.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../injector/x4vr_shadowfov.hpp"

using namespace x4vr::shadowfov;

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


int main() {
    const float opt = 1.4917f, want = rendered(opt);
    check(std::fabs(want * 57.29578f - 110.0f) < 0.01f,
          "the rendered field at option 1.4917 is 110.00 deg");

    // ---- the live layout check ----------------------------------------
    check(layout_ok({kDefault, want}, opt), "X4 as it boots: default base, option x default effective");
    check(layout_ok({want, want}, opt), "after a FIX write: widened base");
    check(layout_ok({want, opt * want}, opt),
          "X4's setter compounding from a widened base is recognised, not refused");
    check(!layout_ok({0.5f, want}, opt), "a base that is neither -> REFUSED");
    check(!layout_ok({kDefault, 1.0f}, opt), "an effective that is not option x default -> REFUSED");
    check(!layout_ok({kDefault, rendered(1.24f)}, opt),
          "an effective for another option (X4VR_FOV not what X4 got) -> REFUSED");

    // ---- what each phase writes -----------------------------------------
    Action a = decide({kDefault, want}, opt, true);
    check(a.write_base && std::fabs(a.base - want) < 1e-6f && !a.write_effective,
          "FIX on a fresh camera: widen the base, leave the effective");
    a = decide({want, want}, opt, true);
    check(!a.write_base && !a.write_effective, "FIX already applied: nothing to write");
    a = decide({want, want}, opt, false);
    check(a.write_base && std::fabs(a.base - kDefault) < 1e-6f,
          "CONTROL after FIX: X4's default base put back");
    a = decide({want, opt * want}, opt, true);
    check(a.write_effective && std::fabs(a.effective - want) < 1e-6f,
          "compounded effective (164 deg) restored to 110");
    check(opt * want * 57.29578f > 160.f, "...which is what it would otherwise be");

    // ---- phases ------------------------------------------------------------
    check(!fix_phase(0.0, 45.0) && !fix_phase(44.9, 45.0) && fix_phase(45.1, 45.0) &&
              !fix_phase(90.5, 45.0),
          "CONTROL first, then FIX, alternating every period");
    check(!fix_phase(1000.0, 0.0), "no period: never FIX");

    // ---- the binary -------------------------------------------------------
    const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
    Elf elf;
    if (!elf.load(x4)) {
        printf("skip  field sites — no X4 at %s\n", x4);
    } else {
        auto rd = [&](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
        uintptr_t bad = 0;
        check(verify_sites(rd, &bad), "every field site matches X4 9.00 byte for byte");
        float def = 0.f;
        check(rd(kDefaultAddr, &def, 4) && def == kDefault,
              "X4's default-field constant reads 1.2870022 rad (73.74 deg)");
        auto bent = [&](uintptr_t va, void *o, size_t n) {
            if (!elf.read(va, o, n))
                return false;
            if (va == 0x19276fc)
                ((uint8_t *)o)[5] ^= 0x04; // +0x890 -> +0x894
            return true;
        };
        bad = 0;
        check(!verify_sites(bent, &bad) && bad == 0x19276fc,
              "one changed byte refuses, and names the site");
    }
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
