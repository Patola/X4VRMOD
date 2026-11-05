// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vrmod: the launcher's command line. Every check and action lives in the
// core; this only displays it.
#include <cstdio>
#include <cstring>
#include <string>

#include "x4vr_launcher.hpp"

using namespace x4vr::launcher;

static void usage(FILE *f) {
    fprintf(f,
            "x4vrmod %s: X4 VR Mod's launcher\n\n"
            "  x4vrmod doctor [--game PATH] [--no-vr]\n"
            "      every condition for playing, one line each. Exit code: 0 ready,\n"
            "      1 can play with something lost, 2 can't play yet.\n"
            "  x4vrmod probe xr [--verbose]     ask the VR runtime (always asks)\n"
            "  x4vrmod probe vulkan             what the GPUs offer\n"
            "  x4vrmod probe x4 [PATH]          X4's version and which patches fit\n"
            "  x4vrmod version\n",
            own_version());
}

static int cmd_doctor(int argc, char **argv) {
    CheckOptions o;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) {
            o.game = argv[++i];
        } else if (!strcmp(argv[i], "--no-vr")) {
            o.skip_vr = true;
        } else {
            fprintf(stderr, "x4vrmod doctor: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    const Layout l = detect_layout(home_dir());
    printf("x4vrmod %s (%s)\n\n", own_version(), layout_name(l.kind));
    const std::vector<Check> checks = run_checks(o);
    int blocked = 0;
    for (const Check &c : checks) {
        const char *tag = c.status == Status::Ok ? "ok   " : c.status == Status::Warn ? "warn " : "block";
        printf("%s %s%s%s\n", tag, c.title.c_str(), c.detail.empty() ? "" : ": ", c.detail.c_str());
        if (!c.fix.empty())
            printf("      fix: %s\n", c.fix.c_str());
        if (c.status == Status::Block)
            blocked++;
    }
    const Status w = worst(checks);
    printf("\n");
    if (w == Status::Ok)
        printf("Play: green, ready.\n");
    else if (w == Status::Warn)
        printf("Play: amber, it can start with something lost.\n");
    else
        printf("Play: grey, %d thing%s to fix first.\n", blocked, blocked == 1 ? "" : "s");
    return w == Status::Ok ? 0 : w == Status::Warn ? 1 : 2;
}

static int cmd_probe_x4(int argc, char **argv) {
    std::string path;
    if (argc > 3) {
        path = argv[3];
    } else {
        const std::vector<Install> all = discover_all(home_dir());
        if (const Install *p = preferred_install(all))
            path = p->exe;
    }
    if (path.empty()) {
        printf("X4 not found. Give its folder or program as an argument.\n");
        return 2;
    }
    const GamePath g = classify_game_path(path);
    if (g.kind == PathKind::Windows) {
        printf("%s\n", proton_explanation());
        return 2;
    }
    if (g.kind != PathKind::Linux) {
        printf("%s: %s\n", path.c_str(), g.why.c_str());
        return 2;
    }
    ElfFile elf;
    if (!elf.open(g.exe)) {
        printf("%s: can't read it as a 64-bit x86 program\n", g.exe.c_str());
        return 2;
    }
    printf("program  %s\n", g.exe.c_str());
    printf("version  %s (this release was tested with %s)\n",
           format_x4_version(read_x4_version(g.dir)).c_str(),
           format_x4_version(kTestedX4Version).c_str());
    printf("steam    %s\n", binary_needs_steam(g.exe) ? "links libsteam_api: needs Steam running"
                                                      : "doesn't link libsteam_api");
    int bad = 0;
    const auto rd = [&elf](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
    for (const PatchResult &r : check_patches(rd)) {
        if (r.ok) {
            printf("ok       %-9s %s\n", r.id, r.feature);
        } else {
            printf("differs  %-9s %s (at %#lx)\n", r.id, r.feature, (unsigned long)r.bad);
            bad++;
        }
    }
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return argc < 2 ? 2 : 0;
    }
    const std::string cmd = argv[1];
    if (cmd == "version" || cmd == "--version") {
        printf("%s\n", own_version());
        return 0;
    }
    if (cmd == "doctor")
        return cmd_doctor(argc, argv);
    if (cmd == "probe" && argc >= 3) {
        const std::string what = argv[2];
        if (what == "xr") {
            const bool verbose = argc > 3 && !strcmp(argv[3], "--verbose");
            const XrProbe p = xr_probe_here(verbose);
            fputs(xr_probe_format(p).c_str(), stdout);
            return p.status == XrStatus::Ready ? 0 : 2;
        }
        if (what == "vulkan") {
            const VulkanProbe p = vulkan_probe_here();
            fputs(vulkan_probe_format(p).c_str(), stdout);
            return p.ok ? 0 : 2;
        }
        if (what == "x4")
            return cmd_probe_x4(argc, argv);
    }
    usage(stderr);
    return 2;
}
