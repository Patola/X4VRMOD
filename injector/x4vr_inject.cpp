// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// libx4vr_inject.so — LD_PRELOAD injector.
//
// Phase 1: give X4 its own settings file when running under the mod.
// config.xml is forked once into config-x4vrmod.xml; from then on every read
// is answered from the profile (with our overrides applied in memory) and
// every write is redirected into it, so the player's settings file on disk
// is never modified. See injector/README.md and x4vr_config.hpp.
//
// X4 both reads and writes the file with fopen() (confirmed live), so that
// is the primary hook; the open()/openat() family is interposed for
// observation, and redirects writes as a safety net in case a future patch
// changes the access path.
//
// Later phases add LuaJIT FFI and SDL hooks.

#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

extern char **environ;

#define X4VR_LOG_TAG "inject"
#include "../common/x4vr_log.hpp"
#include "../common/x4vr_env.hpp"
#include "../common/x4vr_share.hpp"
#include "x4vr_config.hpp"
#include "x4vr_inputmap.hpp"
#include "../common/x4vr_headlook.hpp"
#include "x4vr_lua.hpp"

namespace {

bool is_config_xml(const char *path) {
    return path && strstr(path, "config.xml");
}

// Logging only, and deliberately broader than x4vr::is_x4_inputmap: reads are
// served from fopen() alone, so if X4 opens its inputmap through open() or
// openat() instead the rebind would silently never happen. The log has to be
// able to distinguish that from "we rebound it and it did not take".
bool is_inputmap_path(const char *path) {
    return path && strstr(path, "inputmap");
}

bool overrides_disabled() {
    static const bool off = [] {
        const char *e = getenv("X4VR_NO_CONFIG");
        return e && *e && *e != '0';
    }();
    return off;
}

// The mod's settings file and its contents. `xml` is empty if we could not
// establish one, in which case the caller leaves X4's own file alone.
struct Profile {
    std::string path;
    std::string xml;
};

// Load config-x4vrmod.xml, forking it verbatim from the player's config.xml
// the first time. The fork is a plain copy: the overrides are applied in
// memory on every read (see open_patched_config), so baking them into the
// file would be redundant — and keeping the copy verbatim means the profile
// starts out as an honest snapshot of the player's own preferences.
Profile load_profile(const char *config_path) {
    Profile p;
    p.path = x4vr::profile_path(config_path);
    if (p.path.empty())
        return p;
    p.xml = x4vr::read_file(p.path.c_str());
    if (!p.xml.empty())
        return p; // already forked (also re-forks if it was left empty)

    p.xml = x4vr::read_file(config_path);
    if (p.xml.empty()) {
        X4VR_LOG("config: could not read %s — passing through", config_path);
        return p;
    }
    if (x4vr::write_file(p.path.c_str(), p.xml))
        X4VR_LOG("config: created %s from your config.xml — X4's settings go "
                 "there while modded; delete it to reset",
                 p.path.c_str());
    else
        X4VR_LOG("config: WARNING could not create %s — settings changed this "
                 "session will not persist",
                 p.path.c_str());
    return p;
}

// Build the rewritten config and return a stream over it, or nullptr to let
// the caller fall back to the real file.
FILE *open_patched_config(const char *path) {
    Profile prof = load_profile(path);
    if (prof.xml.empty())
        return nullptr;
    std::string xml = std::move(prof.xml);

    // Applied on every read, not just the first: if the player changes one of
    // these in the options menu, X4 writes it to the profile and we override
    // it again next launch. Every *other* setting they change persists.
    int changed = 0;
    for (const auto &ov : x4vr::default_overrides()) {
        std::string old;
        if (x4vr::set_tag(xml, ov.tag, ov.value, &old)) {
            X4VR_LOG("config: %s: '%s' -> '%s'", ov.tag, old.c_str(), ov.value);
            changed++;
        }
    }

    // Back the stream with a memfd rather than fmemopen(): an fmemopen
    // stream has no real file descriptor, so any fileno()/fstat()/mmap() in
    // the consumer's XML reader fails and X4 silently falls back to its
    // defaults (observed: desktop resolution instead of ours). A memfd is
    // still purely in memory — nothing is written to disk.
    int fd = memfd_create("x4vr-config", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    size_t off = 0;
    while (off < xml.size()) {
        ssize_t w = ::write(fd, xml.data() + off, xml.size() - off);
        if (w <= 0) {
            ::close(fd);
            return nullptr;
        }
        off += (size_t)w;
    }
    ::lseek(fd, 0, SEEK_SET);
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        ::close(fd);
        return nullptr;
    }
    // Stated on every run, overridden or not: this is what X4 will build its
    // projection from, and a log that omits it cannot reproduce its own run.
    // See get_tag() for why the un-overridden case is the dangerous one.
    X4VR_LOG("config: effective fov=%s res=%sx%s (%s)",
             x4vr::get_tag(xml, "fov").c_str(),
             x4vr::get_tag(xml, "res_width").c_str(),
             x4vr::get_tag(xml, "res_height").c_str(),
             getenv("X4VR_FOV") ? "fov from X4VR_FOV"
                                : "fov from the profile, not this run");
    X4VR_LOG("config: serving %s in memory (%zu bytes, %d overrides applied); "
             "your %s is not touched",
             prof.path.c_str(), xml.size(), changed, path);
    return f;
}

// ------------------------------------------------------------- inputmap
//
// #33 gives head-look its own binding rather than synthesising shift+middle
// mouse, which would go through X4's binding interpreter and corrupt every
// combo built on those. See x4vr_inputmap.hpp for why it is a held key and not
// a toggle. Same fork-and-serve shape as config.xml above: the player's file is
// read once and never written.
bool headlook_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_HEADLOOK");
        return e && *e && *e != '0';
    }();
    return on;
}

Profile load_inputmap_profile(const char *path) {
    Profile p;
    p.path = x4vr::inputmap_profile_path(path);
    if (p.path.empty())
        return p;
    p.xml = x4vr::read_file(p.path.c_str());
    if (!p.xml.empty())
        return p;
    p.xml = x4vr::read_file(path);
    if (p.xml.empty()) {
        X4VR_LOG("inputmap: could not read %s — passing through", path);
        return p;
    }
    if (!x4vr::write_file(p.path.c_str(), p.xml))
        X4VR_LOG("inputmap: WARNING could not create %s", p.path.c_str());
    return p;
}

// Build the rebound inputmap and return a stream over it, or nullptr to fall
// back to the real file.
FILE *open_patched_inputmap(const char *path) {
    Profile prof = load_inputmap_profile(path);
    if (prof.xml.empty())
        return nullptr;
    std::string xml = std::move(prof.xml);

    std::string old;
    const int n = x4vr::bind_headlook(xml, &old);
    if (n == 0) {
        // Stated rather than passed over: an inputmap X4 reads that has no
        // free-look binding means head-look will not work in that profile, and
        // silence here would look exactly like success.
        X4VR_LOG("inputmap: %s has no INPUT_STATE_CAMERA_MOUSELOOK — head-look "
                 "will not be bound from this file",
                 path);
        return nullptr;
    }
    X4VR_LOG("inputmap: %s: %d binding(s) -> %s (was %s)", path, n,
             x4vr::headlook_code(), old.c_str());

    // memfd, not fmemopen: an fmemopen stream has no real descriptor, and a
    // reader that calls fileno()/fstat()/mmap() silently falls back to its
    // defaults. Learned on config.xml; the same reader is likely behind this.
    int fd = memfd_create("x4vr-inputmap", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    size_t off = 0;
    while (off < xml.size()) {
        ssize_t w = ::write(fd, xml.data() + off, xml.size() - off);
        if (w <= 0) {
            ::close(fd);
            return nullptr;
        }
        off += (size_t)w;
    }
    ::lseek(fd, 0, SEEK_SET);
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        ::close(fd);
        return nullptr;
    }
    X4VR_LOG("inputmap: serving %s in memory (%zu bytes); your %s is not touched",
             prof.path.c_str(), xml.size(), path);
    return f;
}

// If this is a write-mode open of X4's config.xml, the path to use instead
// (empty = leave the call alone). X4 saving its settings must never reach the
// player's file. Absolute paths only: an *at() call with a relative path
// resolves against dirfd, which our profile lookup cannot see.
std::string redirect_write(const char *path) {
    if (overrides_disabled() || !path || path[0] != '/')
        return {};
    if (x4vr::is_x4_config(path)) {
        Profile prof = load_profile(path);
        return prof.xml.empty() ? std::string{} : prof.path;
    }
    if (headlook_enabled() && x4vr::is_x4_inputmap(path)) {
        Profile prof = load_inputmap_profile(path);
        return prof.xml.empty() ? std::string{} : prof.path;
    }
    return {};
}

// ------------------------------------------------------- the environment
//
// The launcher composes an environment for X4 and has never checked that X4
// received it. Take thirty-three came up on a Wayland surface while the
// launcher was passing `env -u WAYLAND_DISPLAY SDL_VIDEO_DRIVER=x11`, and
// nothing in the log distinguished "the environment did not arrive" from "X4
// overrode it" from "SDL fell back". These two hooks separate those: the
// startup dump is what arrived, the setenv/putenv watch is what changed after.
const char *const kWatchedEnv[] = {
    "SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER", "WAYLAND_DISPLAY",
    "DISPLAY",         "XDG_SESSION_TYPE", "XDG_RUNTIME_DIR",
};

bool is_watched_env(const char *name, size_t len) {
    for (const char *w : kWatchedEnv)
        if (strlen(w) == len && !strncmp(name, w, len))
            return true;
    return false;
}

// Only the game, not the dozen shell utilities the launcher runs on the way.
bool exe_is_game(const char *exe) {
    const char *slash = strrchr(exe, '/');
    return slash && !strcmp(slash + 1, "X4");
}

