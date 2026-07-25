#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# x4vr-launch.sh — run X4 (or any command) with the X4VRMOD harness.
#
# Usage:
#   launch/x4vr-launch.sh /path/to/X4 [args...]
#   Steam launch options:  /path/to/X4VRMOD/launch/x4vr-launch.sh %command%
#
# Env knobs:
#   X4VR_BUILD=<dir>       build dir (default: <repo>/build)
#   X4VR_LOG=<file>        log destination (default: /tmp/x4vr.log)
#   X4VR_GAMESCOPE=1       wrap in gamescope at X4VR_W x X4VR_H (default off)
#   X4VR_W / X4VR_H        gamescope size (default 2816 x 1408)
#   X4VR_NO_LAYER=1        skip the Vulkan layer
#   X4VR_NO_INJECT=1       skip the LD_PRELOAD injector

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"

export X4VR_LOG="${X4VR_LOG:-/tmp/x4vr.log}"

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

echo "x4vr-launch: log=$X4VR_LOG layer=$([[ ${X4VR_NO_LAYER:-0} != 1 ]] && echo on || echo off) inject=$([[ ${X4VR_NO_INJECT:-0} != 1 ]] && echo on || echo off)" >&2

if [[ "${X4VR_GAMESCOPE:-0}" == 1 ]]; then
    W="${X4VR_W:-2816}"
    H="${X4VR_H:-1408}"
    exec gamescope -w "$W" -h "$H" -W "$W" -H "$H" --backend sdl -- "$@"
else
    exec "$@"
fi
