#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Differential test: does the layer really enable multiview on X4's device?
#
# X4 declares Vulkan 1.2 and leaves the multiview feature off (measured:
# `multiview: X4 requests it? ext=0 feature=0`). Every multiview render pass
# the mod will create is invalid unless the layer turns it on at device
# creation -- and passing a feature struct is not the same as the feature
# being live, so this proves it the only way that counts: by creating a
# two-view render pass and letting the validation layer judge.
#
# Four cases. The first and last MUST fail; that is what makes the middle two
# meaningful rather than a test that cannot fail.
#
# Needs VK_LAYER_KHRONOS_validation installed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_multiview_enable"

[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

fails=0

# Runs the test, prints the verdict, and checks it against what we expect.
# $1 = label, $2 = expected "clean" or "errors", rest = env assignments.
run_case() {
    local label="$1" expect="$2"; shift 2
    local out errs
    out=$(env "$@" X4VR_LOG= "$BIN" 2>&1)
    errs=$(grep -c "Validation Error" <<<"$out")
    local got=clean
    (( errs > 0 )) && got=errors
    if [[ "$got" == "$expect" ]]; then
        printf 'ok   %-46s %s\n' "$label" "$got"
    else
        printf 'FAIL %-46s expected %s, got %s (%d)\n' \
            "$label" "$expect" "$got" "$errs"
        sed 's/^/       | /' <<<"$out" | head -12
        fails=$((fails + 1))
    fi
}

VAL=VK_LAYER_KHRONOS_validation
BOTH="VK_LAYER_X4VR_core,$VAL"

# Without the layer the feature is off and validation must object. If this
# case ever comes back clean the test has stopped proving anything -- either
# validation is missing or something else already enabled multiview.
run_case "no layer (control)"            errors \
    "VK_LOADER_LAYERS_ENABLE=$VAL"

# The two paths through enable_multiview(): a chain with nothing to flip, and
# a chain where X4 already supplies a 1.1 feature struct with multiview off.
run_case "layer, empty chain (prepend)"  clean \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=$BOTH"
run_case "layer, existing struct (flip)" clean \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=$BOTH" \
    "X4VR_MV_TEST_CHAIN=1"

# The escape hatch must actually reach the device, not just the log.
run_case "X4VR_MULTIVIEW=0 (opt out)"    errors \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=$BOTH" \
    "X4VR_MULTIVIEW=0"

echo
if (( fails )); then
    echo "$fails case(s) failed"
    exit 1
fi
echo "all cases passed"
