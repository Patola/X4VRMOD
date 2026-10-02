// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The head-rotation budget, checked against an implementation that did not
// come from the same keyboard session.
//
// `tools/guard_band.py` prices the FIELD for a given budget; this header
// inverts it and answers the budget for a given field. Both walk the same
// boundary rays, so a bug in the shared idea would survive both -- but the
// numbers below are the Python's PUBLISHED output, produced before this header
// existed, and a port that drifts from them fails here.
//
// A solver agreeing with itself proves nothing, which is the whole reason
// these rows are pasted rather than computed.
#include "../common/x4vr_guard.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what, const char *detail = "") {
    printf("%-5s %-46s %s\n", ok ? "ok" : "FAIL", what, detail);
    if (!ok)
        failures++;
}

// Take 112's xr-probe, Quest 3 over WiVRn -- and confirmed live: every stored
// log carrying `vr fov: runtime reports` has exactly these numbers.
const float kDeg = 0.01745329251994330f;
const x4vr::EyeFov kEyes[2] = {
    {-54.0f * kDeg, 40.0f * kDeg, 44.0f * kDeg, -55.0f * kDeg},
    {-40.0f * kDeg, 54.0f * kDeg, 44.0f * kDeg, -55.0f * kDeg},
};

// guard_band.py, `--max-deg 19 --step 1`. Budget -> the X4VR_FOV it requires.
// Reproduced verbatim from the tool's output.
struct Row { float budget_deg, x4vr_fov; };
const Row kPublished[] = {
    {0.0f, 1.4917f},  {1.0f, 1.5366f},  {2.0f, 1.5821f},  {3.0f, 1.6280f},
    {4.0f, 1.6744f},  {5.0f, 1.7214f},  {6.0f, 1.7689f},  {7.0f, 1.8170f},
    {8.0f, 1.8656f},  {9.0f, 1.9147f},  {10.0f, 1.9644f}, {11.0f, 2.0153f},
    {12.0f, 2.0684f}, {13.0f, 2.1216f}, {14.0f, 2.1748f}, {15.0f, 2.2278f},
};

} // namespace

