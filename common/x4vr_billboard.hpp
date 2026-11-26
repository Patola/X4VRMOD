// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#pragma once
// X4's billboards, turned to the eye on the CPU (after take 469).
//
// X4 turns its fog cards (`billboard_view`) with its camera: each card's world matrix is
// the camera's turn times a fixed one of its own, K, so `Q = mat3(M_view) * mat3(M_world)`
// stays K while the camera turns. Head-look turns X4's camera, so in a headset every
// card swivels with the head. Take 469's census: 88% of the draws of X4's near/far-faded
// effects kept their Q over 120 deg of head-look; about a third of them with K tilted
// off the view axis, which no single frame can tell from a card fixed in the world.
// So the layer learns them from their Q staying put while the camera turns, and turns
// them here, in X4's own constants, before the GPU reads them.
//
// Turned to the eye: Q' = F * Q, F the frame whose z is the line from the eye to the
// card's centre and whose up is the ship's -- what X4 draws when its camera looks
// straight at the card, and the same in the world wherever the head points.

#include <cmath>
#include <cstdint>

namespace x4vr::billboard {

// Column-major 4x4s, as X4's blocks hold them. W, WVP: the draw's object block (members
// 1 and 0); V, VP: its camera block (members 0 and 7). up: the ship's up in the view's
// axes. Rewrites W's turn (its translation kept) and WVP = VP * W'. False, and nothing
// written, where the view's turn is not a rotation or the card sits on the up line.
inline bool face_eye(const float V[16], const float VP[16], float W[16], float WVP[16],
                     const float up[3]) {
    double v[3][3], w[3][3]; // [col][row]
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            v[c][r] = V[c * 4 + r];
            w[c][r] = W[c * 4 + r];
        }
    // V's turn must be a rotation: its transpose is then its inverse.
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) {
            double d = 0.0;
            for (int k = 0; k < 3; k++)
                d += v[k][a] * v[k][b]; // row a . row b
            if (std::fabs(d - (a == b ? 1.0 : 0.0)) > 1e-3)
                return false;
        }
    // The card's centre in the view: V * (W's translation, 1).
    double p[3];
    for (int r = 0; r < 3; r++) {
        p[r] = V[12 + r];
        for (int k = 0; k < 3; k++)
            p[r] += v[k][r] * (double)W[12 + k];
    }
    const double pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (!(pl > 0.0))
        return false;
    const double n[3] = {p[0] / pl, p[1] / pl, p[2] / pl};
    double x[3] = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2],
                   up[0] * n[1] - up[1] * n[0]};
    const double xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (!(xl > 1e-3))
        return false;
    for (double &c : x)
        c /= xl;
    const double y[3] = {n[1] * x[2] - n[2] * x[1], n[2] * x[0] - n[0] * x[2],
                         n[0] * x[1] - n[1] * x[0]};
    const double *f[3] = {x, y, n}; // F's columns
    // Q = V W (3x3), Q' = F Q, W' = V^T Q'.
    double q[3][3], q2[3][3], w2[3][3];
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += v[k][r] * w[c][k];
            q[c][r] = acc;
        }
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += f[k][r] * q[c][k];
            q2[c][r] = acc;
        }
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += v[r][k] * q2[c][k]; // V^T: row r of V^T is column r of V
            w2[c][r] = acc;
        }
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            W[c * 4 + r] = (float)w2[c][r];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            double acc = 0.0;
            for (int k = 0; k < 4; k++)
                acc += (double)VP[k * 4 + r] * W[c * 4 + k];
            WVP[c * 4 + r] = (float)acc;
        }
    return true;
}

