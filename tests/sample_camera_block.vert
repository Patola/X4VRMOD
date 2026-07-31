#version 450
// Reproduces the shape of X4's world vertex shaders, so the eye-offset patch
// can be tested against committed bytes. X4's own modules must never enter
// this repository (they are copyrighted); they live in /tmp/x4vr-shaders and
// are what the patch is finally checked against by hand.
//
// The two things that matter here are both structural:
//
//   * the camera block sits at set 1, binding 0, with M_projection as
//     *member 1* -- the patch reads member 1, column 0, component 0, which is
//     P[0][0] = sx. The preceding member exists so the index is not zero, the
//     following ones so an off-by-one lands on a real mat4 rather than off the
//     end of the struct where it would be caught for the wrong reason.
//   * the block is ColMajor by default in GLSL, which is what X4 declares.
//
// The world block at set 3 is here so the module classifies as World, the same
// way X4's do. It is what positions the vertex, exactly as in the real
// shaders: X4 renders camera-relative, so the camera block is read for
// lighting and depth reconstruction rather than for transforming geometry --
// which is why the patch has to reach for it explicitly.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
} cam;

layout(set = 3, binding = 0, std140) uniform WorldBlock {
    mat4 M_worldviewprojection;
} world;

layout(location = 0) in vec3 pos;
layout(location = 0) out vec3 v_viewdir;

void main() {
    gl_Position = world.M_worldviewprojection * vec4(pos, 1.0);
    // Reads the camera block for something other than position, as X4 does.
    v_viewdir = (cam.M_view * vec4(pos, 1.0)).xyz;
}
