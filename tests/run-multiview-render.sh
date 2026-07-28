#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Does a real draw through the layer reach BOTH array layers?
#
# This exists because the in-game symptom (layer 1 black) had too many
# candidate causes to chase one live run at a time. It reproduces X4's shape
# in miniature -- doubled colour target, masked pass, pipeline built against
# that pass, one draw -- then reads both layers back. If the mechanism were
# broken it would be broken here, in a second, with no game involved.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_multiview_render"
VS="$BUILD/tests/fullscreen.vert.spv"
FS="$BUILD/tests/solid.frag.spv"

[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

fails=0
# $1 label, $2 LAYERS_IMPLIED, $3 LAYER0_DRAWN, $4 LAYER1_DRAWN, rest: env
run_case() {
    local label="$1" wl="$2" w0="$3" w1="$4"; shift 4
    local out
    out=$(env "$@" X4VR_LOG= "$BIN" "$VS" "$FS" 2>&1)
    local got_l got_0 got_1 got_id
    got_l=$(sed -n 's/^LAYERS_IMPLIED=//p' <<<"$out")
    got_0=$(sed -n 's/^LAYER0_DRAWN=//p' <<<"$out")
    got_1=$(sed -n 's/^LAYER1_DRAWN=//p' <<<"$out")
    got_id=$(sed -n 's/^LAYERS_IDENTICAL=//p' <<<"$out")
    if [[ "$got_l" == "$wl" && "$got_0" == "$w0" && "$got_1" == "$w1" ]]; then
        printf 'ok   %-38s layers=%s drawn=%s/%s identical=%s\n' \
            "$label" "$got_l" "$got_0" "$got_1" "$got_id"
    else
        printf 'FAIL %-38s want layers=%s drawn=%s/%s, got layers=%s drawn=%s/%s\n' \
            "$label" "$wl" "$w0" "$w1" "${got_l:-?}" "${got_0:-?}" "${got_1:-?}"
        sed 's/^/       | /' <<<"$out" | head -10
        fails=$((fails + 1))
    fi
}

# Control: with doubling off there is no second layer to draw into, and the
# test must say so rather than quietly re-reading the first.
run_case "X4VR_MV=0 (control)" 1 1 absent \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=0"

# The claim: one draw, both layers, no per-view code anywhere.
run_case "X4VR_MV=1 (draw replicates)" 2 1 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1"

# X4VR_MV_MASK=2 before it is used to interpret a live run.
#
# The mask is about to be the sole evidence in a game test whose only other
# instrument has already been wrong twice, so what it does has to be pinned
# down somewhere the answer is not in doubt. Layer 0 must come back *undrawn*
# and layer 1 drawn -- the exact inverse of the case above. If the knob did
# nothing, this case would look identical to the one before it and pass by
# accident, which is why layer 0 is now asserted rather than merely printed.
run_case "X4VR_MV_MASK=2 (layer 1 only)" 2 0 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_MV_MASK=2"

echo
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
echo "all cases passed"
