// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_config.hpp — X4's config.xml, forked into a mod-owned profile.
//
// Non-intrusive by design, in two parts:
//
//   1. The mod runs off its own file, config-x4vrmod.xml, forked from the
//      player's config.xml the first time it is needed (new_config_fork). Every read
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
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/x4vr_immortal.hpp"
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
    static std::string &ww = x4vr::immortal<std::string>(),
                        &hh = x4vr::immortal<std::string>();
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

// X4VR_FOV: X4's own <fov> config tag, served from the profile.
//
// X4 exposes its field of view in config.xml, and the injector already owns
// every read of that file -- so widening the field is a config override, not a
// projection patch. That is task #24's non-intrusive path, and it is the only
// one that keeps X4's own matrices self-consistent: the shear reads
// M_projection[0][0] live (X4VR_PROJ_LIVE), so whatever X4 computes from this
// tag flows into the stereo for free. Patching the projection behind X4's back
// would instead desynchronise its culling, its HUD placement and its depth
// range from what is drawn.
//
// Unset by default, and deliberately so. Every take from 52 to 103 ran with the
// player's own value (1.1111, which is what ten takes measured sx=1.33333 at a
// 1:1 eye against), and those runs have to stay reproducible. Setting this is
// the experiment; omitting it is the control.
//
// The string is passed through verbatim rather than parsed and reprinted, so a
// value cannot pick up rounding on the way in. Only the *shape* is validated --
// a positive decimal number -- because serving a malformed tag would make X4
// fall back to its defaults silently, which is the failure that looks like a
// measurement.
inline bool fov_override(std::string &v) {
    static std::string &vv = x4vr::immortal<std::string>();
    static const bool ok = [] {
        const char *e = getenv("X4VR_FOV");
        if (!e || !*e)
            return false;
        // Plain decimal only -- no exponent, no sign, no trailing units.
        // strtod() would happily accept "1e0", and X4's XML reader may not:
        // a value X4 rejects silently leaves the options menu showing "--"
        // and the engine on its default, which is the POM trap recorded in
        // default_overrides() and reads exactly like a measurement.
        const std::string s(e);
        if (s.find_first_not_of("0123456789.") != std::string::npos ||
            std::count(s.begin(), s.end(), '.') > 1)
            return false;
        if (!(strtod(e, nullptr) > 0.0))
            return false;
        vv = s;
        return true;
    }();
    if (ok)
        v = vv;
    return ok;
}

// X4VR_SEPARATE_RADAR: X4's own <separateRadar> config tag.
//
// Read straight out of X4's UI Lua, which routes the radar with two predicates
// that are exact complements on this one setting:
//
//   isSeparateRadarEnabled()     = allowRadar and not tickerOnly and radarEnabled and     enableSeparateRadar
//   isTargetMonitorRadarEnabled() = allowRadar and not tickerOnly and radarEnabled and not enableSeparateRadar
//   private.enableSeparateRadar = C.GetConfigSetting("separateRadar") ~= 0
//
// and X4's own comment above the second one says the integrated target monitor
// radar is enabled when "we are not using the separate radar". checkRadarActivation()
// then deactivates one and activates the other, and activateMonitor() keeps the
// target monitor alive even in its inactive state when the radar lives there.
//
// Serving 0 should therefore move the radar off the flat screen-space element
// and onto the cockpit's target monitor -- world-space geometry, at the
// cockpit's depth. That is worth a great deal here: it gets correct stereo for
// free, it would follow #33's vertex-stage rotation for free, it needs none of
// #30's canvas, and its contrast is designed against the console instead of
// against whatever the widened field happens to swing in behind it (take 161's
// washed-out radar, which no amount of render resolution fixed).
//
// Unset by default and deliberately so, exactly as X4VR_FOV: setting this is
// the experiment, omitting it is the control. Only "0" and "1" are accepted --
// a malformed value would make X4 fall back silently, which is the trap
// recorded above and reads exactly like a measurement.
inline bool separate_radar_override(std::string &v) {
    static std::string &vv = x4vr::immortal<std::string>();
    static const bool ok = [] {
        const char *e = getenv("X4VR_SEPARATE_RADAR");
        if (!e || !*e)
            return false;
        const std::string s(e);
        if (s != "0" && s != "1")
            return false;
        vv = s;
        return true;
    }();
    if (ok)
        v = vv;
    return ok;
}

