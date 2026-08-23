#version 450
// A procedural fullscreen triangle that DECLARES X4's camera block but never
// touches M_projection. That combination is the one the classifier and the
// pose patch disagree about, and reproducing it here is the whole point.
//
// classify(wide) asks "does this module ACCESS the camera block's projection
// member?" -- that is what separates X4's instanced light volumes (task #48)
// from screen-space quads. patch_vertex_pose_rotate ADDS exactly that access
// when it emits the P.R.P^-1 conjugation. So a module that reads NonWorld
// before the patch reads World after it, and the layer classified the patched
// bytes: 30 of X4's 409 modules flipped that way, every one of them a
// procedural fullscreen quad, including both of rp #31's pairs and rp #32's.
//
// The wide rule fires on an access to member 0, 1, 7 or 8 from inside the
// VERTEX function. So the fixture reads member 2 -- enough to keep the block
// from being optimised away, not one of the four that mean "world". The pose
// patch then adds member 1, and the reading flips.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
} cam;

layout(location = 0) out vec2 uv;

void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // The fullscreen triangle, in clip space -- no camera involved, which is
    // exactly why it must classify NonWorld.
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    // A dependency on member 2 only, scaled to zero: the block is real, and
    // none of the four members that mean "world" is touched.
    gl_Position.z += cam.M_invprojection[0][0] * 0.0;
}
