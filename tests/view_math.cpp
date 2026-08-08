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

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