// X4VR_AA: X4's own <antialiasing> config tag. The accepted strings, read out
// of X4 9.00's Lua (gameoptions.lua, valueGfxAA):
//
//   none  fxaa_low  fxaa_medium  fxaa_high  msaa_2x  msaa_4x  msaa_8x
//   msaa_16x  ssaa_2x  ssaa_4x  temporal
//
// **The upscaler is NOT here any more.** Before 9.00 this tag also selected
// FSR (take 163 read `fsr`, `fsr_quality`, ... in the same list). X4 9.00
// moved the upscaler to its own tag, <upmode> (GetUpscalingOption reads
// "upmode": none, fsr3_native, fsr3_quality, fsr3_balanced,
// fsr3_performance, fsr3_ultra_performance, fsr_ultra_quality, fsr_quality,
// fsr_balanced, fsr_performance). Patola's 4224-per-eye runs with
// X4VR_AA=fsr_quality (2026-10-31) served a value 9.00 does not know, so they
// rendered natively and the options showed "FSR off". This list rejects the
// old values instead of serving them.
//
// We serve "none" by default and have since the beginning; X4VR_AA is unset
// by default like X4VR_FOV: setting it is the experiment, omitting it is the
// control.
//
// Frame generation is deliberately NOT reachable from here. X4 keeps it in its
// own tags (dlssg, fsr3g), and an interpolated frame carries no valid head
// pose while adding latency, so it is harmful in VR regardless of whether
// upscaling works. If it is ever wired it needs its own knob and its own
// argument, not a value smuggled through this one.
inline bool aa_override(std::string &v) {
    static std::string &vv = x4vr::immortal<std::string>();
    static const bool ok = [] {
        const char *e = getenv("X4VR_AA");
        if (!e || !*e)
            return false;
        static const char *const modes[] = {
            "none", "fxaa_low", "fxaa_medium", "fxaa_high", "msaa_2x",
            "msaa_4x", "msaa_8x", "msaa_16x", "ssaa_2x", "ssaa_4x", "temporal",
        };
        const std::string s(e);
        for (const char *m : modes)
            if (s == m) {
                vv = s;
                return true;
            }
        // Rejected rather than served: X4 falls back silently on a value it does
        // not know, which reads exactly like a measurement.
        return false;
    }();
    if (ok)
        v = vv;
    return ok;
}

