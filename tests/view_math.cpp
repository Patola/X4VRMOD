// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The projection math, checked without a GPU or a running X4.
//
// make_eye_shear() is an analytic shortcut: instead of building P, T(-d) and
// P^-1 and multiplying them, it writes a single term. That shortcut is only
// valid for projections shaped like X4's (reversed-Z, infinite far plane, clip
// z constant). So the load-bearing test here is not "does it fill in the field
// we expect" but "does the shortcut agree with the long way", computed through
// the general mul/invert helpers on a matrix of X4's measured shape.
//
// read_proj_terms() is tested against the same matrix in both storage orders,
// because a transposed read is the failure mode that looks plausible: it still
// returns a number, and the number is still a float near the right magnitude.
#include "../common/x4vr_view.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <initializer_list>

static int g_fail = 0;

static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_fail++;
}

static void check_near(float got, float want, float tol, const char *what) {
    const bool ok = std::fabs(got - want) <= tol;
    printf("%s %s (got %.6f want %.6f)\n", ok ? "ok  " : "FAIL", what, got,
           want);
    if (!ok)
        g_fail++;
}

static x4vr::Mat4 identity() {
    x4vr::Mat4 m{};
    for (int i = 0; i < 4; i++)
        m.m[i * 4 + i] = 1.0f;
    return m;
}

// X4's projection as measured in Phase 4a, column-major:
//   x_c = sx*x   y_c = sy*y   z_c = near   w_c = z
// Reversed-Z with an infinite far plane, Y-flipped (sy negative).
static x4vr::Mat4 x4_projection(float sx, float sy, float near_z) {
    x4vr::Mat4 p{};
    p.m[0] = sx;      // P[0][0]
    p.m[5] = sy;      // P[1][1]
    p.m[11] = 1.0f;   // P[3][2]: w_c = z
    p.m[14] = near_z; // P[2][3]: z_c = near
    return p;
}

static float deg(float d) { return d * 0.01745329251994330f; }
static float deg_of(float r) { return r * 57.2957795130823f; }

// Where a view-space ray belongs in a target frustum, written from the
// DEFINITION rather than from a matrix -- so the round-trip test below is not
// checking the implementation against a second copy of itself.
static void target_ndc(const x4vr::EyeFrustum &f, float x, float y, float z,
                       float out[2]) {
    out[0] = (2.0f * (x / z) - (f.tan_r + f.tan_l)) / (f.tan_r - f.tan_l);
    out[1] = -(2.0f * (y / z) - (f.tan_u + f.tan_d)) / (f.tan_u - f.tan_d);
}

// The inverse reading: what angle does a given NDC x sit at? This is how the
// probe reports the defect, so the test can quote its numbers back.
static float angle_of_ndc(const x4vr::EyeFrustum &f, float n) {
    const float u = 0.5f * (n + 1.0f);
    return std::atan(f.tan_l + u * (f.tan_r - f.tan_l));
}

static x4vr::Mat4 transpose(const x4vr::Mat4 &a) {
    x4vr::Mat4 t{};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t.m[c * 4 + r] = a.m[r * 4 + c];
    return t;
}

