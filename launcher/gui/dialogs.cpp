// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#include "dialogs.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace x4vr::launcher::gui {

static QString q(const std::string &s) { return QString::fromStdString(s); }

// ---------------------------------------------------------------------- help
HelpWindow::HelpWindow(const std::string &helpDir, QWidget *parent)
    : QDialog(parent), m_view(new QTextBrowser(this)) {
    setWindowTitle("X4 VR Mod help");
    resize(760, 640);
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    box->addWidget(m_view);
    m_view->setOpenExternalLinks(true);
    m_view->setSearchPaths({q(helpDir)});
    if (QFile::exists(q(helpDir) + "/index.html")) {
        m_view->setSource(QUrl("index.html"));
        m_loaded = true;
    } else {
        m_view->setPlainText("Help isn't installed with this copy of the mod.");
    }
}

void HelpWindow::show(const QString &anchor) {
    QDialog::show();
    raise();
    activateWindow();
    if (m_loaded && !anchor.isEmpty())
        m_view->scrollToAnchor(anchor);
}

// ----------------------------------------------------------------- uninstall
static QString listed(const std::vector<std::string> &paths) {
    QStringList l;
    for (const std::string &p : paths)
        l << q(p);
    return l.join('\n');
}

static QPlainTextEdit *pathList(const QString &text, QWidget *parent) {
    auto *e = new QPlainTextEdit(text, parent);
    e->setReadOnly(true);
    e->setLineWrapMode(QPlainTextEdit::NoWrap);
    e->setFixedHeight(96);
    return e;
}

