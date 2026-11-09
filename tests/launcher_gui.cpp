// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The window, with nothing real behind it: a fake home, a fake game and rows fed in
// by hand, on Qt's offscreen platform. The Play button from the rows, the eye-size
// rule through the real lists, Proton, the uninstall dialog's defaults, every help
// link and picture. Then a live window, for the worker thread. Self-checking.
//
//   x4vr_test_launcher_gui              the checks
//   x4vr_test_launcher_gui --shots DIR  also writes the help's pictures
//   ... --shots DIR --lang CODE         the pictures in one language. The checks read
//                                       English, so in another language they're printed
//                                       and not counted: this is for the pictures.
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QTest>
#include <QToolButton>
#include <QTranslator>

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "dialogs.hpp"
#include "i18n.hpp"
#include "window.hpp"

using namespace x4vr::launcher;
using gui::Window;

static int g_fail = 0;
// --lang: the checks read English, so they're printed and not counted.
static bool g_counted = true;
static void check(bool ok, const std::string &what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    if (!ok && g_counted)
        g_fail++;
}

// Titles and details are the core's words, so they go through its translations.
static Check row(const char *id, Status s, const char *title, const std::string &detail = "",
                 const char *fix = "") {
    Check c;
    c.id = id;
    c.status = s;
    c.title = tr_(title);
    c.detail = detail.empty() ? detail : std::string(tr_(detail.c_str()));
    c.fix = fix;
    return c;
}

// The help shows pictures at this width, smoothly: its own scaling isn't. What a
// hidden line frees is laid out by a posted event, which the window's own loop runs
// before it paints, so the picture waits for it too.
static void shot(QWidget &w, const QString &path) {
    QApplication::processEvents();
    w.grab().scaledToWidth(640, Qt::SmoothTransformation).save(path);
}

static std::vector<Check> good_rows() {
    // As checks.cpp words them.
    return {row("x4", Status::Ok, "X4 found",
                fmt(tr_("%1, X4 %2, build %3"), {"Steam", "9.00", "23660954"}) + ", /games/X4 Foundations"),
            row("patches", Status::Ok, "The mod fits this X4",
                fmt(trn_("all %1 feature matches its code", "all %1 features match its code", 10), {"10"})),
            row("steam", Status::Ok, "Steam running"),
            row("running", Status::Ok, "X4 not running yet"),
            row("gamescope", Status::Ok, "gamescope installed", "3.17.0"),
            row("vr", Status::Ok, "VR ready",
                fmt(tr_("%1, %2. It recommends %3 x %4 per eye."),
                    {"WiVRn", "Meta Quest 3 on WiVRn", "2889", "3026"})),
            row("mod", Status::Ok, "Mod installed", fmt(tr_("%1, up to date"), {"v0.9.10"})),
            row("gpu", Status::Ok, "GPU capable", fmt(tr_("%1, Vulkan %2"), {"Radeon RX 7900 XTX", "1.4"}))};
}

static void replace(std::vector<Check> &rows, const Check &c) {
    for (Check &r : rows)
        if (r.id == c.id)
            r = c;
}

static QString dump(const std::string &home) {
    return QString::fromStdString(load_profile(profile_path(home)).get("eye"));
}

