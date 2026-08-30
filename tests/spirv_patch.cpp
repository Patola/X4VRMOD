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

#include <cstdio>
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
                argv[0], argv[0], argv[0], argv[0], argv[0]);
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
        const bool ok = x4vr::spv::patch_vertex_eye_offset_mvp(
            code, set, binding, member, dl, have_dr ? &dr : nullptr,
            argc > 9 ? &oal : nullptr, argc > 10 ? &oar : nullptr);
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
