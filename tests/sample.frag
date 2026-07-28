#version 450
// Reads the doubled target back through an ordinary combined image sampler --
// the descriptor the layer's gate-2 redirect rewrites. Whatever layer that
// descriptor ends up naming is what lands in the output, so the output colour
// is a direct readout of where the redirect pointed.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 colour;
void main() { colour = texelFetch(src, ivec2(gl_FragCoord.xy), 0); }
