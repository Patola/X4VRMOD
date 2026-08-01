#version 450
// Reproduces the shape of X4's instanced deferred light volumes (mod-0207): a
// sphere drawn per light, positioned from per-instance attributes and the
// camera's view-projection, with NO set-3 per-object block.
//
// That combination is the whole point. classify() calls a module World only if
// its vertex stage reads member 0 of the set-3 block, so this shape classifies
// UI and draws unsheared -- while the geometry it lights is sheared per eye.
// The light then lands on the wrong pixels in view 1. See task #22 and P70.
//
// The camera block must be at set 1, binding 0 with the members in X4's order,
// because the widened predicate looks for members 0/1/7/8 by index. Members are
// padded out to 7 so M_viewprojection lands where X4 puts it.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;           // 0
    mat4 M_projection;     // 1
    mat4 M_invprojection;  // 2
    mat4 M_projection_uj;  // 3
    mat4 M_invprojection_uj; // 4
    mat4 M_jitter;         // 5
    mat4 M_prevjitter;     // 6
    mat4 M_viewprojection; // 7
} cam;

// Per-instance light parameters, the way mod-0207 receives them.
layout(location = 0) in vec3 SPECIAL_VERTEXLOCATION_POSITION;
layout(location = 1) in vec3 SPECIAL_VERTEXLOCATION_INSTANCE0; // center
layout(location = 2) in float SPECIAL_VERTEXLOCATION_INSTANCE1; // radius

layout(location = 0) out vec3 IO_center;
layout(location = 1) out float IO_radius;

void main() {
    IO_center = SPECIAL_VERTEXLOCATION_INSTANCE0;
    IO_radius = SPECIAL_VERTEXLOCATION_INSTANCE1;
    vec3 world = SPECIAL_VERTEXLOCATION_INSTANCE0 +
                 SPECIAL_VERTEXLOCATION_POSITION * SPECIAL_VERTEXLOCATION_INSTANCE1;
    // Member 7. The camera positions this geometry; no per-object matrix does.
    gl_Position = cam.M_viewprojection * vec4(world, 1.0);
}
