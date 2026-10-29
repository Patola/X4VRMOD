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
// 2: task #75 added the raw camera readback and its timestamp beside the
// injector's belief, so the declared pose and the pose X4 actually reports can
// appear on one line instead of being paired across two logs after the fact.
// 3: that block got the seqlock it should have had from the start (`cam_seq`).
// 4: `fullscreen_menu`, so the layer can draw the cursor through the UI canvas
// only while X4 shows a full-screen menu (X4VR_CURSOR_CANVAS=auto, take 327).
// 5: `steer_line` and `steer_mode`, the mouse-steering target A/B
//    (x4vr_steer.hpp).
// 6: `on_foot`, head-look writing off the pilot seat (X4VR_HEADLOOK_FOOT).
// 7: `foot_menu`, an on-foot menu holding the pointer (take 338).
// 8: `centred_menu`, a HUD-down menu head-look centred for (take 339).
// 9: `interact_menu`, X4's right-click menu open (take 342).
// 10: `external_head`, the head turning an external view
//     (X4VR_HEADLOOK_EXTERNAL).
// 11: `cinematic_ui`, the cinematic view's overlay at the full window
//     (take 357).
constexpr uint32_t kShareVersion = 11u;

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

    // #33: where the injector believes it has driven X4's camera. The layer
    // submits this as the composition layer's orientation, because that is the
    // pose the frame was RENDERED from. Not the head pose: X4's camera lags it
    // and clamps at 65.00 deg yaw / 35.00 pitch (take 136, measured off X4's
    // own readback -- this said 56.5, which was take 117's integrated estimate
    // and was superseded), and declaring a pose X4 did not render from is what
    // makes the world swim.
    // **Its own seqlock, and the note at the top of this file is why.** That
    // note says two floats "would tear harmlessly in practice, but 'in practice'
    // is how a reader ends up with x from one frame and y from the next exactly
    // once, during the measurement that matters" -- and then this whole block
    // was added underneath it without one. It happened exactly as written.
    //
    // Two things went wrong, and the second is the worse one:
    //
    //   the DECLARED pose  yaw from one tick with pitch from the next is an
    //                      orientation the frame was never rendered from, and
    //                      the compositor reprojects by it. At the 20-30 deg per
    //                      frame takes 285b/287b reached, that is a large warp
    //                      applied to pixels that never had it.
    //   the MEASUREMENT    `poselag` loaded the readback near the top of the
    //                      submission block and `cam_yaw_deg` again a hundred
    //                      lines further down, so `x4=` and `cmd=` on one line
    //                      could come from ticks an unbounded distance apart.
    //                      Task #75's uncommanded-motion residual was measured
    //                      by subtracting one from the other.
    //
    // Separate from `seq` because the cursor moves on X4's input path and the
    // camera on the servo tick; sharing a counter would make each reader retry
    // on the other's writes for a payload that had not moved.
    std::atomic<uint32_t> cam_seq{0};

    std::atomic<float> cam_yaw_deg{0.f};
    std::atomic<float> cam_pitch_deg{0.f};
    std::atomic<uint32_t> cam_valid{0};

    // ---- task #75: what X4 ACTUALLY reported, beside what we declared ------
    //
    // `cam_yaw_deg` above is the injector's BELIEF, and it is stored after the
    // servo step -- so it already contains a command X4 will not receive for
    // about two ticks. The layer declares it as the pose the frame was
    // rendered from, and the compositor reprojects by it. If the belief runs
    // ahead of the pixels, the compositor warps the frame by an angle it was
    // never drawn with, and the world swims exactly when the head moves.
    //
    // Nothing measured that, because no line ever carried both numbers. The
    // one attempt paired the layer's periodic `vr pose` against `camread` and
    // got 6-7 deg from seven samples across a 0.25 s window -- during which the
    // camera moves further than the effect. That is not a measurement.
    //
    // So the injector publishes X4's raw readback next to its belief, stamped
    // with when it was taken, and the layer logs all of it on ONE line at
    // submission. The stamp is what makes it honest: the readback is older than
    // the frame, and by how much has to be visible rather than assumed small.
    std::atomic<float> cam_raw_yaw_deg{0.f};
    std::atomic<float> cam_raw_pitch_deg{0.f};
    // 0 when GetCameraRotation reported nothing this tick. X4 returns exactly
    // 0.0000,0.0000 while loading and in some views, which is "no answer", not
    // "centred" -- 66% of the samples across 15 stored takes.
    std::atomic<uint32_t> cam_raw_valid{0};
    // CLOCK_MONOTONIC nanoseconds when the readback was taken. Same clock the
    // log prefix uses, so a line's own timestamp minus this IS the age.
    std::atomic<uint64_t> cam_raw_ns{0};

    // X4's `IsFullscreenMenuDisplayed(true, NULL)`, polled on X4's main thread
    // each tick while X4VR_CURSOR_CANVAS=auto. Where the pointer is hit-tested
    // depends on it: a full-screen menu (the map) hit-tests in X4's UI, which
    // the layer shows scaled by the canvas map (take 311), while in flight X4
    // picks the world at the raw pointer (take 314). 1 = a menu is up.
    std::atomic<uint32_t> fullscreen_menu{0};

    // The mouse-steering target (x4vr_steer.hpp), published by the injector
    // on X4's main thread each tick. `steer_line`: 1 while X4 would show its
    // own steering line, its option aside -- the bytes
    // ShouldShowMouseSteeringHelperLine reads, with the cockpit view and the
    // HUD up. `steer_mode`: steer::Mode, kOff unless X4VR_STEER asked for the
    // overlay; the apostrophe key flips it between A and B.
    std::atomic<uint32_t> steer_line{0};
    std::atomic<uint32_t> steer_mode{0};

    // 1 while head-look writes OFF the pilot seat (X4VR_HEADLOOK_FOOT, the
    // patched 0x1929f70). There GetCameraRotation is the player's body times
    // the head, so the layer declares head-look's written angle -- the head
    // part X4 composes -- instead of the camera it samples at the first pass:
    // declaring the body too is what swung the window in take 334.
    std::atomic<uint32_t> on_foot{0};

    // 1 while an on-foot menu has the pointer and the cursor is to be drawn
    // through the UI canvas (X4VR_CURSOR_CANVAS=auto). On foot X4 holds the
    // pointer in relative mode and frees it only for its interact menu and
    // NPC dialogs, which are UI and hit-test there -- take 338 drew the
    // cursor raw over them and it sat off the highlighted option. Under
    // X4VR_CURSOR_FOOT_AB only the canvas arm's openings set it.
    std::atomic<uint32_t> foot_menu{0};

    // 1 while X4VR_HEADLOOK_MENU_CENTRE holds X4's camera at 0,0 for a menu
    // shown with the HUD down, and the cursor is to be drawn through the UI
    // canvas. Take 339: off the seat the station teleporters are such menus
    // and X4 calls none of them full-screen, so the cursor stayed raw over
    // them. Under X4VR_CURSOR_CENTRED_AB only the canvas arm's periods off
    // the seat set it.
    std::atomic<uint32_t> centred_menu{0};

    // 1 while X4's table of open menus holds "InteractMenu" -- the cockpit's
    // right-click menu, which X4 does not call full-screen, so the cursor
    // stayed raw over it and moved 1.9x as far as its highlight (take 340's
    // video). The table is READ (x4vr_menus.hpp); take 342 found the entry
    // appear 70-100 ms after each right-click and leave when the HUD's
    // TopLevelMenu came back.
    std::atomic<uint32_t> interact_menu{0};

    // 1 while the stub at 0x192a295 composes head-look's angle into an
    // external view (X4VR_HEADLOOK_EXTERNAL, x4vr_extview.hpp). There
    // GetCameraRotation is X4's orbit times the head, so -- as on foot --
    // the layer declares head-look's angle (the camera snapshot, valid) and
    // not the camera it samples: declaring the orbit too would swing the
    // picture with the numpad.
    std::atomic<uint32_t> external_head{0};

    // 1 while the cinematic view (F6) is up: in the pilot seat, the HUD
    // down, not paused, no conversation and nothing in X4's open-menu table
    // but TopLevelMenu, held 0.3 s (takes 354-358: every F6 stretch and no
    // other). Its overlay is laid out for the whole flat screen, so the layer
    // binds each canvas pipeline's twin and X4's UI is drawn as X4 laid it
    // out (Patola, take 357: "It should be bigger and cover the entire
    // window"). Also while the START MENU is up (X4's OptionsMenu, full
    // screen, with no player camera), whose menu was shrunk the same way.
    std::atomic<uint32_t> cinematic_ui{0};
};