void log_startup_env() {
    for (const char *w : kWatchedEnv) {
        const char *v = getenv(w);
        X4VR_LOG("env: %s=%s", w, v ? v : "(unset)");
    }
    // Every X4VR_* knob, in one copy-pasteable line.
    //
    // Runs have been referred to by take number for thirty-three takes and
    // the command that produced each one was never written down, so
    // reproducing an earlier configuration meant reconstructing it from what
    // the log happened to reveal. Since every knob is an environment
    // variable, listing them *is* the command. X4VR_TAKE is an ordinary
    // variable with no meaning to the code -- it exists only to be printed
    // here, so a run can carry its own name.
    std::string run;
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "X4VR_", 5)) {
            run += ' ';
            run += *e;
        }
    X4VR_LOG("env: run =%s ./launch/x4vr-launch.sh", run.c_str());
    // argv, for the command-line switches that pick a backend
    // (-prefer-wayland and friends): the launcher does not add them, but the
    // Steam launch options are outside its view entirely.
    std::string cmd = x4vr::read_file("/proc/self/cmdline");
    for (auto &c : cmd)
        if (c == '\0')
            c = ' ';
    X4VR_LOG("env: argv = %s", cmd.c_str());
}

template <typename T>
T real(const char *name) {
    static_assert(sizeof(T) == sizeof(void *), "fn ptr size");
    void *p = dlsym(RTLD_NEXT, name);
    T fn;
    memcpy(&fn, &p, sizeof(fn));
    return fn;
}

} // namespace

extern "C" {

__attribute__((constructor)) static void x4vr_inject_init() {
    char exe[256] = "?";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
        exe[n] = 0;
    X4VR_LOG("injector loaded into pid %d (%s)", getpid(), exe);
    if (exe_is_game(exe))
        log_startup_env();
}

// X4 sets SDL_VIDEO_DRIVER itself -- the string "x11,wayland" is in the
// binary, next to SDL_VIDEO_DRIVER and SDL_GetCurrentVideoDriver. Whether it
// arrives through the environment or through SDL_SetHint decides whether the
// launcher can win by exporting a value at all, so watch the environment path
// here; the SDL path is read from the layer via SDL_GetHint.
//
// Interposed rather than merely observed for one reason: nothing else can see
// a write that happens after our constructor ran.
// Where X4 gets its render size.
//
// Take thirty-nine closed the last alternative: the layer reported a real
// 1408x1408 currentExtent and X4 asked for 2816x1408 anyway, so X4 does not
// consult the Vulkan surface at all. Take thirty-eight had already shown it
// follows its window. The window is therefore one number serving two purposes
// that must differ -- input wants the display width, the render wants one eye
// -- and the only way to separate them is to change what X4 *believes* the
// window is while leaving the real one alone.
//
// SDL_GetWindowSize and SDL_GetWindowSizeInPixels are the plausible sources.
// Interposing SDL was avoided until now because SDL2 and SDL3 disagree about
// SDL_CreateWindow's signature and gamescope (SDL2) shares this process tree
// with X4 (SDL3). These two are safe where that one was not: both SDL
// generations declare identical argument lists, and the return value is
// forwarded opaquely rather than interpreted.
//
// Logging is unconditional; the halving is behind X4VR_HALVE_WINDOW, so one
// run answers both "does X4 ask this" and "does answering differently work".
bool halve_window() {
    static const bool on = [] {
        const char *e = getenv("X4VR_HALVE_WINDOW");
        return e && *e && *e != '0';
    }();
    return on;
}

bool this_is_the_game() {
    static const bool yes = [] {
        char exe[256] = "?";
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0)
            exe[n] = 0;
        return exe_is_game(exe);
    }();
    return yes;
}

// Reports at most a handful of lines: X4 asks every frame, and the answer is
// only interesting when it changes.
void note_window_size(const char *fn, int w, int h, bool halved) {
    static int last_w = -1, last_h = -1;
    if (w == last_w && h == last_h)
        return;
    last_w = w;
    last_h = h;
    X4VR_LOG("sdl: %s -> %dx%d%s (pid %d)", fn, w, h,
             halved ? " (HALVED from the real window)" : "", getpid());
}

// Which display server X4 is actually talking to.
//
// Take thirty-six: X4 creates a Wayland surface (handle 0x3edd2040) and an xcb
// surface (0x3befb820), never asks whether it can present on the xcb one, and
// builds its swapchain on the Wayland one -- while SDL's video driver is x11.
// Every explanation for that turns on which socket the Wayland connection
// reached, and the two candidates sit side by side in $XDG_RUNTIME_DIR:
// `wayland-0` is the host Plasma session, `gamescope-0` is gamescope's own.
// The launcher's `env -u WAYLAND_DISPLAY` strips the variable that would have
// named the second, so an unqualified wl_display_connect(NULL) falls back to
// the first.
//
// That is an argument. This is the measurement: the connect() path itself.
// Only display-server sockets are logged -- X4 opens plenty of others.
// The input channel, watched before anything writes to it.
//
// Take forty separated the two purposes the window was thought to serve. X4's
// first SDL_GetWindowSize call lands 33ms *after* it has already created the
// swapchain, so the render size does not come from there -- and halving it
// broke menu clicks and map hover while leaving the render at 2816. So
// SDL_GetWindowSize is X4's **input space**, and the render size arrives by
// some other route entirely. They are different channels, which is what makes
// a shim that owns input possible without touching the render at all.
//
// SDL_PollEvent has identical arguments in SDL2 and SDL3; only the SDL_Event
// layout differs, and that is read solely when the process is X4 (SDL3). The
// offsets below are SDL3's: type/reserved/timestamp/windowID/which fill the
// first 24 bytes of both the motion and button events, putting x and y at 28
// and 32 in each.
struct Sdl3MouseEvent {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint32_t state_or_buttons;
    float x, y;
    float xrel, yrel;
};
enum { SDL_EV_MOUSE_MOTION = 0x400, SDL_EV_MOUSE_DOWN = 0x401,
       SDL_EV_MOUSE_UP = 0x402 };

// SDL3 SDL_KeyboardEvent, laid out positionally for the same reason as
// Sdl3MouseEvent above; tests/sdl_event_layout.cpp asserts both against the
// real headers.
//
// **Take 118 is why this exists.** X4 *imports* SDL_GetKeyboardState -- `nm -D`
// says so -- and never calls it: the run produced zero `headlook: armed` lines
// from a one-shot log inside the hook, while every other SDL interposer in this
// file fired normally. Imports are necessary, not sufficient, and the recorded
// error class this project already carries ("hooking a symbol X4 never
// imports") has a subtler sibling: hooking one it imports but does not call.
// X4 reads the keyboard the same way it reads the mouse, from the event queue.
struct Sdl3KeyEvent {
    uint32_t type;      //  0
    uint32_t reserved;  //  4
    uint64_t timestamp; //  8
    uint32_t windowID;  // 16
    uint32_t which;     // 20
    uint32_t scancode;  // 24
    uint32_t key;       // 28
    uint16_t mod;       // 32
    uint16_t raw;       // 34
    bool down;          // 36
    bool repeat;        // 37
};
enum { SDL_EV_KEY_DOWN = 0x300, SDL_EV_KEY_UP = 0x301 };
// "X4VR" little-endian, stamped into the unused `reserved` field of every event
// we synthesise so the log can separate ours from the player's.
constexpr uint32_t kOurEventMagic = 0x52565834u;

// A genuine motion event's windowID/which, copied so ours are indistinguishable
// from X4's own. Guessing zero would be a plausible-looking value X4 may filter
// on, and the failure would present as "head-look does nothing".
std::atomic<uint32_t> g_motion_window{0};
std::atomic<uint32_t> g_motion_which{0};
std::atomic<uint32_t> g_motion_seen{0};
// Set on the first SDL_PumpEvents from X4; see the interposer.
std::atomic<bool> g_pump_seen{false};
// True while head-look is actually driving X4's camera; read by the event
// drain to decide whether the player's own mouse motion should reach the view.
std::atomic<bool> g_headlook_steering{false};
// Take 126 measured raw=48 for the apostrophe: on Linux this is the X11
// keycode, which is the evdev code plus 8. Settable so a run can test whether
// our key activates free-look with no real press to seed it -- the one thing
// take 126 could not separate, because the same press both activated mouselook
// and donated the field.
std::atomic<uint32_t> g_key_raw{[] {
    const char *e = getenv("X4VR_HEADLOOK_RAW");
    return e && *e ? (uint32_t)atoi(e) : 0u;
}()};
std::atomic<bool> g_key_raw_seen{getenv("X4VR_HEADLOOK_RAW") != nullptr};


// Observation only. Enough samples to establish the coordinate space and
// whether motion is absolute or relative, and then quiet: the question is what
// space X4 is told about, not how often.
//
// These hook SDL_WaitEvent and SDL_PeepEvents because that is what X4 actually
// calls. SDL_PollEvent was interposed here from take 40 until take 87 and fired
// exactly zero times across every run, including one with heavy map and menu
// use -- because X4 never imported it:
//
//     $ nm -D --undefined-only X4 | grep -c SDL_PollEvent
//     0
//
// One command, available from the first day, would have shown that the
// instrument could not work. Check the symbol is imported before drawing any
// conclusion from a silent hook.
// ---------------------------------------------------------------------------
// The input fold (task #19).
//
// X4 renders one eye; the compositor draws that frame twice, side by side, and
// gamescope centres X4's W-wide surface in the 2W-wide display. So an element
// X4 draws at `x_x4` appears on screen at `x_x4` and `x_x4 + W`, while the
// pointer -- confined to the surface -- reaches X4 as `x_sdl = x_screen - W/2`,
// measured at take 89 as exactly `0…1407` for W = 1408.
//
// The consequence is that pointing *at* an element never selects it: the input
// box straddles the seam, covering the right half of one copy and the left half
// of the other. To hover the station drawn at 251 you must put the pointer at
// 955.
//
//     x_x4 = (x_sdl + W/2) mod W
//
// undoes it. Coverage is complete rather than merely better: folding the full
// `0…W-1` yields `W/2…W-1` together with `0…W/2-1`, which is X4's whole frame,
// each element reachable by pointing at whichever of its two copies is nearer.
//
// **The fold is its own inverse**, since applying it twice adds W and W ≡ 0.
// That is what makes `SDL_WarpMouseInWindow` safe to route through the same
// function: X4 warps to a logical position, the shim folds it to the real one,
// and the position X4 reads back folds to what it asked for. A separate inverse
// would be a second thing to keep in step with this one.
//
// Off by default. This changes where every click lands, so it gets proven
// against a run with it off before it becomes the default.
bool input_fold() {
    static const bool on = [] {
        const char *e = getenv("X4VR_INPUT_FOLD");
        return e && *e && *e != '0';
    }();
    return on;
}