static void test_play(Window &w, const QString &shots) {
    auto *play = w.findChild<QPushButton *>("play");
    auto *status = w.findChild<QLabel *>("status");
    check(play && status, "the Play button and its line exist");
    check(!play->isEnabled() && status->text().contains("Checking"), "no rows yet: Play waits");

    auto rows = good_rows();
    w.showChecks(rows);
    check(play->isEnabled() && play->text() == "Play", "all good: Play");
    check(play->styleSheet().contains("#2e9e4f"), "...and green");
    auto *x4 = w.findChild<QLabel *>("row-x4");
    check(x4 && x4->text().contains("build 23660954") && !x4->text().contains("/games"),
          "the X4 row leaves the folder to the Game box");
    for (const char *id : {"x4", "patches", "steam", "running", "gamescope", "vr", "mod", "gpu"}) {
        auto *r = w.findChild<QLabel *>(QString("row-") + id);
        check(r && r->toolTip().size() > 20, std::string("the ") + id + " row explains itself on hover");
    }
    auto *running = w.findChild<QLabel *>("row-running");
    check(running && running->toolTip().contains("already running"), "...each its own");
    for (const char *name : {"game", "eye", "upscaler", "debug", "flatscreen", "report"}) {
        auto *field = w.findChild<QWidget *>(name);
        check(field && !field->toolTip().isEmpty(), std::string(name) + " explains itself on hover");
    }
    bool noQuestionMarks = true;
    for (QToolButton *b : w.findChildren<QToolButton *>())
        noQuestionMarks = noQuestionMarks && b->text() != "?";
    check(noQuestionMarks, "no ? buttons");
    if (!shots.isEmpty())
        shot(w, shots + "/window-ready.png");

    replace(rows, row("patches", Status::Warn, "X4 build not supported yet",
                      fmt(tr_("These turn off: %1."),
                          {std::string(tr_("Clicking the cockpit HUD")) + "; " + tr_("The shadow fix")}),
                      "details"));
    w.showChecks(rows);
    check(play->isEnabled() && play->text() == "Play anyway", "amber: Play anyway");
    check(play->styleSheet().contains("#c67d00"), "...and amber");
    auto *fix = w.findChild<QPushButton *>("fix-patches");
    check(fix && fix->isVisibleTo(&w) && fix->text() == "Details", "the amber row offers Details");
    auto *amber = w.findChild<QLabel *>("row-patches");
    check(amber && amber->text().contains("not supported") && !amber->text().contains("turn off"),
          "...so the row itself is only its title");
    fix->click();
    check(w.lastMessage().contains("Clicking the cockpit HUD") && w.lastMessage().contains("still runs"),
          "...and Details says what turns off");
    if (!shots.isEmpty())
        shot(w, shots + "/window-amber.png");

    rows = good_rows();
    replace(rows, row("vr", Status::Block, "Headset not connected",
                      "Connect from the WiVRn app in the headset.", "retry"));
    replace(rows, row("mod", Status::Block, "Mod not installed",
                      "The x4vrmod extension isn't in X4's folder.", "install"));
    w.showChecks(rows);
    check(!play->isEnabled() && play->text() == "Play", "red: Play is greyed");
    check(play->styleSheet().isEmpty(), "...in the theme's own colours");
    check(status->text().contains("2 pending actions"), "...and counts them");
    fix = w.findChild<QPushButton *>("fix-mod");
    check(fix && fix->isVisibleTo(&w) && fix->text().startsWith("Install"), "the mod's row offers Install");
    fix = w.findChild<QPushButton *>("fix-vr");
    check(fix && fix->isVisibleTo(&w) && fix->text() == "Retry", "the VR row offers Retry");
    fix = w.findChild<QPushButton *>("fix-x4");
    check(fix && !fix->isVisibleTo(&w), "a good row offers nothing");
    {
        auto flat = rows;
        replace(flat, row("x4", Status::Block, "Steam's Flatpak isn't supported", "", "help-flatpak"));
        w.showChecks(flat);
        check(fix && fix->isVisibleTo(&w) && fix->text() == "Help",
              "Steam's Flatpak's copy: the row offers the help's section on it");
        w.showChecks(rows);
    }
    if (!shots.isEmpty())
        shot(w, shots + "/window-fix.png");

    replace(rows, good_rows()[5]);
    w.showChecks(rows);
    check(status->text().contains("1 pending action") && status->text().contains("Mod not installed"),
          "one left: it's named");

    // A GOG copy has no Steam row, and it mustn't linger from a Steam copy.
    rows = good_rows();
    rows.erase(rows.begin() + 2);
    w.showChecks(rows);
    auto *steam = w.findChild<QLabel *>("row-steam");
    check(steam && !steam->isVisibleTo(&w), "no Steam check, no Steam row");
    w.showChecks(good_rows());
    check(steam && steam->isVisibleTo(&w), "...and it comes back");
    check(w.height() >= 400 && w.width() == 800, "the window is 800 wide and holds its rows");
    w.play();
    check(w.lastLaunch() == QStringList({"run", "--game", QString::fromStdString(w.gameExe())}),
          "Play runs `x4vrmod run --game` for the chosen copy");
    auto *debug = w.findChild<QCheckBox *>("debug");
    auto *flat = w.findChild<QCheckBox *>("flatscreen");
    check(debug && !debug->isChecked() && flat && !flat->isChecked(), "both checkboxes start off");
    debug->setChecked(true);
    w.play();
    check(w.lastLaunch() == QStringList({"run", "--debug", "--game", QString::fromStdString(w.gameExe())}),
          "Debug log asks for it");
    debug->setChecked(false);
    w.play();
    check(!w.lastLaunch().contains("--debug"), "...and off, it doesn't");
    auto *bug = w.findChild<QPushButton *>("report");
    check(bug && bug->isVisibleTo(&w) && bug->text() == "Report a bug", "Report a bug is in plain sight");
    auto *version = w.findChild<QLabel *>("version");
    check(version && version->text().contains("unofficial"), "the window says it's unofficial");

    // Flatscreen only needs a copy of X4 that can start, whatever VR and the mod are doing.
    // Red beats amber: features off can still start, with no VR runtime nothing can.
    auto both = good_rows();
    replace(both, row("patches", Status::Warn, "X4 build not supported yet", "These turn off: X.", "details"));
    replace(both, row("vr", Status::Block, "No VR runtime running", "", "start-vr"));
    w.showChecks(both);
    check(!play->isEnabled() && play->text() == "Play" && status->text().contains("No VR runtime running"),
          "amber and red together: greyed, naming what to fix");
    replace(both, good_rows()[5]);
    w.showChecks(both);
    check(play->isEnabled() && play->text() == "Play anyway", "...and once VR is up, Play anyway");

    auto modless = good_rows();
    replace(modless, row("vr", Status::Block, "No VR runtime running", "", "start-vr"));
    replace(modless, row("mod", Status::Block, "Mod not installed", "", "install"));
    replace(modless, row("gamescope", Status::Block, "gamescope isn't installed"));
    replace(modless, row("patches", Status::Warn, "X4 build not supported yet", "", "details"));
    w.showChecks(modless);
    check(!play->isEnabled(), "without VR and the mod, Play waits");
    {
        int installs = 0;
        for (QPushButton *b : w.findChildren<QPushButton *>())
            if (b->isVisibleTo(&w) && b->text().startsWith("Install"))
                installs++;
        check(installs == 1, "the mod's parts missing: one Install button");
    }
    flat->setChecked(true);
    check(play->isEnabled() && play->text() == "Play" && play->styleSheet().contains("#2e9e4f"),
          "Flatscreen: green all the same");
    check(status->text().contains("without the mod"), "...and it says what it's about to do");
    auto *modRow = w.findChild<QLabel *>("row-mod");
    auto *modFix = w.findChild<QPushButton *>("fix-mod");
    check(modRow && !modRow->isEnabled() && modFix && !modFix->isVisibleTo(&w),
          "...the mod's rows grey out, with nothing to fix");
    check(!w.findChild<QToolButton *>("start-vr")->isVisibleTo(&w), "...the VR row offers nothing");
    check(!debug->isEnabled() && !w.findChild<QComboBox *>("eye")->isEnabled(),
          "...and Debug log and the VR settings grey out");
    debug->setChecked(true);
    w.play();
    check(w.lastLaunch() == QStringList({"run", "--flat", "--game", QString::fromStdString(w.gameExe())}),
          "...Play starts it flat, without a debug log");
    debug->setChecked(false);
    if (!shots.isEmpty())
        shot(w, shots + "/window-flat.png");
    replace(modless, row("running", Status::Block, "X4 is already running", "Close it first."));
    w.showChecks(modless);
    check(!play->isEnabled() && status->text().contains("already running"), "...but X4 already running still stops it");
    auto steamDown = good_rows();
    replace(steamDown, row("steam", Status::Block, "Steam isn't running", "", "start-steam"));
    w.showChecks(steamDown);
    check(!play->isEnabled(), "...and so does Steam being closed");
    flat->setChecked(false);
    check(modRow->isEnabled() && debug->isEnabled() && w.findChild<QComboBox *>("eye")->isEnabled(),
          "off again, everything's back");
    w.showChecks(good_rows());
}