UninstallDialog::UninstallDialog(const UninstallPlan &plan, QWidget *parent)
    : QDialog(parent), m_also(new QCheckBox("Also delete my VR settings", this)) {
    setWindowTitle("Uninstall the mod");
    setMinimumWidth(620);
    auto *box = new QVBoxLayout(this);
    box->addWidget(new QLabel("This removes:", this));
    auto *mod = pathList(plan.mod.empty() ? "Nothing of the mod's is installed." : listed(plan.mod),
                         this);
    mod->setObjectName("mod-list");
    box->addWidget(mod);
    if (!plan.note.empty())
        box->addWidget(new QLabel(q(plan.note), this));
    auto *safe = new QLabel("X4's saves and its own settings are never touched.", this);
    box->addWidget(safe);

    box->addSpacing(8);
    m_also->setObjectName("also-settings");
    m_also->setChecked(false);
    box->addWidget(m_also);
    auto *warn = new QLabel("<b>This can't be undone.</b> Your resolution and upscaler choices, "
                            "and what you changed in X4 while in VR, are deleted:",
                            this);
    warn->setWordWrap(true);
    warn->setObjectName("settings-warning");
    auto *settings = pathList(plan.settings.empty() ? "No VR settings were found."
                                                    : listed(plan.settings),
                              this);
    settings->setObjectName("settings-list");
    box->addWidget(warn);
    box->addWidget(settings);
    warn->hide();
    settings->hide();
    connect(m_also, &QCheckBox::toggled, this, [this, warn, settings](bool on) {
        warn->setVisible(on);
        settings->setVisible(on);
        adjustSize();
    });

    auto *buttons = new QDialogButtonBox(this);
    auto *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    auto *go = buttons->addButton("Uninstall", QDialogButtonBox::DestructiveRole);
    go->setObjectName("uninstall");
    go->setAutoDefault(false);
    cancel->setDefault(true);
    connect(go, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    box->addWidget(buttons);
}

bool UninstallDialog::alsoSettings() const { return m_also->isChecked(); }

// ------------------------------------------------------------- launch options
static QWidget *copyLine(const QString &line, const QString &name, QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto *edit = new QLineEdit(line, row);
    edit->setObjectName(name);
    edit->setReadOnly(true);
    auto *copy = new QPushButton("Copy", row);
    QObject::connect(copy, &QPushButton::clicked, row, [edit, copy] {
        QApplication::clipboard()->setText(edit->text());
        copy->setText("Copied");
    });
    h->addWidget(edit, 1);
    h->addWidget(copy);
    return row;
}

LaunchOptionsDialog::LaunchOptionsDialog(const QString &steamLine, const QString &terminalLine,
                                         const std::string &helpDir, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle("Launch options");
    setMinimumWidth(680);
    auto *box = new QVBoxLayout(this);
    auto *why = new QLabel("The Play button is one way to start the game. These are the others. "
                           "All of them run the mod the same way, with your settings from this "
                           "window.",
                           this);
    why->setWordWrap(true);
    box->addWidget(why);
    box->addSpacing(6);
    box->addWidget(new QLabel("<b>From Steam:</b> X4's launch options", this));
    box->addWidget(copyLine(steamLine, "steam-line", this));
    QPixmap pic(q(helpDir) + "/steam-launch-options.png");
    if (!pic.isNull()) {
        auto *l = new QLabel(this);
        const qreal r = devicePixelRatioF();
        QPixmap s = pic.scaledToWidth(int(640 * r), Qt::SmoothTransformation);
        s.setDevicePixelRatio(r);
        l->setPixmap(s);
        box->addWidget(l);
    }
    auto *clear = new QLabel("With that line set, Steam always starts X4 in VR. Clear it to play "
                             "flat again.",
                             this);
    clear->setWordWrap(true);
    box->addWidget(clear);
    box->addSpacing(6);
    box->addWidget(new QLabel("<b>From a terminal:</b>", this));
    box->addWidget(copyLine(terminalLine, "terminal-line", this));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    box->addWidget(buttons);
}

// ----------------------------------------------------------------- bug reports
ReportDialog::ReportDialog(const std::string &home, const std::string &game, QWidget *parent)
    : QDialog(parent), m_home(home), m_game(game), m_session(new QComboBox(this)),
      m_note(new QPlainTextEdit(this)), m_create(new QPushButton("Create report", this)),
      m_files(new QPushButton("Show the files", this)),
      m_issue(new QPushButton("Open a GitHub issue", this)), m_outcome(new QLabel(this)) {
    setWindowTitle("Report a bug");
    setMinimumWidth(640);
    auto *box = new QVBoxLayout(this);
    auto *why = new QLabel("This makes a report you can read through, then attach to an issue on "
                           "GitHub. <b>Nothing is sent by itself.</b> Your home folder, user name, "
                           "computer name and Steam account number are taken out of it.",
                           this);
    why->setWordWrap(true);
    box->addWidget(why);
    box->addSpacing(6);

    box->addWidget(new QLabel("<b>Which session?</b>", this));
    m_session->setObjectName("session");
    for (const SessionLog &l : session_logs(m_home)) {
        const QString when = QDateTime::fromSecsSinceEpoch(l.when).toString("yyyy-MM-dd hh:mm");
        const double mb = double(l.size) / (1024.0 * 1024.0);
        m_session->addItem(QString("%1%2, %3 MB")
                               .arg(when, l.debug ? ", debug log" : "")
                               .arg(mb, 0, 'f', 1),
                           q(l.path));
    }
    if (m_session->count() == 0)
        m_session->addItem("No session log yet: X4 hasn't got that far", QString());
    box->addWidget(m_session);

    box->addWidget(new QLabel("<b>What happened?</b>", this));
    m_note->setObjectName("note");
    m_note->setPlaceholderText("What you saw, and what you expected instead.");
    m_note->setFixedHeight(90);
    box->addWidget(m_note);

    auto *hint = new QLabel("Can you make it happen again? Tick <b>Debug log</b> beside Play, make "
                            "it happen, then report that session. It logs much more, which often "
                            "makes the difference.",
                            this);
    hint->setObjectName("debug-hint");
    hint->setWordWrap(true);
    box->addWidget(hint);

    m_outcome->setObjectName("outcome");
    m_outcome->setWordWrap(true);
    m_outcome->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_outcome->hide();
    box->addWidget(m_outcome);

    m_create->setObjectName("create");
    m_create->setDefault(true);
    m_files->setObjectName("files");
    m_files->setEnabled(false);
    m_issue->setObjectName("issue");
    m_issue->setEnabled(false);
    connect(m_create, &QPushButton::clicked, this, [this] { create(); });
    connect(m_files, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(q(dir_name(m_result.dir))));
    });
    connect(m_issue, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromEncoded(QByteArray::fromStdString(m_result.issue_url)));
    });
    auto *buttons = new QDialogButtonBox(this);
    buttons->addButton(m_create, QDialogButtonBox::ActionRole);
    buttons->addButton(m_files, QDialogButtonBox::ActionRole);
    buttons->addButton(m_issue, QDialogButtonBox::ActionRole);
    auto *close = buttons->addButton(QDialogButtonBox::Close);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    box->addWidget(buttons);
}

ReportDialog::~ReportDialog() {
    if (m_worker)
        m_worker->wait();
}

