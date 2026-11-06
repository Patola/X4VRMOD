// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The launcher's core, without Steam, a headset or a game: Valve's text format,
// telling paths apart, discovery under a fake home, the ELF reader on a built
// file, children that hang, the probes' lines. Against the real X4 when there is
// one, every feature's check must fail when its own bytes are bent. Self-checking.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../common/x4vr_steer.hpp"
#include "../injector/x4vr_aimpick.hpp"
#include "../injector/x4vr_camwrite.hpp"
#include "../injector/x4vr_csmfit.hpp"
#include "../injector/x4vr_extview.hpp"
#include "../injector/x4vr_foot.hpp"
#include "../injector/x4vr_loadart.hpp"
#include "../injector/x4vr_menus.hpp"
#include "../injector/x4vr_ngx.hpp"
#include "../injector/x4vr_uimouse.hpp"
#include "x4vr_launcher.hpp"

using namespace x4vr::launcher;

static int g_fail = 0;
static void check(bool ok, const std::string &what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    if (!ok)
        g_fail++;
}

static std::string g_tmp;

static void mkdirs(const std::string &p) {
    std::string cur;
    size_t i = 0;
    while (i != std::string::npos) {
        i = p.find('/', i + 1);
        cur = p.substr(0, i);
        mkdir(cur.c_str(), 0755);
    }
}

static void put(const std::string &path, const std::string &bytes) {
    mkdirs(dir_name(path));
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }
}

template <class T> static void pod(std::string &s, size_t at, T v) {
    memcpy(&s[at], &v, sizeof v);
}

// A small but real ELF64: one LOAD over the whole file, one DYNAMIC naming `lib`.
// Enough for what the launcher reads: magic, segments and DT_NEEDED.
static std::string make_elf(const std::string &lib, uint64_t va = 0x400000) {
    const size_t ph = 64, dyn = ph + 2 * 56, str = dyn + 3 * 16;
    std::string s(str + 1 + lib.size() + 1 + 32, '\0');
    memcpy(&s[0], "\x7f" "ELF", 4);
    s[4] = 2, s[5] = 1, s[6] = 1;
    pod<uint16_t>(s, 16, 2);  // ET_EXEC
    pod<uint16_t>(s, 18, 62); // x86-64
    pod<uint64_t>(s, 0x20, ph);
    pod<uint16_t>(s, 0x36, 56);
    pod<uint16_t>(s, 0x38, 2);
    pod<uint32_t>(s, ph, 1); // PT_LOAD
    pod<uint64_t>(s, ph + 8, 0);
    pod<uint64_t>(s, ph + 16, va);
    pod<uint64_t>(s, ph + 32, s.size());
    pod<uint32_t>(s, ph + 56, 2); // PT_DYNAMIC
    pod<uint64_t>(s, ph + 56 + 8, dyn);
    pod<uint64_t>(s, ph + 56 + 16, va + dyn);
    pod<uint64_t>(s, ph + 56 + 32, 3 * 16);
    pod<uint64_t>(s, dyn, 1); // DT_NEEDED, offset 1 in the string table
    pod<uint64_t>(s, dyn + 8, 1);
    pod<uint64_t>(s, dyn + 16, 5); // DT_STRTAB
    pod<uint64_t>(s, dyn + 24, va + str);
    memcpy(&s[str + 1], lib.c_str(), lib.size());
    return s;
}

static void test_vdf() {
    VdfNode r;
    const char *libs = R"(// a comment
"libraryfolders"
{
	"0"
	{
		"path"		"/home/p/.local/share/Steam"
		"apps"
		{
			"228980"		"1"
		}
	}
	"1"
	{
		"PATH"		"/nvme/My \"Odd\" Library"
	}
}
)";
    check(parse_vdf(libs, &r), "vdf: libraryfolders parses");
    const VdfNode *lf = r.find("LibraryFolders");
    check(lf && lf->children.size() == 2, "vdf: two libraries, key case ignored");
    check(lf && lf->children[0].get("path") == "/home/p/.local/share/Steam",
          "vdf: a value");
    check(lf && lf->children[1].get("path") == "/nvme/My \"Odd\" Library",
          "vdf: an escaped quote, and a key in another case");
    check(lf && lf->children[0].get("apps").empty() && lf->children[0].find("apps"),
          "vdf: a block is not a value");
    check(!parse_vdf("\"a\" { \"b\" \"c\"", &r), "vdf: an unclosed block is refused");
    check(!parse_vdf("\"a\" \"unterminated", &r), "vdf: an unterminated string is refused");
    check(!parse_vdf("}", &r), "vdf: a stray brace is refused");
    check(parse_vdf("", &r) && r.children.empty(), "vdf: an empty file is empty");
}

static void test_classify() {
    const std::string d = g_tmp + "/classify";
    const std::string elf = make_elf("libc.so.6");
    put(d + "/plain/X4", elf);
    put(d + "/gog/game/X4", elf);
    put(d + "/both/X4", elf);
    put(d + "/both/X4.exe", "MZ\x90");
    put(d + "/proton/X4.exe", "MZ\x90");
    put(d + "/text/X4", "#!/bin/sh\n");
    put(d + "/other/mygame", elf);
    std::string elf32 = elf;
    elf32[4] = 1;
    put(d + "/bits/X4", elf32);
    mkdirs(d + "/empty");

    auto kind = [](const std::string &p) { return classify_game_path(p).kind; };
    check(kind(d + "/plain") == PathKind::Linux, "classify: a folder with X4");
    check(classify_game_path(d + "/gog").exe == d + "/gog/game/X4",
          "classify: a folder with game/X4");
    check(classify_game_path(d + "/gog").dir == d + "/gog/game",
          "classify: the game's folder is the binary's");
    check(kind(d + "/plain/X4") == PathKind::Linux, "classify: the program itself");
    check(kind(d + "/both") == PathKind::Linux, "classify: Linux wins when both are there");
    check(kind(d + "/proton") == PathKind::Windows, "classify: a folder with only X4.exe");
    check(kind(d + "/proton/X4.exe") == PathKind::Windows, "classify: X4.exe itself");
    check(kind(d + "/text") == PathKind::NotX4, "classify: a script called X4 is not X4");
    check(kind(d + "/bits") == PathKind::NotX4, "classify: a 32-bit X4 is not X4");
    check(kind(d + "/other/mygame") == PathKind::NotX4,
          "classify: an ELF with another name is not X4");
    check(kind(d + "/empty") == PathKind::NotX4, "classify: an empty folder");
    check(kind(d + "/nowhere") == PathKind::Missing, "classify: a missing path");
    check(kind("") == PathKind::Missing, "classify: no path");
}

