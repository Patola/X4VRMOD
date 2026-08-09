#!/usr/bin/env bash
# The cursor overlay, offline: placement arithmetic without a GPU, then the
# real thing on one under validation.
#
# The shifted case exists because of task #30. Without it the suite would only
# ever exercise canvas_shift = 0, where every per-layer assertion passes
# whether the shift is applied or silently dropped -- the two are the same
# picture. A case that is never run is not a check, so it runs here rather than
# living in a comment as an invocation someone might type.
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

# The generated header must match the .spv it came from, or the layer draws
# yesterday's shader -- so the paths are passed rather than letting it skip.
# They are in the source tree, not the build dir: the .spv are committed
# alongside the GLSL because a layer cannot compile shaders at load time.
run "placement, formats, knob semantics" \
    "$BUILD/tests/x4vr_test_cursor_place" \
    "$ROOT/layer/cursor_quad.vert.spv" "$ROOT/layer/cursor_quad.frag.spv"

# No canvas: both layers must come out byte-identical. This is the shape every
# take up to 96 ran.
run "overlay on a GPU, no canvas" \
    env X4VR_TEST_CANVAS_PX=0 "$BUILD/tests/x4vr_test_cursor_render"

# With a canvas: the pointer follows the UI, +8 px in the left eye and -8 px in
# the right. 8 is chosen so the two quads (4 px wide) cannot overlap -- at 1 px
# apart "translated" and "smeared" would look the same to the comparison.
run "overlay follows an 8 px canvas" \
    env X4VR_TEST_CANVAS_PX=8 "$BUILD/tests/x4vr_test_cursor_render"

if [[ $fails -eq 0 ]]; then
    echo; echo "all cases passed"
else
    echo; echo "$fails suite(s) FAILED"
fi
exit $((fails > 0))
