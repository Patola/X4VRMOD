// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// What a pause reads as (injector/x4vr_pause.hpp), replayed from the
// sequences takes 383–385 logged: the Pause key, the ESC menu opening in the
// pump that pauses, menu to menu while paused (one check with none up), the
// last menu closed while paused, no full-screen query. Self-checking.
#include <cstdio>
#include <string>
#include <vector>

#include "../injector/x4vr_pause.hpp"

using x4vr::pause::Decider;
using x4vr::pause::Kind;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok    " : "FAIL  ", what);
    if (!ok)
        g_fail = 1;
}

// One full-screen check per tick: paused[i], menu_up[i] -> Kind per tick.
struct Replay {
    Decider d;
    uint64_t seq = 0, down_seq = 0;
    bool up = false;
    std::vector<Kind> run(const std::string &paused, const std::string &menu,
                          bool known = true) {
        std::vector<Kind> out;
        for (size_t i = 0; i < paused.size(); i++) {
            // head-look's tick reads the pause first, then the check runs
            out.push_back(d.step(paused[i] == 'P', known, up, seq, down_seq));
            const bool now = menu[i] == 'M';
            seq++;
            if (up && !now)
                down_seq = seq;
            up = now;
        }
        return out;
    }
};

static int count(const std::vector<Kind> &v, Kind k) {
    int n = 0;
    for (Kind x : v)
        n += x == k;
    return n;
}

int main() {
    {
        // The Pause key in the cockpit: no menu for a long time before.
        Replay r;
        auto v = r.run("..........PPPPPPPPPPPPPPP.....",
                       "..............................");
        check(count(v, Kind::MenuPause) == 0,
              "Pause key: never PAUSED (no window)");
        check(v[10] == Kind::Undecided && v[11] == Kind::Undecided &&
                  v[12] == Kind::KeyPause,
              "Pause key: undecided for two checks, then the Pause key");
    }
    {
        // The ESC menu: the pause and the menu in the same pump, the pause
        // read first (take 383).
        Replay r;
        auto v = r.run("..........PPPPPPPPPPPPPPP.....",
                       "..........MMMMMMMMMMMMMMM.....");
        check(count(v, Kind::KeyPause) == 0, "ESC menu: never the Pause key");
        check(v[12] == Kind::MenuPause && v[24] == Kind::MenuPause,
              "ESC menu: PAUSED once decided");
    }
    {
        // Menu to menu while paused: one check with none up (take 385).
        Replay r;
        auto v = r.run("..........PPPPPPPPPPPPPPPPPPPPPPPPPP",
                       "..........MMMMMMMM.MMMMMMMM.MMMMMMMM");
        check(count(v, Kind::KeyPause) == 0,
              "menu to menu while paused: no Pause-key flip");
    }
    {
        // The last menu closed while still paused: the window ~8 checks
        // longer, then the Pause key.
        Replay r;
        auto v = r.run("..........PPPPPPPPPPPPPPPPPPPPPPPPPPPPPP",
                       "..........MMMMMMMMMM....................");
        int first_key = -1;
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == Kind::KeyPause) {
                first_key = (int)i;
                break;
            }
        // The menu's last tick is 19; the check during tick 20 is the first
        // to see none up, and the settle counts kSettle checks from there.
        check(first_key == 20 + (int)x4vr::pause::kSettle + 1,
              ("menu closed while paused: Pause key after the settle (at " +
               std::to_string(first_key) + ")").c_str());
    }
    {
        // Without the full-screen query every pause is PAUSED, as before.
        Replay r;
        auto v = r.run("..........PPPPPPPP", "..................", false);
        check(count(v, Kind::MenuPause) == 8 && count(v, Kind::KeyPause) == 0,
              "no query: every pause PAUSED");
    }
    {
        // Not paused: nothing to decide, whatever the menus do.
        Replay r;
        auto v = r.run("..........", "..MMM.....");
        check(count(v, Kind::NotPaused) == 10, "not paused: NotPaused");
    }
    std::printf("\n%s\n", g_fail ? "FAILED" : "all cases passed");
    return g_fail;
}
