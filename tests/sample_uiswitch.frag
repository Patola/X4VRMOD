#version 450
// Task #76. Reproduces the shape of X4's `copy_gamma.frag.glsl`: the final
// present-pass shader, which composites a separate premultiplied-alpha UI
// texture over an already-HUD-less scene unless one switch says otherwise.
//
// What has to be faithful is the BLOCK, not the shader. The patch is refused
// or accepted on four properties at once — descriptor set 4 binding 0, a
// member at byte offset exactly 512, that member's type being a 32-bit
// UNSIGNED int, and its OpMemberName — so the fixture has to carry all four.
//
// Explicit `layout(offset=)` rather than X4's real member list. X4 reaches 512
// by declaring 21 members ahead of it, and copying that list here would be
// copying Egosoft's shader. The offsets are what the patch reads, and they are
// reproduced exactly; the member INDEX deliberately is not, so a patch that
// secretly keyed on "member 21" fails this test.
//
// `S_diffuse_idx` at 480 is declared and never read, on purpose. It is this
// suite's `M_shadowCSM0Clip` — a member the patch can find and must still
// refuse, because a member nothing loads is one nothing can be forced through.
layout(set = 4, binding = 0, std140) uniform DynamicBlock {
    layout(offset = 0)   vec4 pad_head;
    layout(offset = 432) float U_gamma;
    layout(offset = 480) uint S_diffuse_idx;
    layout(offset = 512) uint U_fsr3g_useuitexture;
} dyn;

layout(set = 0, binding = 1) uniform sampler2D sceneTex;
layout(set = 0, binding = 2) uniform sampler2D uiTex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec4 hudless = texture(sceneTex, uv);
    if (dyn.U_fsr3g_useuitexture != 0u) {
        color = hudless;
    } else {
        vec4 ui = texture(uiTex, uv);
        color = vec4(ui.rgb + (1.0 - ui.a) * hudless.rgb, hudless.a);
    }
    color.rgb = pow(color.rgb, vec3(dyn.U_gamma));
}
