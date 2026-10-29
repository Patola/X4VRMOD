// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_head6.hpp -- X4VR_HEAD_6DOF: the head's position in the cockpit,
// through X4's own camera (Patola, 2026-10-29: "a great solution, better
// than the one we had previously").
//
// In camera mode 0 X4 rebuilds the camera's own transform every frame as the
// look-around rotation times the camera's BASE transform (+0x6f0), so the
// eye's position in the cockpit is the base's translation, and only X4's
// camera set-up routines ever write the base (docs/x4-quirks.md, "Full 6DOF
// in the cockpit"). The layer measures the head's offset from its neutral
// point (the neck pivot taken out, so a pure rotation moves nothing), in the
// tracking space's axes; the injector writes it, in X4's axes, into the
// base's translation. Nothing rotates it: the cockpit and the room share
// their axes, which is what the shader shift of #74 got wrong (take 244's
// lever arm).
#pragma once

#include <cmath>

namespace x4vr::head6 {

// OpenXR's reference spaces are right-handed with -z forward; X4's camera
// rows put +z forward (camwrite::rows_for: yaw 0 looks along row 2 = +z).
inline void to_x4(const float t[3], float out[3]) {
    out[0] = t[0];
    out[1] = t[1];
    out[2] = -t[2];
}
inline void from_x4(const float x[3], float out[3]) {
    out[0] = x[0];
    out[1] = x[1];
    out[2] = -x[2];
}

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
