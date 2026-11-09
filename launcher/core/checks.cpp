// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The checks behind the Play button, one per row of the window and one per line
// of `x4vrmod doctor`. Everything here only reads.
#include "x4vr_i18n.hpp"
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
        l.locale_dir = zip + "/locale";
        return l;
    }
    if (here == X4VRMOD_BUILD_DIR "/launcher" || here == X4VRMOD_BUILD_DIR "/tests") {
        l.kind = LayoutKind::Dev;
        l.data_dir = X4VRMOD_SOURCE_DIR;
        l.extension_dir = X4VRMOD_BUILD_DIR "/extension/x4vrmod";
        l.layer_manifest = X4VRMOD_BUILD_DIR "/layer/VK_LAYER_X4VR_core.json";
        l.injector = X4VRMOD_BUILD_DIR "/injector/libx4vr_inject.so";
        l.launch_script = X4VRMOD_SOURCE_DIR "/launch/x4vr-launch.sh";
        l.deploy_script = X4VRMOD_SOURCE_DIR "/launch/deploy-extensions.sh";
        l.help_dir = X4VRMOD_SOURCE_DIR "/launcher/help";
        l.locale_dir = X4VRMOD_BUILD_DIR "/locale";
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
    l.locale_dir = l.data_dir + "/locale";
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
        c.title = tr_("Can't read X4's program");
        c.detail = in.exe;
        return c;
    }
    const std::vector<PatchResult> res = check_patches(elf);
    std::string off;
    size_t good = 0;
    for (const PatchResult &r : res) {
        if (r.ok) {
            good++;
        } else {
            off += (off.empty() ? "" : "; ");
            off += tr_(r.feature);
        }
    }
    const int v = read_x4_version(in.dir);
    const bool other_major = v >= 0 && v / 100 != kTestedX4Version / 100;
    if (!off.empty()) {
        c.status = Status::Warn;
        c.title = tr_("X4 build not supported yet");
        c.detail = fmt(tr_("These turn off: %1."), {off});
        c.fix = "details";
    } else if (other_major) {
        c.status = Status::Warn;
        c.title = fmt(tr_("Not tested with X4 %1"), {format_x4_version(v)});
        c.detail = fmt(tr_("Every patch still fits, but this release was tested with X4 %1."),
                       {format_x4_version(kTestedX4Version)});
    } else {
        c.title = tr_("The mod fits this X4");
        c.detail = fmt(trn_("all %1 feature matches its code", "all %1 features match its code", good),
                       {std::to_string(good)});
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

// The window's button says the rest: WiVRn first, then SteamVR.
static void no_runtime(Check &c) {
    c.title = tr_("No VR runtime running");
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
        no_runtime(c);
        return c;
    }
    const XrProbe p = xr_probe_child(5000);
    if (xr_out)
        *xr_out = p;
    return vr_row(rt, p);
}

// What the player knows a runtime as: "SteamVR/OpenXR in Meta compatibility mode" is SteamVR,
// "WiVRn 'ddcadff1'" is WiVRn.
static std::string runtime_short_name(const std::string &name) {
    for (const char *known : {"SteamVR", "WiVRn", "Monado", "ALVR"})
        if (name.find(known) != std::string::npos)
            return known;
    return name;
}

