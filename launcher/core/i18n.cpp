// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#include "x4vr_i18n.hpp"

#include <cstdlib>

namespace x4vr::launcher {

std::string fmt(const std::string &pattern, std::initializer_list<std::string> values) {
    const std::vector<std::string> v(values);
    std::string out;
    out.reserve(pattern.size() + 32);
    for (size_t i = 0; i < pattern.size(); i++) {
        const char c = pattern[i];
        if (c == '%' && i + 1 < pattern.size()) {
            const char d = pattern[i + 1];
            if (d == '%') {
                out += '%';
                i++;
                continue;
            }
            if (d >= '1' && d <= '9') {
                const size_t k = (size_t)(d - '1');
                if (k < v.size())
                    out += v[k];
                i++;
                continue;
            }
        }
        out += c;
    }
    return out;
}

namespace {

// The offered language for a locale name, or "" when it isn't one of them.
std::string offered(const std::string &locale) {
    // "pt_BR.UTF-8@euro" -> "pt_BR"
    std::string l = locale.substr(0, locale.find_first_of(".@"));
    const std::string lang = l.substr(0, l.find('_'));
    for (const Language &x : kLanguages)
        if (l == x.code)
            return x.code;
    for (const Language &x : kLanguages)
        if (lang == x.code)
            return x.code;
    if (lang == "pt")
        return "pt_BR";
    return "";
}

const char *process_env(const char *name) { return getenv(name); }

} // namespace

std::string language_for_locale(const std::string &locale) {
    const std::string l = offered(locale);
    return l.empty() ? "en" : l;
}

bool is_language(const std::string &code) {
    for (const Language &x : kLanguages)
        if (code == x.code)
            return true;
    return false;
}

std::string system_language(EnvFn env) {
    if (!env)
        env = process_env;
    if (const char *list = env("LANGUAGE")) {
        const std::string s = list;
        for (size_t at = 0; at <= s.size();) {
            size_t end = s.find(':', at);
            if (end == std::string::npos)
                end = s.size();
            const std::string l = offered(s.substr(at, end - at));
            if (!l.empty())
                return l;
            at = end + 1;
        }
    }
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char *v = env(name); v && *v)
            return language_for_locale(v);
    return "en";
}

std::vector<std::string> message_locales_for(const std::string &code) {
    static const std::pair<const char *, const char *> kCountry[] = {
        {"en", "en_US"}, {"pt_BR", "pt_BR"}, {"fr", "fr_FR"}, {"ru", "ru_RU"}, {"de", "de_DE"}};
    std::vector<std::string> out;
    for (const auto &[c, l] : kCountry)
        if (code == c)
            out.push_back(std::string(l) + ".UTF-8");
    if (code != "en")
        out.push_back("en_US.UTF-8");
    // Before glibc 2.35, Debian's C.UTF-8 was a locale of its own and followed
    // LANGUAGE.
    out.push_back("C.UTF-8");
    return out;
}

} // namespace x4vr::launcher
