#version 450
// The control for sample_twin.frag: same table, same declaration, reading slot 0
// instead of the twin at slot 4. With only source layer 1 drawn, this must come
// back empty while the twin comes back with content -- which is what pins "the
// mirror put layer 1 in the twin *and left X4's own slot alone*". A mirror that
// retargeted slot 0 would pass the twin case and fail this one.
layout(set = 0, binding = 0) uniform sampler2D tex[8];
layout(location = 0) out vec4 colour;
void main() {
    colour = texture(tex[0], gl_FragCoord.xy / 128.0);
}
