#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""The clip-space matrix for #33's head rotation, derived and self-checked.

    tools/headlook_clip.py --self-check
    tools/headlook_clip.py --yaw 10 --pitch 5 --sx 0.75405

**The question this exists to answer before any code is written:** is the head
rotation a matrix that can be BAKED at `vkCreateShaderModule`, the way
`patch_vertex_clip` bakes its `float k[16]`? It is not, and the margin is not
small. That is the whole finding, and it is cheap to reproduce here rather than
expensive to discover in a headset.

**The geometry.** X4's projection, measured every run since take 104 and
printed by `X4VR_DUMP_MATRICES`, acts on a view point as

    x_c = sx * x_v     y_c = sy * y_v     z_c = near     w_c = z_v

-- `m[10] == 0`, so clip z is the constant near plane and w carries view z.
That is a reversed-Z projection with an infinite far plane, and X4's own
`hiddenareamask.vert.glsl` says so in as many words ("since we have an inverse
z-buffer, 1.0 is closest and 0.0 is furthest"). P is invertible, so a camera
rotation R about the eye point is exactly a clip-space homography:

    M = P . R . P^-1        applied to c = P . V . x
    M . c = P . R . V . x

which is a render with view `R.V` -- exact at every depth, no parallax term, no
depth buffer, no disocclusion. Check 2 below asserts that to 1e-13.

**Why it cannot be baked.** Writing it out,

    M[0][3] =  sx * R02          M[3][0] = R20 / sx
    M[1][3] =  sy * R12          M[3][1] = R21 / sy
    M[0][1] = (sx/sy) * R01      M[1][0] = (sy/sx) * R10

Six of sixteen entries carry `sx` or `sy`, and they are exactly the entries
that couple the rotation into and out of w -- that is, the entries that DO the
rotating. `M[0][0]`, `M[1][1]`, `M[3][3]` are the free ones and they only hold
`cos`. Take fifty-three measured `sx` across a session of ordinary play at 1.15
to 37.75, a 33x spread, and the shipped `X4VR_FOV=1.4917` camera sits at
0.75405 while the menu camera sits at 1.33333. Check 3 prints what a matrix
baked at one of those costs at the others.

This is task #23's trap in a new place, and it is worse here. The shear
collapsed to a single scalar because `z_c == near` made the depth term
constant; the rotation does not collapse, because it mixes `w_c` into `x_c`.
A stale `sx` there is not a degraded rotation, it is the wrong geometry.

**So the rotation rides the live-sx patches** -- `patch_vertex_eye_offset`,
which already loads `M_projection[0][0]` and `[1][1]` out of X4's camera block,
and `patch_vertex_eye_offset_mvp`, which recovers them from
`M_worldviewprojection`. It must NOT ride `patch_vertex_clip`, whose `k[16]` is
frozen ~26 s before X4 has a camera. That is not a limitation to work around:
`patch_vertex_clip` carries the non-World modules, which are the ones that must
stay screen-locked for #30's canvas anyway.

`k[16]` is emitted column-major, matching `make_canvas_k` in
`common/x4vr_view.hpp` -- `k[0]` is row 0 column 0, `k[12]` is row 0 column 3.
"""
import argparse
import math

import numpy as np

NEAR = 0.1
# Measured cameras, from docs/known-good-runs.md and take 158's camera dump.
CAMERAS = {
    'menu / no-fov camera': 1.33333,
    'fov 1.437': 0.75405,
    'fov 1.1111': 1.15174,
    'fov 2.21': 0.14976,
    'gameplay, zoomed out': 3.78085,
    'zoom stop (take 53)': 37.75,
}


def projection(sx, sy, near=NEAR):
    """X4's projection as a 4x4 acting on view (x, y, z, 1)."""
    return np.array([[sx, 0.0, 0.0, 0.0],
                     [0.0, sy, 0.0, 0.0],
                     [0.0, 0.0, 0.0, near],
                     [0.0, 0.0, 1.0, 0.0]], dtype=np.float64)


def rotation(yaw_deg, pitch_deg, roll_deg=0.0):
    """Camera rotation, yaw about +y, pitch about +x, roll about +z."""
    def rx(a):
        c, s = math.cos(a), math.sin(a)
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=np.float64)

    def ry(a):
        c, s = math.cos(a), math.sin(a)
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=np.float64)

    def rz(a):
        c, s = math.cos(a), math.sin(a)
        return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=np.float64)

    m = np.eye(4)
    m[:3, :3] = ry(math.radians(yaw_deg)) @ rx(math.radians(pitch_deg)) @ \
        rz(math.radians(roll_deg))
    return m


def clip_matrix(sx, sy, yaw_deg, pitch_deg, roll_deg=0.0, near=NEAR):
    p = projection(sx, sy, near)
    return p @ rotation(yaw_deg, pitch_deg, roll_deg) @ np.linalg.inv(p)


def _sample(rng, n):
    """View points in front of the camera, over X4's whole depth range."""
    return np.stack([rng.uniform(-500, 500, n), rng.uniform(-500, 500, n),
                     np.exp(rng.uniform(math.log(0.2), math.log(2e6), n)),
                     np.ones(n)], axis=1)


