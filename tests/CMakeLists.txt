# Tests link libvulkan (unlike the layer, which only uses the headers).
add_executable(x4vr_test_multiview_enable multiview_enable.cpp)
target_link_libraries(x4vr_test_multiview_enable PRIVATE Vulkan::Vulkan)

add_executable(x4vr_test_multiview_double multiview_double.cpp)
target_link_libraries(x4vr_test_multiview_double PRIVATE Vulkan::Vulkan)
