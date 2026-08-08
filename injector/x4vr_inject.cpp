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
#include "../common/x4vr_share.hpp"
#include "x4vr_config.hpp"

namespace {

bool is_config_xml(const char *path) {
    return path && strstr(path, "config.xml");
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
    X4VR_LOG("config: serving %s in memory (%zu bytes, %d overrides applied); "
             "your %s is not touched",
             prof.path.c_str(), xml.size(), changed, path);
    return f;
}

// If this is a write-mode open of X4's config.xml, the path to use instead
// (empty = leave the call alone). X4 saving its settings must never reach the
// player's file. Absolute paths only: an *at() call with a relative path
// resolves against dirfd, which our profile lookup cannot see.
std::string redirect_write(const char *path) {
    if (overrides_disabled() || !path || path[0] != '/' ||
        !x4vr::is_x4_config(path))
        return {};
    Profile prof = load_profile(path);
    if (prof.xml.empty())
        return {};
    return prof.path;
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
void hide_sdl_cursor() {
    static const bool want = [] {
        const char *e = getenv("X4VR_HIDE_CURSOR");
        return e && *e && *e != '0';
    }();
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

void note_mouse_event(const Sdl3MouseEvent *e) {
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
int SDL_GetWindowSize(void *win, int *w, int *h) {
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
bool SDL_WaitEvent(void *event) {
    static auto real_fn = real<bool (*)(void *)>("SDL_WaitEvent");
    const bool r = real_fn(event);
    if (r && event && this_is_the_game()) {
        auto *e = (Sdl3MouseEvent *)event;
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
    const bool consuming = action == 2;
    if (n > 0 && events && this_is_the_game()) {
        for (int i = 0; i < n; i++) {
            auto *e = (Sdl3MouseEvent *)((unsigned char *)events + 128 * i);
            if (e->type >= SDL_EV_MOUSE_MOTION && e->type <= SDL_EV_MOUSE_UP) {
                note_mouse_event(e);
                if (consuming)
                    e->x = fold_x(e->x);
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
    if (is_config_xml(path))
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
    if (is_config_xml(path))
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
    if (is_config_xml(path))
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
    if (is_config_xml(path))
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
    if (is_config_xml(path))
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
    return real_fopen(path, mode);
}

FILE *fopen64(const char *path, const char *mode) {
    static auto real_fopen64 =
        real<FILE *(*)(const char *, const char *)>("fopen64");
    if (is_config_xml(path))
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
    return real_fopen64(path, mode);
}

} // extern "C"
