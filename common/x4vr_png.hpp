// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_png.hpp — the dump format, in one place so a test can reach it.
//
// This lived inside layer/x4vr_layer.cpp, where nothing offline could call it.
// Take 177 is why that matters: the layer switched from PPM to PNG, the
// analysers still globbed `.ppm`, and the run FAILED with "no present dumps
// were found" while twenty of them sat on disk. tests/run-dump-format.sh was
// written to stop that recurring, and it can only assert what it can invoke.
//
// Lossless, because these dumps are PHOTOMETRY: tools/bright_object.py takes
// luminance ratios off them, and a lossy artefact around a saturated Sun would
// land exactly where the measurement is taken.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <zlib.h>

namespace x4vr {
namespace png {

/// Write `px` as an 8-bit PNG. `ch` is 3 for RGB or 4 for RGBA.
///
/// **The alpha case is not decoration.** X4 renders its entire UI into an image
/// of its own and composites that over the scene, so that image's alpha is the
/// UI's own coverage — a mask by construction, which is the thing every
/// downstream attempt to recognise the UI by colour failed to produce. Writing
/// RGB there would discard the one channel that might BE the mask. "Might" is
/// the honest word: the colour is kept too, and the offline tool reports which
/// of the two actually separates rather than betting on one here.
///
/// Emits one IDAT from a single compress2 call, with every row unfiltered:
/// simple, and the sizes here are nowhere near where streaming would matter.
/// Level 1 measures 35 ms for a 1408x1408 layer, against the ~1 s that a dump's
/// QueueWaitIdle costs — so compression is not what makes a dump expensive.
///
/// Silent on failure, like every other writer on this path: a diagnostic that
/// aborts the frame it is diagnosing is worse than a missing file, and the
/// caller's log line reports the count that resulted.
inline void write_png_n(const char *path, const uint8_t *px, uint32_t w,
                        uint32_t h, uint32_t ch) {
    if (!path || !px || !w || !h || (ch != 3 && ch != 4))
        return;
    const size_t stride = (size_t)w * ch;
    std::vector<uint8_t> raw((size_t)h * (1 + stride));
    for (uint32_t y = 0; y < h; y++) {
        raw[(size_t)y * (1 + stride)] = 0; // filter: none
        memcpy(&raw[(size_t)y * (1 + stride) + 1], px + (size_t)y * stride,
               stride);
    }
    uLongf cap = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(cap);
    if (compress2(z.data(), &cap, raw.data(), (uLong)raw.size(), 1) != Z_OK)
        return;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    auto be32 = [](uint8_t *o, uint32_t v) {
        o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = (uint8_t)v;
    };
    auto chunk = [&](const char *tag, const uint8_t *d, uint32_t n) {
        uint8_t hdr[4];
        be32(hdr, n);
        fwrite(hdr, 1, 4, f);
        fwrite(tag, 1, 4, f);
        if (n)
            fwrite(d, 1, n, f);
        uLong c = crc32(0, (const Bytef *)tag, 4);
        if (n)
            c = crc32(c, (const Bytef *)d, n);
        uint8_t cb[4];
        be32(cb, (uint32_t)c);
        fwrite(cb, 1, 4, f);
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;               // bit depth
    ihdr[9] = ch == 4 ? 6 : 2; // truecolour, with alpha when ch == 4
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), (uint32_t)cap);
    chunk("IEND", nullptr, 0);
    fclose(f);
}

inline void write_png_rgb(const char *path, const uint8_t *rgb, uint32_t w,
                          uint32_t h) {
    write_png_n(path, rgb, w, h, 3);
}

} // namespace png
} // namespace x4vr
