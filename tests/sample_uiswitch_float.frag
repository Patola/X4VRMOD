#version 450
// The collider, and the reason the patch exists in this form.
//
// Byte 512 of X4's per-draw block is `U_fsr3g_useuitexture` in four of the 370
// modules that declare the block and a FLOAT in 119 others — `U_patternstr` in
// 54 of them alone (tools/block_offset_census.py 4 0 --at 512). A host write of
// `uint 1` at that offset would put 1.4e-45 into every one of them, which is
// what retired the write-the-buffer route.
//
// So this fixture is the same block with a float at 512 under the commonest
// colliding name, and the patch must refuse it and leave the module
// byte-identical. Type is checked before name here: this file fails the type
// test, and asking for the wrong NAME on the good fixture covers the other
// half.
layout(set = 4, binding = 0, std140) uniform DynamicBlock {
    layout(offset = 0)   vec4 pad_head;
    layout(offset = 432) float U_gamma;
    layout(offset = 480) uint S_pattern_idx;
    layout(offset = 512) float U_patternstr;
} dyn;

layout(set = 0, binding = 1) uniform sampler2D patternTex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec4 c = texture(patternTex, uv * dyn.U_gamma);
    color = vec4(c.rgb * dyn.U_patternstr, c.a);
}
