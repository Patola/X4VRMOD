# X4VRMOD — top-level build.
# GPLv3 with a Linux-only X4 linking exception (see LICENSE, LICENSE.exception).
cmake_minimum_required(VERSION 3.20)
project(x4vrmod LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED) # headers; the layer does not link libvulkan

# OpenXR headers only. The loader is dlopen'd at runtime (see common/x4vr_xr.hpp),
# so a machine without an OpenXR runtime still builds and still runs flatscreen.
find_package(OpenXR CONFIG QUIET)
if(NOT TARGET OpenXR::headers)
    find_path(OPENXR_HEADER_DIR openxr/openxr.h)
endif()
if(TARGET OpenXR::headers OR OPENXR_HEADER_DIR)
    message(STATUS "OpenXR headers found — VR paths will be built")
else()
    message(STATUS "OpenXR headers NOT found — VR paths will be skipped")
endif()

include_directories(${Vulkan_INCLUDE_DIRS})

# The version both libraries carry and print on their first log line (bug
# reports need it). Regenerated at every build, rewritten only when changed.
set(X4VR_VERSION_FILE "${CMAKE_BINARY_DIR}/generated/x4vr_version.cpp")
add_custom_target(x4vr_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
            -DOUT=${X4VR_VERSION_FILE}
            "-DX4VR_VERSION_FALLBACK=unknown (built without git)"
            -P ${CMAKE_SOURCE_DIR}/cmake/x4vr_version.cmake
    BYPRODUCTS ${X4VR_VERSION_FILE}
    COMMENT "x4vr version"
    VERBATIM)

add_subdirectory(layer)
add_subdirectory(injector)
add_subdirectory(launcher)
option(X4VR_BUILD_TESTS "Build the test programs" ON)
if(X4VR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# A package's layout (docs/packaging-plan.md, Install and uninstall). The zip is
# tools/make-zip.sh. Fixed lib/x4vrmod, because the core looks there.
set(X4VR_LAYER_LIB "${CMAKE_INSTALL_PREFIX}/lib/x4vrmod/libVkLayer_X4VR_core.so")
configure_file(layer/VK_LAYER_X4VR_core.json.in
               "${CMAKE_BINARY_DIR}/package/VK_LAYER_X4VR_core.json" @ONLY)
install(TARGETS x4vr_layer x4vr_inject LIBRARY DESTINATION lib/x4vrmod)
install(TARGETS x4vrmod RUNTIME DESTINATION bin)
install(FILES "${CMAKE_BINARY_DIR}/package/VK_LAYER_X4VR_core.json"
        DESTINATION share/vulkan/explicit_layer.d)
install(PROGRAMS launch/x4vr-launch.sh launch/deploy-extensions.sh
        DESTINATION share/x4vrmod/launch)
install(FILES launch/x4vr-defaults.sh launch/defaults.conf
        DESTINATION share/x4vrmod/launch)
install(DIRECTORY extension/x4vrmod DESTINATION share/x4vrmod/extension)
install(DIRECTORY launcher/help DESTINATION share/x4vrmod)
install(FILES LICENSE LICENSE.exception DESTINATION share/licenses/x4vrmod)
install(FILES README.md docs/releases.md DESTINATION share/doc/x4vrmod)
# add_subdirectory(cursor)   # later phase
