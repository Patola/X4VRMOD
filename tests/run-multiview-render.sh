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

ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
    #
    # Anchored to the probe's own lines, and -w so a longer word cannot match.
    # Both are repairs, not tidying: this took the first IDENTICAL-or-DIFFER
    # anywhere in the output, and a new layer log line containing the word
    # "DIFFERENT" made every match land on it instead of on the probe. The
    # verdict of a specific instrument has to be read off that instrument.
    verdict=$(grep 'mv probe:' <<<"$out" | grep -ow 'IDENTICAL\|DIFFER' | head -1)
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
# All five vertex-patch cases below carry X4VR_SHEAR_NODEPTH=1, and without it
# they test nothing.
#
# Two independent classifications decide whether K reaches a draw:
#
#   the *module*  -> classify() says World or NonWorld, picking K_world or
#                    K_nonworld. This harness's fullscreen.vert has no camera
#                    block, so it is always NonWorld and takes K_nonworld.
#   the *pass*    -> needs_original() returns classify_unsheared()[subpass], and
#                    an unsheared pass binds the UNPATCHED module, whatever the
#                    module was patched with.
#
# Since take 71 a colour pass with no depth is unsheared, and every pass this
# harness renders is that shape. So the patched module was built, logged
# ("patched vertex shader #1 (ui)"), and then thrown away at pipeline creation
# ("unsheared pipeline: using unpatched modules"). K_nonworld was inert, the
# triangle never moved, and three of these cases had been failing ever since --
# not because the patch broke, but because nothing was binding it.
#
# X4VR_SHEAR_NODEPTH=1 is the knob that disables exactly that exclusion, so the
# pass becomes STEREO and the patched module is bound. It is the only way to
# reach patch_vertex_clip from this harness, since it renders no pass with depth.
OFFSCREEN="1,0,0,0, 0,1,0,0, 0,0,1,0, 10.0,0,0,1"
run_case "mono patch applies (draws nothing)" 2 0 0 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_SHEAR_NODEPTH=1" "X4VR_CLIP_K_NONWORLD=$OFFSCREEN"

# And the same matrix left in place must still reach both views identically,
# so the mono path cannot regress into an accidental per-view one.
run_case "mono patch is view-independent" 2 1 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_SHEAR_NODEPTH=1" "X4VR_CLIP_K_NONWORLD=$ID"

# Must-pass: same matrix both eyes. Proves the patched module still renders,
# that gl_ViewIndex is readable, and that reading it changes nothing when the
# two matrices agree. A patch that corrupted the module fails here.
run_case "stereo patch, same K both eyes" 2 1 1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "X4VR_MV=1" "X4VR_SHEAR_NODEPTH=1" "X4VR_CLIP_K_NONWORLD=$ID" "X4VR_CLIP_K_NONWORLD_RIGHT=$ID"

# Must-fail-for-the-right-reason: differing matrices must make the layers
# differ. If gl_ViewIndex always read 0 -- the exact failure this whole
# mechanism risks -- both layers would still be drawn and identical, and this
# case is what catches it.
probe_case "stereo patch, per-eye K differs" DIFFER \
    "X4VR_SHEAR_NODEPTH=1" "X4VR_CLIP_K_NONWORLD=$ID" "X4VR_CLIP_K_NONWORLD_RIGHT=$SHIFTED"


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
    "X4VR_SHEAR_NODEPTH=1" "X4VR_CLIP_K_NONWORLD=$ID" "X4VR_CLIP_K_NONWORLD_RIGHT=$SHIFTED"


# The predicate split: "does K apply?" and "does this replicate?" used to be
# one question, and are no longer.
#
# rp #1 in this test is a single colour attachment, LDR, consuming the per-eye
# chain. The assertion is the whole line -- MONO (no K) and +MASKED
# (replicates) at once -- because either half alone would pass while the other
# regressed.
#
# What this pass demonstrates changed at take 71. It was written as "X4's
# tonemap in miniature", testing that masking keys on the SRGB format rather
# than on LDR in general. That carve-out still exists but no longer decides
# anything here: take 71 made every colour-with-no-depth subpass per-eye
# unconditionally, which subsumes it. On real X4 the SRGB resolve is
# `rp #55.0: 1 colour [76H] no-depth -> MONO (fullscreen post)
# +MASKED(fullscreen)` -- so X4VR_MASK_TONEMAP does not decide that pass in the
# game either. The carve-out is tested where it is still the deciding rule, in
# the probe cases below, which have depth.
mask_case() {
    local label="$1" want="$2"; shift 2
    local out cls fb
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 X4VR_MV_INVENTORY=1 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    # Read the verdict, not one reason string. This used to match the literal
    # "MONO (all-LDR/UI)", so when take 71 started reporting the same pass as
    # "MONO (fullscreen post)" every case fell through to `sheared` and the
    # failure looked like a shear regression -- which is the one thing that had
    # not happened. The reason text is the classifier's business; what this
    # assertion is about is MONO-vs-STEREO and masked-vs-not.
    if grep -qE 'rp #1\.0:.*-> MONO \(.*\) \+MASKED' <<<"$out"; then
        cls=masked
    elif grep -qE 'rp #1\.0:.*-> MONO \(' <<<"$out"; then
        cls=mono
    else
        cls=sheared   # the regression this split exists to prevent
    fi
    grep -q 'fb  rp #1:.*MASKED' <<<"$out" && fb=masked || fb=mono
    # A pass classified masked whose framebuffer is not is a fallback wearing a
    # disguise, so the two signals are asserted together rather than either
    # one alone.
    if [[ "$cls" == "$want" && "$fb" == "$want" ]] && ! grep -q FALLBACK <<<"$out"; then
        printf 'ok   %-38s rp=%s fb=%s\n' "$label" "$cls" "$fb"
    else
        printf 'FAIL %-38s want %s, got rp=%s fb=%s%s\n' "$label" "$want" \
            "$cls" "$fb" "$(grep -q FALLBACK <<<"$out" && echo ' (FALLBACK!)')"
        fails=$((fails + 1))
    fi
}

# These four used to read:
#
#   mask_case "tonemap masks when SRGB"      masked SRGB=1 MASK_TONEMAP=1
#   mask_case "...but not without the knob"  mono   SRGB=1
#   mask_case "...and not for UNORM LDR"     mono   MASK_TONEMAP=1
#   mask_case "LDR pass unmasked by default" mono
#
# and had been failing since take 71, which added an unconditional clause to
# classify_per_eye(): a subpass with colour attachments and **no depth** is
# always per-eye, with no knob to turn it off, because unmasking those would
# leave layer 1 with no lighting at all.
#
# rp #1 -- every pass this harness renders, in fact -- is exactly that shape.
# So `mono` became unreachable, and the three cases asserting it were demanding
# an outcome no input this file can construct will produce. They were not
# detecting a regression; they were describing a layer that no longer exists.
#
# What is actually worth asserting about rp #1 is the take-71 rule itself, and
# that it is *unconditional* -- the knobs must not move it.
mask_case "fullscreen pass masks, SRGB+knob"  masked "X4VR_TEST_OUT_SRGB=1" "X4VR_MASK_TONEMAP=1"
mask_case "...and without the knob"           masked "X4VR_TEST_OUT_SRGB=1"
mask_case "...and for UNORM LDR"              masked "X4VR_MASK_TONEMAP=1"
mask_case "...and by default: no knob undoes it" masked


