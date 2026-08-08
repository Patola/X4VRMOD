// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A textured quad, positioned entirely by push constants. No vertex buffer and
// no index buffer: four corners are cheaper to derive from gl_VertexIndex than
// to allocate, bind and keep alive, and the quad's whole job is to sit where a
// push constant says.
//
// Regenerate with:  glslangValidator -V cursor_quad.vert -o cursor_quad.vert.spv
#version 450

layout(push_constant) uniform Push {
    vec4 rect; // x0 y0 x1 y1, already in NDC
    vec4 uv;   // u0 v0 u1 v1, the used sub-rect of the cursor atlas
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
    // 0 -> (0,0), 1 -> (1,0), 2 -> (0,1), 3 -> (1,1): a triangle strip.
    vec2 t = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, t), 0.0, 1.0);
    v_uv = mix(pc.uv.xy, pc.uv.zw, t);
}