// ---- the camera block, as one consistent snapshot ------------------------
//
// Every field the servo tick publishes about the camera, read together or not
// at all. `CamSnap` exists so a caller cannot take half of it by accident: the
// layer used to load the readback near the top of its submission block and the
// command a hundred lines further down, which is not a tear of one tick but of
// however long the code in between takes.
struct CamSnap {
    float yaw_deg = 0.0f;       // the servo's BELIEF, stored after its step
    float pitch_deg = 0.0f;
    bool valid = false;         // steering
    float raw_yaw_deg = 0.0f;   // what GetCameraRotation actually reported
    float raw_pitch_deg = 0.0f;
    bool raw_valid = false;
    uint64_t raw_ns = 0;
};

// Publish the whole block. Called from the servo tick, the only writer.
inline void share_write_camera(Shared *s, const CamSnap &c) {
    if (!s)
        return;
    s->cam_seq.fetch_add(1, std::memory_order_relaxed); // -> odd
    std::atomic_thread_fence(std::memory_order_release);
    s->cam_yaw_deg.store(c.yaw_deg, std::memory_order_relaxed);
    s->cam_pitch_deg.store(c.pitch_deg, std::memory_order_relaxed);
    s->cam_valid.store(c.valid ? 1u : 0u, std::memory_order_relaxed);
    s->cam_raw_yaw_deg.store(c.raw_yaw_deg, std::memory_order_relaxed);
    s->cam_raw_pitch_deg.store(c.raw_pitch_deg, std::memory_order_relaxed);
    s->cam_raw_valid.store(c.raw_valid ? 1u : 0u, std::memory_order_relaxed);
    s->cam_raw_ns.store(c.raw_ns, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->cam_seq.fetch_add(1, std::memory_order_relaxed); // -> even
}

// Read it. Same discipline as share_read.
//
// **`out` is carried, not filled.** On failure `*out` is left exactly as the
// caller had it, so a caller that keeps one `CamSnap` across frames gets the
// last complete snapshot -- a frame stale, never inconsistent -- rather than a
// pose of zero. That matters at the declaration site: refusing there and
// leaving `cv` false declares IDENTITY to the compositor, which pops the world
// lock for a frame, and is a worse artefact than the tear this replaces.
//
// Both callers therefore hold a static snapshot. A fresh `CamSnap` per call
// would give zeroes on refusal and the guarantee above would be a comment
// describing behaviour the code does not have.
inline bool share_read_camera(const Shared *s, CamSnap *out) {
    if (!s || !out || s->magic != kShareMagic || s->version != kShareVersion)
        return false;
    // 32, not 8. The writer holds the block open for seven relaxed stores, so a
    // reader that gives up after eight attempts refuses constantly whenever the
    // two threads are in step -- measured at 98% refusal against a hot writer in
    // tests/share.cpp. Refusals are not free here: each one is a frame served
    // from the previous snapshot.
    for (int tries = 0; tries < 32; tries++) {
        const uint32_t a = s->cam_seq.load(std::memory_order_acquire);
        if (a & 1u)
            continue;
        CamSnap c;
        c.yaw_deg = s->cam_yaw_deg.load(std::memory_order_relaxed);
        c.pitch_deg = s->cam_pitch_deg.load(std::memory_order_relaxed);
        c.valid = s->cam_valid.load(std::memory_order_relaxed) != 0;
        c.raw_yaw_deg = s->cam_raw_yaw_deg.load(std::memory_order_relaxed);
        c.raw_pitch_deg = s->cam_raw_pitch_deg.load(std::memory_order_relaxed);
        c.raw_valid = s->cam_raw_valid.load(std::memory_order_relaxed) != 0;
        c.raw_ns = s->cam_raw_ns.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->cam_seq.load(std::memory_order_relaxed) != a)
            continue;
        *out = c;
        return true;
    }
    return false;
}

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

