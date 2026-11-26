// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr::billboard::face_eye (after take 469): a card X4 turned with its camera, by any
// turn of its own K (facing the view, rolled, flipped, tilted off the view axis), must
// come out as X4 draws it when its camera looks straight at the card with the ship's up
// -- from every head-look, ship and card position -- with WVP = VP * W'. Controls: the
// card as X4 gave it moves with the head-look (so the check can fail), and ship_up is
// column 1 of the look rows the shader patches read. Self-checking.
//
// And with the WORLD's up (after take 483, X4VR_FOG_CARDS=fix: the cards turned by
// positions alone): the card must come out the same from every head-look AND every ship
// attitude -- the frame whose z is the line from the eye to the card and whose up is the
// world's +y, times X4's own K.
#include "../common/x4vr_billboard.hpp"
#include "../common/x4vr_view.hpp"

#include <array>
#include <cmath>
#include <cstdio>

using M3 = std::array<double, 9>; // column-major
static M3 rot(double yaw, double pitch, double roll) {
    const double k = M_PI / 180.0, cy = cos(yaw * k), sy = sin(yaw * k), cx = cos(pitch * k),
                 sx = sin(pitch * k), cz = cos(roll * k), sz = sin(roll * k);
    return M3{cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz,
              -cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz,
              sy * cx, -sx, cy * cx};
}
static M3 mul(const M3 &a, const M3 &b) {
    M3 o{};
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++)
                o[c * 3 + r] += a[k * 3 + r] * b[c * 3 + k];
    return o;
}
static M3 tr(const M3 &a) {
    M3 o{};
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            o[c * 3 + r] = a[r * 3 + c];
    return o;
}
static void m4(const M3 &a, const double t[3], float o[16]) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            o[c * 4 + r] = (float)((c < 3 && r < 3) ? a[c * 3 + r]
                                   : (c == 3 && r < 3) ? (t ? t[r] : 0.0)
                                   : (c == 3 && r == 3) ? 1.0
                                                        : 0.0);
}
static void mul4(const float *a, const float *b, float *o) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++)
                s += (double)a[k * 4 + r] * b[c * 4 + k];
            o[c * 4 + r] = (float)s;
        }
}

