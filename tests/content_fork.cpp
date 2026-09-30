// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The content.xml fork, checked without X4: which paths it claims (the
// profile's, never an extension's own manifest), and that the rewrite flips
// exactly our entry and leaves every other byte alone. Self-checking.
#include <cstdio>
#include <string>

#include "../injector/x4vr_content.hpp"

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

int main() {
    using namespace x4vr;
    check(is_x4_profile_content("/home/p/.config/EgoSoft/X4/96679891/content.xml"),
          "claims the profile's content.xml");
    check(!is_x4_profile_content("/nvme/SteamLibrary/steamapps/common/X4 Foundations/"
                                 "extensions/x4vrmod_lookaround/content.xml"),
          "does NOT claim an extension's own manifest");
    check(!is_x4_profile_content("/home/p/.config/EgoSoft/X4/96679891/extensions/"
                                 "foo/content.xml"),
          "does NOT claim an extension manifest even under the profile dir");
    check(!is_x4_profile_content("/home/p/.config/EgoSoft/X4/96679891/content-x4vrmod.xml"),
          "does NOT claim its own fork");
    check(!is_x4_profile_content("/tmp/content.xml"),
          "does NOT claim a content.xml outside an X4 profile");
    check(content_profile_path("/a/EgoSoft/X4/1/content.xml") ==
              "/a/EgoSoft/X4/1/content-x4vrmod.xml",
          "fork path sits beside it");

    // Patola's real shape: our entry present and disabled among others.
    const std::string real =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<content>\n"
        "  <extension id=\"ws_1666869624\" enabled=\"true\"/>\n"
        "  <extension id=\"SectorSatellites\" enabled=\"false\"/>\n"
        "  <extension id=\"x4vrmod_lookaround\" enabled=\"false\"/>\n"
        "</content>\n";
    std::string a = real;
    check(enable_extension(a, "x4vrmod_lookaround") == 1, "flips a disabled entry");
    check(a.find("<extension id=\"x4vrmod_lookaround\" enabled=\"true\"/>") !=
              std::string::npos,
          "our entry now reads enabled=\"true\"");
    check(a.find("<extension id=\"SectorSatellites\" enabled=\"false\"/>") !=
              std::string::npos,
          "another extension's disabled state is left alone");
    check(a.size() == real.size() - 1, "only 'false' -> 'true' changed (one byte shorter)");
    std::string b = a;
    check(enable_extension(b, "x4vrmod_lookaround") == 0 && b == a,
          "already enabled: untouched");

    std::string c = "<content>\n  <extension id=\"other\" enabled=\"true\"/>\n</content>\n";
    check(enable_extension(c, "x4vrmod_lookaround") == 2 &&
              c.find("<extension id=\"x4vrmod_lookaround\" enabled=\"true\"/>\n</content>") !=
                  std::string::npos,
          "absent: inserted before </content>");

    std::string d = "<content><extension id=\"x4vrmod_lookaround\"/></content>";
    check(enable_extension(d, "x4vrmod_lookaround") == 1 &&
              d.find("<extension id=\"x4vrmod_lookaround\" enabled=\"true\"/>") !=
                  std::string::npos,
          "entry without the attribute gains enabled=\"true\"");

    // A prefix must not be mistaken for our id.
    std::string e = "<content>\n  <extension id=\"x4vrmod_lookaround_old\" enabled=\"false\"/>\n</content>\n";
    check(enable_extension(e, "x4vrmod_lookaround") == 2 &&
              e.find("x4vrmod_lookaround_old\" enabled=\"false\"") != std::string::npos,
          "an id that merely starts with ours is not touched");

    std::string f = "garbage";
    check(enable_extension(f, "x4vrmod_lookaround") == -1 && f == "garbage",
          "no </content>: refused, file untouched");

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
