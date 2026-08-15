# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
add_library(x4vr_inject SHARED x4vr_inject.cpp)
set_target_properties(x4vr_inject PROPERTIES
    OUTPUT_NAME "x4vr_inject"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/injector")
target_link_libraries(x4vr_inject PRIVATE dl Threads::Threads)

# An LD_PRELOAD library that stops EXPORTING a symbol stops interposing it, in
# total silence: the library still loads, its constructor still runs, its
# logging still appears. A misplaced anonymous namespace once moved fopen, open
# and the whole SDL family inside it, and X4 ran a full take with the injector
# doing nothing but log — clean build, all tests green. This checks the symbol
# table of the artifact, which is the only place that knew.
add_custom_command(TARGET x4vr_inject POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "checking interposer exports..."
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../tests/check-exports.sh
            $<TARGET_FILE:x4vr_inject>
    VERBATIM)
