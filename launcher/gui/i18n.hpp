// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The window's side of the translations (core/x4vr_i18n.hpp): qtr() and qtrn()
// hand Qt a translated QString. Named apart from QObject::tr, which a widget's
// member functions would otherwise pick.
#pragma once

#include <QCoreApplication>
#include <QDesktopServices>
#include <QLibraryInfo>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTranslator>
#include <QUrl>

#include <clocale>
#include <cstdlib>
#include <cstring>
#include <string>

#include "x4vr_i18n.hpp"

namespace x4vr::launcher::gui {

inline QString qtr(const char *msgid) { return QString::fromUtf8(tr_(msgid)); }

inline QString qtrn(const char *one, const char *many, unsigned long n) {
    return QString::fromUtf8(trn_(one, many, n));
}

// The language the window is in: main.cpp sets LANGUAGE to it before the window
// exists, so gettext and this agree. English when nothing has.
inline std::string current_language() {
    const char *l = getenv("LANGUAGE");
    if (!l || !*l)
        return "en";
    std::string s = l;
    return s.substr(0, s.find(':'));
}

inline bool is_c_locale(const char *l) {
    return !l || !strcmp(l, "C") || !strcmp(l, "POSIX") || !strncmp(l, "C.", 2);
}

// The window's words in `code`, and Qt's own (its dialogs' buttons) with them.
// The x4vrmod command never comes through here, so what it prints stays English.
// False when the system has no locale to show a translation under: then the
// window is English whatever `code` says.
inline bool apply_language(const std::string &code, const std::string &localeDir, QTranslator &qt) {
    setenv("LANGUAGE", code.c_str(), 1);
    bool shown = !is_c_locale(setlocale(LC_MESSAGES, nullptr));
    for (size_t i = 0; !shown && i < message_locales_for(code).size(); i++) {
        const std::string l = message_locales_for(code)[i];
        shown = setlocale(LC_MESSAGES, l.c_str()) && !is_c_locale(l.c_str());
    }
    bindtextdomain(kTextDomain, localeDir.c_str());
    bind_textdomain_codeset(kTextDomain, "UTF-8");
    // gettext keeps what it has looked up; this makes it look again.
    textdomain(textdomain(nullptr));

    QCoreApplication::removeTranslator(&qt);
    if (qt.load(QLocale(QString::fromStdString(code)), "qtbase", "_",
                QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(&qt);
    return shown || code == "en";
}

// The environment this program was started with. The window's language lives in
// LANGUAGE, which every program it starts would inherit: X4 through `x4vrmod run`,
// WiVRn's dashboard, Steam, the file manager. They get this instead, so choosing
// Deutsch here changes nothing outside this window. main.cpp asks for it first.
inline const QProcessEnvironment &started_environment() {
    static const QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    return e;
}

inline bool start_outside(const QString &program, const QStringList &args, qint64 *pid = nullptr) {
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setProcessEnvironment(started_environment());
    return p.startDetached(pid);
}

// A page or a folder, in the program the desktop picks, as start_outside starts it.
// QDesktopServices, which hands it the window's environment, only without xdg-open.
inline void open_outside(const QUrl &url) {
    if (!start_outside("xdg-open", {url.toString(QUrl::FullyEncoded)}))
        QDesktopServices::openUrl(url);
}

} // namespace x4vr::launcher::gui
