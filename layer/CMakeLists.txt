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

# The headset's loading panel: our art and each language's words, compiled in
# (launcher/art, made by tools/make_vr_loading.py; cmake/x4vr_embed.cmake).
set(X4VR_ART ${CMAKE_SOURCE_DIR}/launcher/art)
set(X4VR_VR_LOADING_FILES
    "kVrLoadingArt=${X4VR_ART}/loading-screen.png"
    "kVrLoadingWords_en=${X4VR_ART}/vr-loading-en.png"
    "kVrLoadingWords_pt_BR=${X4VR_ART}/vr-loading-pt_BR.png"
    "kVrLoadingWords_fr=${X4VR_ART}/vr-loading-fr.png"
    "kVrLoadingWords_ru=${X4VR_ART}/vr-loading-ru.png"
    "kVrLoadingWords_de=${X4VR_ART}/vr-loading-de.png")
set(X4VR_VR_LOADING_HPP ${CMAKE_CURRENT_BINARY_DIR}/generated/vr_loading_assets.hpp)
set(X4VR_VR_LOADING_DEPS)
foreach(pair IN LISTS X4VR_VR_LOADING_FILES)
    string(REGEX REPLACE "^[^=]*=" "" path "${pair}")
    list(APPEND X4VR_VR_LOADING_DEPS "${path}")
endforeach()
add_custom_command(OUTPUT ${X4VR_VR_LOADING_HPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${X4VR_VR_LOADING_HPP}
            "-DFILES=${X4VR_VR_LOADING_FILES}" -P ${CMAKE_SOURCE_DIR}/cmake/x4vr_embed.cmake
    DEPENDS ${X4VR_VR_LOADING_DEPS} ${CMAKE_SOURCE_DIR}/cmake/x4vr_embed.cmake
    COMMENT "The headset's loading panel"
    VERBATIM)
target_sources(x4vr_layer PRIVATE ${X4VR_VR_LOADING_HPP})
target_include_directories(x4vr_layer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

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
