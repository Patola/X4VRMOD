#version 450
// The part of X4's froxel fog pass (volume_point.comp) patch_fog_points_facing
// reads and rewrites, as straight-line code the test interpreter can run: a block
// with M_offset (view-to-blob matrices) and I_point_num, and the product of one
// loaded M_offset[i] with a view-space position. X4's own module never enters this
// repository (it is copyrighted); this one only has the same names and products.
// Optimised to SSA (spirv-opt -O), as X4's modules are: each M_offset[i] is loaded
// once and its value feeds the products directly.
layout(set = 4, binding = 0, std140) uniform DynamicBlock {
    mat4 M_texturematrix0;
    vec4 U_bias;
    mat4 M_offset[32];
    vec4 V_scale[32];
    int I_index[32];
    int I_point_num;
};
layout(location = 0) in vec3 VIEW_POSITION; // the froxel's position in the camera's space
layout(location = 0) out vec3 OFF0;
layout(location = 1) out vec3 OFF1;
layout(location = 2) out vec3 OFF2;

void main() {
    vec3 view = VIEW_POSITION;
    // Two blobs, and the gradient's second product of the same matrix, as X4 has.
    mat4 view2loc = M_offset[0];
    vec3 off = (view2loc * vec4(view, 1)).xyz;
    vec3 off1 = (view2loc * vec4(view + vec3(1.0), 1)).xyz;
    OFF0 = off;
    OFF1 = (M_offset[1] * vec4(view, 1)).xyz;
    OFF2 = off1;
    gl_Position = vec4(view, 1.0);
}
