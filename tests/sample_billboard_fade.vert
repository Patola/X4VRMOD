#version 450
// sample_billboard.vert as one of X4's near/far-faded effects (its fog cards) has it.
// The shape of X4's common world vertex shader as patch_vertex_facing_body sees
// it: the camera block at (1, 0) with M_view, the object block at (3, 0) with
// M_world, and X4's input names as its compiled modules carry them
// (SPECIAL_VERTEXLOCATION_*; its GLSL's IN_VERTEXLOCATION_* is a macro for them). X4's own modules never enter this repository
// (they are copyrighted); this one only has to read and write the same things.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
    mat4 M_invprojection_uj;
    mat4 M_jitter;
    mat4 M_prevjitter;
    mat4 M_viewprojection;
    mat4 M_viewinverse;
};
layout(set = 3, binding = 0, std140) uniform WorldBlock {
    mat4 M_worldviewprojection;
    mat4 M_world;
    mat4 M_prevworldviewprojection;
};
// One of X4's near/far-faded effect materials declares this (its fog cards among them).
layout(set = 4, binding = 0, std140) uniform DynamicBlock {
    float U_camera_fade_range_near_start;
};
layout(location = 0) in vec3 SPECIAL_VERTEXLOCATION_POSITION;
layout(location = 1) in vec4 SPECIAL_VERTEXLOCATION_NORMAL;
layout(location = 0) out vec3 IO_world_pos;
layout(location = 1) out vec3 IO_normal;

struct InstanceData {
    mat4 world;
    mat4 wvp;
};

void main() {
    InstanceData inst;
    inst.world = M_world;
    inst.wvp = M_worldviewprojection;
    vec3 Pos = SPECIAL_VERTEXLOCATION_POSITION;
    gl_Position = inst.wvp * vec4(Pos, 1.0);
    IO_world_pos = (inst.world * vec4(Pos, 1.0)).xyz;
    vec4 normal = SPECIAL_VERTEXLOCATION_NORMAL;
    IO_normal = mat3(inst.world) * normal.xyz;
}
