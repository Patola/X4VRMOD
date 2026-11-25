// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The field of view a frame is declared with. X4 draws a symmetric square field (X4VR_FOV) and
// the mod declares exactly that, with the whole image. OpenXR lets a runtime say it won't have
// it (XrViewConfigurationProperties::fovMutable false): then the field declared must be the one
// it reported. Where X4's covers that field, the same picture serves: the part of the image it
// falls on, by whole pixels, declared with those pixels' own angles.
//
// And how the runtime's two eyes stand to each other, which the mod takes for parallel.
#pragma once

#include <cmath>
#include <cstdint>

namespace x4vr {

struct FieldRect {
    bool ok = false; // the reported field lies inside X4's
    int x = 0, y = 0, w = 0, h = 0;
    float left = 0.f, right = 0.f, up = 0.f, down = 0.f; // radians, the pixels' own
};

// `half` is X4's half-field in radians on both axes; `view` the runtime's left, right, up and
// down angles; the image's first row is its top.
inline FieldRect field_rect(float half, int w, int h, const float view[4]) {
    FieldRect r;
    const double t = std::tan((double)half);
    const double l = std::tan((double)view[0]), rr = std::tan((double)view[1]);
    const double u = std::tan((double)view[2]), d = std::tan((double)view[3]);
    if (!(t > 0.0) || w <= 0 || h <= 0 || !(l < rr) || !(d < u) || l < -t || rr > t || u > t ||
        d < -t)
        return r;
    // Outwards to whole pixels: never less than the runtime asked for.
    const double px = (double)w / (2.0 * t), py = (double)h / (2.0 * t);
    int x0 = (int)std::floor((l + t) * px + 1e-6), x1 = (int)std::ceil((rr + t) * px - 1e-6);
    int y0 = (int)std::floor((t - u) * py + 1e-6), y1 = (int)std::ceil((t - d) * py - 1e-6);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;
    if (x1 <= x0 || y1 <= y0)
        return r;
    r.x = x0;
    r.y = y0;
    r.w = x1 - x0;
    r.h = y1 - y0;
    r.left = (float)std::atan(-t + x0 / px);
    r.right = (float)std::atan(-t + x1 / px);
    r.up = (float)std::atan(t - y0 / py);
    r.down = (float)std::atan(t - y1 / py);
    r.ok = true;
    return r;
}

// Eye 1's orientation from eye 0's, in degrees: in all, and by axis (small angles). Positions
// in metres from the middle of the two, in eye 0's axes: x right, y up, z back.
struct EyePair {
    float deg = 0.f, yaw = 0.f, pitch = 0.f, roll = 0.f;
    float at[2][3] = {};
};
inline EyePair eye_pair(const float q0[4], const float q1[4], const float p0[3],
                        const float p1[3]) {
    EyePair r;
    // conj(q0) * q1, quaternions as x, y, z, w
    const float a[4] = {-q0[0], -q0[1], -q0[2], q0[3]};
    float d[4] = {a[3] * q1[0] + a[0] * q1[3] + a[1] * q1[2] - a[2] * q1[1],
                  a[3] * q1[1] - a[0] * q1[2] + a[1] * q1[3] + a[2] * q1[0],
                  a[3] * q1[2] + a[0] * q1[1] - a[1] * q1[0] + a[2] * q1[3],
                  a[3] * q1[3] - a[0] * q1[0] - a[1] * q1[1] - a[2] * q1[2]};
    if (d[3] < 0.f)
        for (float &v : d)
            v = -v;
    const float k = 57.2957795130823f;
    const float s = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    r.deg = 2.f * std::atan2(s, d[3]) * k;
    const float f = s > 1e-7f ? r.deg / s : 0.f;
    r.pitch = d[0] * f;
    r.yaw = d[1] * f;
    r.roll = d[2] * f;
    for (int e = 0; e < 2; e++) {
        const float *p = e ? p1 : p0;
        const float v[3] = {p[0] - 0.5f * (p0[0] + p1[0]), p[1] - 0.5f * (p0[1] + p1[1]),
                            p[2] - 0.5f * (p0[2] + p1[2])};
        // v turned by conj(q0): v + 2 w (a x v) + 2 a x (a x v)
        const float c[3] = {a[1] * v[2] - a[2] * v[1], a[2] * v[0] - a[0] * v[2],
                            a[0] * v[1] - a[1] * v[0]};
        const float cc[3] = {a[1] * c[2] - a[2] * c[1], a[2] * c[0] - a[0] * c[2],
                             a[0] * c[1] - a[1] * c[0]};
        for (int i = 0; i < 3; i++)
            r.at[e][i] = v[i] + 2.f * a[3] * c[i] + 2.f * cc[i];
    }
    return r;
}

} // namespace x4vr
