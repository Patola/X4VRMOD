# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The OpenXR loader the mod ships (take 441). Started from Steam, X4 runs inside Steam's runtime
# container, which has no OpenXR loader, and the host's can't be loaded in there: Arch's needs
# libjsoncpp.so.27, which the container lacks. So the layer falls back to this copy, beside it
# (common/x4vr_xr.hpp, api_open); the system's loader still goes first everywhere else.
#
# Khronos' loader from a pinned commit, built with its own jsoncpp and the C++ runtime linked in,
# for the reason the layer links it in: it runs in X4's process, possibly next to Steam's runtime
# libraries. Apache-2.0; its licence and jsoncpp's install beside the mod's.
#
# CI fetches the commit. Offline, or to build from a checkout already on disk, pass
# -DX4VR_OPENXR_SDK_DIR=<an OpenXR-SDK checkout at that commit>.
option(X4VR_BUNDLE_OPENXR_LOADER "Build and ship the Khronos OpenXR loader beside the layer" ON)
set(X4VR_OPENXR_SDK_DIR "" CACHE PATH
    "An OpenXR-SDK checkout to build the shipped loader from, instead of fetching it")
set(X4VR_OPENXR_SDK_REPO "https://github.com/KhronosGroup/OpenXR-SDK.git")
# release-1.1.60
set(X4VR_OPENXR_SDK_COMMIT "64f2b37c8c6da3d83c9b4d11865ba1fb752cb8ec")

set(X4VR_OPENXR_LOADER "${CMAKE_BINARY_DIR}/layer/libopenxr_loader.so.1")

if(X4VR_BUNDLE_OPENXR_LOADER AND (TARGET OpenXR::headers OR OPENXR_HEADER_DIR))
    include(ExternalProject)
    if(X4VR_OPENXR_SDK_DIR)
        set(_x4vr_oxr_source SOURCE_DIR "${X4VR_OPENXR_SDK_DIR}" DOWNLOAD_COMMAND "")
        set(_x4vr_oxr_src "${X4VR_OPENXR_SDK_DIR}")
    else()
        set(_x4vr_oxr_source GIT_REPOSITORY "${X4VR_OPENXR_SDK_REPO}"
                             GIT_TAG "${X4VR_OPENXR_SDK_COMMIT}" GIT_PROGRESS OFF
                             UPDATE_DISCONNECTED ON)
        set(_x4vr_oxr_src "${CMAKE_BINARY_DIR}/openxr-sdk/src/openxr_sdk")
    endif()
    set(_x4vr_oxr_prefix "${CMAKE_BINARY_DIR}/openxr-sdk/install")
    ExternalProject_Add(openxr_sdk
        ${_x4vr_oxr_source}
        PREFIX "${CMAKE_BINARY_DIR}/openxr-sdk"
        INSTALL_DIR "${_x4vr_oxr_prefix}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            -DCMAKE_INSTALL_LIBDIR=lib
            # Where the loader looks for runtime manifests besides /etc/xdg: /etc, as in a
            # system's loader, not this build's install folder.
            -DCMAKE_INSTALL_SYSCONFDIR=/etc
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            # The build's own LDFLAGS kept (a package's hardening: full RELRO, namcap).
            "-DCMAKE_SHARED_LINKER_FLAGS=$ENV{LDFLAGS} -static-libstdc++ -static-libgcc"
            -DCMAKE_DISABLE_FIND_PACKAGE_JsonCpp=ON
            -DBUILD_WITH_SYSTEM_JSONCPP=OFF
            -DDYNAMIC_LOADER=ON
            -DBUILD_LOADER=ON
            -DBUILD_API_LAYERS=OFF
            -DBUILD_TESTS=OFF
            -DBUILD_CONFORMANCE_TESTS=OFF
            -DBUILD_SDK_TESTS=OFF
            -DBUILD_ALL_EXTENSIONS=OFF
        BUILD_BYPRODUCTS "${_x4vr_oxr_prefix}/lib/libopenxr_loader.so.1"
        LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)
    # Beside the build tree's layer, where api_open looks; `cmake -E copy` copies what the
    # soname's link points at. One target makes it, so the install rule can name it.
    # Its licences in the build tree too, for tools/make-zip.sh.
    set(_x4vr_oxr_licences "${CMAKE_BINARY_DIR}/licenses/OpenXR-loader.LICENSE"
                           "${CMAKE_BINARY_DIR}/licenses/jsoncpp.LICENSE")
    add_custom_command(OUTPUT "${X4VR_OPENXR_LOADER}" ${_x4vr_oxr_licences}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/layer"
                "${CMAKE_BINARY_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy "${_x4vr_oxr_prefix}/lib/libopenxr_loader.so.1"
                "${X4VR_OPENXR_LOADER}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_x4vr_oxr_src}/LICENSE"
                "${CMAKE_BINARY_DIR}/licenses/OpenXR-loader.LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy "${_x4vr_oxr_src}/src/external/jsoncpp/LICENSE"
                "${CMAKE_BINARY_DIR}/licenses/jsoncpp.LICENSE"
        DEPENDS openxr_sdk "${_x4vr_oxr_prefix}/lib/libopenxr_loader.so.1"
        COMMENT "The OpenXR loader beside the layer"
        VERBATIM)
    add_custom_target(x4vr_openxr_loader ALL DEPENDS "${X4VR_OPENXR_LOADER}" ${_x4vr_oxr_licences})
    install(FILES "${X4VR_OPENXR_LOADER}" DESTINATION lib/x4vrmod
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE)
    install(FILES ${_x4vr_oxr_licences} DESTINATION share/licenses/x4vrmod)
    if(X4VR_OPENXR_SDK_DIR)
        message(STATUS "The OpenXR loader is shipped beside the layer, built from "
                       "${X4VR_OPENXR_SDK_DIR}")
    else()
        message(STATUS "The OpenXR loader is shipped beside the layer, built from "
                       "OpenXR-SDK ${X4VR_OPENXR_SDK_COMMIT}")
    endif()
else()
    message(STATUS "The OpenXR loader is NOT shipped: started from Steam, X4 gets no VR")
endif()
