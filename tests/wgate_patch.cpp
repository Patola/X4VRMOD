// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Patches a vertex module with task #82's w-gate so run-wgate.sh can put the
// result on a real GPU. K TRANSLATES far off screen on purpose: when the gate
// fires the geometry leaves the viewport and nothing is drawn, which is a
// difference the render harness can actually see. A scale would only shrink
// the triangle, and "drawn" would stay 1 either way.
#include "../common/x4vr_spirv.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <in.spv> <out.spv> <eps>\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (b.size() < 4) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    std::vector<uint32_t> code(b.size() / 4);
    memcpy(code.data(), b.data(), code.size() * 4);
    const float k[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 0, 0, 1};
    if (!x4vr::spv::patch_vertex_clip(code, k, nullptr, (float)atof(argv[3]))) {
        fprintf(stderr, "patch REFUSED %s\n", argv[1]);
        return 1;
    }
    std::ofstream o(argv[2], std::ios::binary);
    o.write((const char *)code.data(), code.size() * 4);
    return 0;
}
