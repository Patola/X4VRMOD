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
SF="$BUILD/tests/sample.frag.spv"

[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

fails=0
# $1 label, $2 LAYERS_IMPLIED, $3 LAYER0_DRAWN, $4 LAYER1_DRAWN, rest: env
run_case() {
    local label="$1" wl="$2" w0="$3" w1="$4"; shift 4
    local out
    out=$(env "$@" X4VR_LOG= "$BIN" "$VS" "$FS" "$SF" 2>&1)
    local got_l got_0 got_1 got_id
    got_l=$(sed -n 's/^LAYERS_IMPLIED=//p' <<<"$out")
    got_0=$(sed -n 's/^LAYER0_DRAWN=//p' <<<"$out")
    got_1=$(sed -n 's/^LAYER1_DRAWN=//p' <<<"$out")
    got_id=$(sed -n 's/^LAYERS_IDENTICAL=//p' <<<"$out")
    local got_s
    got_s=$(sed -n 's/^SAMPLED_NONZERO=//p' <<<"$out")
    if [[ "$got_l" == "$wl" && "$got_0" == "$w0" && "$got_1" == "$w1" ]]; then
        printf 'ok   %-38s layers=%s drawn=%s/%s identical=%s sampled=%s\n' \
            "$label" "$got_l" "$got_0" "$got_1" "$got_id" "${got_s:-?}"
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

# The read path, which until now had no instrument except itself.
#
# The last case is the one that matters: layer 0 is left undrawn and the only
# content anywhere is in layer 1, so SAMPLED_NONZERO=1 can only happen if the
# gate-2 redirect really did point the descriptor at layer 1. Reading layer 0
# would come back black and the case would fail.
run_case "redirect to layer 1" 2 1 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_MV_PRESENT_LAYER=1"

run_case "redirect to layer 1, only layer 1 drawn" 2 0 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_MV_MASK=2" "X4VR_MV_PRESENT_LAYER=1"


# The readback instrument, checked against ground truth the same run already
# establishes independently. It exists to answer one question in the game --
# are the two layers the same bytes -- so it must be shown to say "differ"
# when they demonstrably differ, not merely "identical" when they match.
probe_case() {
    local label="$1" want="$2"; shift 2
    local out verdict
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 X4VR_MV_PROBE=1 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    # Matched by word, not by end-of-line: a DIFFER line now carries the
    # texel count and location after the verdict.
    verdict=$(grep -o 'IDENTICAL\|DIFFER' <<<"$out" | head -1)
    local own size
    own=$(sed -n 's/^LAYERS_IDENTICAL=//p' <<<"$out")
    # The probe must hash the whole attachment. It once copied a fixed 64x64
    # patch, which in X4 is blank most frames, so both sides hashed empty and
    # every capture agreed with itself. Asserting the reported extent is what
    # makes that regression visible here instead of in a live run.
    size=$(sed -n 's/.*mv probe: img #[0-9]* \([0-9]*x[0-9]*\) .*/\1/p' <<<"$out")
    if [[ "$size" != "128x128" ]]; then
        printf 'FAIL %-38s probe covered %s, want the full 128x128\n' \
            "$label" "${size:-?}"
        fails=$((fails + 1))
        return
    fi
    # Cross-check: the probe and the test's own readback must never disagree.
    local expect_own; [[ "$want" == IDENTICAL ]] && expect_own=1 || expect_own=0
    if [[ "$verdict" == "$want" && "$own" == "$expect_own" ]]; then
        printf 'ok   %-38s probe=%s own=%s\n' "$label" "$verdict" "$own"
    else
        printf 'FAIL %-38s want probe=%s own=%s, got probe=%s own=%s\n' \
            "$label" "$want" "$expect_own" "${verdict:-?}" "${own:-?}"
        fails=$((fails + 1))
    fi
}

probe_case "probe: layers match"  IDENTICAL
probe_case "probe: layers differ" DIFFER "X4VR_MV_MASK=2"


# The stereo vertex patch: one draw, one module, two different eyes.
#
# This is the mechanism stage 2 rests on, and it is worth proving here for
# the same reason the draw-replication case was: in the game it would be one
# more black-frame candidate among many, and here it is a second.
#
# Column-major identity, and identity with a large x translation in column 3
# (element 12). The shift is half of clip space, so the two layers cannot
# coincidentally agree.
ID="1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1"
SHIFTED="1,0,0,0, 0,1,0,0, 0,0,1,0, 1.0,0,0,1"

# The test's shader declares no descriptor set, so it classifies as UI --
# hence the _UI knobs. That is also why the UI right-eye override exists at
# all: in the game the UI stays mono deliberately.

# The MONO patch, which stage 2 restructured and nothing was covering.
#
# Phase 3b and 4a both rest on the single-matrix path, and it is tagged and
# proven -- but proven by live runs, not by anything that runs in a second.
# When patch_vertex_clip grew the per-view branch, every case in this file
# still passed while setting no clip matrix at all, so the mono path went
# through a rewrite untested.
#
# A translation of 10 in clip x puts the full-screen triangle entirely
# outside [-1,1], so a working patch draws NOTHING. That is the point: a
# patch that silently did nothing would leave the triangle covering the
# screen and the case would fail. Asserting "still renders" could not tell
# those apart.
OFFSCREEN="1,0,0,0, 0,1,0,0, 0,0,1,0, 10.0,0,0,1"
run_case "mono patch applies (draws nothing)" 2 0 0 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_CLIP_K_UI=$OFFSCREEN"

# And the same matrix left in place must still reach both views identically,
# so the mono path cannot regress into an accidental per-view one.
run_case "mono patch is view-independent" 2 1 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_CLIP_K_UI=$ID"

# Must-pass: same matrix both eyes. Proves the patched module still renders,
# that gl_ViewIndex is readable, and that reading it changes nothing when the
# two matrices agree. A patch that corrupted the module fails here.
run_case "stereo patch, same K both eyes" 2 1 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_CLIP_K_UI=$ID" "X4VR_CLIP_K_UI_RIGHT=$ID"

# Must-fail-for-the-right-reason: differing matrices must make the layers
# differ. If gl_ViewIndex always read 0 -- the exact failure this whole
# mechanism risks -- both layers would still be drawn and identical, and this
# case is what catches it.
probe_case "stereo patch, per-eye K differs" DIFFER \
    "X4VR_CLIP_K_UI=$ID" "X4VR_CLIP_K_UI_RIGHT=$SHIFTED"


# The uniformity annotation, which every probe verdict now leans on.
#
# "Both layers agree" is evidence of nothing unless the layers held something.
# #101 cost an investigation to exactly this: three of its four agreeing
# captures were the image cleared to byte 0x10, and because the only test was
# "is any byte non-zero", a cleared mask counted as content whose two layers
# happened to match -- so the image read as mysteriously intermittent while
# behaving perfectly normally.
#
# The third case is the one with teeth. The first two would still pass if the
# probe annotated *everything* as uniform, which is the failure that would make
# every real capture look trivial and every mono target look fine.
ann_of() { # $1 output, $2 layer index -> zero | uniform | content
    local seg
    seg=$(grep -o "layer$2=[0-9a-f]*\( ([^)]*)\)\?" <<<"$1" | head -1)
    case "$seg" in
        *"(all zero)"*) echo zero ;;
        *"(uniform "*)  echo uniform ;;
        *)              echo content ;;
    esac
}

