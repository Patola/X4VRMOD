// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The player's settings, ~/.config/x4vrmod/profile.toml, and the one rule that
// ties the eye size to the upscaler (docs/packaging-plan.md, decision 6).
#include "x4vr_launcher.hpp"

#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace x4vr::launcher {

// README, "Resolution and upscaling". Measured for fsr_quality and fsr3_balanced,
// AMD's standard ratios for the others.
const std::vector<Upscaler> &upscalers() {
    static const std::vector<Upscaler> v = {
        {"none", "None (full resolution)", 1.0},
        {"fsr_ultra_quality", "FSR 1 Ultra Quality", 1.3},
        {"fsr_quality", "FSR 1 Quality", 1.5},
        {"fsr_balanced", "FSR 1 Balanced", 1.7},
        {"fsr_performance", "FSR 1 Performance", 2.0},
        {"fsr3_native", "FSR 3 Native (anti-aliasing only)", 1.0},
        {"fsr3_quality", "FSR 3 Quality", 1.5},
        {"fsr3_balanced", "FSR 3 Balanced", 1.7},
        {"fsr3_performance", "FSR 3 Performance", 2.0},
        {"fsr3_ultra_performance", "FSR 3 Ultra Performance", 3.0},
    };
    return v;
}

const std::vector<int> &eye_sizes() {
    static const std::vector<int> v = {1600, 2048, 2400, 2816, 3072, 3200, 3600, 4000, 4224};
    return v;
}

std::string eye_note(int eye, bool steamvr) {
    std::string note;
    // docs/packaging-plan.md, Resolution: takes 376-378 and Patola, 2026-10-31.
    if (eye > 4068)
        note = "Above 4068 the map's 3D view is flat, not in depth.";
    if (steamvr && eye > kAutoUpscaleAbove)
        note += std::string(note.empty() ? "" : " ") +
                "At 4224 SteamVR has shown a jumbled picture. If you see that, go lower.";
    return note;
}

bool valid_upscaler(const std::string &mode) {
    for (const Upscaler &u : upscalers())
        if (mode == u.mode)
            return true;
    return false;
}

int render_size(int eye, const std::string &mode) {
    for (const Upscaler &u : upscalers())
        if (mode == u.mode)
            return (int)(eye / u.ratio + 0.5);
    return eye;
}

std::string set_eye(EyeSettings &s, int eye) {
    const bool was_large = s.eye > kAutoUpscaleAbove;
    const bool large = eye > kAutoUpscaleAbove;
    s.eye = eye;
    if (large && !was_large && s.upscaler == "none") {
        s.upscaler = kAutoUpscaler;
        s.upscaler_auto = true;
        return "FSR 1 Quality chosen for this size. Choose None for full resolution.";
    }
    if (!large && was_large && s.upscaler_auto) {
        s.upscaler = "none";
        s.upscaler_auto = false;
    }
    return "";
}

void set_upscaler(EyeSettings &s, const std::string &mode) {
    s.upscaler = mode;
    s.upscaler_auto = false;
}

std::string profile_path(const std::string &home) {
    const char *x = getenv("XDG_CONFIG_HOME");
    return (x && *x ? std::string(x) : home + "/.config") + "/x4vrmod/profile.toml";
}

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

Profile load_profile(const std::string &path) {
    Profile p;
    std::stringstream ss(read_text_file(path, 1024 * 1024));
    std::string line;
    while (std::getline(ss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            std::string out;
            for (size_t i = 1; i + 1 < v.size(); i++) {
                if (v[i] == '\\' && i + 2 < v.size())
                    i++;
                out.push_back(v[i]);
            }
            v = out;
        }
        if (!k.empty())
            p.values[k] = v;
    }
    return p;
}

