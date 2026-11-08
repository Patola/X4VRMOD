// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#include <QApplication>
#include <QIcon>
#include <QTimer>
#include <QTranslator>

#include <memory>

#include "i18n.hpp"
#include "window.hpp"

int main(int argc, char **argv) {
    using namespace x4vr::launcher;
    QApplication app(argc, argv);
    QApplication::setApplicationName("x4vrmod-launcher");
    QApplication::setApplicationDisplayName("X4 VR Mod");
    QApplication::setApplicationVersion(own_version());
    QGuiApplication::setDesktopFileName("x4vrmod-launcher");
    const std::string home = home_dir();
    const Layout layout = detect_layout(home);
    QApplication::setWindowIcon(QIcon(QString::fromStdString(layout.help_dir) + "/icon.png"));

    // Before LANGUAGE changes: what the programs the window starts are given.
    gui::started_environment();
    // The system's language, until the player picks one in the window.
    QTranslator qt;
    const std::string chosen = load_profile(profile_path(home)).get("language");
    gui::apply_language(is_language(chosen) ? chosen : system_language(), layout.locale_dir, qt);

    // A new language is a new window: every text in it is made where it's built.
    std::unique_ptr<gui::Window> window;
    gui::Window::Options options;
    options.languageChanged = [&](const std::string &code) {
        QTimer::singleShot(0, &app, [&, code] {
            const QPoint at = window->pos();
            // The old window goes first: its checks may still be reading the language.
            QApplication::setQuitOnLastWindowClosed(false);
            window.reset();
            gui::apply_language(code, layout.locale_dir, qt);
            window = std::make_unique<gui::Window>(home, options);
            window->move(at);
            window->show();
            QApplication::setQuitOnLastWindowClosed(true);
        });
    };
    window = std::make_unique<gui::Window>(home, options);
    window->show();
    return app.exec();
}
