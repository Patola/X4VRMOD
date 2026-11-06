// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_STEER, checked without X4: the mode knob, X4's own condition for its
// steering line (and the code it is read from, against the real binary), the
// nose against head-look's matrix convention, the B/A geometry, the line's
// quad, and the config fork's insert-and-restore of `mousesteeringline`.
// Self-checking; one case of each must-refuse kind is planted.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/x4vr_steer.hpp"
#include "../injector/x4vr_camwrite.hpp"
#include "../injector/x4vr_config.hpp"
#include "x4_path.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}
static bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

struct Elf {
    std::vector<uint8_t> d;
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        d.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        const bool ok = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        return ok && d.size() > 64;
    }
    bool read(uintptr_t va, void *out, size_t n) const {
        uint64_t phoff;
        uint16_t phentsize, phnum;
        memcpy(&phoff, &d[0x20], 8);
        memcpy(&phentsize, &d[0x36], 2);
        memcpy(&phnum, &d[0x38], 2);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint8_t *p = &d[phoff + (size_t)i * phentsize];
            uint32_t type;
            uint64_t off, vaddr, filesz;
            memcpy(&type, p, 4);
            memcpy(&off, p + 8, 8);
            memcpy(&vaddr, p + 16, 8);
            memcpy(&filesz, p + 32, 8);
            if (type == 1 && va >= vaddr && va + n <= vaddr + filesz) {
                memcpy(out, &d[off + (va - vaddr)], n);
                return true;
            }
        }
        return false;
    }
};