# The rest of the predicate matrix, which the rendered passes cannot reach.
#
# classify_unsheared()/classify_per_eye() are pure functions of the create-info,
# so a render pass that is never executed still exercises them completely. The
# harness creates four such probes as rp #2..#5 (see multiview_render.cpp) to
# cover the shapes it cannot render. There is no framebuffer to cross-check
# against here, which is why the executed pass above keeps its paired rp+fb
# assertion -- this is the weaker check, used only where the stronger one
# cannot exist.
classify_case() {
    local label="$1" serial="$2" want="$3"
    local out got
    out=$(env X4VR_LOG= X4VR_MV=1 X4VR_MV_INVENTORY=1 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    got=$(grep -oE "rp #$serial\.0:.*" <<<"$out" | head -1)
    got="${got#*-> }"
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s %s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want "%s", got "%s"\n' "$label" "$want" "$got"
        fails=$((fails + 1))
    fi
}

classify_case "colour, no depth: always masked" 2 \
    "MONO (fullscreen post) +MASKED(fullscreen) +CANVAS"
# The carve-out the four cases above were written to test. It is reachable --
# it just needs depth, which is what stops the take-71 clause from firing.
classify_case "LDR+depth is UI: unsheared, unmasked" 3 "MONO (all-LDR/UI)"
classify_case "HDR+depth is world: sheared" 4 "STEREO (world)"
# The load-bearing one. A depth-only pass must stay MONO *and* unmasked: X4's
# five cascaded shadow maps are rendered by passes of this shape, and both eyes
# have to sample the same light-space map. classify_per_eye() guards its
# fullscreen clause on `colorAttachmentCount > 0` for exactly this reason.
#
# If this case ever reports +MASKED, the shadow maps have gone per-eye and the
# per-eye shading defect that took takes 56-83 to find is back.
classify_case "depth-only stays mono and unmasked" 5 "MONO (depth-only/shadow)"

# Task #30's discriminating pair. These two passes are the same shape to every
# other predicate in the layer -- one colour attachment, no depth, masked as a
# fullscreen post pass -- and differ only in format. rp #2 is LDR and is the
# canvas; rp #6 is HDR and must not be, or X4's 29 deferred/post passes would
# take the UI's constant shift the moment their modules classify World (which
# under X4VR_SHEAR_LIGHTS the light volumes do).
#
# The assertion is the *whole* verdict string, so "+CANVAS appeared on the HDR
# pass" fails here rather than turning into a lighting bug nobody attributes to
# this commit.
classify_case "HDR without depth is not a canvas" 6 \
    "MONO (fullscreen post) +MASKED(fullscreen)"


# The fragment patch: the *sample* follows the view index.
#
# Everything above is about the write path. This is the read path, and it is a
# separate mechanism because of one asymmetry: a masked pass view-indexes its
# subpass inputs by itself, and never its samplers. A descriptor set is bound
# once for the whole pass and has no per-view form, so both views read array
# layer 0 and draw the same picture -- which is exactly what X4's tonemap does
# to #103.
#
# The setup makes the source's two layers maximally distinguishable without
# involving the vertex patch at all: X4VR_MV_MASK=2 draws only into layer 1, so
# layer 0 is the black clear and layer 1 is the shader's red. Keeping the vertex
# patch out of it matters -- a sheared vertex shader would move the second pass's
# own triangle and make the two output layers differ for a reason that has
# nothing to do with sampling, and the control case would stop controlling
# anything.
PATCHDIR="$(mktemp -d)"
trap 'rm -rf "$PATCHDIR"' EXIT
PATCHER="$BUILD/tests/x4vr_test_spirv_patch"

frag_case() {
    local label="$1" want1="$2" wantd="$3" shader="$4" arr="$5"; shift 5
    local out g1 gd
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 X4VR_TEST_OUT_SRGB=1 \
        X4VR_MASK_TONEMAP=1 "X4VR_TEST_ARRAY_SAMPLER=$arr" \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "${CASE_VS:-$VS}" "$FS" "$shader" 2>&1)
    g1=$(sed -n 's/^OUT1_NONZERO=//p' <<<"$out")
    gd=$(sed -n 's/^OUT_DIFFER=//p' <<<"$out")
    if [[ "$g1" == "$want1" && "$gd" == "$wantd" ]]; then
        printf 'ok   %-38s out1=%s differ=%s\n' "$label" "$g1" "$gd"
    else
        printf 'FAIL %-38s want out1=%s differ=%s, got out1=%s differ=%s\n' \
            "$label" "$want1" "$wantd" "${g1:-?}" "${gd:-?}"
        sed 's/^/       | /' <<<"$out" | grep -E '^\s+\| (OUT|LAYER|SAMPLED|FAIL)' | head -8
        fails=$((fails + 1))
    fi
}

if ! "$PATCHER" frag-view-layer "$SF" "$PATCHDIR/sample_arr.spv" 0 0 |
        grep -q PATCHED=1; then
    printf 'FAIL %-38s patcher refused the sample shader\n' "frag patch: setup"
    fails=$((fails + 1))
fi

# Must-pass. Only layer 1 of the source was ever drawn, so a view that lands on
# layer 1 comes back red and a view that lands on layer 0 comes back black.
frag_case "frag patch: view 1 reads layer 1" 1 1 \
    "$PATCHDIR/sample_arr.spv" 1 "X4VR_MV_MASK=2"

# The control, and the fact it establishes: without the patch the very same
# frame reads layer 0 in both views. This is the asymmetry stated as a
# measurement rather than as a claim -- the draw replicated (LAYER1_DRAWN=1
# above) and the sample did not follow it.
frag_case "...unpatched reads layer 0 in both" 0 0 \
    "$SF" 0 "X4VR_MV_MASK=2"

# Must-not-invent. Both layers drawn, identical content, patch still applied:
# the output layers have to come back the same. A patch that offset the
# coordinate, sampled out of range, or returned the view index itself would
# make them differ here and pass the case above for the wrong reason.
frag_case "frag patch invents no difference"  1 0 \
    "$PATCHDIR/sample_arr.spv" 1


# The patch as a pure transform, checked without a GPU in the way.
#
# The cases above prove the patched shader does the right thing on this driver.
# These prove the module it produces is well formed and that the refusals are
# refusals -- a different question, and the one a validator can answer. Both are
# needed: a module can sample the right layer and still be accepted only by
# accident, and the next driver is under no obligation to repeat the accident.
patch_case() {
    local label="$1" want="$2" shader="$3" set="$4" bind="$5"
    local out got
    out=$("$PATCHER" frag-view-layer "$BUILD/tests/$shader" \
        "$PATCHDIR/out.spv" "$set" "$bind" 2>&1)
    got=$(sed -n 's/^PATCHED=//p' <<<"$out")
    local val=skip
    if [[ "$got" == 1 ]]; then
        spirv-val --target-env vulkan1.2 "$PATCHDIR/out.spv" >/dev/null 2>&1 && val=OK || val=BAD
    fi
    if [[ "$got" == "$want" && "$val" != BAD ]]; then
        printf 'ok   %-38s patched=%s val=%s\n' "$label" "$got" "$val"
    else
        printf 'FAIL %-38s want patched=%s, got patched=%s val=%s\n' \
            "$label" "$want" "${got:-?}" "$val"
        fails=$((fails + 1))
    fi
}

# The same, for the index-offset transform. Same shape, different subcommand,
# so a refusal rule can be pinned without a GPU.
idx_case() {
    local label="$1" want="$2" shader="$3" set="$4" bind="$5"
    local out got
    out=$("$PATCHER" frag-index-offset "$BUILD/tests/$shader" \
        "$PATCHDIR/out.spv" "$set" "$bind" 26653 2>&1)
    got=$(sed -n 's/^PATCHED=//p' <<<"$out")
    local val=skip
    if [[ "$got" == 1 ]]; then
        spirv-val --target-env vulkan1.2 "$PATCHDIR/out.spv" >/dev/null 2>&1 && val=OK || val=BAD
    fi
    if [[ "$got" == "$want" && "$val" != BAD ]]; then
        printf 'ok   %-38s patched=%s val=%s\n' "$label" "$got" "$val"
    else
        printf 'FAIL %-38s want patched=%s, got patched=%s val=%s\n' \
            "$label" "$want" "${got:-?}" "$val"
        fails=$((fails + 1))
    fi
}

if ! command -v spirv-val >/dev/null; then
    echo "note: spirv-val not found, module structure is unchecked"
fi

# Both read forms in one module -- texture() and texelFetch() -- so a patch that
# handles one and walks past the other cannot pass.
patch_case "patch: two textures, one patched" 1 sample_two.frag.spv 0 0

# The refusals. Each is a shader the patch would silently break:
#   - a shadow sampler is not among the doubled images, so view 1 would read a
#     layer that does not exist. This is the one with history behind it.
#   - textureSize() returns ivec2 for a 2D image and ivec3 for an array, so
#     promoting the type changes the result of an instruction the patch never
#     looked at.
#   - a binding that is not there, and a module with no fragment entry point,
#     must both come back untouched rather than half-edited.
#
# What mutation testing said about these two, because it is not what it looks
# like and re-deriving it costs an afternoon:
#
#   The textureSize case pins the unknown-use rule, but only since the shader
#   was given a real sample as well. With just the query in it the module was
#   refused as "declared but never read", and the case passed with the rule
#   deleted.
#
#   The shadow case pins nothing in particular -- it is over-determined. The
#   depth-type guard, the unknown-use rule and the never-read rule each refuse
#   it on their own, and deleting all three still leaves it refused, because
#   GLSL cannot express a shadow-sampler read that is not a Dref op in the
#   first place. So it asserts the outcome, which is the thing that matters
#   here, and no single guard should be justified by it.
patch_case "patch refuses shadow samplers"   0 sample_shadow.frag.spv 0 0
patch_case "patch refuses textureSize"       0 sample_size.frag.spv   0 0
patch_case "patch refuses a missing binding" 0 sample.frag.spv        0 1
patch_case "patch refuses a vertex module"   0 fullscreen.vert.spv    0 0

