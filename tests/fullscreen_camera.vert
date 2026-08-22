#version 450
// Full-screen triangle that ALSO declares X4's camera block — task #60.
//
// `fullscreen.vert` is what this harness renders, and it has no camera block,
// so `patch_vertex_pose_rotate` correctly refuses it and the GPU never sees
// #33's conjugation. `sample_camera_block.vert` has the block but takes a
// vertex attribute and a set-3 block this harness does not supply. This is the
// two halves put together: procedural geometry the harness can already draw,
// with the block shaped the way the patch verifies before it emits anything.
//
// The block layout matters and is not arbitrary: **M_projection must be member
// 1**, because the patch reads member 1 column 0 component 0 for `sx` and
// column 1 component 1 for `sy`. The members around it exist so an off-by-one
// lands on a real mat4 rather than off the end of the struct, where it would be
// caught for the wrong reason. Same shape as `sample_camera_block.vert`.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
} cam;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // The block has to be READ or the compiler strips it and the patch finds
    // nothing — which would look exactly like the refusal this shader exists to
    // stop. M_view[3][3] is 1.0 in both blocks the harness writes (identity,
    // and take 178's logged matrix), so this is 0.0 and the triangle is
    // unmoved; it is a use, not a transform.
    const float z = cam.M_view[3][3] - 1.0;
    gl_Position = vec4(p * 2.0 - 1.0, z, 1.0);
}
