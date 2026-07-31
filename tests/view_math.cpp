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

    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
