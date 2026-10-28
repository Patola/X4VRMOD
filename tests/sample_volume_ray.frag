#version 450
// The shape of X4's volumetric fog ray march (volume_raymarch3.frag.glsl):
// the camera block's inverse projections plus a DYNAMIC block whose member
// U_volume_box_position names the shader. The layer recognises the module by
// that name (x4vr::spv::declares_member_name) and keeps X4's own inverse
// projections for it -- take 348. sample_invproj.frag is the same without the
// name, and must not be recognised.
layout(set = 1, binding = 0, std140) uniform CameraBlock {
    mat4 M_view;
    mat4 M_projection;
    mat4 M_invprojection;
    mat4 M_projection_uj;
    mat4 M_invprojection_uj;
} cam;

layout(set = 4, binding = 0, std140) uniform Dynamic {
    vec4 U_volume_box_position;
    vec4 U_volume_box_dimensions;
} dyn;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    // GetRay(): the direction from un-projecting the near plane (z = 1 under
    // reversed-Z), starting at the camera.
    vec4 p = cam.M_invprojection_uj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w);
    color = vec4(dir * 0.5 + 0.5, 1.0) + dyn.U_volume_box_position * 1e-6 +
            dyn.U_volume_box_dimensions * 1e-6;
}