# The bindless table, which is what X4 actually has and what killed the
# type-promotion approach. Promoting the element type promotes every entry, and
# only a handful of images are doubled -- so this refusal is not a nicety.
patch_case "patch refuses a bindless table" 0 sample_bindless.frag.spv 0 7


# The index offset, on X4's actual shape -- the one the abandoned transform had
# to refuse. sample_bindless.frag is hand-written to match: a 64-entry
# texture2D array at set 0 binding 7, indexed by a uint from a uniform block.
idx_case "index offset patches a bindless table" 1 sample_bindless.frag.spv 0 7
# A plain texture has no index to offset, and accepting one here would quietly
# resurrect the type-promotion approach this replaced.
idx_case "...refuses a plain texture"            0 sample.frag.spv 0 0
idx_case "...refuses a vertex-only module"       0 fullscreen.vert.spv 0 7
idx_case "...refuses a binding that is not there" 0 sample_bindless.frag.spv 0 3
idx_case "...patches aliased vars on one binding" 1 sample_alias_binding.frag.spv 0 5

# Take forty-eight, as an assertion rather than a story. Two variables share
# (set 0, binding 5) -- X4's shape in 228 of 409 modules. The patch must offset
# EACH of them EXACTLY ONCE. The old code stopped at the first variable, so the
# caller's second call to the same binding offset it a second time, to
# index + 2*26653 = 53307 in a 53306-element array. That read as zeros, and it
# was the black right eye.
#
# Counting OpIAdd is the check: one per variable. Three means something got
# offset twice; one means a variable was left on view 0's slot.
alias_case() {
    local label="$1" want="$2"
    if ! command -v spirv-dis >/dev/null; then
        printf 'skip %-38s (spirv-dis not found)\n' "$label"
        return
    fi
    "$PATCHER" frag-index-offset \
        "$BUILD/tests/sample_alias_binding.frag.spv" "$PATCHDIR/alias.spv" \
        0 5 26653 >/dev/null 2>&1
    local got
    got=$(spirv-dis "$PATCHDIR/alias.spv" 2>/dev/null | grep -c 'OpIAdd')
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s OpIAdd=%s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want OpIAdd=%s, got %s\n' "$label" "$want" "$got"
        fails=$((fails + 1))
    fi
}
alias_case "...offsets each aliased var exactly once" 2

# The same module must not be patchable twice: the transform is not idempotent,
# which is precisely why the layer now dedupes by (set, binding) before calling
# it. If this ever stops double-offsetting, the caller's guard can be revisited
# -- until then it is load-bearing and this records why.
if command -v spirv-dis >/dev/null; then
    "$PATCHER" frag-index-offset "$PATCHDIR/alias.spv" "$PATCHDIR/alias2.spv" \
        0 5 26653 >/dev/null 2>&1
    twice=$(spirv-dis "$PATCHDIR/alias2.spv" 2>/dev/null | grep -c 'OpIAdd')
    if [[ "$twice" == 4 ]]; then
        printf 'ok   %-38s OpIAdd=%s (guard is load-bearing)\n' \
            "...double-patch still doubles" "$twice"
    else
        printf 'FAIL %-38s want OpIAdd=4, got %s\n' \
            "...double-patch still doubles" "$twice"
        fails=$((fails + 1))
    fi
fi
# The eye offset read from the camera block instead of baked (task #23).
#
# sx moves 33x under zoom, so there is no constant to bake; the shader has to
# read X4's live projection. These cases pin the two halves of that: the patch
# must fire on a module shaped like X4's world shaders, and it must *refuse*
# on anything else rather than patch the wrong buffer -- a wrong (set, binding)
# still produces a module that validates and renders nonsense, which is the
# failure mode worth spending cases on.
eye_case() {
    local label="$1" want="$2" shader="$3" set="$4" bind="$5" member="${6:-1}"
    local out got val=skip
    out=$("$PATCHER" vert-eye-offset "$BUILD/tests/$shader" \
        "$PATCHDIR/eye.spv" "$set" "$bind" "$member" -0.008 0.008 2>&1)
    got=$(sed -n 's/^PATCHED=//p' <<<"$out")
    if [[ "$got" == 1 ]]; then
        spirv-val --target-env vulkan1.2 "$PATCHDIR/eye.spv" >/dev/null 2>&1 && val=OK || val=BAD
    fi
    if [[ "$got" == "$want" && "$val" != BAD ]]; then
        printf 'ok   %-38s patched=%s val=%s\n' "$label" "$got" "$val"
    else
        printf 'FAIL %-38s want patched=%s, got patched=%s val=%s\n' \
            "$label" "$want" "${got:-?}" "$val"
        fails=$((fails + 1))
    fi
}
eye_case "eye: reads sx from the camera block" 1 sample_camera_block.vert.spv 1 0
eye_case "eye: refuses, no camera block"       0 fullscreen.vert.spv 1 0
eye_case "eye: refuses, wrong set"             0 sample_camera_block.vert.spv 2 0
eye_case "eye: refuses, wrong binding"         0 sample_camera_block.vert.spv 1 7
# Member 99 is off the end of the struct. The patch verifies the member is a
# mat4 before touching anything, so this must refuse -- an unchecked index
# would emit an access chain into whatever follows and validate happily.
eye_case "eye: refuses, member out of range"   0 sample_camera_block.vert.spv 1 0 99

# What the patched module actually says, not merely that it validates. One
# OpCompositeInsert per return, writing component 0 -- the shear moves clip x
# and nothing else, so a patch that touched y or w would still validate and
# would still be wrong.
if command -v spirv-dis >/dev/null; then
    "$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/eye.spv" 1 0 1 -0.008 0.008 >/dev/null 2>&1
    ins=$(spirv-dis "$PATCHDIR/eye.spv" 2>/dev/null | grep -c 'OpCompositeInsert')
    vidx=$(spirv-dis "$PATCHDIR/eye.spv" 2>/dev/null | grep -c 'BuiltIn ViewIndex')
    if [[ "$ins" == 1 && "$vidx" == 1 ]]; then
        printf 'ok   %-38s insert=%s viewindex=%s\n' \
            "...one insert, one ViewIndex" "$ins" "$vidx"
    else
        printf 'FAIL %-38s want insert=1 viewindex=1, got %s/%s\n' \
            "...one insert, one ViewIndex" "$ins" "$vidx"
        fails=$((fails + 1))
    fi
    # Mono: no second eye, so no gl_ViewIndex and no MultiView capability. The
    # layer uses this path for the unsheared twin, where an unused builtin
    # would be a needless capability on a module that never needs it.
    "$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/eyemono.spv" 1 0 1 -0.008 >/dev/null 2>&1
    mvidx=$(spirv-dis "$PATCHDIR/eyemono.spv" 2>/dev/null | grep -c 'BuiltIn ViewIndex')
    mcap=$(spirv-dis "$PATCHDIR/eyemono.spv" 2>/dev/null | grep -c 'OpCapability MultiView')
    if [[ "$mvidx" == 0 && "$mcap" == 0 ]]; then
        printf 'ok   %-38s viewindex=%s cap=%s\n' \
            "...mono form takes no ViewIndex" "$mvidx" "$mcap"
    else
        printf 'FAIL %-38s want 0/0, got %s/%s\n' \
            "...mono form takes no ViewIndex" "$mvidx" "$mcap"
        fails=$((fails + 1))
    fi
fi

# The off-axis affine, task #35: X4 renders one symmetric frustum per eye and a
# headset has none, so clip x and y are remapped onto the runtime's canted one.
#
# The angles below are the measured Quest 3 / WiVRn frusta from take 112 --
# L-54 R+40 U+44 D-55 for eye 0 and the mirror for eye 1 -- and they are used
# here rather than round numbers so the constants this emits can be checked
# against the ones tests/view_math.cpp already pins for the same input.
OA_L="-54,40,44,-55"
OA_R="-40,54,44,-55"
# atan(1)*4/180 * 55 -> a symmetric target at the union half-angle. Its map is
# the IDENTITY when X4 is rendering at that field, which is the negative
# control the run itself will use.
OA_SYM="-55,55,55,-55"

