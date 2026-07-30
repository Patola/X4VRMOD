#version 450
// Reads the twin element directly, with the index hardcoded rather than
// computed from gl_ViewIndex. That is deliberate: it tests the *mirror* --
// is slot OFFSET written, readable, and holding layer 1 -- without depending
// on the step-B shader patch that does not exist yet. Pair it with
// sample_twin_base.frag, which reads slot 0 of the same table.
//
// Must agree with tests/multiview_render.cpp: 8 descriptors declared, the
// application writing the first 4, so with X4VR_MIRROR_OFFSET=4 the twins land
// at 4..7 and element 4 is the twin of element 0.
layout(set = 0, binding = 0) uniform sampler2D tex[8];
layout(location = 0) out vec4 colour;
void main() {
    colour = texture(tex[4], gl_FragCoord.xy / 128.0);
}
