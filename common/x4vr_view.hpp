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

// Task #30: the constant clip-space x offset that puts a canvas at `z` metres.
//
// Applied as K[12], it makes `gl_Position = (x + s·w, y, z, w)`, whose NDC x is
// `x/w + s` for *any* w -- which is what separates a canvas from geometry. The
// UI's own matrix may be an orthographic screen transform or the map's
// perspective one; the shift is the same either way.
//
// The value is the world shear evaluated at a fixed depth, and that is not a
// coincidence but the definition: "the UI at z metres" and "world geometry at z
// metres" have to produce the same disparity or the canvas would not sit where
// it is asked to. make_eye_shear puts `m8 = sx·(ipd/2)/near` for the left eye,
// and the derivation holds clip z at the near plane, so the NDC displacement of
// a point at view depth z is `m8·near/z` -- near cancels, leaving this. Locked
// against make_eye_shear in tests/view_math.cpp so the two cannot drift.
//
// Positive for the left eye, negative for the right: a near object appears
// displaced toward the right in the left eye. Zero for a non-positive distance,
// which is the same as infinity -- identical in both eyes, today's behaviour.
inline float canvas_shift(float sx, float ipd, float z) {
    if (!(z > 0.0f))
        return 0.0f;
    return sx * 0.5f * ipd / z;
}

// ---------------------------------------------------------------- task #35
//
// X4 renders one symmetric frustum per eye. A headset does not have one: WiVRn
// on a Quest 3 reports L-54 R+40 U+44 D-55 for the left eye and the mirror for
// the right, so "straight ahead" is 15.04 deg off centre in each image, in
// opposite directions. Submitting X4's frame as if it were centred asks the
// eyes to diverge by 30.07 deg, which nobody can fuse; the probe reproduces
// exactly that as a negative control.
//
// The map from one to the other is an affine on values gl_Position already
// carries. Writing it out: X4 gives x_c = sx*x and w_c = z, so the true ray is
// x/z = x_c/(sx*w_c). The target's NDC x for that ray is
//
//     x_ndc = (2*(x/z) - (tan_r + tan_l)) / (tan_r - tan_l)
//
// -- linear in TANGENT, which is the whole reason a rectilinear image can be
// remapped by an affine at all. Multiplying by w_c to get back to clip space:
//
//     x_c' = [2/((tan_r - tan_l)*sx)] * x_c  -  [(tan_r + tan_l)/(tan_r - tan_l)] * w_c
//
// The vertical is the same derivation with one extra sign: OpenXR's angleUp is
// positive and Vulkan's NDC y points DOWN, so the numerator negates -- and X4's
// sy is already negative for exactly that reason, which is why ay_num is
// negative and the product comes out positive. Getting this pair wrong flips
// the image vertically in the headset, so it is asserted rather than argued.
//
//     y_c' = [-2/((tan_u - tan_d)*sy)] * y_c  +  [(tan_u + tan_d)/(tan_u - tan_d)] * w_c
//
// Both collapse to the identity when the target is symmetric and sx = cot(t),
// which is the property tests/view_math.cpp keys on: a map that is not the
// identity in the case where it must be is wrong everywhere else too.
//
// `sx` and `sy` are read LIVE, per draw, for the reason task #23 established:
// X4's cameras run sx from 0.754 to 3.781 within one session and not all of
// them are square (the map camera reports |sy/sx| = 1.5). A baked A is the
// bug #23 already fixed once.
struct EyeFrustum {
    float tan_l = 0.0f, tan_r = 0.0f, tan_u = 0.0f, tan_d = 0.0f;
};

// OpenXR hands back angles; everything downstream wants tangents. Converting
// once, here, is what stops a `tan` from being forgotten at a call site --
// the mistake that made the first test card unfusible.
inline EyeFrustum frustum_of_angles(float l, float r, float u, float d) {
    EyeFrustum f;
    f.tan_l = std::tan(l);
    f.tan_r = std::tan(r);
    f.tan_u = std::tan(u);
    f.tan_d = std::tan(d);
    return f;
}

