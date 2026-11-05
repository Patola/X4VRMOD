// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// window.hpp -- x4vrmod-launcher's main window. It only shows what the core decides:
// every check, install and launch rule lives in launcher/core, so the command line
// and the window can't disagree (docs/packaging-plan.md, Phase 3).
#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "x4vr_launcher.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QThread;
class QTimer;
class QToolButton;

namespace x4vr::launcher::gui {

class HelpWindow;

class Window : public QWidget {
public:
    struct Options {
        // False: no checks, timers, dialogs or launches, so a test can feed the rows
        // and read what the window would have done.
        bool live = true;
    };
    Window(const std::string &home, const Options &options);
    ~Window() override;

    // One row per check, and the Play button from them.
    void showChecks(const std::vector<Check> &rows);
    // A folder or a file, as Browse hands it over. Says why if it can't be used.
    void chooseGame(const QString &path);
    void openHelp(const QString &anchor);
    // As the two checkboxes beside it say: with a debug log, or X4 by itself.
    void play();

    QString lastMessage() const { return m_lastMessage; }
    QStringList lastLaunch() const { return m_lastLaunch; }
    std::string gameExe() const { return m_game; }
    bool checksRan() const { return m_checksDone > 0; }

protected:
    bool event(QEvent *e) override;

private:
    struct Row {
        QLabel *icon = nullptr, *text = nullptr;
        QPushButton *fix = nullptr;
    };

    void buildUi();
    QWidget *buildSettings();
    QWidget *buildChecks();

    void reloadGames();
    void syncSettings();
    void saveProfile();
    void gameChosen();
    void eyeChosen();
    void upscalerChosen();
    void ipdChanged(double mm);
    void flatToggled();
    void browse();

    void requestChecks(bool forceVr);
    void applyChecks(const std::vector<Check> &rows, bool keepVr, bool keepGpu);
    void updateRow(const std::string &id);
    void updateVrStart(bool wanted);
    void startProgram(const QString &program, const QStringList &args);
    void updatePlay();
    void fitHeight();
    void tick();
    void sessionEnded();

    void runFix(const std::string &id, const std::string &fix);
    void install();
    void uninstall();
    void showDetails(const std::string &id);
    void launchOptions();
    void advanced();
    void resetDefaults();
    void about();
    void addToMenu();
    void openLogs();
    void report();

    void message(const QString &title, const QString &text, bool warning);
    bool confirm(const QString &title, const QString &text, const QString &yes);
    QString shortPath(const std::string &path) const;

    std::string m_home;
    Options m_opt;
    Layout m_layout;
    Profile m_profile;
    EyeSettings m_eye;
    std::string m_game;
    bool m_syncing = false;
    QString m_note;

    QComboBox *m_gameBox = nullptr, *m_eyeBox = nullptr, *m_upBox = nullptr;
    QLabel *m_renders = nullptr, *m_status = nullptr;
    QDoubleSpinBox *m_ipd = nullptr;
    QCheckBox *m_debug = nullptr, *m_flat = nullptr;
    QPushButton *m_play = nullptr;
    QToolButton *m_vrStart = nullptr;
    QString m_vrChoices;
    QAction *m_menuAction = nullptr;
    std::map<std::string, Row> m_rows;
    std::map<std::string, Check> m_checks;
    QPointer<HelpWindow> m_help;

    QPointer<QThread> m_worker;
    QTimer *m_timer = nullptr;
    int m_checksDone = 0, m_ticks = 0;
    bool m_again = false, m_againForce = false;
    std::string m_runtimePath;
    bool m_steamvr = false, m_steamvrUp = false;
    unsigned m_headsetAsks = 0;
    std::chrono::steady_clock::time_point m_lastVr{};

    qint64 m_gamePid = 0;
    std::chrono::steady_clock::time_point m_gameStart{};
    bool m_gameDebug = false;
    QString m_lastSession, m_outputFile;

    QString m_lastMessage;
    QStringList m_lastLaunch;
};

} // namespace x4vr::launcher::gui
