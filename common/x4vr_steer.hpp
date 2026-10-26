// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_steer.hpp — the mouse-steering target in VR, A/B with a live toggle
// (Patola, after take 328: "B looks like a better option, but A also feels
// interesting, so yes, I want a toggle").
//
// X4 has ONE pointer. It steers by the pointer's offset from its screen centre
// -- the gaze, under head-look -- and picks the world at the raw pointer (take
// 314). Its helper line runs from the crosshair (the ship's nose, fixed to the
// cockpit) to the pointer's position on the HUD plane. Take 328B's circles
// measured that end at gaze + 0.5252 x the pointer's offset in all four head
// directions (residuals 0.3-3.1 px), while our ring cursor sits at the raw
// pointer, 1.904x farther out. With the head turned, X4's line therefore
// starts far from where the steering is measured from and ends short of the
// cursor.
//
// So X4's line is switched off (its own `mousesteeringline` option, served by
// the config fork) and the layer draws one in eye space instead:
//
//   B (kGaze, the default): the ring stays at the raw pointer, where X4 picks;
//     the line runs from the eye's centre -- the gaze, the centre X4 steers
//     from -- to the ring. It draws exactly the vector X4 steers by.
//   A (kNose): the ring is drawn at the nose plus the same offset, and the
//     line from the nose to it: the target rides with the cockpit. Hover and
//     click-select stay at the raw pointer, off by the head angle while
//     steering -- the stated cost of A.
//
// Everything here is arithmetic on numbers the caller supplies, so
// tests/steer.cpp checks it without X4.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace x4vr {
namespace steer {

enum Mode : uint32_t { kOff = 0, kGaze = 1, kNose = 2 };

inline const char *mode_name(uint32_t m) {
    return m == kGaze ? "B (the line from where you look)"
           : m == kNose ? "A (the target fixed to the cockpit)"
                        : "off (X4's own line)";
}

// X4VR_STEER: unset, empty, "0" or "off" -> kOff (X4's own line, every take
// before 331); "b"/"gaze" -> kGaze; "a"/"nose" -> kNose. The apostrophe key
// flips between A and B live; it never turns the overlay off, so a run
// without the knob is exactly the previous behaviour.
inline Mode mode_from_env(const char *e) {
    if (!e || !*e)
        return kOff;
    if (!strcmp(e, "b") || !strcmp(e, "B") || !strcmp(e, "gaze"))
        return kGaze;
    if (!strcmp(e, "a") || !strcmp(e, "A") || !strcmp(e, "nose"))
        return kNose;
    return kOff;
}

// ---- when X4 would show its line -------------------------------------------
//
// X4 9.00's ShouldShowMouseSteeringHelperLine (0xa45cb0), read:
//
//   option("mousesteeringline", default true)
//     && (in[0x9f36] || in[0x9f37]) && in[0x9f42]
//     && (!in[0x9f39] || in[0xa010])
//
// where `in` is the object at global 0x3daaaf8 -- the one
// IsMouseSteeringActive (0xa45940) returns byte 0x9f36 of WITH NO NULL CHECK.
// Our line replaces X4's, so it is shown on the same condition minus the
// option we switch off; the bytes are READ (process_vm_readv), never called
// for, so a not-yet-built object is a failed read, not a crash. The Lua side
// adds `private.crosshairActive`, which the caller supplies as the cockpit
// with the HUD up.
constexpr uintptr_t kInputGlobal = 0x3daaaf8;
constexpr uintptr_t kActive = 0x9f36, kActive2 = 0x9f37, kF39 = 0x9f39,
                    kF42 = 0x9f42, kA010 = 0xa010;

struct Flags {
    uint8_t active = 0, active2 = 0, f39 = 0, f42 = 0, a010 = 0;
};

inline bool line_shown(const Flags &f) {
    return (f.active || f.active2) && f.f42 && (!f.f39 || f.a010);
}

// The code the predicate is read from, byte for byte; a different build
// refuses (the overlay stays off and X4's own line is left alone).
struct Site {
    uintptr_t va;
    uint8_t n;
    uint8_t bytes[12];
};
inline constexpr Site kSites[] = {
    // IsMouseSteeringActive: `mov 0x33651b1(%rip),%rax` -> 0x3daaaf8, then
    // `movzbl 0x9f36(%rax),%eax`.
    {0xa45940, 7, {0x48, 0x8b, 0x05, 0xb1, 0x51, 0x36, 0x03}},
    {0xa45947, 7, {0x0f, 0xb6, 0x80, 0x36, 0x9f, 0x00, 0x00}},
    // ShouldShowMouseSteeringHelperLine: the option lookup...
    {0xa45cb4, 7, {0x48, 0x8b, 0x05, 0xc5, 0x4e, 0x36, 0x03}},
    {0xa45cc0, 7, {0x48, 0x8d, 0x35, 0x6e, 0xb3, 0x23, 0x02}}, // "mousesteeringline"
    {0xa45cd3, 4, {0x84, 0xc0, 0x75, 0x09}},
    // ...then the same global, and the four flags with their branches.
    {0xa45ce0, 7, {0x48, 0x8b, 0x15, 0x11, 0x4e, 0x36, 0x03}},
    {0xa45ce7, 7, {0x80, 0xba, 0x36, 0x9f, 0x00, 0x00, 0x00}},
    {0xa45cee, 2, {0x74, 0x20}},
    {0xa45cf0, 7, {0x0f, 0xb6, 0x82, 0x42, 0x9f, 0x00, 0x00}},
    {0xa45cf7, 4, {0x84, 0xc0, 0x74, 0xdc}},
    {0xa45cfb, 7, {0x0f, 0xb6, 0x82, 0x39, 0x9f, 0x00, 0x00}},
    {0xa45d02, 3, {0x83, 0xf0, 0x01}},
    {0xa45d05, 6, {0x0a, 0x82, 0x10, 0xa0, 0x00, 0x00}},
    {0xa45d10, 7, {0x0f, 0xb6, 0x82, 0x37, 0x9f, 0x00, 0x00}},
    {0xa45d17, 4, {0x84, 0xc0, 0x74, 0xbc}},
    {0xa45d1b, 2, {0xeb, 0xd3}},
};
inline constexpr size_t kNumSites = sizeof kSites / sizeof kSites[0];

template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (size_t i = 0; i < kNumSites; i++) {
        const Site &s = kSites[i];
        uint8_t b[sizeof s.bytes];
        if (!read(s.va, b, s.n) || memcmp(b, s.bytes, s.n) != 0) {
            if (bad)
                *bad = s.va;
            return false;
        }
    }
    return true;
}

