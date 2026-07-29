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
    if (argc < 4) {
        fprintf(stderr, "usage: %s frag-view-layer <in> <out> [set] [binding]\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "frag-view-layer") != 0) {
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

    const bool ok = x4vr::spv::patch_fragment_view_layer(code, set, binding);
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
