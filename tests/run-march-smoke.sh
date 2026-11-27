#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The march through the layer (after take 486): tests/march_smoke.cpp plays X4 -- its fog
# walk dispatched, its fog blend drawn, in one submission -- with X4VR_FOG_PROBE=march, and
# the validation layer below ours when it is installed. Wanted: the layer swapped in its
# march, dispatched it at its own size, patched the blend, put its one-time setup first;
# the blended picture finite and not X4's grid alone; and no validation error.
# Control: without X4VR_FOG_PROBE=march the picture is X4's grid alone (OURS=0).
set -uo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_march_smoke"
[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }
ARGS=("$BUILD/tests/sample_volume_walk.comp.spv" "$BUILD/tests/fullscreen.vert.spv" "$BUILD/tests/sample_fog_apply.frag.spv")
LAYERS=VK_LAYER_X4VR_core
VAL=0
if ls /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json >/dev/null 2>&1; then
    LAYERS=VK_LAYER_X4VR_core:VK_LAYER_KHRONOS_validation
    VAL=1
fi
LOG="$(mktemp)"
fails=0
# With the stereo table patch, as every take runs: it rewrites the blend's lookup of
# X4's volume to "39 + eye * offset", and take 487's blend patch, run after it, found no
# element 39 and patched nothing (the scene black).
BL=(X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1)
out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" VK_INSTANCE_LAYERS="$LAYERS" X4VR_POSE=1 X4VR_STEREO=1 \
      "${BL[@]}" X4VR_FOG_PROBE=march X4VR_LOG="$LOG" "$BIN" "${ARGS[@]}" 2>&1)
dev=$(sed -n 's/^DEVICE=//p' <<<"$out")
check() { # label, condition-command
    if eval "$2"; then printf 'ok   %-38s %s\n' "$1" "$dev"; else
        printf 'FAIL %-38s %s\n' "$1" "$dev"; fails=$((fails + 1)); fi
}
check "march: the walk replaced" "grep -q 'replaced by our march' '$LOG'"
check "march: the blend patched" "grep -q \"now reads our march's volume\" '$LOG'"
check "march: the stereo patch ran too" "grep -q 'patched fragment shader #1 (index + ViewIndex' '$LOG'"
check "march: setup in front" "grep -q 'one-time setup goes in front' '$LOG'"
check "march: its GPU stopwatch on" "grep -q 'its GPU stopwatch is on' '$LOG'"
check "march: no setup warning" "! grep -q 'fog march: WARNING' '$LOG'"
check "march: picture finite" "grep -q '^FINITE=1' <<<\"\$out\""
check "march: the blend reads ours" "grep -q '^OURS=1' <<<\"\$out\""
if [[ $VAL == 1 ]]; then
    check "march: no validation error" "! grep -qiE 'Validation Error|VUID-' <<<\"\$out\""
fi
grep -iE 'Validation Error' <<<"$out" | head -5 | sed 's/^/       | /'
# X4's grid holding NaN: in A and B X4's walk does not run, and its volume holds what it
# last held, or never-written memory when the first phase is ours. The blend must take ours
# alone, not X4's times zero: NaN times zero is NaN.
outn=$(env VK_ADD_LAYER_PATH="$BUILD/layer" VK_INSTANCE_LAYERS="$LAYERS" X4VR_POSE=1 X4VR_STEREO=1 \
       "${BL[@]}" X4VR_FOG_PROBE=march X4VR_LOG= SMOKE_GRID=nan "$BIN" "${ARGS[@]}" 2>&1)
check "march: X4's grid NaN, ours finite" "grep -q '^FINITE=1' <<<\"\$outn\""
# Take 492 (The Void): X4 skips its fog walk where there is no fog, and so our march, but
# still draws its blend. Three frames, presented: the walk and blend, then the blend alone
# twice. Frame 3 must be no fog at all (the stand-in's 0.3, no spread), not the last
# march's picture fixed to the screen; one clear, after frame 2's present.
LOG3="$(mktemp)"
out3=$(env VK_ADD_LAYER_PATH="$BUILD/layer" VK_INSTANCE_LAYERS="$LAYERS" X4VR_POSE=1 X4VR_STEREO=1 \
       "${BL[@]}" X4VR_FOG_PROBE=march X4VR_LOG="$LOG3" SMOKE_FRAMES=3 "$BIN" "${ARGS[@]}" 2>&1)
check "march: frame 1 ours (three frames)" "grep -q '^OURS=1' <<<\"\$out3\""
check "march: no march, then no fog" "awk -F= '/^F3_MEAN=/{m=\$2} /^F3_SPREAD=/{s=\$2} END{exit !(m>0.298 && m<0.302 && s+0==0)}' <<<\"\$out3\""
check "march: cleared once" "[ \"\$(grep -c 'cleared to no fog' '$LOG3')\" = 1 ]"
if [[ $VAL == 1 ]]; then
    check "march: three frames, no validation error" "! grep -qiE 'Validation Error|VUID-' <<<\"\$out3\""
fi
grep -E '^F3_(MEAN|SPREAD)|^FAIL' <<<"$out3" | sed 's/^/       3 frames | /'
rm -f "$LOG3"
# The player's choice (rc11): X4VR_NEBULA=volumetric alone -- no measurement knob -- is the
# march; =vanilla is X4's own.
LOGN="$(mktemp)"
outn1=$(env VK_ADD_LAYER_PATH="$BUILD/layer" VK_INSTANCE_LAYERS="$LAYERS" X4VR_POSE=1 X4VR_STEREO=1 \
        "${BL[@]}" X4VR_NEBULA=volumetric X4VR_LOG="$LOGN" "$BIN" "${ARGS[@]}" 2>&1)
check "nebula volumetric: the walk replaced" "grep -q 'replaced by our march' '$LOGN'"
check "nebula volumetric: the blend reads ours" "grep -q '^OURS=1' <<<\"\$outn1\""
outn0=$(env VK_ADD_LAYER_PATH="$BUILD/layer" VK_INSTANCE_LAYERS=VK_LAYER_X4VR_core X4VR_POSE=1 X4VR_STEREO=1 \
        "${BL[@]}" X4VR_NEBULA=vanilla X4VR_LOG= "$BIN" "${ARGS[@]}" 2>&1)
check "nebula vanilla: X4's grid alone" "grep -q '^OURS=0' <<<\"\$outn0\""
rm -f "$LOGN"
# Control: X4's grid alone.
out0=$(env VK_ADD_LAYER_PATH="$BUILD/layer" VK_INSTANCE_LAYERS=VK_LAYER_X4VR_core X4VR_POSE=1 X4VR_STEREO=1 \
       "${BL[@]}" X4VR_LOG= "$BIN" "${ARGS[@]}" 2>&1)
check "march off: X4's grid alone" "grep -q '^OURS=0' <<<\"\$out0\""
grep -E '^(MEAN|FAIL)' <<<"$out0" | sed 's/^/       off | /'
sed -n 's/^\(MEAN\|SPREAD\)=/       \1=/p' <<<"$out"
rm -f "$LOG"
exit $((fails > 0))
