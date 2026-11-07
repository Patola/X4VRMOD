// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_launcher.hpp -- the launcher's core: everything the command-line tool
// and the window share. Finding X4, asking its file whether the mod's patches
// fit, probing VR, and the checks behind the Play button. No Qt here: the CLI
// must work without it, and every check exists once.
// See docs/packaging-plan.md, Phases 2 and 3.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace x4vr::launcher {

// What this release was built and tested against (version.dat's value).
constexpr int kTestedX4Version = 900;
constexpr const char *kSteamAppId = "392160";

// ---------------------------------------------------------------- small tools
std::string read_text_file(const std::string &path, size_t max = 16 * 1024 * 1024);
bool is_file(const std::string &path);
bool is_dir(const std::string &path);
std::string real_path(const std::string &path);
std::string dir_name(const std::string &path);
std::string base_name(const std::string &path);
std::string home_dir();
// The first program called `name` on PATH, or "".
std::string find_on_path(const std::string &name);
// Feral GameMode's wrapper: `gamemoderun` on the PATH.
bool have_gamemode();
// Every value of `"key": "..."` in a JSON text. A scan, not a parser: enough for
// the few small files the launcher reads.
std::vector<std::string> json_strings(const std::string &text, const std::string &key);

// A child process with a timeout, in its own process group so a timeout kills
// everything it started. A broken VR runtime can hang or crash whatever loads
// it, so probes never run in the caller's process.
struct ChildResult {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string out;
};
ChildResult run_child(const std::vector<std::string> &argv, int timeout_ms,
                      bool merge_stderr = false,
                      const std::vector<std::string> &extra_env = {});

// ---------------------------------------------------------------- Valve's VDF
// The text format of libraryfolders.vdf and appmanifest_*.acf.
struct VdfNode {
    std::string key, value;
    bool block = false;
    std::vector<VdfNode> children;
    // Keys compare without case, as Steam's own do; nullptr when absent.
    const VdfNode *find(const std::string &k) const;
    std::string get(const std::string &k) const;
};
bool parse_vdf(const std::string &text, VdfNode *root);

// ------------------------------------------------------------ finding the game
enum class PathKind { Linux, Windows, NotX4, Missing };
struct GamePath {
    PathKind kind = PathKind::Missing;
    std::string exe; // the program, when one was found
    std::string dir; // the folder that holds it
    std::string why; // for NotX4 and Missing
};
// A folder or a file, as Browse may hand it over. In a folder: X4, then game/X4
// (GOG's layout, to verify), then X4.exe. Decided by the file's magic, not only
// its name.
GamePath classify_game_path(const std::string &path);
// Why a Windows copy can't work, in words for the player.
const char *proton_explanation();

enum class Store { Steam, Gog, Unknown };
const char *store_name(Store s);
struct Install {
    Store store = Store::Unknown;
    PathKind kind = PathKind::Missing;
    std::string exe, dir;
    std::string buildid; // Steam only
    std::string source;  // where this came from, for the report
};
std::vector<Install> discover_steam(const std::string &home);
std::vector<Install> discover_gog(const std::string &home);
// X4VR_GAME first, then Steam, then GOG, without duplicates.
std::vector<Install> discover_all(const std::string &home);
// The install to use by default: the first Linux one, else the first found.
// nullptr when there is none.
const Install *preferred_install(const std::vector<Install> &all);
Install install_from_path(const std::string &path);

// version.dat beside the binary: 900 means 9.00. -1 when absent or unreadable.
int read_x4_version(const std::string &dir);
std::string format_x4_version(int v);

// ------------------------------------------------------------------ X4's file
// A read-only view of an ELF file. X4 is non-relocatable, so a virtual address
// maps through the PT_LOAD headers to a file offset.
class ElfFile {
public:
    ElfFile() = default;
    ~ElfFile();
    ElfFile(const ElfFile &) = delete;
    ElfFile &operator=(const ElfFile &) = delete;
    bool open(const std::string &path);
    bool is_x86_64() const;
    // False unless all n bytes are in one segment's file contents.
    bool read(uintptr_t va, void *out, size_t n) const;
    // The DT_NEEDED libraries.
    std::vector<std::string> needed() const;
    // The code segment: its bytes, length and address. False if there's none.
    bool text(const uint8_t **bytes, size_t *size, uintptr_t *va) const;

private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
};

// True when the binary links libsteam_api: it asks the binary, not the path,
// because that is what makes Steam necessary.
bool binary_needs_steam(const std::string &exe);

using VaReader = std::function<bool(uintptr_t, void *, size_t)>;
// One feature whose patches depend on X4's exact code. The same checks the mod
// makes in X4's memory at load, so the two can't disagree.
struct PatchResult {
    const char *id;      // short, stable
    const char *feature; // what the player loses when it doesn't match
    bool ok = false;
    uintptr_t bad = 0;
};
std::vector<PatchResult> check_patches(const VaReader &read);
// The same on a whole file, wherever that build keeps the code (common/x4vr_reloc.hpp).
std::vector<PatchResult> check_patches(const ElfFile &x4);
// For each block of code and data the mod names: "code|data steam-address offset".
std::string reloc_report(const ElfFile &x4);

