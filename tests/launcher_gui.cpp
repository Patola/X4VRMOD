// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The window, with nothing real behind it: a fake home, a fake game and rows fed in
// by hand, on Qt's offscreen platform. The Play button from the rows, the eye-size
// rule through the real lists, Proton, the uninstall dialog's defaults, every help
// link and picture. Then a live window, for the worker thread. Self-checking.
//
//   x4vr_test_launcher_gui              the checks
//   x4vr_test_launcher_gui --shots DIR  also writes the help's pictures
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTest>

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "dialogs.hpp"
#include "window.hpp"

using namespace x4vr::launcher;
using gui::Window;

static int g_fail = 0;
static void check(bool ok, const std::string &what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    if (!ok)
        g_fail++;
}

static Check row(const char *id, Status s, const char *title, const char *detail = "",
                 const char *fix = "") {
    Check c;
    c.id = id;
    c.status = s;
    c.title = title;
    c.detail = detail;
    c.fix = fix;
    return c;
}

// The help shows pictures at this width, smoothly: its own scaling isn't.
static void shot(QWidget &w, const QString &path) {
    w.grab().scaledToWidth(640, Qt::SmoothTransformation).save(path);
}

static std::vector<Check> good_rows() {
    return {row("x4", Status::Ok, "X4 found", "Steam, X4 9.00, build 23660954, /games/X4 Foundations"),
            row("patches", Status::Ok, "The mod fits this X4", "all 8 features match its code"),
            row("steam", Status::Ok, "Steam running"),
            row("running", Status::Ok, "X4 not running yet"),
            row("gamescope", Status::Ok, "gamescope installed", "3.17.0"),
            row("vr", Status::Ok, "VR ready", "WiVRn, Meta Quest 3 on WiVRn, asks 2889 x 3026 per eye"),
            row("layer", Status::Ok, "Vulkan layer installed", "the same version as this launcher"),
            row("mod", Status::Ok, "Mod installed", "x4vrmod extension, up to date"),
            row("gpu", Status::Ok, "GPU capable", "Radeon RX 7900 XTX, Vulkan 1.4")};
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
    check(x4 && x4->toolTip().contains("/games"), "...and has it in its tooltip");
    if (!shots.isEmpty())
        shot(w, shots + "/window-ready.png");

    replace(rows, row("patches", Status::Warn, "X4 build not supported yet",
                      "These turn off: Clicking the cockpit HUD; The shadow fix.", "details"));
    w.showChecks(rows);
    check(play->isEnabled() && play->text() == "Play anyway", "amber: Play anyway");
    check(play->styleSheet().contains("#c67d00"), "...and amber");
    auto *fix = w.findChild<QPushButton *>("fix-patches");
    check(fix && fix->isVisibleTo(&w) && fix->text() == "Details", "the amber row offers Details");
    if (!shots.isEmpty())
        shot(w, shots + "/window-amber.png");

    rows = good_rows();
    replace(rows, row("vr", Status::Block, "Headset not connected",
                      "WiVRn is running but gives no answer, as it does until a headset connects. "
                      "Connect from the WiVRn app in the headset.",
                      "retry"));
    replace(rows, row("mod", Status::Block, "Mod not installed",
                      "The x4vrmod extension isn't in X4's folder.", "install"));
    w.showChecks(rows);
    check(!play->isEnabled() && play->text() == "Play", "red: Play is greyed");
    check(play->styleSheet().isEmpty(), "...in the theme's own colours");
    check(status->text().contains("2 things to fix first"), "...and counts them");
    fix = w.findChild<QPushButton *>("fix-mod");
    check(fix && fix->isVisibleTo(&w) && fix->text().startsWith("Install"), "the mod's row offers Install");
    fix = w.findChild<QPushButton *>("fix-vr");
    check(fix && fix->isVisibleTo(&w) && fix->text() == "Retry", "the VR row offers Retry");
    fix = w.findChild<QPushButton *>("fix-x4");
    check(fix && !fix->isVisibleTo(&w), "a good row offers nothing");
    if (!shots.isEmpty())
        shot(w, shots + "/window-fix.png");

    replace(rows, good_rows()[5]);
    w.showChecks(rows);
    check(status->text().contains("1 thing to fix first") && status->text().contains("Mod not installed"),
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
    w.play(true);
    check(w.lastLaunch() == QStringList({"run", "--debug", "--game", QString::fromStdString(w.gameExe())}),
          "the debug run asks for it");
    w.play();
    check(!w.lastLaunch().contains("--debug"), "...and the next Play is a normal one");
    auto *bug = w.findChild<QPushButton *>("report");
    check(bug && bug->isVisibleTo(&w) && bug->text() == "Report a bug", "Report a bug is in plain sight");
    auto *version = w.findChild<QLabel *>("version");
    check(version && version->text().contains("unofficial"), "the window says it's unofficial");

    // With something to fix, the menu's debug run says so instead of starting anyway.
    auto blocked = good_rows();
    replace(blocked, row("mod", Status::Block, "Mod not installed", "", "install"));
    w.showChecks(blocked);
    w.lastLaunch();
    const QStringList before = w.lastLaunch();
    w.play(true);
    check(w.lastLaunch() == before && w.lastMessage().contains("fix first"), "a debug run waits for the checklist too");
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
    check(renders->text().contains("2816 x 2816"), "the render size is shown");

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
    check(renders->text().contains("4224 x 4224") && !renders->text().contains("chosen"),
          "unselected: full size, and the note goes");
    eye->setCurrentIndex(eye->findData(4000));
    check(up->currentData().toString() == "none", "moving between large sizes respects that");
    check(!renders->text().contains("flat"), "4000 keeps the map in depth");
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

    auto *ipd = w.findChild<QDoubleSpinBox *>("ipd");
    check(ipd && ipd->value() == 64.0, "eye distance starts at the mod's 64 mm");
    ipd->setValue(63.5);
    check(load_profile(profile_path(home)).get("ipd") == "0.0635", "63.5 mm is saved in metres");
    ipd->setValue(64.0);
    check(load_profile(profile_path(home)).get("ipd").empty(), "the default isn't written down");
    auto *lean = w.findChild<QCheckBox *>("lean");
    check(lean && lean->isChecked(), "lean starts on, as the shipping line");
    lean->setChecked(false);
    check(load_profile(profile_path(home)).get("lean") == "0", "off is saved");
    bool leanOff = false;
    for (const auto &[k, v] : profile_env(load_profile(profile_path(home))))
        leanOff = leanOff || (k == "X4VR_HEAD_6DOF" && v == "0");
    check(leanOff, "...and reaches the launch script's variable");
    lean->setChecked(true);
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
    check(!d.wantsDebugRun(), "no debug run unless asked");
    auto *debug = d.findChild<QPushButton *>("debug-run");
    debug->click();
    check(d.wantsDebugRun(), "the button asks for a debug run");
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
    auto rows = good_rows();
    replace(rows, row("vr", Status::Block, "No VR runtime running", "", "start-vr"));
    setenv("PATH", bin.c_str(), 1);
    w.showChecks(rows);
    auto *fix = w.findChild<QPushButton *>("fix-vr");
    check(fix && fix->text() == "Start WiVRn", "the VR row's button starts WiVRn");
    setenv("PATH", "/nonexistent", 1);
    w.showChecks(rows);
    check(fix && fix->text() == "Help", "with no WiVRn it opens the help that recommends it");
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
    for (const char *id : {"x4", "patches", "steam", "running", "gamescope", "vr", "layer", "mod", "gpu"})
        check(anchors.count(QString("row-") + id) > 0, std::string("the ") + id + " row has a section");
    for (const char *a : {"game", "eye", "headsets", "upscaler", "ipd", "lean", "play", "launch",
                          "install", "uninstall", "more", "wivrn", "troubleshooting", "codec", "input",
                          "report"})
        check(anchors.count(a) > 0, std::string("help has a section on ") + a);
    check(html.contains("can't be undone"), "help says uninstall's settings can't be undone");
    for (const char *h : {"Quest 3", "Quest 3S", "Quest 2", "Valve Index", "Steam Frame", "HTC Vive"})
        check(html.contains(h), std::string("the headset table has the ") + h);
    for (const char *f2 : {"banner.png", "icon.png", "steam-launch-options.png"})
        check(QFile::exists(dir + "/" + f2), std::string("the artwork has ") + f2);
    check(html.contains("We suggest WiVRn"), "the help says WiVRn is what we suggest");
    for (const char *c : {"AV1", "H.265", "H.264", "Steam Input", "Wayland"})
        check(html.contains(c), std::string("troubleshooting covers ") + c);
    check(html.contains("Play with debug"), "the help explains the debug run");
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
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--shots"))
            shots = argv[i + 1];
        if (!strcmp(argv[i], "--help-shot"))
            helpShot = argv[i + 1];
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
                v->setText("supports X4 9.00 - unofficial, not affiliated with Egosoft");
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