oa_case() {
    local label="$1" want="$2" mode="$3" shader="$4" set="$5" bind="$6" \
          member="$7"; shift 7
    local out got val=skip
    out=$("$PATCHER" "$mode" "$BUILD/tests/$shader" "$PATCHDIR/oa.spv" \
        "$set" "$bind" "$member" -0.032 0.032 "$@" 2>&1)
    got=$(sed -n 's/^PATCHED=//p' <<<"$out")
    # A refusal that already edited the module is the dangerous kind: the
    # caller falls back believing the bytes are untouched. The patcher checks
    # that itself and prints FAIL=refusal_modified_code, so catch it here.
    if grep -q '^FAIL=' <<<"$out"; then
        printf 'FAIL %-38s %s\n' "$label" "$(grep '^FAIL=' <<<"$out")"
        fails=$((fails + 1))
        return
    fi
    if [[ "$got" == 1 ]] && command -v spirv-val >/dev/null; then
        spirv-val --target-env vulkan1.2 "$PATCHDIR/oa.spv" >/dev/null 2>&1 \
            && val=OK || val=BAD
    fi
    if [[ "$got" == "$want" && "$val" != BAD ]]; then
        printf 'ok   %-38s patched=%s val=%s\n' "$label" "$got" "$val"
    else
        printf 'FAIL %-38s want patched=%s, got patched=%s val=%s\n' \
            "$label" "$want" "${got:-?}" "$val"
        fails=$((fails + 1))
    fi
}
oa_case "offaxis: camera block, per-eye"   1 vert-eye-offset \
    sample_camera_block.vert.spv 1 0 1 "$OA_L" "$OA_R"
oa_case "offaxis: camera block, one map"   1 vert-eye-offset \
    sample_camera_block.vert.spv 1 0 1 "$OA_SYM"
oa_case "offaxis: mvp form, per-eye"       1 vert-eye-offset-mvp \
    sample_light_volume.vert.spv 1 0 7 "$OA_L" "$OA_R"
# tan_r <= tan_l is a frustum with no width. make_off_axis returns !ok and the
# patch must decline the module rather than render it symmetric -- a shader
# that quietly ignores the cant looks exactly like a headset problem.
oa_case "offaxis: refuses a backwards frustum" 0 vert-eye-offset \
    sample_camera_block.vert.spv 1 0 1 "40,-54,44,-55"
oa_case "offaxis: refuses garbage angles"      0 vert-eye-offset \
    sample_camera_block.vert.spv 1 0 1 "not,a,frustum,x"

if command -v spirv-dis >/dev/null; then
    # What the affine actually says. Two inserts, because it writes clip x AND
    # clip y -- the shear alone writes only x, and the single-insert case above
    # is what pins that. One OpLogicalAnd, which is the degenerate-scale guard
    # and therefore a marker for "the affine is present at all".
    "$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/oa.spv" 1 0 1 -0.032 0.032 "$OA_L" "$OA_R" >/dev/null 2>&1
    ins=$(spirv-dis "$PATCHDIR/oa.spv" 2>/dev/null | grep -c 'OpCompositeInsert')
    andc=$(spirv-dis "$PATCHDIR/oa.spv" 2>/dev/null | grep -c 'OpLogicalAnd')
    if [[ "$ins" == 2 && "$andc" == 1 ]]; then
        printf 'ok   %-38s insert=%s guard=%s\n' \
            "...writes x and y, guarded once" "$ins" "$andc"
    else
        printf 'FAIL %-38s want insert=2 guard=1, got %s/%s\n' \
            "...writes x and y, guarded once" "$ins" "$andc"
        fails=$((fails + 1))
    fi

    # The affine must be ABSENT when it is not asked for. This is the property
    # that lets every state tagged before #35 be reproduced by leaving the knob
    # unset, and it is asserted rather than assumed: the emission is threaded
    # through the same OpReturn handler as the shear, so a stray unconditional
    # line there would be invisible in any other case in this file.
    "$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/noaa.spv" 1 0 1 -0.032 0.032 >/dev/null 2>&1
    nand=$(spirv-dis "$PATCHDIR/noaa.spv" 2>/dev/null | grep -c 'OpLogicalAnd')
    ndiv=$(spirv-dis "$PATCHDIR/noaa.spv" 2>/dev/null | grep -c 'OpFDiv')
    if [[ "$nand" == 0 && "$ndiv" == 0 ]]; then
        printf 'ok   %-38s and=%s div=%s\n' \
            "...absent when not requested" "$nand" "$ndiv"
    else
        printf 'FAIL %-38s want and=0 div=0, got %s/%s\n' \
            "...absent when not requested" "$nand" "$ndiv"
        fails=$((fails + 1))
    fi

    # The coefficients themselves, against the numbers tests/view_math.cpp
    # pins for these same frusta. This is what catches a swapped x/y, a
    # dropped tan(), or a sign read off the wrong end of the frustum -- all of
    # which produce a module that validates perfectly and renders wrong.
    #
    # ax_num = 2/(tan40 - tan(-54)) = 0.902738, bx = +0.242513 for eye 0, and
    # the eye-1 constant is emitted as a DIFFERENCE, so bx's mirror shows up as
    # -0.485025 rather than -0.242513.
    #
    # Disassembled ONCE into a variable, not piped into five greps. `grep -q`
    # exits at the first match and closes the pipe, spirv-dis takes SIGPIPE,
    # and under `set -o pipefail` the pipeline then reports failure even though
    # the constant was found -- so the verdict depended on whether the module
    # was small enough for spirv-dis to have finished writing. This case passed
    # for that reason alone; the #39 one below, on a larger module, failed all
    # five. An assertion whose answer turns on process timing is not one.
    dis=$(spirv-dis "$PATCHDIR/oa.spv" 2>/dev/null)
    miss=""
    for c in 0.902738 0.242512 -0.835478 -0.193187 -0.485025; do
        [[ "$dis" == *"OpConstant %float $c"* ]] || miss="$miss $c"
    done
    if [[ -z "$miss" ]]; then
        printf 'ok   %-38s all five match view_math\n' \
            "...coefficients are make_off_axis's"
    else
        printf 'FAIL %-38s missing:%s\n' \
            "...coefficients are make_off_axis's" "$miss"
        fails=$((fails + 1))
    fi

    # The mvp form recovers |sy| from row 1 and negates it, because the sign is
    # not in the block. One OpFNegate is that decision; its absence would flip
    # the image vertically in exactly the twelve modules nobody would think to
    # check, and only in those.
    "$PATCHER" vert-eye-offset-mvp "$BUILD/tests/sample_light_volume.vert.spv" \
        "$PATCHDIR/oamvp.spv" 1 0 7 -0.032 0.032 "$OA_L" "$OA_R" >/dev/null 2>&1
    neg=$(spirv-dis "$PATCHDIR/oamvp.spv" 2>/dev/null | grep -c 'OpFNegate')
    sqrt=$(spirv-dis "$PATCHDIR/oamvp.spv" 2>/dev/null | grep -c ' Sqrt ')
    if [[ "$neg" == 1 && "$sqrt" == 2 ]]; then
        printf 'ok   %-38s negate=%s sqrt=%s\n' \
            "...mvp recovers sy as -|sy|" "$neg" "$sqrt"
    else
        printf 'FAIL %-38s want negate=1 sqrt=2, got %s/%s\n' \
            "...mvp recovers sy as -|sy|" "$neg" "$sqrt"
        fails=$((fails + 1))
    fi
fi