int main() {
    int checked = 0, bad = 0, ctrl_seen = 0, ctrl_runs = 0, refused = 0, up_bad = 0,
        ship_checked = 0, faces_bad = 0, world_checked = 0, world_bad = 0;
    double world_worst = 0.0;
    double worst = 0.0, worst_wvp = 0.0, ship_worst = 0.0;
    // X4's projection: row3 = [0 0 1 0], reversed-Z near 0.1.
    float P[16] = {};
    P[0] = 0.7f, P[5] = -0.7f, P[2 * 4 + 3] = 1.f, P[3 * 4 + 2] = 0.1f;
    const double ships[][3] = {{0, 0, 0}, {37, -12, 8}, {-140, 60, -30}};
    const double looks[][2] = {{0, 0}, {30, 0}, {-45, 20}, {120, -50}, {-10, 74}, {-66, -44}};
    // K: facing the view; rolled; flipped and rolled (take 468); tilted 40 deg about the
    // view's up (take 468's "otherwise"); tilted every way (take 469's 43 km sheets).
    const double ks[][3] = {{0, 0, 0}, {0, 0, 37}, {180, 0, 1.4}, {40, 0, 0}, {-65, 24, 110}};
    const double scales[][3] = {{336, 336, 1}, {24500, 24500, 1}, {3.9, 3.9, 3.9}};
    const double places[][3] = {{120, -40, 900}, {-2500, 300, -1800}, {30000, 12000, 30000}};
    for (const auto &sh : ships)
        for (const auto &kk : ks)
            for (const auto &sc : scales)
                for (const auto &t : places) {
                    const M3 body = rot(sh[0], sh[1], sh[2]);
                    const M3 K = rot(kk[0], kk[1], kk[2]);
                    const M3 S{sc[0], 0, 0, 0, sc[1], 0, 0, 0, sc[2]};
                    // What X4 draws with its camera looking straight at the card, the
                    // ship's up its up: forward the card's direction (the eye at 0).
                    const double tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                    const double zf[3] = {t[0] / tl, t[1] / tl, t[2] / tl};
                    const double upw[3] = {body[3], body[4], body[5]};
                    double xf[3] = {upw[1] * zf[2] - upw[2] * zf[1], upw[2] * zf[0] - upw[0] * zf[2],
                                    upw[0] * zf[1] - upw[1] * zf[0]};
                    const double xl = sqrt(xf[0] * xf[0] + xf[1] * xf[1] + xf[2] * xf[2]);
                    for (double &c : xf)
                        c /= xl;
                    const double yf[3] = {zf[1] * xf[2] - zf[2] * xf[1], zf[2] * xf[0] - zf[0] * xf[2],
                                          zf[0] * xf[1] - zf[1] * xf[0]};
                    const M3 cam_s{xf[0], xf[1], xf[2], yf[0], yf[1], yf[2], zf[0], zf[1], zf[2]};
                    const M3 want = mul(cam_s, mul(K, S));
                    // The same frame with the world's up.
                    double xw[3] = {zf[2], 0.0, -zf[0]}; // (0,1,0) x zf
                    const double xwl = sqrt(xw[0] * xw[0] + xw[2] * xw[2]);
                    for (double &c : xw)
                        c /= xwl;
                    const double yw[3] = {zf[1] * xw[2] - zf[2] * xw[1], zf[2] * xw[0] - zf[0] * xw[2],
                                          zf[0] * xw[1] - zf[1] * xw[0]};
                    const M3 cam_w{xw[0], xw[1], xw[2], yw[0], yw[1], yw[2], zf[0], zf[1], zf[2]};
                    const M3 want_w = mul(cam_w, mul(K, S));
                    float first_w[16] = {};
                    bool have_first = false;
                    for (const auto &lk : looks) {
                        x4vr::PoseBlock pb = x4vr::pose_identity();
                        x4vr::pose_set_look(pb, (float)lk[0], (float)lk[1], true);
                        M3 rows{};
                        for (int r = 0; r < 3; r++)
                            for (int c = 0; c < 3; c++)
                                rows[c * 3 + r] = pb.look[c * 4 + r];
                        float up[3];
                        x4vr::billboard::ship_up((float)lk[0], (float)lk[1], up);
                        for (int k = 0; k < 3; k++)
                            if (fabs(up[k] - rows[3 + k]) > 1e-6)
                                up_bad++;
                        const M3 cam = mul(body, tr(rows)); // the head's camera
                        float V[16], VP[16], W[16], WVP[16];
                        m4(tr(cam), nullptr, V); // camera-relative: no translation
                        mul4(P, V, VP);
                        m4(mul(cam, mul(K, S)), t, W); // the card, turned with the camera
                        mul4(VP, W, WVP);
                        float W0[16];
                        for (int i = 0; i < 16; i++)
                            W0[i] = W[i];
                        // Turned to the ship: X4's own card with the head straight, the
                        // camera the ship's, from every head-look.
                        {
                            float Lr[9], Ws[16], WVPs[16];
                            x4vr::billboard::look_rows((float)lk[0], (float)lk[1], Lr);
                            for (int i = 0; i < 9; i++)
                                if (fabs(Lr[i] - rows[i]) > 1e-6)
                                    up_bad++;
                            for (int i = 0; i < 16; i++) {
                                Ws[i] = W[i];
                                WVPs[i] = WVP[i];
                            }
                            if (!x4vr::billboard::face_ship(V, VP, Ws, WVPs, Lr)) {
                                refused++;
                            } else {
                                const M3 ws = mul(body, mul(K, S));
                                double es = 0.0, ms = 0.0;
                                for (int c = 0; c < 3; c++)
                                    for (int r = 0; r < 3; r++) {
                                        es = std::max(es, fabs(Ws[c * 4 + r] - ws[c * 3 + r]));
                                        ms = std::max(ms, fabs(ws[c * 3 + r]));
                                    }
                                for (int r = 0; r < 4; r++)
                                    es = std::max(es, (double)fabs(Ws[12 + r] - W[12 + r]));
                                ship_worst = std::max(ship_worst, es / ms);
                                ship_checked++;
                                if (es / ms > 2e-5)
                                    bad++;
                            }
                            // faces_view: X4's true billboards (K facing, rolled, flipped)
                            // and not the tilted ones.
                            const bool facing = kk[0] == 0 && kk[1] == 0 ? true
                                                : (kk[0] == 180 && kk[1] == 0);
                            if (x4vr::billboard::faces_view(V, W) != facing)
                                faces_bad++;
                        }
                        // With the world's up: V's column 1 is the world's +y in the view's axes.
                        {
                            float Ww[16], WVPw[16];
                            for (int i = 0; i < 16; i++) {
                                Ww[i] = W[i];
                                WVPw[i] = WVP[i];
                            }
                            const float upv[3] = {V[4], V[5], V[6]};
                            if (!x4vr::billboard::face_eye(V, VP, Ww, WVPw, upv)) {
                                refused++;
                            } else {
                                double e = 0.0, m = 0.0;
                                for (int c = 0; c < 3; c++)
                                    for (int r = 0; r < 3; r++) {
                                        e = std::max(e, fabs(Ww[c * 4 + r] - want_w[c * 3 + r]));
                                        m = std::max(m, fabs(want_w[c * 3 + r]));
                                    }
                                world_worst = std::max(world_worst, e / m);
                                world_checked++;
                                if (e / m > 2e-5)
                                    world_bad++;
                            }
                        }
                        if (!x4vr::billboard::face_eye(V, VP, W, WVP, up)) {
                            refused++;
                            continue;
                        }
                        double err = 0.0, mag = 0.0;
                        for (int c = 0; c < 3; c++)
                            for (int r = 0; r < 3; r++) {
                                err = std::max(err, fabs(W[c * 4 + r] - want[c * 3 + r]));
                                mag = std::max(mag, fabs(want[c * 3 + r]));
                            }
                        for (int r = 0; r < 4; r++)
                            err = std::max(err, (double)fabs(W[12 + r] - W0[12 + r]) / mag);
                        err /= mag;
                        float chk[16];
                        mul4(VP, W, chk);
                        double ew = 0.0, mw = 0.0;
                        for (int i = 0; i < 16; i++) {
                            ew = std::max(ew, (double)fabs(chk[i] - WVP[i]));
                            mw = std::max(mw, (double)fabs(chk[i]));
                        }
                        worst_wvp = std::max(worst_wvp, ew / mw);
                        worst = std::max(worst, err);
                        checked++;
                        if (err > 2e-5 || ew / mw > 1e-5)
                            bad++;
                        // Control: X4's own card moves with the head-look.
                        if (!have_first) {
                            for (int i = 0; i < 16; i++)
                                first_w[i] = W0[i];
                            have_first = true;
                        } else {
                            ctrl_runs++;
                            double d = 0.0;
                            for (int i = 0; i < 12; i++)
                                d = std::max(d, (double)fabs(W0[i] - first_w[i]));
                            if (d > 1e-3 * mag)
                                ctrl_seen++;
                        }
                    }
                }
    printf("CHECKED=%d BAD=%d WORST=%.2e WVP_WORST=%.2e REFUSED=%d UP_BAD=%d CONTROL_CAUGHT=%d/%d\n",
           checked, bad, worst, worst_wvp, refused, up_bad, ctrl_seen, ctrl_runs);
    printf("SHIP_CHECKED=%d SHIP_WORST=%.2e FACES_VIEW_WRONG=%d\n", ship_checked, ship_worst,
           faces_bad);
    printf("WORLD_UP_CHECKED=%d WORLD_UP_BAD=%d WORLD_UP_WORST=%.2e\n", world_checked, world_bad,
           world_worst);
    // The smoke's knobs survive the per-frame rotation write (take 485: pose_set_euler
    // cleared them, so a run without the A/B had no smoke); the identity is the smoke as
    // built.
    int knobs_bad = 0;
    {
        x4vr::PoseBlock pb = x4vr::pose_identity();
        for (int k = 0; k < 4; k++)
            if (pb.fog[k] != 1.0f || pb.fog2[k] != 0.0f)
                knobs_bad++;
        const float f[4] = {0.f, 0.7f, 1.f, 0.6667f}, f2[4] = {600.f, 0.f, 0.f, 0.f};
        for (int k = 0; k < 4; k++) {
            pb.fog[k] = f[k];
            pb.fog2[k] = f2[k];
        }
        x4vr::pose_set_euler(pb, 12.f, -7.f, 3.f);
        for (int k = 0; k < 4; k++)
            if (pb.fog[k] != f[k] || pb.fog2[k] != f2[k])
                knobs_bad++;
    }
    printf("SMOKE_KNOBS_BAD=%d\n", knobs_bad);
    const bool pass = bad == 0 && refused == 0 && up_bad == 0 && faces_bad == 0 &&
                      world_bad == 0 && world_checked > 0 && knobs_bad == 0 &&
                      ship_checked > 0 && ctrl_runs > 0 && ctrl_seen == ctrl_runs && checked > 0;
    printf(pass ? "all passed\n" : "FAIL\n");
    return pass ? 0 : 1;
}
