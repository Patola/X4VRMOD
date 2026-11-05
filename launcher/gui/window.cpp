// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#include "window.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <signal.h>

#include <algorithm>
#include <cmath>

#include "dialogs.hpp"

namespace x4vr::launcher::gui {

namespace {

constexpr int kWidth = 800;
// The rows' order: what a player fixes first comes first.
const char *const kRowIds[] = {"x4",  "patches", "steam", "running", "gamescope",
                               "vr",  "layer",   "mod",   "gpu"};

QString q(const std::string &s) { return QString::fromStdString(s); }

QPixmap statusIcon(Status s, qreal ratio) {
    const int n = 16;
    QPixmap pm(int(n * ratio), int(n * ratio));
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(s == Status::Ok     ? QColor(0x2e, 0x9e, 0x4f)
               : s == Status::Warn ? QColor(0xd9, 0x8a, 0x00)
                                   : QColor(0xd0, 0x39, 0x2b));
    p.drawEllipse(QRectF(0.5, 0.5, n - 1, n - 1));
    p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (s == Status::Ok) {
        QPainterPath path;
        path.moveTo(4.5, 8.5);
        path.lineTo(7, 11);
        path.lineTo(11.5, 5.5);
        p.drawPath(path);
    } else if (s == Status::Warn) {
        p.drawLine(QPointF(8, 4), QPointF(8, 9));
        p.drawPoint(QPointF(8, 12));
    } else {
        p.drawLine(QPointF(5, 5), QPointF(11, 11));
        p.drawLine(QPointF(11, 5), QPointF(5, 11));
    }
    return pm;
}

QString fixLabel(const std::string &fix) {
    if (fix == "install")
        return "Install...";
    if (fix == "retry")
        return "Retry";
    if (fix == "browse")
        return "Browse...";
    if (fix == "details")
        return "Details";
    if (fix == "start-steam")
        return "Start Steam";
    if (fix == "start-steamvr")
        return "Start SteamVR";
    if (fix == "start-vr")
        return wivrn_program().empty() ? "Help" : "Start WiVRn";
    return "";
}

} // namespace

// ---------------------------------------------------------------- construction
Window::Window(const std::string &home, const Options &options)
    : m_home(home), m_opt(options), m_layout(detect_layout(home)),
      m_profile(load_profile(profile_path(home))), m_eye(eye_settings(m_profile)) {
    setWindowTitle("X4 VR Mod");
    buildUi();
    reloadGames();
    syncSettings();
    updatePlay();
    fitHeight();
    if (m_opt.live) {
        m_timer = new QTimer(this);
        m_timer->setInterval(4000);
        connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
        m_timer->start();
        requestChecks(true);
    }
}

Window::~Window() {
    // Bounded: every child the core starts has a timeout.
    if (m_worker)
        m_worker->wait();
}

QToolButton *Window::helpButton(const QString &anchor) {
    auto *b = new QToolButton(this);
    b->setText("?");
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip("What this is, and why");
    connect(b, &QToolButton::clicked, this, [this, anchor] { openHelp(anchor); });
    return b;
}

QWidget *Window::labelled(const QString &text, const QString &anchor) {
    auto *w = new QWidget(this);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(new QLabel(text, w));
    h->addWidget(helpButton(anchor));
    h->addStretch(1);
    return w;
}

QWidget *Window::buildSettings() {
    auto *w = new QWidget(this);
    w->setFixedWidth(280);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    v->addWidget(labelled("Eye resolution", "eye"));
    m_eyeBox = new QComboBox(w);
    m_eyeBox->setObjectName("eye");
    v->addWidget(m_eyeBox);
    connect(m_eyeBox, &QComboBox::currentIndexChanged, this, [this] { eyeChosen(); });

    v->addWidget(labelled("Upscaler", "upscaler"));
    m_upBox = new QComboBox(w);
    m_upBox->setObjectName("upscaler");
    for (const Upscaler &u : upscalers())
        m_upBox->addItem(u.label, QString(u.mode));
    v->addWidget(m_upBox);
    connect(m_upBox, &QComboBox::currentIndexChanged, this, [this] { upscalerChosen(); });

    m_renders = new QLabel(w);
    m_renders->setObjectName("renders");
    m_renders->setWordWrap(true);
    v->addWidget(m_renders);

    v->addSpacing(4);
    v->addWidget(labelled("Eye distance", "ipd"));
    m_ipd = new QDoubleSpinBox(w);
    m_ipd->setObjectName("ipd");
    m_ipd->setRange(50.0, 80.0);
    m_ipd->setDecimals(1);
    m_ipd->setSingleStep(0.5);
    m_ipd->setSuffix(" mm");
    v->addWidget(m_ipd);
    connect(m_ipd, &QDoubleSpinBox::valueChanged, this, [this](double mm) { ipdChanged(mm); });

    v->addSpacing(4);
    auto *leanRow = new QHBoxLayout;
    m_lean = new QCheckBox("Lean in the cockpit", w);
    m_lean->setObjectName("lean");
    leanRow->addWidget(m_lean);
    leanRow->addWidget(helpButton("lean"));
    leanRow->addStretch(1);
    v->addLayout(leanRow);
    connect(m_lean, &QCheckBox::toggled, this, [this](bool on) { leanToggled(on); });
    v->addStretch(1);
    return w;
}

QWidget *Window::buildChecks() {
    auto *w = new QWidget(this);
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    auto *heading = new QLabel("<b>Ready to play?</b>", w);
    v->addWidget(heading);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);
    grid->setColumnStretch(1, 1);
    int r = 0;
    for (const char *id : kRowIds) {
        Row row;
        row.icon = new QLabel(w);
        row.text = new QLabel(w);
        row.text->setObjectName(QString("row-") + id);
        row.text->setWordWrap(true);
        row.text->setTextFormat(Qt::RichText);
        row.fix = new QPushButton(w);
        row.fix->setObjectName(QString("fix-") + id);
        row.help = helpButton(QString("row-") + id);
        const std::string sid = id;
        connect(row.fix, &QPushButton::clicked, this, [this, sid] {
            const auto it = m_checks.find(sid);
            if (it != m_checks.end())
                runFix(sid, it->second.fix);
        });
        grid->addWidget(row.icon, r, 0, Qt::AlignTop);
        grid->addWidget(row.text, r, 1);
        grid->addWidget(row.fix, r, 2, Qt::AlignTop);
        grid->addWidget(row.help, r, 3, Qt::AlignTop);
        m_rows[sid] = row;
        row.icon->hide();
        row.text->hide();
        row.fix->hide();
        row.help->hide();
        r++;
    }
    v->addLayout(grid);
    v->addStretch(1);
    return w;
}

