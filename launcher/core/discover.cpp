// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Finding X4: Steam from its own metadata, GOG from the usual places, and any
// path the player points at, told apart by what the file is.
#include "x4vr_i18n.hpp"
#include "x4vr_launcher.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace x4vr::launcher {

// ---------------------------------------------------------------- small tools
std::string read_text_file(const std::string &path, size_t max) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    std::string s(max, '\0');
    f.read(&s[0], (std::streamsize)max);
    s.resize((size_t)f.gcount());
    return s;
}

bool is_file(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string real_path(const std::string &path) {
    char buf[PATH_MAX];
    return realpath(path.c_str(), buf) ? std::string(buf) : path;
}

std::string dir_name(const std::string &path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string base_name(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string home_dir() {
    const char *h = getenv("HOME");
    if (h && *h)
        return h;
    const passwd *pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "";
}

bool have_gamemode() { return !find_on_path("gamemoderun").empty(); }

std::string find_on_path(const std::string &name) {
    const char *p = getenv("PATH");
    std::stringstream ss(p ? p : "/usr/local/bin:/usr/bin:/bin");
    std::string d;
    while (std::getline(ss, d, ':')) {
        const std::string f = (d.empty() ? "." : d) + "/" + name;
        if (is_file(f) && access(f.c_str(), X_OK) == 0)
            return f;
    }
    return "";
}

ChildResult run_child(const std::vector<std::string> &argv, int timeout_ms,
                      bool merge_stderr,
                      const std::vector<std::string> &extra_env) {
    ChildResult r;
    int fds[2];
    if (argv.empty() || pipe(fds) != 0)
        return r;
    // The window calls this from a worker thread, and after fork in a threaded
    // process only async-signal-safe calls are allowed. So everything the child
    // needs is built here, before it exists.
    std::vector<std::string> env;
    for (char **e = environ; e && *e; e++) {
        const std::string kv = *e;
        const std::string name = kv.substr(0, kv.find('=') + 1);
        bool replaced = false;
        for (const std::string &x : extra_env)
            replaced = replaced || x.rfind(name, 0) == 0;
        if (!replaced)
            env.push_back(kv);
    }
    env.insert(env.end(), extra_env.begin(), extra_env.end());
    std::vector<char *> envp;
    for (const std::string &s : env)
        envp.push_back(const_cast<char *>(s.c_str()));
    envp.push_back(nullptr);
    std::vector<char *> a;
    for (const std::string &s : argv)
        a.push_back(const_cast<char *>(s.c_str()));
    a.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return r;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0)
            dup2(null, 0);
        if (merge_stderr)
            dup2(fds[1], 2);
        else if (null >= 0)
            dup2(null, 2);
        close(fds[0]);
        close(fds[1]);
        execvpe(a[0], a.data(), envp.data());
        _exit(127);
    }
    close(fds[1]);
    r.started = true;
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              end - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            r.timed_out = true;
            break;
        }
        pollfd pf{fds[0], POLLIN, 0};
        const int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0) {
            r.timed_out = pr == 0;
            break;
        }
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0)
            break;
        if (r.out.size() < 1024 * 1024)
            r.out.append(buf, (size_t)n);
    }
    close(fds[0]);
    if (r.timed_out)
        kill(-pid, SIGKILL);
    int st = 0;
    if (!r.timed_out) {
        // The pipe closing isn't the child exiting: it may have closed stdout
        // and hung. The same deadline holds for the wait.
        while (waitpid(pid, &st, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= end) {
                r.timed_out = true;
                kill(-pid, SIGKILL);
                break;
            }
            usleep(2000);
        }
    }
    if (r.timed_out) {
        waitpid(pid, &st, 0);
        return r;
    }
    r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