inline const std::vector<TagOverride> &default_overrides() {
    // On Wayland the surface reports no preferred extent, so X4 falls back to
    // res_width/res_height -- which makes the config the lever for the split
    // render there. (On X11 currentExtent wins and the layer halves that
    // instead; asking for the eye size here is then simply consistent.)
    static std::string &res_w = x4vr::immortal<std::string>(),
                        &res_h = x4vr::immortal<std::string>();
    static const bool explicit_res = res_override(res_w, res_h);
    static const std::string &eye_w =
        x4vr::immortal<std::string>(std::to_string(X4VR_SBS_WIDTH / 2));
    static const bool split = sbs_split_render();
    static std::string &fov_v = x4vr::immortal<std::string>();
    static const bool have_fov = fov_override(fov_v);
    // Overrides the base entry rather than being appended after it: two entries
    // for one tag would leave which one wins to the order set_tag() is called
    // in, and "none" is load-bearing enough to be stated in exactly one place.
    static std::string &aa_v = x4vr::immortal<std::string>();
    static const bool have_aa = aa_override(aa_v);

    static const std::vector<TagOverride> &base =
        x4vr::immortal<std::vector<TagOverride>>(std::initializer_list<TagOverride>{
        // These two are necessary but NOT sufficient: X4 only honours them
        // when borderless is off, and then loses the window decoration from
        // the height (observed: 1408 -> 1385). With borderless on it ignores
        // them and sizes to the display instead. Hence gamescope at exactly
        // this size -- see common/x4vr_sbs.hpp for the measurements.
        {"res_width", explicit_res ? res_w.c_str()
                                   : (split ? eye_w.c_str()
                                            : X4VR_SBS_WIDTH_STR)},
        {"res_height", explicit_res ? res_h.c_str() : X4VR_SBS_HEIGHT_STR},
        // Borderless makes X4 ignore res_width/res_height and size to the
        // display -- which is stated three lines above and was then set
        // unconditionally anyway.
        //
        // That is right for one-eye mode, where gamescope IS the eye size, so
        // "size to the display" and "size to one eye" are the same request.
        // It is wrong for the split render, where gamescope is deliberately
        // the *full* SBS width: X4 sized to 2816 instead of 1408, the layer's
        // split test (which asks whether X4 requested exactly one eye) failed,
        // and the composite fell back to duplicating the left half. Takes
        // thirty-one and thirty-two both died here.
        //
        // So the split render needs the config lever honoured, which means
        // borderless off. On X11 the halved surface capabilities would have
        // brought X4 to the eye size on their own; this is what makes the
        // Wayland path work, and it is the path this machine is on.
        {"borderless", split ? "false" : "true"},
        // Leaving fullscreen on would let X4 pick the output's mode, which
        // is a mode-set we neither need nor control.
        {"fullscreen", "false"},
        // Screen-space reflections reproject the frame's own depth buffer,
        // which is computed for one view. Until the camera block is patched
        // per eye, both eyes would reuse the same mono reflection -- a
        // stereo mismatch exactly on shiny surfaces. Revisit after per-eye
        // lighting lands.
        {"ssr", "false"},
        // "none" unless X4VR_AA says otherwise. Not an arbitrary default: this
        // tag also selects X4's upscaler, and every temporal mode here (temporal,
        // dlaa, the fsr_* family) jitters the projection -- into m[8]/m[9],
        // which are the eye shear's own slots. See the X4VR_AA comment.
        {"antialiasing", have_aa ? aa_v.c_str() : "none"},
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
    });

    // Appended rather than listed above, so that a run without X4VR_FOV serves
    // byte-for-byte the config every take before 104 served.
    static std::string &radar_v = x4vr::immortal<std::string>();
    static const bool have_radar = separate_radar_override(radar_v);

    static const std::vector<TagOverride> &v = x4vr::immortal<std::vector<TagOverride>>([] {
        std::vector<TagOverride> t = base;
        if (have_fov)
            t.push_back({"fov", fov_v.c_str()});
        if (have_radar)
            t.push_back({"separateRadar", radar_v.c_str()});
        return t;
    }());
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

// set_tag, and when the tag is ABSENT, insert it before </root>. For the one
// tag that needs it: X4 leaves options at their defaults out of config.xml
// (Patola's has no <mousesteeringline> at all), so a value we must serve for
// such an option has nothing to rewrite. `*old_value` is "" when it was
// absent. Every other override keeps set_tag's never-invent rule.
inline bool set_or_insert_tag(std::string &xml, const char *tag,
                              const char *value,
                              std::string *old_value = nullptr) {
    const std::string open = std::string("<") + tag + ">";
    if (xml.find(open) != std::string::npos)
        return set_tag(xml, tag, value, old_value);
    const size_t root = xml.rfind("</root>");
    if (root == std::string::npos)
        return false;
    if (old_value)
        old_value->clear();
    xml.insert(root, std::string("  ") + open + value + "</" + tag + ">\n");
    return true;
}

// X4VR_UPSCALE: X4's upscaler, <upmode> since X4 9.00 (GetUpscalingOption,
// 0xb7f1f0, reads "upmode"). The accepted values are exactly the ten X4's
// binary and its graphics options know:
//
//   none
//   fsr3_native  fsr3_quality  fsr3_balanced  fsr3_performance
//   fsr3_ultra_performance                       FSR3, temporal
//   fsr_ultra_quality  fsr_quality  fsr_balanced  fsr_performance   FSR 1
//
// **The mod owns this setting** (Patola, 2026-10-31): it is tied to the
// resolution the launcher sets, so it is served on every read, "none" when
// X4VR_UPSCALE is unset, and an in-game change lasts only until the next
// launch. A value outside the list is REJECTED and "none" served, rather than
// a value X4 would ignore silently (the X4VR_AA lesson). The output size is
// the eye (X4VR_W/2 x X4VR_H); X4 renders the scene smaller and upscales,
// quality = output / 1.5 per axis.
//
// Frame generation is served off with it (<fsr3g>, <dlssg>): an interpolated
// frame carries no head pose and adds latency, harmful in VR.
//
// **FSR 1 works in VR since take 380:** X4's FSR pass runs once per eye
// (X4VR_FSR_STEREO, layer/x4vr_layer.cpp). **FSR3 is not handled per eye
// yet:** it is AMD's FidelityFX SDK linked into X4, with its own descriptor
// sets and history images, and take 381 measures what it touches.
inline bool is_upscale_mode(const char *s) {
    static const char *const modes[] = {
        "none",           "fsr3_native",       "fsr3_quality", "fsr3_balanced",
        "fsr3_performance", "fsr3_ultra_performance", "fsr_ultra_quality",
        "fsr_quality",    "fsr_balanced",      "fsr_performance",
    };
    if (!s)
        return false;
    for (const char *m : modes)
        if (!strcmp(s, m))
            return true;
    return false;
}
// The <upmode> value to serve, and how it was chosen: 0 our default (unset),
// 1 from X4VR_UPSCALE, -1 X4VR_UPSCALE rejected (served "none").
inline const char *upscale_value(int *source = nullptr) {
    static int src = 0;
    static const char *v = [] {
        const char *e = getenv("X4VR_UPSCALE");
        if (!e || !*e)
            return "none";
        if (is_upscale_mode(e)) {
            src = 1;
            return e;
        }
        src = -1;
        return "none";
    }();
    if (source)
        *source = src;
    return v;
}
// Serve the upscaler and frame generation off. True when anything changed;
// `old_up` gets <upmode>'s previous value ("" when absent).
inline bool serve_upscaler(std::string &xml, const char *upmode,
                           std::string *old_up = nullptr) {
    bool changed = set_or_insert_tag(xml, "upmode", upmode, old_up);
    changed |= set_or_insert_tag(xml, "fsr3g", "off");
    changed |= set_or_insert_tag(xml, "dlssg", "off");
    return changed;
}

// <hudscale>: "off" on every read of the mod's config (2026-10-31). X4 places
// its lower HUD (left panel, radar, right panel) in ui/core/lua/monitors.lua's
// initScale. Below a 4:3 aspect -- our square eye always is -- it shifts each
// panel by 112.5 / (1 - factor)^2, factor = (1 - s) * height / 1959, where s
// is the HUD's UI scale: the player's <uiscale> (Patola: 0.6) unless HUD
// scale is "off", when s is 1 (GetHUDUIScale, 0xa41a70 -> 0xa98e60). At s
// 0.6 the formula has a pole at 4898 px tall: at 4224 per eye it threw the
// left panel across to the right (Patola). With "off", factor is 0 and the
// placement does not depend on the height; Patola set it in game and the HUD
// came back. X4 leaves the option out of config.xml at its default, so the
// tag is inserted when absent. The fork is read by mod sessions only, so
// nothing needs restoring for vanilla. True when the text changed.
inline bool serve_hud_scale_off(std::string &xml, std::string *old_value = nullptr) {
    return set_or_insert_tag(xml, "hudscale", "off", old_value);
}

// The sidecar recording what an override replaced, beside the profile:
// .../config-x4vrmod-<tag>.was. X4 saves its settings while it runs, so a
// value served in memory ends up in the profile; the sidecar is how a later
// run that does NOT ask for the override puts the player's own value back.
inline std::string sidecar_path(const std::string &profile, const char *tag) {
    std::string p = profile;
    const size_t dot = p.rfind(".xml");
    if (dot != std::string::npos)
        p.erase(dot);
    return p + "-" + tag + ".was";
}

// Read the text of <tag>...</tag>, or "" if it is not there.
//
// Exists so the log can state the *effective* value of a projection-defining
// tag on every run, not only on the runs that override it. X4 writes its own
// settings into the profile while it plays, so a value set by one run can be
// served to the next one that does not set it -- silently, and looking exactly
// like a default. That is the same trap X4VR_PROJ_INVPROJ sprang on every take
// before 83, where omitting a variable and setting it to 0 quietly stopped
// being the same request. A run whose log does not state its own field of view
// cannot be reproduced from its log.
inline std::string get_tag(const std::string &xml, const char *tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos)
        return "";
    const size_t vstart = a + open.size();
    const size_t vend = xml.find(close, vstart);
    if (vend == std::string::npos)
        return "";
    return xml.substr(vstart, vend - vstart);
}

