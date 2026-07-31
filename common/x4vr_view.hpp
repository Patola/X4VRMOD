// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_view.hpp — layout of and math for X4's per-view constants block.
//
// From docs/frame-analysis.md: each view's constants occupy a 1792-byte
// block; the first 704 bytes are 11 mat4s:
//
//   float off | byte off | name
//   ----------+----------+---------------------------
//        0    |     0    | M_view                (world -> view)
//       16    |    64    | M_projection          (view  -> clip)
//       32    |   128    | M_invprojection
//       48    |   192    | M_projection_uj       (unjittered)
//       64    |   256    | M_invprojection_uj
//       80    |   320    | M_jitter
//       96    |   384    | M_prevjitter
//      112    |   448    | M_viewprojection      (world -> clip)
//      128    |   512    | M_viewinverse         (view  -> world)
//      144    |   576    | M_shadowCSM0Clip      (light space, shared)
//      160    |   640    | M_shadowCSM1Clip      (light space, shared)
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x4vr {

enum ViewMat : uint32_t {
    kView = 0,
    kProjection = 16,
    kInvProjection = 32,
    kProjectionUJ = 48,
    kInvProjectionUJ = 64,
    kJitter = 80,
    kPrevJitter = 96,
    kViewProjection = 112,
    kViewInverse = 128,
    kShadowCSM0 = 144,
    kShadowCSM1 = 160,
};

constexpr uint32_t kViewBlockBytes = 1792;

// A 4x4 of floats in the engine's storage order (we do not assume row- or
// column-major; see detect_major()).
struct Mat4 {
    float m[16];
};

inline Mat4 load(const float *p) {
    Mat4 r;
    memcpy(r.m, p, sizeof(r.m));
    return r;
}
inline void store(float *p, const Mat4 &a) { memcpy(p, a.m, sizeof(a.m)); }

// Storage-order detection from a view matrix's shape. An affine view matrix
// has a translation part and a (0,0,0,1) row/column:
//   column-major (GLSL): translation at m[12..14], m[3]=m[7]=m[11]=0, m[15]=1
//   row-major    (HLSL): translation at m[3],m[7],m[11],       m[12..14]=0
enum class Major { Unknown, Column, Row };

// Detect storage order from the projection matrix. A perspective projection
// has a ±1 that moves view-space z into clip w, and a 0 in the last
// diagonal slot:
//   column-major: m[11] = ±1, m[15] = 0   (m[14] holds the near term)
//   row-major:    m[14] = ±1, m[15] = 0   (m[11] holds the near term)
// This is unambiguous even when the view matrix is identity (X4 renders
// camera-relative, so its M_view is identity and tells us nothing).
inline Major detect_major_proj(const Mat4 &proj) {
    auto isone = [](float f) { return std::fabs(std::fabs(f) - 1.0f) < 1e-4f; };
    if (std::fabs(proj.m[15]) > 1e-4f)
        return Major::Unknown; // not a perspective matrix
    const bool col = isone(proj.m[11]) && !isone(proj.m[14]);
    const bool row = isone(proj.m[14]) && !isone(proj.m[11]);
    if (col)
        return Major::Column;
    if (row)
        return Major::Row;
    return Major::Unknown;
}

inline Major detect_major(const Mat4 &view) {
    auto zero = [](float f) { return std::fabs(f) < 1e-6f; };
    const bool col = zero(view.m[3]) && zero(view.m[7]) && zero(view.m[11]) &&
                     std::fabs(view.m[15] - 1.0f) < 1e-4f;
    const bool row = zero(view.m[12]) && zero(view.m[13]) &&
                     zero(view.m[14]) && std::fabs(view.m[15] - 1.0f) < 1e-4f;
    if (col && !row)
        return Major::Column;
    if (row && !col)
        return Major::Row;
    return Major::Unknown; // e.g. camera at the origin: both patterns match
}

// Matrix product honouring storage order: returns A*B in the mathematical
// sense (apply B first, then A) for the given storage convention.
inline Mat4 mul(const Mat4 &a, const Mat4 &b, Major major) {
    Mat4 r{};
    // Interpret element (row, col):
    //   column-major storage: index = col*4 + row
    //   row-major    storage: index = row*4 + col
    auto at = [major](const Mat4 &x, int row, int col) {
        return major == Major::Row ? x.m[row * 4 + col] : x.m[col * 4 + row];
    };
    auto set = [major](Mat4 &x, int row, int col, float v) {
        (major == Major::Row ? x.m[row * 4 + col] : x.m[col * 4 + row]) = v;
    };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += at(a, i, k) * at(b, k, j);
            set(r, i, j, s);
        }
    return r;
}