static std::string library_vdf(const std::vector<std::string> &paths) {
    std::string s = "\"libraryfolders\"\n{\n";
    int i = 0;
    for (const std::string &p : paths)
        s += "\"" + std::to_string(i++) + "\" { \"path\" \"" + p + "\" }\n";
    return s + "}\n";
}

static std::string manifest(const std::string &dir, const std::string &build) {
    return "\"AppState\"\n{\n\"appid\" \"392160\"\n\"installdir\" \"" + dir +
           "\"\n\"buildid\" \"" + build + "\"\n}\n";
}

static void test_discover() {
    const std::string elf = make_elf("libsteam_api.so");
    {
        const std::string h = g_tmp + "/h1";
        const std::string root = h + "/.local/share/Steam";
        const std::string lib = h + "/drive/SteamLibrary";
        const std::string other = h + "/other/SteamLibrary";
        put(root + "/steamapps/libraryfolders.vdf", library_vdf({root, other, lib}));
        mkdirs(other + "/steamapps");
        put(lib + "/steamapps/appmanifest_392160.acf", manifest("X4 Foundations", "23660954"));
        put(lib + "/steamapps/common/X4 Foundations/X4", elf);
        const auto all = discover_steam(h);
        check(all.size() == 1, "steam: found once, in the second of three libraries");
        if (!all.empty()) {
            check(all[0].exe == lib + "/steamapps/common/X4 Foundations/X4", "steam: the path");
            check(all[0].store == Store::Steam && all[0].kind == PathKind::Linux,
                  "steam: a Linux Steam copy");
            check(all[0].buildid == "23660954", "steam: the build id");
        }
    }
    {
        const std::string h = g_tmp + "/h2";
        const std::string root = h + "/.local/share/Steam";
        mkdirs(root + "/steamapps");
        put(root + "/steamapps/appmanifest_392160.acf", manifest("X4 Foundations", "1"));
        check(discover_steam(h).empty(), "steam: a manifest with no game is ignored");
        put(root + "/steamapps/common/X4 Foundations/X4.exe", "MZ");
        const auto all = discover_steam(h);
        check(all.size() == 1 && all[0].kind == PathKind::Windows,
              "steam: forced Proton is reported, not ignored");
        check(preferred_install(all) == &all[0],
              "preferred: a Windows copy is offered when it's all there is");
    }
    check(discover_steam(g_tmp + "/nobody").empty(), "steam: no Steam at all");
    {
        const std::string h = g_tmp + "/h3";
        put(h + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/"
                "appmanifest_392160.acf",
            manifest("X4", "7"));
        put(h + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/X4/X4", elf);
        check(discover_steam(h).size() == 1, "steam: Flatpak Steam");
    }
    {
        const std::string h = g_tmp + "/h4";
        put(h + "/GOG Games/X4 Foundations/game/X4", make_elf("libc.so.6"));
        put(h + "/GOG Games/Other Game/X4", make_elf("libc.so.6"));
        const std::string where = h + "/lutris/x4";
        put(where + "/X4", make_elf("libc.so.6"));
        put(h + "/.config/heroic/gog_store/installed.json",
            "{\"installed\":[{\"install_path\": \"" + where +
                "\"},{\"install_path\": \"/nonexistent\"}]}");
        const auto all = discover_gog(h);
        check(all.size() == 2, "gog: the usual folder and Heroic's list, nothing else");
        bool store = true;
        for (const Install &i : all)
            store = store && i.store == Store::Gog;
        check(store, "gog: marked GOG");
    }
    {
        const std::string h = g_tmp + "/h5";
        std::vector<Install> v;
        Install w, l;
        w.kind = PathKind::Windows;
        l.kind = PathKind::Linux;
        v.push_back(w);
        v.push_back(l);
        check(preferred_install(v) == &v[1], "preferred: Linux over an earlier Windows copy");
        v.clear();
        check(preferred_install(v) == nullptr, "preferred: nothing found");
    }
}

static void test_version() {
    const std::string d = g_tmp + "/ver";
    put(d + "/a/version.dat", "900\r\n");
    put(d + "/b/version.dat", "not a number");
    check(read_x4_version(d + "/a") == 900, "version: version.dat");
    check(read_x4_version(d + "/b") == -1, "version: garbage");
    check(read_x4_version(d + "/c") == -1, "version: missing");
    check(format_x4_version(900) == "9.00", "version: 900 is 9.00");
    check(format_x4_version(1105) == "11.05", "version: 1105 is 11.05");
    check(format_x4_version(-1) == "unknown", "version: unknown");
}

static void test_elf() {
    const std::string d = g_tmp + "/elf";
    put(d + "/steam", make_elf("libsteam_api.so"));
    put(d + "/plain", make_elf("libSDL3.so.0"));
    put(d + "/text", std::string(200, 'x'));
    put(d + "/short", "\x7f" "ELF");
    check(binary_needs_steam(d + "/steam"), "elf: a binary linking libsteam_api needs Steam");
    check(!binary_needs_steam(d + "/plain"), "elf: one that doesn't, doesn't");
    check(!binary_needs_steam(d + "/text"), "elf: not an ELF");
    check(!binary_needs_steam(d + "/short"), "elf: a truncated file");
    check(!binary_needs_steam(d + "/missing"), "elf: no file");

    ElfFile e;
    check(e.open(d + "/plain"), "elf: opens");
    const auto n = e.needed();
    check(n.size() == 1 && n[0] == "libSDL3.so.0", "elf: DT_NEEDED");
    uint8_t b[4] = {};
    check(e.read(0x400000, b, 4) && memcmp(b, "\x7f" "ELF", 4) == 0, "elf: a VA maps to its bytes");
    check(!e.read(0x3fffff, b, 1), "elf: below the segment");
    const size_t size = make_elf("libSDL3.so.0").size();
    check(e.read(0x400000 + size - 1, b, 1), "elf: the last byte");
    check(!e.read(0x400000 + size - 1, b, 2), "elf: a read running off the end");
    check(!e.read(0x400000 + size, b, 1), "elf: past the end");

    // Headers that point outside the file must not crash the launcher: the
    // player can point Browse at anything.
    std::string bad = make_elf("libc.so.6");
    pod<uint64_t>(bad, 0x20, 0xffffffffffffff00ull);
    put(d + "/badph", bad);
    ElfFile b1;
    check(b1.open(d + "/badph") && !b1.read(0x400000, b, 1) && b1.needed().empty(),
          "elf: a program-header offset past the file");
    bad = make_elf("libc.so.6");
    pod<uint16_t>(bad, 0x38, 0xffff);
    put(d + "/manyph", bad);
    ElfFile b2;
    check(b2.open(d + "/manyph") && b2.read(0x400000, b, 1), "elf: an absurd header count");
    bad = make_elf("libc.so.6");
    pod<uint64_t>(bad, 64 + 56 + 8, 0xfffffffffffffff0ull);
    put(d + "/baddyn", bad);
    ElfFile b3;
    check(b3.open(d + "/baddyn") && b3.needed().empty(), "elf: a dynamic section past the file");
    bad = make_elf("libc.so.6");
    pod<uint64_t>(bad, 64 + 8, 0xfffffffffffffff0ull);
    put(d + "/badload", bad);
    ElfFile b4;
    check(b4.open(d + "/badload") && !b4.read(0x400000, b, 1), "elf: a segment offset past the file");
}

static void test_child() {
    ChildResult r = run_child({"/bin/sh", "-c", "echo out; echo err >&2; exit 3"}, 5000);
    check(r.started && !r.timed_out && r.exit_code == 3, "child: exit code");
    check(r.out == "out\n", "child: stdout only");
    r = run_child({"/bin/sh", "-c", "echo out; echo err >&2"}, 5000, true);
    check(r.out.find("err") != std::string::npos, "child: stderr when asked");
    r = run_child({"/bin/sh", "-c", "echo $X4VRMOD_T"}, 5000, false, {"X4VRMOD_T=hello"});
    check(r.out == "hello\n", "child: extra environment");
    r = run_child({g_tmp + "/no-such-program"}, 5000);
    check(r.exit_code == 127, "child: a missing program");

    const auto t0 = std::chrono::steady_clock::now();
    r = run_child({"/bin/sh", "-c", "sleep 30 & wait"}, 150);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    check(r.timed_out, "child: a hang times out");
    check(ms < 5000, "child: and doesn't wait for it");
    // The pipe stays open in a grandchild when the group isn't killed: the read
    // would then block. Closing stdout and hanging must time out as well.
    const auto t1 = std::chrono::steady_clock::now();
    r = run_child({"/bin/sh", "-c", "exec >&- 2>&-; sleep 30"}, 150);
    const auto ms1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t1).count();
    check(r.timed_out && ms1 < 5000, "child: closing stdout and hanging also times out");
}