int main() {
    using namespace x4vr::steer;

    // ---- the knob ---------------------------------------------------------
    check(mode_from_env(nullptr) == kOff && mode_from_env("") == kOff &&
              mode_from_env("0") == kOff && mode_from_env("off") == kOff,
          "X4VR_STEER unset/empty/0/off -> X4's own line (every take before 331)");
    {
        // B was removed after take 339 (Patola): a b line gets A, and says so.
        bool b1 = false, b2 = false, b3 = false, a1 = true;
        check(mode_from_env("b", &b1) == kNose && mode_from_env("B", &b2) == kNose &&
                  mode_from_env("gaze", &b3) == kNose && b1 && b2 && b3 &&
                  mode_from_env("a", &a1) == kNose && !a1,
              "b / B / gaze -> A, flagged as a request for the removed B");
    }
    check(mode_from_env("a") == kNose && mode_from_env("A") == kNose &&
              mode_from_env("nose") == kNose,
          "a / A / nose -> A");
    check(mode_from_env("1") == kOff,
          "an unknown value is OFF, not a guess");

    // ---- X4's condition, as read from 0xa45cb0 -----------------------------
    {
        Flags f;
        check(!line_shown(f), "nothing set -> no line");
        f.active = 1;
        check(!line_shown(f), "steering active but 0x9f42 clear -> no line");
        f.f42 = 1;
        check(line_shown(f), "active and 0x9f42 -> line");
        f.f39 = 1;
        check(!line_shown(f), "0x9f39 set without 0xa010 -> no line");
        f.a010 = 1;
        check(line_shown(f), "0x9f39 set WITH 0xa010 -> line");
        Flags g;
        g.active2 = 1;
        g.f42 = 1;
        check(line_shown(g), "the second active byte (0x9f37) alone also counts");
    }

    // ---- the sites, against the real binary --------------------------------
    const char *path = x4_test_binary();
    Elf elf;
    if (elf.load(path)) {
        auto rd = [&](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
        uintptr_t bad = 0;
        check(verify_sites(rd, &bad), "all ShouldShowMouseSteeringHelperLine "
                                      "sites match X4's bytes");
        // Must refuse: the same reader with one byte of one site flipped.
        auto rd_bad = [&](uintptr_t va, void *o, size_t n) {
            if (!elf.read(va, o, n))
                return false;
            if (va == kSites[6].va)
                ((uint8_t *)o)[3] ^= 0x01; // 0x9f36 -> 0x9f37 in the cmpb
            return true;
        };
        bad = 0;
        check(!verify_sites(rd_bad, &bad) && bad == kSites[6].va,
              "a changed byte refuses, and names the site");
        char s[32] = {};
        check(elf.read(0x2c81035, s, 17) && !memcmp(s, "mousesteeringline", 17),
              "the option ShouldShow consults is \"mousesteeringline\" -- "
              "the config tag the fork serves");
        check(input_global_from_sites() == kInputGlobal &&
                  kInputGlobal == 0x3daaaf8,
              "the input object's global decodes to 0x3daaaf8");
        // ShouldShow's own load of the global must decode to the same place.
        int32_t d;
        memcpy(&d, kSites[5].bytes + 3, 4);
        check(kSites[5].va + 7 + (intptr_t)d == kInputGlobal,
              "ShouldShow reads the SAME global IsMouseSteeringActive does");
    } else {
        printf("SKIP  %s not readable — the site checks need X4\n", path);
    }

    // ---- the nose ----------------------------------------------------------
    const float t55 = std::tan(55.f * 0.01745329251994330f);
    {
        float x = 9, y = 9;
        check(nose_ndc(0, 0, t55, t55, &x, &y) && near(x, 0) && near(y, 0),
              "head straight: the nose is the eye's centre");
        check(nose_ndc(30, 0, t55, t55, &x, &y) &&
                  near(x, std::tan(30.f * 0.01745329251994330f) / t55) &&
                  near(y, 0),
              "head 30 deg LEFT: nose to the RIGHT by tan30/tan55");
        check(nose_ndc(0, 20, t55, t55, &x, &y) && near(x, 0) &&
                  near(y, std::tan(20.f * 0.01745329251994330f) / t55),
              "head 20 deg UP: nose BELOW (Vulkan y down) by tan20/tan55");
        check(!nose_ndc(120, 0, t55, t55, &x, &y),
              "head 120 deg round: the nose is behind, not projected");
        // The same vector as rows_for's matrix, row k's z component -- the
        // convention take 319 fitted to X4's own stored matrices.
        bool all = true;
        for (float yaw : {-150.f, -60.f, -20.f, 0.f, 15.f, 45.f, 80.f})
            for (float pitch : {-40.f, -5.f, 0.f, 10.f, 35.f}) {
                float rows[12] = {};
                camwrite::rows_for(yaw, pitch, rows);
                const float cx = rows[2], cy = rows[6], cz = rows[10];
                float nx = 0, ny = 0;
                const bool ok = nose_ndc(yaw, pitch, t55, t55, &nx, &ny);
                if (cz > 1e-4f) {
                    if (!ok || !near(nx, (cx / cz) / t55, 1e-4f) ||
                        !near(ny, -(cy / cz) / t55, 1e-4f))
                        all = false;
                } else if (ok) {
                    all = false;
                }
            }
        check(all, "nose_ndc == rows_for's forward axis, projected, over 35 "
                   "head poses");
    }

    // ---- the geometry ------------------------------------------------------
    const float W = 2816, H = 2816, gap = kLineGapPx;
    {
        // A with the nose at the eye's centre: from there (plus the gap) to
        // the pointer, the ring unmoved.
        const Geometry g = geometry(kNose, 0.2f, -0.1f, true, 0.f, 0.f, W, H, gap);
        const float len = std::hypot(0.2f, 0.1f) * 0.5f * W;
        check(g.used == kNose && g.ring_dx == 0 && g.ring_dy == 0 && g.line &&
                  near(g.x1, 0.2f) && near(g.y1, -0.1f) &&
                  near(g.x0, 0.2f * gap / len) && near(g.y0, -0.1f * gap / len),
              "A, nose at the centre: line from it (gap 16 px) to the ring");
        // A: from the nose to nose + offset; the ring moves with the nose.
        const Geometry a = geometry(kNose, 0.2f, -0.1f, true, 0.3f, 0.1f, W, H, gap);
        check(a.used == kNose && near(a.ring_dx, 0.3f) && near(a.ring_dy, 0.1f) &&
                  a.line && near(a.x1, 0.5f) && near(a.y1, 0.0f),
              "A: ring and line end at nose + offset");
        // A ALWAYS rides with the cockpit (Patola after take 332): a ring off
        // the eye stays in A -- simply not seen -- and with the nose behind
        // the eye nothing is drawn. Never B.
        const Geometry off = geometry(kNose, -0.1f, 0.f, true, -1.4f, 0.0f, W, H, gap);
        check(off.used == kNose && !off.hidden && near(off.ring_dx, -1.4f),
              "A, ring off the eye -> still A, at nose + offset (not seen)");
        const Geometry far = geometry(kNose, 0.2f, 0.f, true, -0.7f, 0.f, W, H, gap);
        check(far.used == kNose && near(far.ring_dx, -0.7f),
              "A, ring beyond what the headset shows (take 332's case) -> A");
        const Geometry behind = geometry(kNose, 0.2f, -0.1f, false, 0, 0, W, H, gap);
        check(behind.used == kNose && behind.hidden && !behind.line,
              "A, nose behind the eye (180 deg round) -> nothing drawn, not B");
        // Inside the gap: no line, as X4 hides its own inside 30 units.
        const Geometry in = geometry(kNose, 0.004f, 0.f, true, 0, 0, W, H, gap);
        check(!in.line, "pointer inside the 16 px gap -> no line");
        check(geometry(kOff, 0.2f, 0.f, true, 0, 0, W, H, gap).used == kOff &&
                  !geometry(kOff, 0.2f, 0.f, true, 0, 0, W, H, gap).line,
              "OFF draws nothing");

        float ft = 0;
        const Quad q = line_quad(g, W, H, 2.f, 1.f, &ft);
        // Corners: origin, origin+e1, origin+e2, origin+e1+e2; the centre line
        // of the quad must be the segment.
        const float mx0 = q.ox + 0.5f * q.e2x, my0 = q.oy + 0.5f * q.e2y;
        const float mx1 = mx0 + q.e1x, my1 = my0 + q.e1y;
        const float wpx = std::hypot(q.e2x * 0.5f * W, q.e2y * 0.5f * H);
        const float dot = q.e1x * 0.5f * W * q.e2x * 0.5f * W +
                          q.e1y * 0.5f * H * q.e2y * 0.5f * H;
        check(near(mx0, g.x0) && near(my0, g.y0) && near(mx1, g.x1) &&
                  near(my1, g.y1),
              "the quad's centre line IS the segment");
        check(near(wpx, 4.f, 1e-3f) && near(dot, 0.f, 1e-2f) && near(ft, 0.25f),
              "4 px across (2 core + 1+1 feather), square to the segment, "
              "feather 1/4");
    }

    // ---- the config fork: insert, replace, restore -------------------------
    {
        std::string xml = "<root>\n  <persistentmousesteering>true</persistentmousesteering>\n</root>\n";
        std::string old = "x";
        check(set_or_insert_tag(xml, "mousesteeringline", "false", &old) &&
                  old.empty() &&
                  get_tag(xml, "mousesteeringline") == "false" &&
                  xml.find("</root>") > xml.find("<mousesteeringline>"),
              "absent -> inserted before </root>, old value empty");
        check(!set_or_insert_tag(xml, "mousesteeringline", "false", &old),
              "already false -> no change");
        check(set_or_insert_tag(xml, "mousesteeringline", "true", &old) &&
                  old == "false" && get_tag(xml, "mousesteeringline") == "true",
              "present -> replaced, old value reported");
        std::string noroot = "<x/>";
        check(!set_or_insert_tag(noroot, "mousesteeringline", "false", &old) &&
                  noroot == "<x/>",
              "no </root> -> nothing invented");
        check(sidecar_path("/p/config-x4vrmod.xml", "mousesteeringline") ==
                  "/p/config-x4vrmod-mousesteeringline.was",
              "the sidecar sits beside the profile");
    }

    // <hudscale> off in VR (2026-10-31): absent in Patola's config.xml until
    // he set it in game, so it must be inserted, and any other value replaced.
    {
        std::string xml = "<root>\n  <uiscale>0.6</uiscale>\n</root>\n";
        std::string old = "x";
        check(serve_hud_scale_off(xml, &old) && old.empty() &&
                  get_tag(xml, "hudscale") == "off",
              "hudscale absent -> inserted as off");
        check(!serve_hud_scale_off(xml, &old), "hudscale already off -> no change");
        std::string all = "<root>\n  <hudscale>all</hudscale>\n</root>\n";
        check(serve_hud_scale_off(all, &old) && old == "all" &&
                  get_tag(all, "hudscale") == "off",
              "hudscale all -> off, the old value reported");
        check(get_tag(xml, "uiscale") == "0.6", "the player's uiscale is untouched");
    }

    // X4VR_AA takes X4 9.00's anti-aliasing values only: fsr_quality moved to
    // <upmode> in 9.00 and is rejected rather than served.
    {
        setenv("X4VR_AA", "fsr_quality", 1);
        std::string v = "unchanged";
        check(!aa_override(v) && v == "unchanged",
              "X4VR_AA=fsr_quality -> rejected (the upscaler is <upmode> in 9.00)");
    }

    // X4VR_UPSCALE: X4 9.00's ten <upmode> values, nothing else; the mod
    // serves it on every read, inserting it when absent, frame generation off.
    {
        int n = 0;
        for (const char *m : {"none", "fsr3_native", "fsr3_quality", "fsr3_balanced",
                              "fsr3_performance", "fsr3_ultra_performance",
                              "fsr_ultra_quality", "fsr_quality", "fsr_balanced",
                              "fsr_performance", "dlss_auto", "dlss_dlaa", "dlss_quality",
                              "dlss_balanced", "dlss_performance", "dlss_ultra_performance"})
            n += is_upscale_mode(m);
        check(n == 16, "the ten upmode values and six of DLSS are accepted");
        check(!is_upscale_mode("fsr") && !is_upscale_mode("fsr_easu") &&
                  !is_upscale_mode("FSR3_QUALITY") && !is_upscale_mode("") &&
                  !is_upscale_mode(nullptr),
              "anything else is refused (the old 'fsr', a shader name, case, empty)");
        std::string xml = "<root>\n  <antialiasing>none</antialiasing>\n  "
                          "<fsr3g>on</fsr3g>\n</root>\n";
        std::string old = "x";
        check(serve_upscaler(xml, "fsr3_quality", &old) && old.empty() &&
                  get_tag(xml, "upmode") == "fsr3_quality" &&
                  get_tag(xml, "fsr3g") == "off" && get_tag(xml, "dlssg") == "off",
              "upmode absent -> inserted; frame generation forced off");
        check(!serve_upscaler(xml, "fsr3_quality", &old) && old == "fsr3_quality",
              "already served -> no change");
        check(serve_upscaler(xml, "none", &old) && old == "fsr3_quality" &&
                  get_tag(xml, "upmode") == "none",
              "an in-game choice is replaced by the served value");
        // DLSS is two tags of X4's, and they follow the variable both ways: a player's flat
        // DLSS copied into the fork would upscale the left eye only.
        check(!is_upscale_mode("dlss") && !is_upscale_mode("dlss_") &&
                  !is_upscale_mode("dlss_off") && !dlss_mode_of("fsr_quality"),
              "only DLSS's own modes, and nothing else is one");
        std::string nv = "<root>\n  <dlss>true</dlss>\n  <dlssmode>dlaa</dlssmode>\n"
                         "  <upmode>fsr_quality</upmode>\n</root>\n";
        check(serve_upscaler(nv, "dlss_ultra_performance") && get_tag(nv, "dlss") == "true" &&
                  get_tag(nv, "dlssmode") == "ultra_performance" &&
                  get_tag(nv, "upmode") == "none" && get_tag(nv, "dlssg") == "off",
              "dlss_ultra_performance -> DLSS on in that mode, FSR off");
        check(serve_upscaler(nv, "none") && get_tag(nv, "dlss") == "false" &&
                  get_tag(nv, "dlssmode") == "off",
              "no DLSS asked for -> served off");
        std::string bare = "<root>\n</root>\n";
        check(serve_upscaler(bare, "fsr_quality") && get_tag(bare, "dlss") == "false" &&
                  get_tag(bare, "dlssmode") == "off" && get_tag(bare, "upmode") == "fsr_quality",
              "a sparse config gets all of it inserted");
        setenv("X4VR_UPSCALE", "fsr_quality", 1);
        int src = 0;
        check(!strcmp(upscale_value(&src), "fsr_quality") && src == 1,
              "X4VR_UPSCALE=fsr_quality -> served, from the variable");
    }

    printf("\n%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