// The coefficients, with sx/sy factored out so the shader can divide by the
// live values. `ex` folds in the eye offset the shear already applies, and is
// only used by the folded form -- see apply_off_axis_folded.
struct OffAxis {
    float ax_num = 0.0f; // A_x = ax_num / sx
    float bx = 0.0f;
    float ay_num = 0.0f; // A_y = ay_num / sy
    float by = 0.0f;
    float ex = 0.0f; // = ax_num * d_eye
    bool ok = false;
};

inline OffAxis make_off_axis(const EyeFrustum &f, float d_eye = 0.0f) {
    OffAxis o{};
    const float hspan = f.tan_r - f.tan_l;
    const float vspan = f.tan_u - f.tan_d;
    if (!(hspan > 1e-6f) || !(vspan > 1e-6f))
        return o; // a degenerate frustum refuses rather than dividing
    o.ax_num = 2.0f / hspan;
    o.bx = -(f.tan_r + f.tan_l) / hspan;
    o.ay_num = -2.0f / vspan;
    o.by = (f.tan_u + f.tan_d) / vspan;
    o.ex = o.ax_num * d_eye;
    o.ok = true;
    return o;
}

// Applied AFTER the eye shear, which is how the SPIR-V patch composes it: the
// shear leaves x_c = sx*(x - d), so dividing by sx recovers the eye's own ray
// and no eye-offset term is needed here.
inline void apply_off_axis(const OffAxis &o, float sx, float sy,
                           float clip[4]) {
    if (!o.ok || std::fabs(sx) < 1e-9f || std::fabs(sy) < 1e-9f)
        return;
    clip[0] = (o.ax_num / sx) * clip[0] + o.bx * clip[3];
    clip[1] = (o.ay_num / sy) * clip[1] + o.by * clip[3];
}

// The same map applied to an UNSHEARED clip position, with the eye offset
// folded into the constant term. Saves one multiply and needs no separate
// shear step; kept as the proven-equivalent alternative rather than the
// default, because the shear it would replace is what 60 passing takes ran.
inline void apply_off_axis_folded(const OffAxis &o, float sx, float sy,
                                  float clip[4]) {
    if (!o.ok || std::fabs(sx) < 1e-9f || std::fabs(sy) < 1e-9f)
        return;
    clip[0] = (o.ax_num / sx) * clip[0] + o.bx * clip[3] - o.ex;
    clip[1] = (o.ay_num / sy) * clip[1] + o.by * clip[3];
}

// ---------------------------------------------------------------- task #39
//
// What a DEFERRED pass has to do once the affine exists.
//
// The affine moves `gl_Position`, so the depth buffer and every G-buffer are
// rasterized in the target frustum's NDC. A deferred pass then reads
// `gl_FragCoord`, turns it back into NDC, and reconstructs view position with
// `M_invprojection` -- which is the inverse of X4's SYMMETRIC projection and
// knows nothing about the affine. It therefore recovers a position that is
// wrong by the whole map, in opposite directions in the two eyes, because `bx`
// reverses sign between them. Take 165b's per-eye menu shadows are this.
//
// The repair is one more constant composition. With
//
//     clip_final = A · K · clip_centre       (K = the eye shear, task #22)
//
// recovering the centre frame from a final clip coordinate needs
//
//     T(d) · M_invprojection · A⁻¹
//
// -- A⁻¹ on the RIGHT because it is undone first, T(d) on the left because it
// is undone last. Both compositions are safe under the perspective divide: A
// leaves w alone, and T is a view-space translation, so a shader that hands in
// `(ndc.xy, depth, 1)` and one that hands in a homogeneous clip position get
// the same answer.
//
// A's linear part is `A_x = ax_num/sx`, and `sx` is per-draw. The scales are
// therefore read back OUT of the matrix being corrected: X4's projection has a
// diagonal top-left 2x2 (dumped in every take -- `M_projection [1.333 -0.000
// ... | 0.000 -1.333 ...]`), so its inverse does too, and
//
//     minv[0][0] = 1/sx        minv[1][1] = 1/sy
//
// The same load that is being corrected supplies them, which is both cheaper
// than a second access chain and impossible to get out of step with. It also
// costs nothing per fragment in practice: every input is uniform over the
// draw, so the whole chain hoists into scalar registers.
//
// Indexing is [col][row] -- `m[c*4 + r]` -- which is SPIR-V's own convention
// for OpCompositeExtract on a matrix, and is deliberately the same arithmetic
// the patch emits. These two must not be allowed to drift.

