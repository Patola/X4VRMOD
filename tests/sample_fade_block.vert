#version 450
// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A stand-in for one of X4's near/far-faded effect modules (after take 482): X4 compiles
// a material's properties into a uniform block, the four fade distances among others.
// x4vr::spv::fade_block must find this block's set and binding and the four offsets
// (24, 28, 32, 36 in std140).
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_viewprojection;
};
layout(set = 3, binding = 0, std140) uniform WorldBlock {
    mat4 M_worldviewprojection;
    mat4 M_world;
};
layout(set = 4, binding = 2, std140) uniform MaterialBlock {
    vec4 U_color_emissive;
    float U_angle_fade_speed;
    float U_angle_fade_offset;
    float U_camera_fade_range_near_start;
    float U_camera_fade_range_near_end;
    float U_camera_fade_range_far_start;
    float U_camera_fade_range_far_end;
    float U_diffusestr;
};
layout(location = 0) in vec3 SPECIAL_VERTEXLOCATION_POSITION;
layout(location = 0) out float IO_Fade;

void main() {
    const vec4 p = vec4(SPECIAL_VERTEXLOCATION_POSITION, 1.0);
    const float d = length((M_view * M_world * p).xyz);
    IO_Fade = U_color_emissive.a * U_diffusestr *
              smoothstep(U_camera_fade_range_near_start, U_camera_fade_range_near_end, d) *
              (1.0 - smoothstep(U_camera_fade_range_far_start, U_camera_fade_range_far_end, d)) +
              U_angle_fade_speed * U_angle_fade_offset;
    gl_Position = M_viewprojection * M_world * p + 0.0 * M_worldviewprojection[0];
}
