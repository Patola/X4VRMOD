#version 450
// The deferred reconstruction with nothing else in the way, for take 399's
// round-trip check: a clip-space position in, the view position X4 would
// reconstruct out. sample_invproj.frag takes its depth from a texture, which
// the float interpreter (tests/spirv_interp.hpp) does not sample; this one
// takes the whole clip position as an input so the patched M_invprojection
// can be RUN against the patched vertex stage on the same numbers.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
} cam;

layout(location = 0) in vec4 clip;
layout(location = 0) out vec4 color;

void main() {
    vec4 vpos = cam.M_invprojection * clip;
    vpos /= vpos.w;
    color = vec4(vpos.xyz, 1.0);
}
