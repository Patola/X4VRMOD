// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The launcher's translations: GNU gettext, domain "x4vrmod".
//
// The window translates; the x4vrmod command doesn't, and needn't do anything
// for that. Qt sets the program's locale from the environment, the command
// never does, and gettext with the "C" locale answers every message with its
// English original. So the core marks its words once and the command's output,
// which diagnostics read, stays English (Patola, 2026-11-08).
//
// Words go through tr_() (and trn_() with a count); a sentence with values in it
// is one template with %1..%9, filled by fmt(), so a language can put them in
// its own order. tools/i18n-update.sh collects what's marked into
// launcher/po/x4vrmod.pot and merges it into each language's .po.
#pragma once

#include <libintl.h>
#include <locale.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace x4vr::launcher {

inline constexpr const char *kTextDomain = "x4vrmod";

inline const char *tr_(const char *msgid) { return dgettext(kTextDomain, msgid); }

inline const char *trn_(const char *one, const char *many, unsigned long n) {
    return dngettext(kTextDomain, one, many, n);
}

// Marks a word for translation where it's written, as in a table, and
// leaves it as it is: tr_() translates it where it's shown.
inline constexpr const char *N_(const char *msgid) { return msgid; }

// %1..%9 in `pattern`, each replaced by its value; %% is a percent sign.
std::string fmt(const std::string &pattern, std::initializer_list<std::string> values);

// English on this thread while it lives, whatever the window is in: a bug
// report is for us to read, and the window builds one on a thread of its own.
// gettext follows the thread's locale (uselocale), so the window's other
// threads keep their language.
class InEnglish {
public:
    InEnglish() : m_c(newlocale(LC_ALL_MASK, "C", (locale_t)0)) {
        if (m_c)
            m_was = uselocale(m_c);
    }
    ~InEnglish() {
        if (m_c) {
            uselocale(m_was);
            freelocale(m_c);
        }
    }
    InEnglish(const InEnglish &) = delete;
    InEnglish &operator=(const InEnglish &) = delete;

private:
    locale_t m_c, m_was = (locale_t)0;
};

// The languages the window offers, each named in itself, in Patola's order.
struct Language {
    const char *code; // gettext's: en, pt_BR, fr, ru, de
    const char *name; // as its speakers write it
};
inline constexpr Language kLanguages[] = {
    {"en", "English"},   {"pt_BR", "Português Brasileiro"}, {"fr", "Français"},
    {"ru", "Русский"},   {"de", "Deutsch"},
};

// One of kLanguages' codes for a locale name such as "pt_BR.UTF-8" or "de_AT":
// the language with its country if offered, else the language alone, else
// Portuguese of any country as Brazil's, else English.
std::string language_for_locale(const std::string &locale);
bool is_language(const std::string &code); // one of kLanguages'

// The system's language, read as gettext would: the first offered language in
// LANGUAGE (KDE sets it from its own settings), else LC_ALL, LC_MESSAGES, LANG.
// `env` stands in for getenv in the tests.
using EnvFn = const char *(*)(const char *);
std::string system_language(EnvFn env = nullptr);

// Locales under which gettext would follow LANGUAGE to `code`, best first. glibc
// ignores LANGUAGE in the C locale, and since 2.35 in C.UTF-8 as well, so a
// player whose system says C needs a real locale for their choice to show.
std::vector<std::string> message_locales_for(const std::string &code);

} // namespace x4vr::launcher
