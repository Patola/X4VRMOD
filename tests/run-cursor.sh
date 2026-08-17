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

# Task #40: the canvas is no longer only a translation. Under the off-axis
# affine it scales too -- 1.2892 in x and 1.1931 in y for the measured Quest 3
# frusta, the numbers tests/view_math.cpp pins -- and the pointer has to move
# by the map applied to the HOTSPOT while keeping its own pixel size. A quad
# that stretched with the map would blur, and one that ignored the scale would
# sit further from the button the further the pointer is from centre, which is
# the failure that looks like "the canvas is nearly right".
#
# The hotspot is at x=40 of 96 and y=20 of 64, both off centre on purpose: a
# scale applied about the centre moves nothing at the centre, so a case posed
# there would pass with the scale silently dropped.
run "overlay follows a canted canvas" \
    env X4VR_TEST_CANVAS_PX=8 X4VR_TEST_CANVAS_AX=1.2892 \
    X4VR_TEST_CANVAS_AY=1.1931 "$BUILD/tests/x4vr_test_cursor_render"

# The scale alone, with no translation: separates "the map is applied" from
# "the translation is applied", which the case above cannot on its own.
run "overlay follows a scale-only canvas" \
    env X4VR_TEST_CANVAS_PX=0 X4VR_TEST_CANVAS_AX=1.2892 \
    X4VR_TEST_CANVAS_AY=1.1931 "$BUILD/tests/x4vr_test_cursor_render"

if [[ $fails -eq 0 ]]; then
    echo; echo "all cases passed"
else
    echo; echo "$fails suite(s) FAILED"
fi
exit $((fails > 0))
