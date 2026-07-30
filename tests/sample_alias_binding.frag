#version 450
// Two variables aliased onto ONE (set, binding), which is legal and is what X4
// does in 228 of its 409 dumped modules -- including module #12, the fragment
// shader of the pass that composites into the swapchain.
//
// This is the shape that produced the black right eye through take forty-eight.
// The lister returns one entry per variable, so the layer called the patch
// twice with the same (set, binding); the patch stopped at the first matching
// variable, so both calls hit `tabA` -- offsetting it to index + 2*OFFSET,
// which is 53307 in a 53306-element array, read as zeros -- while `tabB` was
// never patched at all and kept view 1 on view 0's slot.
//
// The two index forms are deliberate and both appear in module #12: a literal
// constant and a value loaded from a uniform block. The constant is the one
// that mattered, because slot 1 is a per-eye image in X4's table.
//
// No nonuniformEXT here, deliberately: it emits OpCopyObject, whose result type
// the patch does not track, so it declines the whole module. X4's modules load
// the index straight out of the block, so that refusal is not X4's shape and
// putting it here would test the wrong thing.
layout(set = 0, binding = 5) uniform sampler2D tabA[53306];
layout(set = 0, binding = 5) uniform sampler2D tabB[53306];
layout(set = 0, binding = 9) uniform Dyn { uint which; } dyn;
layout(location = 0) out vec4 colour;
void main() {
    colour = texture(tabA[1], gl_FragCoord.xy) +
             texture(tabB[dyn.which], gl_FragCoord.xy);
}