static void test_probes_format() {
    XrProbe p;
    p.status = XrStatus::Ready;
    p.runtime = "WiVRn";
    p.system = "Quest 3";
    p.eye_w = 3096;
    p.eye_h = 3243;
    XrProbe q = xr_probe_parse("noise a runtime printed\n" + xr_probe_format(p) + "more noise\n");
    check(q.status == XrStatus::Ready && q.runtime == "WiVRn" && q.system == "Quest 3" &&
              q.eye_w == 3096 && q.eye_h == 3243,
          "xr: the lines survive a runtime printing around them");
    for (XrStatus s : {XrStatus::NoLoader, XrStatus::NoRuntime, XrStatus::NoHeadset})
        check((p.status = s, xr_probe_parse(xr_probe_format(p)).status == s),
              std::string("xr: status ") + xr_status_name(s));
    q = xr_probe_parse("");
    check(q.status == XrStatus::Failed && !q.detail.empty(),
          "xr: a probe that died says so, and isn't taken for ready");
    p.detail = "two\nlines";
    check(xr_probe_parse(xr_probe_format(p)).detail == "two lines", "xr: a value stays one line");

    VulkanProbe v;
    v.ok = true;
    GpuInfo g;
    g.name = "AMD Radeon RX 7900 XTX (RADV NAVI31)";
    g.api_major = 1, g.api_minor = 4, g.max_sets = 32, g.multiview = true, g.discrete = true;
    v.gpus.push_back(g);
    const VulkanProbe w = vulkan_probe_parse(vulkan_probe_format(v));
    check(w.ok && w.gpus.size() == 1 && w.gpus[0].name == g.name && w.gpus[0].max_sets == 32 &&
              w.gpus[0].multiview && w.gpus[0].discrete,
          "vulkan: the lines, and a name with spaces");
    check(g.capable(), "gpu: this one can");
    GpuInfo h = g;
    h.multiview = false;
    check(!h.capable(), "gpu: not without multiview");
    h = g;
    h.max_sets = 6;
    check(!h.capable(), "gpu: not with X4's own 6 sets and none for the layer");
    h.max_sets = 7;
    check(h.capable(), "gpu: 7 sets are enough");
    h = g;
    h.api_minor = 1;
    check(!h.capable(), "gpu: not on Vulkan 1.1");
}