// Read a file fully; empty string on failure.
//
// IMPORTANT: this runs *inside* our interposed fopen(), so it must not call
// fopen() itself (that would re-enter the interposer and recurse forever).
// We use open()/read() via syscall-level libc entry points obtained from
// RTLD_NEXT, which our interposers only observe, never redirect.
// A file of the player's profile that simply isn't there yet: a copy of X4 that has
// never run. Unreadable is different, and is left alone. Only under EgoSoft/X4, because
// is_x4_config matches any file called config.xml.
inline bool new_profile_file(const char *path) {
    return path && strstr(path, "/EgoSoft/X4/") && access(path, F_OK) != 0 && errno == ENOENT;
}

// What the fork starts as: the player's config.xml, or nothing but the root on a new
// profile, because the overrides below still have to reach X4 on its first run. The
// font scale is the one thing we choose rather than copy: X4's own 1.0 is too big in VR
// and 0.6 is right (Patola, 2026-11-05). Only here, at creation, so a later choice in
// X4's options stays.
inline constexpr const char *kVrUiScale = "0.6";
inline std::string new_config_fork(const std::string &player) {
    std::string xml =
        player.empty() ? std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root>\n</root>\n")
                       : player;
    set_or_insert_tag(xml, "uiscale", kVrUiScale);
    return xml;
}

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