# The per-eye M_invprojection correction (task #22). The deferred passes
# reconstruct position from depth in the *eye's* frame and then light it with
# centre-frame shadow matrices, which puts the two eyes' shadows on different
# surface points. Shadows are view-independent, so that is a defect.
inv_case() {
    local label="$1" want="$2" shader="$3" set="$4" bind="$5" member="${6:-2}"
    # Anything past the member is the task #39 off-axis pair; callers that stop
    # at five arguments (member defaulted) must not have shift fail on them.
    shift $(( $# < 6 ? $# : 6 ))
    local out got val=skip
    out=$("$PATCHER" frag-invproj "$BUILD/tests/$shader" \
        "$PATCHDIR/inv.spv" "$set" "$bind" "$member" -0.032 0.032 "$@" 2>&1)
    got=$(sed -n 's/^PATCHED=//p' <<<"$out")
    if [[ "$got" == 1 ]]; then
        spirv-val --target-env vulkan1.2 "$PATCHDIR/inv.spv" >/dev/null 2>&1 && val=OK || val=BAD
    fi
    if [[ "$got" == "$want" && "$val" != BAD ]]; then
        printf 'ok   %-38s patched=%s val=%s\n' "$label" "$got" "$val"
    else
        printf 'FAIL %-38s want patched=%s, got patched=%s val=%s\n' \
            "$label" "$want" "${got:-?}" "$val"
        fails=$((fails + 1))
    fi
}
inv_case "invproj: corrects the reconstruction" 1 sample_invproj.frag.spv 1 0
inv_case "invproj: refuses, wrong set"          0 sample_invproj.frag.spv 2 0
inv_case "invproj: refuses, wrong binding"      0 sample_invproj.frag.spv 1 9
inv_case "invproj: refuses, member out of range" 0 sample_invproj.frag.spv 1 0 99
# Member 1 is M_projection, a mat4 the shader never loads. The patch must find
# no load to rewrite and refuse, rather than "succeeding" by doing nothing.
inv_case "invproj: refuses, member never loaded" 0 sample_invproj.frag.spv 1 0 1
# Compute has no gl_ViewIndex at all, so there is no d to select. Refusing is
# the honest outcome; picking one eye would be wrong in the other.
inv_case "invproj: refuses a compute module"    0 noop.comp.spv 1 0

# Task #39: the same correction, with the off-axis affine composed onto it.
# The refusals have to match the vertex side's exactly -- a frustum accepted
# here and declined there would leave the map applied nowhere and undone
# everywhere, which is a worse state than not attempting it.
inv_case "invproj: affine, per-eye"     1 sample_invproj.frag.spv 1 0 2 \
    "$OA_L" "$OA_R"
inv_case "invproj: affine, one map"     1 sample_invproj.frag.spv 1 0 2 "$OA_L"
inv_case "invproj: affine, symmetric"   1 sample_invproj.frag.spv 1 0 2 \
    "$OA_SYM"
inv_case "invproj: affine refuses a backwards frustum" 0 \
    sample_invproj.frag.spv 1 0 2 "40,-54,44,-55"
inv_case "invproj: affine refuses garbage angles"      0 \
    sample_invproj.frag.spv 1 0 2 "not,a,frustum,x"

if command -v spirv-dis >/dev/null; then
    "$PATCHER" frag-invproj "$BUILD/tests/sample_invproj.frag.spv" \
        "$PATCHDIR/inv.spv" 1 0 2 -0.032 0.032 >/dev/null 2>&1
    ins=$(spirv-dis "$PATCHDIR/inv.spv" 2>/dev/null | grep -c 'OpCompositeInsert')
    flat=$(spirv-dis "$PATCHDIR/inv.spv" 2>/dev/null | grep -c 'Flat')
    vidx=$(spirv-dis "$PATCHDIR/inv.spv" 2>/dev/null | grep -c 'BuiltIn ViewIndex')
    # Four inserts: row 0 of each of the four columns. One ViewIndex, and it
    # must be Flat -- an integer fragment input that is interpolated is invalid
    # and some drivers accept it anyway, which is the worst combination.
    if [[ "$ins" == 4 && "$vidx" == 1 && "$flat" -ge 1 ]]; then
        printf 'ok   %-38s inserts=%s viewindex=%s flat=%s\n' \
            "...four inserts, ViewIndex is Flat" "$ins" "$vidx" "$flat"
    else
        printf 'FAIL %-38s want inserts=4 viewindex=1 flat>=1, got %s/%s/%s\n' \
            "...four inserts, ViewIndex is Flat" "$ins" "$vidx" "$flat"
        fails=$((fails + 1))
    fi

    # Task #39's shape, stated as what the affine ADDS. Counting absolutely
    # would bake in the sample shader's own arithmetic -- it already divides
    # once, for its perspective divide -- and would then fail the day that
    # shader changes for an unrelated reason.
    #
    # Twelve more inserts: columns 0, 1 and 3 of each of the four rows, on top
    # of task #22's four. One guard. Exactly two divides, the reciprocals of
    # A_x and A_y; a third would mean a scale is being recomputed where it
    # should have been reused.
    "$PATCHER" frag-invproj "$BUILD/tests/sample_invproj.frag.spv" \
        "$PATCHDIR/invoa.spv" 1 0 2 -0.032 0.032 "$OA_L" "$OA_R" >/dev/null 2>&1
    disoa=$(spirv-dis "$PATCHDIR/invoa.spv" 2>/dev/null)
    disno=$(spirv-dis "$PATCHDIR/inv.spv" 2>/dev/null)
    cnt() { grep -c "$1" <<<"$2"; }
    dins=$(( $(cnt OpCompositeInsert "$disoa") - $(cnt OpCompositeInsert "$disno") ))
    dand=$(( $(cnt OpLogicalAnd "$disoa") - $(cnt OpLogicalAnd "$disno") ))
    ddiv=$(( $(cnt OpFDiv "$disoa") - $(cnt OpFDiv "$disno") ))
    if [[ "$dins" == 12 && "$dand" == 1 && "$ddiv" == 2 ]]; then
        printf 'ok   %-38s +inserts=%s +guard=%s +div=%s\n' \
            "...affine composes onto the inverse" "$dins" "$dand" "$ddiv"
    else
        printf 'FAIL %-38s want +12/+1/+2, got %s/%s/%s\n' \
            "...affine composes onto the inverse" "$dins" "$dand" "$ddiv"
        fails=$((fails + 1))
    fi

    # Absent when not asked for. Same property, same reason, as the vertex
    # side: every state tagged before #39 has to remain reachable by leaving
    # the knob unset, and the emission threads through the same load handler
    # the #22 correction uses, where a stray unconditional line would show up
    # in no other case in this file. Compared against the UNPATCHED module, so
    # "absent" means what it says rather than "no more than the sample's own".
    disbase=$(spirv-dis "$BUILD/tests/sample_invproj.frag.spv" 2>/dev/null)
    nand=$(( $(cnt OpLogicalAnd "$disno") - $(cnt OpLogicalAnd "$disbase") ))
    ndiv=$(( $(cnt OpFDiv "$disno") - $(cnt OpFDiv "$disbase") ))
    if [[ "$nand" == 0 && "$ndiv" == 0 ]]; then
        printf 'ok   %-38s +and=%s +div=%s\n' \
            "...affine absent when not requested" "$nand" "$ndiv"
    else
        printf 'FAIL %-38s want +and=0 +div=0, got %s/%s\n' \
            "...affine absent when not requested" "$nand" "$ndiv"
        fails=$((fails + 1))
    fi

    # The coefficients, against the same five numbers view_math pins for these
    # frusta -- the eye-1 bx is emitted as a difference, hence -0.485025.
    miss=""
    for c in 0.902738 0.242512 -0.835478 -0.193187 -0.485025; do
        [[ "$disoa" == *"OpConstant %float $c"* ]] || miss="$miss $c"
    done
    if [[ -z "$miss" ]]; then
        printf 'ok   %-38s all five match view_math\n' \
            "...invproj coefficients are the same"
    else
        printf 'FAIL %-38s missing:%s\n' \
            "...invproj coefficients are the same" "$miss"
        fails=$((fails + 1))
    fi
fi

# What the emitted module actually COMPUTES, which is a different question from
# whether it validates. spirv-val accepts an extract of (1,0) where (1,1) was
# meant, and accepts a subtraction with its operands the wrong way round; both
# render wrong and both have cost this project takes. The checker interprets
# the emitted arithmetic against a known M_invprojection and demands the same
# 16 floats common/x4vr_view.hpp produces.
#
# Seeded faults confirm it has range rather than merely agreeing with itself:
# extracting minv[1][0] instead of minv[1][1] scores 1.163, and swapping the
# operands of the col-3 subtraction scores 2.000, against 0.000 here.
eval_case() {
    local label="$1"; shift
    local out worst checked
    out=$("$PATCHER" frag-invproj-check \
        "$BUILD/tests/sample_invproj.frag.spv" 1 0 2 -0.032 0.032 "$@" 2>&1)
    worst=$(sed -n 's/.*WORST=//p' <<<"$out")
    checked=$(sed -n 's/^CHECKED=\([0-9]*\).*/\1/p' <<<"$out")
    # 10 = five cameras x two views. A checker that silently evaluated nothing
    # would report a perfect zero, so the count is asserted too.
    if [[ "$checked" == 10 ]] && awk "BEGIN{exit !($worst < 1e-6)}" 2>/dev/null
    then
        printf 'ok   %-38s checked=%s worst=%s\n' "$label" "$checked" "$worst"
    else
        printf 'FAIL %-38s want checked=10 worst<1e-6, got %s / %s\n' \
            "$label" "${checked:-?}" "${worst:-?}"
        fails=$((fails + 1))
    fi
}
eval_case "invproj: emits T(d)·M (no affine)"
eval_case "invproj: emits T(d)·M·A⁻¹ per eye" "$OA_L" "$OA_R"
eval_case "invproj: emits the identity map"   "$OA_SYM"

# And the instrument, checked against the same shape. It reported "samples
# nothing" about X4's real shaders until it learned to see through OpTypeArray,
# so the count is asserted and not merely printed: it is the number that decides
# type-promotion is impossible and an index offset is the way.
list_case() {
    local label="$1" want="$2" shader="$3"
    local got
    got=$("$PATCHER" list "$BUILD/tests/$shader" 2>&1 |
        sed -n 's/^TEX set=\([0-9]*\) binding=\([0-9]*\) count=\([0-9]*\).*/\1\/\2 x\3/p' |
        paste -sd' ' -)
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s %s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want "%s", got "%s"\n' "$label" "$want" "${got:-?}"
        fails=$((fails + 1))
    fi
}

list_case "lister sees through a bindless array" "0/7 x64" sample_bindless.frag.spv
list_case "lister reports a plain texture as 1"  "0/0 x1"  sample.frag.spv
list_case "lister finds both of two textures"    "0/0 x1 0/1 x1" sample_two.frag.spv

# Coverage, measured stage-agnostically -- the question "does this module
# declare a table the mirror covers?" is not the question `list` answers.
#
# X4's skybox is a COMPUTE shader sampling the same 53306-entry heap as a CUBE
# array. `list` is fragment-only and 2D-only, so it says TEXTURES=0 about it,
# and a refusal counter built on `list` reported "0 refused" for a whole run --
# reassuring precisely because the instrument was blind. skybox_cube.comp
# reproduces that shape (compute + cube + big array) so the blindness cannot
# come back unnoticed.
survey_case() {
    local label="$1" want="$2" shader="$3" min="${4:-32}"
    local got
    got=$("$PATCHER" survey "$BUILD/tests/$shader" "$min" 2>&1 | head -1)
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s %s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want "%s", got "%s"\n' "$label" "$want" "${got:-?}"
        fails=$((fails + 1))
    fi
}

survey_case "survey sees a compute cube table" \
    "LARGE=1 FRAGMENT=0 COMPUTE=1" skybox_cube.comp.spv
# The same shader, through the instrument that cannot see it. Asserted, not
# assumed: this is the exact gap the counter fell into.
if [[ "$("$PATCHER" list "$BUILD/tests/skybox_cube.comp.spv" 2>&1 | head -1)" == "TEXTURES=0" ]]; then
    printf 'ok   %-38s %s\n' "...which the 2D lister cannot" "TEXTURES=0"
else
    printf 'FAIL %-38s lister unexpectedly saw it\n' "...which the 2D lister cannot"
    fails=$((fails + 1))
fi
survey_case "survey sees a bindless fragment table" \
    "LARGE=1 FRAGMENT=1 COMPUTE=0" sample_bindless.frag.spv
survey_case "survey ignores a small texture" \
    "LARGE=0 FRAGMENT=1 COMPUTE=0" sample.frag.spv


# The two paths the writer list cannot see.
#
# Every instrument in the layer is built around render passes, so a frame stage
# performed by a blit or a dispatch leaves no line anywhere -- and "no pass
# writes #100" is only evidence of a non-draw merge if the non-draw calls are
# actually on record. Both of these were entirely unhooked until the HUD
# question ran out of draws to blame.
inv_case() {
    local label="$1" want="$2"; shift 2
    local out got
    out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$@" X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    got=$(grep -c -- "$want" <<<"$out")
    if [[ "$got" -ge 1 ]]; then
        printf 'ok   %-38s %s\n' "$label" "$want"
    else
        printf 'FAIL %-38s no line matching "%s"\n' "$label" "$want"
        fails=$((fails + 1))
    fi
}

inv_case "transfer inventory names the edge" \
    "xfer #0 -> #2 via vkCmdBlitImage" X4VR_TEST_BLIT=1
inv_case "compute inventory counts a dispatch" \
    "compute — 1 pipeline(s), 1 shader(s) dispatched" X4VR_TEST_COMPUTE=1
# Zero has to be a reading, not an absence: "no dispatches" and "compute is
# unhooked again" produced identical logs for this project's whole history.
inv_case "...and reports zero as a number" \
    "compute — 0 pipeline(s), 0 shader(s) dispatched" X4VR_TEST_COMPUTE=0
inv_case "...zero transfer edges likewise" \
    "image transfers — 0 image->image edge(s)" X4VR_TEST_BLIT=0
# The present-pass join, negative side only. This test creates render passes and
# framebuffers but has no surface, so no image is ever a swapchain image and the
# count must be 0 -- which makes this a no-false-positive check, and nothing
# more. The positive side (a framebuffer over a real swapchain image) cannot be
# reached headless and is first exercised live; see docs/frame-analysis.md,
# "Take twenty-eight".
inv_case "present-pass join does not misfire" \
    "present passes — 0 pass(es) draw into a swapchain image"
# X4VR_MASK_PRESENT keys on finalLayout == PRESENT_SRC_KHR. Getting that test
# backwards would mask every pass in the game, so the check that matters
# offline is that it fires on *nothing* here: this suite presents nothing, so
# no subpass may be classified "PRESENT composite" and the knob must not change
# the masked count. The positive side needs a real swapchain and is first
# exercised live -- see docs/frame-analysis.md, "Take thirty-one".
out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 X4VR_MASK_PRESENT=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1)
base=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1)
if grep -q "+PRESENT-CAND" <<<"$out" && ! grep -q "+PRESENT-CAND" <<<"$base"; then
    printf 'ok   %-38s %s\n' "MASK_PRESENT flags LDR candidates" "gated on the knob"
