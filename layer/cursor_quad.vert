// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A textured quad, positioned entirely by push constants. No vertex buffer and
// no index buffer: four corners are cheaper to derive from gl_VertexIndex than
// to allocate, bind and keep alive, and the quad's whole job is to sit where a
// push constant says.
//
// corner(t) = origin + t.x * e1 + t.y * e2. The cursor passes an axis-aligned
// rectangle (e1 = (x1 - x0, 0), e2 = (0, y1 - y0)); the steering line passes
// a rotated one (x4vr_steer.hpp, line_quad).
//
// Regenerate with:  glslangValidator -V cursor_quad.vert -o cursor_quad.vert.spv
#version 450

layout(push_constant) uniform Push {
    vec4 oe1;  // origin.xy, e1.xy -- NDC
    vec4 e2;   // e2.xy, unused
    vec4 uv;   // u0 v0 u1 v1 of the cursor atlas; u0 < 0: solid, v0 = feather
    vec4 tint; // multiplies the sample; the colour itself when solid
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_t;
layout(location = 2) flat out vec4 v_tint;
layout(location = 3) flat out float v_feather;

void main() {
    // 0 -> (0,0), 1 -> (1,0), 2 -> (0,1), 3 -> (1,1): a triangle strip.
    vec2 t = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    gl_Position = vec4(pc.oe1.xy + t.x * pc.oe1.zw + t.y * pc.e2.xy, 0.0, 1.0);
    v_uv = mix(pc.uv.xy, pc.uv.zw, t);
    v_t = t;
    v_tint = pc.tint;
    v_feather = pc.uv.y;
}
