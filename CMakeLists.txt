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
add_subdirectory(tests)
# add_subdirectory(cursor)   # later phase