Check vr_row(const ActiveRuntime &rt, const XrProbe &p) {
    Check c;
    c.id = "vr";
    c.status = Status::Block;
    const std::string who = runtime_short_name(!p.runtime.empty() ? p.runtime
                                               : !rt.name.empty() ? rt.name
                                                                  : tr_("The VR runtime"));
    const bool steamvr = rt.steamvr || who == "SteamVR";
    switch (p.status) {
    case XrStatus::Ready: {
        c.status = Status::Ok;
        c.title = tr_("VR ready");
        // WiVRn names the headset ("Meta Quest 3 on WiVRn"); SteamVR names its driver
        // ("SteamVR/OpenXR : oculus"), which tells the player nothing.
        const bool named = !p.system.empty() && p.system.find("SteamVR") == std::string::npos;
        const std::string w = std::to_string(p.eye_w), h = std::to_string(p.eye_h);
        c.detail = named ? fmt(tr_("%1, %2. It recommends %3 x %4 per eye."), {who, p.system, w, h})
                         : fmt(tr_("%1, headset connected. It recommends %2 x %3 per eye."),
                               {who, w, h});
        break;
    }
    case XrStatus::NoHeadset:
        c.title = tr_("Headset not connected");
        c.detail = fmt(tr_("%1 is running, with no headset connected."), {who});
        c.fix = "retry";
        break;
    case XrStatus::NoRuntime:
        no_runtime(c);
        break;
    case XrStatus::NoLoader:
        c.title = tr_("OpenXR isn't installed");
        c.detail = tr_("Install the OpenXR loader from your package manager.");
        break;
    case XrStatus::TimedOut:
        // WiVRn doesn't say "no headset": it answers nothing until one connects
        // (2026-11-05: 5 s of silence without, at once with).
        if (who.find("WiVRn") != std::string::npos) {
            c.title = tr_("Headset not connected");
            c.detail = tr_("Connect from the WiVRn app in the headset.");
        } else {
            c.title = tr_("VR runtime not answering");
            c.detail = fmt(tr_("%1 didn't answer in time."), {who});
        }
        c.fix = "retry";
        break;
    default:
        // SteamVR waiting for its headset refuses an instance with XR_ERROR_RUNTIME_FAILURE
        // (2026-11-08, Steam Link before the Quest connected).
        if (steamvr && p.status == XrStatus::Failed &&
            p.detail.find("XR_ERROR_RUNTIME_FAILURE") != std::string::npos) {
            c.title = tr_("Headset not connected");
            c.detail = tr_("SteamVR is running and waiting for the headset. Connect it, then Retry.");
            c.fix = "retry";
            break;
        }
        c.title = tr_("VR runtime not answering");
        c.detail = fmt(tr_("%1 didn't start: %2"), {who, p.detail});
        c.fix = "retry";
        break;
    }
    return c;
}

// One row, because to the player it's one thing with one button: Install puts all of it in
// place.
static Check check_mod_row(const Layout &l, const Install &in) {
    Check c;
    c.id = "mod";
    c.status = Status::Block;
    c.fix = "install";
    c.title = tr_("Mod not installed");
    const std::string dest = in.dir + "/extensions/x4vrmod";
    const std::string t = read_text_file(l.layer_manifest, 64 * 1024);
    if (t.empty()) {
        // Nothing yet is the usual first start, and needs no explaining.
        if (is_dir(dest))
            c.detail = fmt(tr_("No Vulkan layer manifest at %1"), {l.layer_manifest});
        return c;
    }
    const auto libs = json_strings(t, "library_path");
    std::string lib = libs.empty() ? "" : libs.front();
    if (!lib.empty() && lib[0] != '/')
        lib = dir_name(l.layer_manifest) + "/" + lib;
    if (lib.empty() || !is_file(lib)) {
        c.detail = fmt(tr_("The Vulkan layer's manifest names a library that isn't there: %1"), {lib});
        return c;
    }
    if (!is_file(l.injector)) {
        c.detail = fmt(tr_("The injector isn't there: %1"), {l.injector});
        return c;
    }
    const std::string lv = library_version(lib), iv = library_version(l.injector);
    if (lv != own_version() || iv != own_version()) {
        c.title = tr_("Mod out of date");
        const std::string unknown = tr_("unknown");
        c.detail = fmt(tr_("Layer %1, injector %2, this launcher %3."),
                       {lv.empty() ? unknown : lv, iv.empty() ? unknown : iv, own_version()});
        return c;
    }
    if (!is_dir(dest)) {
        c.detail = tr_("The x4vrmod extension isn't in X4's folder.");
        return c;
    }
    // Installed once, the launch itself brings the extension up to date and removes the
    // retired names, so neither is something to stop for. Every build differs, by its
    // loading screen.
    c.status = Status::Ok;
    c.fix.clear();
    c.title = tr_("Mod installed");
    bool current = same_tree(l.extension_dir, dest);
    for (const char *old : {"x4vrmod_lookaround", "x4vrmod_nosway", "x4vrmod_headlook"})
        current = current && !is_dir(in.dir + "/extensions/" + old);
    c.detail = fmt(current ? tr_("%1, up to date") : tr_("%1, updates when you play"), {lv});
    return c;
}

static Check check_gamescope_row() {
    Check c;
    c.id = "gamescope";
    const std::string gs = find_on_path("gamescope");
    if (gs.empty()) {
        c.status = Status::Block;
        c.title = tr_("gamescope isn't installed");
        c.detail = tr_("Install it from your package manager.");
        return c;
    }
    c.title = tr_("gamescope installed");
    const ChildResult r = run_child({gs, "--version"}, 2000, true);
    static const char kV[] = "gamescope version ";
    const size_t at = r.out.find(kV);
    if (at != std::string::npos) {
        const size_t s = at + sizeof kV - 1;
        c.detail = r.out.substr(s, r.out.find_first_of(" \n", s) - s);
    }
    return c;
}

