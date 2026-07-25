// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// libx4vr_inject.so — LD_PRELOAD injector.
//
// Phase 1: rewrite X4's config.xml IN MEMORY as the game reads it, so the
// player's settings file on disk is never modified (non-intrusive by
// design). We force the 2:1 SBS resolution and disable the effects that
// interfere with stereo rendering.
//
// X4 reads the file with fopen() (confirmed live), so that is the primary
// hook; the open()/openat() family is still interposed for observation and
// as a safety net if a future patch changes the access path.
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
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

// The player's pre-run value for every tag we override, captured the first
// time X4 reads config.xml — before we patch anything. X4 rewrites the file
// on exit with the values it is running (i.e. ours), so this is what we put
// back afterwards. See x4vr_config.hpp.
std::mutex g_orig_mu;
std::string g_config_path;
std::vector<std::pair<std::string, std::string>> g_orig_values;

// Put the player's values back for our tags only, keeping everything else
// X4 saved. Idempotent, so it is safe to call from both fclose() and exit.
void restore_config() {
    std::lock_guard<std::mutex> lock(g_orig_mu);
    if (g_config_path.empty() || g_orig_values.empty())
        return;
    std::string xml = x4vr::read_file(g_config_path.c_str());
    if (xml.empty())
        return;
    // Restore a tag only if X4 wrote back the value we injected. If it wrote
    // anything else, the player changed that setting in-game this session and
    // meant it -- reverting would silently undo their choice. (Live runs show
    // X4 rewrites config.xml repeatedly during a session, not just at exit,
    // so this distinction matters in practice.)
    int restored = 0, kept = 0;
    for (const auto &kv : g_orig_values) {
        const char *injected = nullptr;
        for (const auto &ov : x4vr::default_overrides())
            if (kv.first == ov.tag)
                injected = ov.value;
        std::string current;
        if (injected && x4vr::get_tag(xml, kv.first.c_str(), current) &&
            current != injected) {
            kept++;
            continue; // the player's own change; leave it alone
        }
        if (x4vr::set_tag(xml, kv.first.c_str(), kv.second.c_str()))
            restored++;
    }
    if (kept)
        X4VR_LOG("config: keeping %d setting(s) the player changed in-game",
                 kept);
    if (!restored)
        return; // already the player's values — nothing to do
    if (x4vr::write_file(g_config_path.c_str(), xml))
        X4VR_LOG("config: restored %d player setting(s) in %s after X4 saved",
                 restored, g_config_path.c_str());
    else
        X4VR_LOG("config: WARNING could not restore %s", g_config_path.c_str());
}

// Streams X4 opened on config.xml for writing, so fclose() knows when the
// saved file has landed and can be fixed up.
std::mutex g_wr_mu;
std::set<FILE *> g_config_writes;

void note_config_write(FILE *f) {
    std::lock_guard<std::mutex> lock(g_wr_mu);
    g_config_writes.insert(f);
}

bool take_config_write(FILE *f) {
    std::lock_guard<std::mutex> lock(g_wr_mu);
    return g_config_writes.erase(f) > 0;
}

// exit() runs atexit handlers *before* it flushes and closes stdio streams.
// So if X4 ever leaves its config stream unflushed, restoring here without
// forcing the flush first would be undone a moment later by that flush
// writing our injected values back out. Flush everything, then restore.
void restore_config_atexit() {
    fflush(nullptr);
    restore_config();
}

// Build the rewritten config and return a stream over it, or nullptr to let
// the caller fall back to the real file.
FILE *open_patched_config(const char *path) {
    std::string xml = x4vr::read_file(path);
    if (xml.empty()) {
        X4VR_LOG("config: could not read %s — passing through", path);
        return nullptr;
    }

    int changed = 0;
    bool capture = false;
    {
        std::lock_guard<std::mutex> lock(g_orig_mu);
        capture = g_orig_values.empty();
        if (capture) {
            g_config_path = path;
            static std::once_flag once;
            std::call_once(once, [] { atexit(restore_config_atexit); });
        }
    }
    for (const auto &ov : x4vr::default_overrides()) {
        std::string old;
        const bool did = x4vr::set_tag(xml, ov.tag, ov.value, &old);
        if (capture && !old.empty()) {
            std::lock_guard<std::mutex> lock(g_orig_mu);
            g_orig_values.emplace_back(ov.tag, old);
        }
        if (did) {
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
    X4VR_LOG("config: serving patched %s in memory (%zu bytes, %d changes; "
             "we never write it, and X4's own save is undone on exit)",
             path, xml.size(), changed);
    return f;
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
    return real_openat64(dirfd, path, flags, mode);
}

// Read-only opens of X4's own config.xml get the patched in-memory copy.
// Writes (the game saving settings) pass straight through to the real file,
// so a failure here can never lose the player's settings — we only fix the
// file up afterwards, in x4vr_note_config_write()'s fclose().
static bool wants_patch(const char *path, const char *mode) {
    return !overrides_disabled() && x4vr::is_x4_config(path) && mode &&
           mode[0] == 'r' && !strchr(mode, '+');
}

static bool is_write_mode(const char *mode) {
    return mode && (mode[0] == 'w' || mode[0] == 'a' || strchr(mode, '+'));
}

FILE *fopen(const char *path, const char *mode) {
    static auto real_fopen = real<FILE *(*)(const char *, const char *)>("fopen");
    if (is_config_xml(path))
        X4VR_LOG("fopen(%s, %s)", path, mode);
    if (wants_patch(path, mode)) {
        if (FILE *f = open_patched_config(path))
            return f;
    }
    FILE *f = real_fopen(path, mode);
    if (f && x4vr::is_x4_config(path) && is_write_mode(mode))
        note_config_write(f);
    return f;
}

FILE *fopen64(const char *path, const char *mode) {
    static auto real_fopen64 =
        real<FILE *(*)(const char *, const char *)>("fopen64");
    if (is_config_xml(path))
        X4VR_LOG("fopen64(%s, %s)", path, mode);
    if (wants_patch(path, mode)) {
        if (FILE *f = open_patched_config(path))
            return f;
    }
    FILE *f = real_fopen64(path, mode);
    if (f && x4vr::is_x4_config(path) && is_write_mode(mode))
        note_config_write(f);
    return f;
}

// X4 saves settings on exit; as soon as it closes that stream the file on
// disk holds our injected values, so put the player's back immediately. The
// atexit() hook registered in open_patched_config() is the safety net for
// the case where the stream is never explicitly closed.
int fclose(FILE *f) {
    static auto real_fclose = real<int (*)(FILE *)>("fclose");
    const bool was_config = take_config_write(f);
    int r = real_fclose(f);
    if (was_config)
        restore_config();
    return r;
}

} // extern "C"
