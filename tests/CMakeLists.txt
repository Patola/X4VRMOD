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

# The projection math. Self-checking: it takes no arguments and returns
# non-zero on the first bad case, so it needs no runner script.
add_executable(x4vr_test_view_math view_math.cpp)

# The injector-to-layer channel. Header-only and process-local, so this needs
# neither Vulkan nor either component -- it links only a thread library, for the
# concurrent-writer case. Self-checking like view_math: no runner script.
add_executable(x4vr_test_share share.cpp)
find_package(Threads REQUIRED)
target_link_libraries(x4vr_test_share PRIVATE Threads::Threads)

# Where the cursor lands and what its bytes mean (task #17). Needs no Vulkan
# library -- it only uses the header's enums and two pure functions -- and no
# GPU, which is the whole point of pulling them out of the overlay class.
add_executable(x4vr_test_cursor_place cursor_place.cpp)
target_include_directories(x4vr_test_cursor_place PRIVATE ${Vulkan_INCLUDE_DIRS})

# The same overlay driven on a real GPU, under the validation layer. This is
# the one that would have caught a bad barrier or a cursor in one eye only,
# and it needs no X4.
add_executable(x4vr_test_cursor_render cursor_render.cpp)
target_link_libraries(x4vr_test_cursor_render PRIVATE Vulkan::Vulkan)

# The OpenXR bring-up, on a real GPU and a real runtime. Links libvulkan but
# NOT libopenxr_loader: common/x4vr_xr.hpp dlopen()s the loader, because the
# layer is injected into someone else's process and a missing DT_NEEDED there
# is X4 refusing to start rather than a graceful fall back to flatscreen. The
# headers are a build dependency; the loader is not.
if(TARGET OpenXR::headers OR OPENXR_HEADER_DIR)
    add_executable(x4vr_test_xr_probe xr_probe.cpp)
    target_link_libraries(x4vr_test_xr_probe PRIVATE Vulkan::Vulkan ${CMAKE_DL_LIBS})
    if(TARGET OpenXR::headers)
        target_link_libraries(x4vr_test_xr_probe PRIVATE OpenXR::headers)
    else()
        target_include_directories(x4vr_test_xr_probe PRIVATE ${OPENXR_HEADER_DIR})
    endif()
else()
    message(STATUS "OpenXR headers not found — skipping x4vr_test_xr_probe")
endif()

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
configure_file(sample_camera_block.vert.spv sample_camera_block.vert.spv COPYONLY)
configure_file(sample_light_volume.vert.spv sample_light_volume.vert.spv COPYONLY)
configure_file(sample_invproj.frag.spv sample_invproj.frag.spv COPYONLY)
configure_file(sample_combined.spv sample_combined.spv COPYONLY)
configure_file(skybox_cube.comp.spv skybox_cube.comp.spv COPYONLY)
configure_file(noop.comp.spv noop.comp.spv COPYONLY)

# The inputmap rewrite that gives head-look a binding of its own. Pure string
# work on a fixture shaped like the real file, so it needs neither X4 nor a
# GPU. Self-checking like view_math: no runner script.
add_executable(x4vr_test_inputmap inputmap.cpp)