// Decision 6, through the real lists, down to what's saved.
static void test_eye_rule(Window &w, const std::string &home) {
    auto *eye = w.findChild<QComboBox *>("eye");
    auto *up = w.findChild<QComboBox *>("upscaler");
    auto *renders = w.findChild<QLabel *>("renders");
    check(eye && up && renders, "the eye and upscaler lists exist");
    check(eye->currentData().toInt() == 2816 && eye->currentText().contains("default"),
          "2816 is the default, and says so");
    check(up->currentData().toString() == "none", "no upscaler by default");
    check(renders->isHidden(), "no upscaler: no render line, since the eye list already says");

    eye->setCurrentIndex(eye->findData(3200));
    check(up->currentData().toString() == "none", "3200 stays native");
    eye->setCurrentIndex(eye->findData(4224));
    check(up->currentData().toString() == kAutoUpscaler, "4224 selects FSR 1 Quality");
    check(renders->text().contains("2816 x 2816") && renders->text().contains("FSR 1 Quality chosen"),
          "...shows what it renders and says why");
    check(renders->text().contains("flat"), "...and warns about the map above 4068");
    check(dump(home) == "4224", "...and is saved");
    check(load_profile(profile_path(home)).get("upscaler") == kAutoUpscaler, "...the upscaler too");

    up->setCurrentIndex(up->findData("none"));
    check(!renders->text().contains("Renders") && !renders->text().contains("chosen"),
          "unselected: no render line, and the note goes");
    check(!renders->isHidden() && renders->text().contains("flat"), "...the map's warning stays");
    eye->setCurrentIndex(eye->findData(4000));
    check(up->currentData().toString() == "none", "moving between large sizes respects that");
    check(renders->isHidden(), "4000 keeps the map in depth, so there's nothing to say");
    eye->setCurrentIndex(eye->findData(2816));
    eye->setCurrentIndex(eye->findData(3600));
    check(up->currentData().toString() == kAutoUpscaler, "crossing again selects it again");
    eye->setCurrentIndex(eye->findData(2816));
    check(up->currentData().toString() == "none", "back under: returns to None, since the rule chose it");

    up->setCurrentIndex(up->findData("fsr3_balanced"));
    eye->setCurrentIndex(eye->findData(4224));
    check(up->currentData().toString() == "fsr3_balanced", "a chosen upscaler survives going large");
    eye->setCurrentIndex(eye->findData(2816));
    check(up->currentData().toString() == "fsr3_balanced", "...and coming back");
    up->setCurrentIndex(up->findData("none"));

    // "Custom" with no dialog to answer it changes nothing.
    eye->setCurrentIndex(eye->findData(0));
    check(eye->currentData().toInt() == 2816, "Custom with no answer goes back");

    check(!w.findChild<QWidget *>("ipd"), "eye distance isn't asked for: it comes from the headset");

    // GameMode: only there to choose when it's installed, and the choice is kept.
    auto *gm = w.findChild<QCheckBox *>("gamemode");
    const QByteArray path_was = qgetenv("PATH");
    const std::string fake_bin = home + "/fake-bin";
    qputenv("PATH", QByteArray::fromStdString(home + "/nothing"));
    eye->setCurrentIndex(eye->findData(3200)); // anything that syncs the settings
    eye->setCurrentIndex(eye->findData(2816));
    check(gm && gm->isHidden(), "no gamemoderun: no GameMode box");
    QDir().mkpath(QString::fromStdString(fake_bin));
    {
        QFile f(QString::fromStdString(fake_bin + "/gamemoderun"));
        f.open(QIODevice::WriteOnly);
        f.write("#!/bin/sh\nexec \"$@\"\n");
        f.close();
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
    }
    qputenv("PATH", QByteArray::fromStdString(fake_bin));
    eye->setCurrentIndex(eye->findData(3200));
    eye->setCurrentIndex(eye->findData(2816));
    check(gm && !gm->isHidden() && !gm->isChecked(), "gamemoderun found: the box is there, off");
    check(!gm->toolTip().isEmpty(), "...and it explains itself on hover");
    gm->setChecked(true);
    check(load_profile(profile_path(home)).get("gamemode") == "1", "ticked, it's saved");
    gm->setChecked(false);
    check(load_profile(profile_path(home)).get("gamemode").empty(), "unticked, it's gone");
    qputenv("PATH", QByteArray::fromStdString(home + "/nothing"));
    eye->setCurrentIndex(eye->findData(3200));
    eye->setCurrentIndex(eye->findData(2816));
    check(gm && gm->isHidden(), "gamemoderun gone: so is the box");
    qputenv("PATH", path_was);

    // DLSS: on the list only with an RTX card, as the GPU row says.
    check(up->findData("dlss_quality") < 0, "a Radeon: DLSS isn't offered");
    auto rtx = good_rows();
    Check rtxRow = row("gpu", Status::Ok, "GPU capable", "NVIDIA GeForce RTX 3070, Vulkan 1.4, DLSS available");
    rtxRow.dlss = true;
    replace(rtx, rtxRow);
    w.showChecks(rtx);
    check(up->count() == (int)upscalers().size(), "an RTX card: every upscaler is offered");
    for (const char *m : {"dlss_auto", "dlss_dlaa", "dlss_quality", "dlss_balanced", "dlss_performance",
                          "dlss_ultra_performance"})
        check(up->findData(m) >= 0, std::string(m) + " is offered");
    check(up->currentData().toString() == "none", "offering it doesn't choose it");
    up->setCurrentIndex(up->findData("dlss_quality"));
    check(load_profile(profile_path(home)).get("upscaler") == "dlss_quality", "DLSS Quality is saved");
    check(!renders->isHidden() && renders->text().contains("hasn't been played"),
          "...and the window says it's untried in a headset");
    w.showChecks(good_rows());
    check(up->findData("dlss_quality") < 0, "the GPU row changes: it's off the list");
    check(up->currentData().toString() == "none" &&
              load_profile(profile_path(home)).get("upscaler") == "none",
          "...and a saved DLSS goes back to None");
    check(renders->text().contains("back to None"), "...saying why");
    up->setCurrentIndex(up->findData("fsr_quality")); // takes the note with it
    up->setCurrentIndex(up->findData("none"));
    w.showChecks({});
    check(!w.findChild<QCheckBox *>("lean"), "lean isn't a choice any more");
}

