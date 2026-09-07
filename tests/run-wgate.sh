#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Task #82's w-gate, ON A REAL GPU. Does the gate SELECT, or does it just run?
#
# X4 draws screen-space chrome and world-anchored object markers through the
# SAME vertex module, differing only in the per-draw M_worldviewprojection, so
# no per-module predicate can split them. An orthographic UI quad emerges with
# w == 1; a perspective-projected world point gets w = view depth. The gate
# applies the canvas map only when |w - 1| < eps.
#
# Two shaders identical but for w, and a K that translates far off screen, so
# "the gate fired" and "the gate stayed silent" differ as DRAWN vs NOT DRAWN --
# a boolean the harness can see. Both unpatched cases draw, which is the
# control: without it, "gated w=1 does not draw" could be the shader rather
# than the gate.
#
# This exists because takes 263, 264 and 265 were spent on instrument bugs
# that a harness run would have caught in a second. See
# docs/x4-quirks.md and [[x4vr-headless-gpu-harness]].
set -uo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_multiview_render"
PATCH="$BUILD/tests/x4vr_test_wgate_patch"
FS="$BUILD/tests/solid.frag.spv"
SF="$BUILD/tests/sample.frag.spv"
[[ -x "$BIN" && -x "$PATCH" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0
check() {  # label  module  want_drawn
    local label="$1" mod="$2" want="$3" out got
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
              VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
              X4VR_MV=1 X4VR_LOG= "$BIN" "$mod" "$FS" "$SF" 2>&1)
    got=$(sed -n 's/^LAYER0_DRAWN=//p' <<<"$out")
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-34s drawn=%s\n' "$label" "$got"
    else
        printf 'FAIL %-34s want drawn=%s got %s\n' "$label" "$want" "${got:-?}"
        sed 's/^/       | /' <<<"$out" | head -6
        fails=$((fails + 1))
    fi
}

for w in 1 5; do
    src="$ROOT/tests/wgate_w${w}.vert.spv"
    "$PATCH" "$src" "$TMP/w${w}.gated.spv" 0.02         || { echo "patch failed"; exit 1; }
    "$PATCH" "$src" "$TMP/w${w}.w1.spv" 0.02 w1         || { echo "patch failed"; exit 1; }
    if command -v spirv-val >/dev/null; then
        spirv-val "$TMP/w${w}.gated.spv" >/dev/null 2>&1 \
            && printf 'ok   %-34s\n' "w=$w gated module validates" \
            || { printf 'FAIL %-34s\n' "w=$w gated module INVALID"; fails=$((fails+1)); }
    fi
done

# The controls first: unpatched, both must draw. If either of these fails the
# rest of the table means nothing.
check "CONTROL unpatched w=1"        "$ROOT/tests/wgate_w1.vert.spv" 1
check "CONTROL unpatched w=5"        "$ROOT/tests/wgate_w5.vert.spv" 1
# DEFAULT POLARITY: scale the PERSPECTIVE draws. Take 266 established that
# X4's cockpit HUD is the perspective population and the object markers are
# the orthographic one, by running the opposite polarity and getting the exact
# inverse of the intended result in the headset.
check "perspective-gate w=1 (ortho)"  "$TMP/w1.gated.spv"            1
check "perspective-gate w=5 (persp)"  "$TMP/w5.gated.spv"            0
# TAKE 266's POLARITY, kept so the bisect knob is covered and so the two are
# visibly each other's inverse rather than one being "the" behaviour.
check "w1-gate w=1 (take 266)"        "$TMP/w1.w1.spv"               0
check "w1-gate w=5 (take 266)"        "$TMP/w5.w1.spv"               1

[[ $fails -eq 0 ]] && echo "all cases passed" || echo "$fails case(s) failed"
exit $((fails ? 1 : 0))
