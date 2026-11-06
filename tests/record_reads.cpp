// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Which of X4's bytes the mod's checks read, for tools/make_reloc_table.py: every check,
// against Steam's X4, printing each read as "address length". Not a test.
#include <cstdio>
#include <map>

#include "../common/x4vr_steer.hpp"
#include "../injector/x4vr_aimpick.hpp"
#include "../injector/x4vr_camwatch.hpp"
#include "../injector/x4vr_camwrite.hpp"
#include "../injector/x4vr_csmfit.hpp"
#include "../injector/x4vr_extview.hpp"
#include "../injector/x4vr_fieldab.hpp"
#include "../injector/x4vr_foot.hpp"
#include "../injector/x4vr_loadart.hpp"
#include "../injector/x4vr_menus.hpp"
#include "../injector/x4vr_shadowfov.hpp"
#include "../injector/x4vr_uimouse.hpp"
#include "x4vr_launcher.hpp"

using namespace x4vr;

int main(int argc, char **argv) {
    launcher::ElfFile elf;
    if (argc < 2 || !elf.open(argv[1])) {
        fprintf(stderr, "usage: x4vr_record_reads <Steam's X4>\n");
        return 2;
    }
    std::map<uintptr_t, size_t> reads;
    auto rd = [&](uintptr_t va, void *o, size_t n) {
        size_t &have = reads[va];
        if (n > have)
            have = n;
        return elf.read(va, o, n);
    };
    uintptr_t bad = 0;
    bool ok = true;
    auto all = [&](bool r, const char *what) {
        if (!r)
            fprintf(stderr, "%s: differs at %#lx\n", what, (unsigned long)bad);
        ok = ok && r;
    };
    all(camwatch::verify_hop2_sites(rd, &bad), "hop2");
    all(camwatch::verify_hop2b_sites(rd, &bad), "hop2b");
    all(camwatch::verify_hop3_sites(rd, &bad), "hop3");
    all(camwrite::verify_write_sites(rd, &bad), "camwrite");
    all(foot::verify_null_exit_sites(rd, &bad), "foot exit");
    all(foot::verify_skip_sites(rd, &bad), "foot skip");
    all(extview::verify_sites(rd, &bad), "extview");
    all(uimouse::verify_sites(rd, &bad), "uimouse");
    all(steer::verify_sites(rd, &bad), "steer");
    all(aimpick::verify_sites(rd, &bad), "aimpick");
    all(menus::verify_sites(rd, &bad), "menus");
    all(csmfit::verify_sites(rd, &bad), "csmfit");
    all(loadart::verify_sites(rd, &bad), "loadart");
    all(shadowfov::verify_sites(rd, &bad), "shadowfov");
    all(fieldab::verify_sites(rd, &bad), "fieldab");
    for (const auto &[va, n] : reads)
        printf("%#lx %zu\n", (unsigned long)va, n);
    return ok ? 0 : 1;
}
