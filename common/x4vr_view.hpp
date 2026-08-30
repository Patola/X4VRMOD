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
    // Camera-block members 9 and 10 are M_shadowCSM0Clip / M_shadowCSM1Clip
    // -- world-to-shadow WITHOUT any object transform. Named without the
    // "Clip" here since before task #56; the offsets are X4's.
    kShadowCSM0 = 144,
    kShadowCSM1 = 160,
    // Task #56. Every offset below was read out of a dumped module's own
    // `OpMemberDecorate ... Offset`, never counted along the GLSL: the block
    // has array members (V_light_direction_view[3], V_evsm[GFX_MAX_CSM],
    // six more sized by GFX_MAX_VOLUME_TYPES) whose lengths are #defines we
    // do not have, so positional counting drifts by an unknown amount.
    //   spirv-dis mod-0180.spv | grep 'OpMemberDecorate .* Offset'
    kCsmThresholds = 232,     // V_csmthresholds       byte 928
    kCsmBlendStrength = 359,  // F_csm_blendstrength   byte 1436
    kShadowMapSize = 360,     // F_shadowmapsize       byte 1440
    kShadowMaxDistance = 361, // F_shadowmaxdistance   byte 1444
    kTextureFactorCSM0 = 363, // F_texturefactorCSM0.. byte 1452, five in a row
    // Task #70. **X4's own cascade debug visualisation**, and the only member
    // here we WRITE rather than read.
    //
    //   l_pass_globallight.frag.glsl:
    //     if (B_csmdebugcolor) OUT_RT0.rgb *= CSMColorCascades(view_pos.z);
    //
    // `CSMColorCascades` tints by which cascade `view_pos.z` falls in --
    // 0 red, 1 yellow, 2 green, 3 cyan, past the last threshold blue -- and it
    // reads the SAME `view_pos` the real shadow lookup does. So it is a direct
    // readout of the reconstructed depth, written by Egosoft, and no
    // instrument of ours can be wrong about it.
    //
    // A bool in std140 occupies 32 bits; any non-zero word is true.
    kCsmDebugColor = 380,     // B_csmdebugcolor       byte 1520
    kShadowEnable = 381,      // B_shadow              byte 1524
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
// This is unambiguous even when the view matrix is identity — which it is for
// nearly every X4 camera, because X4 renders camera-relative and bakes the
// camera into M_worldviewprojection on the CPU. "Nearly" is the correction of
// task #53: 9 of 64 stored runs log a camera with a real view transform, and
// detect_major() would answer for those. Preferring the projection is still
// right, because it answers for *all* of them and does not depend on which
// camera happened to win the frame.
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

// What `M_view` actually contains, decoded — task #53.
//
// This project believed since Phase 3 that X4's `M_view` is *always* identity,
// on the strength of a survey of all 128 blocks of the view arena at one
// instant. The survey was real and complete along the axis it covered, and the
// quantifier it licensed was "at this moment", not "always". Grepping the same
// line out of 64 stored run logs finds **9 that carry a 15.0 deg rotation and
// an 8.70 m offset**, agreeing to three decimals across five weeks — so there
// is at least one camera per frame for which world space is NOT view space,
// and `world2view()` / `view2world()` are real transforms in the 422 entry
// points that read member 0.
//
// The angle is transpose-invariant (it comes from the trace), so it survives a
// wrong `major`. The **axis and the translation do not**: transposing inverts
// the rotation and moves the translation between the last row and the last
// column, and both still return plausible-looking numbers. So this refuses an
// Unknown order for the same reason read_proj_terms does.
//
// `identity` is the thing callers should branch on, not `angle_deg == 0`: a
// pure translation is identity in rotation and still means the camera is not
// at the origin of the space its geometry is expressed in.
struct ViewTerms {
    bool ok = false;         // decoded: affine, m[15] == 1, order known
    bool identity = false;   // rotation AND translation within tolerance
    float angle_deg = 0.0f;  // rotation magnitude, axis-angle
    float axis[3] = {0.0f, 0.0f, 0.0f}; // unit, or zeros below the threshold
    float offset_m = 0.0f;   // |translation|
    float t[3] = {0.0f, 0.0f, 0.0f};
};