// ---- the other direction: the head pose, layer -> injector ----------------
//
// A *second* struct rather than fields added to `Shared`, exactly as the note
// at the top of this file requires: every field of `Shared` is written by the
// injector, every field of this one is written by the layer, and a struct with
// a single writer needs no protocol where a shared one would.
//
// The storage still lives in the **injector**, and the layer still finds it by
// `dlsym`. That is not symmetry for its own sake: the injector arrives by
// `LD_PRELOAD` so its symbols are in the global namespace, while the layer is
// `dlopen`ed by the Vulkan loader and its are not reliably visible to anyone.
// Only one of the two can be found by name, so it has to be the one that owns
// both allocations.
//
// #33 needs this because the two halves of head-look sit on opposite sides of
// the process: OpenXR (and therefore the head pose) is in the layer, and the
// SDL input X4 reads is in the injector.
struct HeadShared {
    uint32_t magic = kShareMagic;
    uint32_t version = kShareVersion;

    // Same seqlock discipline as above, for the same reason: yaw and pitch
    // must come from one sample of one pose, never one of each from two.
    std::atomic<uint32_t> seq{0};

    std::atomic<float> yaw_deg{0.f};
    std::atomic<float> pitch_deg{0.f};

    // A located pose, not merely a session. The runtime returns poses without
    // XR_VIEW_STATE_ORIENTATION_VALID_BIT while tracking is establishing, and
    // driving X4's camera from those would swing the view to wherever an
    // uninitialised pose points. The injector must gate on this, not on the
    // struct existing.
    std::atomic<uint32_t> valid{0};