const GpuInfo *best_gpu(const VulkanProbe &p) {
    const GpuInfo *best = nullptr;
    for (const GpuInfo &g : p.gpus)
        if (g.capable() && (!best || (g.discrete && !best->discrete)))
            best = &g;
    return best;
}

bool GpuInfo::rtx() const { return vendor == 0x10de && name.find("RTX") != std::string::npos; }

static Check check_gpu_row() {
    Check c;
    c.id = "gpu";
    const VulkanProbe p = vulkan_probe_child(8000);
    if (!p.ok) {
        c.status = Status::Block;
        c.title = tr_("Vulkan not working");
        c.detail = p.detail;
        return c;
    }
    const GpuInfo *best = best_gpu(p);
    if (!best) {
        c.status = Status::Block;
        c.title = tr_("No GPU can run the mod");
        c.detail = tr_("It needs Vulkan 1.2 with multiview and at least 7 descriptor sets.");
        return c;
    }
    c.title = tr_("GPU capable");
    c.detail = fmt(tr_("%1, Vulkan %2"),
                   {best->name, std::to_string(best->api_major) + "." + std::to_string(best->api_minor)});
    c.dlss = best->rtx();
    if (c.dlss)
        c.detail = fmt(tr_("%1, DLSS available"), {c.detail});
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
            x4.title = tr_("X4 not found there");
            x4.detail = classify_game_path(want).why;
        } else {
            x4.title = tr_("X4 not found");
            x4.detail = tr_("It isn't in Steam's libraries or the usual GOG folders. "
                            "Choose its folder.");
        }
    }
    const bool usable = have && in.kind == PathKind::Linux;
    if (have && !usable) {
        x4.title = tr_("This is the Windows version of X4");
        x4.detail = proton_explanation();
    }
    if (usable) {
        const int v = read_x4_version(in.dir);
        x4.status = Status::Ok;
        x4.fix.clear();
        x4.title = tr_("X4 found");
        // The window cuts the row at ", /": the Game box already shows the folder.
        x4.detail = (in.buildid.empty()
                         ? fmt(tr_("%1, X4 %2"), {store_name(in.store), format_x4_version(v)})
                         : fmt(tr_("%1, X4 %2, build %3"),
                               {store_name(in.store), format_x4_version(v), in.buildid})) +
                    ", " + in.dir;
    }
    if (usable && in.flatpak_steam) {
        // Its sandbox can't reach the mod's parts or gamescope, and VR in it is limited for
        // every game. Not planned for now (docs/packaging-plan.md, "Steam's Flatpak").
        x4.status = Status::Block;
        x4.title = tr_("Steam's Flatpak isn't supported");
        x4.detail = tr_("This X4 is Steam's Flatpak copy, and the mod can't reach into that "
                        "sandbox. Use Steam from your distribution's packages, or a GOG copy.");
        x4.fix = "help-flatpak";
    }
    out.push_back(x4);

    if (usable)
        out.push_back(check_patches_row(in));

    if (usable && binary_needs_steam(in.exe)) {
        Check c;
        c.id = "steam";
        if (steam_is_running(home)) {
            c.title = tr_("Steam running");
        } else {
            c.status = Status::Block;
            c.title = tr_("Steam isn't running");
            c.detail = tr_("The Steam version of X4 needs Steam open.");
            c.fix = "start-steam";
        }
        out.push_back(c);
    }

    {
        Check c;
        c.id = "running";
        if (x4_is_running(usable ? in.exe : "")) {
            c.status = Status::Block;
            c.title = tr_("X4 is already running");
            c.detail = tr_("Close it first.");
        } else {
            c.title = tr_("X4 not running yet");
        }
        out.push_back(c);
    }

    out.push_back(check_gamescope_row());
    if (!o.skip_vr)
        out.push_back(check_vr_row(home, o.xr_out));

    const Layout l = detect_layout(home);
    if (usable)
        out.push_back(check_mod_row(l, in));
    if (!o.skip_gpu)
        out.push_back(check_gpu_row());
    return out;
}

} // namespace x4vr::launcher
