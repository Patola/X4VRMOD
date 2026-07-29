#version 450
// Two textures, one type id. glslc gives `src` and `other` the same
// OpTypeImage, which is why the patch builds a fresh arrayed type instead of
// flipping Arrayed on the shared one: that single word would promote `other`
// to an array as well, and `other` stands for every texture in a real shader
// that nobody doubled.
//
// It also exercises both read forms at once -- `texture` (float coordinates,
// OpImageSampleImplicitLod) and `texelFetch` (integer coordinates, OpImage +
// OpImageFetch) -- so a patch that handles one and quietly ignores the other
// cannot pass.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D other;
layout(location = 0) out vec4 colour;
void main() {
    vec2 uv = gl_FragCoord.xy / 128.0;
    colour = texture(src, uv) + texelFetch(other, ivec2(gl_FragCoord.xy), 0);
}
