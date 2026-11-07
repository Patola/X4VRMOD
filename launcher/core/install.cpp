// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Install, uninstall and run. Installing from the zip copies it into the user's
// data folder, because Steam launch options and the layer manifest need paths
// that survive the unpacked folder moving; a package only needs the extension.
// Uninstall removes what's ours by exact name and never anything else.
#include "x4vr_launcher.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace x4vr::launcher {

static const char *const kExtensionNames[] = {"x4vrmod", "x4vrmod_lookaround",
                                              "x4vrmod_nosway", "x4vrmod_headlook"};

static std::string layer_manifest_user(const std::string &home) {
    return home + "/.local/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json";
}
static std::string install_root_user(const std::string &home) {
    return home + "/.local/share/x4vrmod";
}
static std::string record_path(const std::string &home) {
    return install_root_user(home) + "/installed.json";
}

static std::string json_quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

Install choose_install(const std::string &home, const std::string &game, bool *found) {
    *found = false;
    // As everywhere: what's asked for now, then the environment, then the profile.
    std::string want = game;
    if (want.empty())
        if (const char *e = getenv("X4VR_GAME"))
            want = e;
    if (want.empty())
        want = load_profile(profile_path(home)).get("game");
    if (!want.empty()) {
        Install in = install_from_path(want);
        if (in.kind == PathKind::Linux || in.kind == PathKind::Windows) {
            *found = true;
            for (const Install &d : discover_all(home))
                if (real_path(d.exe) == real_path(in.exe) && !d.buildid.empty())
                    in.buildid = d.buildid;
        }
        return in;
    }
    const std::vector<Install> all = discover_all(home);
    if (const Install *p = preferred_install(all)) {
        *found = true;
        return *p;
    }
    return Install{};
}

const char *install_disclosure() {
    return "X4 marks saves made with a non-Egosoft extension as modified; the mod's "
           "extension is enabled only in VR sessions.";
}

std::string menu_entry_path(const std::string &home) {
    return home + "/.local/share/applications/x4vrmod-launcher.desktop";
}

bool menu_entry_possible(const std::string &home) {
    return detect_layout(home).kind == LayoutKind::Zip &&
           is_file(install_root_user(home) + "/x4vrmod-launcher");
}

ActionReport add_menu_entry(const std::string &home) {
    ActionReport r;
    if (!menu_entry_possible(home)) {
        r.error = "Install the mod first: the menu needs the copy whose path stays.";
        return r;
    }
    const std::string root = install_root_user(home), path = menu_entry_path(home);
    std::error_code ec;
    fs::create_directories(dir_name(path), ec);
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        r.error = "Couldn't write " + path;
        return r;
    }
    fprintf(f,
            "[Desktop Entry]\nType=Application\nName=X4 VR Mod\n"
            "Comment=Install and launch the VR mod for X4: Foundations\n"
            "Exec=\"%s/x4vrmod-launcher\"\nIcon=%s/x4vrmod-data/help/icon.png\n"
            "Categories=Game;\nTerminal=false\n",
            root.c_str(), root.c_str());
    fclose(f);
    r.ok = true;
    r.log.push_back("Added to the application menu: " + path);
    return r;
}