inline ViewTerms read_view_terms(const Mat4 &v, Major major) {
    ViewTerms r{};
    if (major == Major::Unknown)
        return r;
    if (std::fabs(v.m[15] - 1.0f) > 1e-3f)
        return r; // unpopulated block, or not an affine view matrix
    auto at = [&](int row, int col) {
        return major == Major::Row ? v.m[row * 4 + col] : v.m[col * 4 + row];
    };
    for (int i = 0; i < 3; i++)
        r.t[i] = major == Major::Column ? v.m[12 + i] : v.m[i * 4 + 3];
    r.offset_m = std::sqrt(r.t[0] * r.t[0] + r.t[1] * r.t[1] + r.t[2] * r.t[2]);

    const float tr = at(0, 0) + at(1, 1) + at(2, 2);
    // acos is ill-conditioned near +-1 and the trace can land a hair outside
    // [-1, 1] through float error, which would make it NaN -- and a NaN angle
    // would read as "not identity" and fire the loud path on every camera.
    const float c = std::fmax(-1.0f, std::fmin(1.0f, (tr - 1.0f) * 0.5f));
    r.angle_deg = std::acos(c) * 57.2957795130823f;
    const float s = std::sqrt(std::fmax(0.0f, 1.0f - c * c));
    if (s > 1e-4f) {
        const float a[3] = {at(2, 1) - at(1, 2), at(0, 2) - at(2, 0),
                            at(1, 0) - at(0, 1)};
        // Normalise the skew vector by ITS OWN length, not by 2*sin(theta).
        // The textbook divisor is exact only for a perfectly orthonormal R,
        // and the matrices this reads are not: take 178's is a log line
        // rounded to three decimals, and dividing by 2*sin(theta) returned an
        // axis of length 1.005 for it. A "unit" vector printing 1.005 reads as
        // a bug in the decoder rather than as rounding in its input, and an
        // instrument nobody trusts is one nobody runs.
        const float n = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (n > 1e-6f)
            for (int i = 0; i < 3; i++)
                r.axis[i] = a[i] / n;
    }
    r.ok = true;
    // 0.05 deg and 1 mm. Loose enough that float noise in a matrix X4 rebuilds
    // every frame does not flap the verdict, tight enough that the 15 deg /
    // 8.70 m camera cannot hide in it by three orders of magnitude.
    r.identity = r.angle_deg < 0.05f && r.offset_m < 1e-3f;
    return r;
}

// The per-frame block the layer hands its patched shaders -- tasks #54/#59.
//
// std140, and the C++ layout has to match what patch_vertex_pose_clip declares
// in SPIR-V: two column-major mat4 at offsets 0 and 64, matrix stride 16.
// tests/view_math.cpp asserts the sizes rather than trusting them, because a
// mismatch reads back as a plausible wrong matrix rather than as an error.
//
// It carries R ALONE. The clip-space form of a head rotation is P.R.P^-1 and
// six of its sixteen entries scale with sx/sy (tools/headlook_clip.py), which
// vary 33x with zoom and differ per camera -- so the conjugation belongs in the
// shader, next to the live sx it already loads, and the per-frame channel stays
// camera-independent. That is what allows ONE write per frame to serve every
// draw, where writing X4's own arena would need one write per credited slot.
struct PoseBlock {
    float r[16];      // identity until #33 drives it
    float r_inv[16];
    // The PREVIOUS frame's r, for X4's motion-vector path. Take 188's seam was
    // this member's absence: X4's common world vertex shader ends with
    // `IO_mv = M_prevworldviewprojection * IO_mv`, and we rotated gl_Position
    // while leaving that alone, so the G-buffer reported a constant apparent
    // motion of exactly the head rotation and every temporal pass smeared
    // along it. The seam sat on the ray at yaw/2 -- the angular bisector
    // between the two orientations -- to 1.1 px over six measured cases.
    //
    // It is a SEPARATE matrix and not just r because the correct velocity
    // *does* include genuine head motion: when the head is really turning the
    // pixel really moved, and TAA must know. Only when the pose is static do
    // the two coincide, which is what makes the static take a clean test.
    float r_prev[16];

