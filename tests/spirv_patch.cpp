// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Applies a shader patch to a SPIR-V file, so the transform can be checked
// without a GPU, a driver, or the layer in between.
//
// The render test proves the patched shader *does the right thing*; this
// proves the module it produces is well formed, which is a different question
// and the one spirv-val can answer. Both matter: a patch that samples the
// wrong layer still validates, and a patch that samples the right layer can
// still be rejected by the next driver if its module structure is only
// accidentally acceptable.
//
//     spirv_patch frag-view-layer <in.spv> <out.spv> <set> <binding>
//
// Prints PATCHED=1 or PATCHED=0. A refusal is a legitimate result -- most of
// this suite's cases are shaders the patch is *supposed* to leave alone -- so
// the exit status is 0 either way and the runner reads the key.
#include "../common/x4vr_spirv.hpp"
#include "../injector/x4vr_camwrite.hpp"
#include "spirv_interp.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<uint32_t> load_spv(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> v((size_t)n / 4);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n)
        v.clear();
    fclose(f);
    return v;
}

// Task #35: "l,r,u,d" in DEGREES -> the OffAxis the patch takes. Degrees
// because that is the unit OpenXR reports and the unit a run's command line is
// legible in; the tan() lives in frustum_of_angles, which is where it lives for
// the layer too, so a forgotten conversion cannot differ between them.
//
// Returns a non-ok OffAxis on a malformed string, and the patch refuses that
// rather than rendering symmetric while the caller believes otherwise.
static x4vr::OffAxis parse_off_axis(const char *s) {
    float a[4] = {0, 0, 0, 0};
    int n = 0;
    for (const char *p = s; *p && n < 4; n++) {
        char *end = nullptr;
        a[n] = strtof(p, &end);
        if (end == p)
            return {};
        p = end;
        if (*p == ',')
            p++;
    }
    if (n != 4)
        return {};
    const float r = 3.14159265358979f / 180.0f;
    return x4vr::make_off_axis(
        x4vr::frustum_of_angles(a[0] * r, a[1] * r, a[2] * r, a[3] * r));
}

// "yaw,pitch,roll" in DEGREES -- the same three numbers pose_set_euler takes,
// so a command line and the layer's X4VR_POSE_YAW cannot mean different things.
static bool parse_ypr(const char *s, float ypr[3]) {
    int n = 0;
    for (const char *p = s; *p && n < 3; n++) {
        char *end = nullptr;
        ypr[n] = strtof(p, &end);
        if (end == p)
            return false;
        p = end;
        if (*p == ',')
            p++;
    }
    return n == 3;
}

