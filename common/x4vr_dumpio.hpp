// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Pixel transforms for the dump writers — separated from the layer so they can
// be tested, and rewritten so they touch the mapped readback buffer ONCE.
//
// **Take 259 measured where the flush's 2378 ms per frame went**, after three
// offline hypotheses had each been measured and each been wrong:
//
//     QueueWaitIdle 2 ms · eye 1098 · ui write 203 · coverage 1076 · OTHER 0
//
// OTHER is zero, so that is the whole cost. Both hot terms read the mapped
// buffer ONE BYTE AT A TIME:
//
//   * write_ppm8 did `rgb[...] = bgra ? p[2 - c] : p[c]` — three separate byte
//     loads per pixel, with a runtime condition inside the inner loop;
//   * the flush then counted UI coverage in a SECOND per-pixel pass over the
//     same bytes, with data-dependent branches.
//
// That memory is HOST_VISIBLE|HOST_COHERENT and — being first-match — not
// HOST_CACHED, so it is write-combined: every scalar load is an uncached round
// trip that no streaming path can coalesce. Measured on this GPU, a scalar
// read of one 1408x1408 layer costs 81 ms against 22 ms for a bulk memcpy of
// the same bytes, and in situ the gap is larger still.
//
// So: **copy the layer into ordinary RAM once, in bulk, then do every
// per-pixel operation there.** The remedy does not depend on the diagnosis
// being right — it converts every WC read into a single vectorisable memcpy
// whatever the reason scalar reads are slow.
//
// An earlier offline test concluded the memory type was worth only 3.5% and
// that conclusion was wrong: it keyed on write_png_n, a BULK read of an RGBA
// buffer, which is not what either hot path does. An instrument that measures
// an operation the code does not perform answers nothing.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace x4vr {
namespace dump {

// What the UI mask covers, counted on the RAW source bytes.
//
// `colour` is "any of the three colour channels non-zero" and `alpha` is "the
// alpha byte non-zero". Both criteria, because which of them IS the UI is
// unsettled: alpha is X4's own composite coverage, colour is what survives if
// the composite ignores it — and X4 writes the weapon arcs and the bracket
// rectangles with colour and ZERO alpha, which is why alpha alone is the wrong
// mask.
struct Coverage {
    uint64_t colour = 0, alpha = 0;
};

// One bulk read of the mapped buffer. Everything else in this header works
// from the result.
inline void snapshot(std::vector<uint8_t> &dst, const uint8_t *src,
                     size_t bytes) {
    dst.resize(bytes);
    if (bytes)
        memcpy(dst.data(), src, bytes);
}

// 4-channel source -> 3-channel RGB. `bgra` swaps R and B.
//
// The channel order is hoisted OUT of the inner loop. It is a per-image
// constant and testing it per pixel is what stopped the old loop from
// vectorising.
inline void to_rgb(std::vector<uint8_t> &out, const uint8_t *src, uint32_t w,
                   uint32_t h, bool bgra) {
    const size_t n = (size_t)w * h;
    out.resize(n * 3);
    if (bgra) {
        for (size_t i = 0; i < n; i++) {
            out[i * 3 + 0] = src[i * 4 + 2];
            out[i * 3 + 1] = src[i * 4 + 1];
            out[i * 3 + 2] = src[i * 4 + 0];
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            out[i * 3 + 0] = src[i * 4 + 0];
            out[i * 3 + 1] = src[i * 4 + 1];
            out[i * 3 + 2] = src[i * 4 + 2];
        }
    }
}

// 4-channel source -> RGBA with alpha kept, AND the coverage, in ONE pass.
//
// Counting here rather than in a second loop is where 45% of take 259's flush
// went. The count is over the SOURCE bytes and is order-independent for
// `colour` — it ORs the same three bytes whichever permutation they are in —
// so folding it in cannot change the numbers a previous run reported.
inline Coverage to_rgba_counted(std::vector<uint8_t> &out, const uint8_t *src,
                                uint32_t w, uint32_t h, bool bgra) {
    const size_t n = (size_t)w * h;
    out.resize(n * 4);
    Coverage c;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = src + i * 4;
        uint8_t *o = &out[i * 4];
        if (bgra) {
            o[0] = p[2];
            o[1] = p[1];
            o[2] = p[0];
        } else {
            o[0] = p[0];
            o[1] = p[1];
            o[2] = p[2];
        }
        o[3] = p[3];
        if (p[0] | p[1] | p[2])
            c.colour++;
        if (p[3])
            c.alpha++;
    }
    return c;
}

} // namespace dump
} // namespace x4vr