int main() {
    // The two configurations that matter: what Phase 4a measured at 2816x1408,
    // and what a 1408x1408 eye should give if sx tracks the aspect.
    const float kSy = 1.778f;
    const float kNear = 0.1f;

    // --- read_proj_terms, column-major -----------------------------------
    {
        const x4vr::Mat4 p = x4_projection(0.889f, -kSy, kNear);
        check(x4vr::detect_major_proj(p) == x4vr::Major::Column,
              "X4-shaped projection detects as column-major");
        const x4vr::ProjTerms t = x4vr::read_proj_terms(p, x4vr::Major::Column);
        check(t.ok, "column-major terms read");
        check_near(t.sx, 0.889f, 1e-6f, "column-major sx");
        check_near(t.sy, -kSy, 1e-6f, "column-major sy (Y-flipped)");
        check_near(t.near_z, kNear, 1e-6f, "column-major near");
    }

    // --- read_proj_terms, row-major --------------------------------------
    // Same projection, transposed. near moves from m[14] to m[11]; sx and sy
    // stay on the diagonal. If read_proj_terms ignored the storage order it
    // would return 1.0 for near here -- a plausible-looking wrong answer.
    {
        const x4vr::Mat4 p = transpose(x4_projection(0.889f, -kSy, kNear));
        check(x4vr::detect_major_proj(p) == x4vr::Major::Row,
              "transposed projection detects as row-major");
        const x4vr::ProjTerms t = x4vr::read_proj_terms(p, x4vr::Major::Row);
        check(t.ok, "row-major terms read");
        check_near(t.sx, 0.889f, 1e-6f, "row-major sx");
        check_near(t.near_z, kNear, 1e-6f, "row-major near (from m[11])");
        check(std::fabs(p.m[14] - 1.0f) < 1e-6f,
              "row-major m[14] is the +-1 slot, not near");
    }

    // --- refusals ---------------------------------------------------------
    {
        const x4vr::Mat4 p = x4_projection(0.889f, -kSy, kNear);
        check(!x4vr::read_proj_terms(p, x4vr::Major::Unknown).ok,
              "unknown storage order refuses rather than guessing");
        const x4vr::Mat4 z{};
        check(!x4vr::read_proj_terms(z, x4vr::Major::Column).ok,
              "all-zero block refuses (near=0)");
        x4vr::Mat4 no_near = x4_projection(0.889f, -kSy, 0.0f);
        check(!x4vr::read_proj_terms(no_near, x4vr::Major::Column).ok,
              "zero near refuses");
    }

    // --- the shortcut equals the long way --------------------------------
    // K = P * T(-d) * P^-1, built through the general helpers, against
    // make_eye_shear's single term. Run at both aspects so the check is not
    // accidentally passing on one magic number.
    for (float sx : {0.889f, 1.778f}) {
        for (float ipd : {0.064f, 0.016f}) {
            const float d = 0.5f * ipd;
            const x4vr::Mat4 p = x4_projection(sx, -kSy, kNear);
            x4vr::Mat4 pinv{};
            check(x4vr::invert(p, pinv), "projection is invertible");

            // T(-d): offset_camera translates by -d, which is what moving the
            // camera by +d means for everything it looks at.
            const x4vr::Mat4 t =
                x4vr::offset_camera(identity(), x4vr::Major::Column, d, 0, 0);
            const x4vr::Mat4 k_ref = x4vr::mul(
                x4vr::mul(p, t, x4vr::Major::Column), pinv, x4vr::Major::Column);

            const x4vr::ProjTerms terms =
                x4vr::read_proj_terms(p, x4vr::Major::Column);
            const x4vr::Mat4 k =
                x4vr::make_eye_shear(terms.sx, 0.0f, terms.near_z, d);

            char what[128];
            snprintf(what, sizeof(what), "K matches P*T(-d)*P^-1 (sx=%.3f ipd=%.3f)",
                     sx, ipd);
            bool same = true;
            for (int i = 0; i < 16; i++)
                if (std::fabs(k.m[i] - k_ref.m[i]) > 1e-5f)
                    same = false;
            check(same, what);
            if (!same) {
                char a[512], b2[512];
                x4vr::format_mat(a, sizeof(a), k);
                x4vr::format_mat(b2, sizeof(b2), k_ref);
                printf("     shortcut %s\n     longway  %s\n", a, b2);
            }
        }
    }

    // --- the magnitude the shear actually applies -------------------------
    // The shear is d*sx/near. sx carries the aspect, so a projection measured
    // at one aspect and used at another is wrong by exactly the aspect ratio.
    // This is the arithmetic behind task #23, asserted so the claim in the
    // docs cannot quietly stop being true.
    {
        const float d = 0.5f * 0.064f;
        const x4vr::Mat4 wide = x4_projection(0.889f, -kSy, kNear);
        const x4vr::Mat4 square = x4_projection(1.778f, -kSy, kNear);
        const x4vr::ProjTerms tw =
            x4vr::read_proj_terms(wide, x4vr::Major::Column);
        const x4vr::ProjTerms ts =
            x4vr::read_proj_terms(square, x4vr::Major::Column);
        const x4vr::Mat4 kw = x4vr::make_eye_shear(tw.sx, 0, tw.near_z, d);
        const x4vr::Mat4 ks = x4vr::make_eye_shear(ts.sx, 0, ts.near_z, d);
        check_near(std::fabs(kw.m[8]), 0.28448f, 1e-4f,
                   "2816x1408 shear reproduces the logged 0.28448");
        check_near(std::fabs(ks.m[8]) / std::fabs(kw.m[8]), 2.0f, 1e-4f,
                   "square-eye shear is exactly 2x the wide-eye shear");
    }

    // --- the in-shader form needs only sx ---------------------------------
    // Take 53 measured m[10] = 0, so clip z is the constant near plane for
    // every vertex. K's single term then collapses:
    //     x_c' = x_c + (-sx*d/near)*z_c = x_c - sx*d
    // near cancels. That is what lets the shader patch load one scalar and
    // skip the matrix multiply -- 1 mul + 1 sub per vertex instead of a full
    // mat4, so the correct version is cheaper than the wrong one.
    //
    // Checked against K*p for *real* clip positions -- points that came
    // through P. For an arbitrary vector the two do not agree, and asserting
    // on arbitrary vectors would be asserting the wrong property.
    {
        const float sx = 1.33333f, near_z = 0.1f;
        const float d = 0.5f * 0.064f;
        const x4vr::Mat4 p = x4_projection(sx, -sx, near_z);
        const x4vr::Mat4 k = x4vr::make_eye_shear(sx, 0.0f, near_z, d);

        // column-major: out[row] = sum_col m[col*4+row] * v[col]
        auto apply = [](const x4vr::Mat4 &m, const float v[4], float out[4]) {
            for (int r = 0; r < 4; r++) {
                out[r] = 0.0f;
                for (int c = 0; c < 4; c++)
                    out[r] += m.m[c * 4 + r] * v[c];
            }
        };

        const float pts[5][4] = {{0, 0, 1, 1},      {1, 2, 10, 1},
                                 {-3, 0.5f, 0.5f, 1}, {100, -50, 5000, 1},
                                 {0.01f, 0.01f, 0.11f, 1}};
        bool all_ok = true, z_const = true;
        for (const auto &v : pts) {
            float clip[4], sheared[4];
            apply(p, v, clip);
            apply(k, clip, sheared);
            if (std::fabs(clip[2] - near_z) > 1e-5f)
                z_const = false;
            // the one-scalar form: only x moves, by exactly sx*d
            if (std::fabs(sheared[0] - (clip[0] - sx * d)) > 1e-5f ||
                std::fabs(sheared[1] - clip[1]) > 1e-6f ||
                std::fabs(sheared[2] - clip[2]) > 1e-6f ||
                std::fabs(sheared[3] - clip[3]) > 1e-6f)
                all_ok = false;
        }
        check(z_const, "clip z is the constant near plane for every vertex");
        check(all_ok, "K*p equals p with x -= sx*d (one scalar, no matrix)");
    }

    // ---- the canvas shift agrees with the shear it is derived from -------
    //
    // Task #30 claims canvas_shift(z) is the world shear evaluated at a fixed
    // depth. If that is only approximately true the UI sits at a distance
    // nobody chose, and nothing on screen says so -- a canvas 30 cm from where
    // it was asked for still looks like a canvas.
    //
    // The shear displaces a point at view depth z by m8·(clip z)/w = m8·near/z,
    // because the derivation holds clip z at the near plane (asserted above).
    // So `near` must cancel: the same z through three different near planes has
    // to give the same shift, and all three have to equal canvas_shift.
    {
        const float sx = 1.3333f, ipd = 0.064f;
        for (float z : {0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 60.0f}) {
            const float s = x4vr::canvas_shift(sx, ipd, z);
            for (float near_z : {0.01f, 0.1f, 1.0f}) {
                const x4vr::Mat4 kl =
                    x4vr::make_eye_shear(sx, 0.0f, near_z, -0.5f * ipd);
                char what[96];
                snprintf(what, sizeof what,
                         "canvas z=%.2g equals the left shear at near=%.2g", z,
                         near_z);
                check_near(kl.m[8] * near_z / z, s, 1e-6f, what);
            }
        }
        // The numbers the design was written against, so a change of units or
        // a dropped factor of two is caught by name rather than by ratio.
        check_near(x4vr::canvas_shift(1.3333f, 0.064f, 1.0f), 0.0426656f, 1e-6f,
                   "1 m -> s = 0.04267 (30 px on a 1408-wide eye)");
        check_near(x4vr::canvas_shift(1.3333f, 0.064f, 2.0f) * 704.0f, 15.0f,
                   0.02f, "2 m -> 15 px per eye on a 1408-wide eye");
        check_near(x4vr::canvas_shift(1.3333f, 0.064f, 10.0f) * 704.0f, 3.0f,
                   0.01f, "10 m -> 3 px per eye");
        // Infinity, and the two ways of asking for something that is not a
        // distance. All three must be the *same* answer as today's frame,
        // because P102 is that an unset canvas reproduces take 96 exactly.
        check(x4vr::canvas_shift(1.3333f, 0.064f, 0.0f) == 0.0f,
              "z = 0 is refused as a shift of zero, not a division");
        check(x4vr::canvas_shift(1.3333f, 0.064f, -3.0f) == 0.0f,
              "a negative distance is refused, not mirrored");
        check_near(x4vr::canvas_shift(1.3333f, 0.064f, 1e9f), 0.0f, 1e-9f,
                   "an effectively infinite canvas is mono, as it is today");
        // The left eye's shift is positive. The sign is the one thing here
        // that cannot be caught by a magnitude check, and getting it backwards
        // puts the UI behind the starfield instead of in front of it.
        check(x4vr::canvas_shift(1.3333f, 0.064f, 2.0f) > 0.0f &&
                  x4vr::make_eye_shear(1.3333f, 0.0f, 0.1f, -0.032f).m[8] > 0.0f,
              "left eye is +s, matching the sign of the left eye's shear");
    }

    // ---- Task #23: recovering sx from M_worldviewprojection alone ----
    //
    // Twelve of X4's World vertex modules declare ONE descriptor -- the set-3
    // per-object block -- and no camera block, so patch_vertex_eye_offset
    // refuses them and they fall back to a baked sx. At rest that is now
    // correct; under zoom the scene camera runs 0.75405 to 29.18689, a factor
    // of 38, and those modules keep shearing by the resting value.
    //
    // sx is not stored anywhere they can see. It is however *recoverable*, and
    // that is what this block checks before any shader is patched on the
    // strength of it. With X4's projection,
    //
    //     row0(P) = [sx 0 0 0]        so  row0(MVP) = sx * row0(VW)
    //     row3(P) = [0 0 1 0]         so  row3(MVP) =      row2(VW)
    //
    // and for a view matrix that is rigid and an object matrix that is rigid
    // times a uniform scale s, the linear parts of row0(VW) and row2(VW) both
    // have magnitude s. So
    //
    //     |row0(MVP).xyz| / |row3(MVP).xyz| = (sx*s)/s = sx
    //
    // exactly, with the object's scale cancelling. One divide and one sqrt per
    // vertex, no new descriptor, no push constant, no pipeline layout change.
    {
        printf("\n-- #23: sx recovered from MVP --\n");
        auto mul = [](const x4vr::Mat4 &a, const x4vr::Mat4 &b) {
            x4vr::Mat4 o{}; // column-major: element (row r, col c) is m[c*4+r]
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) {
                    float s = 0.0f;
                    for (int k = 0; k < 4; k++)
                        s += a.m[k * 4 + r] * b.m[c * 4 + k];
                    o.m[c * 4 + r] = s;
                }
            return o;
        };
        // The recovery, written exactly as the shader will compute it.
        auto recover = [](const x4vr::Mat4 &mvp) {
            const float r0[3] = {mvp.m[0], mvp.m[4], mvp.m[8]};
            const float r3[3] = {mvp.m[3], mvp.m[7], mvp.m[11]};
            const float n0 = r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2];
            const float n3 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
            return n3 > 0.0f ? std::sqrt(n0 / n3) : 0.0f;
        };
        // Rigid transform: rotation about a normalised axis, then translation,
        // then a uniform scale.
        auto rigid = [](float ax, float ay, float az, float ang, float tx,
                        float ty, float tz, float s) {
            const float l = std::sqrt(ax * ax + ay * ay + az * az);
            ax /= l; ay /= l; az /= l;
            const float c = std::cos(ang), si = std::sin(ang), t = 1.0f - c;
            x4vr::Mat4 m{};
            // column 0
            m.m[0] = (t * ax * ax + c) * s;
            m.m[1] = (t * ax * ay + si * az) * s;
            m.m[2] = (t * ax * az - si * ay) * s;
            // column 1
            m.m[4] = (t * ax * ay - si * az) * s;
            m.m[5] = (t * ay * ay + c) * s;
            m.m[6] = (t * ay * az + si * ax) * s;
            // column 2
            m.m[8] = (t * ax * az + si * ay) * s;
            m.m[9] = (t * ay * az - si * ax) * s;
            m.m[10] = (t * az * az + c) * s;
            // column 3
            m.m[12] = tx; m.m[13] = ty; m.m[14] = tz; m.m[15] = 1.0f;
            return m;
        };

        // Every sx this project has actually measured, not a round number:
        // the fov=1.0 base, the profile, the two widened fields, and the zoom
        // stop that take 106 landed on.
        const float sxs[] = {1.33333f, 1.15174f, 0.83923f, 0.75405f,
                             0.69231f, 3.78085f, 29.18689f, 37.75372f};
        int worst_case = 0;
        float worst = 0.0f;
        for (float sx : sxs) {
            const x4vr::Mat4 p = x4_projection(sx, -sx, 0.1f);
            // X4 renders camera-relative, so its M_view is identity in every
            // dump -- but the derivation does not depend on that, and a test
            // that only ever passes V = I would not notice if it did.
            const x4vr::Mat4 views[] = {
                identity(),
                rigid(0.3f, 1.0f, -0.2f, 0.7f, 12.0f, -3.0f, 40.0f, 1.0f)};
            for (const x4vr::Mat4 &v : views)
                for (int i = 0; i < 6; i++) {
                    // Uniform scales spanning what X4 draws: a cockpit prop to
                    // a station module.
                    const float s = (float[]){1.0f,   0.01f, 0.25f,
                                              7.5f,   120.0f, 3000.0f}[i];
                    const x4vr::Mat4 w =
                        rigid(1.0f, 0.4f * i - 1.0f, 0.9f, 0.4f * i,
                              -20.0f * i, 5.0f * i, 300.0f * i, s);
                    const float got = recover(mul(p, mul(v, w)));
                    const float rel = std::fabs(got - sx) / sx;
                    if (rel > worst) { worst = rel; worst_case = i; }
                }
        }
        check(worst < 1e-4f, "sx recovered from MVP for every measured sx, "
                             "both views and scales from 0.01 to 3000");
        printf("     worst relative error %.2e (scale case %d)\n", worst,
               worst_case);

        // The assumption, stated as a failing case rather than as a sentence.
        // A NON-uniform object scale breaks the cancellation, and the error is
        // the ratio of the two axes' scales -- so this pins how wrong it gets
        // rather than leaving "rare in practice" as the only argument.
        {
            const float sx = 0.75405f;
            x4vr::Mat4 w = identity();
            w.m[0] = 2.0f;  // x scaled 2x
            w.m[10] = 1.0f; // z left alone
            const float got =
                recover(mul(x4_projection(sx, -sx, 0.1f), mul(identity(), w)));
            check_near(got, sx * 2.0f, 1e-4f,
                       "a 2x non-uniform x scale reports 2x sx — the "
                       "cancellation needs uniform scale, and this is how far "
                       "it misses");
        }

        // Degenerate input must not produce a plausible number. A zero matrix
        // is what an unpopulated block reads as, and take 104's proj instrument
        // already refuses those; the shader gets the same treatment.
        {
            x4vr::Mat4 zero{};
            check(recover(zero) == 0.0f,
                  "an unpopulated block recovers 0, not a NaN or a guess");
        }
    }

    // ---- Task #35: the per-eye off-axis map -----------------------------
    //
    // Checked against the DEFINITION of the target frustum rather than against
    // a matrix built the same way the code under test builds it. The failure
    // this guards is the one that already happened once: the first eye test
    // card was written as if the frusta were symmetric, against a prediction
    // that said they would not be, and it took a headset run to find out.
    {
        printf("\n-- #35: off-axis map onto the runtime's canted frusta --\n");

        // WiVRn on a Quest 3, as measured by tests/run-xr-probe.sh. View 0 is
        // the left eye (the probe confirmed view 1 is +0.063 m in x).
        const x4vr::EyeFrustum eye[2] = {
            x4vr::frustum_of_angles(deg(-54.0f), deg(40.0f), deg(44.0f),
                                    deg(-55.0f)),
            x4vr::frustum_of_angles(deg(-40.0f), deg(54.0f), deg(44.0f),
                                    deg(-55.0f))};
        const float kIpd = 0.0630f;
        const float d_eye[2] = {-0.5f * kIpd, +0.5f * kIpd};
        // view-space points as {x, y, z, 1}, same shape as the block above
        const float pts[5][4] = {{0, 0, 1, 1},        {1, 2, 10, 1},
                                 {-3, 0.5f, 0.5f, 1}, {100, -50, 5000, 1},
                                 {0.01f, 0.01f, 0.11f, 1}};

        // What X4 must render so nothing visible is culled: one symmetric
        // frustum covering both eyes' worst edge.
        const float half = x4vr::union_half_angle(eye, 2);
        check_near(half * 57.2957795130823f, 55.0f, 1e-3f,
                   "union half-angle is 55 deg (the D-55 edge, not the L-54)");
        check_near(x4vr::x4_fov_for_half(half), 1.4917f, 1e-3f,
                   "X4VR_FOV = 1.4917 covers the union");
        // The two readings of the same law, which two callers now depend on
        // agreeing: the declared field and the canvas's screen field.
        {
            float worst = 0.0f;
            for (float f : {0.5f, 1.0f, 1.437f, 1.4917f, 2.0f})
                worst = std::fmax(
                    worst,
                    std::fabs(x4vr::x4_fov_for_half(x4vr::x4_half_for_fov(f)) -
                              f));
            check(worst < 1e-5f,
                  "x4_half_for_fov inverts x4_fov_for_half over five fovs");
            check_near(x4vr::x4_half_for_fov(0.0f),
                       x4vr::x4_half_for_fov(1.0f), 1e-9f,
                       "an unset fov reads as X4's default of 1.0");
        }
        const float sx_union = 1.0f / std::tan(half);
        check_near(sx_union, 0.70021f, 1e-4f, "union sx = cot(55 deg)");

        // The constants the docs quote, so a change of convention has to
        // change the doc too.
        const x4vr::OffAxis oa0 = x4vr::make_off_axis(eye[0], d_eye[0]);
        const x4vr::OffAxis oa1 = x4vr::make_off_axis(eye[1], d_eye[1]);
        check(oa0.ok && oa1.ok, "both frusta accepted");
        check_near(oa0.ax_num / sx_union, 1.2892f, 1e-3f, "view 0 A_x = 1.2892");
        check_near(oa0.bx, +0.2425f, 1e-3f, "view 0 B_x = +0.2425");
        check_near(oa1.bx, -0.2425f, 1e-3f, "view 1 B_x = -0.2425 (mirrored)");
        check_near(oa0.ay_num / -sx_union, 1.1932f, 1e-3f,
                   "view 0 A_y = 1.1932 (sy is negative, A_y is not)");
        check_near(oa0.by, -0.1932f, 1e-3f, "view 0 B_y = -0.1932");
        check(oa0.by == oa1.by,
              "both eyes share the vertical — measured, not assumed");

        // ---- the identity case ----
        // A symmetric target at the angle X4 is already rendering must leave
        // every clip position untouched. A map that is not the identity here
        // is wrong everywhere, and this is the cheapest place to see it.
        {
            bool all_id = true;
            for (float t_deg : {20.0f, 36.87f, 55.0f, 70.0f}) {
                const float t = deg(t_deg), tt = std::tan(t);
                const x4vr::EyeFrustum sym =
                    x4vr::frustum_of_angles(-t, t, t, -t);
                const x4vr::OffAxis o = x4vr::make_off_axis(sym, 0.0f);
                const float sx = 1.0f / tt, sy = -1.0f / tt;
                for (const float *v : {pts[0], pts[1], pts[2], pts[3], pts[4]}) {
                    float clip[4] = {sx * v[0], sy * v[1], 0.1f, v[2]};
                    const float before[4] = {clip[0], clip[1], clip[2], clip[3]};
                    x4vr::apply_off_axis(o, sx, sy, clip);
                    for (int i = 0; i < 4; i++)
                        if (std::fabs(clip[i] - before[i]) > 1e-5f)
                            all_id = false;
                }
            }
            check(all_id,
                  "a symmetric target at X4's own half-angle is the identity");
        }

        // ---- the round trip, against the definition ----
        // X4's projection, then the shear the layer already applies, then the
        // affine -- versus the target NDC computed straight from the ray. Run
        // over both eyes, several sx (including the 1.5:1 non-square map
        // camera, where reading sy independently is what saves it), and points
        // spread across the field including well outside it.
        {
            const float cam[][2] = {// {sx, |sy/sx|}
                                    {0.70021f, 1.0f},  // the union scene camera
                                    {0.75405f, 1.0f},  // X4VR_FOV=1.437
                                    {1.33333f, 1.0f},  // fov 1.0
                                    {3.78085f, 1.5f},  // the map camera
                                    {29.18689f, 1.0f}}; // full zoom
            const float world[][3] = {{0, 0, 1},      {0.5f, 0.3f, 2},
                                      {-1.2f, 0.9f, 3}, {4.0f, -2.5f, 1.5f},
                                      {-0.05f, 0.02f, 0.4f}, {60.0f, 40.0f, 250.0f}};
            float worst = 0.0f;
            for (int e = 0; e < 2; e++) {
                const x4vr::OffAxis o = x4vr::make_off_axis(eye[e], d_eye[e]);
                for (const auto &c : cam) {
                    const float sx = c[0], sy = -c[0] * c[1];
                    for (const auto &p : world) {
                        // X4 -> clip, then the shear, then the affine.
                        float clip[4] = {sx * p[0], sy * p[1], 0.1f, p[2]};
                        clip[0] -= sx * d_eye[e];
                        x4vr::apply_off_axis(o, sx, sy, clip);
                        const float got[2] = {clip[0] / clip[3],
                                              clip[1] / clip[3]};
                        // The definition: where that ray belongs in this eye.
                        float want[2];
                        target_ndc(eye[e], p[0] - d_eye[e], p[1], p[2], want);
                        for (int i = 0; i < 2; i++) {
                            const float err = std::fabs(got[i] - want[i]);
                            if (err > worst)
                                worst = err;
                        }
                    }
                }
            }
            check(worst < 1e-4f,
                  "clip -> shear -> affine equals the target frustum's own NDC, "
                  "both eyes, 5 cameras incl. a 1.5:1 one, 6 points");
            printf("     worst NDC error %.2e\n", worst);
        }

        // ---- the defect, and its repair, in the units the probe reports ----
        // Straight ahead from each eye. Without the affine it lands at the
        // image centre in both, which the probe measured as 30.07 deg of
        // divergence; with it, at 0 deg in both.
        {
            const float before[2] = {angle_of_ndc(eye[0], 0.0f),
                                     angle_of_ndc(eye[1], 0.0f)};
            check_near(deg_of(before[0]), -15.037f, 1e-2f,
                       "uncorrected: straight ahead reads -15.04 deg in view 0");
            check_near(deg_of(before[0] - before[1]) , -30.074f, 2e-2f,
                       "uncorrected divergence is the 30.07 deg the probe found");

            float worst = 0.0f;
            for (int e = 0; e < 2; e++) {
                const x4vr::OffAxis o = x4vr::make_off_axis(eye[e], d_eye[e]);
                const float sx = 0.70021f, sy = -0.70021f;
                // a point directly in front of THIS eye, 5 m out
                float clip[4] = {sx * d_eye[e], 0.0f, 0.1f, 5.0f};
                clip[0] -= sx * d_eye[e];
                x4vr::apply_off_axis(o, sx, sy, clip);
                const float a = angle_of_ndc(eye[e], clip[0] / clip[3]);
                if (std::fabs(deg_of(a)) > worst)
                    worst = std::fabs(deg_of(a));
            }
            check(worst < 1e-2f,
                  "corrected: straight ahead reads 0 deg in BOTH views");
            printf("     residual %.4f deg\n", worst);
        }

        // ---- the vertical sign, stated as a direction ----
        // A magnitude check cannot catch a flip, and a flip is invisible in
        // any aggregate: the image simply looks upside down in the headset.
        {
            const x4vr::OffAxis o = x4vr::make_off_axis(eye[0], d_eye[0]);
            const float sx = 0.70021f, sy = -0.70021f;
            float up[4] = {0.0f, sy * 1.0f, 0.1f, 2.0f}; // 1 m ABOVE, 2 m out
            x4vr::apply_off_axis(o, sx, sy, up);
            check(up[1] / up[3] < 0.0f,
                  "a point above the camera lands at negative NDC y "
                  "(Vulkan's y points down, and X4's sy already flipped it)");
            check(o.ay_num / sy > 0.0f,
                  "A_y is positive, so the affine preserves the flip rather "
                  "than adding a second one");
        }

        // ---- folded equals composed ----
        // The eye offset can be folded into the constant term, which drops one
        // multiply. Proven here so the optimisation is available with a test
        // already written, rather than adopted on the strength of an argument.
        {
            float worst = 0.0f;
            for (int e = 0; e < 2; e++) {
                const x4vr::OffAxis o = x4vr::make_off_axis(eye[e], d_eye[e]);
                for (float sx : {0.70021f, 1.33333f, 3.78085f}) {
                    const float sy = -sx;
                    for (const float *v : {pts[0], pts[1], pts[3]}) {
                        float a[4] = {sx * v[0], sy * v[1], 0.1f, v[2]};
                        float b[4] = {a[0], a[1], a[2], a[3]};
                        a[0] -= sx * d_eye[e];
                        x4vr::apply_off_axis(o, sx, sy, a);
                        x4vr::apply_off_axis_folded(o, sx, sy, b);
                        for (int i = 0; i < 4; i++)
                            worst = std::fmax(worst, std::fabs(a[i] - b[i]));
                    }
                }
            }
            check(worst < 1e-5f, "folded form equals shear-then-affine");
        }

        // ---- refusals ----
        {
            check(!x4vr::make_off_axis(x4vr::EyeFrustum{}, 0.0f).ok,
                  "an all-zero frustum refuses rather than dividing by zero");
            x4vr::EyeFrustum back = eye[0];
            std::swap(back.tan_l, back.tan_r);
            check(!x4vr::make_off_axis(back, 0.0f).ok,
                  "a left/right swapped frustum refuses -- negative span");
            const x4vr::OffAxis o = x4vr::make_off_axis(eye[0], d_eye[0]);
            float clip[4] = {1.0f, 2.0f, 0.1f, 3.0f};
            const float keep[4] = {clip[0], clip[1], clip[2], clip[3]};
            x4vr::apply_off_axis(o, 0.0f, -1.0f, clip);
            check(clip[0] == keep[0] && clip[1] == keep[1],
                  "sx = 0 (an unpopulated camera block) leaves clip untouched");
        }

        // ---- task #39: the deferred reconstruction, round trip ----
        //
        // A deferred pass reads gl_FragCoord, rebuilds NDC, and reconstructs
        // view position with M_invprojection. Once the affine exists that
        // matrix is wrong by it, and by opposite amounts in the two eyes. The
        // repair is T(d)·M_invprojection·A⁻¹, and the only honest test of a
        // composition is to run the whole path: project, shear, affine,
        // divide -- then reconstruct and demand the ORIGINAL view position
        // back.
        {
            // X4's projection in its own storage, read off a dumped matrix:
            // col0 = (sx,0,0,0), col1 = (0,sy,0,0), col2 = (0,0,0,1),
            // col3 = (0,0,near,0). Reverse-Z with an infinite far plane, so
            // clip = (sx·x, sy·y, near, z).
            auto x4_proj = [](float sx, float sy, float near_z) {
                x4vr::Mat4 p{};
                p.m[0] = sx;
                p.m[5] = sy;
                p.m[11] = 1.0f;
                p.m[14] = near_z;
                return p;
            };
            // column-major m[c*4+r], the convention the patch emits
            auto mv = [](const float *m, const float *v, float *out) {
                for (int r = 0; r < 4; r++)
                    out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] +
                             m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
            };

            // The inverse is the one X4 itself uploads, so take it from the
            // project's own routine rather than hand-deriving it.
            {
                const x4vr::Mat4 p = x4_proj(0.70021f, -0.70021f, 0.1f);
                x4vr::Mat4 pi{};
                check(x4vr::invert(p, pi), "X4's projection is invertible");
                const x4vr::Mat4 id =
                    x4vr::mul(p, pi, x4vr::Major::Column);
                float worst = 0.0f;
                for (int i = 0; i < 16; i++)
                    worst = std::fmax(worst,
                                      std::fabs(id.m[i] - (i % 5 ? 0.0f : 1.0f)));
                check(worst < 1e-5f,
                      "P·P⁻¹ = I under column-major, which pins the convention "
                      "the composition is written in");
                check_near(pi.m[0], 1.0f / 0.70021f, 1e-4f,
                           "M_invprojection[0][0] is 1/sx — where the patch "
                           "reads the live scale from");
                check_near(pi.m[5], 1.0f / -0.70021f, 1e-4f,
                           "M_invprojection[1][1] is 1/sy");
            }

            // The identity case again, one level down: a symmetric target at
            // X4's own half-angle must leave M_invprojection untouched.
            {
                const float t = deg(55.0f), tt = std::tan(t);
                const x4vr::EyeFrustum sym =
                    x4vr::frustum_of_angles(-t, t, t, -t);
                const x4vr::OffAxis o = x4vr::make_off_axis(sym, 0.0f);
                const x4vr::Mat4 p = x4_proj(1.0f / tt, -1.0f / tt, 0.1f);
                x4vr::Mat4 pi{};
                x4vr::invert(p, pi);
                float m[16];
                memcpy(m, pi.m, sizeof(m));
                check(x4vr::compose_inv_off_axis(o, m), "identity case accepted");
                float worst = 0.0f;
                for (int i = 0; i < 16; i++)
                    worst = std::fmax(worst, std::fabs(m[i] - pi.m[i]));
                check(worst < 1e-5f,
                      "a symmetric target at X4's own half-angle leaves "
                      "M_invprojection unchanged");
            }

            // The round trip, over both eyes, five cameras and six points --
            // the same grid the forward direction is tested on, so a failure
            // here is a failure of the composition and not of the coverage.
            const float cam[][2] = {{0.70021f, 1.0f},
                                    {0.75405f, 1.0f},
                                    {1.33333f, 1.0f},
                                    {3.78085f, 1.5f},
                                    {29.18689f, 1.0f}};
            const float world[][3] = {{0, 0, 1},
                                      {0.5f, 0.3f, 2},
                                      {-1.2f, 0.9f, 3},
                                      {4.0f, -2.5f, 1.5f},
                                      {-0.05f, 0.02f, 0.4f},
                                      {60.0f, 40.0f, 250.0f}};
            float worst = 0.0f, worst_uncorrected = 0.0f;
            bool all_accepted = true;
            for (int e = 0; e < 2; e++) {
                const x4vr::OffAxis o = x4vr::make_off_axis(eye[e], d_eye[e]);
                for (const auto &c : cam) {
                    const float sx = c[0], sy = -c[0] * c[1];
                    const x4vr::Mat4 p = x4_proj(sx, sy, 0.1f);
                    x4vr::Mat4 pi{};
                    if (!x4vr::invert(p, pi))
                        continue;
                    for (const auto &pt : world) {
                        // forward: X4's projection, the eye shear, the affine
                        const float v[4] = {pt[0], pt[1], pt[2], 1.0f};
                        float clip[4];
                        mv(p.m, v, clip);
                        clip[0] -= sx * d_eye[e];
                        x4vr::apply_off_axis(o, sx, sy, clip);
                        // what the rasterizer leaves behind for the deferred
                        // pass: NDC and a depth value, w divided out
                        const float frag[4] = {clip[0] / clip[3],
                                               clip[1] / clip[3],
                                               clip[2] / clip[3], 1.0f};

                        // backward, corrected: T(d)·M_invprojection·A⁻¹
                        float m[16];
                        memcpy(m, pi.m, sizeof(m));
                        if (!x4vr::compose_inv_off_axis(o, m))
                            all_accepted = false;
                        x4vr::compose_eye_untranslate(d_eye[e], m);
                        float r[4];
                        mv(m, frag, r);
                        for (int i = 0; i < 3; i++)
                            worst = std::fmax(
                                worst, std::fabs(r[i] / r[3] - v[i]));

                        // the negative control: the same reconstruction the
                        // 244 patched modules do TODAY, with the shear undone
                        // and the affine ignored. If this is not large the
                        // test above is not proving anything.
                        float m0[16];
                        memcpy(m0, pi.m, sizeof(m0));
                        x4vr::compose_eye_untranslate(d_eye[e], m0);
                        float r0[4];
                        mv(m0, frag, r0);
                        for (int i = 0; i < 3; i++)
                            worst_uncorrected = std::fmax(
                                worst_uncorrected,
                                std::fabs(r0[i] / r0[3] - v[i]));
                    }
                }
            }
            check(all_accepted,
                  "every live camera in the grid was accepted, so no case "
                  "passed by being silently skipped");
            check(worst < 2e-3f,
                  "T(d)·M_invprojection·A⁻¹ recovers the centre-frame view "
                  "position, both eyes, 5 cameras, 6 points");
            check(worst_uncorrected > 1.0f,
                  "and the uncorrected matrix does not — the negative control "
                  "the round trip is only meaningful against");
            printf("     worst view-space error %.2e m (uncorrected %.2f m)\n",
                   worst, worst_uncorrected);

            // ---- task #40: screen-locked content, in the declared frame ----
            //
            // Asserted in ANGLES, not in NDC. NDC is what the shader writes,
            // but "the two eyes can fuse this" is a statement about where the
            // content ends up in the world, and only the angle says that. The
            // 30.07 deg failure is invisible in NDC — both eyes write the same
            // number, which is exactly the bug.
            {
                const float half = deg(55.0f); // X4VR_FOV 1.4917
                const float sx_s = 1.0f / std::tan(half);
                const float sy_s = -sx_s;
                // The vertical reading of a target NDC, the inverse of the
                // derivation in x4vr_view.hpp: NDC y points down, so this
                // returns a real elevation with up positive.
                auto elev_of_ndc = [](const x4vr::EyeFrustum &f, float n) {
                    return std::atan(0.5f * ((f.tan_u + f.tan_d) -
                                             n * (f.tan_u - f.tan_d)));
                };

                // Compatibility with #30, which is the check that matters
                // most: with a symmetric target at X4's own half-angle this
                // must be canvas_shift and nothing else, or the change is a
                // regression to a proven feature dressed as a generalisation.
                {
                    const x4vr::EyeFrustum sym =
                        x4vr::frustum_of_angles(-half, half, half, -half);
                    const x4vr::OffAxis id = x4vr::make_off_axis(sym, 0.0f);
                    const x4vr::OffAxis none{}; // never latched at all
                    float worst = 0.0f;
                    for (float z : {0.5f, 2.0f, 10.0f}) {
                        const float s = x4vr::canvas_shift(sx_s, kIpd, z);
                        for (int e = 0; e < 2; e++) {
                            const float want12 = e ? -s : +s;
                            for (const x4vr::OffAxis *o : {&id, &none}) {
                                float k[16];
                                x4vr::make_canvas_k(*o, sx_s, sy_s, d_eye[e], z,
                                                    k);
                                worst = std::fmax(worst, std::fabs(k[0] - 1.0f));
                                worst = std::fmax(worst, std::fabs(k[5] - 1.0f));
                                worst = std::fmax(worst, std::fabs(k[13]));
                                worst = std::fmax(worst,
                                                  std::fabs(k[12] - want12));
                            }
                        }
                    }
                    check(worst < 1e-6f,
                          "with no cant, make_canvas_k IS #30's canvas_shift — "
                          "identity target and never-latched agree, 3 distances");
                    printf("     worst deviation from canvas_shift %.2e\n",
                           worst);
                }

                // Canted, at infinity: the UI must land at the angle X4 drew
                // it at, in BOTH eyes. Same angle in both eyes is zero
                // disparity, which is what "pinned at infinity" means.
                {
                    float worst_same = 0.0f, worst_place = 0.0f;
                    for (float x : {-0.9f, -0.4f, 0.0f, 0.35f, 0.8f}) {
                        float a[2], b[2];
                        for (int e = 0; e < 2; e++) {
                            const x4vr::OffAxis o =
                                x4vr::make_off_axis(eye[e], d_eye[e]);
                            float k[16];
                            x4vr::make_canvas_k(o, sx_s, sy_s, d_eye[e], 0.0f, k);
                            a[e] = angle_of_ndc(eye[e], k[0] * x + k[12]);
                            b[e] = elev_of_ndc(eye[e], k[5] * x + k[13]);
                        }
                        worst_same = std::fmax(worst_same,
                                               std::fabs(deg_of(a[0] - a[1])));
                        worst_same = std::fmax(worst_same,
                                               std::fabs(deg_of(b[0] - b[1])));
                        // ...and at the angle it was drawn at, not merely at
                        // some angle both eyes agree on. A map that collapsed
                        // everything to straight ahead would pass the check
                        // above perfectly.
                        const float want_h = std::atan(x * std::tan(half));
                        const float want_v = std::atan(-x * std::tan(half));
                        worst_place = std::fmax(worst_place,
                                                std::fabs(deg_of(a[0] - want_h)));
                        worst_place = std::fmax(worst_place,
                                                std::fabs(deg_of(b[0] - want_v)));
                    }
                    check(worst_same < 1e-3f,
                          "canvas at infinity: both eyes place screen-locked "
                          "content at the SAME angle, horizontally and "
                          "vertically");
                    check(worst_place < 1e-3f,
                          "...and at the angle X4 drew it at, so the UI keeps "
                          "the size and position it has on a flat screen");
                    printf("     agreement %.2e deg, placement %.2e deg\n",
                           worst_same, worst_place);
                }

                // The negative control, in the units the failure was reported
                // in. Without the map the two eyes write the same NDC, which
                // in a canted declaration is 30.07 deg apart in the world.
                {
                    float worst = 0.0f;
                    for (float x : {-0.9f, 0.0f, 0.8f}) {
                        const float a0 = angle_of_ndc(eye[0], x);
                        const float a1 = angle_of_ndc(eye[1], x);
                        worst = std::fmax(worst, std::fabs(deg_of(a0 - a1)));
                    }
                    check(worst > 25.0f,
                          "untransformed, the same NDC in both eyes is tens of "
                          "degrees apart — take 165b's HUD, and the control "
                          "the two checks above are only meaningful against");
                    printf("     untransformed divergence up to %.2f deg\n",
                           worst);
                }

                // Finite z: the disparity must be the one a point at z metres
                // actually subtends. This is #30's claim — "the UI at z metres
                // and world geometry at z metres produce the same disparity" —
                // re-stated in the canted frame, where it is no longer a
                // constant NDC offset.
                {
                    float worst = 0.0f;
                    for (float z : {0.5f, 2.0f, 10.0f}) {
                        for (float x : {-0.7f, 0.0f, 0.6f}) {
                            for (int e = 0; e < 2; e++) {
                                const x4vr::OffAxis o =
                                    x4vr::make_off_axis(eye[e], d_eye[e]);
                                float k[16];
                                x4vr::make_canvas_k(o, sx_s, sy_s, d_eye[e], z,
                                                    k);
                                const float got =
                                    angle_of_ndc(eye[e], k[0] * x + k[12]);
                                // the ray from THIS eye to the canvas point
                                const float want = std::atan(
                                    x * std::tan(half) - d_eye[e] / z);
                                worst = std::fmax(worst,
                                                  std::fabs(deg_of(got - want)));
                            }
                        }
                    }
                    check(worst < 1e-3f,
                          "canvas at z metres: each eye sees it along the ray "
                          "that eye would really have to it, 3 distances");
                    printf("     worst canvas placement error %.2e deg\n",
                           worst);
                }
            }

            // ---- refusals ----
            {
                const x4vr::OffAxis o = x4vr::make_off_axis(eye[0], d_eye[0]);
                float zero[16] = {};
                float keep[16] = {};
                check(!x4vr::compose_inv_off_axis(o, zero),
                      "an all-zero camera block refuses rather than dividing");
                check(memcmp(zero, keep, sizeof(zero)) == 0,
                      "and leaves the matrix exactly as it found it");
                float m[16] = {};
                m[0] = 1.0f;
                m[5] = 1.0f;
                const x4vr::OffAxis bad{};
                check(!x4vr::compose_inv_off_axis(bad, m),
                      "a refused frustum composes nothing");
            }
        }
    }

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
