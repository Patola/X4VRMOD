#version 450
// The full-screen triangle of fullscreen.vert, passing its screen position on
// as an interpolated coordinate (0..1 across the target). A pattern drawn from
// it stays at the same place at any render size -- gl_FragCoord would not --
// which is what X4VR_UI_SS needs to be tested against: X4's UI shaders do not
// read gl_FragCoord either.
layout(location = 0) out vec2 o_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    o_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