else
    printf 'FAIL %-38s knob does not gate the candidate flag\n' \
        "MASK_PRESENT flags LDR candidates"
    fails=$((fails + 1))
fi
# The half that still guards a backwards predicate: a world (HDR) pass must
# never be a present candidate, whatever the knob says.
if ! grep -E "STEREO \(world\).*PRESENT-CAND" <<<"$out" >/dev/null; then
    printf 'ok   %-38s %s\n' "...and never flags a world pass" "HDR excluded"
else
    printf 'FAIL %-38s flagged a world pass as presenting\n' \
        "...and never flags a world pass"
    fails=$((fails + 1))
fi
# And "never measured" must not read as "measured zero". The bindless survey
# lost a run to exactly that, so the unmeasured case gets its own assertion.
out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 "$BIN" "$VS" "$FS" "$SF" 2>&1)
if grep -q "not measured (needs X4VR_MV_INVENTORY=1)" <<<"$out" &&
   ! grep -q "image->image edge(s)" <<<"$out"; then
    printf 'ok   %-38s %s\n' "...unmeasured says so, not 0" "not measured"
else
    printf 'FAIL %-38s zero and unmeasured read alike\n' "...unmeasured says so, not 0"
    fails=$((fails + 1))
fi


# The bindless survey, checked against ground truth this test already knows.
#
# Three instruments here have been quietly wrong for at least one run each, so a
# new one gets its first reading verified rather than believed. This test's
# sampled image IS a doubled per-eye target -- the same image every case above
# reads both layers of -- so the survey must name exactly one per-eye slot and
# say which image it is. With doubling off, the same descriptor is written to the
# same slot and the image simply is not per-eye, so the count must be zero: that
# is what separates "the join works" from "the join answers yes to everything".
#
# The `slots` field is asserted alongside, and it is not decoration. The first
# version of this case read the per-eye list only, mapped "no output" to "none",
# and passed vacuously -- the report was gated on X4VR_MV, so with doubling off
# the survey printed nothing at all and "found none" and "never ran" were the
# same string. Requiring the binding line to exist is what makes the zero real.
survey_case() {
    local label="$1" want_slots="$2" want_pe="$3"; shift 3
    local out slots pe
    out=$(env "$@" X4VR_LOG= X4VR_BINDLESS_SURVEY=1 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    # The layout label must be a serial, not "?" -- the binding carries a
    # descriptor array, so a "?" here means the set was never joined to the
    # layout it came from and the count could belong to any table.
    slots=$(sed -n 's/.*bindless final: layout #[0-9]* binding 0 — \([0-9]*\) distinct slots.*/\1/p' \
        <<<"$out" | head -1)
    pe=$(sed -n 's/.*distinct slots, range [0-9]*\.\.[0-9]*, \([0-9]*\) holding.*/\1/p' \
        <<<"$out" | head -1)
    if [[ "$slots" == "$want_slots" && "$pe" == "$want_pe" ]]; then
        printf 'ok   %-38s slots=%s per-eye=%s\n' "$label" "$slots" "$pe"
    else
        printf 'FAIL %-38s want slots=%s per-eye=%s, got slots=%s per-eye=%s\n' \
            "$label" "$want_slots" "$want_pe" "${slots:-ABSENT}" "${pe:-ABSENT}"
        fails=$((fails + 1))
    fi
}

survey_case "survey finds the per-eye slots"   4 4 "X4VR_MV=1"
survey_case "...and none when nothing doubles" 4 0 "X4VR_MV=0"

# The extent of the per-eye set, because take twenty-one printed 26 of 191
# entries with no marker and the shape of the set had to be guessed from an
# arbitrary sample. Count and range are order-independent; the list is not.
survey_pe=$(env X4VR_LOG= X4VR_MV=1 X4VR_BINDLESS_SURVEY=1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 |
    sed -n 's/.*bindless final: layout #[0-9]* binding 0 per-eye slots: //p' |
    head -1)
# ...and the image each slot names, since a count alone would not catch a join
# that credited the wrong image. Every slot here holds the one doubled target.
survey_named=$(grep -o 'img#[0-9]*' <<<"$survey_pe" | sort -u | tr '\n' ' ')
if [[ "$survey_pe" == "4 in 0..3, showing 4: "* &&
      "$survey_named" == "img#0 " ]]; then
    printf 'ok   %-38s %s\n' "survey reports the set's extent" \
        "${survey_pe%%:*}"
else
    printf 'FAIL %-38s want "4 in 0..3, showing 4" naming only img#0, got "%s"\n' \
        "survey reports the set's extent" "$survey_pe"
    fails=$((fails + 1))
fi

# --- step A: the mirror, with nothing but these shaders reading it ------------
#
# P1 offline. The twin element is read by a hardcoded index, so this tests the
# mirror alone and needs none of step B. Only source layer 1 is ever drawn
# (MV_MASK=2), so "the twin holds layer 1" shows up as content and "slot 0 still
# holds layer 0" shows up as an empty target. Both runs have the mirror on, so
# neither reads an unwritten descriptor.
# $1 label, $2 want OUT1_NONZERO, $3 want OUT_DIFFER, $4 mask, $5 shader
mirror_case() {
    local label="$1" want1="$2" wantd="$3" mask="$4" shader="$5"; shift 5
    local out g1 gd
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 "X4VR_MV_MASK=$mask" X4VR_TEST_OUT_SRGB=1 \
        X4VR_MASK_TONEMAP=1 X4VR_BINDLESS_MIRROR=1 X4VR_MIRROR_OFFSET=4 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "${CASE_VS:-$VS}" "$FS" "$shader" 2>&1)
    g1=$(sed -n 's/^OUT1_NONZERO=//p' <<<"$out")
    gd=$(sed -n 's/^OUT_DIFFER=//p' <<<"$out")
    if [[ "$g1" == "$want1" && "$gd" == "$wantd" ]]; then
        printf 'ok   %-38s out1=%s differ=%s\n' "$label" "$g1" "$gd"
    else
        printf 'FAIL %-38s want out1=%s differ=%s, got out1=%s differ=%s\n' \
            "$label" "$want1" "$wantd" "${g1:-?}" "${gd:-?}"
        grep -E "^(OUT|FAIL)|bindless mirror|VUID" <<<"$out" | sed 's/^/       | /' |
            head -8
        fails=$((fails + 1))
    fi
}

