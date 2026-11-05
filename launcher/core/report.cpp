// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A bug report the player can read before it goes anywhere (docs/packaging-plan.md,
// "The report bundle"): plain text in a folder, with an archive beside it to attach
// to a GitHub issue. Nothing here sends anything.
#include <pwd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "x4vr_launcher.hpp"

namespace fs = std::filesystem;

namespace x4vr::launcher {

// GitHub refuses attachments above this, and URLs past about 8 KB.
static constexpr uint64_t kAttachLimit = 25ull * 1024 * 1024;
static constexpr size_t kUrlField = 3000;
// What's kept of a long log: how the session started, and how it ended.
static constexpr size_t kLogHead = 20u * 1024 * 1024, kLogTail = 100u * 1024 * 1024;

static const char kIssues[] = "https://github.com/Patola/X4VRMOD/issues/new";

// ------------------------------------------------------------------ scrubbing
static bool word_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

// `from` where it stands alone: a user "pat" doesn't touch "pattern" or "/home/pat2".
static std::string replace_word(const std::string &s, const std::string &from,
                                const std::string &to) {
    if (from.empty())
        return s;
    std::string out;
    out.reserve(s.size());
    size_t at = 0;
    for (;;) {
        const size_t hit = s.find(from, at);
        if (hit == std::string::npos) {
            out.append(s, at, std::string::npos);
            return out;
        }
        const size_t end = hit + from.size();
        const bool left = hit == 0 || !word_char(s[hit - 1]) || from[0] == '/';
        const bool right = end == s.size() || !word_char(s[end]);
        out.append(s, at, hit - at);
        out.append(left && right ? to : from);
        at = end;
    }
}

std::string scrub_text(const std::string &text, const ScrubNames &n) {
    // The home path first, because the user name is inside it.
    std::string s = replace_word(text, n.home, "~");
    if (n.user.size() >= 3)
        s = replace_word(s, n.user, "<user>");
    if (n.host.size() >= 3)
        s = replace_word(s, n.host, "<host>");
    for (const std::string &id : n.profile_ids)
        s = replace_word(s, id, "<profile>");
    // Any other number after EgoSoft/X4/ is a Steam account too, as is a SteamID64.
    static const std::string kProfile = "/EgoSoft/X4/";
    std::string out;
    out.reserve(s.size());
    size_t at = 0;
    for (;;) {
        const size_t hit = s.find(kProfile, at);
        if (hit == std::string::npos) {
            out.append(s, at, std::string::npos);
            break;
        }
        size_t d = hit + kProfile.size();
        size_t e = d;
        while (e < s.size() && isdigit((unsigned char)s[e]))
            e++;
        out.append(s, at, d - at);
        if (e > d)
            out += "<profile>";
        at = e;
    }
    size_t p = 0;
    while ((p = out.find("7656119", p)) != std::string::npos) {
        size_t e = p;
        while (e < out.size() && isdigit((unsigned char)out[e]))
            e++;
        if (e - p == 17 && (p == 0 || !word_char(out[p - 1])))
            out.replace(p, 17, "<steamid>");
        else
            p = e;
    }
    return out;
}

ScrubNames scrub_names(const std::string &home) {
    ScrubNames n;
    n.home = home;
    if (const passwd *pw = getpwuid(getuid()))
        n.user = pw->pw_name;
    if (const char *u = getenv("USER"); n.user.empty() && u)
        n.user = u;
    char host[256] = "";
    if (gethostname(host, sizeof host - 1) == 0)
        n.host = host;
    for (const std::string &d : x4_profile_dirs(home)) {
        const std::string b = base_name(d);
        if (!b.empty() && std::all_of(b.begin(), b.end(),
                                      [](char c) { return isdigit((unsigned char)c); }))
            n.profile_ids.push_back(b);
    }
    return n;
}

// -------------------------------------------------------------------- sessions
std::vector<SessionLog> session_logs(const std::string &home) {
    std::vector<SessionLog> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(log_dir(home), ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("x4vrmod-", 0) != 0 || e.path().extension() != ".log")
            continue;
        struct stat st;
        if (stat(e.path().c_str(), &st) != 0)
            continue;
        SessionLog l;
        l.path = e.path().string();
        l.when = st.st_mtime;
        l.size = (uint64_t)st.st_size;
        l.debug = n.find("-debug.log") != std::string::npos;
        out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const SessionLog &a, const SessionLog &b) {
        return a.when != b.when ? a.when > b.when : a.path > b.path;
    });
    return out;
}

std::string log_excerpt(const std::string &path, size_t head, size_t tail) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    f.seekg(0, std::ios::end);
    const uint64_t size = (uint64_t)f.tellg();
    f.seekg(0);
    auto read = [&f](uint64_t n) {
        std::string s((size_t)n, '\0');
        f.read(&s[0], (std::streamsize)n);
        s.resize((size_t)f.gcount());
        return s;
    };
    if (size <= head + tail)
        return read(size);
    std::string out = read(head);
    out += "\n[... " + std::to_string((size - head - tail) / (1024 * 1024)) +
           " MB of this log left out ...]\n";
    f.seekg((std::streamoff)(size - tail));
    out += read(tail);
    return out;
}

// ------------------------------------------------------------------ the report
std::string checks_text(const std::vector<Check> &checks) {
    std::string out;
    for (const Check &c : checks) {
        out += c.status == Status::Ok ? "ok    " : c.status == Status::Warn ? "warn  " : "block ";
        out += c.title;
        if (!c.detail.empty())
            out += ": " + c.detail;
        out += "\n";
    }
    return out;
}

