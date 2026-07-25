// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_config.hpp — X4's config.xml, forked into a mod-owned profile.
//
// Non-intrusive by design, in two parts:
//
//   1. The mod runs off its own file, config-x4vrmod.xml, forked verbatim
//      from the player's config.xml the first time it is needed. Every read
//      X4 makes of config.xml is answered from the profile, and every write
//      is redirected into it. The player's file is opened read-only, once,
//      and never again — which matters because X4 saves its settings while
//      running, and would otherwise persist *our* values into it (observed
//      live: a config.xml left at 2816x1408 / antialiasing=none afterwards).
//
//   2. The overrides below are applied to the profile **in memory** on each
//      read, so they hold even if the player changes those settings in the
//      options menu mid-session. Everything else the player changes is
//      written to the profile by X4 and simply persists.
//
// If anything in this path fails we fall back to serving the untouched file.
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

#include "../common/x4vr_sbs.hpp"

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
        // These two are necessary but NOT sufficient: X4 only honours them
        // when borderless is off, and then loses the window decoration from
        // the height (observed: 1408 -> 1385). With borderless on it ignores
        // them and sizes to the display instead. Hence gamescope at exactly
        // this size -- see common/x4vr_sbs.hpp for the measurements.
        {"res_width", X4VR_SBS_WIDTH_STR},
        {"res_height", X4VR_SBS_HEIGHT_STR},
        // No decoration to lose, and under a correctly sized gamescope
        // "the display" is already what we asked for.
        {"borderless", "true"},
        // Leaving fullscreen on would let X4 pick the output's mode, which
        // is a mode-set we neither need nor control.
        {"fullscreen", "false"},
        // Screen-space reflections reproject the frame's own depth buffer,
        // which is computed for one view. Until the camera block is patched
        // per eye, both eyes would reuse the same mono reflection -- a
        // stereo mismatch exactly on shiny surfaces. Revisit after per-eye
        // lighting lands.
        {"ssr", "false"},
        {"antialiasing", "none"},
        {"ssao", "0"},
        {"glow", "0"},
        {"uiglow", "0"},
        {"distortion", "false"},
        // Parallax occlusion mapping. The stored value for "Off" is "none",
        // NOT "off" -- confirmed by setting it in-game and reading back the
        // file. "off" round-trips through the config but the options menu
        // then shows "--", matching no known option (see
        // injector/README.md).
        //
        // This one is *temporary*: POM displaces texture lookups using the
        // per-fragment view vector, which comes from the camera constants.
        // Those are not per-eye yet, so both eyes currently compute identical
        // surface parallax. Once the camera block is patched per eye, POM
        // becomes correct and should go back on -- unlike ssr, which is
        // awkward in stereo regardless.
        {"pom", "none"},
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

// Write a file fully; true on success. Same RTLD_NEXT discipline as
// read_file() so it can run from inside our own interposers.
inline bool write_file(const char *path, const std::string &data) {
    using open_fn = int (*)(const char *, int, ...);
    static open_fn real_open = [] {
        void *p = dlsym(RTLD_NEXT, "open");
        open_fn fn;
        memcpy(&fn, &p, sizeof(fn));
        return fn;
    }();
    if (!real_open)
        return false;
    int fd = real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w <= 0) {
            ::close(fd);
            return false;
        }
        off += (size_t)w;
    }
    ::close(fd);
    return true;
}

// True if `path`'s basename is exactly "config.xml" (not ventureconfig.xml).
inline bool is_x4_config(const char *path) {
    if (!path)
        return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return strcmp(base, "config.xml") == 0;
}

// The mod's own settings file, alongside X4's: .../config-x4vrmod.xml.
// Deliberately does not contain "config.xml" as a substring, so none of the
// injector's own path tests can ever match it.
inline std::string profile_path(const char *config_path) {
    std::string p = config_path ? config_path : "";
    const size_t slash = p.rfind('/');
    p.replace(slash == std::string::npos ? 0 : slash + 1, std::string::npos,
              "config-x4vrmod.xml");
    return p;
}

} // namespace x4vr