static void test_runtime_file() {
    const std::string h = g_tmp + "/xr";
    const std::string none = g_tmp + "/xr-none";
    setenv("XDG_CONFIG_DIRS", none.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XR_RUNTIME_JSON");
    if (!is_file("/etc/openxr/1/active_runtime.json"))
        check(active_runtime(h).path.empty(), "runtime: none (WiVRn off: the file is gone)");
    put(h + "/.config/openxr/1/active_runtime.json",
        "{\"file_format_version\": \"1.0.0\", \"runtime\": {\"name\": \"WiVRn\", "
        "\"library_path\": \"/usr/lib/wivrn/libopenxr_wivrn.so\"}}");
    ActiveRuntime rt = active_runtime(h);
    check(rt.name == "WiVRn" && rt.library == "/usr/lib/wivrn/libopenxr_wivrn.so",
          "runtime: name and library");
    check(!rt.steamvr, "runtime: WiVRn isn't SteamVR");
    const std::string steam = h + "/steamxr_linux64.json";
    put(steam, "{\"runtime\": {\"name\": \"SteamVR\", \"library_path\": "
               "\"../linux64/vrclient.so\"}}");
    setenv("XR_RUNTIME_JSON", steam.c_str(), 1);
    rt = active_runtime(h);
    check(rt.path == steam && rt.steamvr, "runtime: XR_RUNTIME_JSON comes first; SteamVR known");
    unsetenv("XR_RUNTIME_JSON");
    setenv("XDG_CONFIG_HOME", (h + "/elsewhere").c_str(), 1);
    setenv("XDG_CONFIG_DIRS", (h + "/.config").c_str(), 1);
    check(active_runtime(h).name == "WiVRn", "runtime: XDG_CONFIG_DIRS is searched");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CONFIG_DIRS");
}

static void test_library_version() {
    const std::string d = g_tmp + "/lib";
    put(d + "/good.so", std::string("\0stuff\0X4VRMOD_VERSION=v0.9.9\0more", 33));
    put(d + "/none.so", "no marker here");
    put(d + "/cut.so", "X4VRMOD_VERSION=v0.9");
    check(library_version(d + "/good.so") == "v0.9.9", "library: the version from the file");
    check(library_version(d + "/none.so").empty(), "library: no marker");
    check(library_version(d + "/cut.so").empty(), "library: a cut-off marker");
    check(library_version(d + "/missing.so").empty(), "library: no file");
    check(strlen(own_version()) > 0, "library: this program knows its own");
}

static void test_layout() {
    const std::string zip = g_tmp + "/zipdata";
    put(zip + "/manifest", "x");
    setenv("X4VRMOD_DATA", zip.c_str(), 1);
    Layout l = detect_layout("/home/p");
    check(l.kind == LayoutKind::Zip, "layout: a data folder with a manifest is the zip");
    check(l.extension_dir == zip + "/extension/x4vrmod", "layout: the zip's extension");
    check(l.layer_manifest ==
              "/home/p/.local/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json",
          "layout: the zip's layer goes in the user's folder");
    setenv("X4VRMOD_DATA", (g_tmp + "/nothing").c_str(), 1);
    l = detect_layout("/home/p");
    check(l.kind == LayoutKind::Dev, "layout: without one, this test runs from the build tree");
    unsetenv("X4VRMOD_DATA");
}

static const Check *row(const std::vector<Check> &v, const char *id) {
    for (const Check &c : v)
        if (c.id == id)
            return &c;
    return nullptr;
}

static void test_checks() {
    const std::string d = g_tmp + "/chk";
    put(d + "/gog/X4", make_elf("libc.so.6"));
    put(d + "/gog/version.dat", "900\n");
    put(d + "/steam/X4", make_elf("libsteam_api.so"));
    put(d + "/proton/X4.exe", "MZ");
    CheckOptions o;
    o.home = g_tmp + "/nobody";
    o.skip_vr = true;

    o.game = d + "/gog";
    auto v = run_checks(o);
    check(row(v, "x4") && row(v, "x4")->status == Status::Ok, "checks: a Linux X4 is found");
    check(!row(v, "steam"), "checks: no Steam row without libsteam_api");
    const Check *p = row(v, "patches");
    check(p && p->status == Status::Warn, "checks: code that doesn't match is amber, not grey");
    check(p && p->detail.find("Head tracking in the cockpit") != std::string::npos,
          "checks: and names what turns off");
    check(!row(v, "vr"), "checks: --no-vr skips the VR row");

    o.game = d + "/steam";
    v = run_checks(o);
    check(row(v, "steam") != nullptr, "checks: libsteam_api brings the Steam row");

    o.game = d + "/proton";
    v = run_checks(o);
    check(row(v, "x4") && row(v, "x4")->status == Status::Block &&
              row(v, "x4")->detail == proton_explanation(),
          "checks: the Windows version blocks and explains");
    check(!row(v, "patches") && !row(v, "steam") && !row(v, "mod"),
          "checks: nothing is asked of a copy that can't run");

    o.game = d + "/nothing-here";
    v = run_checks(o);
    check(row(v, "x4") && row(v, "x4")->status == Status::Block && row(v, "x4")->fix == "browse",
          "checks: a bad path offers Browse");
    o.game.clear();
    v = run_checks(o);
    check(row(v, "x4") && row(v, "x4")->fix == "browse", "checks: nothing found offers Browse");

    std::vector<Check> w(2);
    check(worst(w) == Status::Ok, "worst: all ok is green");
    w[1].status = Status::Warn;
    check(worst(w) == Status::Warn, "worst: a warning is amber");
    w[0].status = Status::Block;
    check(worst(w) == Status::Block, "worst: a block is grey, over a warning");
}

// Each feature's check must see its own bytes bent. A group that kept passing
// would turn a broken patch into a green Play button.
// Decision 6: the rule only acts on crossing 3200, and never undoes a choice the
// player made.
static void test_eye_rule() {
    EyeSettings s;
    check(s.eye == 2816 && s.upscaler == "none" && !s.upscaler_auto, "the default: 2816, no upscaler");
    check(set_eye(s, 3200).empty() && s.upscaler == "none", "3200 is still native");
    std::string note = set_eye(s, 3201);
    check(!note.empty() && s.upscaler == kAutoUpscaler && s.upscaler_auto,
          "3201 selects FSR 1 Quality and says so");
    check(set_eye(s, 4224).empty() && s.upscaler == kAutoUpscaler && s.upscaler_auto,
          "between larger sizes nothing changes");
    set_eye(s, 3000);
    check(s.upscaler == "none" && !s.upscaler_auto, "back under 3200 an automatic choice returns to None");

    EyeSettings m;
    set_upscaler(m, "fsr3_balanced");
    set_eye(m, 4224);
    check(m.upscaler == "fsr3_balanced" && !m.upscaler_auto, "a chosen upscaler survives going large");
    set_eye(m, 2816);
    check(m.upscaler == "fsr3_balanced", "...and coming back");

    EyeSettings u;
    set_eye(u, 4224);
    set_upscaler(u, "none");
    check(u.upscaler == "none" && !u.upscaler_auto, "the player can unselect it");
    check(set_eye(u, 4000).empty() && u.upscaler == "none", "...and moving between larger sizes respects that");
    set_eye(u, 2816);
    check(u.upscaler == "none", "...and coming back");

    check(render_size(4224, "fsr_quality") == 2816, "FSR 1 Quality at 4224 renders 2816 (measured)");
    check(render_size(4224, "fsr3_balanced") == 2485, "FSR 3 Balanced at 4224 renders about 2484 (measured)");
    check(render_size(2816, "none") == 2816, "None renders the eye size");
    check(kAutoUpscaleAbove == 3200, "the threshold is Patola's 3200");
}

static void test_profile() {
    const std::string path = g_tmp + "/profile/x4vrmod/profile.toml";
    Profile p;
    check(load_profile(path).values.empty(), "no profile file reads as empty");
    check(profile_set(p, "eye", "3072").empty(), "eye 3072 is accepted");
    const std::string game = g_tmp + "/profile/where with spaces/X4";
    put(game, make_elf("libc.so.6"));
    check(profile_set(p, "game", game).empty(), "a path with spaces is accepted");
    check(profile_set(p, "game", g_tmp + "/nowhere/X4").rfind("error: ", 0) == 0,
          "a game that isn't there is refused");
    check(p.get("game") == game, "...and the one set stays");
    check(profile_set(p, "env.X4VR_FOO", "bar baz").empty(), "an extra variable is accepted");
    check(profile_set(p, "eye", "12").rfind("error: ", 0) == 0, "eye 12 is refused");
    check(profile_set(p, "eye", "4224x").rfind("error: ", 0) == 0, "eye 4224x is refused");
    check(profile_set(p, "eye", "3e3").rfind("error: ", 0) == 0, "eye 3e3 is refused");
    check(p.get("eye") == "3072", "a refusal changes nothing");
    check(profile_set(p, "upscaler", "dlss").rfind("error: ", 0) == 0, "an unknown upscaler is refused");
    check(profile_set(p, "nonsense", "1").rfind("error: ", 0) == 0, "an unknown setting is refused");
    check(profile_set(p, "lean", "0").rfind("error: ", 0) == 0, "lean is no longer a setting");
    check(profile_set(p, "env.NOT A NAME", "1").rfind("error: ", 0) == 0, "a bad variable name is refused");
    check(profile_set(p, "env.1A", "1").rfind("error: ", 0) == 0, "a name can't start with a digit");
    check(profile_set(p, "env.A=B", "1").rfind("error: ", 0) == 0, "a name can't hold an =");
    check(p.get("env.A=B").empty(), "a refused variable isn't kept");

    check(save_profile(path, p), "saving makes the folder");
    const Profile back = load_profile(path);
    check(back.get("eye") == "3072", "the eye reads back");
    check(back.get("game") == game, "a path with spaces reads back");
    check(back.get("env.X4VR_FOO") == "bar baz", "the variable reads back");

    bool h = false, foo = false, named = false;
    for (const auto &[k, v] : profile_env(back)) {
        h = h || (k == "X4VR_H" && v == "3072");
        foo = foo || (k == "X4VR_FOO" && v == "bar baz");
        named = named || k == "game" || k == "X4VR_GAME";
    }
    check(h && foo, "the profile becomes the launch script's variables");
    check(!named, "the game isn't a variable: run decides it");

    Profile q;
    profile_set(q, "eye", "4224");
    check(q.get("upscaler") == kAutoUpscaler, "through the profile, the rule applies");
    profile_set(q, "upscaler", "none");
    check(q.get("upscaler") == "none", "through the profile, it can be unselected");
    profile_set(q, "eye", "4000");
    check(q.get("upscaler") == "none", "through the profile, that choice stays");

    put(path, "# a comment\n\neye = 2816\nnot a setting\n");
    check(load_profile(path).get("eye") == "2816", "a stray line doesn't lose the rest");
}

// The row each answer gives. WiVRn's silence means no headset, anyone else's doesn't.
static void test_vr_row() {
    ActiveRuntime wivrn;
    wivrn.name = "WiVRn";
    XrProbe p;
    p.status = XrStatus::TimedOut;
    Check c = vr_row(wivrn, p);
    check(c.title == "Headset not connected" && c.status == Status::Block && c.fix == "retry",
          "WiVRn's silence reads as no headset");
    ActiveRuntime other;
    other.name = "Monado";
    c = vr_row(other, p);
    check(c.title == "VR runtime not answering", "another runtime's silence doesn't");
    p.status = XrStatus::NoHeadset;
    p.runtime = "Monado";
    c = vr_row(other, p);
    check(c.title == "Headset not connected", "the runtime saying so reads the same");
    p.status = XrStatus::Ready;
    p.system = "A Headset";
    p.eye_w = 2889;
    p.eye_h = 3026;
    c = vr_row(other, p);
    check(c.status == Status::Ok && c.detail.find("2889 x 3026") != std::string::npos,
          "ready names what the headset asks for");
}

// X4 asks XDG_CONFIG_HOME, then HOME. The folder inside is the player's own.
static void test_profile_dirs() {
    const std::string home = g_tmp + "/pd/home", xdg = g_tmp + "/pd/xdg";
    put(home + "/.config/EgoSoft/X4/1234567/config.xml", "x");
    put(home + "/.config/EgoSoft/X4/gog-someone/config.xml", "x");
    put(home + "/.config/EgoSoft/X4/.hidden/config.xml", "x");
    put(xdg + "/EgoSoft/X4/7654321/config.xml", "x");
    auto has = [](const std::vector<std::string> &v, const std::string &s) {
        for (const std::string &e : v)
            if (e == s)
                return true;
        return false;
    };
    const char *saved = getenv("XDG_CONFIG_HOME");
    const std::string keep = saved ? saved : "";
    unsetenv("XDG_CONFIG_HOME");
    std::vector<std::string> d = x4_profile_dirs(home);
    check(has(d, home + "/.config/EgoSoft/X4/1234567"), "any number counts");
    check(has(d, home + "/.config/EgoSoft/X4/gog-someone"), "any name counts");
    check(has(d, home + "/.config/EgoSoft/X4"), "X4's own folder counts");
    check(!has(d, home + "/.config/EgoSoft/X4/.hidden"), "a hidden folder doesn't");
    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
    d = x4_profile_dirs(home);
    check(has(d, xdg + "/EgoSoft/X4/7654321"), "XDG_CONFIG_HOME's folders count");
    check(has(d, home + "/.config/EgoSoft/X4/1234567"), "...and HOME's still do");
    setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);
    size_t n = 0;
    for (const std::string &e : x4_profile_dirs(home))
        n += e == home + "/.config/EgoSoft/X4/1234567";
    check(n == 1, "the same folder by both routes is listed once");
    if (saved)
        setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    check(x4_profile_dirs(g_tmp + "/pd/nobody").empty(), "no X4 folder, no list");
}

