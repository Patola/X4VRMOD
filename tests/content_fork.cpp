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
    // The folder under X4/ is the player's own: Steam's is their account number.
    check(is_x4_profile_content("/home/q/.config/EgoSoft/X4/1234567/content.xml"),
          "claims it under someone else's number");
    check(is_x4_profile_content("/data/xdg/EgoSoft/X4/1234567/content.xml"),
          "claims it under XDG_CONFIG_HOME");
    check(is_x4_profile_content("/home/q/.config/EgoSoft/X4/content.xml"),
          "claims it with no folder for the player");
    check(!is_x4_profile_content("/some/library/X4 Foundations/"
                                 "extensions/x4vrmod/content.xml"),
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
        "  <extension id=\"x4vrmod\" enabled=\"false\"/>\n"
        "</content>\n";
    std::string a = real;
    check(enable_extension(a, "x4vrmod") == 1, "flips a disabled entry");
    check(a.find("<extension id=\"x4vrmod\" enabled=\"true\"/>") !=
              std::string::npos,
          "our entry now reads enabled=\"true\"");
    check(a.find("<extension id=\"SectorSatellites\" enabled=\"false\"/>") !=
              std::string::npos,
          "another extension's disabled state is left alone");
    check(a.size() == real.size() - 1, "only 'false' -> 'true' changed (one byte shorter)");
    std::string b = a;
    check(enable_extension(b, "x4vrmod") == 0 && b == a,
          "already enabled: untouched");

    std::string c = "<content>\n  <extension id=\"other\" enabled=\"true\"/>\n</content>\n";
    check(enable_extension(c, "x4vrmod") == 2 &&
              c.find("<extension id=\"x4vrmod\" enabled=\"true\"/>\n</content>") !=
                  std::string::npos,
          "absent: inserted before </content>");

    std::string d = "<content><extension id=\"x4vrmod\"/></content>";
    check(enable_extension(d, "x4vrmod") == 1 &&
              d.find("<extension id=\"x4vrmod\" enabled=\"true\"/>") !=
                  std::string::npos,
          "entry without the attribute gains enabled=\"true\"");

    // A prefix must not be mistaken for our id -- and ours is a prefix of the
    // ids it replaced, which a player's fork may still list.
    std::string e = "<content>\n  <extension id=\"x4vrmod_lookaround\" enabled=\"false\"/>\n</content>\n";
    check(enable_extension(e, "x4vrmod") == 2 &&
              e.find("x4vrmod_lookaround\" enabled=\"false\"") != std::string::npos,
          "an id that merely starts with ours is not touched");

    // OFF as well as on: X4 writes content.xml into our fork, so an extension
    // a session enabled must be forced back off in the next one.
    std::string g = "<content>\n  <extension id=\"x4vrmod_headlook\" enabled=\"true\"/>\n</content>\n";
    check(set_extension(g, "x4vrmod_headlook", false) == 1 &&
              g.find("<extension id=\"x4vrmod_headlook\" enabled=\"false\"/>") !=
                  std::string::npos,
          "set off: an enabled entry is flipped to false");
    std::string h = g;
    check(set_extension(h, "x4vrmod_headlook", false) == 0 && h == g,
          "set off: already off, untouched");
    std::string i = "<content>\n</content>\n";
    check(set_extension(i, "x4vrmod_headlook", false) == 2 &&
              i.find("<extension id=\"x4vrmod_headlook\" enabled=\"false\"/>") !=
                  std::string::npos,
          "set off: absent is inserted as false, not left to X4's default");

    // Retired ids (merged into x4vrmod): off when listed, never added.
    std::string j = g;
    j.replace(j.find("false"), 5, "true");
    check(retire_extension(j, "x4vrmod_headlook") == 1 && j == g,
          "retire: a listed, enabled id is flipped to false");
    check(retire_extension(j, "x4vrmod_headlook") == 0 && j == g,
          "retire: a listed, disabled id is untouched");
    std::string m = "<content>\n  <extension id=\"x4vrmod\" enabled=\"true\"/>\n"
                    "  <extension id=\"x4vrmod_nosway_x\" enabled=\"true\"/>\n</content>\n";
    const std::string m0 = m;
    check(retire_extension(m, "x4vrmod_nosway") == 3 && m == m0,
          "retire: an absent id is NOT added (nor a longer id touched)");

    std::string f = "garbage";
    check(enable_extension(f, "x4vrmod") == -1 && f == "garbage",
          "no </content>: refused, file untouched");

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