void Window::buildUi() {
    setFixedWidth(kWidth);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *banner = new QLabel(this);
    QPixmap art(q(m_layout.help_dir) + "/banner.png");
    if (!art.isNull()) {
        // The window's width, and whatever height the picture has at that width.
        const qreal r = devicePixelRatioF();
        QPixmap s = art.scaledToWidth(int(kWidth * r), Qt::SmoothTransformation);
        s.setDevicePixelRatio(r);
        banner->setPixmap(s);
    } else {
        banner->setText("<h1 style='margin:12px'>X4 VR Mod</h1>");
    }
    root->addWidget(banner);

    auto *content = new QWidget(this);
    auto *v = new QVBoxLayout(content);
    v->setContentsMargins(14, 10, 14, 8);
    v->setSpacing(8);

    auto *gameRow = new QHBoxLayout;
    gameRow->addWidget(new QLabel("Game", content));
    gameRow->addWidget(helpButton("game"));
    m_gameBox = new QComboBox(content);
    m_gameBox->setObjectName("game");
    m_gameBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_gameBox->setMinimumContentsLength(30);
    gameRow->addWidget(m_gameBox, 1);
    auto *browseBtn = new QPushButton("Browse...", content);
    browseBtn->setObjectName("browse");
    gameRow->addWidget(browseBtn);
    v->addLayout(gameRow);
    connect(m_gameBox, &QComboBox::currentIndexChanged, this, [this] { gameChosen(); });
    connect(browseBtn, &QPushButton::clicked, this, [this] { browse(); });

    auto *middle = new QHBoxLayout;
    middle->setSpacing(12);
    middle->addWidget(buildSettings());
    auto *line = new QFrame(content);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    middle->addWidget(line);
    middle->addWidget(buildChecks(), 1);
    v->addLayout(middle);

    auto *hline = new QFrame(content);
    hline->setFrameShape(QFrame::HLine);
    hline->setFrameShadow(QFrame::Sunken);
    v->addWidget(hline);

    auto *bottom = new QHBoxLayout;
    auto *helpBtn = new QPushButton("Help", content);
    helpBtn->setObjectName("help");
    connect(helpBtn, &QPushButton::clicked, this, [this] { openHelp(""); });
    bottom->addWidget(helpBtn, 0, Qt::AlignVCenter);
    auto *launch = new QPushButton("Launch options", content);
    launch->setObjectName("launch-options");
    connect(launch, &QPushButton::clicked, this, [this] { launchOptions(); });
    bottom->addWidget(launch, 0, Qt::AlignVCenter);
    auto *bug = new QPushButton("Report a bug", content);
    bug->setObjectName("report");
    connect(bug, &QPushButton::clicked, this, [this] { report(); });
    bottom->addWidget(bug, 0, Qt::AlignVCenter);

    auto *more = new QPushButton("More", content);
    more->setObjectName("more");
    auto *menu = new QMenu(more);
    auto item = [this, menu](const QString &text, auto fn) {
        QAction *a = menu->addAction(text);
        connect(a, &QAction::triggered, this, fn);
        return a;
    };
    item("Play with debug log", [this] { play(true); });
    item("Open logs", [this] { openLogs(); });
    if (m_layout.kind == LayoutKind::Zip)
        m_menuAction = item("Add to application menu", [this] { addToMenu(); });
    item("Advanced...", [this] { advanced(); });
    item("Reset to defaults...", [this] { resetDefaults(); });
    menu->addSeparator();
    item("Uninstall...", [this] { uninstall(); });
    menu->addSeparator();
    item("About", [this] { about(); });
    more->setMenu(menu);
    bottom->addWidget(more, 0, Qt::AlignVCenter);

    m_status = new QLabel(content);
    m_status->setObjectName("status");
    m_status->setWordWrap(true);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bottom->addWidget(m_status, 1);

    m_play = new QPushButton("Play", content);
    m_play->setObjectName("play");
    m_play->setMinimumSize(170, 50);
    QFont big = m_play->font();
    big.setBold(true);
    big.setPointSizeF(big.pointSizeF() * 1.35);
    m_play->setFont(big);
    connect(m_play, &QPushButton::clicked, this, [this] { play(); });
    bottom->addWidget(m_play);
    v->addLayout(bottom);

    auto *version = new QLabel(QString("%1 - supports X4 %2 - unofficial, not affiliated with Egosoft")
                                   .arg(own_version())
                                   .arg(q(format_x4_version(kTestedX4Version))),
                               content);
    version->setObjectName("version");
    QFont small = version->font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    version->setFont(small);
    v->addWidget(version);
    root->addWidget(content);
}