// X4's own window width, captured from the calls it makes rather than assumed.
// gamescope asks for the same information about its own 2816-wide surface, so
// this is recorded only when the caller is the game.
std::atomic<int> g_x4_win_w{0};

// Mouse-look. X4 switches the pointer to relative mode when it takes the
// camera, and in that mode positions are not what it steers by -- it integrates
// xrel/yrel, which the fold must never touch. Folding a position X4 is ignoring
// would be harmless; folding one it is not would not be, and the flag costs a
// hook X4 already calls.
std::atomic<bool> g_relative_mouse{false};

// The channel itself. One instance per process; the layer resolves the accessor
// by name. Function-local static so it is constructed on first use rather than
// racing other translation units' initialisers.
extern "C" x4vr::Shared *x4vr_shared_state() {
    static x4vr::Shared state;
    return &state;
}

// The other direction, #33: the layer writes the head pose here and the SDL
// hooks below read it. Owned by the injector for the reason x4vr_share.hpp
// gives -- only the LD_PRELOADed side has globally visible symbols.
extern "C" x4vr::HeadShared *x4vr_head_state() {
    static x4vr::HeadShared state;
    return &state;
}

namespace {

// True only while a located head pose is arriving and head-look is armed.
std::atomic<bool> g_headlook_active{false};
// The integrator. Single-threaded by construction: only the SDL hooks touch
// it, and X4 polls input from one thread.
x4vr::HeadLook g_headlook;

bool headlook_key_held() {
    // Only while the head is actually driving. Holding X4's free-look modifier
    // when nothing is publishing a pose would leave the game in mouselook with
    // no one steering it.
    return g_headlook_active.load(std::memory_order_relaxed);
}

// The scancode our rebound free-look key sits on. Must agree with
// x4vr::headlook_code(); SDL_SCANCODE_F13 is 104 and X4's INPUT_KEYCODE_F13 is
// the same physical key. Declared here rather than included, because this file
// deliberately has no SDL headers -- SDL2 and SDL3 share this process tree.
int headlook_scancode() {
    static const int sc = [] {
        const char *e = getenv("X4VR_HEADLOOK_SCANCODE");
        return e && *e ? atoi(e) : 104; // SDL_SCANCODE_F13
    }();
    return sc;
}

} // namespace

// Publish the pointer. Called from the position channel X4 actually reads, so
// what the layer draws and what X4 hit-tests are the same number by
// construction rather than by two paths agreeing.
//
// **Publishes the value X4 receives, fold included.** The first version here
// published the raw SDL position instead, reasoning that the fold is a
// flatscreen ergonomic and the channel should not carry a workaround it
// replaces. That had it backwards. The field means "where in X4's frame does X4
// believe the pointer is", and when the fold is on, that *is* the folded value
// -- X4 never sees the other one. Publishing the raw position would put the
// drawn cursor 704 px from the button it activates, with both features working
// exactly as designed.
//
// So the two compose instead of conflicting, and the invariant is structural:
// whatever number leaves this function for X4 is the number the layer draws at.
void publish_cursor(float x, float y, bool visible) {
    x4vr::Shared *s = x4vr_shared_state();
    const uint32_t start = s->seq.load(std::memory_order_relaxed);
    s->seq.store(start + 1, std::memory_order_relaxed); // odd: writing
    // The fence is the whole correctness of this. A *release store* on seq
    // would only stop earlier writes sinking below it -- the payload writes
    // come after, and nothing would stop them being hoisted above the odd
    // store, which lets a reader see an even seq either side of a half-written
    // pair. The first version here did exactly that.
    //
    // **Not observed, and not observable on this machine.** x86 is
    // store-ordered, so the wrong version does not actually tear here; a
    // negative control that reintroduced it still passed. The fix is for
    // correctness on weakly-ordered hardware, not for a bug that was measured.
    // Said plainly because a run of this project's mistakes have come from
    // recording a fix as "caught by a test" when the test could not have caught
    // it -- a tear that did show up in testing turned out to be the test's own
    // seeding, not the writer.
    std::atomic_thread_fence(std::memory_order_release);
    s->cursor_x.store(x, std::memory_order_relaxed);
    s->cursor_y.store(y, std::memory_order_relaxed);
    s->cursor_visible.store(visible ? 1u : 0u, std::memory_order_relaxed);
    s->window_w = (uint32_t)g_x4_win_w.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->seq.store(start + 2, std::memory_order_release); // even: settled
}

// SDL3's SDL_Surface, read positionally for the same reason SDL_Event is: the
// injector has no SDL headers and wants none. Layout, from SDL 3's public
// struct, on LP64:
//
//     0  SDL_SurfaceFlags flags   (Uint32)
//     4  SDL_PixelFormat  format  (enum, 4 bytes)
//     8  int w
//    12  int h
//    16  int pitch
//    20  (4 bytes of padding, so the pointer is 8-aligned)
//    24  void *pixels
//
// Every field read from it is validated below rather than trusted. If SDL ever
// moves one, the numbers stop being plausible and the capture refuses -- which
// is a great deal better than compositing whatever happened to be at offset 24.
struct Sdl3Surface {
    uint32_t flags;
    uint32_t format;
    int32_t w, h, pitch;
    uint32_t pad;
    const void *pixels;
};

// Cursors X4 has built, so SDL_SetCursor can publish the one it selects. X4
// swaps between a handful (the reticle and the arrow at least), and the handle
// is the only thing SDL_SetCursor gives us.
struct CapturedCursor {
    const void *handle = nullptr;
    uint32_t w = 0, h = 0, format = 0, pitch = 0;
    int32_t hot_x = 0, hot_y = 0;
    std::vector<uint8_t> px;
};
std::mutex g_cursor_mu;
std::vector<CapturedCursor> g_cursors;

void publish_cursor_image(const CapturedCursor &c) {
    x4vr::Shared *s = x4vr_shared_state();
    const uint32_t start = s->cursor_img_seq.load(std::memory_order_relaxed);
    s->cursor_img_seq.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->cursor_w = c.w;
    s->cursor_h = c.h;
    s->cursor_hot_x = c.hot_x;
    s->cursor_hot_y = c.hot_y;
    s->cursor_format = c.format;
    s->cursor_pitch = c.w * 4u; // repacked tightly by the capture below
    const size_t n = c.px.size() < sizeof(s->cursor_pixels) ? c.px.size()
                                                            : sizeof(s->cursor_pixels);
    for (size_t i = 0; i < n; i++)
        s->cursor_pixels[i] = c.px[i];
    std::atomic_thread_fence(std::memory_order_release);
    s->cursor_img_seq.store(start + 2, std::memory_order_release);
}

// Task #17, step 3. Once the layer draws the pointer into the eye image, the
// one the compositor draws is a second cursor -- in a different coordinate
// system, at a different place, in only one of the two halves.
//
// **`SDL_SetCursor` is the only lever X4 gives us.** `nm -D` on X4 says it
// imports exactly six mouse entry points -- CreateColorCursor, GetMouseState,
// SetCursor, SetWindowMouseGrab, SetWindowRelativeMouseMode, WarpMouseInWindow
// -- and neither SDL_ShowCursor nor SDL_HideCursor is among them. That was
// checked before this was written, because four instruments in this project
// have been hooked to symbols X4 never calls and each cost a run to discover.
//
// It also means nothing will undo this: X4 cannot re-show a cursor through a
// function it never calls. We resolve SDL_HideCursor ourselves -- it is exported
// by the libSDL3 X4 ships (3.2.28), whether or not X4 references it -- and SDL
// then tells the compositor there is no cursor to draw.
//
// Patola's "it stops being drawn if I hold still" is gamescope's own
// --hide-cursor-delay, which is more evidence that the pointer on screen is
// gamescope's and not something in X4's frame.
// **On by default, and it stands down only on declared intent.**
//
// The tempting design was to have the layer report "I am drawing" and hide only
// then. That is a *fallback*, and it is the wrong thing while the mod is still
// being debugged: if the overlay fails to build, gamescope's pointer quietly
// returns, the game looks fine, and the defect is invisible. This project's
// expensive mistakes are all that shape -- an aggregate that reported no defect
// on a 2.4x shading error, a predicate that masked its own suspect, a metric
// that could not tell correct stereo from broken.
//
// So the rule is: **gate on intent, never on outcome.**
//
//   * intent -- did this run ask for a drawn cursor? -- is configuration, and
//     standing down for it conceals nothing, because a run that never asked for
//     one has no defect to conceal;
//   * outcome -- did the draw succeed? -- is a fallback, and it converts a
//     defect into a working-looking system.
//
// The case that stays loud is exactly the one worth being loud: layer on, SBS
// on, cursor on, overlay failed to build -> no pointer at all, with the reason
// on the line above it in the log. That is escapable (the keyboard still works)
// and diagnosable (every failure path names itself), which is what makes
// failing early a fair trade here rather than a trap.
//
// **Decided 2026-08-08, for the development phase.** It should flip to the
// outcome-gate once anyone other than Patola runs this: then a silent fallback
// to the compositor's pointer protects a user instead of hiding a bug. See
// docs/known-good-runs.md.
bool hide_cursor_wanted() {
    const char *why = nullptr;
    if (!x4vr::env_on("X4VR_HIDE_CURSOR", true))
        why = "X4VR_HIDE_CURSOR=0";
    else if (x4vr::env_on("X4VR_NO_LAYER", false))
        why = "X4VR_NO_LAYER=1 — nothing is loaded that could draw one";
    else if (!x4vr::env_on("X4VR_SBS", false))
        why = "no side-by-side composite (X4VR_SBS is off)";
    else if (!x4vr::env_on("X4VR_SBS_SPLIT", true))
        why = "X4VR_SBS_SPLIT=0 — X4 renders full width, so there is no eye "
              "image to draw into";
    else if (!x4vr::env_on("X4VR_CURSOR", true))
        why = "X4VR_CURSOR=0 — the overlay is off";
    if (!why)
        return true;
    // Every one of these reads a variable the *layer* reads too, through the
    // same env_on so the two cannot disagree about what "on" means.
    X4VR_LOG("sdl: leaving the compositor's pointer alone — %s", why);
    return false;
}

