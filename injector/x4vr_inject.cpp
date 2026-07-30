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

#include <cstdio>
#include <cstdlib>
#include <string>

extern char **environ;

#define X4VR_LOG_TAG "inject"
#include "../common/x4vr_log.hpp"
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