// ----------------------------------------------------------------------- games
QString Window::shortPath(const std::string &path) const {
    const QString p = q(path), h = q(m_home);
    return p.startsWith(h + "/") ? "~" + p.mid(h.size()) : p;
}

void Window::reloadGames() {
    m_syncing = true;
    m_gameBox->clear();
    const std::vector<Install> all = discover_all(m_home);
    for (const Install &in : all) {
        QString text = QString("%1: %2").arg(store_name(in.store), shortPath(in.dir));
        if (in.kind == PathKind::Windows)
            text += " (Windows version)";
        m_gameBox->addItem(text, q(in.exe));
    }
    std::string want = m_game.empty() ? m_profile.get("game") : m_game;
    if (want.empty())
        if (const Install *p = preferred_install(all))
            want = p->exe;
    if (!want.empty()) {
        int at = m_gameBox->findData(q(want));
        if (at < 0) {
            const Install in = install_from_path(want);
            if (in.kind == PathKind::Linux) {
                m_gameBox->addItem(QString("Chosen: %1").arg(shortPath(in.dir)), q(in.exe));
                at = m_gameBox->count() - 1;
            }
        }
        if (at >= 0)
            m_gameBox->setCurrentIndex(at);
    }
    if (m_gameBox->count() == 0)
        m_gameBox->setPlaceholderText("X4 wasn't found: choose it with Browse");
    m_game = m_gameBox->currentData().toString().toStdString();
    m_syncing = false;
}

