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
            "  x4vrmod install [--game PATH]    put the mod in place\n"
            "  x4vrmod uninstall [--all] [--yes] [--dry-run]\n"
            "      take it away. --all also deletes your VR settings, which can't\n"
            "      be undone. X4's saves and its own settings are never touched.\n"
            "  x4vrmod report [--log FILE] [--note TEXT]\n"
            "      make a bug report to read and attach to an issue; nothing is sent\n"
            "  x4vrmod run [--debug | --flat] [--game PATH] [COMMAND...]\n"
            "      play. With COMMAND it wraps it: Steam's launch options use\n"
            "      `x4vrmod run %%command%%`.\n"
            "  x4vrmod launch-options           the lines for Steam and a terminal\n"
            "  x4vrmod config [list | get KEY | set KEY VALUE | unset KEY]\n"
            "      eye, upscaler, ipd, fov, gamemode, language, game, env.NAME. ipd overrides\n"
            "      the headset's eye distance, in metres: 0.0635 is 63.5 mm. language is the\n"
            "      window's (en, pt_BR, fr, ru, de); this command's output stays English\n"
            "  x4vrmod locate [--all]           the X4 program that would run; --all: every copy found\n"
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
        fputs(checks_text({c}).c_str(), stdout);
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
        printf("Play: grey, %d pending action%s.\n", blocked, blocked == 1 ? "" : "s");
    return w == Status::Ok ? 0 : w == Status::Warn ? 1 : 2;
}

static bool confirm(const char *question) {
    printf("%s ", question);
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof line, stdin))
        return false;
    return !strcmp(line, "y\n") || !strcmp(line, "yes\n");
}