ActionReport install_mod(const Install &x4, const std::string &home) {
    ActionReport r;
    if (x4.kind != PathKind::Linux) {
        r.error = x4.kind == PathKind::Windows ? proton_explanation() : "X4 wasn't found.";
        return r;
    }
    const Layout l = detect_layout(home);
    if (!is_file(l.deploy_script)) {
        r.error = "This copy of the mod is incomplete: no " + l.deploy_script;
        return r;
    }
    std::vector<std::string> written;
    std::error_code ec;
    if (l.kind == LayoutKind::Zip) {
        const std::string root = install_root_user(home);
        const std::string dest = root + "/x4vrmod-data";
        // Not over itself, when the installed copy is what's running.
        if (real_path(l.data_dir) != real_path(dest)) {
            fs::remove_all(dest, ec);
            fs::create_directories(root, ec);
            fs::copy(l.data_dir, dest, fs::copy_options::recursive, ec);
            if (ec) {
                r.error = "Couldn't copy the mod to " + dest + ": " + ec.message();
                return r;
            }
            for (const char *prog : {"x4vrmod", "x4vrmod-launcher"}) {
                const std::string from = self_dir() + "/" + prog;
                if (is_file(from)) {
                    fs::copy_file(from, root + "/" + prog,
                                  fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        r.error = "Couldn't copy " + from + ": " + ec.message();
                        return r;
                    }
                }
            }
        }
        written.push_back(root);
        r.log.push_back("Mod copied to " + root);
        // The manifest names the library by absolute path, so it's made here.
        std::string json = read_text_file(dest + "/lib/VK_LAYER_X4VR_core.json.in", 64 * 1024);
        const std::string token = "@X4VR_LAYER_LIB@";
        const size_t at = json.find(token);
        if (at == std::string::npos) {
            r.error = "This copy of the mod is incomplete: no layer manifest template.";
            return r;
        }
        json.replace(at, token.size(), dest + "/lib/libVkLayer_X4VR_core.so");
        const std::string man = layer_manifest_user(home);
        fs::create_directories(dir_name(man), ec);
        FILE *f = fopen(man.c_str(), "w");
        if (!f || fputs(json.c_str(), f) < 0) {
            if (f)
                fclose(f);
            r.error = "Couldn't write " + man;
            return r;
        }
        fclose(f);
        written.push_back(man);
        r.log.push_back("Vulkan layer registered: " + man);
    }
    // One deployer, the script a launch uses too. It removes the retired names.
    const ChildResult c = run_child({l.deploy_script, x4.dir}, 60000, true);
    if (!c.started || c.exit_code != 0) {
        r.error = "Couldn't put the extension in X4's folder: " + c.out;
        return r;
    }
    r.log.push_back("Extension in place: " + x4.dir + "/extensions/x4vrmod");
    written.push_back(x4.dir + "/extensions/x4vrmod");

    fs::create_directories(install_root_user(home), ec);
    const std::string rec = record_path(home);
    // Keep what an earlier install recorded: another copy of X4's extension.
    for (const std::string &old : json_strings(read_text_file(rec), "path"))
        if (std::find(written.begin(), written.end(), old) == written.end())
            written.push_back(old);
    FILE *f = fopen(rec.c_str(), "w");
    if (!f) {
        r.error = "Couldn't write " + rec;
        return r;
    }
    fprintf(f, "{\n  \"version\": %s,\n  \"layout\": %s,\n  \"installed\": [\n",
            json_quote(own_version()).c_str(), json_quote(layout_name(l.kind)).c_str());
    for (size_t i = 0; i < written.size(); i++)
        fprintf(f, "    {\"path\": %s}%s\n", json_quote(written[i]).c_str(),
                i + 1 < written.size() ? "," : "");
    fputs("  ]\n}\n", f);
    fclose(f);
    r.ok = true;
    return r;
}

// Only what we put there, by exact name: a record that says otherwise is ignored.
static bool is_ours(const std::string &path, const std::string &home) {
    if (path == install_root_user(home) || path == layer_manifest_user(home) ||
        path == menu_entry_path(home))
        return true;
    const std::string parent = base_name(dir_name(path));
    if (parent != "extensions")
        return false;
    const std::string name = base_name(path);
    return std::any_of(std::begin(kExtensionNames), std::end(kExtensionNames),
                       [&name](const char *n) { return name == n; });
}

UninstallPlan uninstall_plan(const std::string &home, bool with_settings) {
    UninstallPlan p;
    const Layout l = detect_layout(home);
    auto add = [&p, &home](const std::string &path) {
        if (!is_ours(path, home) || (!is_file(path) && !is_dir(path)))
            return;
        if (std::find(p.mod.begin(), p.mod.end(), path) == p.mod.end())
            p.mod.push_back(path);
    };
    std::vector<std::string> dirs;
    for (const Install &in : discover_all(home))
        dirs.push_back(in.dir);
    const std::string chosen = load_profile(profile_path(home)).get("game");
    if (!chosen.empty())
        dirs.push_back(dir_name(chosen));
    for (const std::string &d : dirs)
        for (const char *n : kExtensionNames)
            add(d + "/extensions/" + n);
    for (const std::string &r : json_strings(read_text_file(record_path(home)), "path"))
        add(r);
    add(layer_manifest_user(home));
    add(install_root_user(home));
    add(menu_entry_path(home));

    if (l.kind == LayoutKind::Package)
        p.note = "The package itself stays. Remove it with your package manager.";
    if (!with_settings)
        return p;

    // The mod's forks in every X4 profile folder, and nothing else there: saves,
    // config.xml and the rest are the player's.
    for (const std::string &prof : x4_profile_dirs(home)) {
        DIR *f = opendir(prof.c_str());
        if (!f)
            continue;
        while (const dirent *g = readdir(f)) {
            const std::string n = g->d_name;
            const bool fork = n == "config-x4vrmod.xml" || n == "content-x4vrmod.xml" ||
                              n == "inputmap-x4vrmod.xml";
            const bool side = n.find("-x4vrmod-") != std::string::npos && n.size() > 4 &&
                              n.compare(n.size() - 4, 4, ".was") == 0;
            if ((fork || side) && is_file(prof + "/" + n))
                p.settings.push_back(prof + "/" + n);
        }
        closedir(f);
    }
    const std::string pf = profile_path(home);
    if (is_file(pf))
        p.settings.push_back(pf);
    std::sort(p.settings.begin(), p.settings.end());
    return p;
}