// The child's environment is built before fork now: both halves must still arrive.
static void test_child_env() {
    setenv("X4VR_TEST_KEEP", "kept", 1);
    setenv("X4VR_TEST_OVER", "old", 1);
    const ChildResult r = run_child({"sh", "-c", "echo $X4VR_TEST_KEEP $X4VR_TEST_OVER $X4VR_TEST_NEW"},
                                    5000, false, {"X4VR_TEST_OVER=new", "X4VR_TEST_NEW=added"});
    check(r.out == "kept new added\n", "a child gets the environment, replaced and added: " + r.out);
    const ChildResult e = run_child({"sh", "-c", "echo out; echo err >&2"}, 5000, false);
    check(e.out == "out\n", "stderr goes nowhere unless asked: " + e.out);
    const ChildResult m = run_child({"sh", "-c", "echo out; echo err >&2"}, 5000, true);
    check(m.out == "out\nerr\n", "...and comes along when asked: " + m.out);
    unsetenv("X4VR_TEST_KEEP");
    unsetenv("X4VR_TEST_OVER");
}

static void test_offered() {
    check(!eye_note(4000, false).empty() == false, "4000 keeps the map in depth");
    check(eye_note(4096, false).find("flat") != std::string::npos, "4096 says the map is flat");
    check(eye_note(3200, true).empty(), "SteamVR at 3200 has no warning");
    check(eye_note(3600, true).find("SteamVR") != std::string::npos, "SteamVR above 3200 has one");
    check(eye_note(3600, false).empty(), "WiVRn at 3600 doesn't");
    bool have_default = false, sorted = true;
    int last = 0;
    for (int e : eye_sizes()) {
        have_default = have_default || e == EyeSettings{}.eye;
        sorted = sorted && e > last;
        last = e;
    }
    check(have_default && sorted, "the sizes offered include the default, in order");
    bool all_valid = !upscalers().empty();
    for (const Upscaler &u : upscalers())
        all_valid = all_valid && valid_upscaler(u.mode) && u.label && *u.label;
    check(all_valid && upscalers()[0].mode == std::string("none"), "None comes first, all labelled");
    check(valid_upscaler(kAutoUpscaler), "the rule's choice is on the list");
    check(!is_dlss(kAutoUpscaler), "the rule never chooses what hasn't been played in VR");
    int dlss = 0;
    for (const Upscaler &u : upscalers())
        if (u.dlss) {
            dlss++;
            check(is_dlss(u.mode) && !upscaler_note(u.mode).empty(),
                  std::string(u.mode) + " says it hasn't been played in a headset");
        } else {
            check(!is_dlss(u.mode) && upscaler_note(u.mode).empty(),
                  std::string(u.mode) + " isn't DLSS");
        }
    check(dlss == 6, "DLSS's six modes are listed");
    check(render_size(3000, "dlss_quality") == 2000 && render_size(3000, "dlss_dlaa") == 3000,
          "DLSS Quality renders at 1/1.5, DLAA at full size");

    // Who's offered DLSS: an NVIDIA card named RTX.
    auto gpu = [](uint32_t vendor, const char *name) {
        GpuInfo g;
        g.vendor = vendor;
        g.name = name;
        return g;
    };
    check(gpu(0x10de, "NVIDIA GeForce RTX 3070 Laptop GPU").rtx(), "an RTX 3070 is");
    check(gpu(0x10de, "NVIDIA GeForce RTX 5090").rtx(), "an RTX 5090 is");
    check(gpu(0x10de, "NVIDIA RTX A4000").rtx(), "a workstation RTX is");
    check(!gpu(0x10de, "NVIDIA GeForce GTX 1660 SUPER").rtx(), "a GTX isn't");
    check(!gpu(0x1002, "AMD Radeon RX 7900 XTX (RADV NAVI31)").rtx(), "an AMD card isn't");
    check(!gpu(0x1002, "RTX lookalike").rtx(), "...whatever it's called");
    VulkanProbe vp;
    vp.ok = true;
    GpuInfo a = gpu(0x8086, "Intel(R) UHD Graphics");
    a.api_major = 1, a.api_minor = 3, a.max_sets = 8, a.multiview = true;
    GpuInfo b = gpu(0x10de, "NVIDIA GeForce RTX 3070 Laptop GPU");
    b.api_major = 1, b.api_minor = 4, b.max_sets = 32, b.multiview = b.discrete = true;
    vp.gpus = {a, b};
    const VulkanProbe back = vulkan_probe_parse(vulkan_probe_format(vp));
    check(back.gpus.size() == 2 && back.gpus[1].vendor == 0x10de &&
              back.gpus[1].name == b.name && back.gpus[1].discrete,
          "the vendor and the name survive the child process's output");
    const GpuInfo *best = best_gpu(back);
    check(best && best->rtx(), "a laptop's discrete RTX is the one chosen over its Intel");
}

