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

# --layer <lib>: the layer exports exactly its two entry points and their vk*
# aliases (layer/x4vr_layer.map). Otherwise <lib> is the injector.
MODE=injector
if [ "${1:-}" = "--layer" ]; then
    MODE=layer
    shift
fi
SO="${1:?usage: check-exports.sh [--layer] <lib.so>}"

if [ "$MODE" = layer ]; then
    REQUIRED="
vkGetInstanceProcAddr vkGetDeviceProcAddr
x4vr_GetInstanceProcAddr x4vr_GetDeviceProcAddr
"
else
    # injector/x4vr_inject.map is the other copy of this list; keep the two in
    # step. The read family (read pread pread64 close mmap fread
    # fread_unlocked fclose) and connect were interposed for years without
    # being checked here.
    REQUIRED="
fopen fopen64 fclose fread fread_unlocked
access __xstat __xstat64 __lxstat
open open64 openat openat64
read pread pread64 close mmap
setenv putenv unsetenv connect
SDL_GetWindowSize SDL_GetWindowSizeInPixels
SDL_PeepEvents SDL_WaitEvent SDL_PumpEvents
SDL_GetMouseState SDL_WarpMouseInWindow SDL_SetWindowRelativeMouseMode
SDL_GetKeyboardState SDL_SetCursor SDL_CreateColorCursor
SDL_GetJoysticks SDL_OpenJoystick SDL_OpenGamepad
SDL_IsGamepad SDL_GetJoystickAxis SDL_AddGamepadMappingsFromFile
luaL_loadbuffer luaL_loadfile
x4vr_shared_state x4vr_head_state
"
fi
NAME=$(basename "$SO")

have=$(nm -D --defined-only "$SO" | awk '$2 == "T" { print $3 }')
missing=""
for sym in $REQUIRED; do
    echo "$have" | grep -qx "$sym" || missing="$missing $sym"
done
if [ -n "$missing" ]; then
    echo "FAIL  $SO does not export:$missing" >&2
    echo "      An interposer that is not exported hooks NOTHING, silently." >&2
    echo "      Check for an anonymous namespace or a 'static' around it, and" >&2
    echo "      the library's version script (*.map)." >&2
    exit 1
fi

# Nothing else: every other defined dynamic symbol (code, data, weak, unique)
# is a leak -- an internal helper the game could bind to, or the statically
# linked C++ runtime showing through.
extra=$(nm -D --defined-only "$SO" | awk '{ print $3 }' | while read -r sym; do
    case " $(echo $REQUIRED) " in
        *" $sym "*) ;;
        *) printf ' %s' "$sym" ;;
    esac
done)
if [ -n "$extra" ]; then
    echo "FAIL  $SO exports more than it must:$(echo "$extra" | cut -c1-300)" >&2
    exit 1
fi

# And no dynamic C++ runtime: linked in statically (-static-libstdc++
# -static-libgcc), so a mismatched libstdc++ in X4's process cannot reach it.
needed=$(readelf -d "$SO" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
for lib in $needed; do
    case "$lib" in
        libstdc++.so*|libgcc_s.so*)
            echo "FAIL  $SO needs $lib — the C++ runtime is not linked in" >&2
            exit 1 ;;
    esac
done

n=$(echo "$REQUIRED" | wc -w)
echo "ok    $NAME exports exactly its $n symbols, C++ runtime linked in"