    // **Task #77: where the eye offset points, as (cos roll, sin roll).**
    //
    // The disparity is baked along image-x, and the compositor rotates the
    // submitted image by the head's full orientation -- so at roll phi a
    // sin(phi) share of it becomes VERTICAL, which the eye cannot fuse. Turning
    // the offset by phi in the shader is the fix, and phi is per-frame, so it
    // has to arrive here rather than be baked.
    //
    // Two floats and not an angle: the shader would otherwise need sin and cos
    // of its own, which means importing GLSL.std.450 into modules that may not
    // have it. A vec4 rather than a vec2 because std140 aligns vec4 to 16 and
    // this is the last member -- the padding exists either way, so spending it
    // on named slack is free. z and w are reserved and written as 0.
    //
    // Identity is (1, 0): no roll, offset along x, which is exactly what every
    // run before this did. A block left at zeros would put the offset at the
    // ORIGIN and collapse the stereo, so the layer writes (1,0) at init rather
    // than relying on the caller.
    float roll_cs[4];
};
static_assert(sizeof(PoseBlock) == 208,
              "std140: three mat4 at 0, 64, 128 and a vec4 at 192");
// The offset the SPIR-V patch decorates its one-member view with. Named here so
// the shader-side constant and the struct cannot drift apart silently.
constexpr uint32_t kPoseRollOffset = 192;

// The shear angle that cancels a head roll, in the frame pose_set_roll wants.
//
// It is the head's roll NEGATED, and that negation is the whole of task #77's
// second half. The compositor turns the rendered image by the head's roll to
// hold the world level; that turn is what drags the disparity into the
// vertical. So the disparity has to be pre-turned the OTHER way to arrive
// horizontal on a panel that is head-fixed and therefore never tilts.
//
// CALIBRATED, not derived -- a derivation is what got this wrong. Takes 216
// (forced +40) and 219 (forced +65) each put the comfort null at a head rolled
// LEFT by that angle, and head_angles() reports a left roll as NEGATIVE (take
// 219 logged head -76..-84 deg while the null sat at +65). Two points, opposite
// signs, so the map is a negation and not an offset.
//
// Take 220 fed head_roll straight through and Patola reported BOTH A/B arms
// wrong "in different ways": at head roll H the control arm leaves the
// disparity at H off horizontal, and the uncompensated-sign arm leaves it at
// 2H -- which past 45 deg has a negative horizontal component, so the eyes are
// also swapped. Two distinct wrongnesses, no null anywhere, which is exactly
// what he described and exactly what take 217 had already reported.
inline float roll_compensation_deg(float head_roll_deg) {
    return -head_roll_deg;
}

// Fill the roll slot. Degrees in, (cos, sin) out, and the weight is the SAME
// guard yaw uses -- at the pole yaw and roll are one rotation and the split
// between them is arbitrary, so fading roll there with a second predicate would
// be a second place to get it wrong. Weight 0 leaves (1, 0): no rotation.
inline void pose_set_roll(PoseBlock &p, float roll_deg, float weight = 1.0f) {
    const float w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    const float r = roll_deg * w * 0.01745329251994330f;
    p.roll_cs[0] = std::cos(r);
    p.roll_cs[1] = std::sin(r);
    p.roll_cs[2] = 0.0f;
    p.roll_cs[3] = 0.0f;
}