// ---------------------------------------------------------------- Valve's VDF
static bool same_nocase(const std::string &a, const std::string &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

const VdfNode *VdfNode::find(const std::string &k) const {
    for (const VdfNode &c : children)
        if (same_nocase(c.key, k))
            return &c;
    return nullptr;
}

std::string VdfNode::get(const std::string &k) const {
    const VdfNode *c = find(k);
    return c && !c->block ? c->value : "";
}

namespace {
struct VdfParser {
    const std::string &t;
    size_t i = 0;
    int depth = 0;
    void skip() {
        for (;;) {
            while (i < t.size() && isspace((unsigned char)t[i]))
                i++;
            if (i + 1 < t.size() && t[i] == '/' && t[i + 1] == '/') {
                while (i < t.size() && t[i] != '\n')
                    i++;
                continue;
            }
            return;
        }
    }
    bool string(std::string *out) {
        if (i >= t.size() || t[i] != '"')
            return false;
        i++;
        out->clear();
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < t.size()) {
                i++;
                const char c = t[i];
                out->push_back(c == 'n' ? '\n' : c == 't' ? '\t' : c);
            } else {
                out->push_back(t[i]);
            }
            i++;
        }
        if (i >= t.size())
            return false; // an unterminated string
        i++;
        return true;
    }
    // Entries until `}` or, at the top, the end of the text.
    bool entries(VdfNode *parent, bool top) {
        if (++depth > 64)
            return false;
        for (;;) {
            skip();
            if (i >= t.size()) {
                depth--;
                return top;
            }
            if (t[i] == '}') {
                if (top)
                    return false;
                i++;
                depth--;
                return true;
            }
            VdfNode n;
            if (!string(&n.key))
                return false;
            skip();
            if (i < t.size() && t[i] == '{') {
                i++;
                n.block = true;
                if (!entries(&n, false))
                    return false;
            } else if (!string(&n.value)) {
                return false;
            }
            parent->children.push_back(std::move(n));
        }
    }
};
} // namespace

bool parse_vdf(const std::string &text, VdfNode *root) {
    VdfParser p{text};
    *root = VdfNode{};
    root->block = true;
    return p.entries(root, true);
}

// ------------------------------------------------------------ finding the game
enum class FileKind { LinuxX64, Windows, Other, Unreadable };

static FileKind file_kind(const std::string &path) {
    uint8_t h[20] = {};
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return FileKind::Unreadable;
    const size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n >= 2 && h[0] == 'M' && h[1] == 'Z')
        return FileKind::Windows;
    if (n >= 20 && memcmp(h, "\x7f"
                             "ELF",
                          4) == 0 &&
        h[4] == 2 && h[5] == 1 && h[18] == 62 && h[19] == 0)
        return FileKind::LinuxX64;
    return FileKind::Other;
}

GamePath classify_game_path(const std::string &path) {
    GamePath g;
    if (path.empty() || (!is_file(path) && !is_dir(path))) {
        g.why = tr_("that path doesn't exist");
        return g;
    }
    auto found = [&g](const std::string &exe, PathKind k) {
        g.kind = k;
        g.exe = exe;
        g.dir = dir_name(exe);
        return g;
    };
    if (is_file(path)) {
        const FileKind k = file_kind(path);
        if (k == FileKind::Windows)
            return found(path, PathKind::Windows);
        if (k == FileKind::LinuxX64 && base_name(path) == "X4")
            return found(path, PathKind::Linux);
        g.kind = PathKind::NotX4;
        g.why = k == FileKind::LinuxX64
                    ? tr_("that's a Linux program, but not one called X4")
                    : tr_("that isn't the X4 program");
        return g;
    }
    for (const char *rel : {"X4", "game/X4"}) {
        const std::string f = path + "/" + rel;
        if (is_file(f) && file_kind(f) == FileKind::LinuxX64)
            return found(f, PathKind::Linux);
    }
    for (const char *rel : {"X4.exe", "game/X4.exe"}) {
        const std::string f = path + "/" + rel;
        if (is_file(f))
            return found(f, PathKind::Windows);
    }
    g.kind = PathKind::NotX4;
    g.why = tr_("no X4 program in that folder");
    return g;
}

