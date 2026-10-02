// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_content.hpp — X4's content.xml, forked so OUR extensions are enabled
// for the modded session only.
//
// content.xml (which extensions are enabled) lives in the SAME profile
// directory vanilla X4 reads. Enabling x4vrmod_lookaround there would enable
// it for vanilla too. So, like config.xml: the player's file is read once to
// seed content-x4vrmod.xml, every read X4 makes is answered from the fork with
// our extensions forced on in memory, and every write goes to the fork.
//
// Matching is by DIRECTORY as much as by name: every extension carries its own
// content.xml under X4's extensions/ folder, and those must pass untouched.
#pragma once

#include <cstring>
#include <string>

namespace x4vr {

// The profile's content.xml: basename "content.xml", under an EgoSoft/X4
// profile directory, and NOT an extension's own manifest.
inline bool is_x4_profile_content(const char *path) {
    if (!path)
        return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    return strcmp(base, "content.xml") == 0 && !strstr(path, "/extensions/") &&
           strstr(path, "/EgoSoft/X4/") != nullptr;
}

// .../content-x4vrmod.xml, alongside it. Does not end in "content.xml", so it
// can never match the test above.
inline std::string content_profile_path(const char *content_path) {
    std::string p = content_path ? content_path : "";
    const size_t slash = p.rfind('/');
    p.replace(slash == std::string::npos ? 0 : slash + 1, std::string::npos,
              "content-x4vrmod.xml");
    return p;
}

// Force `<extension id="ID" ... enabled="true|false"/>` in `xml`. Returns what
// it did: 0 already so, 1 flipped an existing entry, 2 inserted a new one, -1
// could not (no </content>). The rest of the file is left byte for byte.
//
// OFF is needed as much as ON: X4 writes its content.xml into our fork, so an
// extension one session enabled would stay enabled in every later session
// unless the knob that owns it says otherwise each time.
inline int set_extension(std::string &xml, const char *id, bool on) {
    const char *want = on ? "true" : "false";
    const std::string key = std::string("<extension id=\"") + id + "\"";
    const size_t at = xml.find(key);
    if (at != std::string::npos) {
        const size_t end = xml.find('>', at);
        if (end == std::string::npos)
            return -1;
        std::string el = xml.substr(at, end - at);
        const size_t en = el.find("enabled=\"");
        if (en != std::string::npos) {
            const size_t v = en + 9;
            const size_t q = el.find('"', v);
            if (q == std::string::npos)
                return -1;
            if (el.compare(v, q - v, want) == 0)
                return 0;
            el.replace(v, q - v, want);
        } else {
            // No attribute: X4's default is not ours to assume. Say it.
            const size_t ins = el.back() == '/' ? el.size() - 1 : el.size();
            el.insert(ins, std::string(" enabled=\"") + want + "\"");
        }
        xml.replace(at, end - at, el);
        return 1;
    }
    const size_t close = xml.rfind("</content>");
    if (close == std::string::npos)
        return -1;
    xml.insert(close, std::string("  <extension id=\"") + id + "\" enabled=\"" +
                          want + "\"/>\n");
    return 2;
}

inline int enable_extension(std::string &xml, const char *id) {
    return set_extension(xml, id, true);
}

} // namespace x4vr
