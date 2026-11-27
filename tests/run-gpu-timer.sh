#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The march's GPU stopwatch (layer/x4vr_gpu_timer.hpp) on whatever Vulkan device is
# found -- lavapipe in CI: tests/gpu_timer.cpp says what it checks.
set -uo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_gpu_timer"
[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }
exec "$BIN"