// An optional positional argument that is really absent rather than empty.
//
// The off-axis arguments sit BEFORE the pose ones, so reaching a pose set
// means passing something for them; an empty string parses to a non-ok OffAxis
// which the caller then hands over as a valid pointer, and the patch emits an
// affine built from zeroes. "-" says "not this one" out loud instead.
static bool arg_given(int argc, char **argv, int i) {
    return argc > i && argv[i][0] != '\0' && strcmp(argv[i], "-") != 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s frag-view-layer <in> <out> [set] [binding]\n"
                "       %s frag-index-offset <in> <out> [set] [binding] "
                "[offset]\n"
                "       %s frag-invproj <in> <out> [set] [binding] [member] "
                "[dl] [dr] [oa_l] [oa_r]\n"
                "       %s frag-invproj-check <in> [set] [binding] [member] "
                "[dl] [dr] [oa_l] [oa_r] [pose_set] [pose_binding] [ypr]\n"
                "       %s frag-force-uint <in> <out> <set> <binding> "
                "<byteoffset> <name> <value>\n"
                "       %s list <in>\n"
                "\n"
                "  oa_l / oa_r are off-axis frusta as \"l,r,u,d\" in DEGREES.\n"
                "  Pass \"-\" for either to check with no off-axis at all --\n"
                "  needed to reach the pose arguments past them.\n"
                "  ypr is the head rotation as \"yaw,pitch,roll\" in DEGREES,\n"
                "  and only means anything with a pose_set: it seeds the pose\n"
                "  block so task #61's R^-1 term has something to compute.\n"
                "  frag-invproj-check patches in memory and interprets the\n"
                "  emitted arithmetic against common/x4vr_view.hpp, printing\n"
                "  CHECKED=<n> WORST=<err>; it writes no file.\n"
                "\n"
                "  X4VR_CHECK_RINV_FLIP=1 composes the REFERENCE in the wrong\n"
                "  order (M.R^-1 instead of R^-1.M). That is the negative\n"
                "  control: a check that has never been seen to fail on this\n"
                "  arithmetic is worth no more than one that cannot see it.\n",
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    // `list` is what read X4's own shaders. It is the same code path the layer
    // logs through, run against a dumped module, so the instrument can be
    // checked against real bytes instead of only against shaders we wrote --
    // which is how it was caught claiming a bindless shader samples nothing.
    // The stage-agnostic coverage check. Kept separate from "list" because
    // the two answer different questions, and conflating them is what let a
    // compute shader sampling the bindless heap read as "declares nothing".
    if (strcmp(argv[1], "survey") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const uint32_t min_count = argc > 3 ? (uint32_t)atoi(argv[3]) : 32;
        auto s = x4vr::spv::survey_image_tables(code, min_count);
        printf("LARGE=%u FRAGMENT=%d COMPUTE=%d\n", s.large, (int)s.fragment,
               (int)s.compute);
        return 0;
    }

    // The eye offset read from X4's camera block instead of baked. Prints the
    // patched module so spirv-val can judge it, and PATCHED=0 when the module
    // has no camera block at (set, binding) -- a refusal the layer relies on
    // to fall back to the baked matrix rather than silently losing the shear.
    if (strcmp(argv[1], "vert-eye-offset") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const std::vector<uint32_t> before = code;
        const uint32_t set = argc > 4 ? (uint32_t)strtoul(argv[4], nullptr, 0) : 1;
        const uint32_t binding =
            argc > 5 ? (uint32_t)strtoul(argv[5], nullptr, 0) : 0;
        const uint32_t member =
            argc > 6 ? (uint32_t)strtoul(argv[6], nullptr, 0) : 1;
        const float dl = argc > 7 ? strtof(argv[7], nullptr) : -0.032f;
        const bool have_dr = argc > 8;
        const float dr = have_dr ? strtof(argv[8], nullptr) : 0.0f;
        const x4vr::OffAxis oal = argc > 9 ? parse_off_axis(argv[9])
                                           : x4vr::OffAxis{};
        const x4vr::OffAxis oar = argc > 10 ? parse_off_axis(argv[10])
                                            : x4vr::OffAxis{};
        // #77: argv[11] is the descriptor set carrying the roll (cos, sin).
        // Absent means unrolled, and the emitted words are then byte-identical
        // to the pre-#77 ones -- measured, not assumed: patching
        // sample_camera_block.vert.spv both ways gives 590 words either way and
        // `cmp` reports no difference.
        const uint32_t roll_set =
            arg_given(argc, argv, 11)
                ? (uint32_t)strtoul(argv[11], nullptr, 0)
                : 0xffffffffu;
        // "-" means ABSENT, not "an off-axis with default fields". The
        // arguments are positional, so reaching argv[11] means passing
        // something at 9 and 10 -- and presence alone used to turn the affine
        // on, which made the roll case impossible to invoke at all (it is
        // refused in combination, deliberately). Presence is not intent.
        const bool have_oal = argc > 9 && strcmp(argv[9], "-") != 0;
        const bool have_oar = argc > 10 && strcmp(argv[10], "-") != 0;
        const bool ok = x4vr::spv::patch_vertex_eye_offset(
            code, set, binding, member, dl, have_dr ? &dr : nullptr,
            have_oal ? &oal : nullptr, have_oar ? &oar : nullptr, roll_set, 0);
        printf("PATCHED=%d\n", ok ? 1 : 0);
        if (!ok && code != before) {
            printf("FAIL=refusal_modified_code\n");
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) {
            printf("FAIL=open_out\n");
            return 1;
        }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }

    // Task #23: the same eye offset for the modules that have no camera block,
    // recovering sx from the per-object M_worldviewprojection instead. Defaults
    // are the ones the layer uses -- set 3, binding 0, member 0.
    if (strcmp(argv[1], "vert-eye-offset-mvp") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const std::vector<uint32_t> before = code;
        const uint32_t set = argc > 4 ? (uint32_t)strtoul(argv[4], nullptr, 0) : 3;
        const uint32_t binding =
            argc > 5 ? (uint32_t)strtoul(argv[5], nullptr, 0) : 0;
        const uint32_t member =
            argc > 6 ? (uint32_t)strtoul(argv[6], nullptr, 0) : 0;
        const float dl = argc > 7 ? strtof(argv[7], nullptr) : -0.032f;
        const bool have_dr = argc > 8;
        const float dr = have_dr ? strtof(argv[8], nullptr) : 0.0f;
        const x4vr::OffAxis oal = argc > 9 ? parse_off_axis(argv[9])
                                           : x4vr::OffAxis{};
        const x4vr::OffAxis oar = argc > 10 ? parse_off_axis(argv[10])
                                            : x4vr::OffAxis{};
        // Take 397: argv[11] is the roll set, as for vert-eye-offset, and "-"
        // at 9 or 10 means absent so the roll case can be reached at all.
        const uint32_t roll_set =
            arg_given(argc, argv, 11)
                ? (uint32_t)strtoul(argv[11], nullptr, 0)
                : 0xffffffffu;
        const bool have_oal = argc > 9 && strcmp(argv[9], "-") != 0;
        const bool have_oar = argc > 10 && strcmp(argv[10], "-") != 0;
        const bool ok = x4vr::spv::patch_vertex_eye_offset_mvp(
            code, set, binding, member, dl, have_dr ? &dr : nullptr,
            have_oal ? &oal : nullptr, have_oar ? &oar : nullptr, roll_set, 0);
        printf("PATCHED=%d\n", ok ? 1 : 0);
        // A refusal that has already edited the module is worse than a wrong
        // patch: the caller falls back believing the code is untouched.
        if (!ok && code != before) {
            printf("FAIL=refusal_modified_code\n");
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) {
            printf("FAIL=open_out\n");
            return 1;
        }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }

    // Take 397: do the two vertex routes put the same vertex in the same place?
    //
    //     vert-eye-check <in.spv> [roll_set]
    //
    // <in.spv> must carry both a camera block at (1, 0) with M_projection as
    // member 1 and a set-3 block with M_worldviewprojection as member 0 --
    // sample_camera_block.vert.spv does -- so ONE module can be patched by
    // both routes and run on the same inputs: X4's projection convention
    // (row3 = [0 0 1 0], m[10] = 0, sy < 0), a rigid view, a uniform object
    // scale. Rolls, head shifts and forward offsets are swept for both eyes.
    //
    //   live    patch_vertex_eye_offset,     roll on  (the camera-block route)
    //   mvp     patch_vertex_eye_offset_mvp, roll on  (the fix)
    //   control patch_vertex_eye_offset_mvp, roll OFF (takes <= 396)
    //
    // Passes when live and mvp agree with each other and with the formula,
    // and the control DISAGREES with live -- the negative control that shows
    // the check can see the defect it was written for.
    if (strcmp(argv[1], "vert-eye-check") == 0) {
        const std::vector<uint32_t> orig = load_spv(argv[2]);
        if (orig.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const uint32_t rset = arg_given(argc, argv, 3)
                                  ? (uint32_t)strtoul(argv[3], nullptr, 0)
                                  : 31u;
        const float dl = -0.032f, dr = 0.032f;
        std::vector<uint32_t> live = orig, mvp = orig, ctrl = orig;
        if (!x4vr::spv::patch_vertex_eye_offset(live, 1, 0, 1, dl, &dr, nullptr,
                                                nullptr, rset, 0) ||
            !x4vr::spv::patch_vertex_eye_offset_mvp(mvp, 3, 0, 0, dl, &dr,
                                                    nullptr, nullptr, rset, 0) ||
            !x4vr::spv::patch_vertex_eye_offset_mvp(ctrl, 3, 0, 0, dl, &dr)) {
            printf("FAIL=patch_refused\n");
            return 1;
        }
        auto mat_mul = [](const float *a, const float *b, float *o) {
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) {
                    float s = 0.f;
                    for (int k = 0; k < 4; k++)
                        s += a[k * 4 + r] * b[c * 4 + k];
                    o[c * 4 + r] = s;
                }
        };
        auto put_mat = [](std::vector<uint8_t> &buf, size_t off, const float *m) {
            memcpy(&buf[off], m, 64);
        };
        const float sxy[][2] = {{0.7404f, -0.7404f}, {1.4917f, -1.2003f},
                                {29.18f, -29.18f}};
        const float rolls[] = {0.0f, 2.3f, -15.0f, 45.0f};
        const float shifts[][2] = {{0.f, 0.f}, {0.021f, -0.013f}};
        const float fwds[] = {0.0f, 0.11f};
        const float scales[] = {1.0f, 3.5f};
        // The headset's eye distance over the baked one (x4vr_view.hpp, pose_set_roll).
        const float ipds[] = {1.0f, 0.9828f, 1.1f};
        const float poss[][3] = {{0.3f, -0.2f, 0.5f}, {-1.1f, 0.7f, 2.0f}};
        int checked = 0, failed_runs = 0;
        double worst = 0, worst_ref = 0, ctrl_worst = 0;
        std::string why;
        for (const auto &s : sxy)
            for (float roll : rolls)
                for (const auto &sh : shifts)
                    for (float f : fwds)
                        for (float sc : scales)
                            for (float ipd : ipds)
                            for (const auto &p : poss)
                                for (int view = 0; view < 2; view++) {
            // P: X4's convention. VW: a rigid view, here yaw 20 pitch -10
            // roll 5 and a translation. S: uniform object scale.
            float P[16] = {}, VW[16] = {}, S[16] = {}, VWS[16], MVP[16];
            P[0] = s[0];
            P[5] = s[1];
            P[2 * 4 + 3] = 1.0f;
            P[3 * 4 + 2] = 0.1f;
            const float y = 20.f * 0.0174533f, x = -10.f * 0.0174533f,
                        z = 5.f * 0.0174533f;
            const float cy = cosf(y), sy = sinf(y), cx = cosf(x), sx_ = sinf(x),
                        cz = cosf(z), sz = sinf(z);
            // R = Ry * Rx * Rz, column-major.
            const float R[9] = {cy * cz + sy * sx_ * sz, cx * sz, -sy * cz + cy * sx_ * sz,
                                -cy * sz + sy * sx_ * cz, cx * cz, sy * sz + cy * sx_ * cz,
                                sy * cx, -sx_, cy * cx};
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                    VW[c * 4 + r] = R[c * 3 + r];
            VW[12] = 0.4f, VW[13] = -0.25f, VW[14] = 3.0f, VW[15] = 1.0f;
            S[0] = S[5] = S[10] = sc;
            S[15] = 1.0f;
            mat_mul(VW, S, VWS);
            mat_mul(P, VWS, MVP);
            const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            x4vr::PoseBlock pb = x4vr::pose_identity();
            const float phi = roll * 0.0174533f;
            x4vr::pose_set_roll(pb, roll, 1.0f, ipd);
            pb.eye_ofs[2] = sh[0];
            pb.eye_ofs[3] = sh[1];
            pb.head_fwd[0] = f;

            auto run = [&](const std::vector<uint32_t> &code, float out[4]) {
                x4vr::interp::Machine m;
                std::vector<uint8_t> cam(256), world(64), pose(sizeof pb);
                put_mat(cam, 0, I);
                put_mat(cam, 64, P);
                put_mat(cam, 128, I);
                put_mat(cam, 192, I);
                put_mat(world, 0, MVP);
                memcpy(pose.data(), &pb, sizeof pb);
                m.buffers[{1, 0}] = cam;
                m.buffers[{3, 0}] = world;
                m.buffers[{rset, 0}] = pose;
                m.input_location[0] = {p[0], p[1], p[2]};
                m.view_index = view;
                if (!m.run(code, 0 /* Vertex */)) {
                    why = m.error;
                    return false;
                }
                memcpy(out, m.position, sizeof m.position);
                return true;
            };
            float a[4], b[4], c[4];
            if (!run(live, a) || !run(mvp, b) || !run(ctrl, c)) {
                failed_runs++;
                continue;
            }
            // The formula, from the inputs alone.
            float ref[4];
            for (int r = 0; r < 4; r++)
                ref[r] = MVP[0 * 4 + r] * p[0] + MVP[1 * 4 + r] * p[1] +
                         MVP[2 * 4 + r] * p[2] + MVP[3 * 4 + r];
            const float d = view ? dr : dl;
            ref[0] -= s[0] * d * ipd * cosf(phi) + s[0] * sh[0];
            ref[1] -= s[1] * d * ipd * sinf(phi) + s[1] * sh[1];
            ref[3] -= f;
            double mag = 1.0;
            for (float v : ref)
                mag = std::max(mag, (double)fabsf(v));
            double e_ab = 0, e_ref = 0, e_ctrl = 0;
            for (int k = 0; k < 4; k++) {
                e_ab = std::max(e_ab, (double)fabsf(a[k] - b[k]) / mag);
                e_ref = std::max(e_ref, (double)std::max(fabsf(a[k] - ref[k]),
                                                         fabsf(b[k] - ref[k])) / mag);
                e_ctrl = std::max(e_ctrl, (double)fabsf(a[k] - c[k]) / mag);
            }
            worst = std::max(worst, e_ab);
            worst_ref = std::max(worst_ref, e_ref);
            ctrl_worst = std::max(ctrl_worst, e_ctrl);
            checked++;
        }
        const bool ok = !failed_runs && checked > 0 && worst <= 2e-5 &&
                        worst_ref <= 2e-5 && ctrl_worst > 1e-3;
        printf("CHECKED=%d WORST=%.3g WORST_REF=%.3g CONTROL_WORST=%.3g%s%s\n",
               checked, worst, worst_ref, ctrl_worst,
               failed_runs ? " RUN_FAILED=" : "", failed_runs ? why.c_str() : "");
        printf("%s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    // Take 399: does the fragment reconstruction undo what the vertex stage
    // did, slot and all?
    //
    //     eye-roundtrip-check <vert.spv> <frag.spv> [roll_set]
    //
    // A view-space point goes through the patched vertex stage (camera-block
    // route, sample_camera_block.vert.spv with M_worldviewprojection = P) to
    // the eye's clip position, then through the patched M_invprojection
    // (sample_invproj_clip.frag.spv) back to view space; it must come back to
    // where it started, for rolls, head shifts, forward offsets and both eyes.
    // Two negative controls: the fragment patched with no slot at all, and the
    // fragment fed head_fwd = 0 while the vertex stage had it -- the
    // reconstruction as it was until take 399. Both must miss.
    if (strcmp(argv[1], "eye-roundtrip-check") == 0 && argc >= 4) {
        const std::vector<uint32_t> vorig = load_spv(argv[2]);
        const std::vector<uint32_t> forig = load_spv(argv[3]);
        if (vorig.empty() || forig.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const uint32_t rset = arg_given(argc, argv, 4)
                                  ? (uint32_t)strtoul(argv[4], nullptr, 0)
                                  : 31u;
        const float dl = -0.032f, dr = 0.032f;
        std::vector<uint32_t> vert = vorig, frag = forig, frag_plain = forig;
        if (!x4vr::spv::patch_vertex_eye_offset(vert, 1, 0, 1, dl, &dr, nullptr,
                                                nullptr, rset, 0) ||
            !x4vr::spv::patch_fragment_invproj_eye(frag, 1, 0, 2, dl, dr,
                                                   nullptr, nullptr, 0xffffffffu,
                                                   0, rset, 0) ||
            !x4vr::spv::patch_fragment_invproj_eye(frag_plain, 1, 0, 2, dl, dr)) {
            printf("FAIL=patch_refused\n");
            return 1;
        }
        // 4x4 inverse, Gauss-Jordan in double, column-major in and out.
        auto invert = [](const float *m, float *o) {
            double a[4][8];
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++) {
                    a[r][c] = m[c * 4 + r];
                    a[r][c + 4] = r == c;
                }
            for (int c = 0; c < 4; c++) {
                int piv = c;
                for (int r = c + 1; r < 4; r++)
                    if (fabs(a[r][c]) > fabs(a[piv][c]))
                        piv = r;
                for (int k = 0; k < 8; k++)
                    std::swap(a[c][k], a[piv][k]);
                const double div = a[c][c];
                for (int k = 0; k < 8; k++)
                    a[c][k] /= div;
                for (int r = 0; r < 4; r++)
                    if (r != c) {
                        const double f = a[r][c];
                        for (int k = 0; k < 8; k++)
                            a[r][k] -= f * a[c][k];
                    }
            }
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    o[c * 4 + r] = (float)a[r][c + 4];
        };
        const float sxy[][2] = {{0.7404f, -0.7404f}, {1.4917f, -1.2003f}};
        const float rolls[] = {0.0f, 2.3f, -15.0f, 45.0f};
        const float shifts[][2] = {{0.f, 0.f}, {0.021f, -0.013f}};
        const float fwds[] = {0.0f, 0.11f};
        const float pts[][3] = {{0.3f, -0.2f, 0.9f}, {-1.1f, 0.7f, 4.0f},
                                {2.5f, 1.5f, 12.0f}};
        int checked = 0, failed_runs = 0;
        double worst = 0, ctrl_plain = 0, ctrl_nofwd = 0;
        std::string why;
        for (const auto &s : sxy)
            for (float roll : rolls)
                for (const auto &sh : shifts)
                    for (float f : fwds)
                        for (float ipd : {1.0f, 0.9828f, 1.1f})
                        for (const auto &v : pts)
                            for (int view = 0; view < 2; view++) {
            float P[16] = {}, Pinv[16];
            P[0] = s[0];
            P[5] = s[1];
            P[2 * 4 + 3] = 1.0f; // row3(P) = [0 0 1 0]
            P[3 * 4 + 2] = 0.1f; // reversed-Z near
            invert(P, Pinv);
            const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            x4vr::PoseBlock pb = x4vr::pose_identity();
            x4vr::pose_set_roll(pb, roll, 1.0f, ipd);
            pb.eye_ofs[2] = sh[0];
            pb.eye_ofs[3] = sh[1];
            pb.head_fwd[0] = f;
            auto buffers = [&](x4vr::interp::Machine &m, const x4vr::PoseBlock &q) {
                std::vector<uint8_t> cam(256), world(64), pose(sizeof q);
                memcpy(&cam[0], I, 64);
                memcpy(&cam[64], P, 64);
                memcpy(&cam[128], Pinv, 64);
                memcpy(&cam[192], I, 64);
                memcpy(world.data(), P, 64); // M_worldviewprojection = P
                memcpy(pose.data(), &q, sizeof q);
                m.buffers[{1, 0}] = cam;
                m.buffers[{3, 0}] = world;
                m.buffers[{rset, 0}] = pose;
                m.view_index = view;
            };
            x4vr::interp::Machine mv;
            buffers(mv, pb);
            mv.input_location[0] = {v[0], v[1], v[2]};
            if (!mv.run(vert, 0)) {
                why = mv.error;
                failed_runs++;
                continue;
            }
            const std::vector<float> clip(mv.position, mv.position + 4);
            auto back = [&](const std::vector<uint32_t> &code,
                            const x4vr::PoseBlock &q, float out[3]) {
                x4vr::interp::Machine mf;
                buffers(mf, q);
                mf.input_location[0] = clip;
                if (!mf.run(code, 4 /* Fragment */) || !mf.outputs.count(0)) {
                    why = mf.error.empty() ? "no output 0" : mf.error;
                    return false;
                }
                for (int k = 0; k < 3; k++)
                    out[k] = mf.outputs[0][k];
                return true;
            };
            x4vr::PoseBlock nofwd = pb;
            nofwd.head_fwd[0] = 0.0f;
            float r_fix[3], r_plain[3], r_nofwd[3];
            if (!back(frag, pb, r_fix) || !back(frag_plain, pb, r_plain) ||
                !back(frag, nofwd, r_nofwd)) {
                failed_runs++;
                continue;
            }
            const double mag = std::max(1.0, (double)sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            double e = 0, ep = 0, en = 0;
            for (int k = 0; k < 3; k++) {
                e = std::max(e, (double)fabsf(r_fix[k] - v[k]) / mag);
                ep = std::max(ep, (double)fabsf(r_plain[k] - v[k]));
                if (f != 0.0f)
                    en = std::max(en, (double)fabsf(r_nofwd[k] - v[k]));
            }
            worst = std::max(worst, e);
            if (roll != 0.0f || sh[0] != 0.0f || f != 0.0f || ipd != 1.0f)
                ctrl_plain = std::max(ctrl_plain, ep);
            ctrl_nofwd = std::max(ctrl_nofwd, en);
            checked++;
        }
        const bool ok = !failed_runs && checked > 0 && worst <= 2e-5 &&
                        ctrl_plain > 1e-3 && ctrl_nofwd > 1e-3;
        printf("CHECKED=%d WORST=%.3g CONTROL_NO_SLOT=%.3g m CONTROL_NO_FWD=%.3g m%s%s\n",
               checked, worst, ctrl_plain, ctrl_nofwd,
               failed_runs ? " RUN_FAILED=" : "", failed_runs ? why.c_str() : "");
        printf("%s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    // Does the emitted SPIR-V compute what common/x4vr_view.hpp says it does?
    //
    // spirv-val proves the module is well formed, and a well-formed module
    // that extracts (1,0) where it meant (1,1), or subtracts its operands the
    // wrong way round, validates perfectly and renders wrong. Every failure
    // this project has spent a take on is in that gap, so the gap gets its own
    // instrument: interpret the straight-line float arithmetic the patch
    // emitted, with a known M_invprojection in the uniform block and a known
    // gl_ViewIndex, and demand the same 16 floats compose_inv_off_axis and
    // compose_eye_untranslate produce for the same input.
    //
    // Both sides come from the same header, so there is no second copy of the
    // arithmetic to drift -- what is being compared is the EMISSION against
    // the definition, which is the only thing in doubt.
    //
    //   frag-invproj-check <in.spv> <set> <binding> <member> <dl> <dr> [l] [r]
    if (strcmp(argv[1], "frag-invproj-check") == 0) {
        const std::vector<uint32_t> orig_code = load_spv(argv[2]);
        if (orig_code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const uint32_t set = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0) : 1;
        const uint32_t binding =
            argc > 4 ? (uint32_t)strtoul(argv[4], nullptr, 0) : 0;
        const uint32_t member =
            argc > 5 ? (uint32_t)strtoul(argv[5], nullptr, 0) : 2;
        const float dl = argc > 6 ? strtof(argv[6], nullptr) : -0.032f;
        const float dr = argc > 7 ? strtof(argv[7], nullptr) : 0.032f;
        const bool have_l = arg_given(argc, argv, 8);
        const bool have_r = arg_given(argc, argv, 9);
        const x4vr::OffAxis oal = have_l ? parse_off_axis(argv[8])
                                         : x4vr::OffAxis{};
        const x4vr::OffAxis oar = have_r ? parse_off_axis(argv[9])
                                         : x4vr::OffAxis{};

        // Task #61's R^-1. Absent by default, which is what every call before
        // this argument existed was measuring -- and the reason none of them
        // said anything about the rotation term.
        const bool pose_on = arg_given(argc, argv, 10);
        const uint32_t pose_set =
            pose_on ? (uint32_t)strtoul(argv[10], nullptr, 0) : 0xffffffffu;
        const uint32_t pose_binding =
            arg_given(argc, argv, 11) ? (uint32_t)strtoul(argv[11], nullptr, 0)
                                      : 0;
        float ypr[3] = {10.0f, 0.0f, 0.0f};
        if (arg_given(argc, argv, 12) && !parse_ypr(argv[12], ypr)) {
            printf("FAIL=bad_ypr\n");
            return 1;
        }
        // An identity rotation makes R^-1 commute with everything, so the
        // wrong composition order would agree with the right one and the
        // check would pass while proving nothing. Refuse it rather than
        // report a zero that means "not asked".
        if (pose_on && ypr[0] == 0.0f && ypr[1] == 0.0f && ypr[2] == 0.0f) {
            printf("FAIL=identity_rotation_proves_nothing\n");
            return 1;
        }
        x4vr::PoseBlock pose = x4vr::pose_identity();
        if (pose_on)
            x4vr::pose_set_euler(pose, ypr[0], ypr[1], ypr[2]);
        // The negative control, kept in the binary rather than performed once
        // by hand: composing the reference the other way round must make this
        // check go red, and the suite asserts that it does.
        const char *flip_env = getenv("X4VR_CHECK_RINV_FLIP");
        const bool flip = pose_on && flip_env && *flip_env && *flip_env != '0';

        // The load the patch redefines. Taken from the UNPATCHED module,
        // because the patch preserves the result id precisely so downstream
        // uses need no rewriting -- that identity is what makes this findable.
        uint32_t want_id = 0, cam_ptr = 0;
        {
            std::vector<x4vr::spv::Inst> insts;
            x4vr::spv::iterate(orig_code, insts);
            std::vector<uint32_t> chains;
            std::vector<uint32_t> cam_vars;
            std::vector<std::pair<uint32_t, uint32_t>> dset, dbind;
            std::vector<std::pair<uint32_t, uint32_t>> iconst;
            for (const auto &in : insts) {
                const uint32_t *w = &orig_code[in.start];
                if (in.op == x4vr::spv::OpDecorate && in.len >= 4) {
                    if (w[2] == x4vr::spv::DecorationDescriptorSet)
                        dset.push_back({w[1], w[3]});
                    if (w[2] == x4vr::spv::DecorationBinding)
                        dbind.push_back({w[1], w[3]});
                }
                if (in.op == x4vr::spv::OpConstant && in.len >= 4)
                    iconst.push_back({w[2], w[3]});
            }
            for (const auto &s : dset)
                for (const auto &b : dbind)
                    if (s.first == b.first && s.second == set &&
                        b.second == binding)
                        cam_vars.push_back(s.first);
            for (const auto &in : insts) {
                const uint32_t *w = &orig_code[in.start];
                if (in.op != x4vr::spv::OpAccessChain || in.len != 5)
                    continue;
                bool is_cam = false;
                for (uint32_t v : cam_vars)
                    is_cam |= (w[3] == v);
                if (!is_cam)
                    continue;
                for (const auto &c : iconst)
                    if (c.first == w[4] && c.second == member)
                        chains.push_back(w[2]);
            }
            for (const auto &in : insts) {
                const uint32_t *w = &orig_code[in.start];
                if (in.op != x4vr::spv::OpLoad || in.len < 4)
                    continue;
                for (uint32_t c : chains)
                    if (w[3] == c) {
                        want_id = w[2];
                        cam_ptr = w[3];
                    }
            }
        }
        if (!want_id) {
            printf("FAIL=no_load_to_check\n");
            return 1;
        }

        std::vector<uint32_t> code = orig_code;
        if (!x4vr::spv::patch_fragment_invproj_eye(
                code, set, binding, member, dl, dr,
                have_l ? &oal : nullptr, have_r ? &oar : nullptr, pose_set,
                pose_binding)) {
            printf("FAIL=patch_refused\n");
            return 1;
        }

        // Where the emitted R^-1 is read from, found in the PATCHED module the
        // same way cam_ptr was found in the original: the pose block is new
        // code, so there is nothing in the input to look for.
        //
        // One pointer per patch site, and a module with two loads of
        // M_invprojection gets two -- collecting only the last would seed
        // whichever site want_id did not come from, and score a zero for the
        // wrong reason.
        std::vector<uint32_t> pose_ptrs;
        float pose_rinv[16] = {};
        uint32_t pose_off = 0, pose_stride = 0;
        bool pose_colmajor = true;
        if (pose_on) {
            std::vector<x4vr::spv::Inst> insts;
            x4vr::spv::iterate(code, insts);
            std::vector<std::pair<uint32_t, uint32_t>> dset, dbind;
            std::vector<std::pair<uint32_t, uint32_t>> var_ptr; // var -> ptrtype
            std::vector<std::pair<uint32_t, uint32_t>> ptr_to;  // ptrtype -> to
            for (const auto &in : insts) {
                const uint32_t *w = &code[in.start];
                if (in.op == x4vr::spv::OpDecorate && in.len >= 4) {
                    if (w[2] == x4vr::spv::DecorationDescriptorSet)
                        dset.push_back({w[1], w[3]});
                    if (w[2] == x4vr::spv::DecorationBinding)
                        dbind.push_back({w[1], w[3]});
                }
                if (in.op == x4vr::spv::OpVariable && in.len >= 4)
                    var_ptr.push_back({w[2], w[1]});
                if (in.op == x4vr::spv::OpTypePointer && in.len >= 4)
                    ptr_to.push_back({w[1], w[3]});
            }
            std::vector<uint32_t> pvars;
            for (const auto &s : dset)
                for (const auto &b : dbind)
                    if (s.first == b.first && s.second == pose_set &&
                        b.second == pose_binding)
                        pvars.push_back(s.first);
            // Exactly one. Two means the module already had a descriptor where
            // the layer puts the pose block, and everything below would then
            // be checking arithmetic the GPU will not perform.
            if (pvars.size() != 1) {
                printf("FAIL=pose_binding_occupied n=%zu set=%u binding=%u\n",
                       pvars.size(), pose_set, pose_binding);
                return 1;
            }
            for (const auto &in : insts) {
                const uint32_t *w = &code[in.start];
                if (in.op == x4vr::spv::OpAccessChain && in.len == 5 &&
                    w[3] == pvars[0])
                    pose_ptrs.push_back(w[2]);
            }
            if (pose_ptrs.empty()) {
                printf("FAIL=no_pose_chain\n");
                return 1;
            }

            // The block's std140 layout, READ rather than assumed.
            //
            // patch_fragment_invproj_eye's own comment says a mismatch here
            // "reads back as a plausible wrong matrix, not as an error" -- so
            // an instrument that seeds pose.r_inv directly would agree with
            // the emission no matter what offset, stride or majorness the
            // module declared. Decoding PoseBlock's bytes through the
            // module's decorations is what makes those three checkable: a
            // module that said offset 0 would hand back r, and one that said
            // RowMajor would hand back R instead of R^-1, and both come out
            // as a nonzero WORST rather than as a silent pass.
            uint32_t block_ty = 0;
            for (const auto &v : var_ptr)
                if (v.first == pvars[0])
                    for (const auto &p : ptr_to)
                        if (p.first == v.second)
                            block_ty = p.second;
            if (!block_ty) {
                printf("FAIL=no_pose_block_type\n");
                return 1;
            }
            constexpr uint32_t kDecorationRowMajor = 4;
            bool have_off = false, have_stride = false, have_major = false;
            for (const auto &in : insts) {
                const uint32_t *w = &code[in.start];
                if (in.op != x4vr::spv::OpMemberDecorate || in.len < 4)
                    continue;
                if (w[1] != block_ty || w[2] != 1) // member 1 is r_inv
                    continue;
                if (w[3] == x4vr::spv::DecorationOffset && in.len >= 5) {
                    pose_off = w[4];
                    have_off = true;
                } else if (w[3] == x4vr::spv::DecorationMatrixStride &&
                           in.len >= 5) {
                    pose_stride = w[4];
                    have_stride = true;
                } else if (w[3] == x4vr::spv::DecorationColMajor) {
                    pose_colmajor = true;
                    have_major = true;
                } else if (w[3] == kDecorationRowMajor) {
                    pose_colmajor = false;
                    have_major = true;
                }
            }
            if (!have_off || !have_stride || !have_major) {
                printf("FAIL=pose_layout_undeclared off=%d stride=%d major=%d\n",
                       (int)have_off, (int)have_stride, (int)have_major);
                return 1;
            }
            // The second negative control: force the other majorness and the
            // decode must disagree with the reference. A layout reader that
            // has never been seen to read the wrong thing is an assumption
            // wearing an instrument's clothes.
            const char *rm = getenv("X4VR_CHECK_RINV_ROWMAJOR");
            if (rm && *rm && *rm != '0')
                pose_colmajor = !pose_colmajor;
            if ((size_t)pose_off + 3 * (size_t)pose_stride + 16 >
                sizeof(x4vr::PoseBlock)) {
                printf("FAIL=pose_layout_out_of_range off=%u stride=%u\n",
                       pose_off, pose_stride);
                return 1;
            }
            const unsigned char *raw =
                reinterpret_cast<const unsigned char *>(&pose);
            for (uint32_t c = 0; c < 4; c++)
                for (uint32_t r = 0; r < 4; r++) {
                    const size_t at =
                        pose_off + (pose_colmajor ? c * pose_stride + r * 4
                                                  : r * pose_stride + c * 4);
                    memcpy(&pose_rinv[c * 4 + r], raw + at, 4);
                }
        }
        auto is_pose_ptr = [&](uint32_t id) {
            for (uint32_t p : pose_ptrs)
                if (p == id)
                    return true;
            return false;
        };

        // A tagged value, because the emitted graph mixes scalars, a bool and
        // the matrix itself; an untyped float array would silently accept an
        // index applied to the wrong kind.
        struct Val {
            enum K { None, F, I, B, M } k = None;
            float f = 0.0f;
            int32_t i = 0;
            bool b = false;
            float m[16] = {};
        };

        // Five cameras, the same grid tests/view_math.cpp uses, so a case that
        // only works at 1:1 cannot pass here either.
        const float cam[][2] = {{0.70021f, 1.0f},
                                {0.75405f, 1.0f},
                                {1.33333f, 1.0f},
                                {3.78085f, 1.5f},
                                {29.18689f, 1.0f}};
        float worst = 0.0f;
        int checked = 0;
        for (const auto &c : cam) {
            // X4's projection, inverted -- the matrix the block really holds.
            x4vr::Mat4 p{};
            p.m[0] = c[0];
            p.m[5] = -c[0] * c[1];
            p.m[11] = 1.0f;
            p.m[14] = 0.1f;
            x4vr::Mat4 pi{};
            if (!x4vr::invert(p, pi))
                continue;
            for (int view = 0; view < 2; view++) {
                // what the header says the answer is
                float want[16];
                memcpy(want, pi.m, sizeof(want));
                const x4vr::OffAxis *o = view ? (have_r ? &oar : &oal)
                                              : (have_l ? &oal : nullptr);
                if (o)
                    x4vr::compose_inv_off_axis(*o, want);
                x4vr::compose_eye_untranslate(view ? dr : dl, want);
                // Task #61 left-multiplies: `emit(OpMatrixTimesMatrix, {t,
                // orig, rinv, acc})` makes rinv the LeftMatrix, and SPIR-V's
                // Left x Right applies Right first. So R^-1 lands on the
                // result of everything above, which is what puts the
                // reconstruction back in X4's own frame.
                //
                // Getting this round the wrong way would make the check agree
                // with a wrong emission, so the wrong way is available on
                // purpose and the suite runs it.
                if (pose_on) {
                    const x4vr::Mat4 a = x4vr::load(want);
                    const x4vr::Mat4 ri = x4vr::load(pose.r_inv);
                    x4vr::store(want, flip
                                          ? x4vr::mul(a, ri,
                                                      x4vr::Major::Column)
                                          : x4vr::mul(ri, a,
                                                      x4vr::Major::Column));
                }

                // what the module computes
                std::vector<Val> reg((size_t)code[3]);
                auto set_f = [&](uint32_t id, float v) {
                    reg[id].k = Val::F;
                    reg[id].f = v;
                };
                std::vector<x4vr::spv::Inst> insts;
                x4vr::spv::iterate(code, insts);
                uint32_t view_var = 0;
                for (const auto &in : insts) {
                    const uint32_t *w = &code[in.start];
                    if (in.op == x4vr::spv::OpDecorate && in.len >= 4 &&
                        w[2] == x4vr::spv::DecorationBuiltIn &&
                        w[3] == x4vr::spv::BuiltInViewIndex)
                        view_var = w[1];
                }
                bool done = false;
                for (const auto &in : insts) {
                    if (done)
                        break;
                    const uint32_t *w = &code[in.start];
                    auto R = [&](uint32_t n) -> Val & { return reg[w[n]]; };
                    switch (in.op) {
                    case x4vr::spv::OpConstant:
                        if (in.len >= 4) {
                            // Float or int is decided by the type id, and both
                            // are needed: the view index is an int constant in
                            // some modules and the coefficients are floats.
                            float f;
                            memcpy(&f, &w[3], 4);
                            reg[w[2]].k = Val::F;
                            reg[w[2]].f = f;
                            reg[w[2]].i = (int32_t)w[3];
                        }
                        break;
                    case x4vr::spv::OpLoad:
                        if (in.len >= 4 && w[3] == cam_ptr) {
                            reg[w[2]].k = Val::M;
                            memcpy(reg[w[2]].m, pi.m, sizeof(pi.m));
                        } else if (in.len >= 4 && is_pose_ptr(w[3])) {
                            // Member 1 of the pose block, decoded from
                            // PoseBlock's raw bytes through the offset,
                            // stride and majorness the module declared --
                            // NOT copied from pose.r_inv, which would agree
                            // with any layout at all.
                            reg[w[2]].k = Val::M;
                            memcpy(reg[w[2]].m, pose_rinv, sizeof(pose_rinv));
                        } else if (in.len >= 4 && w[3] == view_var) {
                            reg[w[2]].k = Val::I;
                            reg[w[2]].i = view;
                        }
                        break;
                    case x4vr::spv::OpMatrixTimesMatrix:
                        // Left x Right, and Right is applied first. x4vr::mul
                        // is used rather than a second hand-written product:
                        // what is in doubt is which operand SPIR-V applies
                        // first, and a fresh loop here could carry a
                        // transposition that cancels the very error the
                        // reference exists to catch.
                        if (in.len >= 5 && R(3).k == Val::M &&
                            R(4).k == Val::M) {
                            const x4vr::Mat4 pr =
                                x4vr::mul(x4vr::load(R(3).m),
                                          x4vr::load(R(4).m),
                                          x4vr::Major::Column);
                            reg[w[2]].k = Val::M;
                            memcpy(reg[w[2]].m, pr.m, sizeof(pr.m));
                        }
                        break;
                    case x4vr::spv::OpConvertSToF:
                        if (in.len >= 4 && R(3).k == Val::I)
                            set_f(w[2], (float)R(3).i);
                        break;
                    case x4vr::spv::OpFMul:
                        if (in.len >= 5 && R(3).k == Val::F && R(4).k == Val::F)
                            set_f(w[2], R(3).f * R(4).f);
                        break;
                    case x4vr::spv::OpFAdd:
                        if (in.len >= 5 && R(3).k == Val::F && R(4).k == Val::F)
                            set_f(w[2], R(3).f + R(4).f);
                        break;
                    case x4vr::spv::OpFSub:
                        if (in.len >= 5 && R(3).k == Val::F && R(4).k == Val::F)
                            set_f(w[2], R(3).f - R(4).f);
                        break;
                    case x4vr::spv::OpFDiv:
                        if (in.len >= 5 && R(3).k == Val::F && R(4).k == Val::F)
                            set_f(w[2], R(3).f / R(4).f);
                        break;
                    case x4vr::spv::OpFOrdGreaterThan:
                        if (in.len >= 5 && R(3).k == Val::F &&
                            R(4).k == Val::F) {
                            reg[w[2]].k = Val::B;
                            reg[w[2]].b = R(3).f > R(4).f;
                        }
                        break;
                    case x4vr::spv::OpLogicalAnd:
                        if (in.len >= 5 && R(3).k == Val::B &&
                            R(4).k == Val::B) {
                            reg[w[2]].k = Val::B;
                            reg[w[2]].b = R(3).b && R(4).b;
                        }
                        break;
                    case x4vr::spv::OpSelect:
                        if (in.len >= 6 && R(3).k == Val::B &&
                            R(4).k == Val::F && R(5).k == Val::F)
                            set_f(w[2], R(3).b ? R(4).f : R(5).f);
                        break;
                    case x4vr::spv::OpCompositeExtract:
                        if (in.len >= 6 && R(3).k == Val::M)
                            set_f(w[2], R(3).m[w[4] * 4 + w[5]]);
                        break;
                    case x4vr::spv::OpCompositeInsert:
                        if (in.len >= 7 && R(3).k == Val::F &&
                            R(4).k == Val::M) {
                            reg[w[2]].k = Val::M;
                            memcpy(reg[w[2]].m, R(4).m, sizeof(R(4).m));
                            reg[w[2]].m[w[5] * 4 + w[6]] = R(3).f;
                        }
                        break;
                    default:
                        break;
                    }
                    // Which opcode redefines want_id depends on the pose: with
                    // one, the four column inserts all take fresh ids and the
                    // OpMatrixTimesMatrix takes over the original, because
                    // R^-1 has to be applied last and whatever runs last is
                    // what downstream uses must see. Watching only for the
                    // insert would run past the end of the patch and score
                    // whatever the shader's own code left behind.
                    if (in.len >= 3 && w[2] == want_id &&
                        (in.op == x4vr::spv::OpCompositeInsert ||
                         in.op == x4vr::spv::OpMatrixTimesMatrix))
                        done = true;
                }
                if (reg[want_id].k != Val::M) {
                    // The patch's last write to want_id is always a matrix --
                    // an insert without the pose, a product with it -- so this
                    // can only mean the graph was not followed. Report it
                    // rather than scoring a zero.
                    printf("FAIL=unevaluated view=%d sx=%.5f\n", view, c[0]);
                    return 1;
                }
                for (int i = 0; i < 16; i++) {
                    const float e = fabsf(reg[want_id].m[i] - want[i]);
                    if (e > worst)
                        worst = e;
                }
                checked++;
            }
        }
        // The result line first, because two callers already anchor on
        // ^CHECKED= and .*WORST=. What follows says what was checked: every
        // `CHECKED=10 WORST=0.000e+00` printed before this argument existed
        // was a run with POSE=off, and none of them said anything about R^-1.
        printf("CHECKED=%d WORST=%.3e\n", checked, worst);
        if (pose_on)
            printf("POSE=set%u/%u ypr=%g,%g,%g ORDER=%s LAYOUT=off%u/stride%u/%s\n",
                   pose_set, pose_binding, ypr[0], ypr[1], ypr[2],
                   flip ? "M*Rinv(FLIPPED)" : "Rinv*M", pose_off, pose_stride,
                   pose_colmajor ? "col" : "row");
        else
            printf("POSE=off\n");
        return 0;
    }

    // The per-eye M_invprojection correction (task #22).
    // frag-invproj-pose <in> <out> [set] [binding] [member] [dl] [dr]
    // -- task #61: the same patch with R^-1 composed from the pose block.
    if (strcmp(argv[1], "frag-invproj-pose") == 0 && argc >= 4) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) { printf("FAIL=load\n"); return 1; }
        const uint32_t st = argc > 4 ? (uint32_t)atoi(argv[4]) : 1;
        const uint32_t bi = argc > 5 ? (uint32_t)atoi(argv[5]) : 0;
        const uint32_t me = argc > 6 ? (uint32_t)atoi(argv[6]) : 2;
        const float dl = argc > 7 ? strtof(argv[7], nullptr) : -0.032f;
        const float dr = argc > 8 ? strtof(argv[8], nullptr) : 0.032f;
        if (!x4vr::spv::patch_fragment_invproj_eye(code, st, bi, me, dl, dr,
                                                   nullptr, nullptr, 5, 0)) {
            printf("REFUSED=1\n");
            return 0;
        }
        printf("REFUSED=0 WORDS=%zu\n", code.size());
        FILE *f = fopen(argv[3], "wb");
        if (!f) { printf("FAIL=open\n"); return 1; }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }
    if (strcmp(argv[1], "frag-invproj") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const std::vector<uint32_t> before = code;
        const uint32_t set = argc > 4 ? (uint32_t)strtoul(argv[4], nullptr, 0) : 1;
        const uint32_t binding =
            argc > 5 ? (uint32_t)strtoul(argv[5], nullptr, 0) : 0;
        const uint32_t member =
            argc > 6 ? (uint32_t)strtoul(argv[6], nullptr, 0) : 2;
        const float dl = argc > 7 ? strtof(argv[7], nullptr) : -0.032f;
        const float dr = argc > 8 ? strtof(argv[8], nullptr) : 0.032f;
        const x4vr::OffAxis oal = argc > 9 ? parse_off_axis(argv[9])
                                           : x4vr::OffAxis{};
        const x4vr::OffAxis oar = argc > 10 ? parse_off_axis(argv[10])
                                            : x4vr::OffAxis{};
        // #77's regression: argv[11] is the descriptor set carrying the roll,
        // the SAME argument position vert-eye-offset uses, so the two stages
        // are invoked the same way in the harness as they are in the layer.
        // Absent means unrolled and the emitted words must be byte-identical
        // to the pre-fix ones -- asserted below, not assumed.
        const uint32_t roll_set =
            arg_given(argc, argv, 11)
                ? (uint32_t)strtoul(argv[11], nullptr, 0)
                : 0xffffffffu;
        // "-" means ABSENT. Positional arguments mean reaching argv[11]
        // requires passing something at 9 and 10, and presence alone used to
        // turn the affine on -- which would make the roll case impossible to
        // invoke, since the two are refused in combination.
        const bool have_oal = argc > 9 && strcmp(argv[9], "-") != 0;
        const bool have_oar = argc > 10 && strcmp(argv[10], "-") != 0;
        // argv[12] is #61's pose set. It exists so the roll+pose REFUSAL can
        // be exercised at all: with pose_set hardcoded absent, a case named
        // "refuses roll with pose" passed by never reaching the branch, which
        // is a check that cannot fail.
        const uint32_t pose_set =
            arg_given(argc, argv, 12)
                ? (uint32_t)strtoul(argv[12], nullptr, 0)
                : 0xffffffffu;
        const bool ok = x4vr::spv::patch_fragment_invproj_eye(
            code, set, binding, member, dl, dr, have_oal ? &oal : nullptr,
            have_oar ? &oar : nullptr, pose_set, 0, roll_set, 0);
        printf("PATCHED=%d\n", ok ? 1 : 0);
        if (!ok && code != before) {
            printf("FAIL=refusal_modified_code\n");
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) {
            printf("FAIL=open_out\n");
            return 1;
        }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }

    // The fog passthrough (task #22 measurement).
    if (strcmp(argv[1], "frag-disable-fog") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const std::vector<uint32_t> before = code;
        const bool ok = x4vr::spv::patch_fragment_disable_fog(code);
        printf("PATCHED=%d\n", ok ? 1 : 0);
        if (!ok && code != before) {
            printf("FAIL=refusal_modified_code\n");
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) {
            printf("FAIL=open_out\n");
            return 1;
        }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }

    // Reports both readings of the World predicate for one module, so a test
    // can pin which modules the widened rule newly catches -- and, just as
    // importantly, which it must not.
    if (strcmp(argv[1], "classify") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        auto name = [](x4vr::spv::Kind k) {
            return k == x4vr::spv::Kind::World      ? "World"
                   : k == x4vr::spv::Kind::NonWorld ? "NonWorld"
                                                    : "NotVertex";
        };
        printf("NARROW=%s WIDE=%s PROCEDURAL=%d\n",
               name(x4vr::spv::classify(code, false)),
               name(x4vr::spv::classify(code, true)),
               x4vr::spv::is_procedural_fullscreen(code) ? 1 : 0);
        return 0;
    }

    // Task #30. The canvas variant is patch_vertex_clip with a constant-shift
    // matrix instead of the shear, so this exists to sweep it over every
    // dumped module offline: a refusal or an invalid module found here costs
    // nothing, and found in a take costs a load screen.
    //
    //     vert-clip <in.spv> <out.spv> [s]
    //
    // Writes the left eye's matrix (identity with m[12] = +s) and the right
    // eye's (-s), which is exactly what the layer builds.
    // vert-pose-rotate <in.spv> [pose_set] [cam_set] [cam_binding] [member]
    //
    // #33's conjugation. Emits to <out> so spirv-val can judge it, and prints
    // REFUSED for a module with no camera block -- which is the correct answer
    // for the 54 non-World modules and must not read as a failure.
    // vert-facing-check <in.spv> [out.spv]
    //
    // patch_vertex_facing_body, run: a card X4 turned to its camera (a world
    // matrix whose turn is the camera's) must come out turned to the ship's
    // forward, as X4 would draw it with the head straight -- the head-look in
    // x4vr_camwrite.hpp's convention, swept with several ships, scales (one of
    // them unequal and with a flipped axis) and positions. Its normal follows.
    // Three controls: the head-look at identity gives X4's own card; an object
    // not turned as the camera is moves not at all; and the unpatched module is
    // caught (it puts the card where the head turned it). gl_Position must be
    // X4's world-view-projection of the same world point.
    if (strcmp(argv[1], "vert-facing-check") == 0) {
        const std::vector<uint32_t> orig = load_spv(argv[2]);
        std::vector<uint32_t> code = orig;
        x4vr::spv::FacingReport rep;
        if (orig.empty() || !x4vr::spv::patch_vertex_facing_body(code, 1, 0, 3, 0, 31, 0, &rep)) {
            printf("FAIL=patch_refused\n");
            return 1;
        }
        printf("INPUTS=%u PARTIAL=%u FADE=%d\n", rep.inputs, rep.partial, rep.fade ? 1 : 0);
        // The look slot's rows ARE the rows the injector writes into X4's camera.
        {
            double far_off = 0.0;
            for (float y : {0.f, 30.f, -45.f, 120.f, -170.f})
                for (float p : {0.f, 20.f, -50.f, 74.f}) {
                    x4vr::PoseBlock pb = x4vr::pose_identity();
                    x4vr::pose_set_look(pb, y, p, true);
                    float rows[12] = {};
                    x4vr::camwrite::rows_for(y, p, rows);
                    for (int r = 0; r < 3; r++)
                        for (int c = 0; c < 3; c++)
                            far_off = std::max(far_off, (double)fabsf(pb.look[c * 4 + r] -
                                                                      rows[r * 4 + c]));
                }
            printf("LOOK_VS_CAMWRITE=%.2e\n", far_off);
            if (far_off > 1e-6) {
                printf("FAIL=look_rows\n");
                return 1;
            }
        }
        if (argc >= 4) {
            FILE *f = fopen(argv[3], "wb");
            if (!f || fwrite(code.data(), 4, code.size(), f) != code.size()) {
                printf("FAIL=write\n");
                return 1;
            }
            fclose(f);
        }
        using M3 = std::array<float, 9>; // column-major
        auto rot = [](float yaw, float pitch, float roll) {
            const float k = 0.01745329f, cy = cosf(yaw * k), sy = sinf(yaw * k),
                        cx = cosf(pitch * k), sx = sinf(pitch * k), cz = cosf(roll * k),
                        sz = sinf(roll * k);
            // Ry * Rx * Rz, column-major
            return M3{cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz,
                      -cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz,
                      sy * cx, -sx, cy * cx};
        };
        auto mul = [](const M3 &a, const M3 &b) {
            M3 o{};
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++)
                        o[c * 3 + r] += a[k * 3 + r] * b[c * 3 + k];
            return o;
        };
        auto tr = [](const M3 &a) {
            M3 o{};
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                    o[c * 3 + r] = a[r * 3 + c];
            return o;
        };
        auto mv = [](const M3 &a, const float v[3], float o[3]) {
            for (int r = 0; r < 3; r++)
                o[r] = a[r] * v[0] + a[3 + r] * v[1] + a[6 + r] * v[2];
        };
        auto m4 = [](const M3 &a, const float t[3], float o[16]) {
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    o[c * 4 + r] = (c < 3 && r < 3) ? a[c * 3 + r]
                                   : (c == 3 && r < 3) ? (t ? t[r] : 0.f)
                                   : (c == 3 && r == 3) ? 1.f
                                                        : 0.f;
        };
        auto mul4 = [](const float *a, const float *b, float *o) {
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) {
                    float s = 0.f;
                    for (int k = 0; k < 4; k++)
                        s += a[k * 4 + r] * b[c * 4 + k];
                    o[c * 4 + r] = s;
                }
        };
        // X4's projection: row3 = [0 0 1 0], m[10] = 0, reversed-Z near 0.1.
        float P[16] = {};
        P[0] = 0.7f, P[5] = -0.7f, P[2 * 4 + 3] = 1.f, P[3 * 4 + 2] = 0.1f;
        const float ships[][3] = {{0, 0, 0}, {37, -12, 8}, {-140, 60, -30}};
        const float looks[][2] = {{0, 0}, {30, 0}, {-45, 20}, {120, -50}, {-10, 74}};
        const float scales[][3] = {{1, 1, 1}, {3.5f, 3.5f, 3.5f}, {2, 5, 0.5f}, {4, 4, -4}};
        // X4's cards keep a roll of their own about the view axis (takes 466-467: the
        // diagonal test missed them).
        const float rolls[] = {0.f, 25.f, -140.f};
        const float trans[][3] = {{120, -40, 900}, {-2500, 300, -1800}};
        const float corners[][3] = {{-64, -64, 0}, {64, -64, 0}, {64, 64, 0}, {-30, 50, 2}};
        const float normal_in[4] = {0.f, 0.f, -1.f, 0.f};
        int checked = 0, bad = 0, ctrl_seen = 0, ctrl_runs = 0, ortho_kept = 0, ortho_runs = 0,
            k_right = 0, k_runs = 0, eye_checked = 0, eye_bad = 0, inv_bad = 0;
        double eye_worst = 0.0, inv_worst = 0.0;
        double worst = 0.0;
        std::string why;
        // An orthographic camera (a menu, the HUD): clip w is 1, column 2's w is 0.
        float Portho[16] = {};
        Portho[0] = 0.01f, Portho[5] = -0.01f, Portho[10] = 0.001f, Portho[15] = 1.f;
        bool ortho_cam = false;
        auto run = [&](const std::vector<uint32_t> &c, const float view[16], const float world[16],
                       const float wvp[16], const x4vr::PoseBlock &pb, const float pos[3],
                       float out_pos[3], float out_n[3], float clip[4]) {
            x4vr::interp::Machine m;
            std::vector<uint8_t> cam(9 * 64, 0), obj(3 * 64, 0), pose(sizeof pb);
            memcpy(&cam[0], view, 64);
            memcpy(&cam[64], ortho_cam ? Portho : P, 64); // M_projection, member 1
            memcpy(&obj[0], wvp, 64);
            memcpy(&obj[64], world, 64);
            memcpy(pose.data(), &pb, sizeof pb);
            m.buffers[{1, 0}] = cam;
            m.buffers[{3, 0}] = obj;
            m.buffers[{31, 0}] = pose;
            m.input_location[0] = {pos[0], pos[1], pos[2]};
            m.input_location[1] = {normal_in[0], normal_in[1], normal_in[2], normal_in[3]};
            if (!m.run(c, 0)) {
                why = m.error;
                return false;
            }
            for (int k = 0; k < 3; k++) {
                out_pos[k] = m.outputs[0][k];
                out_n[k] = m.outputs[1][k];
            }
            memcpy(clip, m.position, 16);
            return true;
        };
        for (const auto &sh : ships)
            for (const auto &lk : looks)
                for (const float roll : rolls)
                for (const auto &sc : scales)
                    for (const auto &t : trans)
                        for (const auto &pc : corners) {
            const M3 body = rot(sh[0], sh[1], sh[2]);
            x4vr::PoseBlock pb = x4vr::pose_identity();
            x4vr::pose_set_look(pb, lk[0], lk[1], true);
            // The camera's axes in the ship's are look's rows (x4vr_camwrite.hpp):
            // camera = body * rows^T.
            M3 rows{};
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    rows[c * 3 + r] = pb.look[c * 4 + r];
            const M3 cam = mul(body, tr(rows));
            const M3 S = mul(rot(0, 0, roll), M3{sc[0], 0, 0, 0, sc[1], 0, 0, 0, sc[2]});
            float view[16], world[16], vw[16], wvp[16], ownw[16];
            m4(tr(cam), nullptr, view); // camera-relative: X4's view has no translation
            m4(mul(cam, S), t, world);  // the card, turned to the camera
            mul4(view, world, vw);
            mul4(P, vw, wvp);
            float got[3], gn[3], clip[4];
            if (!run(code, view, world, wvp, pb, pc, got, gn, clip)) {
                printf("FAIL=interp %s\n", why.c_str());
                return 1;
            }
            // Expected: the card turned to the ship.
            float want[3], wn[3], sp[3], sn[3];
            const M3 bs = mul(body, S);
            mv(bs, pc, sp);
            for (int k = 0; k < 3; k++)
                want[k] = t[k] + sp[k];
            mv(bs, normal_in, sn);
            for (int k = 0; k < 3; k++)
                wn[k] = sn[k];
            // gl_Position = X4's projection of the same world point.
            float wp4[4] = {want[0], want[1], want[2], 1.f}, cw[4] = {}, c4[4] = {};
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++)
                    cw[r] += view[k * 4 + r] * wp4[k];
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++)
                    c4[r] += P[k * 4 + r] * cw[k];
            double err = 0.0, scale = 1.0;
            for (int k = 0; k < 3; k++) {
                err = std::max(err, (double)fabsf(got[k] - want[k]));
                err = std::max(err, (double)fabsf(gn[k] - wn[k]));
                scale = std::max(scale, (double)fabsf(want[k]));
            }
            for (int k = 0; k < 4; k++)
                err = std::max(err, (double)fabsf(clip[k] - c4[k]) / std::max(1.f, fabsf(c4[3])));
            err /= scale;
            worst = std::max(worst, err);
            checked++;
            // Facing the eye (the look slot's column 3 x at 1): the card's normal on the
            // line from the eye to its centre, its up the ship's -- built here from the
            // geometry, and the same in the world whatever the head-look.
            {
                x4vr::PoseBlock pe = x4vr::pose_identity();
                x4vr::pose_set_look(pe, lk[0], lk[1], true, true);
                float ge[3], gne[3], cle[4];
                if (!run(code, view, world, wvp, pe, pc, ge, gne, cle))
                    return printf("FAIL=interp %s\n", why.c_str()), 1;
                const float up[3] = {rows[3], rows[4], rows[5]};
                float pv[3];
                mv(tr(cam), t, pv);
                const float pl = sqrtf(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
                const float nn[3] = {pv[0] / pl, pv[1] / pl, pv[2] / pl};
                float xx[3] = {up[1] * nn[2] - up[2] * nn[1], up[2] * nn[0] - up[0] * nn[2],
                               up[0] * nn[1] - up[1] * nn[0]};
                const float xl = sqrtf(xx[0] * xx[0] + xx[1] * xx[1] + xx[2] * xx[2]);
                for (float &c : xx)
                    c /= xl;
                const float yy[3] = {nn[1] * xx[2] - nn[2] * xx[1], nn[2] * xx[0] - nn[0] * xx[2],
                                     nn[0] * xx[1] - nn[1] * xx[0]};
                const M3 F{xx[0], xx[1], xx[2], yy[0], yy[1], yy[2], nn[0], nn[1], nn[2]};
                float sp2[3], wante[3];
                mv(mul(cam, mul(F, S)), pc, sp2);
                for (int k = 0; k < 3; k++)
                    wante[k] = t[k] + sp2[k];
                double ee = 0.0;
                for (int k = 0; k < 3; k++)
                    ee = std::max(ee, (double)fabsf(ge[k] - wante[k]));
                ee /= scale;
                eye_worst = std::max(eye_worst, ee);
                eye_checked++;
                if (ee > 2e-4)
                    eye_bad++;
                // The same card, same ship, the head straight: the same world point.
                x4vr::PoseBlock p0 = x4vr::pose_identity();
                x4vr::pose_set_look(p0, 0.f, 0.f, true, true);
                const M3 cam0 = body; // the head straight: the camera is the ship
                float view0[16], world0[16], vw0[16], wvp0[16], g0[3], gn0[3], cl0[4];
                m4(tr(cam0), nullptr, view0);
                m4(mul(cam0, S), t, world0);
                mul4(view0, world0, vw0);
                mul4(P, vw0, wvp0);
                if (!run(code, view0, world0, wvp0, p0, pc, g0, gn0, cl0))
                    return printf("FAIL=interp %s\n", why.c_str()), 1;
                double ei = 0.0;
                for (int k = 0; k < 3; k++)
                    ei = std::max(ei, (double)fabsf(ge[k] - g0[k]));
                ei /= scale;
                inv_worst = std::max(inv_worst, ei);
                if (ei > 2e-4)
                    inv_bad++;
            }
            if (err > 2e-4) {
                if (getenv("X4VR_FACING_DEBUG"))
                    printf("bad: ship %g,%g,%g look %g,%g scale %g,%g,%g err %.3g got %g %g %g want %g %g %g\n",
                           sh[0], sh[1], sh[2], lk[0], lk[1], sc[0], sc[1], sc[2], err, got[0], got[1],
                           got[2], want[0], want[1], want[2]);
                bad++;
            }
            // Controls, once per ship and look.
            if (&sc[0] == scales[0] && &t[0] == trans[0] && &pc[0] == corners[0]) {
                // 1: the head-look at identity: X4's own card, turned to the camera.
                x4vr::PoseBlock id = x4vr::pose_identity();
                float g1[3], n1[3], cl1[4], own[3];
                if (!run(code, view, world, wvp, id, pc, g1, n1, cl1))
                    return printf("FAIL=interp %s\n", why.c_str()), 1;
                mv(mul(cam, S), pc, own);
                for (int k = 0; k < 3; k++)
                    if (fabsf(g1[k] - (t[k] + own[k])) > 1e-3f * scale)
                        bad++;
                // 2: an object turned as the ship is: no billboard, untouched.
                float objw[16], objvw[16], objwvp[16], g2[3], n2[3], cl2[4], ob[3];
                m4(mul(body, S), t, objw);
                mul4(view, objw, objvw);
                mul4(P, objvw, objwvp);
                if (!run(code, view, objw, objwvp, pb, pc, g2, n2, cl2))
                    return printf("FAIL=interp %s\n", why.c_str()), 1;
                // 5: the collapse diagnostic's K (a zero look slot) on that ordinary
                // object: collapsed to its origin in a near/far-faded module, as X4
                // drew it in any other.
                {
                    x4vr::PoseBlock zero = pb;
                    memset(zero.look, 0, sizeof zero.look);
                    float g5[3], n5[3], cl5[4], ob5[3];
                    if (!run(code, view, objw, objwvp, zero, pc, g5, n5, cl5))
                        return printf("FAIL=interp %s\n", why.c_str()), 1;
                    mv(mul(body, S), pc, ob5);
                    k_runs++;
                    double e5 = 0.0;
                    for (int k = 0; k < 3; k++)
                        e5 = std::max(e5, (double)fabsf(g5[k] - (rep.fade ? t[k] : t[k] + ob5[k])));
                    if (lk[0] == 0 && lk[1] == 0) {
                        // Head straight: an object turned as the ship IS turned as the
                        // camera, a card to the test, which K collapses in any module.
                        double e5c = 0.0;
                        for (int k = 0; k < 3; k++)
                            e5c = std::max(e5c, (double)fabsf(g5[k] - t[k]));
                        e5 = std::min(e5, e5c);
                    }
                    if (e5 <= 1e-3 * scale)
                        k_right++;
                    else
                        bad++;
                }
                mv(mul(body, S), pc, ob);
                const bool head_straight = lk[0] == 0 && lk[1] == 0;
                for (int k = 0; k < 3; k++)
                    if (fabsf(g2[k] - (t[k] + ob[k])) > 1e-3f * scale)
                        bad++;
                (void)head_straight;
                // 3: the unpatched module puts the card where the head turned it.
                float g3[3], n3[3], cl3[4];
                if (!run(orig, view, world, wvp, pb, pc, g3, n3, cl3))
                    return printf("FAIL=interp %s\n", why.c_str()), 1;
                if (lk[0] != 0 || lk[1] != 0) {
                    ctrl_runs++;
                    double e3 = 0.0;
                    for (int k = 0; k < 3; k++)
                        e3 = std::max(e3, (double)fabsf(g3[k] - want[k]));
                    if (e3 > 1.0)
                        ctrl_seen++;
                    // 4: the same card under an orthographic camera: no billboard,
                    // untouched however the head turned.
                    float g4[3], n4[3], cl4[4];
                    ortho_cam = true;
                    const bool ok4 = run(code, view, world, wvp, pb, pc, g4, n4, cl4);
                    ortho_cam = false;
                    if (!ok4)
                        return printf("FAIL=interp %s\n", why.c_str()), 1;
                    ortho_runs++;
                    double e4 = 0.0;
                    for (int k = 0; k < 3; k++)
                        e4 = std::max(e4, (double)fabsf(g4[k] - g3[k]));
                    if (e4 <= 1e-3 * scale)
                        ortho_kept++;
                    else
                        bad++;
                }
                (void)ownw;
            }
        }
        printf("CHECKED=%d BAD=%d WORST=%.2e CONTROL_CAUGHT=%d/%d ORTHO_KEPT=%d/%d K_%s=%d/%d\n",
               checked, bad, worst, ctrl_seen, ctrl_runs, ortho_kept, ortho_runs,
               rep.fade ? "COLLAPSED" : "KEPT", k_right, k_runs);
        printf("EYE_CHECKED=%d EYE_BAD=%d EYE_WORST=%.2e HEAD_INVARIANT_BAD=%d INV_WORST=%.2e\n",
               eye_checked, eye_bad, eye_worst, inv_bad, inv_worst);
        bad += eye_bad + inv_bad;
        printf(bad == 0 && ctrl_runs > 0 && ctrl_seen == ctrl_runs && rep.inputs >= 2 ? "PASS\n"
                                                                                       : "FAIL\n");
        return bad == 0 && ctrl_seen == ctrl_runs ? 0 : 1;
    }

    // fog-points-check <in.spv> [out.spv]
    //
    // patch_fog_points_facing, run on a straight-line stand-in for X4's froxel fog
    // pass: a blob whose view-to-blob matrix X4 built along the camera's axes (with a
    // roll and a scale of its own) must come out sampled as X4 would sample it with
    // the head straight -- the ship's axes -- for ships, head-looks, blobs and froxel
    // points swept. Controls: the head-look at identity is X4's own; a zero look slot
    // (the collapse diagnostic) puts every froxel out of every blob's reach; the
    // unpatched module, head turned, must be measurably wrong.
    if (strcmp(argv[1], "fog-points-check") == 0) {
        const std::vector<uint32_t> orig = load_spv(argv[2]);
        std::vector<uint32_t> code = orig;
        x4vr::spv::FogPointReport rep;
        if (orig.empty() || !x4vr::spv::patch_fog_points_facing(code, 31, 0, &rep)) {
            printf("FAIL=patch_refused %s\n", rep.why);
            return 1;
        }
        printf("PRODUCTS=%u\n", rep.products);
        if (argc >= 4) {
            FILE *f = fopen(argv[3], "wb");
            if (!f || fwrite(code.data(), 4, code.size(), f) != code.size()) {
                printf("FAIL=write\n");
                return 1;
            }
            fclose(f);
        }
        using M3 = std::array<float, 9>; // column-major
        auto rot = [](float yaw, float pitch, float roll) {
            const float k = 0.01745329f, cy = cosf(yaw * k), sy = sinf(yaw * k),
                        cx = cosf(pitch * k), sx = sinf(pitch * k), cz = cosf(roll * k),
                        sz = sinf(roll * k);
            return M3{cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz,
                      -cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz,
                      sy * cx, -sx, cy * cx};
        };
        auto mul = [](const M3 &a, const M3 &b) {
            M3 o{};
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++)
                        o[c * 3 + r] += a[k * 3 + r] * b[c * 3 + k];
            return o;
        };
        auto tr = [](const M3 &a) {
            M3 o{};
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                    o[c * 3 + r] = a[r * 3 + c];
            return o;
        };
        auto mv = [](const M3 &a, const float v[3], float o[3]) {
            for (int r = 0; r < 3; r++)
                o[r] = a[r] * v[0] + a[3 + r] * v[1] + a[6 + r] * v[2];
        };
        // std140: M_texturematrix0 at 0, U_bias 64, M_offset 80 (32 x 64), V_scale 2128,
        // I_index 2640 (stride 16), I_point_num 3152.
        const size_t kOff = 80, kBlock = 3168;
        struct Blob { float c[3], s, roll; };
        auto block_of = [&](const M3 &cam, const Blob (&b)[2]) {
            std::vector<uint8_t> buf(kBlock, 0);
            for (int i = 0; i < 2; i++) {
                // X4's matrix along the camera's axes: A = Rz(roll) / s, t = -A p,
                // p the blob's centre in the camera's space.
                M3 A = rot(0, 0, b[i].roll);
                for (float &x : A)
                    x /= b[i].s;
                float p[3], ap[3];
                mv(tr(cam), b[i].c, p);
                mv(A, p, ap);
                float m[16] = {};
                for (int c = 0; c < 3; c++)
                    for (int r = 0; r < 3; r++)
                        m[c * 4 + r] = A[c * 3 + r];
                for (int r = 0; r < 3; r++)
                    m[12 + r] = -ap[r];
                m[15] = 1.f;
                memcpy(&buf[kOff + 64 * i], m, 64);
            }
            int n = 2;
            memcpy(&buf[3152], &n, 4);
            return buf;
        };
        std::string why;
        auto run = [&](const std::vector<uint32_t> &c, const std::vector<uint8_t> &blk,
                       const x4vr::PoseBlock &pb, const float v[3], float o0[3], float o1[3],
                       float o2[3]) {
            x4vr::interp::Machine m;
            std::vector<uint8_t> pose(sizeof pb);
            memcpy(pose.data(), &pb, sizeof pb);
            m.buffers[{4, 0}] = blk;
            m.buffers[{31, 0}] = pose;
            m.input_location[0] = {v[0], v[1], v[2]};
            if (!m.run(c, 0)) {
                why = m.error;
                return false;
            }
            for (int k = 0; k < 3; k++) {
                o0[k] = m.outputs[0][k];
                o1[k] = m.outputs[1][k];
                o2[k] = m.outputs[2][k];
            }
            return true;
        };
        const float ships[][3] = {{0, 0, 0}, {37, -12, 8}, {-140, 60, -30}};
        const float looks[][2] = {{0, 0}, {30, 0}, {-45, 20}, {120, -50}, {-10, 74}};
        const Blob blobs[][2] = {{{{0, 0, 30000}, 22000, 0}, {{-9000, 4000, 52000}, 30000, 0}},
                                 {{{15000, -8000, -40000}, 44000, 35}, {{500, 300, 800}, 900, -70}}};
        const float points[][3] = {{0, 0, 30000}, {12000, -3000, 25000}, {-20000, 9000, 41000},
                                   {3000, 2000, -1000}};
        int checked = 0, bad = 0, ident_ok = 0, ident_runs = 0, killed = 0, kill_runs = 0,
            ctrl_seen = 0, ctrl_runs = 0;
        double worst = 0.0;
        for (const auto &sh : ships)
            for (const auto &lk : looks)
                for (const auto &bl : blobs)
                    for (const auto &X : points) {
            const M3 body = rot(sh[0], sh[1], sh[2]);
            x4vr::PoseBlock pb = x4vr::pose_identity();
            x4vr::pose_set_look(pb, lk[0], lk[1], true);
            M3 rows{};
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++)
                    rows[c * 3 + r] = pb.look[c * 4 + r];
            const M3 cam = mul(body, tr(rows)); // the head's camera (x4vr_camwrite.hpp)
            const std::vector<uint8_t> blk = block_of(cam, bl);
            float v[3];
            mv(tr(cam), X, v); // the froxel, in the camera's space
            float g0[3], g1[3], g2[3];
            if (!run(code, blk, pb, v, g0, g1, g2))
                return printf("FAIL=interp %s\n", why.c_str()), 1;
            // Expected: the blob's frame along the SHIP's axes: A body^T (X - c).
            double err = 0.0;
            for (int i = 0; i < 2; i++) {
                M3 A = rot(0, 0, bl[i].roll);
                for (float &x : A)
                    x /= bl[i].s;
                float d[3] = {X[0] - bl[i].c[0], X[1] - bl[i].c[1], X[2] - bl[i].c[2]}, ds[3],
                      want[3];
                mv(tr(body), d, ds);
                mv(A, ds, want);
                const float *got = i ? g1 : g0;
                for (int k = 0; k < 3; k++)
                    err = std::max(err, (double)fabsf(got[k] - want[k]));
            }
            {
                // The gradient's point, view + (1,1,1): in the world X + cam (1,1,1).
                const float one[3] = {1, 1, 1};
                float dw[3], ds[3], want[3];
                mv(cam, one, dw);
                M3 A = rot(0, 0, bl[0].roll);
                for (float &x : A)
                    x /= bl[0].s;
                float d[3] = {X[0] + dw[0] - bl[0].c[0], X[1] + dw[1] - bl[0].c[1],
                              X[2] + dw[2] - bl[0].c[2]};
                mv(tr(body), d, ds);
                mv(A, ds, want);
                for (int k = 0; k < 3; k++)
                    err = std::max(err, (double)fabsf(g2[k] - want[k]));
            }
            worst = std::max(worst, err);
            checked++;
            if (err > 2e-4) {
                if (getenv("X4VR_FACING_DEBUG"))
                    printf("bad: ship %g,%g,%g look %g,%g err %.3g\n", sh[0], sh[1], sh[2], lk[0],
                           lk[1], err);
                bad++;
            }
            // Controls.
            float u0[3], u1[3], u2[3];
            if (!run(orig, blk, pb, v, u0, u1, u2))
                return printf("FAIL=interp %s\n", why.c_str()), 1;
            // 1: the head-look at identity is X4's own.
            float i0[3], i1[3], i2[3];
            if (!run(code, blk, x4vr::pose_identity(), v, i0, i1, i2))
                return printf("FAIL=interp %s\n", why.c_str()), 1;
            ident_runs++;
            double e1 = 0.0;
            for (int k = 0; k < 3; k++)
                e1 = std::max({e1, (double)fabsf(i0[k] - u0[k]), (double)fabsf(i1[k] - u1[k]),
                               (double)fabsf(i2[k] - u2[k])});
            if (e1 <= 2e-4)
                ident_ok++;
            else
                bad++;
            // 2: a zero look slot puts the froxel out of every blob's reach.
            x4vr::PoseBlock zero = pb;
            memset(zero.look, 0, sizeof zero.look);
            float k0[3], k1[3], k2[3];
            if (!run(code, blk, zero, v, k0, k1, k2))
                return printf("FAIL=interp %s\n", why.c_str()), 1;
            kill_runs++;
            if (k0[0] > 1e8f && k0[1] > 1e8f && k0[2] > 1e8f && k1[0] > 1e8f && k2[0] > 1e8f)
                killed++;
            else
                bad++;
            // 3: unpatched, the head turned: the blob along the head's axes, wrong.
            if (lk[0] != 0 || lk[1] != 0) {
                ctrl_runs++;
                double e3 = 0.0;
                for (int i = 0; i < 2; i++) {
                    M3 A = rot(0, 0, bl[i].roll);
                    for (float &x : A)
                        x /= bl[i].s;
                    float d[3] = {X[0] - bl[i].c[0], X[1] - bl[i].c[1], X[2] - bl[i].c[2]}, ds[3],
                          want[3];
                    mv(tr(body), d, ds);
                    mv(A, ds, want);
                    const float *u = i ? u1 : u0;
                    for (int k = 0; k < 3; k++)
                        e3 = std::max(e3, (double)fabsf(u[k] - want[k]));
                }
                if (e3 > 1e-2)
                    ctrl_seen++;
            }
        }
        printf("CHECKED=%d BAD=%d WORST=%.2e IDENTITY=%d/%d KILLED=%d/%d CONTROL_CAUGHT=%d/%d\n",
               checked, bad, worst, ident_ok, ident_runs, killed, kill_runs, ctrl_seen, ctrl_runs);
        const bool pass = bad == 0 && ctrl_runs > 0 && ctrl_seen == ctrl_runs && rep.products >= 3;
        printf(pass ? "PASS\n" : "FAIL\n");
        return pass ? 0 : 1;
    }

    // fog-points <in.spv> <out.spv>: patch_fog_points_facing alone, for X4's own
    // froxel module from a dump (/tmp only), which the interpreter cannot run.
    if (strcmp(argv[1], "fog-points") == 0 && argc >= 4) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        x4vr::spv::FogPointReport rep;
        if (code.empty() || !x4vr::spv::patch_fog_points_facing(code, 31, 0, &rep)) {
            printf("REFUSED=1 WHY=%s\n", rep.why);
            return 0;
        }
        printf("REFUSED=0 PRODUCTS=%u WORDS=%zu\n", rep.products, code.size());
        FILE *f = fopen(argv[3], "wb");
        if (!f) { printf("FAIL=open\n"); return 1; }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }


    // volume-base <in.spv>: is_x4_volume_base's verdict and why.
    if (strcmp(argv[1], "volume-base") == 0 && argc >= 3) {
        const std::vector<uint32_t> code = load_spv(argv[2]);
        x4vr::spv::VolumeBaseReport rep;
        const bool is = !code.empty() && x4vr::spv::is_x4_volume_base(code, &rep);
        printf("IS=%d WHY=%s\n", is ? 1 : 0, is ? "" : rep.why);
        return 0;
    }
    // fog-prep <in.spv>: which of X4's froxel fog preparation stages this is (after take
    // 495): 0 none, 1 volume_base, 2 volume_point, 3 volume_shad.
    if (strcmp(argv[1], "fog-prep") == 0 && argc >= 3) {
        const std::vector<uint32_t> code = load_spv(argv[2]);
        printf("PREP=%d\n", code.empty() ? -1 : (int)x4vr::spv::x4_fog_prep_kind(code));
        return 0;
    }
    // volume-walk <in.spv>: is_x4_volume_walk's verdict, the media, and why.
    if (strcmp(argv[1], "volume-walk") == 0 && argc >= 3) {
        const std::vector<uint32_t> code = load_spv(argv[2]);
        x4vr::spv::VolumeWalkReport rep;
        const bool is = !code.empty() && x4vr::spv::is_x4_volume_walk(code, &rep);
        printf("IS=%d MEDIA=%d WHY=%s\n", is ? 1 : 0, rep.media, is ? "" : rep.why);
        return 0;
    }
    // fog-apply <in.spv> <out.spv>: X4's fog blend made to read our march too (after take
    // 486), and why not where it refuses (the module then unchanged).
    if (strcmp(argv[1], "fog-apply") == 0 && argc >= 4) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        const std::vector<uint32_t> before = code;
        x4vr::spv::FogApplyReport rep;
        const bool ok = !code.empty() && x4vr::spv::patch_fog_apply_march(code, 31, 0, 2, &rep);
        if (!ok && code != before) {
            printf("FAIL=refusal_modified_code\n");
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) { printf("FAIL=open_out\n"); return 1; }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        printf("PATCHED=%d WHY=%s\n", ok ? 1 : 0, ok ? "" : rep.why);
        return 0;
    }
    // fade-block <in.spv>: where a fade module keeps its material's fade distances; and the
    // nebula rule on X4's materials (after take 482).
    if (strcmp(argv[1], "fade-block") == 0 && argc >= 3) {
        const std::vector<uint32_t> code = load_spv(argv[2]);
        x4vr::spv::FadeBlock fb;
        if (code.empty() || !x4vr::spv::fade_block(code, &fb)) {
            printf("IS=0 WHY=%s\n", code.empty() ? "load" : fb.why);
        } else {
            printf("IS=1 SET=%u BINDING=%u OFF=%u,%u,%u,%u\n", fb.set, fb.binding, fb.off[0],
                   fb.off[1], fb.off[2], fb.off[3]);
        }
        // fog_02, fog_pattern_new_near_01: nebula; a ship trail (500..15000), a hologram
        // (0.1..400), an explosion (0..35000), the default (100..120000): not.
        printf("NEB=%d%d%d%d%d%d\n", x4vr::spv::is_nebula_fade(500.f, 3000.f),
               x4vr::spv::is_nebula_fade(500.f, 2900.f), x4vr::spv::is_nebula_fade(500.f, 15000.f),
               x4vr::spv::is_nebula_fade(0.1f, 400.f), x4vr::spv::is_nebula_fade(0.f, 35000.f),
               x4vr::spv::is_nebula_fade(100.f, 120000.f));
        return 0;
    }
    // retarget-set <in.spv> <from> <to> <out.spv>: our module's placeholder set moved to the
    // layer's, and how many decorations moved.
    if (strcmp(argv[1], "retarget-set") == 0 && argc >= 6) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        const uint32_t n = code.empty() ? 0
                           : x4vr::spv::retarget_descriptor_set(code, (uint32_t)atoi(argv[3]),
                                                                 (uint32_t)atoi(argv[4]));
        FILE *f = fopen(argv[5], "wb");
        if (!f) { printf("FAIL=write\n"); return 1; }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        printf("MOVED=%u\n", n);
        return 0;
    }
    // volume-base-offset <in.spv> <out.spv> <member-name> <new-offset>: a copy with one
    // member's Offset moved, the negative control for the check above.
    if (strcmp(argv[1], "volume-base-offset") == 0 && argc >= 6) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        std::vector<x4vr::spv::Inst> insts;
        if (code.empty() || !x4vr::spv::iterate(code, insts)) { printf("FAIL=load\n"); return 1; }
        uint32_t st = 0, mi = 0;
        bool found = false;
        for (const auto &in : insts)
            if (in.op == x4vr::spv::OpMemberName) {
                const char *n = (const char *)&code[in.start + 3];
                if (!strcmp(n, argv[4])) {
                    st = code[in.start + 1];
                    mi = code[in.start + 2];
                    found = true;
                }
            }
        int moved = 0;
        for (const auto &in : insts)
            if (found && in.op == x4vr::spv::OpMemberDecorate && code[in.start + 1] == st &&
                code[in.start + 2] == mi && code[in.start + 3] == x4vr::spv::DecorationOffset) {
                code[in.start + 4] = (uint32_t)atoi(argv[5]);
                moved++;
            }
        FILE *f = fopen(argv[3], "wb");
        if (!f) { printf("FAIL=open\n"); return 1; }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        printf("MOVED=%d\n", moved);
        return 0;
    }

    if (strcmp(argv[1], "vert-pose-rotate") == 0 && argc >= 4) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) { printf("FAIL=load\n"); return 1; }
        const uint32_t ps = argc > 4 ? (uint32_t)atoi(argv[4]) : 5;
        const uint32_t cs = argc > 5 ? (uint32_t)atoi(argv[5]) : 1;
        const uint32_t cb = argc > 6 ? (uint32_t)atoi(argv[6]) : 0;
        const uint32_t cm = argc > 7 ? (uint32_t)atoi(argv[7]) : 1;
        // set 3 member 0 is M_worldviewprojection -- the #62 fallback.
        // Member 2 is M_prevworldviewprojection: take 188's motion vector.
        bool mv = false;
        if (!x4vr::spv::patch_vertex_pose_rotate(code, ps, 0, cs, cb, cm, 3, 0,
                                                 0, 2, &mv)) {
            printf("REFUSED=1\n");
            return 0;
        }
        printf("REFUSED=0 MV=%d WORDS=%zu\n", mv ? 1 : 0, code.size());
        FILE *f = fopen(argv[3], "wb");
        if (!f) { printf("FAIL=open\n"); return 1; }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }
    if (strcmp(argv[1], "vert-clip") == 0 && argc >= 4) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const float s = argc > 4 ? strtof(argv[4], nullptr) : 0.02133f;
        float kl[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float kr[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        kl[12] = +s;
        kr[12] = -s;
        const bool ok = x4vr::spv::patch_vertex_clip(code, kl, kr);
        printf("PATCHED=%d KIND=%s\n", ok ? 1 : 0,
               x4vr::spv::classify(code, false) == x4vr::spv::Kind::World
                   ? "World"
                   : "other");
        if (ok) {
            FILE *f = fopen(argv[3], "wb");
            if (!f)
                return 1;
            fwrite(code.data(), 4, code.size(), f);
            fclose(f);
        }
        return ok ? 0 : 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        auto tex = x4vr::spv::list_sampled_textures(code);
        printf("TEXTURES=%zu\n", tex.size());
        for (const auto &t : tex)
            printf("TEX set=%u binding=%u count=%u%s%s\n", t.set, t.binding,
                   t.count, t.arrayed ? " arrayed" : "",
                   t.depth ? " depth" : "");
        return 0;
    }
    // Task #76: force one uint member of one uniform block to a constant.
    //
    //   frag-force-uint <in> <out> <set> <binding> <byteoffset> <name> <value>
    //
    // Prints LOADS=<n> beside PATCHED, because "the transform applied" and "it
    // reached every read" are different claims and X4's copy_gamma reads the
    // switch exactly once. A patch that rewrote one of two loads would still
    // print PATCHED=1 and leave the branch live.
    if (strcmp(argv[1], "frag-force-uint") == 0) {
        if (argc < 9) {
            fprintf(stderr, "frag-force-uint needs <in> <out> <set> <binding> "
                            "<byteoffset> <name> <value>\n");
            return 2;
        }
        std::vector<uint32_t> code = load_spv(argv[2]);
        if (code.empty()) {
            printf("FAIL=load\n");
            return 1;
        }
        const std::vector<uint32_t> before = code;
        const uint32_t set = (uint32_t)strtoul(argv[4], nullptr, 0);
        const uint32_t binding = (uint32_t)strtoul(argv[5], nullptr, 0);
        const uint32_t byteoff = (uint32_t)strtoul(argv[6], nullptr, 0);
        const uint32_t value = (uint32_t)strtoul(argv[8], nullptr, 0);
        uint32_t loads = 0;
        const bool ok = x4vr::spv::patch_fragment_force_block_uint(
            code, set, binding, byteoff, argv[7], value, &loads);
        printf("PATCHED=%d LOADS=%u WORDS=%zu\n", ok ? 1 : 0, loads,
               code.size());
        if (!ok && code != before) {
            printf("FAIL=refusal_modified_code\n");
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f) {
            printf("FAIL=open_out\n");
            return 1;
        }
        fwrite(code.data(), 4, code.size(), f);
        fclose(f);
        return 0;
    }
    if (argc < 4) {
        fprintf(stderr, "%s needs <in> <out>\n", argv[1]);
        return 2;
    }
    const bool index_offset = strcmp(argv[1], "frag-index-offset") == 0;
    if (!index_offset && strcmp(argv[1], "frag-view-layer") != 0) {
        fprintf(stderr, "unknown patch '%s'\n", argv[1]);
        return 2;
    }
    const uint32_t set = argc > 4 ? (uint32_t)strtoul(argv[4], nullptr, 0) : 0;
    const uint32_t binding =
        argc > 5 ? (uint32_t)strtoul(argv[5], nullptr, 0) : 0;

    std::vector<uint32_t> code = load_spv(argv[2]);
    if (code.empty()) {
        printf("FAIL=load\n");
        return 1;
    }
    const std::vector<uint32_t> before = code;

    const uint32_t offset =
        argc > 6 ? (uint32_t)strtoul(argv[6], nullptr, 0) : 26653u;
    const bool ok =
        index_offset
            ? x4vr::spv::patch_fragment_index_offset(code, set, binding, offset)
            : x4vr::spv::patch_fragment_view_layer(code, set, binding);
    printf("PATCHED=%d\n", ok ? 1 : 0);

    // A refusal must leave the module byte-identical. This is the property the
    // layer relies on to fall back to the original module, and it is cheap
    // enough to assert on every run rather than trust.
    if (!ok && code != before) {
        printf("FAIL=refusal_modified_code\n");
        return 1;
    }

    FILE *f = fopen(argv[3], "wb");
    if (!f) {
        printf("FAIL=open_out\n");
        return 1;
    }
    fwrite(code.data(), 4, code.size(), f);
    fclose(f);
    return 0;
}