std::vector<std::string> x4_profile_dirs(const std::string &home) {
    std::vector<std::string> roots;
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x)
        roots.push_back(std::string(x) + "/EgoSoft/X4");
    roots.push_back(home + "/.config/EgoSoft/X4");
    std::vector<std::string> out;
    auto add = [&out](const std::string &dir) {
        for (const std::string &have : out)
            if (real_path(have) == real_path(dir))
                return;
        out.push_back(dir);
    };
    for (const std::string &root : roots) {
        if (!is_dir(root))
            continue;
        add(root);
        DIR *d = opendir(root.c_str());
        if (!d)
            continue;
        while (const dirent *e = readdir(d))
            if (e->d_name[0] != '.' && is_dir(root + "/" + e->d_name))
                add(root + "/" + e->d_name);
        closedir(d);
    }
    return out;
}

ActionReport uninstall_apply(const UninstallPlan &p) {
    ActionReport r;
    r.ok = true;
    std::error_code ec;
    for (const std::vector<std::string> *list : {&p.mod, &p.settings})
        for (const std::string &path : *list) {
            fs::remove_all(path, ec);
            if (ec) {
                r.ok = false;
                r.error += (r.error.empty() ? "" : "\n") + path + ": " + ec.message();
            } else {
                r.log.push_back("Removed " + path);
            }
        }
    return r;
}

int run_flat(const std::string &home, const std::string &game) {
    bool found = false;
    const Install in = choose_install(home, game, &found);
    if (in.kind == PathKind::Windows) {
        fprintf(stderr, "x4vrmod: %s\n", proton_explanation());
        return 2;
    }
    if (!found || in.kind != PathKind::Linux) {
        fprintf(stderr, "x4vrmod: X4 wasn't found. Use: x4vrmod config set game PATH\n");
        return 2;
    }
    if (chdir(in.dir.c_str()) != 0) {
        perror("x4vrmod: couldn't enter X4's folder");
        return 2;
    }
    if (binary_needs_steam(in.exe)) {
        setenv("SteamAppId", kSteamAppId, 1);
        setenv("SteamGameId", kSteamAppId, 1);
    }
    // testandlaunch is how X4 starts itself; the flags are the ones a mod session gets,
    // so the mod is the only difference between the two.
    const std::string own = is_file("testandlaunch") ? "./testandlaunch" : "./" + base_name(in.exe);
    std::vector<char *> argv = {const_cast<char *>(own.c_str()), const_cast<char *>("-skipintro"),
                                const_cast<char *>("-nocputhrottle"),
                                const_cast<char *>("-nosoundthrottle"), nullptr};
    // GameMode isn't the mod's: a baseline with it off wouldn't compare with a session with it on.
    const char *gm_env = getenv("X4VR_GAMEMODE");
    const std::string gm = gm_env ? gm_env : load_profile(profile_path(home)).get("gamemode");
    const std::string wrap = !gm.empty() && gm[0] != '0' ? find_on_path("gamemoderun") : "";
    if (!wrap.empty())
        argv.insert(argv.begin(), const_cast<char *>(wrap.c_str()));
    fflush(nullptr);
    execv(argv[0], argv.data());
    perror("x4vrmod: couldn't start X4");
    return 2;
}

std::string steam_launch_line(const std::string &home) {
    const Layout l = detect_layout(home);
    if (l.kind == LayoutKind::Package)
        return "x4vrmod run %command%";
    if (l.kind == LayoutKind::Zip)
        return install_root_user(home) + "/x4vrmod run %command%";
    return self_dir() + "/x4vrmod run %command%";
}