void Window::gameChosen() {
    if (m_syncing)
        return;
    m_game = m_gameBox->currentData().toString().toStdString();
    // A Windows copy is shown, and its row says why it can't work, but isn't kept.
    if (profile_set(m_profile, "game", m_game).rfind("error: ", 0) != 0)
        saveProfile();
    requestChecks(false);
}

void Window::chooseGame(const QString &path) {
    const GamePath g = classify_game_path(path.toStdString());
    if (g.kind == PathKind::Windows) {
        message("That's the Windows version", proton_explanation(), true);
        return;
    }
    if (g.kind != PathKind::Linux) {
        message("That isn't X4", q(g.why), true);
        return;
    }
    m_game = g.exe;
    profile_set(m_profile, "game", g.exe);
    saveProfile();
    reloadGames();
    requestChecks(false);
}

void Window::browse() {
    const QString start = m_game.empty() ? q(m_home) : q(dir_name(m_game));
    const QString f = QFileDialog::getOpenFileName(this, "Find X4's program", start,
                                                   "X4 (X4 X4.exe);;All files (*)");
    if (!f.isEmpty())
        chooseGame(f);
}

// -------------------------------------------------------------------- settings
void Window::syncSettings() {
    m_syncing = true;
    m_eyeBox->clear();
    // Which offered size is nearest what the headset asks for, when it has said.
    int nearest = 0;
    if (m_headsetAsks)
        for (int e : eye_sizes())
            if (!nearest || std::abs(e - (int)m_headsetAsks) < std::abs(nearest - (int)m_headsetAsks))
                nearest = e;
    const int dflt = EyeSettings{}.eye;
    auto label = [&](int e) {
        QString t = QString("%1 x %1").arg(e);
        if (e == dflt)
            t += " (default)";
        if (e == nearest)
            t += " - nearest your headset";
        return t;
    };
    std::vector<int> sizes = eye_sizes();
    if (std::find(sizes.begin(), sizes.end(), m_eye.eye) == sizes.end()) {
        sizes.push_back(m_eye.eye);
        std::sort(sizes.begin(), sizes.end());
    }
    for (int e : sizes)
        m_eyeBox->addItem(label(e), e);
    m_eyeBox->addItem("Custom...", 0);
    m_eyeBox->setCurrentIndex(m_eyeBox->findData(m_eye.eye));
    m_upBox->setCurrentIndex(m_upBox->findData(q(m_eye.upscaler)));

    const int size = render_size(m_eye.eye, m_eye.upscaler);
    QString text = QString("Renders %1 x %1.").arg(size);
    if (!m_note.isEmpty())
        text += " " + m_note;
    const std::string en = eye_note(m_eye.eye, m_steamvr);
    if (!en.empty())
        text += " " + q(en);
    m_renders->setText(text);

    const std::string ipd = m_profile.get("ipd");
    m_ipd->setValue(ipd.empty() ? 64.0 : atof(ipd.c_str()) * 1000.0);
    m_lean->setChecked(m_profile.get("lean", "1") != "0");
    m_syncing = false;
}

void Window::saveProfile() {
    if (!save_profile(profile_path(m_home), m_profile))
        message("Couldn't save", q("Couldn't write " + profile_path(m_home)), true);
}