static void test_browse(Window &w, const std::string &tmp) {
    const std::string before = w.gameExe();
    const std::string win = tmp + "/proton/X4.exe";
    QDir().mkpath(QString::fromStdString(tmp + "/proton"));
    QFile f(QString::fromStdString(win));
    if (f.open(QIODevice::WriteOnly))
        f.write("MZ\x90");
    f.close();
    w.chooseGame(QString::fromStdString(win));
    check(w.lastMessage() == proton_explanation(), "X4.exe says why it can't work");
    check(w.gameExe() == before, "...and the game stays as it was");
    w.chooseGame(QString::fromStdString(tmp + "/nowhere"));
    check(!w.lastMessage().isEmpty() && w.lastMessage() != proton_explanation(),
          "a folder with no X4 says so");
    check(w.gameExe() == before, "...and the game stays as it was");
}

static void test_uninstall(const QString &shots) {
    UninstallPlan p;
    p.mod = {"/games/X4 Foundations/extensions/x4vrmod", "~/.local/share/x4vrmod"};
    p.settings = {"~/.config/EgoSoft/X4/1234567/config-x4vrmod.xml",
                  "~/.config/EgoSoft/X4/1234567/content-x4vrmod.xml", "~/.config/x4vrmod/profile.toml"};
    gui::UninstallDialog d(p, nullptr);
    auto *also = d.findChild<QCheckBox *>("also-settings");
    auto *warn = d.findChild<QLabel *>("settings-warning");
    auto *list = d.findChild<QPlainTextEdit *>("settings-list");
    auto *go = d.findChild<QPushButton *>("uninstall");
    check(also && !also->isChecked() && !d.alsoSettings(), "uninstall: VR settings are kept by default");
    check(warn && warn->isHidden() && list && list->isHidden(), "...their list stays hidden until asked");
    check(go && !go->isDefault(), "...and Enter doesn't uninstall");
    auto *mod = d.findChild<QPlainTextEdit *>("mod-list");
    check(mod && mod->toPlainText().contains("extensions/x4vrmod"), "it lists what goes");
    also->setChecked(true);
    check(d.alsoSettings() && !warn->isHidden() && !list->isHidden(), "asked for: they're shown");
    check(warn->text().contains("can't be undone"), "...with the warning");
    check(list->toPlainText().contains("config-x4vrmod.xml"), "...and the files");
    if (!shots.isEmpty()) {
        d.adjustSize();
        d.grab().save(shots + "/uninstall.png");
    }
}