std::string terminal_line(const std::string &home) {
    const Layout l = detect_layout(home);
    if (l.kind == LayoutKind::Package)
        return "x4vrmod run";
    if (l.kind == LayoutKind::Zip)
        return install_root_user(home) + "/x4vrmod run";
    return self_dir() + "/x4vrmod run";
}

// One log per run, because the scorer refuses a file holding two sessions; the
// newest twenty stay.
static std::string new_log_path(const std::string &home, bool debug) {
    const char *x = getenv("XDG_STATE_HOME");
    const std::string dir = (x && *x ? std::string(x) : home + "/.local/state") + "/x4vrmod/logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<std::string> logs;
    if (DIR *d = opendir(dir.c_str())) {
        while (const dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("x4vrmod-", 0) == 0 && n.size() > 4 &&
                n.compare(n.size() - 4, 4, ".log") == 0)
                logs.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(logs.begin(), logs.end());
    while (logs.size() >= 20) {
        unlink(logs.front().c_str());
        logs.erase(logs.begin());
    }
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    return dir + "/x4vrmod-" + stamp + (debug ? "-debug.log" : ".log");
}

std::string log_dir(const std::string &home) {
    const char *x = getenv("XDG_STATE_HOME");
    return (x && *x ? std::string(x) : home + "/.local/state") + "/x4vrmod/logs";
}

// Started from Steam there's no terminal to read: a notification too, if it can.
static void tell_player(const std::string &what, bool from_steam) {
    fprintf(stderr, "x4vrmod: %s\n", what.c_str());
    const std::string ns = from_steam ? find_on_path("notify-send") : "";
    if (!ns.empty())
        run_child({ns, "X4 VR Mod", what}, 2000, false);
}

int run_mod(const std::string &home, const std::string &game,
            const std::vector<std::string> &command) {
    const Layout l = detect_layout(home);
    if (!is_file(l.launch_script)) {
        tell_player("the mod isn't installed. Run: x4vrmod install", !command.empty());
        return 2;
    }
    // Which copy Steam's %command% means. It's a chain of wrappers, with X4 or its start script
    // at the end: steam-launch-wrapper, reaper, the runtime's entry point, testandlaunch.
    std::string named = game;
    bool from_chain = false;
    for (const std::string &a : command) {
        const std::string b = base_name(a);
        if (b == "X4.exe" || b == "proton") {
            tell_player(proton_explanation(), true);
            return 2;
        }
        if (b == "X4" || b == "testandlaunch") {
            named = b == "X4" ? a : dir_name(a) + "/X4";
            from_chain = true;
        }
    }
    bool found = false;
    const Install in = choose_install(home, named, &found);
    if (!command.empty() && !from_chain && found)
        fprintf(stderr, "x4vrmod: this command doesn't name an X4, so the mod takes it to be %s\n",
                in.exe.c_str());
    if (in.kind == PathKind::Windows) {
        fprintf(stderr, "x4vrmod: %s\n", proton_explanation());
        return 2;
    }
    if (in.kind != PathKind::Linux || !found) {
        if (command.empty()) {
            fprintf(stderr, "x4vrmod: X4 wasn't found. Use: x4vrmod config set game PATH\n");
            return 2;
        }
    } else {
        // choose_install has already weighed X4VR_GAME; the script must run the
        // copy this decided about, not another one.
        setenv("X4VR_GAME", in.exe.c_str(), 1);
        setenv("X4VR_NEEDS_STEAM", binary_needs_steam(in.exe) ? "1" : "0", 0);
    }
    // An explicit environment variable wins over the profile, as it does over
    // the defaults table.
    for (const auto &[k, v] : profile_env(load_profile(profile_path(home))))
        setenv(k.c_str(), v.c_str(), 0);
    if (l.kind != LayoutKind::Dev) {
        setenv("X4VR_LAYER_MANIFEST", l.layer_manifest.c_str(), 0);
        setenv("X4VR_INJECTOR", l.injector.c_str(), 0);
    }
    if (!getenv("X4VR_LOG")) {
        const char *d = getenv("X4VR_DEBUG");
        setenv("X4VR_LOG", new_log_path(home, d && !strcmp(d, "1")).c_str(), 1);
    }

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(l.launch_script.c_str()));
    for (const std::string &a : command)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    fflush(nullptr);
    execv(l.launch_script.c_str(), argv.data());
    perror("x4vrmod: couldn't start the launch script");
    return 2;
}

} // namespace x4vr::launcher