static void test_menu_entry() {
    const std::string home = g_tmp + "/menu";
    mkdirs(home);
    check(!menu_entry_possible(home), "no menu entry outside the zip");
    check(!add_menu_entry(home).ok, "...and asking for one says so");
    put(menu_entry_path(home), "x");
    bool listed = false;
    for (const std::string &p : uninstall_plan(home, false).mod)
        listed = listed || p == menu_entry_path(home);
    check(listed, "uninstall takes the menu entry with it");

    // As the zip: the entry is for the copy that stays, so only once that's there.
    const std::string data = g_tmp + "/menu-zip/x4vrmod-data";
    put(data + "/manifest", "version=1\n");
    setenv("X4VRMOD_DATA", data.c_str(), 1);
    remove(menu_entry_path(home).c_str());
    check(!menu_entry_possible(home), "the zip, not installed yet: no entry");
    put(home + "/.local/share/x4vrmod/x4vrmod-launcher", "x");
    check(menu_entry_possible(home), "installed: it can have one");
    const ActionReport made = add_menu_entry(home);
    check(made.ok, "...and it's written");
    const std::string entry = read_text_file(menu_entry_path(home));
    check(entry.find("Exec=\"" + home + "/.local/share/x4vrmod/x4vrmod-launcher\"") !=
              std::string::npos, "...naming the installed copy, quoted");
    check(entry.find("Name=X4 VR Mod") != std::string::npos, "...and the mod's name");
    unsetenv("X4VRMOD_DATA");
}