int main() {
    printf("guard budget: inverted against tools/guard_band.py\n\n");

    // Each published row says "this field is what that budget needs". So the
    // budget this header finds for that field must be at least that much --
    // and must NOT be wildly more, or the header is buying range the Python
    // says costs extra.
    float worst = 0.0f;
    for (const Row &r : kPublished) {
        const float ht = x4vr::guard::half_tan_of_x4vr_fov(r.x4vr_fov);
        const float got = x4vr::guard::max_budget_deg(kEyes, 2, ht);
        const float err = std::fabs(got - r.budget_deg);
        if (err > worst)
            worst = err;
        if (err > 0.35f) {
            char buf[128];
            snprintf(buf, sizeof(buf), "fov %.4f: want %.1f deg, got %.2f",
                     r.x4vr_fov, r.budget_deg, got);
            check(false, "budget matches the published curve", buf);
        }
    }
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "16 rows, worst error %.3f deg", worst);
        check(worst <= 0.35f, "budget matches the published curve", buf);
    }

    // The shipped knob. Every take since 114d ran X4VR_FOV=1.4917, and the
    // Python's self-check says that is the ZERO-budget field -- so the layer
    // must not believe it has room to rotate at the setting it ships with.
    {
        const float ht = x4vr::guard::half_tan_of_x4vr_fov(1.4917f);
        const float got = x4vr::guard::max_budget_deg(kEyes, 2, ht);
        char buf[96];
        snprintf(buf, sizeof(buf), "%.3f deg at the shipped 1.4917", got);
        check(got < 0.35f, "the shipped field has no rotation budget", buf);
    }

    // Monotone: a wider field can never buy LESS range. If this fails the
    // bisection is riding a non-monotone predicate and every number above is
    // luck.
    {
        float prev = -1.0f;
        bool mono = true;
        for (float fov = 1.45f; fov <= 2.30f; fov += 0.05f) {
            const float got = x4vr::guard::max_budget_deg(
                kEyes, 2, x4vr::guard::half_tan_of_x4vr_fov(fov));
            if (got < prev - 1e-3f)
                mono = false;
            prev = got;
        }
        check(mono, "budget is monotone in the field");
    }

    // The wall, at the resolution it actually sits at. `--step 1` prints 19.0
    // as the first unreachable row, but that is the GRID: at --step 0.1 the
    // Python separates two regimes, and only the second is a wall.
    //
    //   19.0, 19.1  "required FOV reaches 180 deg"  -- needs a field nobody has
    //   19.2+       "a ray reaches 90 deg"          -- no planar render, ever
    //
    // So at an absurd near-180 field (2.44 * 73.7399 = 179.9 deg) 19.1 DOES
    // fit and 19.2 cannot, and this header must draw the line in the same
    // place. The first version of this case asserted < 19.0 and failed on a
    // correct answer -- the threshold was read off the coarse grid.
    {
        const float ht = x4vr::guard::half_tan_of_x4vr_fov(2.44f);
        const float got = x4vr::guard::max_budget_deg(kEyes, 2, ht, 90.0f);
        char buf[112];
        snprintf(buf, sizeof(buf), "%.2f deg at fov 2.44 (179.9 deg field)",
                 got);
        check(got >= 19.0f && got < 19.2f,
              "the wall sits between 19.1 and 19.2 deg", buf);
    }

    // A negative control for `covered` itself. The predicate must be able to
    // say NO -- one that cannot is not a guard. 60 deg of yaw is far past the
    // wall at any field.
    {
        const bool c = x4vr::guard::covered(
            kEyes[0], x4vr::guard::half_tan_of_x4vr_fov(2.0f), 60.0f * kDeg,
            0.0f);
        check(!c, "covered() refuses a 60 deg box");
    }
    // ...and must be able to say YES, or the NO above is worthless.
    {
        const bool c = x4vr::guard::covered(
            kEyes[0], x4vr::guard::half_tan_of_x4vr_fov(2.0f), 1.0f * kDeg,
            1.0f * kDeg);
        check(c, "covered() accepts a 1 deg box at fov 2.0");
    }

    // ---- the residual itself, against the angles actually measured ---------
    //
    // These are not invented numbers: median 0.22 and p90 2.27 deg are what
    // 2398 live `camread` samples give below X4's clamp, and 71 is head 136
    // against a camera stopped at 65.
    {
        using x4vr::guard::residual_of;
        // The ordinary case must pass through untouched at a 2.3 deg budget.
        auto med = residual_of(20.22f, 5.10f, 20.0f, 5.0f, true, 2.3f);
        check(std::fabs(med.yaw - 0.22f) < 1e-4f && !med.clamped,
              "the median 0.22 deg residual passes unclamped");

        // The p90 does NOT, at the budget the shipped-plus-a-bit field buys.
        auto p90 = residual_of(22.27f, 5.0f, 20.0f, 5.0f, true, 2.0f);
        check(std::fabs(p90.yaw - 2.0f) < 1e-4f && p90.clamped,
              "the p90 2.27 deg residual is clamped at a 2.0 budget");

        // Past X4's clamp the head runs away and the budget must hold.
        auto past = residual_of(136.0f, 0.0f, 65.0f, 0.0f, true, 2.0f);
        check(std::fabs(past.yaw - 2.0f) < 1e-4f && past.clamped,
              "71 deg past the clamp still applies only the budget");

        // Sign: the head LEADS the camera, so the residual is positive.
        check(residual_of(21.0f, 0.0f, 20.0f, 0.0f, true, 5.0f).yaw > 0.0f &&
                  residual_of(19.0f, 0.0f, 20.0f, 0.0f, true, 5.0f).yaw < 0.0f,
              "the residual carries the sign of head minus command");

        // No command: the whole head angle is the residual.
        auto nocmd = residual_of(1.5f, -0.5f, 99.0f, 99.0f, false, 5.0f);
        check(std::fabs(nocmd.yaw - 1.5f) < 1e-4f &&
                  std::fabs(nocmd.pitch + 0.5f) < 1e-4f,
              "with no valid command the head angle IS the residual");

        // Across the back (take 318: X4 wraps at +-180 with the extension).
        // Head -179 against command +179 is +2 deg the short way; unwrapped
        // it was -358 and clamped to -budget, the wrong direction.
        auto back = residual_of(-179.0f, 0.0f, 179.0f, 0.0f, true, 5.0f);
        check(std::fabs(back.yaw - 2.0f) < 1e-3f && !back.clamped,
              "crossing +-180 the residual takes the short way (+2, not -358)");
        auto back2 = residual_of(179.0f, 0.0f, -179.0f, 0.0f, true, 5.0f);
        check(std::fabs(back2.yaw + 2.0f) < 1e-3f && !back2.clamped,
              "and the other way round (-2, not +358)");

        // And the zero-budget case, which is what the SHIPPED field gives:
        // nothing is applied, so nothing may be declared either.
        auto zero = residual_of(30.0f, 20.0f, 0.0f, 0.0f, true, 0.0f);
        check(zero.yaw == 0.0f && zero.pitch == 0.0f && !zero.clamped,
              "a zero budget applies nothing at all");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all cases passed");
    return failures ? 1 : 0;
}
