// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Whether an eye size fits the graphics card's memory. docs/x4-quirks.md, "X4's memory:
// the budget it keeps to and its blocks", has X4's side of it.
//
// X4 asks the driver how much it may use (VK_EXT_memory_budget) and takes no new block past
// that, so on a small card it doesn't fail: textures go missing. What its images take is
// decided by the eye size alone and is in place before the first texture (2026-11-24, takes
// 457a-g, 457z, 458): what's left is what textures get, and the runs tell the rest.
#include "x4vr_launcher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "x4vr_i18n.hpp"

namespace x4vr::launcher {

namespace {
double mpx(int side) { return (double)side * (double)side / 1e6; }
} // namespace

// Measured on an RTX 3070 laptop with no upscaler, 2016 to 4000 per eye, the use steady
// before the start menu: 1159, 2040, 2550, 3235, 3884 MB through WiVRn and 2232, 2796, 3547,
// 4268 through SteamVR. That is 233 MB and 228 or 252 a megapixel, within 50 MB. An upscaler
// saves little: with FSR 1 Quality, 2907 at 3600 through WiVRn (take 460), so 39 of those a
// rendered megapixel and the rest an eye's. Only that mode is measured. A Radeon took 490 more
// with none (2531 at 2816, take 459) and 400 more with FSR 1 Quality (2737 at 3200).
int images_mb(int eye, int render, bool nvidia, bool steamvr) {
    const double per_eye = steamvr ? 213.0 : 189.0;
    return (int)std::lround(233.0 + per_eye * mpx(eye) + 39.0 * mpx(render) +
                            (nvidia ? 0.0 : 500.0));
}

// X4 places the map's image in a block of max(128 MiB, the card's memory / 96), and the mod
// doubles that image, 16 bytes a pixel. The layer never doubles one above 248 MiB. Cards align
// the image differently, by up to 3.1% (203.9 MiB at 3600 on the RTX 3070).
static int map_side(uint32_t heap_mb, double bytes) {
    const double block = std::max(128.0, (double)heap_mb / 96.0);
    const double mib = bytes > 8.0 ? std::min(block, 255.8) : block;
    return (int)std::sqrt(mib / 1.031 * 1048576.0 / bytes) / 50 * 50;
}
int map_depth_up_to(uint32_t heap_mb) { return heap_mb ? map_side(heap_mb, 16.0) : 0; }
int map_drawn_up_to(uint32_t heap_mb) { return heap_mb ? map_side(heap_mb, 8.0) : 0; }

// ------------------------------------------------------------- what a session had
static bool number_after(const std::string &s, const char *key, int *out) {
    const size_t at = s.find(key);
    if (at == std::string::npos)
        return false;
    *out = std::atoi(s.c_str() + at + strlen(key));
    return true;
}

bool session_memory(const std::string &text, SessionMemory *out) {
    SessionMemory m;
    m.upscaler = "none";
    bool eye = false;
    double first = -1.0, armed = -1.0;
    struct Sample {
        double t;
        int free;
    };
    std::vector<Sample> samples;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        // Each line is "[ seconds] module  message".
        const double t = text[at] == '[' ? std::atof(text.c_str() + at + 1) : -1.0;
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!eye && line.find("env: run =") != std::string::npos) {
            eye = number_after(line, " X4VR_H=", &m.eye);
            const size_t u = line.find(" X4VR_UPSCALE=");
            if (u != std::string::npos)
                m.upscaler = line.substr(u + 14, line.find(' ', u + 14) - (u + 14));
        } else if (line.find("layer   vram (periodic): the card's ") != std::string::npos) {
            int card = 0, use = 0, budget = 0;
            if (!number_after(line, "the card's ", &card) ||
                !number_after(line, "X4 uses ", &use) ||
                !number_after(line, "its budget ", &budget) || t < 0.0)
                continue;
            m.card_mb = (uint32_t)card;
            if (first < 0.0)
                first = t;
            samples.push_back({t, budget - use});
        } else if (armed < 0.0 && line.find("inject  headlook write: ARMED") != std::string::npos) {
            armed = t;
        } else if (line.find("vr summary (periodic): runtime=") != std::string::npos) {
            m.steamvr = m.steamvr || line.find("runtime=SteamVR") != std::string::npos;
        }
    }
    if (!eye || samples.empty())
        return false;
    // Before the save: in it, a session that has room also ends near its ceiling, because X4
    // keeps what it has loaded (9 to 12 GB of a Radeon's 24, 2026-11-23). The median, because
    // the last seconds before it are the save loading: 177 MB in 457g, whose textures were all
    // there, against 868 in its start menu.
    const double until = armed >= 0.0 ? armed : first + 90.0;
    std::vector<int> menu;
    for (const Sample &s : samples)
        if (s.t < until)
            menu.push_back(std::max(0, s.free));
    if (menu.empty())
        return false;
    std::nth_element(menu.begin(), menu.begin() + (long)(menu.size() / 2), menu.end());
    m.menu_free = menu[menu.size() / 2];
    *out = m;
    return true;
}