// ----------------------------------------------------------------- the probes
// Which OpenXR runtime is active, by the loader's search order. It is only a
// file: nothing is loaded. WiVRn's exists only while WiVRn runs.
struct ActiveRuntime {
    std::string path; // "" when there is none
    std::string name, library;
    bool steamvr = false;
};
ActiveRuntime active_runtime(const std::string &home);

enum class XrStatus { Ready, NoLoader, NoRuntime, NoHeadset, Failed, TimedOut, NotRun };
const char *xr_status_name(XrStatus s);
struct XrProbe {
    XrStatus status = XrStatus::NotRun;
    std::string runtime, system, detail;
    unsigned eye_w = 0, eye_h = 0;
};
// In this process: what `x4vrmod probe xr` runs. Instance, xrGetSystem, view
// sizes, destroy. No session.
XrProbe xr_probe_here(bool verbose);
// As a child, through the x4vrmod next to this program.
XrProbe xr_probe_child(int timeout_ms);
std::string xr_probe_format(const XrProbe &p);
XrProbe xr_probe_parse(const std::string &text);

struct GpuInfo {
    std::string name;
    unsigned api_major = 0, api_minor = 0;
    uint32_t max_sets = 0, vendor = 0;
    bool multiview = false, discrete = false;
    // The layer binds its set at the top of the device's range, above X4's 6.
    bool capable() const;
    // What DLSS is offered on: an NVIDIA card named RTX.
    bool rtx() const;
};
struct VulkanProbe {
    bool ok = false; // an instance came up
    std::string detail;
    std::vector<GpuInfo> gpus;
};
VulkanProbe vulkan_probe_here();
VulkanProbe vulkan_probe_child(int timeout_ms);
std::string vulkan_probe_format(const VulkanProbe &p);
// The GPU the mod would run on: capable, a discrete one first. nullptr if there's none.
const GpuInfo *best_gpu(const VulkanProbe &p);
VulkanProbe vulkan_probe_parse(const std::string &text);

// ---------------------------------------------------------------- how we're run
enum class LayoutKind { Dev, Zip, Package };
const char *layout_name(LayoutKind k);
struct Layout {
    LayoutKind kind = LayoutKind::Package;
    std::string data_dir;       // what ships: extension/, launch/, help
    std::string extension_dir;  // the x4vrmod extension to deploy
    std::string layer_manifest; // where the manifest must be for X4 to find it
    std::string injector;       // the preload library
    std::string launch_script;  // x4vr-launch.sh, what `run` execs
    std::string deploy_script;  // deploy-extensions.sh, from what ships
    std::string help_dir;       // the window's help and artwork
};
Layout detect_layout(const std::string &home);
// The directory of this program, from /proc/self/exe.
std::string self_dir();
// The version a library or manifest's library carries, read from the file,
// never by loading it. "" when it has none.
std::string library_version(const std::string &path);
// This program's own version.
const char *own_version();
// The x4vrmod command line: beside this program, else on PATH.
std::string cli_path();

// ----------------------------------------------------------------- is it running
bool steam_is_running(const std::string &home);
bool steamvr_is_running();
// Any process whose program is this file, or is called X4.
bool x4_is_running(const std::string &exe);

// ------------------------------------------------------------------ the profile
// ~/.config/x4vrmod/profile.toml: the player's settings, nothing derived. A flat
// "key = value" subset of TOML.
struct Profile {
    std::map<std::string, std::string> values;
    std::string get(const std::string &k, const std::string &fallback = "") const;
};
std::string profile_path(const std::string &home);
Profile load_profile(const std::string &path);
bool save_profile(const std::string &path, const Profile &p);
// Validates, and applies the eye-size rule. Returns "" or a note for the player;
// a refusal starts with "error: " and changes nothing.
std::string profile_set(Profile &p, const std::string &key, const std::string &value);
// The profile as environment variables for the launch script.
std::vector<std::pair<std::string, std::string>> profile_env(const Profile &p);

// Eye size and upscaler (decision 6). Going above 3200 per eye with no upscaler
// selects FSR 1 Quality; a choice the player made is never overridden.
constexpr int kAutoUpscaleAbove = 3200;
constexpr const char *kAutoUpscaler = "fsr_quality";
struct EyeSettings {
    int eye = 2816;
    std::string upscaler = "none";
    bool upscaler_auto = false; // the rule chose it, not the player
};
EyeSettings eye_settings(const Profile &p);
void store_eye_settings(Profile &p, const EyeSettings &s);
// Returns a note when the rule changed the upscaler, else "".
std::string set_eye(EyeSettings &s, int eye);
void set_upscaler(EyeSettings &s, const std::string &mode);
bool valid_upscaler(const std::string &mode);
// What X4 renders for this eye size and upscaler.
int render_size(int eye, const std::string &mode);
// What's offered, in order; the DLSS ones only with an NVIDIA RTX GPU.
struct Upscaler {
    const char *mode;
    const char *label;
    double ratio;
    bool dlss = false;
};
bool is_dlss(const std::string &mode);
// What the player should know about this upscaler, or "".
std::string upscaler_note(const std::string &mode);
const std::vector<Upscaler> &upscalers();
const std::vector<int> &eye_sizes();
// What the player should know about this size, or "".
std::string eye_note(int eye, bool steamvr);

