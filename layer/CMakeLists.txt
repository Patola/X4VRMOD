# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
add_library(x4vr_layer SHARED x4vr_layer.cpp ${X4VR_VERSION_FILE})
add_dependencies(x4vr_layer x4vr_version)
set_target_properties(x4vr_layer PROPERTIES
    OUTPUT_NAME "VkLayer_X4VR_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/layer")
# dl: the WSI probe reads SDL_GetCurrentVideoDriver through dlsym rather than
# interposing it -- see log_sdl_backend_once().
find_package(ZLIB REQUIRED)   # lossless PNG for the image dumps (task #49)
target_link_libraries(x4vr_layer PRIVATE Threads::Threads ${CMAKE_DL_LIBS}
                      ZLIB::ZLIB)
# The C++ runtime linked in, and only the entry points exported (step 2 of
# docs/packaging-plan.md): the layer runs inside X4's process, possibly next
# to Steam's runtime libraries, where a mismatched libstdc++ is a classic
# crash. tests/check-exports.sh checks the result after every build.
target_link_options(x4vr_layer PRIVATE -static-libstdc++ -static-libgcc
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/x4vr_layer.map")
set_property(TARGET x4vr_layer APPEND PROPERTY LINK_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/x4vr_layer.map)
add_custom_command(TARGET x4vr_layer POST_BUILD
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tests/check-exports.sh
            --layer $<TARGET_FILE:x4vr_layer>
    VERBATIM)

# OpenXR: headers at build time, loader dlopen'd at run time. The layer is
# injected into X4's process, so a DT_NEEDED on libopenxr_loader would be X4
# refusing to start on a machine without one -- not a graceful fall back to
# flatscreen. Nothing is linked here on purpose.
if(TARGET OpenXR::headers OR OPENXR_HEADER_DIR)
    target_compile_definitions(x4vr_layer PRIVATE X4VR_HAVE_OPENXR=1)
    if(TARGET OpenXR::headers)
        target_link_libraries(x4vr_layer PRIVATE OpenXR::headers)
    else()
        target_include_directories(x4vr_layer PRIVATE ${OPENXR_HEADER_DIR})
    endif()
endif()

# Manifest with an absolute library path (developer-friendly: point
# VK_ADD_LAYER_PATH at the build dir and it just works).
set(X4VR_LAYER_LIB "${CMAKE_BINARY_DIR}/layer/libVkLayer_X4VR_core.so")
configure_file(VK_LAYER_X4VR_core.json.in
               "${CMAKE_BINARY_DIR}/layer/VK_LAYER_X4VR_core.json" @ONLY)
