#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Gate 0 of docs/phase4b-test-plan.md — does the layer double the frame, and
# is the result valid Vulkan?
#
# The observable is the image's *memory requirement*, not its create info: the
# layer grows the image behind the application's back, so the app's own copy
# still says one layer. Asking the driver how much memory it wants is the only
# way to see what was really created.
#
# Needs VK_LAYER_KHRONOS_validation installed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_multiview_double"

[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

VAL=VK_LAYER_KHRONOS_validation
BOTH="VK_LAYER_X4VR_core,$VAL"
fails=0

# $1 label, $2 expected layer count, $3 expect clean|errors, rest: env
run_case() {
    local label="$1" want_layers="$2" want="$3"; shift 3
    local out errs layers got=clean
    out=$(env "$@" X4VR_LOG= "$BIN" 2>&1)
    errs=$(grep -c "Validation Error" <<<"$out")
    layers=$(sed -n 's/^LAYERS_IMPLIED=//p' <<<"$out" | head -1)
    (( errs > 0 )) && got=errors
    local ok=1
    [[ "$got" == "$want" ]] || ok=0
    [[ "${layers:-0}" == "$want_layers" ]] || ok=0
    grep -q "^FRAMEBUFFER=ok" <<<"$out" || ok=0
    grep -q "^REMAINING_2D_VIEW=ok" <<<"$out" || ok=0
    if (( ok )); then
        printf 'ok   %-42s layers=%s %s\n' "$label" "${layers:-?}" "$got"
    else
        printf 'FAIL %-42s want layers=%s %s, got layers=%s %s (%d errs)\n' \
            "$label" "$want_layers" "$want" "${layers:-?}" "$got" "$errs"
        sed 's/^/       | /' <<<"$out" | head -14
        fails=$((fails + 1))
    fi
}

# The control, and the reason the other case means anything: with the feature
# off, the very same program must produce a one-layer image and still be
# valid. If this ever reports 2, the layer is doubling regardless of its own
# switch and every "ok" below is meaningless.
run_case "X4VR_MV unset (control)" 1 clean \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=$BOTH"

run_case "X4VR_MV=0 (explicit off)" 1 clean \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=$BOTH" \
    "X4VR_MV=0"

# The change under test: doubled image, masked pass, substituted array view,
# and a framebuffer validation accepts.
run_case "X4VR_MV=1 (doubling on)" 2 clean \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=$BOTH" \
    "X4VR_MV=1"

echo
if (( fails )); then
    echo "$fails case(s) failed"
    exit 1
fi
echo "all cases passed"
