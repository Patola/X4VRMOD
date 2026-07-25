#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# x4vr-launch.sh — run X4 (or any command) with the X4VRMOD harness.
#
# Modes:
#   no arguments          direct mode: launch X4 from its install dir with
#                         SteamAppId set and the standard dev args
#                         (-skipintro -nocputhrottle -nosoundthrottle)
#   with arguments        wrapper mode: run the given command (e.g. Steam
#                         launch options:  .../x4vr-launch.sh %command%)
#
# Env knobs:
#   X4VR_GAME=<path>      X4 binary (default: the /nvme SteamLibrary install)
#   X4VR_BUILD=<dir>      build dir (default: <repo>/build)
#   X4VR_LOG=<file>       log destination; empty/unset in wrapper mode keeps
#                         the default /tmp/x4vr.log; set X4VR_LOG= (empty) to
#                         send mod logs to stderr for `2>&1 | tee` capture
#   X4VR_GAMESCOPE=1      wrap in gamescope at X4VR_W x X4VR_H (default off).
#                         Needed for an exact render size: X4 ignores
#                         res_width/res_height while borderless and sizes to
#                         the display instead (see common/x4vr_sbs.hpp).
#   X4VR_W / X4VR_H       gamescope size (default: the SBS size in
#                         common/x4vr_sbs.hpp)
#   X4VR_SBS=1            side-by-side composite: copy the left half of each
#                         frame over the right half. Both halves are the same
#                         eye for now -- this validates the container, not the
#                         stereo. Best with X4VR_GAMESCOPE=1.
#   X4VR_X11=1            clear WAYLAND_DISPLAY for the game (force X11/SDL-x11;
#                         X4's Wayland output is new and may misbehave)
#   X4VR_FOSSILIZE=1      keep Valve's fossilize layer (default: disabled to
#                         keep the Vulkan layer chain clean during dev)
#   X4VR_NO_LAYER=1       skip the Vulkan layer
#   X4VR_NO_INJECT=1      skip the LD_PRELOAD injector
#
# Notes:
#   * core dumps are enabled (ulimit -c unlimited) in direct mode.
#   * gamescope on a Plasma Wayland session needs the game itself started
#     with WAYLAND_DISPLAY cleared (it then uses gamescope's XWayland); we
#     always do that inside gamescope.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"

DEFAULT_GAME="/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4"
GAME="${X4VR_GAME:-$DEFAULT_GAME}"

# X4VR_LOG: respect an explicitly-set value, including empty (=stderr).
if [[ -z "${X4VR_LOG+x}" ]]; then
    export X4VR_LOG="/tmp/x4vr.log"
else
    export X4VR_LOG
fi

if [[ "${X4VR_NO_LAYER:-0}" != 1 ]]; then
    if [[ ! -f "$BUILD/layer/VK_LAYER_X4VR_core.json" ]]; then
        echo "x4vr-launch: layer manifest not found in $BUILD/layer — build first" >&2
        exit 1
    fi
    export VK_ADD_LAYER_PATH="$BUILD/layer${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
    export VK_INSTANCE_LAYERS="VK_LAYER_X4VR_core${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
    export VK_LOADER_LAYERS_ENABLE="VK_LAYER_X4VR_core${VK_LOADER_LAYERS_ENABLE:+,$VK_LOADER_LAYERS_ENABLE}"
fi

if [[ "${X4VR_NO_INJECT:-0}" != 1 ]]; then
    if [[ ! -f "$BUILD/injector/libx4vr_inject.so" ]]; then
        echo "x4vr-launch: injector not found in $BUILD/injector — build first" >&2
        exit 1
    fi
    export LD_PRELOAD="$BUILD/injector/libx4vr_inject.so${LD_PRELOAD:+:$LD_PRELOAD}"
fi

# Keep the Vulkan layer chain clean during dev unless explicitly kept.
if [[ "${X4VR_FOSSILIZE:-0}" != 1 ]]; then
    export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
fi

if [[ $# -eq 0 ]]; then
    # ---- direct mode: launch X4 ourselves ----
    if [[ ! -x "$GAME" ]]; then
        echo "x4vr-launch: X4 binary not found at '$GAME' (set X4VR_GAME)" >&2
        exit 1
    fi
    export SteamAppId=392160
    export SteamGameId=392160
    ulimit -c unlimited || true
    cd "$(dirname "$GAME")"
    set -- "$GAME" -skipintro -nocputhrottle -nosoundthrottle
fi

if [[ "${X4VR_X11:-0}" == 1 ]]; then
    export WAYLAND_DISPLAY=""
fi

echo "x4vr-launch: log=${X4VR_LOG:-stderr} layer=$([[ ${X4VR_NO_LAYER:-0} != 1 ]] && echo on || echo off) inject=$([[ ${X4VR_NO_INJECT:-0} != 1 ]] && echo on || echo off) gamescope=${X4VR_GAMESCOPE:-0} sbs=${X4VR_SBS:-0} x11=${X4VR_X11:-0}" >&2

if [[ "${X4VR_SBS:-0}" == 1 && "${X4VR_GAMESCOPE:-0}" != 1 ]]; then
    echo "x4vr-launch: WARNING X4VR_SBS=1 without gamescope — X4 will size to" \
         "the display, so the halves will not be the SBS size" >&2
fi

if [[ "${X4VR_GAMESCOPE:-0}" == 1 ]]; then
    # Single source of truth for the SBS size lives in the C header, so the
    # launcher reads it from there rather than keeping a third copy.
    sbs_dim() { sed -n "s/^#define X4VR_SBS_$1[[:space:]]\+\([0-9]\+\).*/\1/p" \
        "$ROOT/common/x4vr_sbs.hpp"; }
    W="${X4VR_W:-$(sbs_dim WIDTH)}"
    H="${X4VR_H:-$(sbs_dim HEIGHT)}"
    if [[ -z "$W" || -z "$H" ]]; then
        echo "x4vr-launch: could not read the SBS size from" \
             "common/x4vr_sbs.hpp (set X4VR_W / X4VR_H)" >&2
        exit 1
    fi
    # Inside gamescope the game must NOT see the outer Wayland display; it
    # runs on gamescope's XWayland (empirically required on Plasma Wayland).
    exec gamescope -w "$W" -h "$H" -W "$W" -H "$H" --backend sdl -- \
        env WAYLAND_DISPLAY= "$@"
else
    exec "$@"
fi
