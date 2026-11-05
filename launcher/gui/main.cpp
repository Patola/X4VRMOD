// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#include <QApplication>
#include <QIcon>

#include "window.hpp"

int main(int argc, char **argv) {
    using namespace x4vr::launcher;
    QApplication app(argc, argv);
    QApplication::setApplicationName("x4vrmod-launcher");
    QApplication::setApplicationDisplayName("X4 VR Mod");
    QApplication::setApplicationVersion(own_version());
    QGuiApplication::setDesktopFileName("x4vrmod-launcher");
    const std::string home = home_dir();
    QApplication::setWindowIcon(
        QIcon(QString::fromStdString(detect_layout(home).help_dir) + "/icon.png"));
    gui::Window window(home, gui::Window::Options{});
    window.show();
    return app.exec();
}
