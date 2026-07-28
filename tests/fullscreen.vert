#version 450
// Full-screen triangle from gl_VertexIndex, no vertex buffer. Deliberately
// view-independent: with multiview working, both views must produce the same
// pixels, so a difference between layers means something other than the eye
// matrix is leaking in.
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