// What must not leave the machine, and what must survive being scrubbed.
static void test_scrub() {
    ScrubNames n;
    n.home = "/home/pat";
    n.user = "pat";
    n.host = "mybox";
    n.profile_ids = {"1234567"};
    auto s = [&n](const std::string &t) { return scrub_text(t, n); };
    check(s("open /home/pat/.config/x") == "open ~/.config/x", "the home folder becomes ~");
    check(s("/home/pat") == "~", "...at the very end of a line too");
    check(s("/home/pat2/x") == "/home/pat2/x", "another user's folder isn't ours");
    check(s("user pat on mybox") == "user <user> on <host>", "user and computer names go");
    check(s("a pattern in myboxes") == "a pattern in myboxes", "words that merely hold them stay");
    check(s("/x/EgoSoft/X4/96679891/config.xml") == "/x/EgoSoft/X4/<profile>/config.xml",
          "a numbered profile folder goes, whichever account");
    check(s("/x/EgoSoft/X4/gog/config.xml") == "/x/EgoSoft/X4/gog/config.xml", "a named one stays");
    check(s("account 1234567 here") == "account <profile> here", "a known account number goes alone");
    check(s("id 76561198056945619 ok") == "id <steamid> ok", "a SteamID64 goes");
    check(s("id 7656119805694561900 ok") == "id 7656119805694561900 ok", "a longer number stays");
    check(s("frame 1234568 at 0x1234567a") == "frame 1234568 at 0x1234567a", "other numbers stay");
    ScrubNames shortName = n;
    shortName.user = "me";
    check(scrub_text("the frame", shortName) == "the frame", "a very short name isn't hunted");
}

static void test_log_excerpt() {
    const std::string p = g_tmp + "/excerpt.log";
    std::string big(1000, 'a');
    std::fill(big.begin() + 900, big.end(), 'z');
    put(p, big);
    check(log_excerpt(p, 2000, 0) == big, "a short log is kept whole");
    const std::string e = log_excerpt(p, 10, 20);
    check(e.rfind(std::string(10, 'a'), 0) == 0, "a long one keeps its start");
    check(e.size() >= 20 && e.compare(e.size() - 20, 20, std::string(20, 'z')) == 0, "...and its end");
    check(e.find("left out") != std::string::npos, "...and says so");
    check(log_excerpt(g_tmp + "/nothing.log", 10, 10).empty(), "no log, nothing");
}

static void test_report() {
    const std::string home = g_tmp + "/report";
    const char *saved = getenv("XDG_STATE_HOME");
    const std::string keep = saved ? saved : "";
    unsetenv("XDG_STATE_HOME");
    check(session_logs(home).empty(), "no sessions yet");
    const ScrubNames names = scrub_names(home);
    const std::string logs = home + "/.local/state/x4vrmod/logs";
    const std::string normal = logs + "/x4vrmod-20260101-100000.log";
    const std::string debug = logs + "/x4vrmod-20260102-100000-debug.log";
    const std::string secret = home + "/.config/EgoSoft/X4/96679891/config.xml";
    put(normal, "old\n");
    put(debug, "opened " + secret + " on " + names.host + " as " + names.user +
                   " id 76561198056945619\n");
    put(logs + "/launch-output.txt", "x4vr-launch: said " + home + "\n");
    put(logs + "/other.log", "not ours\n");
    {
        const std::string t1 = normal;
        struct stat st;
        check(stat(t1.c_str(), &st) == 0, "the logs are there");
        struct timespec ts[2] = {{1000000, 0}, {1000000, 0}};
        utimensat(AT_FDCWD, t1.c_str(), ts, 0);
    }
    const std::vector<SessionLog> ls = session_logs(home);
    check(ls.size() == 2 && ls[0].path == debug && ls[0].debug && !ls[1].debug,
          "sessions: newest first, debug ones marked, nothing else listed");

    ReportOptions o;
    o.home = home;
    o.log = debug;
    o.note = "It broke at " + home + "\nSecond line";
    const ReportResult r = make_report(o);
    check(r.ok, "a report is made: " + r.error);
    auto file = [&r](const char *n) { return read_text_file(r.dir + "/" + n); };
    check(file("session.log").find("opened ~/.config/EgoSoft/X4/<profile>/config.xml") !=
              std::string::npos, "the log is there, scrubbed");
    for (const char *f : {"session.log", "summary.txt", "what-happened.txt", "launch-output.txt"}) {
        const std::string t = file(f);
        check(!t.empty(), std::string(f) + " is in it");
        check(t.find(home) == std::string::npos && t.find("96679891") == std::string::npos &&
                  t.find("76561198056945619") == std::string::npos &&
                  (names.host.size() < 3 || t.find(names.host + " ") == std::string::npos),
              std::string(f) + " holds nothing private");
    }
    check(file("summary.txt").find(own_version()) != std::string::npos, "the summary has the version");
    check(file("summary.txt").find("debug session") != std::string::npos, "...and says it's a debug log");
    check(file("summary.txt").find("not during the session") != std::string::npos,
          "...and that its checklist is from now");
    check(is_file(r.archive) && r.archive_size > 0 && !r.too_big, "the archive is there, and fits");
    check(r.archive.size() > 7 && r.archive.compare(r.archive.size() - 7, 7, ".tar.gz") == 0,
          "...in a format GitHub takes");
    const ChildResult l = run_child({"tar", "tzf", r.archive}, 20000);
    check(l.out.find("session.log") != std::string::npos && l.out.find("summary.txt") != std::string::npos,
          "...holding the files");
    check(r.issue_url.find("template=bug_report.yml") != std::string::npos, "the issue uses the form");
    check(r.issue_url.find("title=It%20broke") != std::string::npos, "...titled from the note's first line");
    check(r.issue_url.find("Second") == r.issue_url.find("what=") + 5 + r.issue_url.substr(r.issue_url.find("what=") + 5).find("Second"),
          "...with the whole note in its field");
    check(r.issue_url.find("report/.local") == std::string::npos && r.issue_url.size() < 8000,
          "...with nothing private, and short enough for a URL");

    ReportOptions none;
    none.home = home;
    none.log = "";
    const ReportResult nr = make_report(none);
    check(nr.ok && !is_file(nr.dir + "/session.log"), "no log yet: a report can still be made");
    check(read_text_file(nr.dir + "/summary.txt").find("Log: none") != std::string::npos, "...and says so");
    if (saved)
        setenv("XDG_STATE_HOME", keep.c_str(), 1);
}