// Turned to the ship's forward: Q' = L * Q, L the head-look undone in the view's axes
// (column-major 3x3, PoseBlock::look's rows): what X4 draws with the head straight, so
// the cards keep X4's own layout as seen along the ship's nose, held still while the
// head looks around (after take 471: turned to the eye, the cards X4 tilts off the view
// axis -- a third of them -- left their layout, and some went edge-on and faded).
// Rewrites W's turn (its translation kept) and WVP = VP * W'. False where the view's
// turn is not a rotation.
inline bool face_ship(const float V[16], const float VP[16], float W[16], float WVP[16],
                      const float L[9]) {
    double v[3][3], w[3][3];
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            v[c][r] = V[c * 4 + r];
            w[c][r] = W[c * 4 + r];
        }
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) {
            double d = 0.0;
            for (int k = 0; k < 3; k++)
                d += v[k][a] * v[k][b];
            if (std::fabs(d - (a == b ? 1.0 : 0.0)) > 1e-3)
                return false;
        }
    double q[3][3], q2[3][3];
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += v[k][r] * w[c][k];
            q[c][r] = acc;
        }
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += (double)L[k * 3 + r] * q[c][k];
            q2[c][r] = acc;
        }
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += v[r][k] * q2[c][k];
            W[c * 4 + r] = (float)acc;
        }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            double acc = 0.0;
            for (int k = 0; k < 4; k++)
                acc += (double)VP[k * 4 + r] * W[c * 4 + k];
            WVP[c * 4 + r] = (float)acc;
        }
    return true;
}

// Whether a card faces the view up to a roll: its normal (Q's column 2) on the view
// axis, its own x and y in the view plane -- X4's true billboards, as opposed to the
// cards it tilts off the view axis (V, W column-major 4x4s).
inline bool faces_view(const float V[16], const float W[16]) {
    double q[3][3], n[3];
    for (int c = 0; c < 3; c++) {
        double l = 0.0;
        for (int r = 0; r < 3; r++) {
            double acc = 0.0;
            for (int k = 0; k < 3; k++)
                acc += (double)V[k * 4 + r] * W[c * 4 + k];
            q[c][r] = acc;
            l += acc * acc;
        }
        n[c] = std::sqrt(l);
        if (!(n[c] > 0.0))
            return false;
    }
    return std::fabs(q[2][0]) < 1e-3 * n[2] && std::fabs(q[2][1]) < 1e-3 * n[2] &&
           std::fabs(q[0][2]) < 1e-3 * n[0] && std::fabs(q[1][2]) < 1e-3 * n[1];
}

// The head-look undone in the view's axes, for (yaw, pitch) in the injector's convention
// (column-major 3x3; x4vr_view.hpp pose_set_look's rows, pinned to x4vr_camwrite).
inline void look_rows(float yaw_deg, float pitch_deg, float L[9]) {
    const float d = 0.01745329251994330f;
    const float psi = -yaw_deg * d, th = pitch_deg * d;
    const float cy = std::cos(psi), sy = std::sin(psi), cp = std::cos(th), sp = std::sin(th);
    const float rows[3][3] = {{cy, 0.f, -sy}, {-sy * sp, cp, -cy * sp}, {sy * cp, sp, cy * cp}};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            L[c * 3 + r] = rows[r][c];
}

// The ship's up in the view's axes, for a head-look of (yaw, pitch) in the injector's
// convention: column 1 of the look rows (x4vr_view.hpp pose_set_look, pinned there to
// x4vr_camwrite's rows_for).
inline void ship_up(float yaw_deg, float pitch_deg, float up[3]) {
    // rows {cy,0,-sy}, {-sy*sp,cp,-cy*sp}, {sy*cp,sp,cy*cp}: column 1. The yaw turns
    // about the ship's up, so only the pitch moves it.
    (void)yaw_deg;
    const float th = pitch_deg * 0.01745329251994330f;
    up[0] = 0.f;
    up[1] = std::cos(th);
    up[2] = std::sin(th);
}

// How far apart two view turns are, in degrees (column-major 4x4s, their 3x3s).
inline double turn_between_deg(const float A[16], const float B[16]) {
    double tr = 0.0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++)
            tr += (double)A[k * 4 + i] * B[k * 4 + i]; // trace(A B^T)
    const double c = std::fmax(-1.0, std::fmin(1.0, (tr - 1.0) / 2.0));
    return std::acos(c) * 57.29577951308232;
}

} // namespace x4vr::billboard
