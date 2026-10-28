// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr::spv::declares_member_name: the fixture pair, and -- when the dumps
// are present -- every module of take 347's 519: only the fog ray march's two
// modules carry U_volume_box_position (take 348).
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "../common/x4vr_spirv.hpp"

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

static bool load(const std::string &path, std::vector<uint32_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n / 4);
    const bool ok = n > 0 && fread(out.data(), 4, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    // The fixtures are copied next to the binary (tests/CMakeLists.txt).
    std::string dir = argc > 1 ? argv[1] : std::string(argv[0]);
    if (argc <= 1) {
        const size_t slash = dir.rfind('/');
        dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    }
    std::vector<uint32_t> code;
    check(load(dir + "/sample_volume_ray.frag.spv", code) &&
              x4vr::spv::declares_member_name(code, "U_volume_box_position"),
          "the fog-shaped fixture is recognised by U_volume_box_position");
    check(!x4vr::spv::declares_member_name(code, "U_volume_box"),
          "a prefix of the name is not a match");
    check(!x4vr::spv::declares_member_name(code, "U_volume_box_positionX"),
          "a longer name is not a match");
    check(!x4vr::spv::declares_member_name(code, ""),
          "an empty name is never a match");
    check(load(dir + "/sample_invproj.frag.spv", code) &&
              !x4vr::spv::declares_member_name(code, "U_volume_box_position"),
          "the plain inverse-projection fixture is not recognised");
    std::vector<uint32_t> junk = {0x07230203u, 1, 2};
    check(!x4vr::spv::declares_member_name(junk, "U_volume_box_position"),
          "a truncated module answers false");

    // Take 347's dumps, if this machine still has them.
    const char *dumps = "/tmp/x4vr-shaders";
    DIR *d = opendir(dumps);
    if (!d) {
        printf("SKIP   no %s — the census over X4's own modules is unchecked\n",
               dumps);
    } else {
        std::vector<std::string> hits;
        int n = 0;
        while (dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() < 5 || name.substr(name.size() - 4) != ".spv")
                continue;
            if (!load(std::string(dumps) + "/" + name, code))
                continue;
            n++;
            if (x4vr::spv::declares_member_name(code, "U_volume_box_position"))
                hits.push_back(name);
        }
        closedir(d);
        bool only_fog = !hits.empty();
        for (const auto &h : hits)
            only_fog &= h == "mod-0473.spv" || h == "mod-0474.spv";
        printf("       %d modules, %zu with U_volume_box_position\n", n,
               hits.size());
        check(only_fog || n != 519,
              "take 347's 519 modules: only mod-0473/0474 (the ray march)");
    }
    printf("\n%s\n", g_fail ? "SOME CASES FAILED" : "all cases passed");
    return g_fail ? 1 : 0;
}
