#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Does libx4vr_inject.so still export the symbols it interposes?
#
# An LD_PRELOAD library works by *exporting* a symbol the target also imports.
# Lose the export and nothing announces it: the library loads, its constructor
# runs, its logging appears, and every single hook is silently dead.
#
# That is not hypothetical. A block of Lua reconnaissance opened an anonymous
# namespace and closed it in the wrong place, which moved fopen, open, and the
# entire SDL family inside it. Internal linkage, no interposition, no warning.
# The build was clean, all six test binaries passed, and X4 ran a whole take
# with the injector doing nothing but logging Lua -- the config was never
# served, so X4 sized itself to the display and rendered the left eye twice.
# Patola caught it from the headset. No test could have.
#
# So this runs at build time, on the artifact rather than the source, and fails
# the build. Checking the source for `namespace` would have missed it too: the
# braces were balanced, the compiler was happy, and only the symbol table knew.
set -eu

SO="${1:?usage: check-exports.sh <libx4vr_inject.so>}"

# Every symbol here is an interposition contract with something outside this
# repo. Adding a hook means adding it here, and that is the point.
REQUIRED="
fopen fopen64 open open64 openat openat64
setenv putenv unsetenv
SDL_GetWindowSize SDL_GetWindowSizeInPixels
SDL_PeepEvents SDL_WaitEvent SDL_PumpEvents
SDL_GetMouseState SDL_WarpMouseInWindow SDL_SetWindowRelativeMouseMode
SDL_GetKeyboardState SDL_SetCursor SDL_CreateColorCursor
luaL_loadbuffer luaL_loadfile
x4vr_shared_state x4vr_head_state
"

have=$(nm -D --defined-only "$SO" | awk '$2 == "T" { print $3 }')
missing=""
for sym in $REQUIRED; do
    echo "$have" | grep -qx "$sym" || missing="$missing $sym"
done

if [ -n "$missing" ]; then
    echo "FAIL  $SO does not export:$missing" >&2
    echo "      An interposer that is not exported hooks NOTHING, silently." >&2
    echo "      Check for an anonymous namespace or a 'static' around it." >&2
    exit 1
fi

n=$(echo "$REQUIRED" | wc -w)
echo "ok    libx4vr_inject.so exports all $n interposed symbols"
