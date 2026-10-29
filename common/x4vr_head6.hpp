// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_head6.hpp -- X4VR_HEAD_6DOF: the head's position in the cockpit.
//
// The layer measures the head's offset from a neutral point (the neck pivot
// taken out, so a pure rotation moves nothing) in the room's axes, which are
// the cockpit's, and moves the eye in the world shaders (x4vr_view.hpp's
// PoseBlock: eye_ofs z/w and head_fwd). X4's own eye stays put: take 361
// moved it through the camera's base and X4's HUD, a 3D object a few cm in
// front of that eye, swung with every lean. The offset is declared unrotated
// -- the lever arm of take 244 was a head-axes shift rotated by the camera
// angle.
#pragma once

#include <cmath>

namespace x4vr::head6 {

// The offset limited to a sphere of radius `max` m: a lean far enough to put
// the eye through the canopy is held at the limit, in the same direction.
// A non-finite component (a lost pose) gives zero.
inline void clamp_len(float v[3], float max) {
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
        v[0] = v[1] = v[2] = 0.f;
        return;
    }
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (max <= 0.f) {
        v[0] = v[1] = v[2] = 0.f;
    } else if (l > max) {
        const float k = max / l;
        v[0] *= k, v[1] *= k, v[2] *= k;
    }
}

} // namespace x4vr::head6