static void test_report(const std::string &home, const QString &shots) {
    const std::string logs = home + "/.local/state/x4vrmod/logs";
    QDir().mkpath(QString::fromStdString(logs));
    QFile f(QString::fromStdString(logs + "/x4vrmod-20260105-053911-debug.log"));
    if (f.open(QIODevice::WriteOnly))
        f.write("[ 1.0] inject  the log\n");
    f.close();
    gui::ReportDialog d(home, "", nullptr);
    auto *session = d.findChild<QComboBox *>("session");
    check(session && session->count() == 1 && session->currentText().contains("debug log"),
          "report: the session is offered, marked as a debug log");
    auto *issue = d.findChild<QPushButton *>("issue");
    auto *files = d.findChild<QPushButton *>("files");
    check(issue && !issue->isEnabled() && files && !files->isEnabled(), "nothing to send until it's made");
    auto *note = d.findChild<QPlainTextEdit *>("note");
    note->setPlainText("The lights flicker in the Manticore.");
    if (!shots.isEmpty()) {
        d.adjustSize();
        d.grab().save(shots + "/report.png");
    }
    d.create();
    check(QTest::qWaitFor([&d] { return d.hasResult(); }, 30000), "the report is made in the background");
    check(d.result().ok, "...and succeeds: " + d.result().error);
    check(issue->isEnabled() && files->isEnabled(), "then it can be shown, or taken to GitHub");
    auto *outcome = d.findChild<QLabel *>("outcome");
    check(outcome && outcome->text().contains(".tar.gz"), "it says where the file is");
    check(d.result().issue_url.find("Manticore") != std::string::npos, "the note goes in the issue");
    auto *hint = d.findChild<QLabel *>("debug-hint");
    check(hint && hint->text().contains("Debug log"), "it points at the Debug log checkbox");
    gui::ReportDialog none(home + "/nobody", "", nullptr);
    auto *ns = none.findChild<QComboBox *>("session");
    check(ns && ns->count() == 1 && ns->currentData().toString().isEmpty(), "no logs: it says so, and still works");
}

static void test_launch_options(const std::string &help) {
    gui::LaunchOptionsDialog d("x4vrmod run %command%", "x4vrmod run", help, nullptr);
    auto *s = d.findChild<QLineEdit *>("steam-line");
    auto *t = d.findChild<QLineEdit *>("terminal-line");
    check(s && s->text() == "x4vrmod run %command%" && s->isReadOnly(), "the Steam line is shown");
    check(t && t->text() == "x4vrmod run", "the terminal line is shown");
}

static void test_advanced() {
    Profile p;
    p.values["env.X4VR_FOO"] = "1";
    p.values["eye"] = "3072";
    gui::AdvancedDialog d(p, nullptr);
    auto *env = d.findChild<QPlainTextEdit *>("env");
    check(env && env->toPlainText() == "X4VR_FOO=1", "advanced shows the extra variables");
    env->setPlainText("X4VR_BAR=two words\n\nX4VR_BAZ=3\n");
    check(d.apply(p).isEmpty(), "valid lines are accepted");
    check(p.get("env.X4VR_BAR") == "two words" && p.get("env.X4VR_BAZ") == "3", "...and stored");
    check(p.get("env.X4VR_FOO").empty(), "a line taken out is gone");
    check(p.get("eye") == "3072", "other settings aren't touched");
    env->setPlainText("not a line");
    check(!d.apply(p).isEmpty(), "a line that isn't NAME=value is refused");
    env->setPlainText("1BAD=x");
    check(!d.apply(p).isEmpty(), "a bad name is refused");
    check(p.get("env.X4VR_BAZ") == "3", "...and a refusal changes nothing");
}

