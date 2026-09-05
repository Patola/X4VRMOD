// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The dump format, round-tripped. Self-checking: no arguments, non-zero exit on
// the first bad case, no runner script.
//
// **Why this exists.** Take 177 FAILED with "no present dumps were found" while
// twenty of them sat on disk: the layer had switched PPM -> PNG and the
// analysers still globbed `.ppm`. tests/run-dump-format.sh was written to stop
// that recurring, and it checks the layer against the analysers -- but it could
// never check the ENCODER, because the encoder lived inside the layer's
// translation unit where nothing offline could call it.
//
// The RGBA variant is new (task #57's UI-target dump) and its alpha is not
// decoration: it is the UI's coverage, and therefore the mask the whole
// instrument rests on. An encoder bug there would not crash anything -- it
// would produce a plausible file with a wrong mask, which is the failure this
// project keeps paying for. So the round trip is decoded here, in full, rather
// than eyeballed.
//
// The decoder below is written independently of the encoder on purpose: it
// parses the chunk stream, checks the CRCs, and undoes the row filters from the
// PNG specification rather than from what write_png_n happens to emit. A
// decoder derived from the encoder would agree with it about a shared mistake.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "../common/x4vr_png.hpp"

namespace {

int fails = 0;

void say(const char *name, bool ok, const std::string &detail) {
    printf("%s %-52s %s\n", ok ? "ok  " : "FAIL", name, detail.c_str());
    if (!ok)
        fails++;
}

uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

struct Decoded {
    bool ok = false;
    std::string why;
    uint32_t w = 0, h = 0, ch = 0;
    std::vector<uint8_t> px;
};

// A PNG reader that knows nothing about how the file was produced.
Decoded decode(const std::string &path) {
    Decoded d;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        d.why = "file not written at all";
        return d;
    }
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (buf.size() < 8 || memcmp(buf.data(), sig, 8) != 0) {
        d.why = "bad signature";
        return d;
    }
    std::vector<uint8_t> idat;
    size_t i = 8;
    bool saw_ihdr = false, saw_iend = false;
    uint8_t depth = 0, colour = 0;
    while (i + 12 <= buf.size()) {
        const uint32_t len = be32(&buf[i]);
        const char *tag = (const char *)&buf[i + 4];
        if (i + 12 + len > buf.size()) {
            d.why = "chunk runs past end of file";
            return d;
        }
        // The CRC covers the tag and the data, and a wrong one is exactly what
        // a hand-rolled writer gets wrong, so it is checked and not skipped.
        uLong c = crc32(0, (const Bytef *)&buf[i + 4], 4 + len);
        if ((uint32_t)c != be32(&buf[i + 8 + len])) {
            d.why = std::string("CRC mismatch on ") + std::string(tag, 4);
            return d;
        }
        if (!memcmp(tag, "IHDR", 4)) {
            if (len != 13) {
                d.why = "IHDR is not 13 bytes";
                return d;
            }
            saw_ihdr = true;
            d.w = be32(&buf[i + 8]);
            d.h = be32(&buf[i + 12]);
            depth = buf[i + 16];
            colour = buf[i + 17];
            if (buf[i + 18] || buf[i + 19] || buf[i + 20]) {
                d.why = "compression/filter/interlace method is not 0";
                return d;
            }
        } else if (!memcmp(tag, "IDAT", 4)) {
            idat.insert(idat.end(), &buf[i + 8], &buf[i + 8 + len]);
        } else if (!memcmp(tag, "IEND", 4)) {
            saw_iend = true;
        }
        i += 12 + len;
    }
    if (!saw_ihdr || !saw_iend) {
        d.why = "missing IHDR or IEND";
        return d;
    }
    if (depth != 8) {
        d.why = "bit depth is not 8";
        return d;
    }
    if (colour == 2)
        d.ch = 3;
    else if (colour == 6)
        d.ch = 4;
    else {
        d.why = "colour type is neither 2 (RGB) nor 6 (RGBA)";
        return d;
    }

    const size_t stride = (size_t)d.w * d.ch;
    std::vector<uint8_t> raw((size_t)d.h * (1 + stride));
    uLongf out = (uLongf)raw.size();
    if (uncompress(raw.data(), &out, idat.data(), (uLong)idat.size()) != Z_OK ||
        out != raw.size()) {
        d.why = "zlib stream did not inflate to h*(1+w*ch) bytes";
        return d;
    }
    // Undo the filters, per the PNG spec's five types. write_png_n only emits
    // 0, but decoding all five means this test would still pass if the writer
    // ever started filtering -- it asserts the FILE is correct, not that the
    // writer took one particular route to it.
    d.px.assign((size_t)d.h * stride, 0);
    for (uint32_t y = 0; y < d.h; y++) {
        const uint8_t ft = raw[(size_t)y * (1 + stride)];
        const uint8_t *src = &raw[(size_t)y * (1 + stride) + 1];
        uint8_t *cur = &d.px[(size_t)y * stride];
        const uint8_t *up = y ? &d.px[(size_t)(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; x++) {
            const int a = x >= d.ch ? cur[x - d.ch] : 0;
            const int b = up ? up[x] : 0;
            const int c = (up && x >= d.ch) ? up[x - d.ch] : 0;
            int v = src[x];
            switch (ft) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = abs(p - a), pb = abs(p - b),
                          pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default:
                d.why = "unknown row filter";
                return d;
            }
            cur[x] = (uint8_t)v;
        }
    }
    d.ok = true;
    return d;
}