const char *proton_explanation() {
    return tr_("This is the Windows version of X4, which runs through Proton. The "
               "mod works only with the Linux version: it's a Vulkan layer and a "
               "preloaded library that attach to the Linux program. On Steam, open "
               "X4's Properties, then Compatibility, and turn off forced "
               "compatibility so Steam downloads the Linux version.");
}

const char *store_name(Store s) {
    return s == Store::Steam ? "Steam" : s == Store::Gog ? "GOG" : tr_("Other");
}

Install install_from_path(const std::string &path) {
    const GamePath g = classify_game_path(path);
    Install in;
    in.kind = g.kind;
    in.exe = g.exe;
    in.dir = g.dir;
    in.source = path;
    if (g.kind == PathKind::Linux)
        in.store = binary_needs_steam(g.exe) ? Store::Steam : Store::Gog;
    in.flatpak_steam = !g.exe.empty() && in_flatpak_steam(real_path(g.exe));
    return in;
}

bool in_flatpak_steam(const std::string &path) {
    return path.find("/.var/app/com.valvesoftware.Steam/") != std::string::npos;
}

std::vector<Install> discover_steam(const std::string &home) {
    std::vector<Install> out;
    std::vector<std::string> seen_roots, seen_libs;
    const std::string candidates[] = {
        home + "/.steam/root",
        home + "/.steam/steam",
        home + "/.local/share/Steam",
        home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
        home + "/.var/app/com.valvesoftware.Steam/data/Steam",
    };
    for (const std::string &c : candidates) {
        if (!is_dir(c + "/steamapps"))
            continue;
        const std::string root = real_path(c);
        // After the native roots, so a library both list is the native one's.
        const bool flatpak = in_flatpak_steam(root);
        if (std::find(seen_roots.begin(), seen_roots.end(), root) != seen_roots.end())
            continue;
        seen_roots.push_back(root);

        // The libraries Steam knows, and the root itself in case the file is
        // missing or older than the library it should list. Each library's own
        // appmanifest decides, not the file's "apps" list, which can be stale.
        std::vector<std::string> libs{root};
        for (const char *rel : {"/steamapps/libraryfolders.vdf", "/config/libraryfolders.vdf"}) {
            VdfNode vdf;
            if (!parse_vdf(read_text_file(root + rel), &vdf))
                continue;
            const VdfNode *lf = vdf.find("libraryfolders");
            if (!lf)
                continue;
            for (const VdfNode &e : lf->children) {
                const std::string p = e.block ? e.get("path") : "";
                if (!p.empty())
                    libs.push_back(p);
            }
            break;
        }
        for (const std::string &l : libs) {
            const std::string lib = real_path(l);
            if (std::find(seen_libs.begin(), seen_libs.end(), lib) != seen_libs.end())
                continue;
            seen_libs.push_back(lib);
            const std::string acf =
                lib + "/steamapps/appmanifest_" + kSteamAppId + ".acf";
            VdfNode m;
            if (!is_file(acf) || !parse_vdf(read_text_file(acf), &m))
                continue;
            const VdfNode *st = m.find("AppState");
            const std::string dir = st ? st->get("installdir") : "";
            if (dir.empty())
                continue;
            const GamePath g =
                classify_game_path(lib + "/steamapps/common/" + dir);
            if (g.kind == PathKind::Missing || g.kind == PathKind::NotX4)
                continue; // listed but not there: a stale manifest
            Install in;
            in.store = Store::Steam;
            in.kind = g.kind;
            in.exe = g.exe;
            in.dir = g.dir;
            in.buildid = st->get("buildid");
            in.source = acf;
            in.flatpak_steam = flatpak;
            out.push_back(std::move(in));
        }
    }
    return out;
}

