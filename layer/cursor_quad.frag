// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Straight passthrough. The blend state does the compositing, not the shader:
// X4's cursor carries straight (non-premultiplied) alpha, so SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA is exactly right and doing it here as well would apply
// the alpha twice.
//
// Regenerate with:  glslangValidator -V cursor_quad.frag -o cursor_quad.frag.spv
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_cursor;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() { o_color = texture(u_cursor, v_uv); }
