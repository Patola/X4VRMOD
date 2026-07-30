# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
add_library(x4vr_layer SHARED x4vr_layer.cpp)
set_target_properties(x4vr_layer PROPERTIES
    OUTPUT_NAME "VkLayer_X4VR_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/layer")
# dl: the WSI probe reads SDL_GetCurrentVideoDriver through dlsym rather than
# interposing it -- see log_sdl_backend_once().
target_link_libraries(x4vr_layer PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# Manifest with an absolute library path (developer-friendly: point
# VK_ADD_LAYER_PATH at the build dir and it just works).
set(X4VR_LAYER_LIB "${CMAKE_BINARY_DIR}/layer/libVkLayer_X4VR_core.so")
configure_file(VK_LAYER_X4VR_core.json.in
               "${CMAKE_BINARY_DIR}/layer/VK_LAYER_X4VR_core.json" @ONLY)
