# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
add_library(x4vr_inject SHARED x4vr_inject.cpp)
set_target_properties(x4vr_inject PROPERTIES
    OUTPUT_NAME "x4vr_inject"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/injector")
target_link_libraries(x4vr_inject PRIVATE dl Threads::Threads)