void hide_sdl_cursor() {
    static const bool want = hide_cursor_wanted();
    if (!want)
        return;
    static bool done = false;
    if (done)
        return;
    auto fn = real<bool (*)(void)>("SDL_HideCursor");
    if (!fn) {
        done = true; // asking again every frame would not make it appear
        X4VR_LOG("sdl: SDL_HideCursor not found — cannot suppress the "
                 "compositor's pointer, expect two cursors");
        return;
    }
    done = true;
    const bool ok = fn();
    X4VR_LOG("sdl: SDL_HideCursor() -> %d — the compositor's pointer is "
             "suppressed; the one you see is drawn by the layer",
             (int)ok);
}

// The origin term is the display x at which X4's surface starts, because the
// fold is really `x_x4 = (x_sdl + origin) mod W` -- undoing "where on the
// display is this pointer" back to "where in X4's frame".
//
// It defaults to W/2, and that default encodes a geometry rather than a law:
// a 2W-wide side-by-side composite with X4's W-wide surface centred in it,
// which is what this project runs. **1408x1408 is this machine's convenience,
// not the mod's.** An eye is whatever size the headset asks for, it need not
// be square, and it will differ per user -- so W is read from the
// SDL_GetWindowSize calls X4 makes, and the origin is overridable rather than
// computed from an assumed layout.
//
// X4VR_INPUT_FOLD_ORIGIN sets it explicitly for any composite that is not
// centred-2W. Only x is folded: a side-by-side layout duplicates horizontally,
// so y is 1:1. An over-under layout would need the same treatment on y, and
// would be a change here rather than a different value.
int fold_origin(int w) {
    static const int forced = [] {
        const char *e = getenv("X4VR_INPUT_FOLD_ORIGIN");
        return e && *e ? atoi(e) : -1;
    }();
    return forced >= 0 ? forced : w / 2;
}

float fold_x(float x) {
    const int w = g_x4_win_w.load(std::memory_order_relaxed);
    if (!input_fold() || w <= 1 || g_relative_mouse.load(std::memory_order_relaxed))
        return x;
    const int origin = fold_origin(w);
    float f = fmodf(x + (float)origin, (float)w);
    if (f < 0.f)
        f += (float)w;
    static bool said = false;
    if (!said) {
        said = true;
        X4VR_LOG("sdl: input fold ON — x_x4 = (x_sdl + %d) mod %d "
                 "(W from X4's own window; origin %s)",
                 origin, w,
                 getenv("X4VR_INPUT_FOLD_ORIGIN") ? "forced" : "assumed W/2");
    }
    return f;
}

// The question is the *range* X4 is handed, so measure the range.
//
// Take 88 sampled the first twelve changed positions instead, and every one of
// them landed in a 0.2s burst on the first mouse twitch, 97 seconds into a
// 123-second run: largest value 17, with the deliberate sweep to the screen
// edge happening long after the budget was gone. An instrument for "how far
// does it go" must not spend itself on "where did it start".
//
// Logging on a *new extreme* is self-limiting -- the range only ever widens, so
// this settles to silence on its own -- and it captures the edges, which is the
// only part of the range that discriminates 0..1408 from 0..2816.
void note_extent(const char *what, float x, float y) {
    static float lo_x = 1e9f, hi_x = -1e9f, lo_y = 1e9f, hi_y = -1e9f;
    static int logged = 0;
    const bool wider = x < lo_x || x > hi_x || y < lo_y || y > hi_y;
    if (!wider || logged >= 64)
        return;
    lo_x = x < lo_x ? x : lo_x;
    hi_x = x > hi_x ? x : hi_x;
    lo_y = y < lo_y ? y : lo_y;
    hi_y = y > hi_y ? y : hi_y;
    logged++;
    X4VR_LOG("sdl: %s extent x=%.0f..%.0f y=%.0f..%.0f (now %.0f,%.0f)", what,
             lo_x, hi_x, lo_y, hi_y, x, y);
}

// Every key event X4 drains, capped so the log stays readable. Take 121 left
// two possibilities standing that no amount of reasoning could separate: our
// synthetic event never reaching X4's queue, or reaching it and not matching a
// binding. A real press logged in the same format next to ours settles it by
// comparison rather than by argument -- which is what should have happened
// before the keycode was guessed at, and again before the scancode was.
void note_key_event(const Sdl3KeyEvent *k, const char *via, int action) {
    // Separate budgets. Take 122's cap was spent entirely on our own key
    // repeating at ~2 Hz, so not one of Patola's real presses was logged and
    // the comparison this exists for could not be made. An instrument that
    // crowds out its own subject is the take-115 range ceiling again: a few of
    // ours is proof it arrives, and the rest of the budget belongs to the keys
    // being compared against.
    // Ours and a REAL press of the same key are indistinguishable by
    // scancode, so the budget is split by timestamp instead: SDL stamps a
    // genuine event, and ours goes in as zero unless SDL fills it.
    const bool ours = k->reserved == kOurEventMagic;
    // `raw` is the platform scancode -- the evdev code on Linux -- and ours is
    // zero because we have no way to derive it. It is the last field that still
    // differs from a real press, so a real one of OUR key donates it, exactly
    // as a real motion event donates windowID.
    if (!ours && (int)k->scancode == headlook_scancode() && k->raw &&
        !g_key_raw_seen.exchange(true, std::memory_order_relaxed)) {
        g_key_raw.store(k->raw, std::memory_order_relaxed);
        X4VR_LOG("headlook: sampled raw=%u from a real press of scancode %d; "
                 "synthetic keys will carry it",
                 (unsigned)k->raw, headlook_scancode());
    }
    static int seen_ours = 0, seen_other = 0;
    if (ours) {
        if (seen_ours >= 3)
            return;
        seen_ours++;
    } else {
        if (seen_other >= 40)
            return;
        seen_other++;
    }
    // timestamp and action are the two fields take 123 could not rule out.
    // A zero timestamp is the obvious way a synthetic event differs from a real
    // one even when every other field matches, and X4 may treat it as stale;
    // action distinguishes a peek (1) from a consuming get (2), and a binding
    // that only fires on the latter would look exactly like this.
    X4VR_LOG("key[%s%s] type=0x%x scancode=%u keycode=0x%x mod=0x%x which=%u "
             "window=%u down=%d repeat=%d raw=%u ts=%llu action=%d",
             via, ours ? ":OURS" : "", k->type, k->scancode, k->key, k->mod, k->which, k->windowID,
             (int)k->down, (int)k->repeat, (unsigned)k->raw,
             (unsigned long long)k->timestamp,
             action);
    (void)ours;
}

void note_mouse_event(const Sdl3MouseEvent *e) {
    // #33: the first real motion event donates the two fields our synthetic
    // ones must carry. Done here because every mouse event already flows
    // through this function.
    if (e->type == SDL_EV_MOUSE_MOTION &&
        !g_motion_seen.load(std::memory_order_relaxed)) {
        g_motion_window.store(e->windowID, std::memory_order_relaxed);
        g_motion_which.store(e->which, std::memory_order_relaxed);
        g_motion_seen.store(1, std::memory_order_relaxed);
        X4VR_LOG("headlook: sampled a real motion event — window %u, mouse %u; "
                 "synthetic events will carry the same",
                 e->windowID, e->which);
    }
    static int motions = 0, buttons = 0;
    const bool is_motion = e->type == SDL_EV_MOUSE_MOTION;
    // ABOVE the sample caps, deliberately. Putting it below would let the
    // eight-motion budget switch off the instrument that measures the range --
    // the same shape as the bindless counter that sat under the gate removing
    // its own population, and the reason take 88 measured a maximum of 17.
    note_extent(is_motion ? "motion" : "button", e->x, e->y);

    // Is the pointer *confined* to X4's surface, or does it simply leave and
    // stop being reported? Task #31 assumed the former and never tested it, and
    // the answer decides what that task is: defeating a confinement, or moving
    // a surface.
    //
    // The two differ observably. If something pins the pointer to the surface,
    // pushing past the edge keeps delivering motion with non-zero xrel while x
    // stays at the wall -- you are pushing against something. If instead the
    // pointer is free and Wayland just stops sending events once it is off the
    // surface, motion ceases entirely and x freezes because nothing updates it.
    //
    // Raw SDL values: the fold is applied by the callers *after* this runs.
    if (is_motion) {
        const int w = g_x4_win_w.load(std::memory_order_relaxed);
        if (w > 1) {
            // Per-wall budgets. Take 91 used one counter for both and spent it
            // entirely on the left wall before the right wall was ever reached,
            // so the right wall's silence was a budget artifact and not a
            // measurement -- the same mistake as take 88's sampler, made again
            // in the probe written to explain it. A shared budget between two
            // things being compared cannot compare them.
            const bool left = e->x <= 0.5f;
            const bool right = e->x >= (float)(w - 1) - 0.5f;
            static int pins_l = 0, pins_r = 0, frees = 0;
            int &pins = left ? pins_l : pins_r;
            if ((left || right) && fabsf(e->xrel) > 0.5f && pins < 6) {
                pins++;
                X4VR_LOG("sdl: wall push %s#%d — x=%.0f pinned, xrel=%.1f still "
                         "arriving: this wall CONFINES",
                         left ? "LEFT " : "RIGHT ", pins, e->x, e->xrel);
            } else if (!left && !right && frees < 1) {
                frees++;
                X4VR_LOG("sdl: motion away from the walls (x=%.0f) — the probe "
                         "is live, so a later absence of wall pushes is real",
                         e->x);
            }
        }
    }
    if (is_motion && motions >= 8)
        return;
    if (!is_motion && buttons >= 6)
        return;
    (is_motion ? motions : buttons)++;
    X4VR_LOG("sdl: mouse %s x=%.1f y=%.1f xrel=%.1f yrel=%.1f win=%u",
             is_motion ? "motion" : (e->type == SDL_EV_MOUSE_DOWN ? "down" : "up"),
             e->x, e->y, is_motion ? e->xrel : 0.f, is_motion ? e->yrel : 0.f,
             e->windowID);
}