// M -> T(d)·M : row 0 += d · row 3.
//
// What patch_fragment_invproj_eye has emitted since task #22, written out here
// only so the composition above can be proven offline against the vertex path.
inline void compose_eye_untranslate(float d, float m[16]) {
    for (int c = 0; c < 4; c++)
        m[c * 4 + 0] += d * m[c * 4 + 3];
}

// M -> M·A⁻¹, undoing the off-axis affine on the clip coordinate a deferred
// pass feeds in.
//
//     A⁻¹ = [ 1/A_x   0     0   -B_x/A_x ]
//           [   0   1/A_y   0   -B_y/A_y ]
//           [   0     0     1       0    ]
//           [   0     0     0       1    ]
//
// so col0 and col1 scale, and col3 picks up a multiple of each. Refuses as a
// unit, exactly like apply_off_axis: a camera block that reads back all zeroes
// before the first real frame must leave the matrix alone rather than put an
// inf in it.
inline bool compose_inv_off_axis(const OffAxis &o, float m[16]) {
    if (!o.ok)
        return false;
    const float ax = o.ax_num * m[0]; // ax_num · (1/sx) = A_x
    const float ay = o.ay_num * m[5]; // ay_num · (1/sy) = A_y
    if (!(ax * ax > 0.0f) || !(ay * ay > 0.0f))
        return false;
    const float iax = 1.0f / ax, iay = 1.0f / ay;
    for (int r = 0; r < 4; r++) {
        const float c0 = m[0 * 4 + r] * iax;
        const float c1 = m[1 * 4 + r] * iay;
        m[3 * 4 + r] -= o.bx * c0 + o.by * c1;
        m[0 * 4 + r] = c0;
        m[1 * 4 + r] = c1;
    }
    return true;
}

// ---------------------------------------------------------------- task #40
//
// Screen-locked content, put in the DECLARED frame.
//
// The affine reaches world geometry, and everything screen-locked is left in
// X4's symmetric frame — where, once the declaration is canted, image centre
// IS the frustum centre, −15.04° in one eye and +15.04° in the other. The eyes
// are asked to diverge by their separation and nobody fuses it. That is take
// 165b's HUD.
//
// The repair is the same map, evaluated for a canvas rather than for geometry.
// Take a draw at screen NDC `x`. The screen spans X4's own field, so that is
// the ray `tanθ = x·tan(half)`. Put it on a canvas `z` metres away and look at
// it from an eye offset `d`: `tanθ_e = tanθ − d/z`. Where that belongs in the
// target frustum is the definition A already encodes, so
//
//     x' = A_x·x + B_x − ax_num·d/z          A_x = ax_num/sx_screen
//     y' = A_y·y + B_y                       A_y = ay_num/sy_screen
//
// Every term is a constant — no live sx, because `sx_screen` is a property of
// the SCREEN, not of a draw's camera. So this needs no new shader machinery at
// all: it is a 4x4, and patch_vertex_clip has been applying constant 4x4s to
// UI draws since task #30.
//
// **It is the same statement #30 already made**, which is the check that
// matters: with a symmetric target at X4's own half-angle, `ax_num = sx_screen`
// and `bx = 0`, so the whole thing collapses to `x' = x − sx·d/z`, and that is
// `canvas_shift` exactly. #30 said "the UI at z metres and world geometry at z
// metres must produce the same disparity"; this says the same thing in a frame
// that is no longer X4's.
//
// `z <= 0` means infinity — the pre-#30 behaviour, "pinned at infinity", and
// the right default when the affine is on and no distance was chosen: zero
// disparity is fusable, whereas leaving the map off is not.
//
// **sx_screen comes from X4VR_FOV, never from a camera block.** vr_declared_fov()
// gives the reason at length: the layer sees several cameras per frame with sx
// from 0.75 to 3.78, and picking one of those is the mistake fifty takes were
// built on. The knob is what we set, so the knob is what we measure against.
// The same map as a plain NDC function, for consumers that are not shaders.
//
// The cursor overlay is the one that matters: X4 hit-tests the UI CPU-side at
// an unshifted window coordinate, so if the menu moves and the pointer does
// not, every button still WORKS while the pointer sits away from it — both
// halves behaving exactly as designed and the result visibly wrong. #30 got
// away with publishing a single float because its map was a translation; this
// one is not, so the whole thing is published.
struct CanvasNdc {
    float ax = 1.0f, bx = 0.0f, ay = 1.0f, by = 0.0f;
    float x(float v) const { return ax * v + bx; }
    float y(float v) const { return ay * v + by; }
    bool identity() const {
        return ax == 1.0f && bx == 0.0f && ay == 1.0f && by == 0.0f;
    }
};

