// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_config.hpp — in-memory rewriting of X4's config.xml.
//
// Non-intrusive by design: the player's file on disk is never modified. We
// read it, rewrite the tags we need in a memory buffer, and hand X4 a
// stream over that buffer (fmemopen). If anything fails we fall back to the
// untouched file.
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x4vr {

struct TagOverride {
    const char *tag;
    const char *value;
};

// The VR baseline (docs/DESIGN.md): 2:1 SBS resolution and the effects that
// interfere with stereo/reprojection turned off. Values match X4's own
// encoding for each tag (bool-ish tags use false, level tags use 0,
// antialiasing uses the string "none").
inline const std::vector<TagOverride> &default_overrides() {
    static const std::vector<TagOverride> v = {
        {"res_width", "2816"},
        {"res_height", "1408"},
        // Decorated windows lose the titlebar height from the requested
        // size (observed: 1408 -> 1385 both on the desktop and inside
        // gamescope), which would break the exact 2:1 SBS split.
        {"borderless", "true"},
        {"antialiasing", "none"},
        {"ssao", "0"},
        {"glow", "0"},
        {"uiglow", "0"},
        {"distortion", "false"},
        {"chromaticaberration", "false"},
        {"colorcorrection", "0"},
    };
    return v;
}

// Replace the text of <tag>...</tag> in `xml`. Returns true if a change was
// made. Only rewrites existing tags (X4 writes a full config, so every tag
// we care about is present; we deliberately do not invent new ones).
inline bool set_tag(std::string &xml, const char *tag, const char *value,
                    std::string *old_value = nullptr) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos)
        return false;
    size_t vstart = a + open.size();
    size_t vend = xml.find(close, vstart);
    if (vend == std::string::npos)
        return false;
    if (old_value)
        *old_value = xml.substr(vstart, vend - vstart);
    if (xml.compare(vstart, vend - vstart, value) == 0)
        return false; // already the desired value
    xml.replace(vstart, vend - vstart, value);
    return true;
}

// Read a file fully; empty string on failure.
//
// IMPORTANT: this runs *inside* our interposed fopen(), so it must not call
// fopen() itself (that would re-enter the interposer and recurse forever).
// We use open()/read() via syscall-level libc entry points obtained from
// RTLD_NEXT, which our interposers only observe, never redirect.
inline std::string read_file(const char *path) {
    std::string out;
    using open_fn = int (*)(const char *, int, ...);
    static open_fn real_open = [] {
        void *p = dlsym(RTLD_NEXT, "open");
        open_fn fn;
        memcpy(&fn, &p, sizeof(fn));
        return fn;
    }();
    int fd = real_open ? real_open(path, O_RDONLY) : -1;
    if (fd < 0)
        return out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, (size_t)n);
    ::close(fd);
    return out;
}

// True if `path`'s basename is exactly "config.xml" (not ventureconfig.xml).
inline bool is_x4_config(const char *path) {
    if (!path)
        return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return strcmp(base, "config.xml") == 0;
}

} // namespace x4vr