void Window::eyeChosen() {
    if (m_syncing)
        return;
    int v = m_eyeBox->currentData().toInt();
    if (v == 0) {
        if (!m_opt.live) {
            syncSettings();
            return;
        }
        bool ok = false;
        v = QInputDialog::getInt(this, "Eye resolution", "Pixels per eye (the eye is square):",
                                 m_eye.eye, 256, 8192, 16, &ok);
        if (!ok) {
            syncSettings();
            return;
        }
    }
    m_note = q(set_eye(m_eye, v));
    store_eye_settings(m_profile, m_eye);
    saveProfile();
    syncSettings();
    fitHeight();
}

void Window::upscalerChosen() {
    if (m_syncing)
        return;
    set_upscaler(m_eye, m_upBox->currentData().toString().toStdString());
    m_note.clear();
    store_eye_settings(m_profile, m_eye);
    saveProfile();
    syncSettings();
    fitHeight();
}

void Window::ipdChanged(double mm) {
    if (m_syncing)
        return;
    if (std::abs(mm - 64.0) < 0.05)
        m_profile.values.erase("ipd");
    else
        profile_set(m_profile, "ipd", QString::number(mm / 1000.0, 'f', 4).toStdString());
    saveProfile();
}

void Window::leanToggled(bool on) {
    if (m_syncing)
        return;
    if (on)
        m_profile.values.erase("lean");
    else
        profile_set(m_profile, "lean", "0");
    saveProfile();
}

// ---------------------------------------------------------------------- checks
void Window::requestChecks(bool forceVr) {
    if (!m_opt.live)
        return;
    if (m_worker) {
        m_again = true;
        m_againForce = m_againForce || forceVr;
        return;
    }
    CheckOptions o;
    o.home = m_home;
    o.game = m_game;
    o.skip_gpu = m_checks.count("gpu") > 0;
    const auto vr = m_checks.find("vr");
    const bool haveVr = vr != m_checks.end();
    const bool vrOk = haveVr && vr->second.status == Status::Ok;
    const auto now = std::chrono::steady_clock::now();
    const bool vrDue = now - m_lastVr >= std::chrono::seconds(vrOk ? 30 : 10);
    const std::string lastRuntime = m_runtimePath;
    const bool lastSteamvr = m_steamvrUp;

    m_worker = QThread::create([this, o, forceVr, haveVr, vrDue, lastRuntime, lastSteamvr]() mutable {
        // Asking a runtime is what's slow and what could bother it, so only when
        // something changed or it's due. Reading its file costs nothing.
        const ActiveRuntime rt = active_runtime(o.home);
        const bool up = rt.steamvr && steamvr_is_running();
        const bool ask = forceVr || !haveVr || vrDue || rt.path.empty() ||
                         rt.path != lastRuntime || up != lastSteamvr;
        XrProbe xr;
        o.skip_vr = !ask;
        o.xr_out = &xr;
        const std::vector<Check> rows = run_checks(o);
        const bool skipGpu = o.skip_gpu;
        QMetaObject::invokeMethod(
            this,
            [this, rows, xr, ask, skipGpu, path = rt.path, steamvr = rt.steamvr, up] {
                m_runtimePath = path;
                m_steamvrUp = up;
                if (ask)
                    m_lastVr = std::chrono::steady_clock::now();
                const bool steamvrChanged = steamvr != m_steamvr;
                m_steamvr = steamvr;
                const unsigned asks = xr.status == XrStatus::Ready ? xr.eye_h : m_headsetAsks;
                const bool asksChanged = asks != m_headsetAsks;
                m_headsetAsks = asks;
                m_checksDone++;
                applyChecks(rows, !ask, skipGpu);
                if (steamvrChanged || asksChanged) {
                    syncSettings();
                    fitHeight();
                }
                if (m_again) {
                    const bool f = m_againForce;
                    m_again = m_againForce = false;
                    QTimer::singleShot(0, this, [this, f] { requestChecks(f); });
                }
            },
            Qt::QueuedConnection);
    });
    connect(m_worker.data(), &QThread::finished, m_worker.data(), &QObject::deleteLater);
    m_worker->start();
}

