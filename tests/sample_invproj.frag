#version 450
// Reproduces the shape of X4's deferred lighting passes: reconstruct view
// position from the depth buffer with M_invprojection, then use it. 244 of
// X4's fragment modules do this; correcting the matrix per eye is task #22.
//
// The member layout matters — M_invprojection must be *member 2* of the block
// at set 1, binding 0, with mat4 members either side so an off-by-one lands on
// a real matrix rather than off the end of the struct, where it would be
// rejected for the wrong reason.
//
// X4's real modules declare this block once per stage and reach it through two
// different variables aliased onto the same (set, binding). That is not
// expressible in one GLSL translation unit, so it cannot be reproduced here;
// it is checked against the real modules by hand instead. See the comment in
// patch_fragment_invproj_eye — it is the second time first-match-on-an-aliased-
// binding has caused a bug in this project.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
} cam;

layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    float depth = texture(depthTex, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 vpos = cam.M_invprojection * clip;
    vpos /= vpos.w;
    // Something position-dependent, so the reconstruction cannot be optimised
    // away and the load stays in the module.
    color = vec4(vpos.xyz * 0.5 + 0.5, 1.0);
}
