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

# The extension as it ships: extension/x4vrmod plus its loading screen, drawn with this
# build's version. Regenerated at every build, rewritten only when something changed.
find_package(Python3 QUIET COMPONENTS Interpreter)
add_custom_target(x4vr_extension ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DBIN=${CMAKE_BINARY_DIR}
            -DPYTHON=${Python3_EXECUTABLE} -P ${CMAKE_SOURCE_DIR}/cmake/x4vr_extension.cmake
    COMMENT "x4vr extension"
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
install(DIRECTORY ${CMAKE_BINARY_DIR}/extension/x4vrmod DESTINATION share/x4vrmod/extension)
install(DIRECTORY launcher/help DESTINATION share/x4vrmod)
install(FILES LICENSE LICENSE.exception DESTINATION share/licenses/x4vrmod)
install(FILES README.md docs/releases.md DESTINATION share/doc/x4vrmod)
# add_subdirectory(cursor)   # later phase

# The .deb and .rpm (packaging/ci/package-cpack.sh; the zip and Arch's package have their own
# ways). Each lists what it can't find by looking at what the programs link: gamescope, and the
# OpenXR and Vulkan loaders, which are only dlopen'd. A tag's version is its own; between tags
# it sorts after the last tag, like 0.9.9+16.ge052f4e.
execute_process(
    COMMAND git describe --tags --always --match "v[0-9]*"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE X4VR_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(REGEX REPLACE "^v" "" X4VR_PACKAGE_VERSION "${X4VR_DESCRIBE}")
string(REGEX REPLACE "-([0-9]+)-g" "+\\1.g" X4VR_PACKAGE_VERSION "${X4VR_PACKAGE_VERSION}")
if(NOT X4VR_PACKAGE_VERSION MATCHES "^[0-9]")
    set(X4VR_PACKAGE_VERSION "0.0.0+${X4VR_PACKAGE_VERSION}")
endif()
set(CPACK_PACKAGE_NAME x4vrmod)
set(CPACK_PACKAGE_VERSION "${X4VR_PACKAGE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "VR mod for X4: Foundations on native Linux (unofficial, not affiliated with Egosoft)")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Patola/X4VRMOD")
set(CPACK_PACKAGE_CONTACT "Cláudio Sampaio <patola at gmail dot com>")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION games)
if(NOT DEFINED CPACK_DEBIAN_PACKAGE_SHLIBDEPS)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS "bash")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gamescope, libopenxr-loader1, libvulkan1, qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "gamemode, qt6-translations-l10n")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later WITH x4vrmod-linking-exception")
set(CPACK_RPM_PACKAGE_REQUIRES "bash, gamescope, openxr-libs, vulkan-loader")
set(CPACK_RPM_PACKAGE_SUGGESTS "gamemode, qt6-qttranslations")
# Not ours to claim: these directories belong to the system's own packages.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor
    /usr/share/icons/hicolor/256x256 /usr/share/icons/hicolor/256x256/apps
    /usr/share/licenses /usr/share/doc /usr/share/vulkan
    /usr/share/vulkan/explicit_layer.d)
include(CPack)