ann_case() {
    local label="$1" want0="$2" want1="$3"; shift 3
    local out g0 g1
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 X4VR_MV_PROBE=1 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    g0=$(ann_of "$out" 0); g1=$(ann_of "$out" 1)
    if [[ "$g0" == "$want0" && "$g1" == "$want1" ]]; then
        printf 'ok   %-38s layer0=%s layer1=%s\n' "$label" "$g0" "$g1"
    else
        printf 'FAIL %-38s want layer0=%s layer1=%s, got layer0=%s layer1=%s\n' \
            "$label" "$want0" "$want1" "$g0" "$g1"
        fails=$((fails + 1))
    fi
}

# A solid fullscreen triangle covers every texel with one value, so a probe
# that cannot say "uniform" would call this content that agrees.
ann_case "uniform layer is named uniform" uniform uniform

# All-zero keeps the name it always had, so existing logs and doc references
# still read the same.
ann_case "all-zero keeps its own name" zero uniform "X4VR_MV_MASK=2"

# And the negative: two distinct values in layer 1 must NOT be annotated.
ann_case "real content is not called uniform" uniform content \
    "X4VR_CLIP_K_UI=$ID" "X4VR_CLIP_K_UI_RIGHT=$SHIFTED"

echo
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
echo "all cases passed"