void Window::showChecks(const std::vector<Check> &rows) { applyChecks(rows, false, false); }

void Window::applyChecks(const std::vector<Check> &rows, bool keepVr, bool keepGpu) {
    std::map<std::string, Check> next;
    for (const Check &c : rows)
        next[c.id] = c;
    if (keepVr && m_checks.count("vr"))
        next["vr"] = m_checks["vr"];
    if (keepGpu && m_checks.count("gpu"))
        next["gpu"] = m_checks["gpu"];
    m_checks = std::move(next);
    for (const char *id : kRowIds)
        updateRow(id);
    updatePlay();
    fitHeight();
}

void Window::updateRow(const std::string &id) {
    Row &row = m_rows[id];
    const auto it = m_checks.find(id);
    const bool shown = it != m_checks.end();
    row.icon->setVisible(shown);
    row.text->setVisible(shown);
    row.help->setVisible(shown);
    if (!shown) {
        row.fix->hide();
        return;
    }
    const Check &c = it->second;
    row.icon->setPixmap(statusIcon(c.status, devicePixelRatioF()));
    row.icon->setToolTip(c.status == Status::Ok     ? "Good"
                         : c.status == Status::Warn ? "It can start, with something off"
                                                    : "This has to be fixed first");
    QString detail = q(c.detail);
    // The Game box already shows the folder.
    if (id == "x4" && c.status == Status::Ok) {
        const int at = detail.indexOf(", /");
        if (at > 0)
            detail.truncate(at);
    }
    QString html = "<b>" + q(c.title).toHtmlEscaped() + "</b>";
    if (!detail.isEmpty() && c.status == Status::Ok) {
        // A good row's detail recedes, by mixing the theme's own colours so it holds in
        // light and dark. A row that needs fixing is meant to be read, so it doesn't.
        const QColor t = palette().color(QPalette::WindowText), b = palette().color(QPalette::Window);
        const QColor dim((t.red() * 2 + b.red()) / 3, (t.green() * 2 + b.green()) / 3,
                         (t.blue() * 2 + b.blue()) / 3);
        html += QString("&nbsp; <span style='color:%1'>%2</span>")
                    .arg(dim.name(), detail.toHtmlEscaped());
    } else if (!detail.isEmpty()) {
        html += "<br>" + detail.toHtmlEscaped();
    }
    row.text->setText(html);
    row.text->setToolTip(q(c.detail));
    const QString label = fixLabel(c.fix);
    row.fix->setText(label);
    row.fix->setVisible(!label.isEmpty());
}

void Window::updatePlay() {
    int blockers = 0;
    std::string first;
    std::vector<Check> rows;
    for (const char *id : kRowIds) {
        const auto it = m_checks.find(id);
        if (it == m_checks.end())
            continue;
        rows.push_back(it->second);
        if (it->second.status == Status::Block) {
            if (!blockers)
                first = it->second.title;
            blockers++;
        }
    }
    const Status w = worst(rows);
    QString text = "Play", colour, status;
    bool enabled = false;
    if (m_gamePid > 0) {
        text = "X4 is running";
    } else if (rows.empty()) {
        status = "Checking...";
    } else if (w == Status::Block) {
        status = blockers == 1 ? QString("1 thing to fix first: %1").arg(q(first))
                               : QString("%1 things to fix first").arg(blockers);
    } else if (w == Status::Warn) {
        text = "Play anyway";
        colour = "#c67d00";
        enabled = true;
        status = "It can start, with something off. See the amber line.";
    } else {
        colour = "#2e9e4f";
        enabled = true;
    }
    if (status.isEmpty())
        status = m_lastSession;
    else if (!m_lastSession.isEmpty() && m_gamePid == 0 && w != Status::Block)
        status += " " + m_lastSession;
    m_status->setText(status);
    m_play->setText(text);
    m_play->setEnabled(enabled);
    m_play->setStyleSheet(colour.isEmpty()
                              ? QString()
                              : QString("QPushButton{background:%1;color:white;border:none;"
                                        "border-radius:6px;padding:6px 18px}"
                                        "QPushButton:hover{background:%2}")
                                    .arg(colour, QColor(colour).lighter(112).name()));
}

