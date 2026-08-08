// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The injector-to-layer channel.
//
// DESIGN.md has specified this since the start and it has never existed, which
// is why the pointer position and the frame have never been in the same place:
// the injector sees `SDL_GetMouseState`, the layer owns the eye image, and
// neither could answer a question about both. Task #17 needs exactly that
// pairing -- a cursor drawn into the eye image is the position from one side
// composited by the other.
//
// **Both live in X4's own process.** The injector arrives by `LD_PRELOAD` and
// the layer is loaded into the same process by the Vulkan loader, so this is
// not interprocess communication and does not need `shm_open`, a file, or a
// lifetime to manage. The injector exports one symbol; the layer resolves it
// with `dlsym(RTLD_DEFAULT, ...)` and gets a pointer to this struct.
//
// It degrades rather than fails. A layer that cannot find the symbol -- the
// injector not preloaded, or a process that has one and not the other, which is
// the normal case for gamescope -- simply has no cursor to draw, and says so
// once instead of assuming a position of (0, 0).
//
// Every field is written by the injector and read by the layer. If that ever
// needs to go the other way, add a second struct rather than making this one
// bidirectional: a single-writer field needs no protocol, and a shared one does.
#pragma once

#include <atomic>
#include <cstdint>

namespace x4vr {

// "X4VR" little-endian, so a wrong or stale mapping is caught rather than
// interpreted. Bump kShareVersion on any layout change; the reader checks both
// and refuses a mismatch instead of reading a field that has moved.
constexpr uint32_t kShareMagic = 0x52565834u;
constexpr uint32_t kShareVersion = 1u;

struct Shared {
    uint32_t magic = kShareMagic;
    uint32_t version = kShareVersion;

    // Seqlock. The writer makes this odd before touching the payload and even
    // after; a reader that sees an odd value, or a different value before and
    // after its read, tries again.
    //
    // Two floats would tear harmlessly in practice, but "in practice" is how a
    // reader ends up with x from one frame and y from the next exactly once,
    // during the measurement that matters. It costs two increments.
    std::atomic<uint32_t> seq{0};

    // Pointer position in **X4's window space** -- 0..W-1, 0..H-1 -- which take
    // 89 measured as exactly the extent SDL reports and hit-tests in. Not
    // display coordinates: the display is the compositor's business and the
    // layer draws into X4's frame, which is the same space as this.
    // Relaxed atomics, not plain floats. The seqlock guards *consistency* --
    // that x and y come from the same update -- but a plain field written by
    // one thread while another reads it is a data race in its own right. These
    // are lock-free on every target this runs on, so it costs nothing.
    std::atomic<float> cursor_x{0.f};
    std::atomic<float> cursor_y{0.f};

    // Whether a cursor should be drawn at all. X4 hides the pointer for
    // mouse-look, and whatever draws it today also hides it after a few idle
    // seconds, so "there is a position" and "a cursor is visible" are different
    // questions and are kept as different fields.
    std::atomic<uint32_t> cursor_visible{0};

    // ---- the cursor image -------------------------------------------------
    //
    // X4 builds its own cursor with SDL_CreateColorCursor and hands it to SDL,
    // which is why the compositor draws one and why it changes shape over a
    // station. Compositing *that* image is the point: a cursor we invent would
    // not match the game's, and X4 already varies it by context.
    //
    // Its own seqlock, separate from the position's. The image changes rarely
    // -- only when X4 switches cursor -- while the position changes every
    // frame, so sharing one counter would make every reader of the image retry
    // constantly for a payload that had not moved.
    std::atomic<uint32_t> cursor_img_seq{0};
    uint32_t cursor_w = 0;
    uint32_t cursor_h = 0;
    int32_t cursor_hot_x = 0;
    int32_t cursor_hot_y = 0;
    // SDL's pixel-format id, **unconverted**. Publishing the raw bytes and the
    // format, rather than converting here, means one run tells us exactly what
    // X4 passes instead of a guessed conversion table silently mangling it.
    uint32_t cursor_format = 0;
    uint32_t cursor_pitch = 0;
    // Capped, because this struct is a fixed allocation and a cursor is small.
    // A larger one is refused and logged rather than truncated into something
    // that would draw as garbage.
    static constexpr uint32_t kCursorMax = 64;
    uint8_t cursor_pixels[kCursorMax * kCursorMax * 4] = {};

    // X4's own window extent, as it reported it. The layer has the eye extent
    // already; this is here so a mismatch between the two can be *seen* rather
    // than assumed away -- they are equal today and nothing guarantees it.
    uint32_t window_w = 0;
    uint32_t window_h = 0;
};

// A snapshot torn-free read of the fields the layer cares about.
inline bool share_read(const Shared *s, float *x, float *y, bool *visible) {
    if (!s || s->magic != kShareMagic || s->version != kShareVersion)
        return false;
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = s->seq.load(std::memory_order_acquire);
        if (a & 1u)
            continue;
        const float cx = s->cursor_x.load(std::memory_order_relaxed);
        const float cy = s->cursor_y.load(std::memory_order_relaxed);
        const bool vis = s->cursor_visible.load(std::memory_order_relaxed) != 0;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->seq.load(std::memory_order_relaxed) != a)
            continue;
        *x = cx;
        *y = cy;
        *visible = vis;
        return true;
    }
    return false;
}

// The cursor image, if one has been captured. `dst` must hold at least
// kCursorMax*kCursorMax*4 bytes. Same seqlock discipline as share_read.
inline bool share_read_cursor(const Shared *s, uint8_t *dst, uint32_t *w,
                              uint32_t *h, int32_t *hot_x, int32_t *hot_y,
                              uint32_t *format, uint32_t *pitch) {
    if (!s || s->magic != kShareMagic || s->version != kShareVersion)
        return false;
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = s->cursor_img_seq.load(std::memory_order_acquire);
        if (a == 0 || (a & 1u))
            continue; // 0 = nothing captured yet, odd = being written
        const uint32_t cw = s->cursor_w, ch = s->cursor_h;
        if (!cw || !ch || cw > Shared::kCursorMax || ch > Shared::kCursorMax)
            return false;
        const uint32_t bytes = cw * ch * 4u;
        for (uint32_t i = 0; i < bytes; i++)
            dst[i] = s->cursor_pixels[i];
        const uint32_t fmt = s->cursor_format, pit = s->cursor_pitch;
        const int32_t hx = s->cursor_hot_x, hy = s->cursor_hot_y;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->cursor_img_seq.load(std::memory_order_relaxed) != a)
            continue;
        *w = cw;
        *h = ch;
        *hot_x = hx;
        *hot_y = hy;
        *format = fmt;
        *pitch = pit;
        return true;
    }
    return false;
}

} // namespace x4vr

// The one exported symbol. `extern "C"` so the name is not mangled and the
// layer can name it as a string.
extern "C" x4vr::Shared *x4vr_shared_state();
