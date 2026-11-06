# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Runs tools/make_extension.py with this build's version and date, at every build; the
# script does nothing unless something changed. Without Python it copies the extension,
# which then leaves X4's own loading screen in place.
execute_process(COMMAND git describe --tags --always --dirty --match "v[0-9]*"
    WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE ver
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR ver STREQUAL "")
    set(ver "unknown")
endif()
execute_process(COMMAND git log -1 --format=%cs
    WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE day
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc ERROR_QUIET)
if(NOT rc EQUAL 0 OR day STREQUAL "")
    string(TIMESTAMP day "%Y-%m-%d")
endif()
if(PYTHON)
    execute_process(COMMAND "${PYTHON}" "${SRC}/tools/make_extension.py"
        --src "${SRC}/extension/x4vrmod" --art "${SRC}/launcher/art/loading-screen.png"
        --version "${ver}" --date "${day}" --out "${BIN}/extension/x4vrmod"
        RESULT_VARIABLE rc)
    if(rc EQUAL 0)
        return()
    endif()
endif()
file(REMOVE_RECURSE "${BIN}/extension/x4vrmod")
file(COPY "${SRC}/extension/x4vrmod" DESTINATION "${BIN}/extension")
