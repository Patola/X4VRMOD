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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s frag-view-layer <in> <out> [set] [binding]\n"
                "       %s frag-index-offset <in> <out> [set] [binding] "
                "[offset]\n"
                "       %s frag-invproj <in> <out> [set] [binding] [member] "
                "[dl] [dr] [oa_l] [oa_r]\n"
                "       %s frag-invproj-check <in> [set] [binding] [member] "
                "[dl] [dr] [oa_l] [oa_r]\n"
                "       %s list <in>\n"
                "\n"
                "  oa_l / oa_r are off-axis frusta as \"l,r,u,d\" in DEGREES.\n"
                "  frag-invproj-check patches in memory and interprets the\n"
                "  emitted arithmetic against common/x4vr_view.hpp, printing\n"
                "  CHECKED=<n> WORST=<err>; it writes no file.\n",
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
        const bool ok = x4vr::spv::patch_vertex_eye_offset(
            code, set, binding, member, dl, have_dr ? &dr : nullptr,
            argc > 9 ? &oal : nullptr, argc > 10 ? &oar : nullptr);
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
        const x4vr::OffAxis oal = argc > 8 ? parse_off_axis(argv[8])
                                           : x4vr::OffAxis{};
        const x4vr::OffAxis oar = argc > 9 ? parse_off_axis(argv[9])
                                           : x4vr::OffAxis{};

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
                argc > 8 ? &oal : nullptr, argc > 9 ? &oar : nullptr)) {
            printf("FAIL=patch_refused\n");
            return 1;
        }

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
                const x4vr::OffAxis *o = view ? (argc > 9 ? &oar : &oal)
                                              : (argc > 8 ? &oal : nullptr);
                if (o)
                    x4vr::compose_inv_off_axis(*o, want);
                x4vr::compose_eye_untranslate(view ? dr : dl, want);

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
                        } else if (in.len >= 4 && w[3] == view_var) {
                            reg[w[2]].k = Val::I;
                            reg[w[2]].i = view;
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
                    if (in.len >= 3 && in.op == x4vr::spv::OpCompositeInsert &&
                        w[2] == want_id)
                        done = true;
                }
                if (reg[want_id].k != Val::M) {
                    // With no affine the patch's last write to want_id is still
                    // an OpCompositeInsert, so this can only mean the graph was
                    // not followed -- report it rather than scoring a zero.
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
        printf("CHECKED=%d WORST=%.3e\n", checked, worst);
        return 0;
    }

    // The per-eye M_invprojection correction (task #22).
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
        const bool ok = x4vr::spv::patch_fragment_invproj_eye(
            code, set, binding, member, dl, dr, argc > 9 ? &oal : nullptr,
            argc > 10 ? &oar : nullptr);
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