std::vector<std::string> json_strings(const std::string &t, const std::string &key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + key + "\"";
    for (size_t i = t.find(needle); i != std::string::npos; i = t.find(needle, i + 1)) {
        size_t j = i + needle.size();
        while (j < t.size() && isspace((unsigned char)t[j]))
            j++;
        if (j >= t.size() || t[j] != ':')
            continue;
        j++;
        while (j < t.size() && isspace((unsigned char)t[j]))
            j++;
        if (j >= t.size() || t[j] != '"')
            continue;
        std::string v;
        for (j++; j < t.size() && t[j] != '"'; j++) {
            if (t[j] == '\\' && j + 1 < t.size())
                j++;
            v.push_back(t[j]);
        }
        out.push_back(v);
    }
    return out;
}

std::vector<Install> discover_gog(const std::string &home) {
    std::vector<Install> out;
    auto consider = [&out](const std::string &path, const std::string &source) {
        const GamePath g = classify_game_path(path);
        if (g.kind != PathKind::Linux && g.kind != PathKind::Windows)
            return;
        Install in;
        in.store = Store::Gog;
        in.kind = g.kind;
        in.exe = g.exe;
        in.dir = g.dir;
        in.source = source;
        out.push_back(std::move(in));
    };
    const std::string gog = home + "/GOG Games";
    if (DIR *d = opendir(gog.c_str())) {
        while (const dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            std::string low = n;
            for (char &c : low)
                c = (char)tolower((unsigned char)c);
            if (low.rfind("x4", 0) == 0)
                consider(gog + "/" + n, gog);
        }
        closedir(d);
    }
    for (const char *rel : {"/.config/heroic/gog_store/installed.json",
                            "/.var/app/com.heroicgameslauncher.hgl/config/heroic/"
                            "gog_store/installed.json"}) {
        const std::string f = home + rel;
        for (const std::string &p : json_strings(read_text_file(f), "install_path"))
            consider(p, f);
    }
    return out;
}

std::vector<Install> discover_all(const std::string &home) {
    std::vector<Install> all;
    if (const char *e = getenv("X4VR_GAME")) {
        if (*e) {
            Install in = install_from_path(e);
            in.source = "X4VR_GAME";
            if (in.kind == PathKind::Linux || in.kind == PathKind::Windows)
                all.push_back(std::move(in));
        }
    }
    auto add = [&all](std::vector<Install> v) {
        for (Install &in : v) {
            const std::string r = real_path(in.exe);
            bool dup = false;
            for (Install &have : all)
                if (real_path(have.exe) == r) {
                    // The same copy: keep what the metadata knows about it.
                    if (have.buildid.empty())
                        have.buildid = in.buildid;
                    if (have.store == Store::Unknown)
                        have.store = in.store;
                    dup = true;
                }
            if (!dup)
                all.push_back(std::move(in));
        }
    };
    add(discover_steam(home));
    add(discover_gog(home));
    return all;
}

const Install *preferred_install(const std::vector<Install> &all) {
    for (const Install &in : all)
        if (in.kind == PathKind::Linux)
            return &in;
    return all.empty() ? nullptr : &all.front();
}

int read_x4_version(const std::string &dir) {
    const std::string t = read_text_file(dir + "/version.dat", 64);
    size_t i = 0;
    while (i < t.size() && isspace((unsigned char)t[i]))
        i++;
    int v = 0, digits = 0;
    while (i < t.size() && isdigit((unsigned char)t[i]) && digits < 7) {
        v = v * 10 + (t[i] - '0');
        i++;
        digits++;
    }
    return digits ? v : -1;
}

std::string format_x4_version(int v) {
    if (v < 0)
        return "unknown";
    char b[32];
    snprintf(b, sizeof b, "%d.%02d", v / 100, v % 100);
    return b;
}

// ----------------------------------------------------------------- the runtime
static bool any_process(const std::function<bool(const std::string &)> &f);

std::string steamvr_manifest_for(const std::string &vrserver_exe) {
    if (base_name(vrserver_exe) != "vrserver")
        return "";
    const std::string m =
        dir_name(dir_name(dir_name(vrserver_exe))) + "/steamxr_linux64.json";
    return is_file(m) ? m : "";
}

