#version 450
// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A stand-in for X4's tonemap: a 3D colour table looked up at another element (51), no
// U_pass. The fog-blend patch must leave it alone (the older recogniser took it).
layout(set = 0, binding = 5) uniform texture3D S_sampler3D[64];
layout(set = 0, binding = 4) uniform sampler S_sampler[18];
layout(location = 0) in vec3 IO_rgb;
layout(location = 0) out vec4 OUT_RT0;

void main() {
    OUT_RT0 = vec4(textureLod(sampler3D(S_sampler3D[51], S_sampler[9]), IO_rgb, 0.0).rgb, 1.0);
}