// Every link goes somewhere, every picture exists, and every row has its section.
static void test_suggests_wivrn(Window &w) {
    const std::string keep = getenv("PATH");
    const std::string bin = dir_name(self_dir()) + "/tests/fake-wivrn";
    QDir().mkpath(QString::fromStdString(bin));
    QFile f(QString::fromStdString(bin + "/wivrn-server"));
    if (f.open(QIODevice::WriteOnly))
        f.write("#!/bin/sh\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile st(QString::fromStdString(bin + "/steam"));
    if (st.open(QIODevice::WriteOnly))
        st.write("#!/bin/sh\n");
    st.close();
    st.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto rows = good_rows();
    replace(rows, row("vr", Status::Block, "No VR runtime running", "", "start-vr"));
    setenv("PATH", bin.c_str(), 1);
    w.showChecks(rows);
    auto *start = w.findChild<QToolButton *>("start-vr");
    auto *fix = w.findChild<QPushButton *>("fix-vr");
    check(start && start->isVisibleTo(&w) && !fix->isVisibleTo(&w), "the VR row's button is a pull-down");
    check(start->text() == "Start WiVRn", "...and pressing it starts WiVRn");
    const QList<QAction *> choices = start->menu()->actions();
    check(choices.size() == 2 && choices[0]->text() == "Start WiVRn" && choices[1]->text() == "Start SteamVR",
          "...with WiVRn first, then SteamVR");
    start->click();
    check(w.lastLaunch().size() == 1 && w.lastLaunch()[0].endsWith("/wivrn-server"), "...it runs WiVRn");
    choices[1]->trigger();
    check(w.lastLaunch().size() == 2 && w.lastLaunch()[0].endsWith("/steam") &&
              w.lastLaunch()[1] == "steam://run/250820", "...and the arrow's second choice, SteamVR");
    QFile::remove(QString::fromStdString(bin + "/wivrn-server"));
    w.showChecks(rows);
    check(start->text() == "Start SteamVR" && start->menu()->actions().size() == 1,
          "no WiVRn: SteamVR is what's offered");
    setenv("PATH", "/nonexistent", 1);
    w.showChecks(rows);
    check(!start->isVisibleTo(&w) && fix->isVisibleTo(&w) && fix->text() == "Help",
          "neither: it opens the help");
    setenv("PATH", keep.c_str(), 1);
    QDir(QString::fromStdString(bin)).removeRecursively();
    w.showChecks(good_rows());
}

static void test_help(const std::string &helpDir) {
    const QString dir = QString::fromStdString(helpDir);
    QFile f(dir + "/index.html");
    check(f.open(QIODevice::ReadOnly | QIODevice::Text), "the help page exists");
    const QString html = QString::fromUtf8(f.readAll());
    std::set<QString> anchors;
    auto names = QRegularExpression("<a name=\"([^\"]+)\"").globalMatch(html);
    while (names.hasNext())
        anchors.insert(names.next().captured(1));
    bool links = true;
    auto hrefs = QRegularExpression("href=\"#([^\"]+)\"").globalMatch(html);
    int linkCount = 0;
    while (hrefs.hasNext()) {
        const QString a = hrefs.next().captured(1);
        linkCount++;
        if (!anchors.count(a)) {
            links = false;
            printf("      no section for #%s\n", qPrintable(a));
        }
    }
    check(links && linkCount > 5, "every link in the help has its section");
    bool pics = true;
    int picCount = 0;
    auto srcs = QRegularExpression("<img src=\"([^\"]+)\"").globalMatch(html);
    while (srcs.hasNext()) {
        const QString s = srcs.next().captured(1);
        picCount++;
        if (!QFile::exists(dir + "/" + s)) {
            pics = false;
            printf("      no picture %s\n", qPrintable(s));
        }
    }
    check(pics && picCount >= 3, "every picture in the help exists");
    for (const char *id : {"x4", "patches", "steam", "running", "gamescope", "vr", "mod", "gpu"})
        check(anchors.count(QString("row-") + id) > 0, std::string("the ") + id + " row has a section");
    for (const char *a : {"game", "flatpak", "eye", "headsets", "upscaler", "gamemode", "language", "ipd", "play",
                          "flatscreen", "launch", "install", "uninstall", "more", "wivrn", "troubleshooting",
                          "codec", "input", "report"})
        check(anchors.count(a) > 0, std::string("help has a section on ") + a);
    check(html.contains("can't be undone"), "help says uninstall's settings can't be undone");
    for (const char *h : {"Quest 3", "Quest 3S", "Quest 2", "Valve Index", "Steam Frame", "HTC Vive"})
        check(html.contains(h), std::string("the headset table has the ") + h);
    for (const char *f2 : {"banner.png", "icon.png", "steam-launch-options.png"})
        check(QFile::exists(dir + "/" + f2), std::string("the artwork has ") + f2);
    check(html.contains("We suggest WiVRn"), "the help says WiVRn is what we suggest");
    for (const char *c : {"AV1", "H.265", "H.264", "Steam Input", "Wayland"})
        check(html.contains(c), std::string("troubleshooting covers ") + c);
    check(html.contains("Debug log"), "the help explains the debug run");
    check(!html.contains("Lean in"), "the help doesn't mention the lean checkbox");
    check(!html.contains("<b>?</b>"), "or the ? buttons");

    // Each language's page, help/<code>/index.html: the same sections, since the rows
    // and buttons link to them by name, and pictures found as the help window finds
    // them, in its folder first and then in help/.
    for (const Language &l : kLanguages) {
        if (!strcmp(l.code, "en"))
            continue;
        const std::string code = l.code;
        const QString ldir = dir + "/" + l.code;
        QFile lf(ldir + "/index.html");
        if (!lf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            check(false, code + ": the help has a page");
            continue;
        }
        const QString lhtml = QString::fromUtf8(lf.readAll());
        std::set<QString> lanchors;
        auto ln = QRegularExpression("<a name=\"([^\"]+)\"").globalMatch(lhtml);
        while (ln.hasNext())
            lanchors.insert(ln.next().captured(1));
        check(lanchors == anchors, code + ": the help has the same sections as the English");
        bool llinks = true;
        auto lh = QRegularExpression("href=\"#([^\"]+)\"").globalMatch(lhtml);
        while (lh.hasNext())
            llinks = llinks && lanchors.count(lh.next().captured(1));
        check(llinks, code + ": every link has its section");
        bool lpics = true;
        int lcount = 0;
        auto ls = QRegularExpression("<img src=\"([^\"]+)\"").globalMatch(lhtml);
        while (ls.hasNext()) {
            const QString s = ls.next().captured(1);
            lcount++;
            if (!QFile::exists(ldir + "/" + s) && !QFile::exists(dir + "/" + s)) {
                lpics = false;
                printf("      %s: no picture %s\n", l.code, qPrintable(s));
            }
        }
        check(lpics && lcount == picCount, code + ": the same pictures, and each exists");
        for (const char *h : {"Quest 3", "Quest 3S", "Valve Index", "Steam Frame", "AV1", "H.265", "x4vrmod config"})
            check(lhtml.contains(h), code + ": the help still has " + h);
        check(lhtml.contains(QRegularExpression("charset=\"?utf-8", QRegularExpression::CaseInsensitiveOption)),
              code + ": the help says it's UTF-8");
    }
}

// Every language at the sizes desktops use (Qt's offscreen default is 9 pt; KDE's is
// 10, GNOME's and Ubuntu's 11): no button or checkbox narrower than its words, nothing
// past the right edge, the banner across the window. The window grows past its 800
// pixels for words that need it (Russian at 11 pt: 943 in Noto Sans, 1006 in DejaVu
// Sans, the widest of the common fonts), up to 1100, which a 1366-pixel laptop screen
// still shows whole: a translation that needs more must say it shorter.
static void test_languages_fit(const std::string &home, const Layout &layout) {
    if (QFontDatabase::families().isEmpty()) {
        printf("skip  the languages' widths: this system has no fonts to measure words with\n");
        return;
    }
    const QFont was = QApplication::font();
    QTranslator qt;
    for (const Language &l : kLanguages) {
        const std::string code = l.code;
        if (!gui::apply_language(code, layout.locale_dir, qt)) {
            printf("skip  %s: this system has no locale to show a translation under\n", l.code);
            continue;
        }
        // Without its catalogue every language would fit, being English.
        check(code == "en" || gui::qtr("Play") != "Play", code + ": the window is in it");
        for (const qreal points : {was.pointSizeF(), 10.0, 11.0}) {
            QFont f = was;
            f.setPointSizeF(points);
            QApplication::setFont(f);
            Window::Options o;
            o.live = false;
            Window w(home, o);
            w.show();
            // Every fix button at once, which is the widest the rows get.
            auto rows = good_rows();
            replace(rows, row("patches", Status::Warn, "X4 build not supported yet", "", "details"));
            replace(rows, row("vr", Status::Block, "Headset not connected",
                              "Connect from the WiVRn app in the headset.", "retry"));
            replace(rows, row("mod", Status::Block, "Mod not installed",
                              "The x4vrmod extension isn't in X4's folder.", "install"));
            w.showChecks(rows);
            QApplication::processEvents();
            QStringList cut;
            for (QWidget *c : w.findChildren<QWidget *>()) {
                auto *b = qobject_cast<QAbstractButton *>(c);
                auto *box = qobject_cast<QComboBox *>(c);
                if ((!b && !box) || !c->isVisibleTo(&w))
                    continue;
                const int right = c->mapTo(&w, QPoint(c->width(), 0)).x();
                // A list box shortens what it shows; a button can't.
                if (right > w.width() || (b && c->width() < c->sizeHint().width()))
                    cut << (b ? b->text() : box->currentText());
            }
            auto *banner = w.findChild<QLabel *>("banner");
            const bool across = banner && (banner->pixmap().isNull() ||
                                           qRound(banner->pixmap().deviceIndependentSize().width()) ==
                                               w.width());
            check(cut.isEmpty() && w.width() >= 800 && w.width() <= 1100 && across,
                  code + " at " + QString::number(points).toStdString() + " pt (" +
                      QFontInfo(f).family().toStdString() + "): fits in " + std::to_string(w.width()) +
                      " pixels, the banner across" +
                      (cut.isEmpty() ? std::string() : "; cut: " + cut.join(", ").toStdString()));
        }
        QApplication::setFont(was);
    }
    gui::apply_language("en", layout.locale_dir, qt);
}

// A bug report is for us to read: English with the window in Portuguese. The control is
// the same checks made outside a report, which must come out Portuguese, or the language
// wasn't on and the English proves nothing.
static void test_report_in_english(const std::string &home, const std::string &game,
                                   const Layout &layout) {
    QTranslator qt;
    if (!gui::apply_language("pt_BR", layout.locale_dir, qt)) {
        printf("skip  the report in English: no locale here to show a translation under\n");
        return;
    }
    CheckOptions co;
    co.home = home;
    co.game = game;
    co.skip_vr = co.skip_gpu = true;
    const std::vector<Check> rows = run_checks(co);
    check(!rows.empty() && rows[0].title == "X4 encontrado",
          "the control: outside a report, the checks are in Portuguese (" +
              (rows.empty() ? std::string() : rows[0].title) + ")");
    ReportOptions o;
    o.home = home;
    o.game = game;
    o.note = "As luzes piscam na Manticore.";
    const ReportResult r = make_report(o);
    check(r.ok && r.summary.find("X4 found") != std::string::npos &&
              r.summary.find("encontrado") == std::string::npos,
          "a report made with the window in Portuguese is in English");
    check(r.issue_url.find("Manticore") != std::string::npos, "...and the player's own words go in as written");
    gui::apply_language("en", layout.locale_dir, qt);
}

// The Language list: each language in its own words, the window's own chosen, and a
// choice kept and handed to main.cpp, which builds the window again in it.
static void test_language_box(const std::string &home) {
    std::string asked;
    Window::Options o;
    o.live = false;
    o.languageChanged = [&asked](const std::string &code) { asked = code; };
    Window w(home, o);
    auto *box = w.findChild<QComboBox *>("language");
    QStringList names;
    for (int i = 0; box && i < box->count(); i++)
        names << box->itemText(i);
    check(names == QStringList({"English", "Português Brasileiro", "Français", "Русский", "Deutsch"}),
          "the Language list names each language in itself: " + names.join(", ").toStdString());
    check(box && box->currentData().toString() == QString::fromStdString(gui::current_language()),
          "...with the window's own chosen");
    check(box && box->isVisibleTo(&w) && !box->toolTip().isEmpty(), "...and it explains itself on hover");
    box->setCurrentIndex(box->findData("de"));
    check(asked == "de", "choosing Deutsch asks for the window again in it");
    check(load_profile(profile_path(home)).get("language") == "de", "...and keeps the choice");
    QAction *reset = nullptr;
    if (auto *more = w.findChild<QPushButton *>("more"))
        for (QAction *a : more->menu()->actions())
            if (a->text() == gui::qtr("Reset to defaults..."))
                reset = a;
    check(reset, "More has Reset to defaults");
    if (reset)
        reset->trigger();
    check(load_profile(profile_path(home)).get("language") == "de", "...which keeps the language");
    Profile p = load_profile(profile_path(home));
    p.values.erase("language");
    save_profile(profile_path(home), p);
}

// A real worker thread and real checks, against the fake home.
static void test_live(const std::string &home, const std::string &game) {
    Window::Options o;
    o.live = true;
    Window w(home, o);
    check(QTest::qWaitFor([&w] { return w.checksRan(); }, 20000), "the worker thread's checks arrive");
    auto *x4 = w.findChild<QLabel *>("row-x4");
    check(x4 && x4->isVisibleTo(&w), "...and fill the rows");
    check(w.gameExe() == game, "...for the chosen game");
    auto *vr = w.findChild<QLabel *>("row-vr");
    check(vr && vr->text().contains("No VR runtime"), "the VR row is real: no runtime in a fake home");
    auto *play = w.findChild<QPushButton *>("play");
    check(play && !play->isEnabled(), "...so Play waits");
    // Leaving with a check possibly running must not hang or crash: the next line's
    // destructor is the test.
}

int main(int argc, char **argv) {
    QString shots, helpShot;
    std::string lang;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--shots"))
            shots = argv[i + 1];
        if (!strcmp(argv[i], "--help-shot"))
            helpShot = argv[i + 1];
        if (!strcmp(argv[i], "--lang"))
            lang = argv[i + 1];
    }
    if (!lang.empty() && !is_language(lang)) {
        printf("FAIL  --lang is one of en, pt_BR, fr, ru, de\n");
        return 1;
    }
    setenv("QT_QPA_PLATFORM", "offscreen", 0);
    char tmpl[] = "/tmp/x4vr-gui-XXXXXX";
    if (!mkdtemp(tmpl)) {
        printf("FAIL  no temp directory\n");
        return 1;
    }
    const std::string tmp = tmpl;
    const std::string home = tmp + "/home";
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("X4VR_GAME");
    unsetenv("XR_RUNTIME_JSON");
    setenv("HOME", home.c_str(), 1);
    // The probes run the command line, which sits beside this test's folder.
    const std::string cliDir = dir_name(self_dir()) + "/launcher";
    setenv("PATH", (cliDir + ":" + getenv("PATH")).c_str(), 1);

    // A Steam library in the fake home, with a real ELF as X4.
    const std::string lib = home + "/.local/share/Steam";
    const std::string gameDir = lib + "/steamapps/common/X4 Foundations";
    QDir().mkpath(QString::fromStdString(gameDir));
    QDir().mkpath(QString::fromStdString(home + "/.config"));
    {
        QFile vdf(QString::fromStdString(lib + "/steamapps/libraryfolders.vdf"));
        if (vdf.open(QIODevice::WriteOnly))
            vdf.write(QString("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"%1\"\n\t\t\"apps\"\n"
                              "\t\t{\n\t\t\t\"392160\"\t\t\"1\"\n\t\t}\n\t}\n}\n")
                          .arg(QString::fromStdString(lib))
                          .toUtf8());
        QFile acf(QString::fromStdString(lib + "/steamapps/appmanifest_392160.acf"));
        if (acf.open(QIODevice::WriteOnly))
            acf.write("\"AppState\"\n{\n\t\"installdir\"\t\t\"X4 Foundations\"\n\t\"buildid\"\t\t\"23660954\"\n}\n");
        QFile::copy("/proc/self/exe", QString::fromStdString(gameDir + "/X4"));
        QFile ver(QString::fromStdString(gameDir + "/version.dat"));
        if (ver.open(QIODevice::WriteOnly))
            ver.write("900\n");
    }

    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    const Layout layout = detect_layout(home);
    QTranslator qtLang;
    if (!lang.empty()) {
        if (!gui::apply_language(lang, layout.locale_dir, qtLang)) {
            printf("FAIL  no locale on this system shows %s\n", lang.c_str());
            return 1;
        }
        g_counted = lang == "en";
        if (!g_counted)
            printf("--lang %s: the checks below read English, so they aren't counted\n", lang.c_str());
    }
    {
        Window::Options o;
        o.live = false;
        Window w(home, o);
        check(w.gameExe() == gameDir + "/X4", "Steam's copy is found and chosen");
        auto *game = w.findChild<QComboBox *>("game");
        check(game && game->currentText().startsWith("Steam: ~/"), "the Game box says which, and shortens the home");
        w.show();
        // The help's pictures shouldn't carry a build number.
        if (!shots.isEmpty())
            if (auto *v = w.findChild<QLabel *>("version"))
                v->setText(gui::qtr("%1 - supports X4 %2 - unofficial, not affiliated with Egosoft")
                               .arg("X4 VR Mod", "9.00"));
        test_eye_rule(w, home);
        test_play(w, shots);
        test_suggests_wivrn(w);
        test_browse(w, tmp);
    }
    test_uninstall(shots);
    test_report(home, shots);
    test_launch_options(layout.help_dir);
    test_advanced();
    test_help(layout.help_dir);
    if (lang.empty()) {
        test_languages_fit(home, layout);
        test_language_box(home);
        test_report_in_english(home, gameDir + "/X4", layout);
    }
    test_live(home, gameDir + "/X4");
    if (!helpShot.isEmpty()) {
        gui::HelpWindow h(layout.help_dir, nullptr);
        h.resize(760, 1500);
        h.show("");
        QTest::qWait(200);
        h.grab().save(helpShot + "-top.png");
        h.show("headsets");
        QTest::qWait(200);
        h.grab().save(helpShot + "-headsets.png");
    }

    QDir(QString::fromStdString(tmp)).removeRecursively();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
