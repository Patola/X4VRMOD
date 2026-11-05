// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The checks behind the Play button, one per row of the window and one per line
// of `x4vrmod doctor`. Everything here only reads.
#include "x4vr_launcher.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstring>

// cmake/x4vr_version.cmake, as in the layer and the injector.
const char *x4vr_build_version();

namespace x4vr::launcher {

const char *own_version() { return x4vr_build_version(); }

const char *layout_name(LayoutKind k) {
    return k == LayoutKind::Dev ? "build tree" : k == LayoutKind::Zip ? "zip" : "package";
}

Layout detect_layout(const std::string &home) {
    Layout l;
    const std::string here = self_dir();
    const char *over = getenv("X4VRMOD_DATA");
    const std::string zip = over && *over ? std::string(over) : here + "/x4vrmod-data";
    if (is_file(zip + "/manifest")) {
        l.kind = LayoutKind::Zip;
        l.data_dir = zip;
        l.extension_dir = zip + "/extension/x4vrmod";
        l.deploy_script = zip + "/launch/deploy-extensions.sh";
        // What the zip installs into, so paths survive the folder moving.
        const std::string in = home + "/.local/share/x4vrmod/x4vrmod-data";
        l.layer_manifest =
            home + "/.local/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json";
        l.injector = in + "/lib/libx4vr_inject.so";
        l.launch_script = in + "/launch/x4vr-launch.sh";
        l.help_dir = zip + "/help";
        return l;
    }
    if (here == X4VRMOD_BUILD_DIR "/launcher" || here == X4VRMOD_BUILD_DIR "/tests") {
        l.kind = LayoutKind::Dev;
        l.data_dir = X4VRMOD_SOURCE_DIR;
        l.extension_dir = X4VRMOD_SOURCE_DIR "/extension/x4vrmod";
        l.layer_manifest = X4VRMOD_BUILD_DIR "/layer/VK_LAYER_X4VR_core.json";
        l.injector = X4VRMOD_BUILD_DIR "/injector/libx4vr_inject.so";
        l.launch_script = X4VRMOD_SOURCE_DIR "/launch/x4vr-launch.sh";
        l.deploy_script = X4VRMOD_SOURCE_DIR "/launch/deploy-extensions.sh";
        l.help_dir = X4VRMOD_SOURCE_DIR "/launcher/help";
        return l;
    }
    l.kind = LayoutKind::Package;
    l.data_dir = X4VRMOD_PREFIX "/share/x4vrmod";
    l.extension_dir = l.data_dir + "/extension/x4vrmod";
    l.layer_manifest =
        X4VRMOD_PREFIX "/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json";
    l.injector = X4VRMOD_PREFIX "/lib/x4vrmod/libx4vr_inject.so";
    l.launch_script = l.data_dir + "/launch/x4vr-launch.sh";
    l.deploy_script = l.data_dir + "/launch/deploy-extensions.sh";
    l.help_dir = l.data_dir + "/help";
    return l;
}

std::string library_version(const std::string &path) {
    const std::string t = read_text_file(path, 256 * 1024 * 1024);
    static const char kMarker[] = "X4VRMOD_VERSION=";
    const size_t at = t.find(kMarker);
    if (at == std::string::npos)
        return "";
    const size_t start = at + sizeof kMarker - 1;
    const size_t end = t.find('\0', start);
    return end == std::string::npos ? "" : t.substr(start, end - start);
}

// Are these two folders' files the same, both ways?
static bool same_tree(const std::string &a, const std::string &b) {
    DIR *d = opendir(a.c_str());
    if (!d || !is_dir(b)) {
        if (d)
            closedir(d);
        return false;
    }
    std::vector<std::string> names;
    while (const dirent *e = readdir(d))
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            names.push_back(e->d_name);
    closedir(d);
    size_t others = 0;
    if (DIR *o = opendir(b.c_str())) {
        while (const dirent *e = readdir(o))
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
                others++;
        closedir(o);
    }
    if (others != names.size())
        return false;
    for (const std::string &n : names) {
        const std::string pa = a + "/" + n, pb = b + "/" + n;
        if (is_dir(pa)) {
            if (!same_tree(pa, pb))
                return false;
        } else if (!is_file(pb) || read_text_file(pa) != read_text_file(pb)) {
            return false;
        }
    }
    return true;
}

Status worst(const std::vector<Check> &checks) {
    Status s = Status::Ok;
    for (const Check &c : checks) {
        if (c.status == Status::Block)
            return Status::Block;
        if (c.status == Status::Warn)
            s = Status::Warn;
    }
    return s;
}

static Check check_patches_row(const Install &in) {
    Check c;
    c.id = "patches";
    ElfFile elf;
    if (!elf.open(in.exe)) {
        c.status = Status::Warn;
        c.title = "Can't read X4's program";
        c.detail = in.exe;
        return c;
    }
    const auto rd = [&elf](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
    const std::vector<PatchResult> res = check_patches(rd);
    std::string off;
    size_t good = 0;
    for (const PatchResult &r : res) {
        if (r.ok) {
            good++;
        } else {
            off += (off.empty() ? "" : "; ");
            off += r.feature;
        }
    }
    const int v = read_x4_version(in.dir);
    const bool other_major = v >= 0 && v / 100 != kTestedX4Version / 100;
    if (!off.empty()) {
        c.status = Status::Warn;
        c.title = "X4 build not supported yet";
        c.detail = "These turn off: " + off + ".";
        c.fix = "details";
    } else if (other_major) {
        c.status = Status::Warn;
        c.title = "Not tested with X4 " + format_x4_version(v);
        c.detail = "Every patch still fits, but this release was tested with X4 " +
                   format_x4_version(kTestedX4Version) + ".";
    } else {
        c.title = "The mod fits this X4";
        c.detail = "all " + std::to_string(good) + " features match its code";
    }
    return c;
}

std::string wivrn_program() {
    for (const char *name : {"wivrn-dashboard", "wivrn-server"}) {
        const std::string p = find_on_path(name);
        if (!p.empty())
            return p;
    }
    return "";
}

static void no_runtime(Check &c) {
    c.title = "No VR runtime running";
    c.detail = wivrn_program().empty()
                   ? "For a standalone headset we suggest WiVRn: install it. A headset wired to "
                     "the PC uses SteamVR: start that from Steam."
                   : "Start WiVRn, which we suggest. SteamVR works too.";
    c.fix = "start-vr";
}

static Check check_vr_row(const std::string &home, XrProbe *xr_out) {
    Check c;
    c.id = "vr";
    c.status = Status::Block;
    const ActiveRuntime rt = active_runtime(home);
    if (rt.path.empty()) {
        no_runtime(c);
        return c;
    }
    // Asking a SteamVR that isn't running starts its vrserver (SteamVR's log,
    // 2026-11-05: "Starting vrserver process"; with no headset it refused us and was
    // killed 5 s later, so no window showed). A check that runs by itself mustn't do
    // that. `x4vrmod probe xr` always asks.
    if (rt.steamvr && !steamvr_is_running()) {
        // WiVRn is what we suggest, so with it installed that's the button.
        if (!wivrn_program().empty()) {
            c.title = "No VR runtime running";
            c.detail = "Start WiVRn, which we suggest. It takes over from SteamVR while it runs.";
            c.fix = "start-vr";
        } else {
            c.title = "SteamVR isn't running";
            c.detail = "It's the active VR runtime. Start it from Steam. For a standalone "
                       "headset, we suggest WiVRn instead.";
            c.fix = "start-steamvr";
        }
        return c;
    }
    const XrProbe p = xr_probe_child(5000);
    if (xr_out)
        *xr_out = p;
    return vr_row(rt, p);
}

Check vr_row(const ActiveRuntime &rt, const XrProbe &p) {
    Check c;
    c.id = "vr";
    c.status = Status::Block;
    const std::string who = !p.runtime.empty() ? p.runtime
                            : !rt.name.empty() ? rt.name
                                               : "The VR runtime";
    switch (p.status) {
    case XrStatus::Ready:
        c.status = Status::Ok;
        c.title = "VR ready";
        c.detail = who + ", " + (p.system.empty() ? "headset" : p.system) + ", asks " +
                   std::to_string(p.eye_w) + " x " + std::to_string(p.eye_h) + " per eye";
        break;
    case XrStatus::NoHeadset:
        c.title = "Headset not connected";
        c.detail = who + " is running, but no headset has joined it.";
        c.fix = "retry";
        break;
    case XrStatus::NoRuntime:
        no_runtime(c);
        break;
    case XrStatus::NoLoader:
        c.title = "OpenXR isn't installed";
        c.detail = "Install the OpenXR loader from your package manager.";
        break;
    case XrStatus::TimedOut:
        // WiVRn doesn't say "no headset": it answers nothing until one connects
        // (2026-11-05: 5 s of silence without, at once with).
        if (who.find("WiVRn") != std::string::npos) {
            c.title = "Headset not connected";
            c.detail = "WiVRn is running but gives no answer, as it does until a headset "
                       "connects. Connect from the WiVRn app in the headset.";
        } else {
            c.title = "VR runtime not answering";
            c.detail = who + " didn't answer in time.";
        }
        c.fix = "retry";
        break;
    default:
        c.title = "VR runtime not answering";
        c.detail = who + ": " + p.detail;
        c.fix = "retry";
        break;
    }
    return c;
}

static Check check_layer_row(const Layout &l) {
    Check c;
    c.id = "layer";
    c.status = Status::Block;
    c.fix = "install";
    c.title = "Vulkan layer not installed";
    const std::string t = read_text_file(l.layer_manifest, 64 * 1024);
    if (t.empty()) {
        c.detail = "No manifest at " + l.layer_manifest;
        return c;
    }
    const auto libs = json_strings(t, "library_path");
    std::string lib = libs.empty() ? "" : libs.front();
    if (!lib.empty() && lib[0] != '/')
        lib = dir_name(l.layer_manifest) + "/" + lib;
    if (lib.empty() || !is_file(lib)) {
        c.detail = "The manifest names a library that isn't there: " + lib;
        return c;
    }
    if (!is_file(l.injector)) {
        c.detail = "The injector isn't there: " + l.injector;
        return c;
    }
    const std::string lv = library_version(lib), iv = library_version(l.injector);
    if (lv != own_version() || iv != own_version()) {
        c.title = "Vulkan layer out of date";
        c.detail = "Layer " + (lv.empty() ? std::string("unknown") : lv) + ", injector " +
                   (iv.empty() ? std::string("unknown") : iv) + ", this launcher " +
                   own_version() + ".";
        return c;
    }
    c.status = Status::Ok;
    c.fix.clear();
    c.title = "Vulkan layer installed";
    c.detail = lv;
    return c;
}

static Check check_mod_row(const Layout &l, const Install &in) {
    Check c;
    c.id = "mod";
    c.status = Status::Block;
    c.fix = "install";
    const std::string dest = in.dir + "/extensions/x4vrmod";
    if (!is_dir(dest)) {
        c.title = "Mod not installed";
        c.detail = "The x4vrmod extension isn't in X4's folder.";
        return c;
    }
    if (!same_tree(l.extension_dir, dest)) {
        c.title = "Mod out of date";
        c.detail = "The x4vrmod extension differs from this release's.";
        return c;
    }
    for (const char *old : {"x4vrmod_lookaround", "x4vrmod_nosway", "x4vrmod_headlook"})
        if (is_dir(in.dir + "/extensions/" + old)) {
            c.title = "Old mod files present";
            c.detail = std::string("The retired extension ") + old + " is still in X4's folder.";
            return c;
        }
    c.status = Status::Ok;
    c.fix.clear();
    c.title = "Mod installed";
    c.detail = "x4vrmod extension, up to date";
    return c;
}

static Check check_gamescope_row() {
    Check c;
    c.id = "gamescope";
    const std::string gs = find_on_path("gamescope");
    if (gs.empty()) {
        c.status = Status::Block;
        c.title = "gamescope isn't installed";
        c.detail = "Install it from your package manager.";
        return c;
    }
    c.title = "gamescope installed";
    const ChildResult r = run_child({gs, "--version"}, 2000, true);
    static const char kV[] = "gamescope version ";
    const size_t at = r.out.find(kV);
    if (at != std::string::npos) {
        const size_t s = at + sizeof kV - 1;
        c.detail = r.out.substr(s, r.out.find_first_of(" \n", s) - s);
    }
    return c;
}

static Check check_gpu_row() {
    Check c;
    c.id = "gpu";
    const VulkanProbe p = vulkan_probe_child(8000);
    if (!p.ok) {
        c.status = Status::Block;
        c.title = "Vulkan not working";
        c.detail = p.detail;
        return c;
    }
    const GpuInfo *best = nullptr;
    for (const GpuInfo &g : p.gpus)
        if (g.capable() && (!best || (g.discrete && !best->discrete)))
            best = &g;
    if (!best) {
        c.status = Status::Block;
        c.title = "No GPU can run the mod";
        c.detail = "It needs Vulkan 1.2 with multiview and at least 7 descriptor sets.";
        return c;
    }
    c.title = "GPU capable";
    c.detail = best->name + ", Vulkan " + std::to_string(best->api_major) + "." +
               std::to_string(best->api_minor);
    return c;
}

std::vector<Check> run_checks(const CheckOptions &o) {
    const std::string home = o.home.empty() ? home_dir() : o.home;
    std::vector<Check> out;

    bool have = false;
    const Install in = choose_install(home, o.game, &have);
    Check x4;
    x4.id = "x4";
    x4.status = Status::Block;
    x4.fix = "browse";
    if (!have) {
        std::string want = o.game;
        if (const char *e = getenv("X4VR_GAME"); want.empty() && e)
            want = e;
        if (want.empty())
            want = load_profile(profile_path(home)).get("game");
        if (!want.empty()) {
            x4.title = "X4 not found there";
            x4.detail = classify_game_path(want).why;
        } else {
            x4.title = "X4 not found";
            x4.detail = "It isn't in Steam's libraries or the usual GOG folders. "
                        "Choose its folder.";
        }
    }
    const bool usable = have && in.kind == PathKind::Linux;
    if (have && !usable) {
        x4.title = "This is the Windows version of X4";
        x4.detail = proton_explanation();
    }
    if (usable) {
        const int v = read_x4_version(in.dir);
        x4.status = Status::Ok;
        x4.fix.clear();
        x4.title = "X4 found";
        x4.detail = std::string(store_name(in.store)) + ", X4 " + format_x4_version(v) +
                    (in.buildid.empty() ? "" : ", build " + in.buildid) + ", " + in.dir;
    }
    out.push_back(x4);

    if (usable)
        out.push_back(check_patches_row(in));

    if (usable && binary_needs_steam(in.exe)) {
        Check c;
        c.id = "steam";
        if (steam_is_running(home)) {
            c.title = "Steam running";
        } else {
            c.status = Status::Block;
            c.title = "Steam isn't running";
            c.detail = "The Steam version of X4 needs Steam open.";
            c.fix = "start-steam";
        }
        out.push_back(c);
    }

    {
        Check c;
        c.id = "running";
        if (x4_is_running(usable ? in.exe : "")) {
            c.status = Status::Block;
            c.title = "X4 is already running";
            c.detail = "Close it first.";
        } else {
            c.title = "X4 not running yet";
        }
        out.push_back(c);
    }

    out.push_back(check_gamescope_row());
    if (!o.skip_vr)
        out.push_back(check_vr_row(home, o.xr_out));

    const Layout l = detect_layout(home);
    out.push_back(check_layer_row(l));
    if (usable)
        out.push_back(check_mod_row(l, in));
    if (!o.skip_gpu)
        out.push_back(check_gpu_row());
    return out;
}

} // namespace x4vr::launcher
