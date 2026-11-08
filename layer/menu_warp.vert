// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// One triangle that covers the whole target: (-1,-1), (3,-1), (-1,3). No vertex
// buffer, for the same reason as cursor_quad.vert. The fragment shader does all
// the work, from gl_FragCoord.
//
// Regenerate with:  glslangValidator -V menu_warp.vert -o menu_warp.vert.spv
#version 450

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
