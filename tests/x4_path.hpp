// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4_path.hpp -- the X4 program a test checks the patch sites against, on whoever's
// machine it runs: X4_BINARY or X4VR_GAME when set, else what `x4vrmod locate` finds
// from the same build tree. "" when there's none, and the test skips those checks.
#pragma once

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

inline const char *x4_test_binary() {
    static const std::string path = [] {
        for (const char *name : {"X4_BINARY", "X4VR_GAME"}) {
            const char *e = getenv(name);
            if (e && *e)
                return std::string(e);
        }
        // Tests are in build/tests, the tool in build/launcher.
        char self[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0)
            return std::string();
        std::string build(self, (size_t)n);
        build.resize(build.rfind('/'));
        build.resize(build.rfind('/'));
        FILE *p = popen(("'" + build + "/launcher/x4vrmod' locate 2>/dev/null").c_str(), "r");
        if (!p)
            return std::string();
        char line[PATH_MAX] = "";
        const bool got = fgets(line, sizeof line, p) != nullptr;
        pclose(p);
        if (!got)
            return std::string();
        line[strcspn(line, "\n")] = 0;
        return std::string(line);
    }();
    return path.c_str();
}
