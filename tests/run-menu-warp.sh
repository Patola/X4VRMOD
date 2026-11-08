#!/usr/bin/env bash
# The full-screen menu warp, offline: the arithmetic without a GPU, with the
# embedded shaders checked against the .spv beside their GLSL, then the pass on
# a GPU under validation.
set -u
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
fails=0

run() {
    local label="$1"
    shift
    local out
    out=$("$@" 2>&1)
    if [[ $? -eq 0 ]]; then
        printf 'ok   %-42s %s\n' "$label" \
            "$(grep -c '^ok' <<<"$out") case(s)"
    else
        printf 'FAIL %-42s\n' "$label"
        grep -E '^(FAIL|skip)' <<<"$out" | sed 's/^/       /'
        fails=$((fails + 1))
    fi
}

run "arithmetic, push block, embedded shaders" \
    "$BUILD/tests/x4vr_test_menu_warp_math" \
    "$ROOT/layer/menu_warp.vert.spv" "$ROOT/layer/menu_warp.frag.spv"
run "the pass on a GPU" "$BUILD/tests/x4vr_test_menu_warp_render"
run "the pass in the composite, on a GPU" "$BUILD/tests/x4vr_test_sbs_menu_warp"

exit $((fails > 0))