// Fill a PoseBlock from Euler angles in degrees, yaw about +y, pitch about
// +x, roll about +z -- the same composition tools/headlook_clip.py uses, so
// the layer and the offline check cannot come to disagree about what "10
// degrees of yaw" means. r_inv is the TRANSPOSE, which for a rotation is the
// inverse exactly; computing it as an inverse would introduce a difference
// the shader would then have to be robust to.
inline void pose_set_euler(PoseBlock &p, float yaw_deg, float pitch_deg,
                           float roll_deg) {
    const float d = 0.01745329251994330f;
    const float cy = std::cos(yaw_deg * d), sy = std::sin(yaw_deg * d);
    const float cp = std::cos(pitch_deg * d), sp = std::sin(pitch_deg * d);
    const float cr = std::cos(roll_deg * d), sr = std::sin(roll_deg * d);
    const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};     // row-major 3x3
    const float rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
    const float rz[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
    float a[9], m[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            a[i * 3 + j] = 0;
            for (int k = 0; k < 3; k++)
                a[i * 3 + j] += ry[i * 3 + k] * rx[k * 3 + j];
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            m[i * 3 + j] = 0;
            for (int k = 0; k < 3; k++)
                m[i * 3 + j] += a[i * 3 + k] * rz[k * 3 + j];
        }
    p = PoseBlock{};
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            p.r[c * 4 + r] = m[r * 3 + c];      // column-major for std140
            p.r_inv[c * 4 + r] = m[c * 3 + r];  // transpose == inverse
        }
    p.r[15] = p.r_inv[15] = 1.0f;
    // r_prev starts EQUAL to r, which is the no-head-motion case and the only
    // honest starting value: on the first frame there is no previous pose, and
    // seeding it with identity would announce a full head rotation's worth of
    // motion for one frame. pose_roll_frame() advances it afterwards.
    for (int i = 0; i < 16; i++)
        p.r_prev[i] = p.r[i];
}

inline PoseBlock pose_identity() {
    PoseBlock p{};
    for (int i = 0; i < 4; i++)
        p.r[i * 4 + i] = p.r_inv[i * 4 + i] = p.r_prev[i * 4 + i] = 1.0f;
    // (cos 0, sin 0). Zeros here would put the eye offset at the origin and
    // collapse the stereo to mono -- the identity of a direction is not the
    // identity of a matrix, and value-initialising the struct gives the wrong
    // one for this member alone.
    p.roll_cs[0] = 1.0f;
    p.roll_cs[1] = 0.0f;
    return p;
}

// Roll the current rotation into the previous slot, once per frame. With a
// static X4VR_POSE_YAW this is a no-op by construction -- r never changes, so
// r_prev == r always -- and that is exactly why take 189 is a clean test of
// the patch rather than of this function. It exists now so that a live pose is
// correct the day it lands instead of reintroducing take 188's seam in motion.
inline void pose_roll_frame(PoseBlock &p) {
    for (int i = 0; i < 16; i++)
        p.r_prev[i] = p.r[i];
}