// -------------------------------------------------- install, uninstall and run
// The one given, then X4VR_GAME, then the profile's, else the best one discovered.
// *found says whether the result names a program (it may still be the Windows
// version).
Install choose_install(const std::string &home, const std::string &game, bool *found);

struct ActionReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> log;
};
ActionReport install_mod(const Install &x4, const std::string &home);
// What installing does to the player's saves, said before they agree to it.
const char *install_disclosure();
// The zip's entry in the application menu: only once it's installed, because that's
// the copy whose path stays. A package ships its own.
std::string menu_entry_path(const std::string &home);
bool menu_entry_possible(const std::string &home);
ActionReport add_menu_entry(const std::string &home);
// What uninstalling would remove. `settings` is what can't be undone, and is
// only filled when asked for.
struct UninstallPlan {
    std::vector<std::string> mod, settings;
    std::string note;
};
UninstallPlan uninstall_plan(const std::string &home, bool with_settings);
ActionReport uninstall_apply(const UninstallPlan &p);
// Execs the launch script with the profile in its environment; returns only on
// failure. `command` is Steam's %command%, when there is one.
int run_mod(const std::string &home, const std::string &game,
            const std::vector<std::string> &command);
// X4 by itself, on the screen, the way X4 starts itself: a baseline with nothing of
// ours in it, whatever VR is doing. Returns only on failure.
int run_flat(const std::string &home, const std::string &game);
std::string steam_launch_line(const std::string &home);
std::string terminal_line(const std::string &home);
std::string log_dir(const std::string &home);
// Everywhere X4 may keep a player's files. X4 asks XDG_CONFIG_HOME, then HOME. Steam's
// copy keeps them in a folder named for the account, and GOG's in EgoSoft/X4 itself, so
// no name is assumed: EgoSoft/X4 and every folder in it.
std::vector<std::string> x4_profile_dirs(const std::string &home);

// ------------------------------------------------------------------ the checks
// Ok, or Warn (can launch with something lost), or Block (can't launch).
enum class Status { Ok, Warn, Block };
struct Check {
    std::string id;
    Status status = Status::Ok;
    std::string title;
    std::string detail;
    std::string fix; // an action the caller can offer: "" for none
    bool dlss = false; // the gpu row: DLSS can be offered
};
struct CheckOptions {
    std::string home;
    std::string game; // a path chosen by the player; "" to discover
    bool skip_vr = false;
    bool skip_gpu = false; // it won't change while a window stays open
    XrProbe *xr_out = nullptr; // what the VR runtime answered, when it was asked
};
std::vector<Check> run_checks(const CheckOptions &o);
// The VR row, from what the runtime answered.
Check vr_row(const ActiveRuntime &rt, const XrProbe &p);
// The checks as text, one line each.
std::string checks_text(const std::vector<Check> &checks);
// WiVRn's program to start, the dashboard if there is one, or "". It's the first choice
// wherever one is offered (docs/packaging-plan.md, decision 12).
std::string wivrn_program();

// ------------------------------------------------------------------ bug reports
// Taken out of everything a report holds. The player reads it before it goes anywhere.
struct ScrubNames {
    std::string home, user, host;
    std::vector<std::string> profile_ids; // X4's numbered profile folders: Steam accounts
};
ScrubNames scrub_names(const std::string &home);
std::string scrub_text(const std::string &text, const ScrubNames &names);

// The logs `x4vrmod run` kept, newest first.
struct SessionLog {
    std::string path;
    long long when = 0;
    uint64_t size = 0;
    bool debug = false;
};
std::vector<SessionLog> session_logs(const std::string &home);
// A log, keeping `head` and `tail` bytes of one that's longer than both.
std::string log_excerpt(const std::string &path, size_t head, size_t tail);

struct ReportOptions {
    std::string home;
    std::string game; // "" as `run` would choose
    std::string log;  // "" when no session got that far
    std::string note; // what the player says happened
};
struct ReportResult {
    bool ok = false;
    std::string error;
    std::string dir, archive; // the files, and the same packed for GitHub
    std::string summary;      // scrubbed, for the issue
    std::string issue_url;    // a new issue with the note and the summary filled in
    uint64_t archive_size = 0;
    bool too_big = false;     // over GitHub's limit for an attachment
};
ReportResult make_report(const ReportOptions &o);
std::string issue_url(const std::string &note, const std::string &summary);
// The worst status of a set of checks: what the Play button shows.
Status worst(const std::vector<Check> &checks);

} // namespace x4vr::launcher