# X4VR_MV_MASK is global -- it masks the *second* pass as well, so under mask=2
# output layer 0 is never rendered and OUT_DIFFER=1 says nothing except that.
# Read these two by out1 alone: does the read reach source layer 1 or not.
mirror_case "mirror puts layer 1 in the twin"  1 1 2 "$BUILD/tests/sample_twin.frag.spv"
mirror_case "...and leaves slot 0 on layer 0"  0 0 2 "$BUILD/tests/sample_twin_base.frag.spv"

# --- step B: the whole mechanism, end to end ---------------------------------
#
# One module, one draw, and the two views index *different elements of the same
# table*: view 0 takes slot 0, view 1 takes slot 0 + OFFSET, which the mirror
# filled with a view of layer 1.
if ! "$PATCHER" frag-index-offset "$BUILD/tests/sample_twin_base.frag.spv" \
        "$PATCHDIR/idxoff.spv" 0 0 4 | grep -q PATCHED=1; then
    printf 'FAIL %-38s patcher refused the array shader\n' "index offset: setup"
    fails=$((fails + 1))
fi

# View 1 reaches the twin, where the unpatched control two cases up reads slot 0
# and comes back empty.
mirror_case "index offset: view 1 reads the twin" 1 1 2 "$PATCHDIR/idxoff.spv"

# THE case, and the reason the pair above is not enough on its own. Under mask=2
# a patch that added OFFSET *unconditionally* -- ignoring ViewIndex, breaking the
# left eye -- would look exactly like a correct one, because view 0 is not
# rendered and cannot be seen to be wrong.
#
# So run it again with mask=1: now only view 0 renders, and only source layer 0
# has content. A correct patch leaves view 0 on slot 0 and draws (differ=1,
# layer 0 lit against a blank layer 1). An unconditional offset sends view 0 to
# slot 4 -- source layer 1, never drawn under this mask -- and the whole output
# comes back blank at differ=0.
mirror_case "...and view 0 still reads its own"   0 1 1 "$PATCHDIR/idxoff.spv"

# Take twenty-three, as a case. Everything above passes a *separate* module per
# stage, and X4 ships one module carrying both entry points -- so nothing above
# could exercise what happens when that one module is both vertex-patched and
# fragment-patched. In the game it was, and rp #40 (the composition: masked, and
# unsheared because it draws a fullscreen triangle) takes the unsheared twin for
# every one of its pipelines. That twin was the pristine bytes, so the fragment
# edit went with the shear and #103 was the only image in the frame still
# sampling view 0's slots in both eyes.
#
# sample_combined.spv is fullscreen.vert + sample_twin_base.frag linked into one
# module, and it is passed as BOTH stages. X4VR_CLIP_K_NONWORLD makes the vertex patch
# apply, which is what registers a twin at all; the layer does the fragment patch
# itself rather than it being pre-applied, so the twin's provenance is what is
# under test. With a pristine twin this reads 0/0.
CASE_VS="$BUILD/tests/sample_combined.spv" \
mirror_case "unsheared twin keeps the frag patch" 1 1 2 \
    "$BUILD/tests/sample_combined.spv" \
    X4VR_BINDLESS_PATCH=1 "X4VR_CLIP_K_NONWORLD=$ID"

# The accounting, because the pair above would also pass if the mirror wrote the
# twin by some accident of aliasing. Four written descriptors, four twins, all
# four substituted to layer 1, nothing skipped.
mirror_stat=$(env X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=2 X4VR_TEST_OUT_SRGB=1 \
    X4VR_MASK_TONEMAP=1 X4VR_BINDLESS_MIRROR=1 X4VR_MIRROR_OFFSET=4 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "$BIN" "$VS" "$FS" "$BUILD/tests/sample_twin.frag.spv" 2>&1 |
    sed -n 's/.*bindless mirror final: offset 4, \(.*\)$/\1/p' | head -1)
# Asserted field by field, not as one verbatim string.
#
# The verbatim form broke the moment a counter was added: "0 kept at layer 0 as
# shared (unmasked writers only)" landed between two fields it was checking, and
# both cases reported an accounting failure while every number was correct. A
# test that fails on a *new* counter is reporting the wrong thing.
#
# Field-wise is not the weaker check here. Every field is named and asserted,
# and a field that cannot be found fails rather than defaulting to a pass -- so
# a counter that is removed, renamed, or never runs is still caught, which is
# the property the exact match was there for. The difference is only that adding
# a counter no longer breaks the cases that were not about it.
mirror_acct() { # label line writes descriptors layer1 shared noroom
    local label="$1" line="$2" bad="" f got
    if [[ -z "$line" ]]; then
        printf 'FAIL %-38s got "ABSENT" (no mirror summary line)\n' "$label"
        fails=$((fails + 1)); return
    fi
    set -- "twin writes:$3" "twin descriptors:$4" "of them layer-1:$5" \
           "kept at layer 0 as shared:$6" "skipped for no room:$7"
    for f; do
        got=$(grep -oE "[0-9]+ ${f%:*}" <<<"$line" | head -1 | grep -oE '^[0-9]+')
        if [[ -z "$got" ]]; then
            bad="$bad ${f%:*}=MISSING"
        elif [[ "$got" != "${f##*:}" ]]; then
            bad="$bad ${f%:*}=$got(want ${f##*:})"
        fi
    done
    if [[ -z "$bad" ]]; then
        printf 'ok   %-38s %s\n' "$label" "$line"
    else
        printf 'FAIL %-38s%s\n' "$label" "$bad"
        fails=$((fails + 1))
    fi
}