// How coarse X4's shadows are, and whether that tracks the field -- task #56.
//
// **The question.** #33's guard band buys head-rotation range by widening
// X4's `<fov>` so the engine culls more than it shows. Every stored run says
// the shadow atlas does NOT grow to match: 10 maps of 2048x2048 D16 at fov
// 1.0, 1.1111, 1.437, 1.4917 and 1.5, and at render extents 1408 and 4224.
// The pixel budget is fixed. So IF X4 fits its cascades to the camera
// frustum, widening the field spreads the same texels over a wider world and
// every shadow in the frame gets coarser -- including in the part the player
// is looking at. If instead it fits them to a world extent from the video
// settings, the field costs nothing here.
//
// **What settles it is a RATIO, so no convention has to be right.** The row
// length of the world-to-shadow matrix is NDC-per-metre. Measure it at two
// fields and divide: fitted-to-frustum shrinks it, fixed-extent does not.
// That is why `csm0_scale` is reported raw beside the derived metres-per-texel
// -- if the derivation's convention is wrong the ratio still answers.
//
// The metres-per-texel derivation, for the record: the fragment code weights a
// cascade by `min(coord, -coord + 1)`, which is zero at 0 and at 1, so shadow
// coords are in **[0,1]** and the cascade spans `1 / |row|` metres. The kernel
// width is `1 / (F_shadowmapsize * texturefactor)`, so that product is the
// texel count across the same [0,1]. Hence
//
//     metres per texel = 1 / (|row| * F_shadowmapsize * texturefactor)
//
// **`populated` is not decoration.** Members 9 and 10 are read by ZERO of the
// 409 dumped modules -- the shaders take their cascade matrices from the
// per-object block instead -- so X4 may leave these at whatever the buffer
// held. A zero matrix would otherwise report an infinite cascade, which reads
// as "fixed extent" and would answer the question backwards.
// Member 3 of the WORLD/object block (set 3 binding 0) is `M_shadowCSM0`, at
// byte 192 -> float 48; CSM1 follows at 64. It is NOT the camera block, and
// `kShadowCSM0` above (float 144) indexes the camera block's
// `M_shadowCSM0Clip` -- correct for that block, and the wrong object entirely.
//
// **|row0| of this matrix is NOT a cascade scale**, which is the thing 50-odd
// takes assumed. X4's own GLSL applies M_shadowCSM0 to three different source
// frames -- `mul(vec4(Pos,1), M_shadowCSM0)` in 186 vertex entry points
// (object space), `mul(vec4(IO_world_pos,1), ...)` in others, and
// `mul(vec4(view_pos,1), ...)` in 54 fragment ones -- so the block carries the
// draw's own transform composed with the cascade. Only a slot bound to a
// view-space reader yields a cascade scale, and neither this function nor the
// layer's slot walk establishes which. #56 was answered from `V_csmthresholds`
// instead; see docs/x4-quirks.md.
enum : uint32_t {
    kObjShadowCSM0 = 48,      // M_shadowCSM0, object block member 3
    kObjShadowCSM1 = 64,      // M_shadowCSM1, member 4
    // X4 binds this block with range **768** -- measured, 13748 binds in take
    // 202, no other range at set 3. Not the struct size: the declared members
    // reach ~560 bytes, and 768 is that rounded up to a 256-byte offset
    // alignment. The earlier 576 here was a guess from a dump and was never
    // what X4 bound.
    kObjBlockBytes = 768,
};

struct ShadowTerms {
    bool ok = false;
    bool populated = false;   // the live CSM matrix was supplied and is nonzero
    // **The cascade numbers are only meaningful when this is true.**
    //
    // Take 201: `read_shadow_terms` read `M_shadowCSM0Clip`, camera-block
    // member 9. Over all 409 dumps, 370 modules DECLARE it and **0 sample
    // it**; the live matrix is `M_shadowCSM0` at member 3 of the WORLD/object
    // block, sampled by 189. Different blocks -- 99 modules declare both and
    // none shares a struct. X4 has no reason to keep a member nothing reads
    // current, so what came back was whatever the buffer held: takes 197 and
    // 201 are the same knobs and a byte-identical layer and disagree, while
    // the same takes' seam measurements reproduce to 0.1 points.
    //
    // `populated` guarded the all-zero case and could not guard plausible
    // stale data -- the harder failure, because it reads as a measurement.
    // This flag is that guard: the cascade fields stay zero and unreported
    // unless a caller supplies the LIVE matrix.
    bool cascade_measured = false;
    float map_size = 0.0f;    // F_shadowmapsize        -- camera block, live
    float max_distance = 0.0f;
    float blend = 0.0f;
    float factor[5] = {0, 0, 0, 0, 0};
    float thresholds[4] = {0, 0, 0, 0};
    float csm0_scale = 0.0f;  // |row0| of M_shadowCSM0, NDC per metre
    float csm1_scale = 0.0f;
    float csm0_m_per_texel = 0.0f;
};

