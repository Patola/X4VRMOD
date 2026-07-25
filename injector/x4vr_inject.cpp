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
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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