static std::string percent_encode(const std::string &s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string issue_url(const std::string &note, const std::string &summary) {
    std::string title = note.substr(0, note.find('\n'));
    if (title.size() > 70)
        title = title.substr(0, 67) + "...";
    return std::string(kIssues) + "?template=bug_report.yml&title=" + percent_encode(title) +
           "&what=" + percent_encode(note.substr(0, kUrlField)) +
           "&system=" + percent_encode(summary.substr(0, kUrlField));
}

static std::string first_field(const std::string &text, const std::string &key) {
    size_t at = text.find(key);
    if (at == std::string::npos)
        return "";
    at += key.size();
    std::string v = text.substr(at, text.find('\n', at) - at);
    v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
    const size_t start = v.find_first_not_of(" \t:");
    return start == std::string::npos ? "" : v.substr(start);
}

static std::string system_lines() {
    std::string out;
    out += "System: " + first_field(read_text_file("/etc/os-release"), "PRETTY_NAME=");
    utsname u;
    if (uname(&u) == 0)
        out += std::string(", kernel ") + u.release + " " + u.machine;
    out += "\n";
    const char *session = getenv("XDG_SESSION_TYPE"), *desktop = getenv("XDG_CURRENT_DESKTOP");
    out += std::string("Session: ") + (session ? session : "unknown") +
           (desktop ? std::string(", ") + desktop : std::string()) + "\n";
    out += "CPU: " + first_field(read_text_file("/proc/cpuinfo"), "model name") + "\n";
    const std::string mem = first_field(read_text_file("/proc/meminfo"), "MemTotal");
    if (!mem.empty())
        out += "RAM: " + std::to_string(atoll(mem.c_str()) / (1024 * 1024)) + " GB\n";
    return out;
}

static bool write_file(const std::string &path, const std::string &text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    return (bool)f;
}

ReportResult make_report(const ReportOptions &o) {
    ReportResult r;
    const std::string home = o.home.empty() ? home_dir() : o.home;
    const ScrubNames names = scrub_names(home);
    char stamp[32];
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    const std::string base = home + "/x4vrmod-reports";
    // Never into an earlier report, even in the same second.
    std::string name = std::string("x4vrmod-report-") + stamp;
    for (int i = 2; is_dir(base + "/" + name); i++)
        name = std::string("x4vrmod-report-") + stamp + "-" + std::to_string(i);
    r.dir = base + "/" + name;
    std::error_code ec;
    fs::create_directories(r.dir, ec);
    if (ec) {
        r.error = "Couldn't create " + r.dir + ": " + ec.message();
        return r;
    }

    CheckOptions co;
    co.home = home;
    co.game = o.game;
    const std::vector<Check> rows = run_checks(co);
    const Profile profile = load_profile(profile_path(home));
    const EyeSettings eye = eye_settings(profile);
    const std::vector<SessionLog> logs = session_logs(home);
    bool found_log = !o.log.empty();
    bool debug = false, newest = o.log.empty() || (!logs.empty() && logs[0].path == o.log);
    for (const SessionLog &l : logs)
        if (l.path == o.log)
            debug = l.debug;

    std::string summary = std::string("X4 VR Mod ") + own_version() + " (" +
                          layout_name(detect_layout(home).kind) + ")\n";
    summary += system_lines();
    summary += "Settings: eye " + std::to_string(eye.eye) + ", upscaler " + eye.upscaler + "\n";
    summary += std::string("Log: ") + (found_log ? (debug ? "debug session" : "normal session")
                                                 : "none") + "\n\n";
    summary += "The checklist now, not during the session (VR may simply be off):\n";
    summary += checks_text(rows);
    r.summary = scrub_text(summary, names);

    bool ok = write_file(r.dir + "/summary.txt", r.summary);
    if (!o.note.empty())
        ok = write_file(r.dir + "/what-happened.txt", scrub_text(o.note, names)) && ok;
    if (found_log)
        ok = write_file(r.dir + "/session.log",
                        scrub_text(log_excerpt(o.log, kLogHead, kLogTail), names)) && ok;
    // What the launch script said belongs to the last start only.
    const std::string output = log_dir(home) + "/launch-output.txt";
    if (newest && is_file(output))
        ok = write_file(r.dir + "/launch-output.txt", scrub_text(read_text_file(output), names)) && ok;
    const std::string prof = profile_path(home);
    if (is_file(prof))
        ok = write_file(r.dir + "/profile.toml", scrub_text(read_text_file(prof), names)) && ok;
    // A crash's backtrace, if systemd kept one for this X4 lately.
    bool have = false;
    const Install in = choose_install(home, o.game, &have);
    if (have) {
        const ChildResult c =
            run_child({"coredumpctl", "info", "--no-pager", "--since", "-24h", in.exe}, 20000);
        if (c.started && c.exit_code == 0 && !c.out.empty())
            ok = write_file(r.dir + "/crash.txt", scrub_text(c.out, names)) && ok;
    }
    if (!ok) {
        r.error = "Couldn't write the report in " + r.dir;
        return r;
    }

    r.archive = r.dir + ".tar.gz";
    const ChildResult t = run_child({"tar", "-C", base, "-czf", r.archive, name}, 300000, true);
    if (!t.started || t.exit_code != 0) {
        r.error = "Couldn't pack the report: " + t.out;
        return r;
    }
    struct stat st;
    r.archive_size = stat(r.archive.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
    r.too_big = r.archive_size > kAttachLimit;
    r.issue_url = issue_url(scrub_text(o.note, names), r.summary);
    r.ok = true;
    return r;
}

} // namespace x4vr::launcher
