// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// common/x4vr_png.hpp's reader, against Pillow. For each PNG on the command line
// it prints the size and a CRC-32 of the decoded RGBA, and tests/run-png-reader.sh
// compares those with what Pillow decodes from the same files. Pillow is the
// oracle because it was written by other people: a reader checked against our own
// writer would agree with it about a shared mistake, and the files the layer
// embeds are Pillow's, with every row filter in use.
//
// It also round-trips the writer's own output, RGB and RGBA, so the two halves of
// the header agree.

#include <cstdio>
#include <vector>

#include "../common/x4vr_png.hpp"

int main(int argc, char **argv) {
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            printf("FAIL %s: unreadable\n", argv[i]);
            bad++;
            continue;
        }
        std::vector<uint8_t> b;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            b.insert(b.end(), buf, buf + n);
        fclose(f);
        x4vr::png::Image im;
        if (!x4vr::png::decode(b.data(), b.size(), &im)) {
            printf("FAIL %s: not decoded\n", argv[i]);
            bad++;
            continue;
        }
        printf("%s %u %u %08lx\n", argv[i], im.w, im.h,
               crc32(0, im.rgba.data(), (uInt)im.rgba.size()));
    }
    // The writer's own output, read back.
    for (uint32_t ch : {3u, 4u}) {
        const uint32_t w = 37, h = 11;
        std::vector<uint8_t> px((size_t)w * h * ch);
        for (size_t i = 0; i < px.size(); i++)
            px[i] = (uint8_t)(i * 7 + i / 13);
        const char *path = "/tmp/x4vr-png-reader-roundtrip.png";
        x4vr::png::write_png_n(path, px.data(), w, h, ch);
        FILE *f = fopen(path, "rb");
        std::vector<uint8_t> b(1 << 16);
        b.resize(f ? fread(b.data(), 1, b.size(), f) : 0);
        if (f)
            fclose(f);
        remove(path);
        x4vr::png::Image im;
        bool same = x4vr::png::decode(b.data(), b.size(), &im) && im.w == w && im.h == h;
        for (size_t p = 0; same && p < (size_t)w * h; p++)
            for (uint32_t c = 0; c < 4; c++)
                same = same && im.rgba[p * 4 + c] == (c < ch ? px[p * ch + c] : 255);
        printf("%s round trip %s\n", ch == 3 ? "RGB" : "RGBA", same ? "ok" : "FAIL");
        bad += !same;
    }
    static const uint8_t junk[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 1};
    x4vr::png::Image im;
    const bool refused = !x4vr::png::decode(junk, sizeof junk, &im);
    printf("a broken file is refused %s\n", refused ? "ok" : "FAIL");
    bad += !refused;
    return bad ? 1 : 0;
}
