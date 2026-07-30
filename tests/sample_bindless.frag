#version 450
// X4's actual shape, reproduced small enough to commit.
//
// X4 is bindless: one array of 53306 textures at set 0 binding 7, a separate
// array of 18 samplers at set 0 binding 4, and the element index arrives as a
// plain uint in a uniform block -- `S_diffuse_idx`, member 11 of
// BLOCK_BUFFER_BINDING_SLOT_DYNAMIC at set 4 binding 0. So a shader does
//
//     texture(sampler2D(tex[idx], samp[k]), uv)
//
// and every material in the game shares that one table.
//
// Two things this pins:
//
// list_sampled_textures must see *through* the array and report the count. It
// did not, at first, and reported "samples nothing" about X4's tonemap -- the
// variable's type is an OpTypeArray of images, not an image, so looking only
// for the image type walked straight past it. The count is the load-bearing
// number, because it is what rules out promoting the type.
//
// patch_fragment_view_layer must REFUSE this. Promoting the element type
// promotes all 53306 entries to sampler2DArray, and only a handful of images
// are doubled -- every other texture in the game would then be read at a layer
// that does not exist. The patch bails because the pointee is an array rather
// than an image, which was luck as much as design, so it is asserted here.
layout(set = 0, binding = 7) uniform texture2D tex[64];
layout(set = 0, binding = 4) uniform sampler samp[4];
layout(set = 4, binding = 0) uniform Dynamic { uint diffuse_idx; } dyn;
layout(location = 0) out vec4 colour;
void main() {
    colour = texture(sampler2D(tex[dyn.diffuse_idx], samp[1]),
                     gl_FragCoord.xy / 128.0);
}
