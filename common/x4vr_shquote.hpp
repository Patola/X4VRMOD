// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Shell-quoting for the injector's `env: run =` line.
//
// That line is not decoration. The documented way to build the next take's
// command is to diff it against the previous take's -- and take 258 shipped
//
//     X4VR_WATCH_WANT=ui dump: burst frames
//
// which no shell reads as one assignment: `X4VR_WATCH_WANT=ui` is the
// assignment, `dump:` is the command, and the run does not start. Patola
// repaired it by hand. The comment above that code claimed "copy-pasteable"
// while the code could not honour it for any value containing a space.
//
// Extracted into its own header so it can be round-tripped through a real
// shell in tests/ rather than reasoned about.
#pragma once

#include <cctype>
#include <cstring>
#include <string>

namespace x4vr {

// True when `v` survives an unquoted shell word intact. Deliberately
// conservative: the set is what appears in X4VR_* values today (numbers,
// paths, resolutions, ratios like 1408x1408, labels like 258-BURST) and
// nothing whose meaning depends on the shell. Empty is NOT safe -- a bare
// `X4VR_LOG=` is a valid assignment but reads as unset-vs-empty ambiguity in
// a hand-copied line, and `''` says which one was meant.
inline bool sh_word_safe(const char *v) {
    if (!v || !*v)
        return false;
    for (const char *p = v; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("_-+.,:/=@%^", *p)))
            return false;
    return true;
}

// `v` as one shell word. Single quotes, because inside them the only
// character with any meaning is the closing quote itself -- no escapes, no
// variable expansion, no history expansion, identical in sh, bash and zsh.
inline std::string sh_quote(const char *v) {
    if (sh_word_safe(v))
        return std::string(v);
    std::string q = "'";
    for (const char *p = v ? v : ""; *p; p++) {
        if (*p == '\'')
            q += "'\\''";   // close, escaped literal quote, reopen
        else
            q += *p;
    }
    q += '\'';
    return q;
}

// One `NAME=value` entry of `environ`, with the value quoted if it needs it.
// The name is passed through: an environment variable name that needed
// quoting could not have been set in the first place.
inline std::string sh_quote_assignment(const char *entry) {
    if (!entry)
        return std::string();
    const char *eq = strchr(entry, '=');
    if (!eq)                    // not an assignment; nothing to quote
        return std::string(entry);
    std::string out(entry, (size_t)(eq - entry) + 1);
    out += sh_quote(eq + 1);
    return out;
}

} // namespace x4vr