static int cmd_install(int argc, char **argv) {
    std::string game;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) {
            game = argv[++i];
        } else {
            fprintf(stderr, "x4vrmod install: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    const std::string home = home_dir();
    bool found = false;
    const Install in = choose_install(home, game, &found);
    printf("%s\n", install_disclosure());
    const ActionReport r = install_mod(in, home);
    for (const std::string &l : r.log)
        printf("%s\n", l.c_str());
    if (!r.ok) {
        fprintf(stderr, "x4vrmod install: %s\n", r.error.c_str());
        return 2;
    }
    printf("Installed. Check with: x4vrmod doctor\n");
    return 0;
}

static int cmd_uninstall(int argc, char **argv) {
    bool all = false, yes = false, dry = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--all"))
            all = true;
        else if (!strcmp(argv[i], "--yes"))
            yes = true;
        else if (!strcmp(argv[i], "--dry-run"))
            dry = true;
        else {
            fprintf(stderr, "x4vrmod uninstall: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    const std::string home = home_dir();
    if (!dry && x4_is_running("")) {
        fprintf(stderr, "x4vrmod uninstall: X4 is running. Close it first.\n");
        return 2;
    }
    const UninstallPlan p = uninstall_plan(home, all);
    if (p.mod.empty() && p.settings.empty()) {
        printf("Nothing of the mod's is installed.\n");
        if (!p.note.empty())
            printf("%s\n", p.note.c_str());
        return 0;
    }
    if (!p.mod.empty())
        printf("The mod:\n");
    for (const std::string &m : p.mod)
        printf("  %s\n", m.c_str());
    if (!p.settings.empty()) {
        printf("Your VR settings. This can't be undone:\n");
        for (const std::string &m : p.settings)
            printf("  %s\n", m.c_str());
    }
    printf("X4's saves and its own settings are not touched.\n");
    if (!p.note.empty())
        printf("%s\n", p.note.c_str());
    if (dry)
        return 0;
    if (!yes && !confirm(all ? "Remove all of this, for good? [y/N]" : "Remove these? [y/N]")) {
        printf("Nothing removed.\n");
        return 1;
    }
    const ActionReport r = uninstall_apply(p);
    for (const std::string &l : r.log)
        printf("%s\n", l.c_str());
    if (!r.ok) {
        fprintf(stderr, "x4vrmod uninstall: %s\n", r.error.c_str());
        return 2;
    }
    return 0;
}

static int cmd_run(int argc, char **argv) {
    std::string game;
    bool flat = false;
    int i = 2;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--debug")) {
            setenv("X4VR_DEBUG", "1", 1);
        } else if (!strcmp(argv[i], "--flat")) {
            flat = true;
        } else if (i + 1 < argc && !strcmp(argv[i], "--game")) {
            game = argv[++i];
        } else {
            break;
        }
    }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;
    std::vector<std::string> command(argv + i, argv + argc);
    if (flat) {
        if (!command.empty()) {
            fprintf(stderr, "x4vrmod run: --flat starts X4 itself, and takes no command\n");
            return 2;
        }
        return run_flat(home_dir(), game);
    }
    return run_mod(home_dir(), game, command);
}

static int cmd_report(int argc, char **argv) {
    ReportOptions o;
    o.home = home_dir();
    bool logGiven = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            o.log = argv[++i];
            logGiven = true;
        } else if (!strcmp(argv[i], "--note") && i + 1 < argc) {
            o.note = argv[++i];
        } else if (!strcmp(argv[i], "--game") && i + 1 < argc) {
            o.game = argv[++i];
        } else {
            fprintf(stderr, "x4vrmod report: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!logGiven) {
        const std::vector<SessionLog> logs = session_logs(o.home);
        if (!logs.empty())
            o.log = logs[0].path;
    }
    const ReportResult r = make_report(o);
    if (!r.ok) {
        fprintf(stderr, "x4vrmod report: %s\n", r.error.c_str());
        return 2;
    }
    printf("Report saved. Read it first, if you like, nothing has been sent:\n  %s\n\n", r.dir.c_str());
    printf("Attach this file to a new issue:\n  %s\n\n", r.archive.c_str());
    if (r.too_big)
        printf("It's over GitHub's 25 MB limit for an attachment. Share it another way and "
               "put the link in the issue.\n\n");
    printf("New issue, with your system filled in:\n  %s\n", r.issue_url.c_str());
    return 0;
}

static int cmd_config(int argc, char **argv) {
    const std::string path = profile_path(home_dir());
    Profile p = load_profile(path);
    const std::string what = argc > 2 ? argv[2] : "list";
    if (what == "list") {
        for (const auto &[k, v] : p.values)
            printf("%s = %s\n", k.c_str(), v.c_str());
        return 0;
    }
    if (what == "get" && argc == 4) {
        const auto it = p.values.find(argv[3]);
        if (it == p.values.end())
            return 1;
        printf("%s\n", it->second.c_str());
        return 0;
    }
    if (what == "unset" && argc == 4) {
        // Back to what it is when never set: for ipd, the headset's.
        if (p.values.erase(argv[3]) && !save_profile(path, p)) {
            fprintf(stderr, "x4vrmod config: couldn't write %s\n", path.c_str());
            return 2;
        }
        return 0;
    }
    if (what == "set" && argc == 5) {
        const std::string note = profile_set(p, argv[3], argv[4]);
        if (note.rfind("error: ", 0) == 0) {
            fprintf(stderr, "x4vrmod config: %s\n", note.c_str() + 7);
            return 2;
        }
        if (!save_profile(path, p)) {
            fprintf(stderr, "x4vrmod config: couldn't write %s\n", path.c_str());
            return 2;
        }
        if (!note.empty())
            printf("%s\n", note.c_str());
        return 0;
    }
    usage(stderr);
    return 2;
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
    // How far this build moved what the mod names.
    if (argc > 4 && !strcmp(argv[4], "--map")) {
        fputs(reloc_report(elf).c_str(), stdout);
        return 0;
    }
    int bad = 0;
    for (const PatchResult &r : check_patches(elf)) {
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
    if (cmd == "install")
        return cmd_install(argc, argv);
    if (cmd == "uninstall")
        return cmd_uninstall(argc, argv);
    if (cmd == "run")
        return cmd_run(argc, argv);
    if (cmd == "report")
        return cmd_report(argc, argv);
    if (cmd == "config")
        return cmd_config(argc, argv);
    if (cmd == "locate") {
        if (argc > 2 && !strcmp(argv[2], "--all")) {
            for (const Install &in : discover_all(home_dir()))
                printf("%s\t%s\t%s\n", in.flatpak_steam ? "Steam (Flatpak)" : store_name(in.store),
                       in.kind == PathKind::Windows ? "windows" : "linux", in.exe.c_str());
            return 0;
        }
        bool found = false;
        const Install in = choose_install(home_dir(), "", &found);
        if (!found || in.kind != PathKind::Linux)
            return 1;
        printf("%s\n", in.exe.c_str());
        return 0;
    }
    if (cmd == "launch-options") {
        printf("Steam launch options:  %s\n", steam_launch_line(home_dir()).c_str());
        printf("In a terminal:         %s\n", terminal_line(home_dir()).c_str());
        return 0;
    }
    if (cmd == "probe" && argc >= 3) {
        const std::string what = argv[2];
        if (what == "xr") {
            const bool verbose = argc > 3 && !strcmp(argv[3], "--verbose");
            // A SteamVR that isn't the default OpenXR runtime: point the loader
            // at it, as `run` does for X4.
            const ActiveRuntime rt = active_runtime(home_dir());
            if (rt.fallback)
                setenv("XR_RUNTIME_JSON", rt.path.c_str(), 1);
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
