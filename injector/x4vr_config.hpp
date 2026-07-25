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
// True when the layer is making X4 render a single eye (half width). Both
// components read the same two variables so they cannot disagree about it.
inline bool sbs_split_render() {
    static const bool on = [] {
        const char *sbs = getenv("X4VR_SBS");
        if (!(sbs && *sbs && *sbs != '0'))
            return false;
        const char *split = getenv("X4VR_SBS_SPLIT");
        return !(split && *split && *split == '0');
    }();
    return on;
}

// Explicit render resolution, "WxH", overriding everything below. The
// launcher sets this when it sizes the window itself, which is the
// no-trickery path: if the window and the render are the same size there is
// no faked surface extent and nothing for Wayland's resize feedback to undo.
inline bool res_override(std::string &w, std::string &h) {
    static std::string ww, hh;
    static const bool ok = [] {
        const char *e = getenv("X4VR_RES");
        if (!e || !*e)
            return false;
        const char *x = strchr(e, 'x');
        if (!x || x == e || !x[1])
            return false;
        ww.assign(e, x - e);
        hh.assign(x + 1);
        return ww.find_first_not_of("0123456789") == std::string::npos &&
               hh.find_first_not_of("0123456789") == std::string::npos;
    }();
    if (ok) {
        w = ww;
        h = hh;
    }
    return ok;
}

inline const std::vector<TagOverride> &default_overrides() {
    // On Wayland the surface reports no preferred extent, so X4 falls back to
    // res_width/res_height -- which makes the config the lever for the split
    // render there. (On X11 currentExtent wins and the layer halves that
    // instead; asking for the eye size here is then simply consistent.)
    static std::string res_w, res_h;
    static const bool explicit_res = res_override(res_w, res_h);
    static const std::string eye_w = std::to_string(X4VR_SBS_WIDTH / 2);
    static const bool split = sbs_split_render();

    static const std::vector<TagOverride> v = {
        // These two are necessary but NOT sufficient: X4 only honours them
        // when borderless is off, and then loses the window decoration from
        // the height (observed: 1408 -> 1385). With borderless on it ignores
        // them and sizes to the display instead. Hence gamescope at exactly
        // this size -- see common/x4vr_sbs.hpp for the measurements.
        {"res_width", explicit_res ? res_w.c_str()
                                   : (split ? eye_w.c_str()
                                            : X4VR_SBS_WIDTH_STR)},
        {"res_height", explicit_res ? res_h.c_str() : X4VR_SBS_HEIGHT_STR},
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
        // NOTE: `pom` (parallax occlusion mapping) is deliberately NOT
        // overridden. Setting it to "none" appears to break X4's own shader
        // compilation:
        //
        //   Failed to compile shader 'shadergl\ogl\p2_complex_water'!
        //   ERROR: common_frag.glsl:1732: 'textureLod' : no matching
        //          overloaded function
        //
        // which is consistent with POM's height-map sampler being compiled
        // out while a use of it survives. The evidence is a timeline: the
        // last run that reached the cockpit served the *invalid* value "off"
        // (X4 rejects it silently -- the options menu shows "--"), so POM was
        // never really disabled. The first runs to serve the real value
        // "none" are the ones that fail. Every other override, ssr=false
        // included, was already in effect during the good run.
        //
        // Disabling it bought nothing yet anyway: POM displaces texture
        // lookups using the per-fragment view vector from the camera
        // constants, and those are not per-eye, so today both eyes compute
        // identical surface parallax either way. Revisit -- carefully, and
        // only with the compile error reproduced and understood -- when
        // per-eye lighting lands.
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