    // Monotonic, so the injector can tell "the head is being held still" from
    // "the layer stopped publishing". Those look identical in the angles and
    // want opposite responses.
    std::atomic<uint64_t> updates{0};
};

inline void head_share_write(HeadShared *s, float yaw, float pitch, bool valid) {
    if (!s)
        return;
    s->seq.fetch_add(1, std::memory_order_release);
    s->yaw_deg.store(yaw, std::memory_order_relaxed);
    s->pitch_deg.store(pitch, std::memory_order_relaxed);
    s->valid.store(valid ? 1u : 0u, std::memory_order_relaxed);
    s->updates.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->seq.fetch_add(1, std::memory_order_release);
}

inline bool head_share_read(const HeadShared *s, float *yaw, float *pitch,
                            uint64_t *updates) {
    if (!s || s->magic != kShareMagic || s->version != kShareVersion)
        return false;
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = s->seq.load(std::memory_order_acquire);
        if (a & 1u)
            continue;
        const float y = s->yaw_deg.load(std::memory_order_relaxed);
        const float p = s->pitch_deg.load(std::memory_order_relaxed);
        const bool ok = s->valid.load(std::memory_order_relaxed) != 0;
        const uint64_t u = s->updates.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s->seq.load(std::memory_order_relaxed) != a)
            continue;
        if (!ok)
            return false;
        *yaw = y;
        *pitch = p;
        if (updates)
            *updates = u;
        return true;
    }
    return false;
}

} // namespace x4vr

// The exported symbols. `extern "C"` so the names are not mangled and the
// layer can name them as strings.
extern "C" x4vr::Shared *x4vr_shared_state();
extern "C" x4vr::HeadShared *x4vr_head_state();