void ReportDialog::create() {
    if (m_worker)
        return;
    ReportOptions o;
    o.home = m_home;
    o.game = m_game;
    o.log = m_session->currentData().toString().toStdString();
    o.note = m_note->toPlainText().toStdString();
    m_create->setEnabled(false);
    m_outcome->setText("Making the report...");
    m_outcome->show();
    m_worker = QThread::create([this, o] {
        const ReportResult r = make_report(o);
        QMetaObject::invokeMethod(
            this,
            [this, r] {
                m_result = r;
                m_hasResult = true;
                m_create->setEnabled(true);
                if (!r.ok) {
                    m_outcome->setText(q(r.error));
                    return;
                }
                QString text = QString("<b>Saved:</b> %1<br>Read it first if you like, with "
                                       "<b>Show the files</b>. Then <b>Open a GitHub issue</b> and "
                                       "drag that file into the page. A short video helps with "
                                       "anything visual.")
                                   .arg(q(r.archive).toHtmlEscaped());
                if (r.too_big)
                    text += "<br><b>It's over GitHub's 25 MB limit for an attachment.</b> Share it "
                            "another way and put the link in the issue.";
                m_outcome->setText(text);
                m_files->setEnabled(true);
                m_issue->setEnabled(true);
                m_issue->setDefault(true);
            },
            Qt::QueuedConnection);
    });
    connect(m_worker.data(), &QThread::finished, m_worker.data(), &QObject::deleteLater);
    m_worker->start();
}

// ------------------------------------------------------------------- advanced
AdvancedDialog::AdvancedDialog(const Profile &profile, QWidget *parent)
    : QDialog(parent), m_fov(new QDoubleSpinBox(this)), m_fovSet(new QCheckBox("Field of view", this)),
      m_env(new QPlainTextEdit(this)) {
    setWindowTitle("Advanced");
    setMinimumWidth(520);
    auto *box = new QVBoxLayout(this);
    auto *fovRow = new QHBoxLayout;
    m_fov->setRange(0.5, 2.5);
    m_fov->setDecimals(4);
    m_fov->setSingleStep(0.01);
    m_fov->setSuffix(" radians");
    m_fov->setObjectName("fov");
    const std::string f = profile.get("fov");
    m_fovSet->setChecked(!f.empty());
    m_fov->setValue(f.empty() ? 1.4917 : atof(f.c_str()));
    m_fov->setEnabled(!f.empty());
    connect(m_fovSet, &QCheckBox::toggled, m_fov, &QWidget::setEnabled);
    fovRow->addWidget(m_fovSet);
    fovRow->addWidget(m_fov, 1);
    box->addLayout(fovRow);
    auto *fovWhy = new QLabel("Leave it off unless you know you need it: the mod's default suits "
                              "the headsets it was tried with.",
                              this);
    fovWhy->setWordWrap(true);
    box->addWidget(fovWhy);
    box->addSpacing(8);
    auto *envWhy = new QLabel("Extra environment variables, one <tt>NAME=value</tt> per line. "
                              "They go to the game and the mod, for trying things a bug report "
                              "asks for.",
                              this);
    envWhy->setWordWrap(true);
    box->addWidget(envWhy);
    QStringList lines;
    for (const auto &[k, v] : profile.values)
        if (k.rfind("env.", 0) == 0)
            lines << q(k.substr(4)) + "=" + q(v);
    m_env->setPlainText(lines.join('\n'));
    m_env->setObjectName("env");
    box->addWidget(m_env);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    box->addWidget(buttons);
}

QString AdvancedDialog::apply(Profile &p) const {
    Profile next = p;
    next.values.erase("fov");
    for (auto it = next.values.begin(); it != next.values.end();)
        it = it->first.rfind("env.", 0) == 0 ? next.values.erase(it) : std::next(it);
    if (m_fovSet->isChecked()) {
        const std::string e = profile_set(next, "fov", QString::number(m_fov->value(), 'f', 4).toStdString());
        if (!e.empty())
            return q(e);
    }
    for (const QString &raw : m_env->toPlainText().split('\n', Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        const int eq = line.indexOf('=');
        if (eq <= 0)
            return "Not NAME=value: " + line;
        const std::string e = profile_set(next, "env." + line.left(eq).toStdString(),
                                          line.mid(eq + 1).toStdString());
        if (e.rfind("error: ", 0) == 0)
            return q(e.substr(7));
    }
    p = next;
    return "";
}

} // namespace x4vr::launcher::gui
