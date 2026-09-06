// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The `env: run =` line must round-trip through a shell.
//
// Take 258's command was written from that line and contained
// `X4VR_WATCH_WANT=ui dump: burst frames`, which no shell reads as one
// assignment. Patola repaired it by hand before the take could start. The
// comment above the code called the line "copy-pasteable" the whole time.
//
// **This test does not check the quoting against my model of a shell.** It
// hands each case to `sh` and to `zsh` -- Patola's shell, and the one whose
// word-splitting rules already cost a measurement (see the empty --frames that
// silently measured 93 dumps across three scenes) -- and asks THEM what the
// variable ended up holding. A quoting bug and a wrong expectation cannot then
// cancel, which is the failure a hand-written expected-string would allow.
#include "../common/x4vr_shquote.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const std::string &what) {
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what.c_str());
    }
}

// Ask `shell` what a child process receives in X4VR_T after the quoted
// assignment prefix is parsed.
//
// **The assignment must be a PREFIX on a command, and the reader must be that
// command**, because that is the shape of every take: `X4VR_A=1 X4VR_B=...
// ./launch/x4vr-launch.sh`. The first version of this helper wrote
//
//     X4VR_T=<quoted> printf '<%s>' "$X4VR_T"
//
// and every case came back empty -- the calling shell expands `$X4VR_T`
// BEFORE applying the prefix, so it read its own unset variable. That is a
// bug in the harness that looks exactly like a total failure of the code
// under test, which is why the expected values here are never hand-written.
//
// Run through fork/exec rather than popen(): popen would hand the command to
// ANOTHER /bin/sh first, so a quoting bug could be masked or manufactured by a
// layer that does not exist in the real path. Exactly one shell parses exactly
// one string here, and that string is the construct being tested.
static bool shell_roundtrip(const char *shell, const std::string &value,
                            std::string &got) {
    // The inner script is a fixed literal, hand-quoted and verifiable by eye:
    // it contains no single quote, so wrapping it in single quotes needs no
    // escaping. The inner `sh` reads X4VR_T from the environment the prefix
    // put it in -- it does no expansion in the outer shell's context.
    const std::string cmd = "X4VR_T=" + x4vr::sh_quote(value.c_str()) +
                            " sh -c 'printf \"<%s>\" \"$X4VR_T\"'";
    int fd[2];
    if (pipe(fd) != 0)
        return false;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // Nothing inherited can set X4VR_T and mask a failure to set it here.
        unsetenv("X4VR_T");
        execlp(shell, shell, "-c", cmd.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(fd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = read(fd[0], buf, sizeof buf)) > 0)
        out.append(buf, (size_t)n);
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return false;
    // Strip the delimiters the printf added. They are what makes leading and
    // trailing whitespace, and the empty value, distinguishable at all.
    if (out.size() < 2 || out.front() != '<' || out.back() != '>')
        return false;
    got = out.substr(1, out.size() - 2);
    return true;
}

static bool have_shell(const char *sh) {
    const std::string cmd = std::string("command -v ") + sh + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

int main() {
    // The value from take 258 is first, and it is the reason this file exists.
    const std::vector<std::string> cases = {
        "ui dump: burst frames",         // take 258: the one that broke
        "burst dumps",                   // X4VR_WATCH_SAY, same run
        "1",                             // the overwhelmingly common shape
        "1408x1408",
        "258-BURST",
        "/tmp/x4vr-take258.log",
        "600:6",
        "0.115",
        "1.4917",
        "",                              // X4VR_LOG= means stderr, not unset
        "a b",
        "it's got a quote",              // the single character that matters
        "two''quotes",
        "trailing space ",
        " leading space",
        "tab\there",
        "semi;colon && rm -rf /",        // must stay DATA, never run
        "$X4VR_LOG",                     // must not expand
        "`id`",                          // must not expand
        "$(id)",
        "*",                             // must not glob
        "~",                             // must not tilde-expand
        "a\nb",                          // newline inside a value
        "!!",                            // zsh history expansion
        "#comment",
        "back\\slash",
        "quote'in$the`middle",
    };

    // --- the property that matters: value in, same value out --------------
    int shells = 0;
    for (const char *sh : {"sh", "zsh", "bash"}) {
        if (!have_shell(sh)) {
            printf("  skip  %s not installed\n", sh);
            continue;
        }
        shells++;
        for (const auto &v : cases) {
            std::string got;
            const bool ran = shell_roundtrip(sh, v, got);
            ok(ran, std::string(sh) + ": roundtrip ran for [" + v + "]");
            if (ran)
                ok(got == v, std::string(sh) + ": [" + v + "] came back as [" +
                                 got + "]");
        }
    }
    // zsh is Patola's shell and the one every handed-over command is pasted
    // into. If it is missing, this test proved much less than it looks like it
    // did -- say so rather than passing quietly.
    if (!have_shell("zsh"))
        printf("  NOTE: zsh absent — the shell every take command is pasted "
               "into was NOT exercised\n");
    ok(shells > 0, "at least one shell available to test against");

    // --- the bare/quoted split, so the common line stays readable ----------
    // Quoting everything would also be correct and would make every command
    // unreadable. These are the shapes that must stay bare.
    for (const char *v : {"1", "0", "1408x1408", "258-BURST", "600:6", "0.115",
                          "/tmp/x4vr-take258.log", "a=b", "u@h", "50%", "x^2",
                          "a,b", "a.b", "a-b", "a_b", "a+b"})
        ok(x4vr::sh_quote(v) == v, std::string("stays bare: ") + v);

    // And the shapes that must NOT stay bare, checked directly so a widened
    // safe-set cannot pass this file by accident.
    for (const char *v : {"", " ", "a b", "a'b", "a\"b", "a$b", "a`b", "a;b",
                          "a&b", "a|b", "a*b", "a?b", "a(b", "a{b", "a<b",
                          "a>b", "a#b", "a!b", "a~b", "a\\b", "a\nb", "a\tb"})
        ok(x4vr::sh_quote(v) != v, std::string("gets quoted: [") + v + "]");

    // --- assignment form ---------------------------------------------------
    ok(x4vr::sh_quote_assignment("X4VR_STEREO=1") == "X4VR_STEREO=1",
       "assignment: simple value stays bare");
    ok(x4vr::sh_quote_assignment("X4VR_WATCH_WANT=ui dump: burst frames") ==
           "X4VR_WATCH_WANT='ui dump: burst frames'",
       "assignment: take 258's value is quoted");
    ok(x4vr::sh_quote_assignment("X4VR_LOG=") == "X4VR_LOG=''",
       "assignment: empty value is '' — an empty X4VR_LOG means stderr, and a "
       "bare trailing = is ambiguous when hand-copied");
    // A value containing '=' must not be re-split: only the FIRST '=' divides
    // name from value.
    ok(x4vr::sh_quote_assignment("X4VR_A=b=c") == "X4VR_A=b=c",
       "assignment: splits on the first = only");
    ok(x4vr::sh_quote_assignment("NOEQUALS") == "NOEQUALS",
       "assignment: an entry with no = is passed through");
    ok(x4vr::sh_quote_assignment(nullptr).empty(),
       "assignment: null is empty, not a crash");

    printf("%s  shquote: %d check(s), %d failure(s)\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