static void read_runtime_manifest(const std::string &p, ActiveRuntime *rt) {
    rt->path = p;
    const std::string t = read_text_file(p, 64 * 1024);
    const auto names = json_strings(t, "name");
    const auto libs = json_strings(t, "library_path");
    rt->name = names.empty() ? "" : names.front();
    rt->library = libs.empty() ? "" : libs.front();
    const std::string both = rt->name + " " + rt->library + " " + real_path(p);
    rt->steamvr = both.find("SteamVR") != std::string::npos ||
                  both.find("steamxr") != std::string::npos;
}

ActiveRuntime active_runtime(const std::string &home, bool steamvr_fallback) {
    ActiveRuntime rt;
    std::vector<std::string> tries;
    if (const char *e = getenv("XR_RUNTIME_JSON"))
        if (*e)
            tries.push_back(e);
    const char *xch = getenv("XDG_CONFIG_HOME");
    tries.push_back((xch && *xch ? std::string(xch) : home + "/.config") +
                    "/openxr/1/active_runtime.json");
    const char *xcd = getenv("XDG_CONFIG_DIRS");
    std::stringstream ss(xcd && *xcd ? xcd : "/etc/xdg");
    std::string d;
    while (std::getline(ss, d, ':'))
        if (!d.empty())
            tries.push_back(d + "/openxr/1/active_runtime.json");
    tries.push_back("/etc/openxr/1/active_runtime.json");
    for (const std::string &p : tries) {
        if (!is_file(p))
            continue;
        read_runtime_manifest(p, &rt);
        return rt;
    }
    if (steamvr_fallback) {
        std::string exe;
        any_process([&exe](const std::string &e) {
            if (base_name(e) != "vrserver")
                return false;
            exe = e;
            return true;
        });
        const std::string m = steamvr_manifest_for(exe);
        if (!m.empty()) {
            read_runtime_manifest(m, &rt);
            rt.steamvr = true;
            rt.fallback = true;
        }
    }
    return rt;
}

// ----------------------------------------------------------------- is it running
static std::string proc_exe(const std::string &pid) {
    char buf[PATH_MAX];
    const std::string link = "/proc/" + pid + "/exe";
    const ssize_t n = readlink(link.c_str(), buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, (size_t)n) : "";
}

// Calls `f` with each process's program until it returns true.
static bool any_process(const std::function<bool(const std::string &)> &f) {
    DIR *d = opendir("/proc");
    if (!d)
        return false;
    bool hit = false;
    while (!hit) {
        const dirent *e = readdir(d);
        if (!e)
            break;
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        const std::string exe = proc_exe(e->d_name);
        if (!exe.empty() && f(exe))
            hit = true;
    }
    closedir(d);
    return hit;
}

bool steam_is_running(const std::string &home) {
    // steam.pid outlives Steam, so a live process has to back it. A Flatpak
    // Steam's pid is from its own namespace, hence the scan as well.
    for (const char *rel : {"/.steam/steam.pid",
                            "/.var/app/com.valvesoftware.Steam/.steam/steam.pid"}) {
        std::string t = read_text_file(home + rel, 32);
        while (!t.empty() && isspace((unsigned char)t.back()))
            t.pop_back();
        if (!t.empty() && base_name(proc_exe(t)) == "steam")
            return true;
    }
    return any_process([](const std::string &exe) {
        return base_name(exe) == "steam" &&
               exe.find("ubuntu12_32") != std::string::npos;
    });
}

bool steamvr_is_running() {
    return any_process([](const std::string &exe) {
        return base_name(exe) == "vrserver";
    });
}

bool x4_is_running(const std::string &exe) {
    const std::string want = exe.empty() ? "" : real_path(exe);
    return any_process([&want](const std::string &p) {
        return p == want || base_name(p) == "X4";
    });
}

} // namespace x4vr::launcher