// The global the predicate reads, decoded from the verified displacement.
inline uintptr_t input_global_from_sites() {
    int32_t d;
    memcpy(&d, kSites[0].bytes + 3, 4);
    return kSites[0].va + 7 + (intptr_t)d;
}

// ---- where the nose is in the eye ------------------------------------------
//
// X4's camera at draw time, in head-look's convention (degrees; yaw positive
// to the LEFT, pitch positive UP -- GetCameraRotation's yaw negated, as
// camread and x4vr_camwrite.hpp's rows_for use it). The cockpit's forward axis
// in camera space is row k's z component of rows_for's matrix:
//
//   ( sin yaw,  -cos yaw sin pitch,  cos yaw cos pitch )      x right, y up
//
// -- the nose is to the RIGHT when you look left and BELOW when you look up,
// which is where take 328B's recording shows X4's crosshair. Projected at the
// eye's own field (X4's symmetric render, tan of its half-angle per axis) into
// Vulkan NDC, y DOWN. False when the nose is behind the eye.
inline bool nose_ndc(float yaw_deg, float pitch_deg, float tan_half_x,
                     float tan_half_y, float *x, float *y) {
    constexpr float d2r = 0.01745329251994330f;
    const float yw = yaw_deg * d2r, pt = pitch_deg * d2r;
    const float cx = std::sin(yw);
    const float cy = -std::cos(yw) * std::sin(pt);
    const float cz = std::cos(yw) * std::cos(pt);
    if (!(cz > 1e-4f) || !(tan_half_x > 0.f) || !(tan_half_y > 0.f))
        return false;
    *x = (cx / cz) / tan_half_x;
    *y = -(cy / cz) / tan_half_y;
    return true;
}

// ---- the segment and the ring ----------------------------------------------
//
// `hx, hy`: the pointer's hotspot in eye NDC (where X4 picks). Eye size in
// pixels so the gap and width are isotropic on a non-square eye too.
struct Geometry {
    Mode used = kOff;      // the mode actually drawn (A falls back to B below)
    float ring_dx = 0.f;   // added to the ring's hotspot, NDC
    float ring_dy = 0.f;
    bool line = false;     // false when the pointer is inside the gap
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; // the visible segment, NDC
};

// `gap_px`: the segment starts this far from its origin -- X4 leaves its own
// 30 crosshair units (~16 eye px at the canvas scale) so the line never draws
// across the crosshair, and hides it entirely inside that radius.
//
// The part of the eye the headset actually shows, in eye NDC (y down). The
// rendered eye is X4's symmetric field (110 deg); the runtime shows less --
// take 331's Quest reported l=-54 r=40 u=44 d=-55 for the left eye, mirrored
// for the right -- so a ring inside the render can still be out of sight.
// Default: the whole eye.
struct Box {
    float x0 = -1.f, x1 = 1.f, y0 = -1.f, y1 = 1.f;
    bool contains(float x, float y) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
};

