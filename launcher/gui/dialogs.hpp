// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// dialogs.hpp -- the window's dialogs. Uninstall is the one with a rule of its own:
// the player's VR settings go only if asked, and it says that can't be undone.
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

#include "x4vr_launcher.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QThread;

namespace x4vr::launcher::gui {

class HelpWindow : public QDialog {
public:
    HelpWindow(const std::string &helpDir, QWidget *parent);
    void show(const QString &anchor);

private:
    QTextBrowser *m_view;
    bool m_loaded = false;
};

class UninstallDialog : public QDialog {
public:
    // `withSettings` is the same plan with the player's VR settings in it.
    UninstallDialog(const UninstallPlan &withSettings, QWidget *parent);
    bool alsoSettings() const;

private:
    QCheckBox *m_also;
};

class LaunchOptionsDialog : public QDialog {
public:
    LaunchOptionsDialog(const QString &steamLine, const QString &terminalLine,
                        const std::string &helpDir, QWidget *parent);
};

// Makes the report on a worker thread, because asking the VR runtime can take seconds,
// then offers the files and a new GitHub issue. Nothing goes anywhere by itself.
class ReportDialog : public QDialog {
public:
    ReportDialog(const std::string &home, const std::string &game, QWidget *parent);
    ~ReportDialog() override;
    void create();
    bool hasResult() const { return m_hasResult; }
    const ReportResult &result() const { return m_result; }
    bool wantsDebugRun() const { return m_wantsDebug; }

private:
    std::string m_home, m_game;
    QComboBox *m_session;
    QPlainTextEdit *m_note;
    QPushButton *m_create, *m_files, *m_issue;
    QLabel *m_outcome;
    QPointer<QThread> m_worker;
    ReportResult m_result;
    bool m_hasResult = false, m_wantsDebug = false;
};

class AdvancedDialog : public QDialog {
public:
    AdvancedDialog(const Profile &profile, QWidget *parent);
    // Applies to `p`, returning "" or what was wrong; `p` is unchanged on an error.
    QString apply(Profile &p) const;

private:
    QDoubleSpinBox *m_fov;
    QCheckBox *m_fovSet;
    QPlainTextEdit *m_env;
};

} // namespace x4vr::launcher::gui