void Window::fitHeight() {
    QLayout *l = layout();
    l->activate();
    setFixedHeight(l->hasHeightForWidth() ? l->totalHeightForWidth(kWidth)
                                          : l->totalSizeHint().height());
}

bool Window::event(QEvent *e) {
    if (e->type() == QEvent::WindowActivate && m_opt.live && m_gamePid == 0)
        requestChecks(false);
    return QWidget::event(e);
}

void Window::tick() {
    if (m_gamePid > 0) {
        // Only this while the game runs: VR frames matter more than this window.
        if (kill((pid_t)m_gamePid, 0) != 0)
            sessionEnded();
        return;
    }
    if (!isActiveWindow() && ++m_ticks % 3)
        return;
    requestChecks(false);
    if (m_menuAction)
        m_menuAction->setEnabled(menu_entry_possible(m_home));
}

// ------------------------------------------------------------------------- play
void Window::play(bool debug) {
    // The debug run comes from a menu, which doesn't grey out with the button.
    if (!m_play->isEnabled()) {
        message("Not yet", "The checklist has something to fix first.", true);
        return;
    }
    m_lastLaunch = {"run"};
    if (debug)
        m_lastLaunch << "--debug";
    m_lastLaunch << "--game" << q(m_game);
    if (!m_opt.live)
        return;
    const QString dir = q(log_dir(m_home));
    QDir().mkpath(dir);
    m_outputFile = dir + "/launch-output.txt";
    QProcess p;
    p.setProgram(q(cli_path()));
    p.setArguments(m_lastLaunch);
    p.setStandardOutputFile(m_outputFile);
    p.setStandardErrorFile(m_outputFile, QIODevice::Append);
    qint64 pid = 0;
    if (!p.startDetached(&pid)) {
        message("Couldn't start", "Couldn't run " + q(cli_path()), true);
        return;
    }
    m_gamePid = pid;
    m_gameDebug = debug;
    m_gameStart = std::chrono::steady_clock::now();
    m_lastSession.clear();
    m_timer->setInterval(2000);
    updatePlay();
}

void Window::sessionEnded() {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - m_gameStart)
                          .count();
    m_gamePid = 0;
    m_timer->setInterval(4000);
    if (secs < 20) {
        // Whatever it said last is the best explanation there is.
        QString last;
        QFile f(m_outputFile);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            for (const QString &line : QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts))
                last = line;
        m_lastSession = "X4 stopped right after starting. " + last.left(160);
    } else {
        const auto mins = std::max<long long>(1, secs / 60);
        m_lastSession = QString("Last session: %1 min.").arg(mins);
        if (m_gameDebug)
            m_lastSession += " Report a bug can send its debug log.";
    }
    updatePlay();
    requestChecks(true);
}

// ------------------------------------------------------------------------ fixes
void Window::runFix(const std::string &id, const std::string &fix) {
    if (fix == "install") {
        install();
    } else if (fix == "retry") {
        requestChecks(true);
    } else if (fix == "browse") {
        browse();
    } else if (fix == "details") {
        showDetails(id);
    } else if (fix == "start-steam") {
        QProcess::startDetached("steam", {});
    } else if (fix == "start-steamvr") {
        QProcess::startDetached("steam", {"steam://run/250820"});
    } else if (fix == "start-vr") {
        const std::string wivrn = wivrn_program();
        if (!wivrn.empty())
            QProcess::startDetached(q(wivrn), {});
        else
            openHelp("wivrn");
    }
}

void Window::showDetails(const std::string &id) {
    const auto it = m_checks.find(id);
    if (it == m_checks.end())
        return;
    QString text = q(it->second.detail);
    text.replace("; ", ";\n");
    message(q(it->second.title), text + "\n\nThe game itself still runs. Help says more.", false);
}