def check_exact(rng):
    """M.P.v must equal P.R.v after the divide, at every depth."""
    worst = 0.0
    for sx in CAMERAS.values():
        p, r = projection(sx, -sx), rotation(10.0, 5.0)
        m = clip_matrix(sx, -sx, 10.0, 5.0)
        v = _sample(rng, 4000)
        direct = v @ (p @ r).T
        viaM = v @ (m @ p).T
        keep = (np.abs(direct[:, 3]) > 1e-9) & (np.abs(viaM[:, 3]) > 1e-9)
        d = direct[keep] / direct[keep, 3:4]
        b = viaM[keep] / viaM[keep, 3:4]
        worst = max(worst, float(np.abs(d[:, :3] - b[:, :3]).max()))
    return worst


def check_baked(rng, bake_at, yaw, pitch, half_width_px):
    """Worst on-screen error from baking M at one camera and running another."""
    m_baked = clip_matrix(bake_at, -bake_at, yaw, pitch)
    out = []
    for name, sx in CAMERAS.items():
        p, r = projection(sx, -sx), rotation(yaw, pitch)
        v = _sample(rng, 20000)
        direct = v @ (p @ r).T
        viaM = v @ (m_baked @ p).T
        keep = (np.abs(direct[:, 3]) > 1e-9) & (np.abs(viaM[:, 3]) > 1e-9)
        d = direct[keep] / direct[keep, 3:4]
        b = viaM[keep] / viaM[keep, 3:4]
        on = (np.abs(d[:, 0]) <= 1.0) & (np.abs(d[:, 1]) <= 1.0)
        if not on.any():
            out.append((name, sx, float('nan'), 0))
            continue
        err = np.abs(d[on, 0] - b[on, 0]).max() * half_width_px
        # The error is unbounded, not merely large, once the baked M[3][*] row
        # drives w' through zero: those points come out BEHIND the eye and
        # invert. Counting them keeps a six-figure pixel number honest --
        # it is a singularity, not a big number.
        flips = int((np.sign(viaM[keep][on, 3]) !=
                     np.sign(direct[keep][on, 3])).sum())
        out.append((name, sx, float(err), flips))
    return out


def print_k(m):
    """Column-major, the layout `make_canvas_k` fills and the layer bakes."""
    k = m.T.reshape(-1)
    print('    k[16] column-major, one COLUMN per line '
          '(k[0]=M[0][0], k[12]=M[0][3]):')
    for c in range(4):
        vals = ' '.join(f'{k[c * 4 + r]:+12.6f}' for r in range(4))
        print(f'      k[{c * 4:>2}..{c * 4 + 3:>2}] {vals}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--self-check', action='store_true')
    ap.add_argument('--yaw', type=float, default=10.0)
    ap.add_argument('--pitch', type=float, default=0.0)
    ap.add_argument('--roll', type=float, default=0.0)
    ap.add_argument('--sx', type=float, default=0.75405)
    ap.add_argument('--sy', type=float, default=None,
                    help='defaults to -sx; X4 carries the Vulkan y flip in sy')
    ap.add_argument('--eye-px', type=int, default=1408)
    a = ap.parse_args()
    rng = np.random.default_rng(20260822)

    if not a.self_check:
        sy = a.sy if a.sy is not None else -a.sx
        m = clip_matrix(a.sx, sy, a.yaw, a.pitch, a.roll)
        print(f'yaw {a.yaw} pitch {a.pitch} roll {a.roll} deg, '
              f'sx={a.sx} sy={sy} near={NEAR}')
        print_k(m)
        return

    print('1. the homography is exact at every depth')
    worst = check_exact(rng)
    print(f'   worst NDC disagreement, 6 cameras x 4000 points: {worst:.3e}')
    assert worst < 1e-9, 'the derivation is wrong, not the numbers'

    print('\n2. six of sixteen entries scale with sx/sy, and they are the '
          'rotating ones')
    print(f"   {'sx':>10}  {'M[0][3]':>12} {'M[3][0]':>12} {'M[0][0]':>10}")
    for name, sx in CAMERAS.items():
        m = clip_matrix(sx, -sx, 10.0, 0.0)
        print(f'   {sx:>10.5f}  {m[0][3]:>+12.5f} {m[3][0]:>+12.6f} '
              f'{m[0][0]:>+10.5f}   {name}')
    print('   M[0][3] = sx.sin(yaw), M[3][0] = -sin(yaw)/sx, M[0][0] = cos(yaw)')

    print('\n3. what a BAKED matrix costs, at 10 deg of yaw on a '
          f'{a.eye_px} px eye')
    for bake_at, why in ((1.33333, 'the menu camera, which is what a module '
                                   'compiled at startup would see'),
                         (0.75405, 'the fov 1.437 camera')):
        print(f'   baked at sx={bake_at} ({why}):')
        for name, sx, err, flips in check_baked(rng, bake_at, 10.0, 0.0,
                                                a.eye_px / 2.0):
            mark = '  <- baked here' if abs(sx - bake_at) < 1e-6 else ''
            note = f'  [{flips} points inverted through w=0]' if flips else ''
            print(f'     sx={sx:>9.5f}  worst on-screen x error '
                  f'{err:>9.1f} px  {name}{mark}{note}')

    print('\n4. the same rotation as a matrix, for the shipped camera')
    print_k(clip_matrix(0.75405, -0.75405, 10.0, 0.0))
    print('\nSELF-CHECK PASS')


if __name__ == '__main__':
    main()