std::vector<SessionMemory> past_memory(const std::string &home, size_t max) {
    std::vector<SessionMemory> out;
    size_t looked = 0;
    for (const SessionLog &l : session_logs(home)) {
        if (l.flat)
            continue;
        if (out.size() >= max || ++looked > 2 * max)
            break;
        SessionMemory m;
        if (session_memory(log_excerpt(l.path, 16u << 20, 0), &m))
            out.push_back(m);
    }
    return out;
}

// ------------------------------------------------------------------ the notes
// A large image X4 can't place is dropped and made again each frame (takes 376, 457e).
static std::string map_note(int eye, uint32_t heap_mb) {
    if (!heap_mb)
        return "";
    const int depth = map_depth_up_to(heap_mb), drawn = map_drawn_up_to(heap_mb);
    if (eye > drawn)
        return fmt(tr_("On this graphics card, the map's 3D view stays empty above %1."),
                   {std::to_string(drawn)});
    if (eye > depth)
        return fmt(tr_("On this graphics card, the map's 3D view is flat, not in depth, above %1."),
                   {std::to_string(depth)});
    return "";
}

// In GB with one decimal.
static std::string gb(int mb) {
    char b[32];
    snprintf(b, sizeof b, "%.1f", (double)mb / 1024.0);
    return b;
}

std::string memory_note(const EyeSettings &s, bool steamvr, const GpuInfo &g,
                        const std::vector<SessionMemory> &past) {
    const bool nvidia = g.vendor == 0x10de;
    const auto need = [&](int eye, const std::string &mode) {
        return images_mb(eye, render_size(eye, mode), nvidia, steamvr);
    };
    const int now = need(s.eye, s.upscaler);
    // What a session here did beats the estimate: one that ran out with no more to hold, or
    // one that didn't with no less.
    for (const SessionMemory &p : past) {
        if (p.steamvr != steamvr || (g.heap_mb && p.card_mb != g.heap_mb))
            continue;
        const int then = need(p.eye, p.upscaler);
        if (p.ran_out() && then <= now)
            return fmt(tr_("In an earlier session at %1, X4 ran out of graphics memory before the "
                           "save was loaded, and some textures may have been missing. Choose a "
                           "smaller size."),
                       {std::to_string(p.eye)});
        if (!p.ran_out() && then >= now)
            return "";
    }
    if (!g.discrete || !g.budget_mb || (int)g.budget_mb - now >= kTexturesNeedMb)
        return "";
    int fits = 0;
    for (int e : eye_sizes())
        if ((int)g.budget_mb - need(e, s.upscaler) >= kTexturesNeedMb)
            fits = std::max(fits, e);
    std::string note =
        fmt(tr_("At this size X4's images alone take about %1 GB, and your graphics card has %2 "
                "GB free. What's left is too little for the textures, and some may be missing."),
            {gb(now), gb((int)g.budget_mb)});
    // No upscaler suggested: it saves about a tenth (take 460).
    note += " " + (fits ? fmt(tr_("Choose %1 or less."), {std::to_string(fits)})
                        : std::string(tr_("Close other programs that use the graphics card.")));
    return note;
}

std::string eye_note(const EyeSettings &s, bool steamvr, const GpuInfo &g,
                     const std::vector<SessionMemory> &past) {
    std::string note = memory_note(s, steamvr, g, past);
    const auto add = [&note](const std::string &more) {
        if (!more.empty())
            note += std::string(note.empty() ? "" : " ") + more;
    };
    add(map_note(s.eye, g.heap_mb));
    if (steamvr && s.eye > kAutoUpscaleAbove)
        add(tr_("At 4224 SteamVR has shown a jumbled picture. If you see that, go lower."));
    return note;
}

} // namespace x4vr::launcher