# One write, four twins, all four substituted to layer 1. Nothing is shared
# here: "shared" means an image whose only writers are unmasked passes, and this
# case has none, so a non-zero would mean the Shared classification is firing
# where it should not.
mirror_acct "mirror accounts for every twin" "$mirror_stat" 1 4 4 0 0

# The bounds check. At the default offset the 8-slot table has no room for a
# twin, so the mirror must decline rather than write off the end -- and must say
# it declined. Uses the non-array shader, which reads only slot 0, so nothing
# reads an unwritten descriptor.
mirror_room=$(env X4VR_LOG= X4VR_MV=1 X4VR_BINDLESS_MIRROR=1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 |
    sed -n 's/.*bindless mirror final: offset 26653, \(.*\)$/\1/p' | head -1)
mirror_acct "mirror declines a table with no room" "$mirror_room" 0 0 0 0 1

# The template path is a blind spot the survey did not watch:
# vkUpdateDescriptorSetWithTemplate is core 1.1, so it needs no extension string
# and its absence from a log proves nothing. This harness never uses a template,
# so the count is always zero -- the assertion is that the *line exists*, so a
# real zero stays distinguishable from a counter that was removed or never ran.
survey_tmpl=$(env X4VR_LOG= X4VR_MV=1 X4VR_BINDLESS_SURVEY=1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 |
    sed -n 's/.*bindless final: \([0-9]* template updates, [0-9]* of them\).*/\1/p' |
    head -1)
if [[ "$survey_tmpl" == "0 template updates, 0 of them" ]]; then
    printf 'ok   %-38s %s\n' "survey measures the template path" "$survey_tmpl"
else
    printf 'FAIL %-38s want "0 template updates, 0 of them", got "%s"\n' \
        "survey measures the template path" "${survey_tmpl:-ABSENT}"
    fails=$((fails + 1))
fi

# The WSI probe must not change what it observes.
#
# The layer hooks vkCreateWaylandSurfaceKHR and friends to record which
# platform built a surface. An app may legitimately pick its backend by asking
# for one of these and treating a null answer as "not available here" -- X4 is
# on exactly that path -- so shadowing one the driver does not have would make
# the game take a different branch *because* we were watching. The guard is a
# differential: the same binary, with the layer on and off, must give the same
# answers.
gipa_off=$(env X4VR_LOG= "$BIN" "$VS" "$FS" "$SF" 2>&1 | grep '^GIPA_')
gipa_on=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" X4VR_LOG= X4VR_MV=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 | grep '^GIPA_')
if [[ -n "$gipa_off" && "$gipa_on" == "$gipa_off" ]]; then
    printf 'ok   %-38s %s\n' "WSI hooks do not perturb gipa" \
        "$(tr '\n' ' ' <<<"$gipa_on")"
else
    printf 'FAIL %-38s layer changes the answer\n  off: %s\n  on:  %s\n' \
        "WSI hooks do not perturb gipa" "${gipa_off:-ABSENT}" \
        "${gipa_on:-ABSENT}"
    fails=$((fails + 1))
fi

# ...and the control, without which the case above passes on a loader that
# answers every name: a function that does not exist must resolve to null.
if grep -qx 'GIPA_vkNoSuchFunctionX4VR=0' <<<"$gipa_on"; then
    printf 'ok   %-38s %s\n' "gipa still returns null for absent fns" \
        "GIPA_vkNoSuchFunctionX4VR=0"
else
    printf 'FAIL %-38s want GIPA_vkNoSuchFunctionX4VR=0, got "%s"\n' \
        "gipa still returns null for absent fns" \
        "$(grep 'NoSuchFunction' <<<"$gipa_on" || echo ABSENT)"
    fails=$((fails + 1))
fi

# The WSIs a process could use are read off the instance, not guessed. This
# harness enables VK_KHR_surface and nothing else, so exactly that must be
# reported -- the point being that the list comes from
# ppEnabledExtensionNames and not from whatever the log's author assumed.
#
# The per-surface platform line (wsi=wayland|xcb|xlib|unknown) cannot be
# exercised here: it needs a real surface, and this test is headless. That
# gap is deliberate and named rather than papered over -- it is only ever
# read from a live run.
inv_case "instance surface extensions are listed" \
    "wsi: instance enables VK_KHR_surface"

# X4VR_MASK_LDR must mask on "every colour attachment is LDR" and nothing else.
#
# Take forty-three: 8 all-LDR/UI passes were masked by no rule while 6 were
# masked as present candidates, and the scene reached the screen through some
# of the eight. The two guards that matter are that the knob fires here (this
# suite builds an LDR pass) and that it never touches an HDR pass -- a
# backwards predicate would mask the world and cost a stereo frame its second
# eye without any log line saying so.
ldr_on=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 X4VR_MASK_LDR=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 | grep -c '+MASKED(ldr)')
ldr_off=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 | grep -c '+MASKED(ldr)')
if [[ "$ldr_on" -ge 1 && "$ldr_off" -eq 0 ]]; then
    printf 'ok   %-38s %s\n' "MASK_LDR masks all-LDR passes" \
        "$ldr_on on, $ldr_off off"
else
    printf 'FAIL %-38s want >=1 on and 0 off, got %s / %s\n' \
        "MASK_LDR masks all-LDR passes" "$ldr_on" "$ldr_off"
    fails=$((fails + 1))
fi

# The negative side, and the one that would cost a run: an HDR colour target
# must never be masked by this rule, whatever else is on.
hdr_masked=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 X4VR_MASK_LDR=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 |
    grep '+MASKED(ldr)' | grep -c 'H\]')
if [[ "$hdr_masked" -eq 0 ]]; then
    printf 'ok   %-38s %s\n' "...and never an HDR pass" "0 HDR passes masked"
else
    printf 'FAIL %-38s %s HDR pass(es) masked by the LDR rule\n' \
        "...and never an HDR pass" "$hdr_masked"
    fails=$((fails + 1))
fi

# Every masked pass names the rule that masked it. "+MASKED(?)" means some
# path set per_eye without any of the three predicates claiming it, which is
# how a fourth rule would arrive unnoticed.
if ! env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_MV_INVENTORY=1 X4VR_MASK_LDR=1 \
    X4VR_MASK_PRESENT=1 X4VR_MASK_TONEMAP=1 \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 | grep -q '+MASKED(?)'; then
    printf 'ok   %-38s %s\n' "every mask names its rule" "no +MASKED(?)"
else
    printf 'FAIL %-38s a pass was masked by no named rule\n' \
        "every mask names its rule"
    fails=$((fails + 1))
fi

echo
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
# --- the widened World predicate (task #22 / P70) ---------------------------
# Pins both readings against committed bytes. The narrow rule must be unchanged
# by the widening -- a module that was World stays World -- and the widening
# must catch camera-positioned geometry without catching a fullscreen triangle,
# which is what the UI is and what the take 33 logo regression was.
cls_case() {
    local label="$1" want_n="$2" want_w="$3" shader="$4"
    local out gn gw
    out=$("$PATCHER" classify "$BUILD/tests/$shader" 2>&1)
    gn=$(sed -n 's/.*NARROW=\([A-Za-z]*\).*/\1/p' <<<"$out")
    gw=$(sed -n 's/.*WIDE=\([A-Za-z]*\).*/\1/p' <<<"$out")
    if [[ "$gn" == "$want_n" && "$gw" == "$want_w" ]]; then
        printf 'ok   %-38s narrow=%s wide=%s\n' "$label" "$gn" "$gw"
    else
        printf 'FAIL %-38s want narrow=%s wide=%s, got narrow=%s wide=%s\n' \
            "$label" "$want_n" "$want_w" "${gn:-?}" "${gw:-?}"
        fails=$((fails + 1))
    fi
}
# Reads M_projection (member 1) from the camera block at set 1 binding 0 and has
# no set-3 block: the shape of an instanced light volume. Narrow calls it UI --
# which is the defect -- and the widening must call it World.
cls_case "classify: camera-positioned is World"  NonWorld World sample_light_volume.vert.spv
# A fullscreen triangle takes no camera matrix. It must stay UI under BOTH
# readings, or the widening moves the HUD.
cls_case "classify: fullscreen stays NonWorld"   NonWorld NonWorld fullscreen.vert.spv
# The widening must not disturb a fragment-only module's answer either.
cls_case "classify: fragment-only unaffected"    NotVertex NotVertex sample_invproj.frag.spv

echo "all cases passed"
