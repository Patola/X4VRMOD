# Tests link libvulkan (unlike the layer, which only uses the headers).
add_executable(x4vr_test_multiview_enable multiview_enable.cpp)
target_link_libraries(x4vr_test_multiview_enable PRIVATE Vulkan::Vulkan)

add_executable(x4vr_test_multiview_double multiview_double.cpp)
target_link_libraries(x4vr_test_multiview_double PRIVATE Vulkan::Vulkan)

add_executable(x4vr_test_multiview_render multiview_render.cpp)
target_link_libraries(x4vr_test_multiview_render PRIVATE Vulkan::Vulkan)

# The shader patches, exercised without a GPU. Needs no Vulkan library at all --
# it only ever rewrites words in a buffer, which is the point.
add_executable(x4vr_test_spirv_patch spirv_patch.cpp)

# Shaders are committed as SPIR-V so the suite does not need glslc to run.
configure_file(fullscreen.vert.spv fullscreen.vert.spv COPYONLY)
configure_file(solid.frag.spv solid.frag.spv COPYONLY)
configure_file(sample.frag.spv sample.frag.spv COPYONLY)
configure_file(sample_two.frag.spv sample_two.frag.spv COPYONLY)
configure_file(sample_shadow.frag.spv sample_shadow.frag.spv COPYONLY)
configure_file(sample_size.frag.spv sample_size.frag.spv COPYONLY)
configure_file(sample_bindless.frag.spv sample_bindless.frag.spv COPYONLY)
configure_file(sample_twin.frag.spv sample_twin.frag.spv COPYONLY)
configure_file(sample_twin_base.frag.spv sample_twin_base.frag.spv COPYONLY)
configure_file(sample_alias_binding.frag.spv sample_alias_binding.frag.spv COPYONLY)
configure_file(sample_combined.spv sample_combined.spv COPYONLY)
configure_file(skybox_cube.comp.spv skybox_cube.comp.spv COPYONLY)
configure_file(noop.comp.spv noop.comp.spv COPYONLY)