// `blk` is the CAMERA block (set 1 binding 0): the scalars genuinely live
// there and are sampled by X4's own shaders.
//
// `csm0` / `csm1` are members 3 and 4 of the OBJECT block (set 3 binding 0) at
// kObjShadowCSM0 / kObjShadowCSM1. They are separate arguments because they
// are in a DIFFERENT BUFFER, which the layer does now capture -- but see the
// enum above: what comes back is the draw's own transform composed with the
// cascade, so `csm0_scale` is a cascade scale only for a view-space reader.
// Passing nullptr makes the function report the camera-block scalars and
// refuse the matrix, which is the right call whenever the caller cannot say
// which reader its slot belongs to.
inline ShadowTerms read_shadow_terms(const float *blk, Major major,
                                     const float *csm0 = nullptr,
                                     const float *csm1 = nullptr) {
    ShadowTerms s{};
    if (!blk || major == Major::Unknown)
        return s;
    s.map_size = blk[kShadowMapSize];
    s.max_distance = blk[kShadowMaxDistance];
    s.blend = blk[kCsmBlendStrength];
    for (int i = 0; i < 5; i++)
        s.factor[i] = blk[kTextureFactorCSM0 + i];
    for (int i = 0; i < 4; i++)
        s.thresholds[i] = blk[kCsmThresholds + i];
    auto row0 = [major](const Mat4 &m) {
        // Row 0 of the transform, whichever way it is stored.
        const float a = major == Major::Row ? m.m[0] : m.m[0];
        const float b = major == Major::Row ? m.m[1] : m.m[4];
        const float c = major == Major::Row ? m.m[2] : m.m[8];
        return std::sqrt(a * a + b * b + c * c);
    };
    // No cascade source, no cascade numbers. The scalars above are still
    // real, so `ok` stays true and the caller reports what it has.
    if (!csm0) {
        s.ok = true;
        return s;
    }
    const Mat4 c0 = load(csm0), c1 = load(csm1 ? csm1 : csm0);
    s.csm0_scale = row0(c0);
    s.csm1_scale = csm1 ? row0(c1) : 0.0f;
    bool any = false;
    for (int i = 0; i < 16 && !any; i++)
        any = std::fabs(c0.m[i]) > 1e-12f;
    s.populated = any;
    s.cascade_measured = any;
    if (s.populated && s.csm0_scale > 1e-12f && s.map_size > 0.0f &&
        s.factor[0] > 0.0f)
        s.csm0_m_per_texel = 1.0f / (s.csm0_scale * s.map_size * s.factor[0]);
    s.ok = true;
    return s;
}

// **Task #77: where the eye offset points when the head is rolled.**
//
// The disparity is baked into the image along image-x, in X4's camera frame,
// which has no roll. The compositor then displays that image for a head at
// roll phi by rotating it by -phi -- and the disparity rotates with it. The
// eyes are separated along panel-x always, because the panels are head-fixed,
// so sin(phi) of the disparity ends up VERTICAL. Vertical disparity cannot be
// fused: it produces strain and double vision rather than depth.
//
// Patola found it at 90 deg by closing one eye at a time. The reason it
// matters far more than that stunt: it scales as sin(phi), so a 15-20 deg head
// tilt -- held unconsciously for long stretches -- already turns a third of the
// disparity vertical, and nobody would report that as "roll is broken".
//
// So point the offset where it has to end up. Baking (cos phi, sin phi) means
// the compositor's -phi lands it back on panel-x exactly:
//
//     rotate((m cos, m sin), -phi) = (m cos^2 + m sin^2, -m cos sin + m sin cos)
//                                  = (m, 0)
//
// The zero in the second component is the whole fix, and it is exact rather
// than small. tests/view_math.cpp checks both that and the defect it replaces.
struct EyeOffset {
    float dx = 0.0f, dy = 0.0f;
};

inline EyeOffset eye_offset_for_roll(float ipd, float roll_rad, bool right) {
    const float h = (right ? 0.5f : -0.5f) * ipd;
    EyeOffset o;
    o.dx = h * std::cos(roll_rad);
    o.dy = h * std::sin(roll_rad);
    return o;
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
inline void compose_eye_untranslate(float dx, float dy, float m[16]) {
    for (int c = 0; c < 4; c++) {
        m[c * 4 + 0] += dx * m[c * 4 + 3];
        m[c * 4 + 1] += dy * m[c * 4 + 3];
    }
}

// The scalar form, kept because task #22's callers mean exactly this: an
// unrolled eye, offset along x alone. It delegates rather than duplicating the
// loop -- **#77's regression was two spellings of one map**, where the vertex
// stage learned to rotate the offset and this one did not, so the last thing
// this file should carry is a second place to change.
inline void compose_eye_untranslate(float d, float m[16]) {
    compose_eye_untranslate(d, 0.0f, m);
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
