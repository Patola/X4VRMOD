#version 450
// textureSize() on the patched texture, which the patch must refuse.
//
// OpImageQuerySizeLod returns ivec2 for a 2D image and ivec3 for a 2D array,
// so promoting the type silently changes the result type of an instruction the
// patch never looked at, and the arithmetic downstream of it goes wrong in a
// way nothing would report. This is the case the "anything not understood is a
// bail-out" rule exists for.
// The texture is also genuinely sampled, and that is not decoration. With only
// the query in it the module gets refused for a different reason -- "declared
// but never read" -- and the case would still pass with the unknown-use rule
// deleted. Mutation testing found exactly that; the sample is what makes this
// case depend on the rule it is named after.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 colour;
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(src, 0));
    colour = texture(src, uv);
}