// SDL_Window is opaque here on purpose -- the injector has no SDL headers and
// needs none; the handle is only ever passed straight through.
// ------------------------------------------------------------------ Lua
//
// Reconnaissance for #33's open loop: see injector/x4vr_lua.hpp for why this is
// the route and why it is the only one that keeps saves unmodified.
//
// **`static`, not an anonymous namespace.** The first version of this block
// opened one and closed it in the wrong place, which silently moved every real
// interposer -- fopen, open, the whole SDL family -- inside it, where internal
// linkage meant they interposed nothing at all. The injector then did nothing
// but log Lua for a whole run. Nothing in this file needs a namespace it does
// not already have; `static` gives internal linkage without a brace to get
// wrong.
static bool lua_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_LUA");
        return e && *e && *e != '0';
    }();
    return on;
}

// Two lessons from #33 are built in rather than learned again. **An import is
// not a call** -- X4 imports SDL_GetKeyboardState and never calls it -- so the
// first firing of each hook is logged before anything is built on it. And an
// instrument must not flood its own log, which cost takes 119 and 122: chunk
// names are capped, and the keyword scan reports the chunk rather than the
// match.
// Where to write chunks that hit a probe, or empty for none.
//
// **/tmp only, and never committed.** X4's Lua is Egosoft's copyrighted source,
// exactly like the shader modules this project already dumps and has always
// kept out of the tree. The knob takes a directory so the path is the caller's
// choice and nothing defaults into the repo.
static const char *lua_dump_dir() {
    static const char *d = [] {
        const char *e = getenv("X4VR_LUA_DUMP");
        return (e && *e) ? e : "";
    }();
    return d;
}

// Chunk names are paths; flatten them so one directory holds the lot.
static void lua_dump_chunk(const char *name, const char *buff, size_t sz) {
    const char *dir = lua_dump_dir();
    if (!*dir || !buff || !sz)
        return;
    std::string flat(name ? name : "anon");
    for (char &c : flat)
        if (c == '/' || c == '\\' || c == '@' || c == '=')
            c = '_';
    // Make the directory ourselves. Take 133 asked Patola to mkdir it first and
    // he had no reason to remember, so 24 chunks hit and nothing was written --
    // the same shape as gating head-look arming on a mouse nudge. A tool should
    // not put a precondition on the person running it when it can satisfy it.
    static bool made = false;
    if (!made) {
        made = true;
        if (::mkdir(dir, 0755) != 0 && errno != EEXIST)
            X4VR_LOG("lua: WARNING cannot create %s (%s) — nothing will be "
                     "dumped",
                     dir, strerror(errno));
    }

    const std::string path = std::string(dir) + "/" + flat;
    // write_file() goes through open()/write() rather than fopen(), which
    // matters here for the same reason it does for the config profile: fopen is
    // interposed by this very file.
    if (x4vr::write_file(path.c_str(), std::string(buff, sz)))
        X4VR_LOG("lua: dumped %s (%zu bytes)", path.c_str(), sz);
    else
        // Reported, because take 133 failed here in total silence: 24 hits, 0
        // dumps, and a log that read as if nothing had gone wrong.
        X4VR_LOG("lua: WARNING could not write %s (%s)", path.c_str(),
                 strerror(errno));
}

static void note_lua_chunk(const char *buff, size_t sz, const char *name,
                           const char *via) {
    static int chunks = 0, hits = 0;
    const char *n = x4vr::lua_chunk_name(name);
    if (chunks < 400) {
        chunks++;
        X4VR_LOG("lua[%s] %s (%zu bytes)", via, n, sz);
    } else if (chunks == 400) {
        chunks++;
        X4VR_LOG("lua: 400 chunk names logged, capping — the keyword scan below "
                 "keeps running");
    }
    if (!buff || hits >= 60)
        return;
    size_t np = 0;
    const x4vr::LuaProbe *probes = x4vr::lua_probes(&np);
    for (size_t i = 0; i < np && hits < 60; i++) {
        if (!x4vr::lua_chunk_mentions(buff, sz, probes[i].needle))
            continue;
        hits++;
        X4VR_LOG("lua HIT %-14s in %s  (%s)", probes[i].needle, n, probes[i].why);
        static std::string last_dumped;
        if (last_dumped != n) { // one dump per chunk, not one per probe
            last_dumped = n;
            lua_dump_chunk(n, buff, sz);
        }
    }
}

// LuaJIT is Lua 5.1: int luaL_loadbuffer(lua_State*, const char*, size_t,
// const char*). lua_State is opaque here, as SDL_Window is elsewhere in this
// file -- the injector has no Lua headers and does not need them.
extern "C" int luaL_loadbuffer(void *L, const char *buff, size_t sz,
                               const char *name) {
    static auto real_fn =
        real<int (*)(void *, const char *, size_t, const char *)>(
            "luaL_loadbuffer");
    if (lua_enabled() && this_is_the_game()) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("lua: luaL_loadbuffer IS called — X4's Lua is reachable "
                     "from here");
        }
        note_lua_chunk(buff, sz, name, "buf");
    }
    return real_fn(L, buff, sz, name);
}

extern "C" int luaL_loadfile(void *L, const char *filename) {
    static auto real_fn = real<int (*)(void *, const char *)>("luaL_loadfile");
    if (lua_enabled() && this_is_the_game()) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("lua: luaL_loadfile IS called — chunks also arrive by path");
        }
        note_lua_chunk(nullptr, 0, filename, "file");
    }
    return real_fn(L, filename);
}

// ---- X4's own camera, read back -------------------------------------------
//
// `Rotation GetCameraRotation();` is EXPORTED from the X4 binary (nm -D, a T
// symbol at 0xa41610), and the Lua bytecode carries its FFI declaration:
//
//     typedef struct { float yaw; float pitch; float roll; } Rotation;
//
// Field order taken from the declaration itself, not from a sorted listing --
// getting that backwards is exactly the positional error this project keeps
// making.
//
// **This is what #33 has been missing.** Every remaining head-look defect is one
// defect: we drive open-loop and cannot ask X4 where its camera points. The
// unmeasured gain, the real windup at X4's own clamp, and the undetected
// recentre on a seat change or save load are all that same blindness, and a
// readback makes all three self-correcting at once.
//
// The Lua reconnaissance found this and is not needed to use it: the function is
// a plain exported symbol, so no chunk is injected and no game file is touched.
// There is no SetCameraRotation -- the exported setters switch view MODE
// (cockpit, external, target), not orientation -- so this is a readback and not
// direct control.
//
// **Logged before it is trusted.** Which frame it reports in, degrees or
// radians, and whether it follows free-look at all are unknown; nothing here
// assumes an answer. Gated behind X4VR_CAMREAD so calling an unknown game
// function every frame is opt-in and reversible.
struct X4Rotation {
    float yaw, pitch, roll;
};

// X4 reports radians; every angle on our side is degrees.
static constexpr float kRad2Deg = 57.2957795130823f;

// Whether the readback DRIVES the loop or is merely reported. Separate from
// X4VR_CAMREAD so a run can observe without changing behaviour, which is how
// take 135 established the units without risking anything.
static const bool g_camloop = [] {
    const char *e = getenv("X4VR_CAMLOOP");
    return e && *e && *e != '0';
}();

static bool camread_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_CAMREAD");
        return e && *e && *e != '0';
    }();
    return on;
}

// False if the symbol is absent, which is not an error: a future X4 could stop
// exporting it and the open-loop integrator still works without it.
static bool x4_camera_rotation(X4Rotation *out) {
    using Fn = X4Rotation (*)();
    static Fn fn = [] () -> Fn {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
        X4VR_LOG("camread: GetCameraRotation %s",
                 f ? "resolved — X4's camera can be read back"
                   : "NOT FOUND — staying open-loop");
        return f;
    }();
    if (!fn)
        return false;
    *out = fn();
    return true;
}

// ------------------------------------------------------- #33 head-look
//
// X4 is SDL3 and imports SDL_GetKeyboardState, so the held free-look key is a
// *polled state array with one bit forced* -- no event synthesis, no key-up to
// lose, and self-healing by construction, which is exactly what the
// hold-not-toggle decision needs. `nm -D` first, per this project's rule about
// hooking symbols the target never imports; X4 does not import SDL_PollEvent,
// which is what the dead hook described near the top of this file was.
//
// **The mouse path is measured before it is built.** X4 imports BOTH
// SDL_GetMouseState + SDL_WarpMouseInWindow (the warp-to-centre idiom, where a
// delta is the absolute position minus the centre) and
// SDL_SetWindowRelativeMouseMode (where motion arrives as events instead).
// Those need opposite implementations, and the XML gives no way to tell which
// X4 uses for free-look. So this run logs, and the next one implements the one
// that fires. Guessing here would mean writing an SDL_Event by offset against
// a header this file deliberately does not include.
// Defined below with the rest of the mouse half; declared here because the
// tick is what drives it and reads better next to the arithmetic.
void send_mouse_delta(int dx, int dy);
void send_key_down(int scancode);
void send_key(int scancode, bool down);

