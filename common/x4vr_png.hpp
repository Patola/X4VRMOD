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

/// A decoded image, always 8-bit RGBA, rows top to bottom.
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

/// Decode a PNG held in memory: 8-bit grey, grey+alpha, RGB or RGBA, not
/// interlaced, every row filter. That is what the layer embeds (the loading
/// panel's art and words, made by tools/make_vr_loading.py with Pillow), and
/// anything else is refused rather than half-read. CRCs are not checked: the
/// bytes are compiled in, not read from a disk that could damage them.
inline bool decode(const uint8_t *p, size_t n, Image *out) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!p || n < 8 || memcmp(p, sig, 8) != 0 || !out)
        return false;
    auto be32 = [](const uint8_t *b) {
        return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    };
    uint32_t w = 0, h = 0, ch = 0;
    std::vector<uint8_t> z;
    for (size_t at = 8; at + 12 <= n;) {
        const uint32_t len = be32(p + at);
        const char *tag = (const char *)p + at + 4;
        if (len > n - at - 12)
            return false;
        const uint8_t *d = p + at + 8;
        if (!memcmp(tag, "IHDR", 4)) {
            if (len != 13 || d[8] != 8 || d[10] != 0 || d[11] != 0 || d[12] != 0)
                return false; // 8-bit, deflate, adaptive filters, no interlace
            w = be32(d);
            h = be32(d + 4);
            ch = d[9] == 0 ? 1 : d[9] == 4 ? 2 : d[9] == 2 ? 3 : d[9] == 6 ? 4 : 0;
            if (!w || !h || !ch || w > 16384 || h > 16384)
                return false;
        } else if (!memcmp(tag, "IDAT", 4)) {
            z.insert(z.end(), d, d + len);
        } else if (!memcmp(tag, "IEND", 4)) {
            break;
        }
        at += 12 + (size_t)len;
    }
    if (!ch || z.empty())
        return false;
    const size_t stride = (size_t)w * ch;
    std::vector<uint8_t> raw((size_t)h * (1 + stride));
    uLongf got = (uLongf)raw.size();
    if (uncompress(raw.data(), &got, z.data(), (uLong)z.size()) != Z_OK ||
        got != raw.size())
        return false;
    // The row filters, from the PNG specification: each byte is predicted from
    // the one to its left (a), above (b) and above-left (c).
    std::vector<uint8_t> px((size_t)h * stride);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t f = raw[(size_t)y * (1 + stride)];
        const uint8_t *in = &raw[(size_t)y * (1 + stride) + 1];
        uint8_t *row = &px[(size_t)y * stride];
        const uint8_t *up = y ? row - stride : nullptr;
        for (size_t i = 0; i < stride; i++) {
            const int a = i >= ch ? row[i - ch] : 0, b = up ? up[i] : 0,
                      c = (up && i >= ch) ? up[i - ch] : 0;
            int pred = 0;
            switch (f) {
            case 0: pred = 0; break;
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: {
                const int pp = a + b - c, pa = pp > a ? pp - a : a - pp,
                          pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp;
                pred = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                break;
            }
            default: return false;
            }
            row[i] = (uint8_t)(in[i] + pred);
        }
    }
    out->w = w;
    out->h = h;
    out->rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *s = &px[i * ch];
        uint8_t *o = &out->rgba[i * 4];
        if (ch <= 2) {
            o[0] = o[1] = o[2] = s[0];
            o[3] = ch == 2 ? s[1] : 255;
        } else {
            o[0] = s[0];
            o[1] = s[1];
            o[2] = s[2];
            o[3] = ch == 4 ? s[3] : 255;
        }
    }
    return true;
}

} // namespace png
} // namespace x4vr