// What BOTH eyes show, from the runtime's per-eye angles (radians: left and
// down negative), mapped into an eye rendered at `tan_half`. The ring is
// drawn alike in both eyes, so it is visible only where both are.
inline Box binocular_box(const float fov_rad[2][4], float tan_half) {
    Box b;
    if (!(tan_half > 0.f))
        return b;
    auto clampu = [](float v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
    const float l = std::fmax(std::tan(fov_rad[0][0]), std::tan(fov_rad[1][0]));
    const float r = std::fmin(std::tan(fov_rad[0][1]), std::tan(fov_rad[1][1]));
    const float u = std::fmin(std::tan(fov_rad[0][2]), std::tan(fov_rad[1][2]));
    const float d = std::fmax(std::tan(fov_rad[0][3]), std::tan(fov_rad[1][3]));
    b.x0 = clampu(l / tan_half);
    b.x1 = clampu(r / tan_half);
    b.y0 = clampu(-u / tan_half); // up is negative NDC y
    b.y1 = clampu(-d / tan_half);
    return b;
}

// A falls back to B for any frame in which its ring would be out of sight:
// the nose behind the eye, or nose + offset outside what both eyes show
// (take 331: looking right in A, the ring left the headset's view while the
// nose was still inside the 110 deg render, and the cursor was lost).
inline Geometry geometry(Mode mode, float hx, float hy, bool nose_ok,
                         float nose_x, float nose_y, float eye_w, float eye_h,
                         float gap_px, const Box &visible = Box{}) {
    Geometry g;
    if (mode == kOff)
        return g;
    const bool ring_visible =
        nose_ok && visible.contains(nose_x + hx, nose_y + hy);
    g.used = (mode == kNose && ring_visible) ? kNose : kGaze;
    const float ox = g.used == kNose ? nose_x : 0.f;
    const float oy = g.used == kNose ? nose_y : 0.f;
    g.ring_dx = ox;
    g.ring_dy = oy;
    const float ex = ox + hx, ey = oy + hy;
    // In pixels for length and direction.
    const float dxp = (ex - ox) * 0.5f * eye_w, dyp = (ey - oy) * 0.5f * eye_h;
    const float len = std::sqrt(dxp * dxp + dyp * dyp);
    if (!(len > gap_px))
        return g;
    const float ux = dxp / len, uy = dyp / len;
    g.line = true;
    g.x0 = ox + ux * gap_px * 2.f / eye_w;
    g.y0 = oy + uy * gap_px * 2.f / eye_h;
    g.x1 = ex;
    g.y1 = ey;
    return g;
}

// A rotated quad for the segment, in the overlay's push-constant form:
// corner(t) = origin + t.x * e1 + t.y * e2, t in [0,1]^2. `width_px` is the
// solid core, `feather_px` the soft edge on each side; `*feather_t` is the
// feather as a fraction of the quad's width, for the fragment shader.
struct Quad {
    float ox, oy, e1x, e1y, e2x, e2y;
};
inline Quad line_quad(const Geometry &g, float eye_w, float eye_h,
                      float width_px, float feather_px, float *feather_t) {
    const float dxp = (g.x1 - g.x0) * 0.5f * eye_w;
    const float dyp = (g.y1 - g.y0) * 0.5f * eye_h;
    const float len = std::sqrt(dxp * dxp + dyp * dyp);
    // unit normal in pixels, converted back to NDC per axis
    const float nx = len > 0.f ? -dyp / len : 0.f;
    const float ny = len > 0.f ? dxp / len : 1.f;
    const float total = width_px + 2.f * feather_px;
    const float nxn = nx * 2.f / eye_w, nyn = ny * 2.f / eye_h;
    Quad q;
    q.ox = g.x0 - nxn * total * 0.5f;
    q.oy = g.y0 - nyn * total * 0.5f;
    q.e1x = g.x1 - g.x0;
    q.e1y = g.y1 - g.y0;
    q.e2x = nxn * total;
    q.e2y = nyn * total;
    if (feather_t)
        *feather_t = total > 0.f ? feather_px / total : 0.f;
    return q;
}

// What the layer knows about this frame, handed to the cursor overlay: the
// mode in force (kOff = draw nothing extra) and where the nose is. Built only
// while X4 would show its own line (`line_shown` plus the cockpit with the HUD
// up), so a Frame the overlay receives always means "draw the target".
struct Frame {
    Mode mode = kOff;
    bool nose_ok = false;
    float nose_x = 0.f, nose_y = 0.f;
    Box visible; // what the headset shows; the whole eye until it is known
};

// X4's own colour for its line: `crosshair_mousesteering_line` ->
// `white_alpha_12` in libraries/colors.xml (alpha in percent). Take 328B's
// recording shows it at an effective ~0.10 over dark space, consistent.
constexpr float kLineRGBA[4] = {1.f, 1.f, 1.f, 0.12f};
constexpr float kLineWidthPx = 2.f, kLineFeatherPx = 1.f, kLineGapPx = 16.f;

} // namespace steer
} // namespace x4vr
