#version 450
// A depth-compare sampler, which the patch must refuse.
//
// Shadow maps are not among the images the layer doubles, so giving one a
// third coordinate would send view 1 to a layer that does not exist. This is
// not a hypothetical: globally-applied shadows are what killed an earlier X4
// VR attempt, so "refuses to touch shadow samplers" is an asserted property
// here rather than a comment in the patch.
//
// The refusal is over-determined and that is on purpose -- three separate
// rules in the patch each reject this module by themselves. See the note in
// run-multiview-render.sh: it means this case cannot be used to justify any
// one of them.
layout(set = 0, binding = 0) uniform sampler2DShadow shadowmap;
layout(location = 0) out vec4 colour;
void main() { colour = vec4(texture(shadowmap, gl_FragCoord.xyz)); }