void Window::install() {
    bool found = false;
    const Install in = choose_install(m_home, m_game, &found);
    const QString what = m_layout.kind == LayoutKind::Zip
                             ? "This copies the mod to your home folder, registers its Vulkan "
                               "layer, and puts its extension in X4's folder:"
                             : "This puts the mod's extension in X4's folder:";
    if (!confirm("Install the mod",
                 QString("%1\n%2\n\n%3").arg(what, q(in.dir), install_disclosure()), "Install"))
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ActionReport r = install_mod(in, m_home);
    QApplication::restoreOverrideCursor();
    if (!r.ok)
        message("Install failed", q(r.error), true);
    requestChecks(false);
}

void Window::uninstall() {
    const UninstallPlan plan = uninstall_plan(m_home, true);
    UninstallDialog d(plan, this);
    if (d.exec() != QDialog::Accepted)
        return;
    const UninstallPlan chosen = uninstall_plan(m_home, d.alsoSettings());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ActionReport r = uninstall_apply(chosen);
    QApplication::restoreOverrideCursor();
    if (!r.ok)
        message("Uninstall failed", q(r.error), true);
    if (d.alsoSettings()) {
        m_profile = load_profile(profile_path(m_home));
        m_eye = eye_settings(m_profile);
        m_note.clear();
        syncSettings();
    }
    requestChecks(false);
}

void Window::launchOptions() {
    LaunchOptionsDialog d(q(steam_launch_line(m_home)), q(terminal_line(m_home)), m_layout.help_dir,
                          this);
    d.exec();
}

void Window::advanced() {
    AdvancedDialog d(m_profile, this);
    while (d.exec() == QDialog::Accepted) {
        const QString err = d.apply(m_profile);
        if (err.isEmpty()) {
            saveProfile();
            return;
        }
        message("Advanced", err, true);
    }
}

void Window::resetDefaults() {
    if (!confirm("Reset to defaults",
                 "Back to the default eye resolution, upscaler, eye distance, lean and advanced "
                 "settings? The game you chose stays.",
                 "Reset"))
        return;
    const std::string game = m_profile.get("game");
    m_profile.values.clear();
    if (!game.empty())
        m_profile.values["game"] = game;
    m_eye = eye_settings(m_profile);
    m_note.clear();
    saveProfile();
    syncSettings();
    fitHeight();
}

void Window::about() {
    message("About X4 VR Mod",
            QString("X4 VR Mod %1\nSupports X4: Foundations %2 on native Linux.\n\n"
                    "An unofficial mod, not affiliated with or endorsed by Egosoft. X4: "
                    "Foundations is Egosoft's.\n\n"
                    "GPL-3.0-or-later with a linking exception.\n"
                    "https://github.com/Patola/X4VRMOD")
                .arg(own_version(), q(format_x4_version(kTestedX4Version))),
            false);
}

void Window::addToMenu() {
    const ActionReport r = add_menu_entry(m_home);
    message("Application menu", r.ok ? "X4 VR Mod is in your application menu." : q(r.error), !r.ok);
}

void Window::openLogs() {
    const QString dir = q(log_dir(m_home));
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void Window::report() {
    ReportDialog d(m_home, m_game, this);
    d.exec();
    if (d.wantsDebugRun())
        play(true);
}

void Window::openHelp(const QString &anchor) {
    if (!m_help)
        m_help = new HelpWindow(m_layout.help_dir, this);
    m_help->show(anchor);
}

// ---------------------------------------------------------------------- dialogs
void Window::message(const QString &title, const QString &text, bool warning) {
    m_lastMessage = text;
    if (!m_opt.live)
        return;
    if (warning)
        QMessageBox::warning(this, title, text);
    else
        QMessageBox::information(this, title, text);
}

bool Window::confirm(const QString &title, const QString &text, const QString &yes) {
    m_lastMessage = text;
    if (!m_opt.live)
        return true;
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(yes, QMessageBox::AcceptRole);
    box.setDefaultButton(go);
    box.exec();
    return box.clickedButton() == go;
}

} // namespace x4vr::launcher::gui
