#version 450
// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A stand-in for X4's fog blend (after take 486), written here: an image table and a
// sampler table, the volume looked up at constant element 39 with sampler 9, a dynamic
// switch named U_pass, and the composite colour * v.a + v.rgb. x4vr::spv::
// patch_fog_apply_march must find that one lookup and add ours beside it.
// The table as large as X4's and named twice, 2D and 3D, as X4's is: the stereo patch then
// offsets this module's lookups by the eye (take 487 lost the blend patch to exactly that).
layout(set = 0, binding = 5) uniform texture2D S_sampler2D[53306];
layout(set = 0, binding = 5) uniform texture3D S_sampler3D[53306];
layout(set = 0, binding = 4) uniform sampler S_sampler[18];
layout(set = 4, binding = 0, std140) uniform BLOCK_BUFFER_BINDING_SLOT_DYNAMIC {
    uint U_index;
    uint U_pass;
};
layout(location = 0) out vec4 OUT_RT0;

void main() {
    vec4 c = vec4(0.2, 0.3, 0.4, 1.0);
    if (U_pass != 0u) {
        // The volume's far end (the sky), across the screen.
        const vec3 uvw = vec3(gl_FragCoord.xy / 64.0, 0.0);
        const vec4 v = textureLod(sampler3D(S_sampler3D[39], S_sampler[9]), uvw, 0.0);
        c.rgb = c.rgb * v.a + v.rgb;
    } else {
        c = textureLod(sampler2D(S_sampler2D[7], S_sampler[9]), gl_FragCoord.xy / 64.0, 0.0);
    }
    OUT_RT0 = c;
}
