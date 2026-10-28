#version 450
// A UI-shaped vertex shader for tests/run-multiview-render.sh's cinematic-view
// arm: one vertex input, so the layer does not call it procedural fullscreen
// and builds its canvas variant (and so a canvas pipeline and its twin). The
// input is never fed -- the harness binds no vertex buffer -- and only
// multiplied by zero; the arm asserts on the layer's log and on validation,
// not on pixels.
layout(location = 0) in vec4 a_ui;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0) + a_ui * 0.0;
}