// WiVRn is what we suggest: with it installed, nothing should point at SteamVR first.
static void test_suggests_wivrn() {
    if (steamvr_is_running()) {
        printf("skip   SteamVR is running here: the row can't be told apart\n");
        return;
    }
    const std::string home = g_tmp + "/suggest";
    const std::string bin = g_tmp + "/suggest-bin";
    put(home + "/.config/openxr/1/active_runtime.json",
        "{\"file_format_version\":\"1.0.0\",\"runtime\":{\"name\":\"SteamVR\","
        "\"VALVE_runtime_is_steamvr\":true,\"library_path\":\"x\"}}");
    put(bin + "/wivrn-dashboard", "#!/bin/sh\n");
    chmod((bin + "/wivrn-dashboard").c_str(), 0755);
    const std::string keepPath = getenv("PATH") ? getenv("PATH") : "";
    const char *xr = getenv("XR_RUNTIME_JSON");
    const std::string keepXr = xr ? xr : "";
    unsetenv("XR_RUNTIME_JSON");
    auto vr = [&home] {
        CheckOptions o;
        o.home = home;
        o.skip_gpu = true;
        for (const Check &c : run_checks(o))
            if (c.id == "vr")
                return c;
        return Check{};
    };
    setenv("PATH", (bin + ":" + keepPath).c_str(), 1);
    check(wivrn_program() == bin + "/wivrn-dashboard", "the dashboard is what starts WiVRn");
    Check c = vr();
    check(c.fix == "start-vr", "SteamVR active but stopped: the row offers to start a runtime");
    check(c.title == "No VR runtime running", "...the title doesn't send you to SteamVR");
    check(c.detail.empty(), "...and the row has nothing more to say");
    setenv("PATH", "/nonexistent", 1);
    check(wivrn_program().empty(), "no WiVRn installed");
    c = vr();
    check(c.fix == "start-vr" && c.detail.empty(), "...the same row: the window offers what's there");
    remove((home + "/.config/openxr/1/active_runtime.json").c_str());
    c = vr();
    check(c.fix == "start-vr" && c.detail.empty(), "no runtime at all: the same row");
    setenv("PATH", keepPath.c_str(), 1);
    if (xr)
        setenv("XR_RUNTIME_JSON", keepXr.c_str(), 1);
}

static void test_real_x4() {
    const char *e = getenv("X4VR_GAME");
    const auto all = discover_all(home_dir());
    const Install *in = preferred_install(all);
    const std::string exe = e && *e ? e : in ? in->exe : "";
    ElfFile elf;
    if (exe.empty() || !elf.open(exe)) {
        printf("skip  patch groups — no X4 here\n");
        return;
    }
    const auto plain = [&elf](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
    const auto base = check_patches(plain);
    bool good = !base.empty();
    for (const PatchResult &r : base)
        good = good && r.ok;
    if (!good) {
        printf("skip  patch groups — this X4 isn't the analysed build\n");
        return;
    }
    check(true, "patches: every group fits the real X4");

    using namespace x4vr;
    const std::map<std::string, uintptr_t> sites = {
        {"headlook", camwrite::kWriteSites[0].va},
        {"foot", foot::kNullExit.va},
        {"extview", extview::kDispatch.va},
        {"uimouse", uimouse::kEntry.va},
        {"steer", steer::kSites[0].va},
        {"aimpick", aimpick::kLive.va},
        {"menus", menus::kConversation.va},
        {"shadows", csmfit::kSites[0].site.va},
        {"loadart", loadart::kCall.va},
        {"dlss", ngx::kEvaluate.va},
    };
    check(sites.size() == base.size(), "patches: this test covers every group");
    for (const PatchResult &r : base) {
        const auto it = sites.find(r.id);
        if (it == sites.end()) {
            check(false, std::string("patches: no site known for ") + r.id);
            continue;
        }
        const uintptr_t at = it->second;
        const auto bent = [&elf, at](uintptr_t va, void *o, size_t n) {
            if (!elf.read(va, o, n))
                return false;
            if (at >= va && at < va + n)
                ((uint8_t *)o)[at - va] ^= 0xff;
            return true;
        };
        bool mine = false;
        for (const PatchResult &q : check_patches(bent))
            if (!strcmp(q.id, r.id))
                mine = !q.ok && q.bad != 0;
        check(mine, std::string("patches: one bent byte fails ") + r.id);
    }
    const auto none = [](uintptr_t, void *, size_t) { return false; };
    bool allbad = true;
    for (const PatchResult &r : check_patches(none))
        allbad = allbad && !r.ok;
    check(allbad, "patches: a file with none of the code fails every group");
}

int main() {
    char tmpl[] = "/tmp/x4vrmod-test-XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("FAIL  no temp directory\n");
        return 1;
    }
    g_tmp = tmpl;
    test_vdf();
    test_classify();
    test_discover();
    test_version();
    test_elf();
    test_child();
    test_probes_format();
    test_runtime_file();
    test_library_version();
    test_layout();
    test_checks();
    test_eye_rule();
    test_profile();
    test_vr_row();
    test_profile_dirs();
    test_child_env();
    test_offered();
    test_menu_entry();
    test_scrub();
    test_log_excerpt();
    test_report();
    test_suggests_wivrn();
    test_real_x4();
    run_child({"rm", "-rf", g_tmp}, 10000);
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
