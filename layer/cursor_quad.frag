// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The blend state does the compositing, not the shader: X4's cursor carries
// straight (non-premultiplied) alpha, so SRC_ALPHA / ONE_MINUS_SRC_ALPHA is
// exactly right and doing it here as well would apply the alpha twice.
//
// Solid quads (u0 < 0) are the steering line: the tint is the colour, and the
// edges across the line (v_t.y near 0 and 1) fade over `v_feather` so a
// one-to-two-pixel line does not stair-step in the headset.
//
// Regenerate with:  glslangValidator -V cursor_quad.frag -o cursor_quad.frag.spv
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_cursor;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_t;
layout(location = 2) flat in vec4 v_tint;
layout(location = 3) flat in float v_feather;
layout(location = 0) out vec4 o_color;

void main() {
    if (v_uv.x < 0.0) {
        float edge = 0.5 - abs(v_t.y - 0.5);
        float a = v_feather > 0.0 ? clamp(edge / v_feather, 0.0, 1.0) : 1.0;
        o_color = vec4(v_tint.rgb, v_tint.a * a);
    } else {
        o_color = texture(u_cursor, v_uv) * v_tint;
    }
}