void headlook_tick() {
    static const bool armed = [] {
        const char *e = getenv("X4VR_HEADLOOK");
        return e && *e && *e != '0';
    }();
    if (!armed || !this_is_the_game())
        return;

    static x4vr::HeadShared *hs = x4vr_head_state();
    static const float gain = [] {
        const char *e = getenv("X4VR_HEADLOOK_GAIN");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    static const bool once = [] {
        X4VR_LOG("headlook: armed, scancode %d, gain %s", headlook_scancode(),
                 getenv("X4VR_HEADLOOK_GAIN")
                     ? getenv("X4VR_HEADLOOK_GAIN")
                     : "UNSET — commands nothing until calibrated");
        return true;
    }();
    (void)once;
    g_headlook.gain_deg_per_count = gain;
    static const float kp = [] {
        const char *e = getenv("X4VR_HEADLOOK_KP");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    if (kp > 0.0f)
        g_headlook.servo_kp = kp;
    static const float gpitch = [] {
        const char *e = getenv("X4VR_HEADLOOK_GAIN_PITCH");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    g_headlook.gain_pitch_deg_per_count = gpitch;

    float yaw = 0.0f, pitch = 0.0f;
    uint64_t updates = 0;
    if (!x4vr::head_share_read(hs, &yaw, &pitch, &updates)) {
        if (g_headlook_active.exchange(false, std::memory_order_relaxed))
            send_key(headlook_scancode(), false);
        return;
    }
    // **Once per published pose, not once per call.** Take 119 flooded SDL's
    // event queue: SDL_PeepEvents runs far more often than once a frame -- ~18
    // thousand times a second measured -- and send_key_down had no gate, so
    // roughly 3.2 million key events went into the queue over three minutes.
    // X4 toggled relative mouse mode on and off forty times and the camera
    // moved 3.6 degrees in total. The arithmetic was never the problem; the
    // rate was.
    //
    // Gating on the layer's `updates` counter rather than on a timer ties the
    // tick to the thing that actually changed -- one command per new head pose,
    // which is the headset's frame rate and nothing faster.
    //
    // "Held still" and "the layer stopped publishing" look identical in the
    // angles and want opposite responses, which is the second thing `updates`
    // is for.
    static uint64_t last_updates = 0;
    static int stale = 0;
    const bool fresh = updates != last_updates;
    stale = fresh ? 0 : stale + 1;
    last_updates = updates;
    if (stale > 5000) {
        if (g_headlook_active.exchange(false, std::memory_order_relaxed))
            send_key(headlook_scancode(), false);
        return;
    }
    g_headlook_active.store(true, std::memory_order_relaxed);
    if (!fresh)
        return; // nothing new to act on; acting anyway is what take 119 did

    // Observed BEFORE the step, so the servo acts on this frame's reading.
    // It used to run after, which cost a frame of lag for no reason.
    X4Rotation r{};
    const bool have_cam = camread_enabled() && x4_camera_rotation(&r);
    if (have_cam && g_camloop) {
        x4vr::head_look_observe(g_headlook, kRad2Deg * -r.yaw, kRad2Deg * r.pitch);
    }

    // **Never keep pushing a camera that does not answer.**
    //
    // Opening the map made the view rotate, slowly at first and then faster and
    // faster. GetCameraRotation reports 0.00,0.00 outside the cockpit -- the
    // menus in take 137 show it -- so the servo saw a constant error, commanded
    // a correction every frame forever, and whatever DOES consume mouse deltas
    // in the map accelerated away. A servo with no feedback on the thing it is
    // actually moving is an accelerator, and it is not enough that the number we
    // read is a camera; it has to be the camera we are driving.
    //
    // So: if we command real deltas and the observation does not move, stop
    // commanding. Resume when the observation changes on its own, which is the
    // signal that a live context has come back -- probing for it by sending
    // deltas would be the same mistake in miniature.
    static float last_obs_yaw = 0.0f, last_obs_pitch = 0.0f;
    static int stall = 0;
    static bool stalled = false;
    const float obs_moved = std::fabs(kRad2Deg * -r.yaw - last_obs_yaw) +
                            std::fabs(kRad2Deg * r.pitch - last_obs_pitch);
    last_obs_yaw = kRad2Deg * -r.yaw;
    last_obs_pitch = kRad2Deg * r.pitch;

    const x4vr::Delta d = x4vr::head_look_step(g_headlook, {yaw, pitch});

    // Reported whether or not anything is sent yet, because this line is how
    // the calibration run recovers the gain: a known command against the angle
    // camera_rotation.py --integrate measures.
    // Re-asserted every tick. X4 never called SDL_GetKeyboardState in take
    // 118, so the hold is an event, and an event has to be repeated to
    // survive anything that resets X4's own key state.
    // The key only has to keep X4's held state alive, so it is re-asserted on a
    // slow cadence rather than every frame: one every 32 poses is ~3 Hz at
    // headset rate, enough to recover from a save-load reset within a blink and
    // far too slow to be a flood.
    // **Activation needs an edge; sustaining needs a level.** Take 126 measured
    // both halves. Patola's tap was a down and an up three MICROseconds apart,
    // and mouselook then stayed on for 32.7 seconds -- our repeated downs held
    // it. But our downs had been arriving for 100 seconds before that tap
    // without ever switching it on.
    //
    // The reading that fits: our first down lands during loading or a menu,
    // when free-look is not allowed. X4 records the key as held, discards it,
    // and because we never send an up it never sees a fresh down-edge once the
    // cockpit is ready. A level cannot re-arm what a level already set.
    //
    // So while not steering, re-assert as up-then-down to manufacture a fresh
    // edge each cycle; once steering, send only downs, because an up would
    // release the very state we are holding.
    const bool armed_now = g_relative_mouse.load(std::memory_order_relaxed);
    static uint64_t poses = 0;
    if ((poses++ % 32) == 0) {
        if (!armed_now)
            send_key(headlook_scancode(), false);
        send_key_down(headlook_scancode());
    }
    // Only while X4 is actually in mouselook. Take 126 ran the immersive
    // treatment over load screens and menus too, where it does not belong:
    // there is no camera to follow, so the flat panel tilts with the head and
    // Patola asked for the quad back. Worse, mouse deltas sent outside
    // mouselook are cursor movement -- we would be dragging the pointer around
    // a menu we cannot see.
    //
    // g_relative_mouse is the signal, and it costs nothing: X4 turns relative
    // mode on for mouselook and off for anything with a pointer, and the cursor
    // shim has tracked it since #19.
    const bool commanding = (d.dx != 0 || d.dy != 0);
    if (stalled) {
        if (obs_moved > 0.05f) { // it answered on its own: a live context
            stalled = false;
            stall = 0;
            X4VR_LOG("headlook: camera responding again — steering resumed");
        }
    } else if (commanding && obs_moved < 0.01f) {
        if (++stall > 60) {
            stalled = true;
            X4VR_LOG("headlook: commanded %d frames with no camera response — "
                     "steering stood down (map or menu?)",
                     stall);
        }
    } else {
        stall = 0;
    }

    const bool steering =
        g_relative_mouse.load(std::memory_order_relaxed) && !stalled;
    g_headlook_steering.store(steering, std::memory_order_relaxed);
    if (steering)
        send_mouse_delta(d.dx, d.dy);

    // Tell the layer where X4 is now looking, so the frame is submitted with
    // the pose it was rendered from rather than with identity. Cleared when not
    // steering, which puts the layer back to the world-locked quad.
    if (x4vr::Shared *sh = x4vr_shared_state()) {
        sh->cam_yaw_deg.store(g_headlook.cmd_yaw_deg, std::memory_order_relaxed);
        sh->cam_pitch_deg.store(g_headlook.cmd_pitch_deg,
                                std::memory_order_relaxed);
        sh->cam_valid.store(steering ? 1u : 0u, std::memory_order_relaxed);
    }

    // The camera stops being ours when X4 stops listening, so the estimate has
    // to go back to where X4 will have left it rather than keeping a belief it
    // can no longer act on. This is the same reset a seat change needs.
    static bool was_steering = false;
    if (was_steering && !steering)
        x4vr::head_look_recentre(g_headlook);
    was_steering = steering;

    static uint64_t n = 0;
    // Every 30 poses, not 240. Take 129 was a clean controlled excursion and
    // the gain still could not be read off it, because at one sample in 240 the
    // log missed the plateau entirely: cmd appeared to peak at 12.6 deg while
    // the dumps showed 36. A calibration run whose instrument samples slower
    // than the thing it measures is not a calibration run.
    // **Close the loop.** Take 135 established what GetCameraRotation reports:
    // RADIANS, with yaw of the opposite sign to ours, linear to within 0.8% over
    // a 30 degree excursion. So the estimate stops being a belief.
    //
    // With an observation in hand the three defects that outlasted every patch
    // all go at once. The gain only has to be roughly right, because a servo
    // corrects what it gets wrong. The clamp is SEEN rather than modelled, so
    // there is no windup to discharge as a jolt. And a recentre on a seat change
    // or save load fixes itself on the next frame instead of leaving a permanent
    // offset nothing could detect.
    if (have_cam) {
        static uint64_t rn = 0;
        if ((rn++ % 30) == 0)
            X4VR_LOG("camread: X4 %.2f,%.2f deg (raw %.4f,%.4f) | cmd %.2f,%.2f "
                     "| head %.2f,%.2f%s",
                     kRad2Deg * -r.yaw, kRad2Deg * r.pitch, r.yaw, r.pitch,
                     g_headlook.cmd_yaw_deg, g_headlook.cmd_pitch_deg, yaw,
                     pitch, g_camloop ? " CLOSED" : " open-loop");
    }

    if ((n++ % 30) == 0 || d.clamped)
        X4VR_LOG("headlook: head %.2f,%.2f -> cmd %.2f,%.2f delta %d,%d%s",
                 yaw, pitch, g_headlook.cmd_yaw_deg, g_headlook.cmd_pitch_deg,
                 d.dx, d.dy, d.clamped ? " CLAMPED" : "");
}

// ---- the mouse half -------------------------------------------------------
//
// Take 117's log already answered which of X4's two mouse idioms free-look
// uses, without a run being spent on it: `relative mouse mode ON` at 962738 and
// off at 962804, a 66-second window that is exactly the clamp walk. In relative
// mode SDL delivers motion as events, and X4 does not import
// SDL_GetRelativeMouseState, so `xrel`/`yrel` on a motion event is the channel.
//
// Sdl3MouseEvent and the SDL_PeepEvents interposer already exist above, from
// the cursor shim -- the layout was established there and is reused rather than
// declared a second time. tests/sdl_event_layout.cpp asserts it against the
// real SDL3 headers, which this file cannot include because SDL2 (gamescope)
// and SDL3 (X4) share this process tree and the hand declarations here would
// collide with the real prototypes.

// Push one synthetic relative motion. Deliberately silent until a real event
// has been seen: sending before then would mean inventing the two fields this
// exists to copy.
// Hold the free-look key by pushing a key-down and never a key-up. X4 tracks
// held state from the event stream, so one down latches it; re-sending is
// harmless (it looks like key repeat) and is what makes the hold self-healing
// across the save-load desync that ruled out toggle="1".
//
// **The `key` field is why take 120 did nothing.** It was left zero, and X4
// works in *keycodes*, not scancodes -- it imports SDL_GetKeyFromScancode and
// SDL_GetKeyName, which is the tell. A zero keycode is a lookup for a binding
// that does not exist, so the event arrived, was well-formed, and meant
// nothing. Relative mouse mode never turned on in take 120 and, on a second
// reading, probably never did in 119 either: forty short on/off pairs look like
// menu activity, not the single 66-second window a real key press produced in
// take 117.
//
// Resolved through SDL rather than hardcoded. SDLK_F13 is 0x40000068 and the
// rule is plainly SDLK_SCANCODE_MASK | scancode, but X4 imports
// SDL_GetKeyFromScancode so it is there to be called, it respects the keyboard
// layout, and a constant here would be a fourth positional assumption to
// maintain.
uint32_t keycode_of(int scancode) {
    static auto from_sc =
        real<uint32_t (*)(uint32_t, uint16_t, bool)>("SDL_GetKeyFromScancode");
    if (from_sc) {
        const uint32_t k = from_sc((uint32_t)scancode, 0, false);
        if (k)
            return k;
    }
    return 0x40000000u | (uint32_t)scancode; // SDLK_SCANCODE_MASK
}

void send_key(int scancode, bool down) {
    if (!g_motion_seen.load(std::memory_order_relaxed))
        return;
    static auto push = real<int (*)(void *)>("SDL_PushEvent");
    if (!push)
        return;
    unsigned char ev[128] = {};
    auto *k = (Sdl3KeyEvent *)ev;
    k->type = down ? SDL_EV_KEY_DOWN : SDL_EV_KEY_UP;
    k->windowID = g_motion_window.load(std::memory_order_relaxed);
    k->scancode = (uint32_t)scancode;
    k->key = keycode_of(scancode);
    k->down = down;
    // Our own marker. `reserved` is untouched by SDL and by X4, and take 124
    // showed the alternatives do not work: scancode is shared with a real press
    // of the same key, and SDL fills in a zero timestamp on push, so both
    // discriminators the log has used so far were wrong.
    k->reserved = kOurEventMagic;
    k->raw = (uint16_t)g_key_raw.load(std::memory_order_relaxed);
    const int r = push(ev);
    static bool said = false;
    if (!said) {
        said = true;
        // SDL_PushEvent returns 1 on success, 0 if an event filter dropped it,
        // negative on error. Ignoring it is how take 121 could not tell "X4
        // received our key and did nothing with it" from "SDL never queued it".
        X4VR_LOG("headlook: pushed scancode %d as keycode 0x%x, window %u -> "
                 "SDL_PushEvent returned %d",
                 scancode, k->key, k->windowID, r);
    }
}

void send_key_down(int scancode) { send_key(scancode, true); }

void send_mouse_delta(int dx, int dy) {
    if ((dx == 0 && dy == 0) || !g_motion_seen.load(std::memory_order_relaxed))
        return;
    static auto push = real<int (*)(void *)>("SDL_PushEvent");
    if (!push)
        return;
    unsigned char ev[128] = {};
    auto *m = (Sdl3MouseEvent *)ev;
    m->type = SDL_EV_MOUSE_MOTION;
    m->windowID = g_motion_window.load(std::memory_order_relaxed);
    m->which = g_motion_which.load(std::memory_order_relaxed);
    m->xrel = (float)dx;
    m->yrel = (float)dy;
    push(ev);
}

// X4 polls the keyboard every frame; this is where the free-look key is held.
// The real array is copied rather than written through: SDL owns that memory
// and other consumers in this process tree read it.
const bool *SDL_GetKeyboardState(int *numkeys) {
    static auto real_fn = real<const bool *(*)(int *)>("SDL_GetKeyboardState");
    int n = 0;
    const bool *src = real_fn(&n);
    if (numkeys)
        *numkeys = n;
    if (!src || !headlook_key_held())
        return src;
    const int sc = headlook_scancode();
    if (sc < 0 || sc >= n)
        return src;
    static std::vector<bool> dummy; // never used; keeps the intent obvious
    (void)dummy;
    static bool *copy = nullptr;
    static int copy_n = 0;
    if (copy_n != n) {
        delete[] copy;
        copy = new bool[n];
        copy_n = n;
    }
    memcpy(copy, src, (size_t)n * sizeof(bool));
    copy[sc] = true;
    return copy;
}

// #33: the window id, without needing the player to move a mouse.
//
// Take 127 exposed this as a design flaw rather than an inconvenience: every
// synthetic event was gated on having sampled a REAL motion event to donate
// windowID, so a player on a HOTAS or a stick -- which is most of X4's cockpit
// audience -- would never arm head-look at all. X4 hands us its window pointer
// on every SDL_GetWindowSize call, and SDL will convert that to an id for free.
void note_window(void *win) {
    if (!win || !this_is_the_game() ||
        g_motion_seen.load(std::memory_order_relaxed))
        return;
    static auto get_id = real<uint32_t (*)(void *)>("SDL_GetWindowID");
    if (!get_id)
        return;
    const uint32_t id = get_id(win);
    if (!id)
        return;
    g_motion_window.store(id, std::memory_order_relaxed);
    g_motion_seen.store(1, std::memory_order_relaxed);
    X4VR_LOG("headlook: window id %u from SDL_GetWindowID — no mouse nudge "
             "needed to arm",
             id);
}

int SDL_GetWindowSize(void *win, int *w, int *h) {
    note_window(win);
    static auto real_fn = real<int (*)(void *, int *, int *)>("SDL_GetWindowSize");
    int r = real_fn(win, w, h);
    const bool cut = halve_window() && this_is_the_game() && w && *w > 1;
    if (cut)
        *w /= 2;
    if (w && h)
        note_window_size("SDL_GetWindowSize", *w, *h, cut);
    if (w && *w > 1 && this_is_the_game())
        g_x4_win_w.store(*w, std::memory_order_relaxed);
    return r;
}

int SDL_GetWindowSizeInPixels(void *win, int *w, int *h) {
    static auto real_fn =
        real<int (*)(void *, int *, int *)>("SDL_GetWindowSizeInPixels");
    int r = real_fn(win, w, h);
    const bool cut = halve_window() && this_is_the_game() && w && *w > 1;
    if (cut)
        *w /= 2;
    if (w && h)
        note_window_size("SDL_GetWindowSizeInPixels", *w, *h, cut);
    return r;
}

// SDL3 returns `bool` from these, not int. The low byte is what matters either
// way, but declaring it correctly costs nothing.
// X4 imports this; whether it CALLS it is the question take 118 taught me not
// to assume, so the first call is logged and the SDL_PeepEvents path stays as a
// fallback until one arrives. Pumping happens before the drain, so events
// pushed here are already queued when X4 starts reading and the drain
// terminates normally.
void SDL_PumpEvents(void) {
    static auto real_fn = real<void (*)()>("SDL_PumpEvents");
    if (this_is_the_game()) {
        if (!g_pump_seen.exchange(true, std::memory_order_relaxed))
            X4VR_LOG("sdl: SDL_PumpEvents is called — head-look drives from "
                     "here, not from inside the drain");
        headlook_tick();
    }
    real_fn();
}

bool SDL_WaitEvent(void *event) {
    static auto real_fn = real<bool (*)(void *)>("SDL_WaitEvent");
    const bool r = real_fn(event);
    if (r && event && this_is_the_game()) {
        auto *e = (Sdl3MouseEvent *)event;
        if (e->type == SDL_EV_KEY_DOWN || e->type == SDL_EV_KEY_UP)
            note_key_event((const Sdl3KeyEvent *)e, "wait", -1);
        if (e->type >= SDL_EV_MOUSE_MOTION && e->type <= SDL_EV_MOUSE_UP) {
            note_mouse_event(e);
            // Position only. xrel/yrel are deltas and a fold of a delta is
            // meaningless -- it is also what X4 steers the camera by.
            e->x = fold_x(e->x);
        }
    }
    return r;
}

// SDL_Event is a fixed 128 bytes in SDL3 -- it is a union with an explicit
// padding[128] member -- so the array can be walked without the headers.
int SDL_PeepEvents(void *events, int numevents, int action, uint32_t minType,
                   uint32_t maxType) {
    static auto real_fn =
        real<int (*)(void *, int, int, uint32_t, uint32_t)>("SDL_PeepEvents");
    const int n = real_fn(events, numevents, action, minType, maxType);
    // SDL_ADDEVENT=0, SDL_PEEKEVENT=1, SDL_GETEVENT=2. Fold only on GET.
    //
    // A peek leaves the event in the queue, so folding there and again when it
    // is finally fetched would apply the transform twice -- and because the
    // fold is its own inverse, that lands exactly back on the unfolded value.
    // The fix would silently do nothing, which is the worst way for it to fail.
    // #33 is driven from here rather than from SDL_GetKeyboardState: take 118
    // showed X4 imports that and never calls it, while this drain runs every
    // input poll. Outside the `n > 0` test on purpose -- head-look must keep
    // running through frames where the queue happens to be empty.
    // Only if X4 never calls SDL_PumpEvents. Pushing an event from inside the
    // drain is what made take 124 lag: X4 loops on SDL_PeepEvents until it
    // returns nothing, and an event pushed during that loop keeps handing it
    // more work, so the queue never empties and input arrives minutes late --
    // which is exactly what "it took some time to activate and kept applying
    // after I released" describes.
    if (this_is_the_game() && !g_pump_seen.load(std::memory_order_relaxed))
        headlook_tick();
    const bool consuming = action == 2;
    if (n > 0 && events && this_is_the_game()) {
        for (int i = 0; i < n; i++) {
            auto *e = (Sdl3MouseEvent *)((unsigned char *)events + 128 * i);
            if (e->type == SDL_EV_KEY_DOWN || e->type == SDL_EV_KEY_UP)
                note_key_event((const Sdl3KeyEvent *)e, "peep", action);
            if (e->type >= SDL_EV_MOUSE_MOTION && e->type <= SDL_EV_MOUSE_UP) {
                note_mouse_event(e);
                if (consuming)
                    e->x = fold_x(e->x);
                // While head-look is steering, X4 is in mouselook and reads
                // every motion event as LOOK. So the player's own mouse fought
                // the head: Patola saw the view jitter toward the mouse and get
                // dragged back by the servo. The pointer must drive the cursor
                // and nothing else.
                //
                // Only the RELATIVE fields are dropped. x/y still carry the
                // pointer position the cursor shim composites, so targeting and
                // menus are untouched -- and our own synthetic motion is spared
                // by its marker, which is what that marker was for.
                if (consuming && e->type == SDL_EV_MOUSE_MOTION &&
                    e->reserved != kOurEventMagic &&
                    g_headlook_steering.load(std::memory_order_relaxed)) {
                    e->xrel = 0.0f;
                    e->yrel = 0.0f;
                }
            }
        }
    }
    return n;
}

// The other half of the input channel, and the more interesting one: X4 *polls*
// the pointer rather than only reading motion events. Whatever space this
// returns is the space X4 hit-tests in, which is exactly what the shim has to
// rewrite -- so this is where task #19 will act, not on the event stream.
// X4 recentres the pointer for mouse-look. Routed through the same fold, which
// works because the fold is an involution: X4 asks for a logical position, the
// real pointer goes to the place that folds back to it, and the value X4 reads
// next is the one it asked for. Using a separately-derived inverse here would
// be a second expression to keep in step with fold_x, and they would drift.
// X4 builds its cursors here. Captured rather than merely counted, because the
// shim has to draw *this* image -- a cursor of our own would not match, and X4
// varies it by context (Patola: a small blue hollow cross on the map, an arrow
// over a station).
//
// The surface is repacked tightly to w*4 so the consumer never has to carry
// SDL's pitch, and the pixel format is passed through unconverted: one run will
// say what it actually is, which beats a guessed conversion that silently
// mangles the colours.
void *SDL_CreateColorCursor(void *surface, int hot_x, int hot_y) {
    static auto real_fn =
        real<void *(*)(void *, int, int)>("SDL_CreateColorCursor");
    void *cur = real_fn(surface, hot_x, hot_y);
    if (!cur || !surface || !this_is_the_game())
        return cur;
    const auto *sf = (const Sdl3Surface *)surface;
    // Every one of these has to hold for the positional read to be believable.
    const bool sane = sf->w > 0 && sf->h > 0 &&
                      sf->w <= (int32_t)x4vr::Shared::kCursorMax &&
                      sf->h <= (int32_t)x4vr::Shared::kCursorMax &&
                      sf->pitch >= sf->w * 4 && sf->pitch < 1 << 20 && sf->pixels;
    if (!sane) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("sdl: cursor surface refused — w=%d h=%d pitch=%d pixels=%p "
                     "(max %u); either it is larger than the channel or the "
                     "SDL_Surface layout has moved",
                     sf->w, sf->h, sf->pitch, sf->pixels,
                     x4vr::Shared::kCursorMax);
        }
        return cur;
    }
    CapturedCursor c;
    c.handle = cur;
    c.w = (uint32_t)sf->w;
    c.h = (uint32_t)sf->h;
    c.format = sf->format;
    c.pitch = (uint32_t)sf->pitch;
    c.hot_x = hot_x;
    c.hot_y = hot_y;
    c.px.resize((size_t)c.w * c.h * 4u);
    for (uint32_t y = 0; y < c.h; y++)
        for (uint32_t b = 0; b < c.w * 4u; b++)
            c.px[(size_t)y * c.w * 4u + b] =
                ((const uint8_t *)sf->pixels)[(size_t)y * c.pitch + b];
    {
        std::lock_guard<std::mutex> lock(g_cursor_mu);
        g_cursors.push_back(c);
    }
    X4VR_LOG("sdl: captured cursor %p — %ux%u fmt=0x%08x pitch=%u hot=(%d,%d)",
             cur, c.w, c.h, c.format, c.pitch, hot_x, hot_y);
    return cur;
}

// Which of them is current. This is also the *shape* signal: X4 swapping cursor
// is how the reticle becomes an arrow, so a shim that draws a fixed image would
// be wrong exactly when the game is telling the player something.
bool SDL_SetCursor(void *cursor) {
    static auto real_fn = real<bool (*)(void *)>("SDL_SetCursor");
    if (this_is_the_game()) {
        std::lock_guard<std::mutex> lock(g_cursor_mu);
        for (const CapturedCursor &c : g_cursors)
            if (c.handle == cursor) {
                publish_cursor_image(c);
                break;
            }
    }
    return real_fn(cursor);
}

void SDL_WarpMouseInWindow(void *win, float x, float y) {
    static auto real_fn =
        real<void (*)(void *, float, float)>("SDL_WarpMouseInWindow");
    real_fn(win, this_is_the_game() ? fold_x(x) : x, y);
}

// Tracked so fold_x can stand down during mouse-look. Passed through untouched:
// the shim has no business changing whether X4 grabs the pointer, only where it
// believes the pointer is.
bool SDL_SetWindowRelativeMouseMode(void *win, bool enabled) {
    static auto real_fn =
        real<bool (*)(void *, bool)>("SDL_SetWindowRelativeMouseMode");
    if (this_is_the_game()) {
        g_relative_mouse.store(enabled, std::memory_order_relaxed);
        X4VR_LOG("sdl: relative mouse mode %s (fold %s)",
                 enabled ? "ON" : "off",
                 enabled ? "stands down" : "applies");
    }
    return real_fn(win, enabled);
}

uint32_t SDL_GetMouseState(float *x, float *y) {
    static auto real_fn = real<uint32_t (*)(float *, float *)>("SDL_GetMouseState");
    const uint32_t buttons = real_fn(x, y);
    if (this_is_the_game()) {
        note_extent("GetMouseState", x ? *x : 0.f, y ? *y : 0.f);
        if (x)
            *x = fold_x(*x);
        // After the fold, so what is published is what X4 is about to read.
        publish_cursor(x ? *x : 0.f, y ? *y : 0.f,
                       !g_relative_mouse.load(std::memory_order_relaxed));
        hide_sdl_cursor();
    }
    return buttons;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static auto real_connect =
        real<int (*)(int, const struct sockaddr *, socklen_t)>("connect");
    if (addr && addr->sa_family == AF_UNIX && len > sizeof(sa_family_t)) {
        const char *path = ((const struct sockaddr_un *)addr)->sun_path;
        // Abstract sockets start with NUL; print the name after it so the X11
        // ones (@/tmp/.X11-unix/X2) are not silently dropped as empty.
        const bool abstract = path[0] == '\0';
        const char *shown = abstract ? path + 1 : path;
        if (strstr(shown, "wayland") || strstr(shown, "gamescope") ||
            strstr(shown, "X11-unix"))
            X4VR_LOG("net: connect(%s%s) by pid %d", abstract ? "@" : "", shown,
                     getpid());
    }
    return real_connect(fd, addr, len);
}

// No null guards on the arguments: glibc declares all three nonnull, so a
// check here is dead code the compiler is entitled to delete, and -Wnonnull
// -compare says so. A caller passing null is already in undefined behaviour
// before it reaches us.
int setenv(const char *name, const char *value, int overwrite) {
    static auto real_setenv =
        real<int (*)(const char *, const char *, int)>("setenv");
    if (is_watched_env(name, strlen(name)))
        X4VR_LOG("env: setenv(%s=%s, overwrite=%d) by pid %d", name, value,
                 overwrite, getpid());
    return real_setenv(name, value, overwrite);
}

int unsetenv(const char *name) {
    static auto real_unsetenv = real<int (*)(const char *)>("unsetenv");
    if (is_watched_env(name, strlen(name)))
        X4VR_LOG("env: unsetenv(%s) by pid %d", name, getpid());
    return real_unsetenv(name);
}

// putenv takes "NAME=VALUE" in one string, so the name has to be split off
// before it can be matched.
int putenv(char *s) {
    static auto real_putenv = real<int (*)(char *)>("putenv");
    const char *eq = strchr(s, '=');
    if (eq && is_watched_env(s, (size_t)(eq - s)))
        X4VR_LOG("env: putenv(%s) by pid %d", s, getpid());
    return real_putenv(s);
}

int open(const char *path, int flags, ...) {
    static auto real_open = real<int (*)(const char *, int, mode_t)>("open");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("open(%s, 0x%x)", path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_open(to.c_str(), flags, mode);
    }
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    static auto real_open64 =
        real<int (*)(const char *, int, mode_t)>("open64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("open64(%s, 0x%x)", path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_open64(to.c_str(), flags, mode);
    }
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    static auto real_openat =
        real<int (*)(int, const char *, int, mode_t)>("openat");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("openat(%d, %s, 0x%x)", dirfd, path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_openat(dirfd, to.c_str(), flags, mode);
    }
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static auto real_openat64 =
        real<int (*)(int, const char *, int, mode_t)>("openat64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("openat64(%d, %s, 0x%x)", dirfd, path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_openat64(dirfd, to.c_str(), flags, mode);
    }
    return real_openat64(dirfd, path, flags, mode);
}

// Read-only opens of X4's own config.xml get the profile, patched in memory.
// Anything else — the game saving its settings — is redirected to the
// profile file, so it lands in ours instead of the player's. If either path
// fails we fall through to the real file untouched, which for a write means
// the pre-profile behaviour rather than a lost save.
static bool is_read_only(const char *mode) {
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

FILE *fopen(const char *path, const char *mode) {
    static auto real_fopen = real<FILE *(*)(const char *, const char *)>("fopen");
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("fopen(%s, %s)", path, mode);
    if (!overrides_disabled() && x4vr::is_x4_config(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_config(path))
                return f;
        } else {
            Profile prof = load_profile(path);
            if (!prof.xml.empty())
                return real_fopen(prof.path.c_str(), mode);
        }
    }
    if (headlook_enabled() && x4vr::is_x4_inputmap(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_inputmap(path))
                return f;
        } else {
            Profile prof = load_inputmap_profile(path);
            if (!prof.xml.empty())
                return real_fopen(prof.path.c_str(), mode);
        }
    }
    return real_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode) {
    static auto real_fopen64 =
        real<FILE *(*)(const char *, const char *)>("fopen64");
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("fopen64(%s, %s)", path, mode);
    if (!overrides_disabled() && x4vr::is_x4_config(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_config(path))
                return f;
        } else {
            Profile prof = load_profile(path);
            if (!prof.xml.empty())
                return real_fopen64(prof.path.c_str(), mode);
        }
    }
    if (headlook_enabled() && x4vr::is_x4_inputmap(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_inputmap(path))
                return f;
        } else {
            Profile prof = load_inputmap_profile(path);
            if (!prof.xml.empty())
                return real_fopen64(prof.path.c_str(), mode);
        }
    }
    return real_fopen64(path, mode);
}

} // extern "C"