// General 4x4 inverse (cofactor method); storage-order agnostic because the
// inverse of the transpose is the transpose of the inverse.
inline bool invert(const Mat4 &in, Mat4 &out) {
    const float *m = in.m;
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-20f)
        return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++)
        out.m[i] = inv[i] * det;
    return true;
}

// Offset the camera along its own axes (view space): +x right, +y up,
// +z depends on handedness. This is exactly the stereo eye offset, so the
// Phase-3 test primitive is the Phase-4 building block.
//
// A view matrix maps world -> view, so shifting the *camera* by d means
// translating in view space by -d:  V' = T(-d) * V.
inline Mat4 offset_camera(const Mat4 &view, Major major, float dx, float dy,
                          float dz) {
    Mat4 t{};
    // identity
    for (int i = 0; i < 4; i++)
        t.m[i * 4 + i] = 1.0f;
    // translation slot differs by storage order
    if (major == Major::Row) {
        t.m[3] = -dx;
        t.m[7] = -dy;
        t.m[11] = -dz;
    } else {
        t.m[12] = -dx;
        t.m[13] = -dy;
        t.m[14] = -dz;
    }
    return mul(t, view, major);
}

// Per-eye clip-space matrix K, derived analytically from X4's projection.
//
// X4's projection is (column-major storage; values as measured):
//     m[0]=sx  m[5]=sy(neg, Y-flip)  m[11]=1  m[14]=near  m[15]=0
// which for a view-space point (x, y, z, 1) gives
//     x_c = sx*x     y_c = sy*y     z_c = near     w_c = z
// (reversed-Z with an infinite far plane: depth = near/z).
//
// Offsetting the camera by dx along view-space X means translating the
// world by -dx, so
//     x_c' = sx*(x - dx) = x_c - sx*dx
// and because z_c is the constant `near` for every vertex, that constant
// term can be expressed against z_c:
//     x_c' = x_c + (-sx*dx/near) * z_c
//
// So K is the identity with a single shear term. In NDC this becomes
//     x_ndc' = x_ndc - (sx*dx)/z
// i.e. the shift falls off with view depth — true stereo parallax: near
// geometry separates strongly, distant stars not at all. (A plain clip-space
// translation, by contrast, would slide the whole image uniformly and
// produce no depth cue at all.)
// The three numbers make_eye_shear needs, read out of X4's own projection
// rather than assumed. The layer has defaulted to sx=0.889 / near=0.1 since
// Phase 4a; those were *measured*, but at 2816x1408, and sx carries the
// aspect (sx = sy/aspect -- 1.778/0.889 = 2.000 there). The eye now renders
// 1408x1408, so a value that tracks the aspect cannot still be right.
//
// Which slot holds `near` depends on storage order: column-major keeps
// w_c = z in m[11] and the near term in m[14], row-major the transpose. That
// is the same asymmetry detect_major_proj keys on, so pass its verdict in
// rather than re-deriving it here.
//
// Read this from the *un-jittered* projection (kProjectionUJ) when you have
// the choice: X4 does TAA, and the jitter lands in m[8]/m[9] -- the very
// slots the eye shear writes. sx and near are jitter-independent, so either
// matrix answers this question, but reading the clean one keeps the two
// concerns from being confused later.
struct ProjTerms {
    float sx = 0.0f;
    float sy = 0.0f;
    float near_z = 0.0f;
    bool ok = false;
};

inline ProjTerms read_proj_terms(const Mat4 &p, Major major) {
    ProjTerms t{};
    if (major == Major::Unknown)
        return t; // refuse to guess -- a transposed read is silently plausible
    t.sx = p.m[0];
    t.sy = p.m[5];
    t.near_z = (major == Major::Column) ? p.m[14] : p.m[11];
    t.ok = std::fabs(t.sx) > 1e-6f && std::fabs(t.near_z) > 1e-6f;
    return t;
}

inline Mat4 make_eye_shear(float sx, float sy, float near_z, float dx,
                           float dy = 0.0f) {
    Mat4 k{};
    for (int i = 0; i < 4; i++)
        k.m[i * 4 + i] = 1.0f;
    if (near_z != 0.0f) {
        k.m[8] = -sx * dx / near_z;  // K[0][2]
        k.m[9] = -sy * dy / near_z;  // K[1][2]
    }
    return k;
}

inline void format_mat(char *buf, size_t n, const Mat4 &a) {
    snprintf(buf, n,
             "[%8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f | "
             "%8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f]",
             a.m[0], a.m[1], a.m[2], a.m[3], a.m[4], a.m[5], a.m[6], a.m[7],
             a.m[8], a.m[9], a.m[10], a.m[11], a.m[12], a.m[13], a.m[14],
             a.m[15]);
}

} // namespace x4vr
