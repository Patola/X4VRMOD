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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s frag-view-layer <in> <out> [set] [binding]\n"
                "       %s frag-index-offset <in> <out> [set] [binding] "
                "[offset]\n"
                "       %s list <in>\n",
                argv[0], argv[0], argv[0]);
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
        const bool ok = x4vr::spv::patch_vertex_eye_offset(
            code, set, binding, member, dl, have_dr ? &dr : nullptr);
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
        const bool ok = x4vr::spv::patch_vertex_eye_offset_mvp(
            code, set, binding, member, dl, have_dr ? &dr : nullptr);
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
        const bool ok = x4vr::spv::patch_fragment_invproj_eye(code, set, binding,
                                                              member, dl, dr);
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
        printf("NARROW=%s WIDE=%s\n", name(x4vr::spv::classify(code, false)),
               name(x4vr::spv::classify(code, true)));
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
