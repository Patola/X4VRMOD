# X4VRMOD launcher: a Qt-free core and the x4vrmod command line. The window comes
# later, over the same core (docs/packaging-plan.md, Phases 2 and 3).
add_library(x4vrmod_core STATIC
    core/discover.cpp
    core/patches.cpp
    core/probe.cpp
    core/checks.cpp
    core/profile.cpp
    core/install.cpp
    ${X4VR_VERSION_FILE})
add_dependencies(x4vrmod_core x4vr_version)
target_include_directories(x4vrmod_core PUBLIC core)
target_compile_definitions(x4vrmod_core PRIVATE
    X4VRMOD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    X4VRMOD_BUILD_DIR="${CMAKE_BINARY_DIR}"
    X4VRMOD_PREFIX="${CMAKE_INSTALL_PREFIX}")
target_link_libraries(x4vrmod_core PUBLIC ${CMAKE_DL_LIBS})
# OpenXR: headers at build time, loader dlopen'd at run time, as in the layer.
if(TARGET OpenXR::headers OR OPENXR_HEADER_DIR)
    target_compile_definitions(x4vrmod_core PRIVATE X4VR_HAVE_OPENXR=1)
    if(TARGET OpenXR::headers)
        target_link_libraries(x4vrmod_core PRIVATE OpenXR::headers)
    else()
        target_include_directories(x4vrmod_core PRIVATE ${OPENXR_HEADER_DIR})
    endif()
endif()

add_executable(x4vrmod cli/main.cpp)
target_link_libraries(x4vrmod PRIVATE x4vrmod_core)
# The same reason as the libraries: the zip must run on other systems' runtimes.
target_link_options(x4vrmod PRIVATE -static-libstdc++ -static-libgcc)