std::string tmp_path(const char *name) {
    const char *dir = getenv("TMPDIR");
    return std::string(dir && *dir ? dir : "/tmp") + "/x4vr-png-test-" + name +
           ".png";
}

// A pattern where every channel is a DIFFERENT function of position, so a
// writer that duplicates or drops a channel cannot pass. Alpha in particular
// varies independently of the colour: the UI dump's whole value is that alpha
// says something the colour does not.
void fill(std::vector<uint8_t> &px, uint32_t w, uint32_t h, uint32_t ch) {
    px.assign((size_t)w * h * ch, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = &px[((size_t)y * w + x) * ch];
            p[0] = (uint8_t)(x * 7 + 1);
            p[1] = (uint8_t)(y * 13 + 2);
            p[2] = (uint8_t)((x ^ y) * 3 + 5);
            if (ch == 4)
                // Deliberately zero over a large connected region: that is what
                // a UI image looks like, and an encoder that mishandles runs of
                // zero alpha would produce a mask covering the whole screen.
                p[3] = (x < w / 3 && y < h / 3) ? 0 : (uint8_t)(x + y * 5 + 17);
        }
}

void round_trip(const char *name, uint32_t w, uint32_t h, uint32_t ch) {
    std::vector<uint8_t> px;
    fill(px, w, h, ch);
    const std::string path = tmp_path(name);
    remove(path.c_str());
    x4vr::png::write_png_n(path.c_str(), px.data(), w, h, ch);

    const Decoded d = decode(path);
    char buf[256];
    if (!d.ok) {
        say(name, false, d.why);
        remove(path.c_str());
        return;
    }
    if (d.w != w || d.h != h || d.ch != ch) {
        snprintf(buf, sizeof buf, "decoded %ux%u x%u, wrote %ux%u x%u", d.w,
                 d.h, d.ch, w, h, ch);
        say(name, false, buf);
        remove(path.c_str());
        return;
    }
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < px.size(); i++)
        if (px[i] != d.px[i]) {
            if (!bad)
                first = i;
            bad++;
        }
    if (bad) {
        snprintf(buf, sizeof buf,
                 "%zu/%zu bytes differ, first at %zu (wrote %u, read %u)", bad,
                 px.size(), first, px[first], d.px[first]);
        say(name, false, buf);
    } else {
        snprintf(buf, sizeof buf, "%ux%u x%u channels, %zu bytes exact", w, h,
                 ch, px.size());
        say(name, true, buf);
    }
    remove(path.c_str());
}

// The alpha channel has to survive as ITS OWN quantity. A writer that filled
// alpha from a colour channel would still round-trip byte-for-byte above if the
// pattern happened to agree, so this asserts the two disagree in the source and
// still disagree after the trip.
void alpha_is_independent() {
    const uint32_t w = 61, h = 37;
    std::vector<uint8_t> px;
    fill(px, w, h, 4);
    size_t differs = 0, zero_alpha = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (px[i * 4 + 3] != px[i * 4] && px[i * 4 + 3] != px[i * 4 + 1] &&
            px[i * 4 + 3] != px[i * 4 + 2])
            differs++;
        if (!px[i * 4 + 3])
            zero_alpha++;
    }
    char buf[192];
    snprintf(buf, sizeof buf,
             "%zu/%u px have an alpha unequal to all three colours, %zu are "
             "fully transparent",
             differs, w * h, zero_alpha);
    // Not a property of the encoder -- a property of the TEST, checked so a
    // future edit to fill() cannot quietly make the round trip vacuous.
    say("the pattern can actually detect a channel mix-up",
        differs > (size_t)w * h / 2 && zero_alpha > 0, buf);
}

void refusals() {
    const std::string path = tmp_path("refused");
    const uint8_t px[16] = {0};
    struct {
        const char *why;
        uint32_t w, h, ch;
    } cases[] = {
        {"ch=1 is not a format this writes", 2, 2, 1},
        {"ch=2 is not a format this writes", 2, 2, 2},
        {"zero width", 0, 2, 3},
        {"zero height", 2, 0, 3},
    };
    for (const auto &c : cases) {
        remove(path.c_str());
        x4vr::png::write_png_n(path.c_str(), px, c.w, c.h, c.ch);
        FILE *f = fopen(path.c_str(), "rb");
        // Refusing must leave NO file. A truncated or empty one would be
        // globbed by the analysers and read as a frame that decoded to
        // nothing, which is the "empty mask" reading this instrument must
        // never produce by accident.
        say(c.why, f == nullptr, f ? "a file was written anyway" : "no file");
        if (f)
            fclose(f);
    }
    remove(path.c_str());
}

} // namespace

int main() {
    // Odd sizes on purpose: a stride computed as w*ch is right for every width,
    // and a stride rounded to anything is not.
    round_trip("rgb round trip, even size", 64, 48, 3);
    round_trip("rgb round trip, odd size", 61, 37, 3);
    round_trip("rgba round trip, even size", 64, 48, 4);
    round_trip("rgba round trip, odd size", 61, 37, 4);
    round_trip("rgba round trip, single pixel", 1, 1, 4);
    round_trip("rgba round trip, one row", 129, 1, 4);
    round_trip("rgba round trip, one column", 1, 129, 4);
    alpha_is_independent();
    refusals();

    if (fails) {
        printf("\n%d case(s) FAILED\n", fails);
        return 1;
    }
    printf("\nall cases passed\n");
    return 0;
}