bool save_profile(const std::string &path, const Profile &p) {
    std::error_code ec;
    fs::create_directories(dir_name(path), ec);
    const std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    fputs("# X4 VR Mod's settings. The launcher and `x4vrmod config` write this file.\n", f);
    for (const auto &[k, v] : p.values) {
        std::string q;
        for (char c : v) {
            if (c == '"' || c == '\\')
                q.push_back('\\');
            q.push_back(c);
        }
        fprintf(f, "%s = \"%s\"\n", k.c_str(), q.c_str());
    }
    const bool ok = fclose(f) == 0;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string Profile::get(const std::string &k, const std::string &fallback) const {
    const auto it = values.find(k);
    return it == values.end() ? fallback : it->second;
}

EyeSettings eye_settings(const Profile &p) {
    EyeSettings s;
    const int eye = atoi(p.get("eye").c_str());
    if (eye > 0)
        s.eye = eye;
    s.upscaler = p.get("upscaler", "none");
    s.upscaler_auto = p.get("upscaler_auto") == "1";
    return s;
}

void store_eye_settings(Profile &p, const EyeSettings &s) {
    p.values["eye"] = std::to_string(s.eye);
    p.values["upscaler"] = s.upscaler;
    if (s.upscaler_auto)
        p.values["upscaler_auto"] = "1";
    else
        p.values.erase("upscaler_auto");
}

std::vector<std::pair<std::string, std::string>> profile_env(const Profile &p) {
    std::vector<std::pair<std::string, std::string>> out;
    static const struct { const char *key, *env; } kMap[] = {
        {"eye", "X4VR_H"},        {"upscaler", "X4VR_UPSCALE"}, {"ipd", "X4VR_IPD"},
        {"fov", "X4VR_FOV"},
    };
    for (const auto &m : kMap) {
        const auto it = p.values.find(m.key);
        if (it != p.values.end() && !it->second.empty())
            out.emplace_back(m.env, it->second);
    }
    // "env.NAME": Advanced's extra environment variables.
    for (const auto &[k, v] : p.values)
        if (k.rfind("env.", 0) == 0 && k.size() > 4)
            out.emplace_back(k.substr(4), v);
    return out;
}

std::string profile_set(Profile &p, const std::string &key, const std::string &value) {
    auto number = [&value](double lo, double hi) {
        char *end = nullptr;
        const double d = strtod(value.c_str(), &end);
        return !value.empty() && end && !*end && d >= lo && d <= hi;
    };
    if (key == "eye") {
        // Digits only: strtod would pass 3e3, and atoi would read it as 3.
        if (!number(256, 16384) ||
            value.find_first_not_of("0123456789") != std::string::npos)
            return "error: eye is a size in pixels, like 2816";
        EyeSettings s = eye_settings(p);
        const std::string note = set_eye(s, atoi(value.c_str()));
        store_eye_settings(p, s);
        return note;
    }
    if (key == "upscaler") {
        if (!valid_upscaler(value))
            return "error: not one of X4's upscaler modes (README, Resolution and upscaling)";
        EyeSettings s = eye_settings(p);
        set_upscaler(s, value);
        store_eye_settings(p, s);
        return "";
    }
    if (key == "ipd") {
        if (!number(0.04, 0.09))
            return "error: ipd is in metres, like 0.064";
    } else if (key == "fov") {
        if (!number(0.5, 2.5))
            return "error: fov is X4's field of view, like 1.4917";
    } else if (key == "game") {
        const GamePath g = classify_game_path(value);
        if (g.kind == PathKind::Windows)
            return std::string("error: ") + proton_explanation();
        if (g.kind != PathKind::Linux)
            return "error: " + g.why;
        p.values[key] = g.exe;
        return "";
    } else if (key.rfind("env.", 0) != 0 || key.size() <= 4) {
        return "error: unknown setting (eye, upscaler, ipd, fov, game, env.NAME)";
    } else if (key[4] >= '0' && key[4] <= '9') {
        return "error: not a variable name: " + key.substr(4);
    } else if (key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                     "0123456789_", 4) != std::string::npos) {
        return "error: not a variable name: " + key.substr(4);
    }
    p.values[key] = value;
    return "";
}

} // namespace x4vr::launcher