inline CanvasNdc canvas_ndc_of(const float k[16]) {
    CanvasNdc c;
    c.ax = k[0];
    c.bx = k[12];
    c.ay = k[5];
    c.by = k[13];
    return c;
}

inline void make_canvas_k(const OffAxis &o, float sx_screen, float sy_screen,
                          float d_eye, float z, float k[16]) {
    for (int i = 0; i < 16; i++)
        k[i] = 0.0f;
    k[0] = k[5] = k[10] = k[15] = 1.0f;
    const bool have = o.ok && std::fabs(sx_screen) > 1e-9f &&
                      std::fabs(sy_screen) > 1e-9f;
    // The numerator of the disparity term. With no map it is `sx_screen`,
    // which is what makes the fallback below literally canvas_shift rather
    // than something that agrees with it to a few digits.
    const float ax_num = have ? o.ax_num : sx_screen;
    if (have) {
        k[0] = o.ax_num / sx_screen;
        k[5] = o.ay_num / sy_screen;
        k[12] = o.bx;
        k[13] = o.by;
    }
    if (z > 0.0f)
        k[12] -= ax_num * d_eye / z;
}

// The half-angle a single symmetric render must cover so that nothing visible
// in any target frustum is culled by X4 before the affine can place it.
// Returns radians; the caller turns it into X4's <fov> with x4_fov_for_half().
inline float union_half_angle(const EyeFrustum *f, uint32_t n) {
    float t = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const float c[4] = {std::fabs(f[i].tan_l), std::fabs(f[i].tan_r),
                            std::fabs(f[i].tan_u), std::fabs(f[i].tan_d)};
        for (float v : c)
            if (v > t)
                t = v;
    }
    return std::atan(t);
}

// X4's <fov> config value for a given half-angle. The law is linear in
// DEGREES of the full field -- fov 1.0 is 73.7399 deg -- which is measured,
// not assumed: score_run.py reads it back out of every log as
// "105.964 deg = fov 1.437".
inline float x4_fov_for_half(float half_rad) {
    return (2.0f * half_rad * 57.2957795130823f) / 73.7399f;
}

// The same law read the other way: what half-angle is X4 actually rendering?
//
// Written once, because two places now need it and they must not disagree.
// vr_declared_fov() uses it to tell the compositor what field the eye image
// spans; make_canvas_k needs the same number to know what angle a screen NDC
// corresponds to. If those two ever drifted apart, the UI would be placed
// against one field and displayed against another, and the error would look
// like a canvas at the wrong distance rather than like a bug.
//
// A missing or non-positive knob means X4's default fov of 1.0.
inline float x4_half_for_fov(float fov) {
    return (fov > 0.0f ? fov : 1.0f) * 73.7399f * 0.5f * 0.01745329251994330f;
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
