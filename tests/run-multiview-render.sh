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
#                    an unsheared pass binds the NO-SHEAR TWIN, whatever the
#                    module was patched with. The twin is NOT the pristine
#                    bytes: it is built from `frag_only`, after the fragment
#                    edits and before the vertex ones, so it keeps the bindless
#                    offset and the per-eye M_invprojection (including #61's
#                    R^-1) and loses only the geometry edit. That is what
#                    "unsheared twin keeps the frag patch" below asserts, and
#                    it was the take-23 bug when the twin really was pristine.
#                    This comment said "UNPATCHED" until take 197 and misled an
#                    entire investigation; the code was fixed in take 23 and
#                    the sentence describing it was not.
#
# Since take 71 a colour pass with no depth is unsheared, and every pass this
# harness renders is that shape. So the patched module was built, logged
# ("patched vertex shader #1 (ui)"), and then thrown away at pipeline creation
# ("unsheared pipeline: binding the no-shear twin"). K_nonworld was inert, the
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

# Take 197. The layer must classify the module X4 SENT, not the one it just
# wrote.
#
# patch_vertex_pose_rotate emits an OpAccessChain into camera-block member 1
# inside the vertex function, and the WIDE reading treats an access to member
# 0, 1, 7 or 8 there as proof of world geometry (task #48). So the patch
# manufactures the exact evidence the classifier looks for, and a module
# classified AFTER it reads World when X4's own bytes read NonWorld.
#
# Measured over the 409 dumps: 30 modules flipped that way and every one was a
# procedural fullscreen quad -- screen-space triangles being handed K_world,
# the live-sx path and the affine. Both of rp #31's module pairs and rp #32's
# were in that set, which is where the x=791 seam appears. Live only from take
# 195, when #49's fix made the wide reading the default.
#
# sample_fullscreen_uncam.vert is that shape: a procedural fullscreen triangle
# declaring the camera block and reading member 2 only, so it is NonWorld
# before the patch and World after. The pair below is the point -- the offline
# leg proves the fixture still reproduces the flip, so a green layer leg means
# "the layer ignored our edit" rather than "nothing flips any more".
uncam="$BUILD/tests/sample_fullscreen_uncam.vert.spv"
rawk=$("$PATCHER" classify "$uncam" 2>&1 | sed -n 's/.*WIDE=\([A-Za-z]*\).*/\1/p')
tmpr=$(mktemp)
"$PATCHER" vert-pose-rotate "$uncam" "$tmpr" 31 1 0 1 >/dev/null 2>&1
rotk=$("$PATCHER" classify "$tmpr" 2>&1 | sed -n 's/.*WIDE=\([A-Za-z]*\).*/\1/p')
rm -f "$tmpr"
if [[ "$rawk" == NonWorld && "$rotk" == World ]]; then
    printf 'ok   %-38s raw=%s rotated=%s\n' \
        "classify: the fixture still flips" "$rawk" "$rotk"
else
    printf 'FAIL %-38s want NonWorld->World, got %s->%s (fixture no longer reproduces the trap)\n' \
        "classify: the fixture still flips" "${rawk:-?}" "${rotk:-?}"
    fails=$((fails + 1))
fi

# ...and the layer, driven with the rotation ON and the wide reading ON --
# exactly takes 195/196 -- must still report NonWorld for it.
uncam_log="$(mktemp)"
env X4VR_LOG="$uncam_log" X4VR_MV=1 X4VR_STEREO=1 X4VR_PIPE_INVENTORY=1 \
    X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=20 X4VR_SHEAR_LIGHTS=1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    timeout 120 "$BIN" "$uncam" "$FS" "$SF" >/dev/null 2>&1
uncam_kind=$(grep -oE 's0=mod#[0-9]+/[0-9a-f]+\([A-Za-z]+' "$uncam_log" |
    head -1 | sed 's/.*(//')
rm -f "$uncam_log"
if [[ "$uncam_kind" == NonWorld ]]; then
    printf 'ok   %-38s kind=%s under pose+wide\n' \
        "classify: layer reads X4's bytes" "$uncam_kind"
else
    printf 'FAIL %-38s want NonWorld, got "%s" — the layer classified the module it wrote\n' \
        "classify: layer reads X4's bytes" "${uncam_kind:-?}"
    fails=$((fails + 1))
fi

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

# --- task #76: forcing X4's UI-composition switch --------------------------
#
# `copy_gamma.frag.glsl` emits the hudless scene alone when
# U_fsr3g_useuitexture is true, and composites the separate premultiplied-alpha
# UI texture over it when false. Forcing that switch is how #57 gets the
# hudless image, and it is a shader patch rather than a host write because the
# block it lives in is generated per shader family: 123 of the 370 dumped
# modules that declare set 4 binding 0 start a member at byte 512 and in only
# FOUR is it the switch. See tools/block_offset_census.py.
#
# So the refusals here are not hygiene, they are the whole reason for the
# design, and sample_uiswitch_float.frag is the actual collider: the same
# block with U_patternstr -- 54 of the 119 -- at the same byte.
force_case() {
    local label="$1" want="$2" shader="$3" set="$4" bind="$5" off="$6"
    local name="$7" val="$8" wantloads="${9:-}"
    local out got loads
    out=$("$PATCHER" frag-force-uint "$BUILD/tests/$shader" \
        "$PATCHDIR/force.spv" "$set" "$bind" "$off" "$name" "$val" 2>&1)
    got=$(sed -n 's/^PATCHED=\([01]\).*/\1/p' <<<"$out")
    loads=$(sed -n 's/.*LOADS=\([0-9]*\).*/\1/p' <<<"$out")
    local val_st=skip
    if [[ "$got" == 1 ]]; then
        spirv-val --target-env vulkan1.2 "$PATCHDIR/force.spv" >/dev/null 2>&1 \
            && val_st=OK || val_st=BAD
    fi
    if [[ "$got" == "$want" && "$val_st" != BAD ]] &&
       [[ -z "$wantloads" || "$loads" == "$wantloads" ]]; then
        printf 'ok   %-38s patched=%s loads=%s val=%s\n' \
            "$label" "$got" "${loads:-0}" "$val_st"
    else
        printf 'FAIL %-38s want patched=%s loads=%s, got patched=%s loads=%s val=%s\n' \
            "$label" "$want" "${wantloads:-any}" "${got:-?}" "${loads:-?}" "$val_st"
        fails=$((fails + 1))
    fi
}

FSW=U_fsr3g_useuitexture
force_case "force: the copy_gamma shape"      1 sample_uiswitch.frag.spv 4 0 512 $FSW 1 1
# The negative control the live take depends on. Forcing the switch to its
# SHIPPED value exercises every step -- same module matched, same instruction
# rewritten, same driver recompile -- and must be invisible on screen. If this
# ever refuses, the control stops being available and forcing 1 stops being
# interpretable.
force_case "force: 0 is a working control"    1 sample_uiswitch.frag.spv 4 0 512 $FSW 0 1
# THE case. A float at the same byte under the commonest colliding name.
force_case "force refuses a float at 512"     0 sample_uiswitch_float.frag.spv 4 0 512 U_patternstr 1
force_case "...refuses the wrong member name" 0 sample_uiswitch.frag.spv 4 0 512 U_patternstr 1
force_case "...refuses a wrong byte offset"   0 sample_uiswitch.frag.spv 4 0 508 $FSW 1
force_case "...refuses a declared-unread one" 0 sample_uiswitch.frag.spv 4 0 480 S_diffuse_idx 1
force_case "...refuses a missing set"         0 sample_uiswitch.frag.spv 3 0 512 $FSW 1
force_case "...refuses a block-less module"   0 sample.frag.spv 4 0 512 $FSW 1

# The member index in the fixture is 3, not X4's 21. That is deliberate: a
# transform that keyed on the index would pass every case above and fail on
# X4. Asserted rather than left to the comment in the .frag.
fixture_member=$(spirv-dis "$BUILD/tests/sample_uiswitch.frag.spv" 2>/dev/null |
    sed -n 's/.*OpMemberName %DynamicBlock \([0-9]*\) "U_fsr3g_useuitexture".*/\1/p')
if [[ -n "$fixture_member" && "$fixture_member" != 21 ]]; then
    printf 'ok   %-38s member=%s (X4 uses 21, so index is untested-by-luck)\n' \
        "force: fixture does not share X4's index" "$fixture_member"
else
    printf 'FAIL %-38s member=%s — fixture must NOT use X4'"'"'s member index\n' \
        "force: fixture does not share X4's index" "${fixture_member:-?}"
    fails=$((fails + 1))
fi

# The substitution costs NOTHING when the constant already exists, and exactly
# one instruction when it does not. Both arms are asserted because only one of
# them is reachable with the values the layer actually uses -- 0 and 1 are
# already in any module that compares the switch against zero -- so the append
# path would otherwise ship untested.
words_of() { sed -n 's/.*WORDS=\([0-9]*\).*/\1/p' <<<"$1"; }
src_words=$(( $(stat -c%s "$BUILD/tests/sample_uiswitch.frag.spv") / 4 ))
w_reuse=$(words_of "$("$PATCHER" frag-force-uint \
    "$BUILD/tests/sample_uiswitch.frag.spv" "$PATCHDIR/force.spv" \
    4 0 512 $FSW 1 2>&1)")
w_new=$(words_of "$("$PATCHER" frag-force-uint \
    "$BUILD/tests/sample_uiswitch.frag.spv" "$PATCHDIR/force7.spv" \
    4 0 512 $FSW 7 2>&1)")
val7=BAD
spirv-val --target-env vulkan1.2 "$PATCHDIR/force7.spv" >/dev/null 2>&1 && val7=OK
if [[ "$w_reuse" == "$src_words" && "$w_new" == $((src_words + 4)) &&
      "$val7" == OK ]]; then
    printf 'ok   %-38s reuse=%s(+0) new=%s(+4) val=%s\n' \
        "force: constant reused, else appended" "$w_reuse" "$w_new" "$val7"
else
    printf 'FAIL %-38s src=%s reuse=%s (want +0) new=%s (want +4) val=%s\n' \
        "force: constant reused, else appended" "$src_words" "${w_reuse:-?}" \
        "${w_new:-?}" "$val7"
    fails=$((fails + 1))
fi

# ...and against X4's OWN bytes, which is the only check that can fail for a
# reason the fixture cannot express. The fixture is a shader we wrote to the
# shape we believe X4 has; this asserts the belief.
#
# Opportunistic: the dumps are Egosoft's and live in /tmp, never in the repo, so
# this skips loudly rather than failing on a machine that has not run
# X4VR_MV_DUMP=1. The expected count is 4 -- the four copy_gamma permutations,
# from tools/block_offset_census.py 4 0 --at 512 -- and it is the SAME number
# the layer prints at teardown, so the offline claim and the live one are
# checked against each other rather than each against itself.
if [[ -d /tmp/x4vr-shaders ]] && compgen -G '/tmp/x4vr-shaders/mod-*.spv' >/dev/null; then
    real_hits=0 real_bad=0 real_multi=0 real_n=0
    for dump in /tmp/x4vr-shaders/mod-*.spv; do
        real_n=$((real_n + 1))
        rout=$("$PATCHER" frag-force-uint "$dump" "$PATCHDIR/real.spv" \
            4 0 512 $FSW 1 2>&1 | head -1)
        [[ "$rout" == PATCHED=1* ]] || continue
        real_hits=$((real_hits + 1))
        [[ "$rout" == *LOADS=1* ]] || real_multi=$((real_multi + 1))
        spirv-val --target-env vulkan1.2 "$PATCHDIR/real.spv" >/dev/null 2>&1 ||
            real_bad=$((real_bad + 1))
    done
    if [[ "$real_hits" == 4 && "$real_bad" == 0 && "$real_multi" == 0 ]]; then
        printf 'ok   %-38s %s/%s matched, all valid, 1 load each\n' \
            "force: X4's own dumps" "$real_hits" "$real_n"
    else
        printf 'FAIL %-38s want 4 of %s with 1 load each, got %s (invalid=%s multiload=%s)\n' \
            "force: X4's own dumps" "$real_n" "$real_hits" "$real_bad" "$real_multi"
        fails=$((fails + 1))
    fi
else
    printf 'skip %-38s (no dumps in /tmp/x4vr-shaders)\n' "force: X4's own dumps"
fi

# The layer's side of the knob, on a real GPU under validation.
#
# **What this can and cannot show, stated because the difference matters.** No
# shader here declares X4's block, so the count is 0 and the MATCHING is not
# under test -- that is settled above, against X4's own dumps. What runs here is
# everything around it: the strict value parse, the report firing whether or not
# anything matched, and a run with the knob set being otherwise unchanged. The
# one thing neither leg reaches is "the patched module is the one BOUND", which
# depends on X4's pass structure; the live take reads that off the teardown
# count and the screen together.
hud_probe() {
    local label="$1" value="$2" want_n="$3" want_warn="$4"
    local hl; hl="$(mktemp)"
    env X4VR_LOG="$hl" X4VR_MV=1 X4VR_STEREO=1 X4VR_HUDLESS="$value" \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        timeout 120 "$BIN" "$VS" "$FS" "$SF" >/dev/null 2>&1
    local n warned
    n=$(sed -n 's/.*hudless [a-z]*: switch forced in \([0-9]*\) module.*/\1/p' \
        "$hl" | tail -1)
    warned=$(grep -c 'is neither 0 nor 1' "$hl")
    rm -f "$hl"
    if [[ "$n" == "$want_n" && "$warned" == "$want_warn" ]]; then
        printf 'ok   %-38s value=%-6s forced=%s warned=%s\n' \
            "$label" "\"$value\"" "$n" "$warned"
    else
        printf 'FAIL %-38s value=%s want forced=%s warned=%s, got forced=%s warned=%s\n' \
            "$label" "$value" "$want_n" "$want_warn" "${n:-MISSING}" "$warned"
        fails=$((fails + 1))
    fi
}
# --- task #57: naming X4's UI render target --------------------------------
#
# The predicate: every pass that has written the image is one the canvas
# predicate calls UI, and the image is not a swapchain image. Both halves are
# load-bearing — X4's present passes are canvas passes too, and only the image
# tells them apart.
#
# **Both arms are asserted, and the positive one is why this exists.** A check
# that has only ever answered "not found" cannot be distinguished from one that
# is broken. With a canvas pass in the frame the harness has exactly one
# qualifying image; without one it has none, and the report must say so rather
# than going quiet.
ui_log="$(mktemp)"
env X4VR_LOG="$ui_log" X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_INVENTORY=1 \
    X4VR_CANVAS_M=8 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    timeout 120 "$BIN" "$VS" "$FS" "$SF" >/dev/null 2>&1

ui_n=$(grep -c 'is a UI-ONLY target' "$ui_log")
ui_img=$(sed -n 's/.*ui final: img #\([0-9]*\) is a UI-ONLY target.*/\1/p' "$ui_log" | head -1)
ui_warn=$(grep -c 'UI-only target(s) — EXPECTED exactly 1' "$ui_log")
ui_doubled=$(grep -c 'is a UI-ONLY target.*DOUBLED to 2' "$ui_log")
if [[ "$ui_n" == 1 && "$ui_img" == 1 && "$ui_warn" == 0 && "$ui_doubled" == 1 ]]; then
    printf 'ok   %-38s img #%s, doubled, no warning\n' \
        "ui target: exactly one, and it is #1" "$ui_img"
else
    printf 'FAIL %-38s want n=1 img=1 warn=0 doubled=1, got %s/%s/%s/%s\n' \
        "ui target: exactly one, and it is #1" "$ui_n" "${ui_img:-none}" \
        "$ui_warn" "$ui_doubled"
    fails=$((fails + 1))
fi

# The LATCHED headline, which is the line a run actually gets read on. It is
# separate from the report above because the report only fires from mv_report,
# and mv_report's useful call is at teardown — which the VR runs never reach.
# This harness never presents either, so the latch is hooked at the
# framebuffer; asserting it here is what proves it does not need a present.
ui_latched=$(grep -c 'ui: TARGET img #1 ' "$ui_log")
ui_latch_n=$(grep -c 'ui: TARGET' "$ui_log")
if [[ "$ui_latched" == 1 && "$ui_latch_n" == 1 ]]; then
    printf 'ok   %-38s once, with no present in the run\n' \
        "ui target: latched headline"
else
    printf 'FAIL %-38s want exactly one "ui: TARGET img #1", got %s (of %s)\n' \
        "ui target: latched headline" "$ui_latched" "$ui_latch_n"
    fails=$((fails + 1))
fi

# **The negative control, and it is in the SAME frame as the positive.**
#
# Dropping X4VR_CANVAS_M does not work as a negative arm — it was tried, and
# the pass is still classified +CANVAS, so both arms found the same image and
# the "control" asserted nothing. What discriminates is that the other two
# images in this very frame are rejected, each for its own reason: #0 has a
# world pass among its writers, #2 has no writer at all. A predicate that
# accepted everything, or that keyed on the wrong clause, fails here.
ui_r0=$(grep -c 'img #0 is NOT the UI target — written by a pass the canvas predicate does not call UI' "$ui_log")
ui_r2=$(grep -c 'img #2 is NOT the UI target — no writer recorded yet' "$ui_log")
# "X4 owns a framebuffer for it" and "X4 draws into it" are different claims,
# and the second is the one #57 needs. The counter answers it without a knob;
# asserting it here is what stops it shipping as a line nobody has seen fire.
ui_drew=$(grep -c 'ui: X4 has RECORDED 1 render pass(es) into img #1' "$ui_log")
rm -f "$ui_log"
if [[ "$ui_drew" == 1 ]]; then
    printf 'ok   %-38s counted at the pass, not the framebuffer\n' \
        "ui target: draws into it are counted"
else
    printf 'FAIL %-38s want one "RECORDED 1 render pass(es) into img #1", got %s\n' \
        "ui target: draws into it are counted" "$ui_drew"
    fails=$((fails + 1))
fi
if [[ "$ui_r0" == 1 && "$ui_r2" == 1 ]]; then
    printf 'ok   %-38s #0 non-canvas writer, #2 no writer\n' \
        "ui target: rejects for the right reason"
else
    printf 'FAIL %-38s want one reason line each for #0 and #2, got %s and %s\n' \
        "ui target: rejects for the right reason" "$ui_r0" "$ui_r2"
    fails=$((fails + 1))
fi

# --- task #57 layer B: the capture --------------------------------------
#
# What this CAN test: the staging image is created, the copy is recorded, and
# the whole thing adds no validation error. That last one is the case — the
# barriers act on an image X4 owns, transitioning it out of the layout its own
# render pass declared and back, and getting that wrong is undefined behaviour
# rather than an error.
#
# What it CANNOT test: the submission. There is no OpenXR session here, so the
# second swapchain and proj_ui never run. Said out loud so a green suite is not
# read as "layer B works".
#
# The count is compared against a CONTROL RUN, not against zero: the plain
# config already emits two VUIDs (task #79, pre-existing and unrelated), and a
# rule demanding zero would have failed for someone else's reason.
# Sets LB_ERRS and LB_LOG. NOT a command substitution: that runs the body in a
# subshell, so the log path it picked would be lost and the caller would read an
# unset variable — which is exactly how this failed the first time.
LB_ERRS=0
LB_LOG=""
lb_run() {
    LB_LOG="$(mktemp)"
    LB_ERRS=$(env X4VR_LOG="$LB_LOG" X4VR_MV=1 X4VR_STEREO=1 X4VR_CANVAS_M=8 \
        X4VR_MV_INVENTORY=1 "$@" \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
        timeout 120 "$BIN" "$VS" "$FS" "$SF" 2>&1 |
        grep -c 'Validation Error' || true)
}
lb_run
lb_off=$LB_ERRS
lb_off_stage=$(grep -c 'ui: staging image\|ui: CAPTURED' "$LB_LOG" || true)
rm -f "$LB_LOG"
lb_run X4VR_UI_LAYER=1
lb_on=$LB_ERRS
lb_on_stage=$(grep -c 'ui: staging image ' "$LB_LOG" || true)
lb_on_cap=$(grep -c 'ui: CAPTURED the UI target into staging 1 time' "$LB_LOG" || true)
rm -f "$LB_LOG"

if [[ "$lb_on" == "$lb_off" ]]; then
    printf 'ok   %-38s %s VUID(s) with, %s without — adds none\n' \
        "layer B: validation-clean" "$lb_on" "$lb_off"
else
    printf 'FAIL %-38s %s VUID(s) with X4VR_UI_LAYER=1, %s without — the capture adds %s\n' \
        "layer B: validation-clean" "$lb_on" "$lb_off" \
        "$((lb_on - lb_off))"
    fails=$((fails + 1))
fi
if [[ "$lb_on_stage" == 1 && "$lb_on_cap" == 1 ]]; then
    printf 'ok   %-38s staging created, one capture recorded\n' \
        "layer B: the capture runs"
else
    printf 'FAIL %-38s want one staging line and one capture, got %s and %s\n' \
        "layer B: the capture runs" "$lb_on_stage" "$lb_on_cap"
    fails=$((fails + 1))
fi
# Off means OFF: no image allocated, no copy recorded. A knob whose "off" still
# does the work cannot serve as the control arm of anything.
if [[ "$lb_off_stage" == 0 ]]; then
    printf 'ok   %-38s no image, no copy, with the knob off\n' \
        "layer B: off changes nothing"
else
    printf 'FAIL %-38s %s staging/capture line(s) with the knob OFF\n' \
        "layer B: off changes nothing" "$lb_off_stage"
    fails=$((fails + 1))
fi

hud_probe "hudless: reports even at zero"  1  0 0
# The report must exist with the knob UNSET too, or "the switch was not forced"
# and "the run predates the knob" read the same in a log.
hud_probe "hudless: reports when unset"    "" 0 0
# The strict parse. Without it this would force the CONTROL arm and say nothing.
hud_probe "hudless: refuses a bad value"   yes 0 1
hud_probe "hudless: refuses 2"             2   0 1

# The branch really is constant afterwards: the OpLoad is gone and an
# OpCopyObject of a constant stands in its place, keeping the same result id.
# Counted from the disassembly, because PATCHED=1 only says the transform ran.
if command -v spirv-dis >/dev/null; then
    "$PATCHER" frag-force-uint "$BUILD/tests/sample_uiswitch.frag.spv" \
        "$PATCHDIR/force.spv" 4 0 512 $FSW 1 >/dev/null 2>&1
    before_n=$(spirv-dis "$BUILD/tests/sample_uiswitch.frag.spv" 2>/dev/null |
        grep -c 'OpLoad %uint')
    after_n=$(spirv-dis "$PATCHDIR/force.spv" 2>/dev/null | grep -c 'OpLoad %uint')
    copy_n=$(spirv-dis "$PATCHDIR/force.spv" 2>/dev/null | grep -c 'OpCopyObject')
    if [[ "$before_n" == 1 && "$after_n" == 0 && "$copy_n" == 1 ]]; then
        printf 'ok   %-38s uint loads %s->%s, copies=%s\n' \
            "force: the load is really gone" "$before_n" "$after_n" "$copy_n"
    else
        printf 'FAIL %-38s want 1->0 loads and 1 copy, got %s->%s copies=%s\n' \
            "force: the load is really gone" "$before_n" "$after_n" "$copy_n"
        fails=$((fails + 1))
    fi
fi

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
    # ---- task #77: the roll-turned eye offset -------------------------
    #
    # Three things, and the first is the one that protects every tagged state
    # before #77: with no roll set the words must be BYTE-IDENTICAL to what
    # this function emitted before the parameter existed.
    "$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/roll_off.spv" 1 0 1 -0.008 0.008 >/dev/null 2>&1
    "$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/roll_on.spv" 1 0 1 -0.008 0.008 - - 7 >/dev/null 2>&1
    if cmp -s "$PATCHDIR/eye.spv" "$PATCHDIR/roll_off.spv"; then
        printf 'ok   %-38s\n' "...roll absent emits the old words"
    else
        printf 'FAIL %-38s roll_set unset changed the output\n' \
            "...roll absent emits the old words"
        fails=$((fails + 1))
    fi
    # With the roll on: TWO inserts (x and y, chained) where the unrolled form
    # has one, and a second uniform block at the set we asked for. A patch that
    # wrote both components into the pre-insert vector would still validate and
    # would silently drop the x edit.
    rins=$(spirv-dis "$PATCHDIR/roll_on.spv" 2>/dev/null | grep -c 'OpCompositeInsert')
    rset=$(spirv-dis "$PATCHDIR/roll_on.spv" 2>/dev/null | grep -c 'DescriptorSet 7')
    roff=$(spirv-dis "$PATCHDIR/roll_on.spv" 2>/dev/null | grep -c '0 Offset 192')
    # X4VR_HEAD_6DOF: the second member, head_fwd, at 208.
    rfwd=$(spirv-dis "$PATCHDIR/roll_on.spv" 2>/dev/null | grep -c '1 Offset 208')
    rval=BAD
    spirv-val --target-env vulkan1.2 "$PATCHDIR/roll_on.spv" >/dev/null 2>&1 && rval=OK
    # x and y (roll), then z and w (X4VR_HEAD_6DOF's forward offset): four.
    if [[ "$rins" == 4 && "$rset" == 1 && "$roff" == 1 && "$rfwd" == 1 && "$rval" == OK ]]; then
        printf 'ok   %-38s inserts=%s set7=%s off192=%s off208=%s val=%s\n' \
            "...roll adds the y and forward terms" "$rins" "$rset" "$roff" "$rfwd" "$rval"
    else
        printf 'FAIL %-38s want 4/1/1/1/OK, got %s/%s/%s/%s/%s\n' \
            "...roll adds the y and forward terms" "$rins" "$rset" "$roff" "$rfwd" "$rval"
        fails=$((fails + 1))
    fi
    # Refused in combination with the off-axis affine, which rewrites x and y
    # from the sheared x and was derived assuming the offset is horizontal.
    # Untested together, so refused rather than emitted.
    rc=$("$PATCHER" vert-eye-offset "$BUILD/tests/sample_camera_block.vert.spv" \
        "$PATCHDIR/roll_oa.spv" 1 0 1 -0.008 0.008 "1,0,1,0" - 7 2>&1 \
        | sed -n 's/^PATCHED=//p')
    if [[ "$rc" == 0 ]]; then
        printf 'ok   %-38s patched=%s\n' "...roll+offaxis is refused" "$rc"
    else
        printf 'FAIL %-38s want patched=0, got %s\n' \
            "...roll+offaxis is refused" "${rc:-?}"
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

    # ---- take 397: the mvp route takes the roll and the head slot ------
    #
    # Until take 396 it did not, so its passes (X4's depth-only technique
    # among them) drew off every other pass under head tilt and interior
    # lights flickered. Roll absent must leave the words exactly as before;
    # roll present must validate, read the slot at 192/208, and refuse the
    # off-axis map as the camera-block route does.
    "$PATCHER" vert-eye-offset-mvp "$BUILD/tests/sample_light_volume.vert.spv" \
        "$PATCHDIR/mvp_plain.spv" 1 0 7 -0.032 0.032 >/dev/null 2>&1
    "$PATCHER" vert-eye-offset-mvp "$BUILD/tests/sample_light_volume.vert.spv" \
        "$PATCHDIR/mvp_dash.spv" 1 0 7 -0.032 0.032 - - >/dev/null 2>&1
    if cmp -s "$PATCHDIR/mvp_plain.spv" "$PATCHDIR/mvp_dash.spv"; then
        printf 'ok   %-38s\n' "...mvp: roll absent emits the old words"
    else
        printf 'FAIL %-38s "-" for the maps changed the output\n' \
            "...mvp: roll absent emits the old words"
        fails=$((fails + 1))
    fi
    "$PATCHER" vert-eye-offset-mvp "$BUILD/tests/sample_light_volume.vert.spv" \
        "$PATCHDIR/mvp_roll.spv" 1 0 7 -0.032 0.032 - - 7 >/dev/null 2>&1
    mins=$(spirv-dis "$PATCHDIR/mvp_roll.spv" 2>/dev/null | grep -c 'OpCompositeInsert')
    mset=$(spirv-dis "$PATCHDIR/mvp_roll.spv" 2>/dev/null | grep -c 'DescriptorSet 7')
    moff=$(spirv-dis "$PATCHDIR/mvp_roll.spv" 2>/dev/null | grep -c '0 Offset 192')
    mfwd=$(spirv-dis "$PATCHDIR/mvp_roll.spv" 2>/dev/null | grep -c '1 Offset 208')
    mval=BAD
    spirv-val --target-env vulkan1.2 "$PATCHDIR/mvp_roll.spv" >/dev/null 2>&1 && mval=OK
    # x, y and w: X4's projection makes the forward offset's z term zero.
    if [[ "$mins" == 3 && "$mset" == 1 && "$moff" == 1 && "$mfwd" == 1 && "$mval" == OK ]]; then
        printf 'ok   %-38s inserts=%s set7=%s off192=%s off208=%s val=%s\n' \
            "...mvp: roll adds the y and w terms" "$mins" "$mset" "$moff" "$mfwd" "$mval"
    else
        printf 'FAIL %-38s want 3/1/1/1/OK, got %s/%s/%s/%s/%s\n' \
            "...mvp: roll adds the y and w terms" "$mins" "$mset" "$moff" "$mfwd" "$mval"
        fails=$((fails + 1))
    fi
    rc=$("$PATCHER" vert-eye-offset-mvp "$BUILD/tests/sample_light_volume.vert.spv" \
        "$PATCHDIR/mvp_roll_oa.spv" 1 0 7 -0.032 0.032 "$OA_L" - 7 2>&1 \
        | sed -n 's/^PATCHED=//p')
    if [[ "$rc" == 0 ]]; then
        printf 'ok   %-38s patched=%s\n' "...mvp: roll+offaxis is refused" "$rc"
    else
        printf 'FAIL %-38s want patched=0, got %s\n' \
            "...mvp: roll+offaxis is refused" "${rc:-?}"
        fails=$((fails + 1))
    fi
fi

# Take 397: the two routes put the same vertex in the same place. One module
# carries both blocks, so it is patched by both routes and RUN (a float32
# interpreter, tests/spirv_interp.hpp) on X4-shaped inputs across rolls, head
# shifts, forward offsets, scales, zooms and both eyes. The roll-less mvp
# route is the negative control and must disagree.
out=$("$PATCHER" vert-eye-check "$BUILD/tests/sample_camera_block.vert.spv" 2>&1)
if grep -qx PASS <<<"$out"; then
    printf 'ok   %-38s %s\n' "eye: both routes compute the same vertex" "$(head -1 <<<"$out")"
else
    printf 'FAIL %-38s %s\n' "eye: both routes compute the same vertex" "$(tr '\n' ' ' <<<"$out")"
    fails=$((fails + 1))
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

# **Task #77's regression.** The vertex stage displaces the eye along
# (cos roll, sin roll); before these cases existed this function undid a purely
# horizontal d, so from the moment #77 shipped the two were no longer the same
# map and the reconstruction was wrong by d*(cos-1, sin) -- 1.08*d at 65
# degrees, larger than the eye offset itself, feeding per-eye lighting and
# shadows. It shipped defaulted-ON and no take would have shown it.
#
# The acceptance sets of the two stages must be IDENTICAL. A combination
# patched here and declined there undoes an offset that was never applied,
# which is the same defect with the sign flipped -- so every refusal below
# mirrors one in the vertex block above.
inv_case "invproj: roll, per-frame (cos,sin)" 1 sample_invproj.frag.spv 1 0 2 \
    - - 31
inv_case "invproj: roll refuses with #61's pose" 0 sample_invproj.frag.spv 1 0 2 \
    - - 31 31
inv_case "invproj: roll refuses with the affine, L" 0 \
    sample_invproj.frag.spv 1 0 2 "$OA_L" - 31
inv_case "invproj: roll refuses with the affine, R" 0 \
    sample_invproj.frag.spv 1 0 2 - "$OA_R" 31
# #61's pose alone still patches. Never exercised before -- the harness had no
# argument for it, so "refuses roll with pose" would have passed by never
# reaching the branch, which is a check that cannot fail.
inv_case "invproj: #61 pose alone still patches" 1 sample_invproj.frag.spv 1 0 2 \
    - - - 31

# The roll must reach BOTH rows, not just row 0: that is the whole content of
# the fix, and "it validates" would be true of a module that changed nothing.
# Counted from the disassembly rather than inferred from PATCHED=1.
roll_rows() {
    local args=("$@") r0 r1
    "$PATCHER" frag-invproj "$BUILD/tests/sample_invproj.frag.spv" \
        "$PATCHDIR/inv-rows.spv" 1 0 2 -0.032 0.032 "${args[@]}" >/dev/null 2>&1
    r0=$(spirv-dis "$PATCHDIR/inv-rows.spv" | grep -cE 'OpCompositeInsert .* [0-9]+ 0$')
    r1=$(spirv-dis "$PATCHDIR/inv-rows.spv" | grep -cE 'OpCompositeInsert .* [0-9]+ 1$')
    echo "$r0 $r1"
}
read -r R0_OFF R1_OFF <<<"$(roll_rows)"
read -r R0_ON R1_ON <<<"$(roll_rows - - 31)"
if [[ "$R0_OFF" == 4 && "$R1_OFF" == 0 && "$R0_ON" == 4 && "$R1_ON" == 4 ]]; then
    printf 'ok   %-38s off=(%s,%s) on=(%s,%s)\n' \
        "invproj: roll writes row 1 too" "$R0_OFF" "$R1_OFF" "$R0_ON" "$R1_ON"
else
    printf 'FAIL %-38s want off=(4,0) on=(4,4), got off=(%s,%s) on=(%s,%s)\n' \
        "invproj: roll writes row 1 too" "$R0_OFF" "$R1_OFF" "$R0_ON" "$R1_ON"
    fails=$((fails + 1))
fi

# And with the roll absent the emitted words must be BYTE-IDENTICAL to what the
# pre-fix patcher produced, or every tag before this one stops reproducing.
# `cmp` against a module patched with no roll argument at all, which is the
# path every existing take took.
"$PATCHER" frag-invproj "$BUILD/tests/sample_invproj.frag.spv" \
    "$PATCHDIR/inv-noroll-a.spv" 1 0 2 -0.032 0.032 >/dev/null 2>&1
"$PATCHER" frag-invproj "$BUILD/tests/sample_invproj.frag.spv" \
    "$PATCHDIR/inv-noroll-b.spv" 1 0 2 -0.032 0.032 - - >/dev/null 2>&1
if cmp -s "$PATCHDIR/inv-noroll-a.spv" "$PATCHDIR/inv-noroll-b.spv"; then
    printf 'ok   %-38s cmp clean\n' "invproj: roll absent changes nothing"
else
    printf 'FAIL %-38s the no-roll path moved\n' \
        "invproj: roll absent changes nothing"
    fails=$((fails + 1))
fi
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

# Task #61's R^-1 -- which not one of the three cases above can see. They run
# with no pose set, so the patch emits no rotation term at all, and their
# perfect zeros say the eye offsets are right and NOTHING about the rotation.
# #61 had been proven to emit, to hold on paper and to execute on a GPU, but
# never to compute the identity it claims, because this CLI could not pass it
# a pose set. The "-" arguments are the off-axis slots, skipped to reach past
# them.
eval_case "invproj: R⁻¹ composed, no affine"  - - 31 0 10,-7,3
eval_case "invproj: R⁻¹ composed with A⁻¹"    "$OA_L" "$OA_R" 31 0 10,-7,3

# ...and the negative controls, because a zero from a check that has never
# been seen to go red is indistinguishable from a check that evaluated
# nothing. Both of these ran BEFORE the case above was believed.
#
#   FLIP     composes the reference M·R⁻¹ instead of R⁻¹·M. #61 left-
#            multiplies, and the wrong order would have made the check agree
#            with a wrong emission -- the one trap the spec named.
#   ROWMAJOR decodes the pose block's bytes with the other majorness. The
#            patch's own comment says a layout mismatch "reads back as a
#            plausible wrong matrix, not as an error"; this is what makes
#            the offset, the stride and the majorness checkable instead of
#            assumed.
control_case() {
    local label="$1" var="$2"; shift 2
    local out worst checked
    out=$(env "$var=1" "$PATCHER" frag-invproj-check \
        "$BUILD/tests/sample_invproj.frag.spv" 1 0 2 -0.032 0.032 "$@" 2>&1)
    worst=$(sed -n 's/.*WORST=//p' <<<"$out")
    checked=$(sed -n 's/^CHECKED=\([0-9]*\).*/\1/p' <<<"$out")
    # The error is 10·sin(yaw) -- the 10 from the 1/near term in the inverse
    # projection -- so 0.1 is far below what any real mistake would score and
    # far above float noise.
    if [[ "$checked" == 10 ]] && awk "BEGIN{exit !($worst > 0.1)}" 2>/dev/null
    then
        printf 'ok   %-38s worst=%s, red as it must be\n' "$label" "$worst"
    else
        printf 'FAIL %-38s want checked=10 worst>0.1, got %s / %s\n' \
            "$label" "${checked:-?}" "${worst:-?}"
        fails=$((fails + 1))
    fi
}
control_case "invproj: wrong R⁻¹ order is caught"  X4VR_CHECK_RINV_FLIP \
    - - 31 0 10,-7,3
control_case "invproj: wrong pose majorness caught" X4VR_CHECK_RINV_ROWMAJOR \
    - - 31 0 10,-7,3

# An identity rotation commutes with everything, so the flipped order would
# agree with the right one and the check would pass while proving nothing.
# Refusing it is what stops a future caller from reading that as a result.
out=$("$PATCHER" frag-invproj-check "$BUILD/tests/sample_invproj.frag.spv" \
    1 0 2 -0.032 0.032 - - 31 0 0,0,0 2>&1)
if [[ "$out" == *identity_rotation_proves_nothing* ]]; then
    printf 'ok   %-38s refused\n' "invproj: identity rotation refused"
else
    printf 'FAIL %-38s want a refusal, got "%s"\n' \
        "invproj: identity rotation refused" "$out"
    fails=$((fails + 1))
fi

# The layout the patch declares, pinned against x4vr::PoseBlock from the other
# side. The C++ static_assert covers the struct; this covers what the SPIR-V
# says about it, and a shader reads the bytes through the SPIR-V.
out=$("$PATCHER" frag-invproj-check "$BUILD/tests/sample_invproj.frag.spv" \
    1 0 2 -0.032 0.032 - - 31 0 10,0,0 2>&1 | sed -n 's/.*LAYOUT=//p')
if [[ "$out" == "off64/stride16/col" ]]; then
    printf 'ok   %-38s %s\n' "invproj: pose block layout is std140" "$out"
else
    printf 'FAIL %-38s want off64/stride16/col, got "%s"\n' \
        "invproj: pose block layout is std140" "${out:-?}"
    fails=$((fails + 1))
fi

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
# X4VR_COMPLOG (take 379): an FSR-shaped compute pass with real resources --
# the module recognised as FSR with its bindings, and the dispatch resolving
# the sampled input to the doubled target and the storage output to its image.
inv_case "complog names the FSR module" \
    "resources: Pass@0.0 S_input_rt@0.1 U_output_rt@0.2 | FSR: FsrEasuH" \
    X4VR_TEST_COMPUTE=fsr X4VR_COMPLOG=1
inv_case "complog resolves the sampled input" \
    "S_input_rt@0.1: sampled img #0 128x128 fmt=97 usage=0x15 DOUBLED, view 2D from layer 0, 1 layer(s)" \
    X4VR_TEST_COMPUTE=fsr X4VR_COMPLOG=1
inv_case "complog resolves the storage output" \
    "U_output_rt@0.2: storage img #2 64x64 fmt=97 usage=0xc single, view 2D from layer 0, 1 layer(s)" \
    X4VR_TEST_COMPUTE=fsr X4VR_COMPLOG=1
# FidelityFX (X4's FSR 3) destroys each module once its pipeline exists, and
# the driver can hand the freed handle to the next module: the dispatch must
# still be described by the module its pipeline was built from (the layer at
# 409148f named the decoy, c1, with no resources). The first case says the
# second can fail: the driver did reuse the handle.
inv_case "complog: module handle reused" \
    "MODULE_HANDLE_REUSED=1" X4VR_TEST_COMPUTE=fsr X4VR_COMPLOG=1 X4VR_TEST_REUSE=1
inv_case "...the dispatch keeps its own module" \
    "comp: dispatch module c0 groups 4x4x1 direct | Pass@0.0" \
    X4VR_TEST_COMPUTE=fsr X4VR_COMPLOG=1 X4VR_TEST_REUSE=1
# The FSR fix (take 380): tests/fsr_bindless.comp has the resources of X4's
# compiled FSR pass, read and written through the bindless tables. With the
# fix both layers of the doubled output are written (from identical input
# layers, so identical); with X4VR_FSR_STEREO=0 layer 1 stays zero, which is
# take 379's black right eye.
fsrb_case() {
    local label="$1" knob="$2" want="$3"
    local out got
    out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_BINDLESS_MIRROR=1 \
        X4VR_RES=64x64 X4VR_MIRROR_OFFSET=32 X4VR_TEST_COMPUTE=fsrbindless \
        "X4VR_FSR_STEREO=$knob" "$BIN" "$VS" "$FS" "$SF" 2>&1)
    got="$(sed -n 's/^FSRB_OUT_LAYERS=//p' <<<"$out")"
    got="$got $(sed -n 's/^FSRB_L0_WRITTEN=//p' <<<"$out")"
    got="$got $(sed -n 's/^FSRB_L1_WRITTEN=//p' <<<"$out")"
    got="$got $(sed -n 's/^FSRB_IDENTICAL=//p' <<<"$out")"
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s layers/l0/l1/identical=%s\n' "$label" "$got"
    else
        printf 'FAIL %-38s layers/l0/l1/identical=%s, wanted %s\n' \
            "$label" "$got" "$want"
        fails=$((fails + 1))
    fi
}
fsrb_case "FSR fix writes both eyes" 1 "2 1 1 1"
fsrb_case "...and without it layer 1 is black" 0 "2 1 0 0"
inv_case "...the pass patched by signature" \
    "fsr: X4's FSR pass (2696 bytes) patched for both eyes — 2 table access(es) + WorkGroupID.z" \
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=64x64 X4VR_MIRROR_OFFSET=32 X4VR_TEST_COMPUTE=fsrbindless
inv_case "...the eye-sized dispatch doubled" \
    "fsr: dispatch 4x4x1 covers the 64x64 eye — run as 4x4x2" \
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=64x64 X4VR_MIRROR_OFFSET=32 X4VR_TEST_COMPUTE=fsrbindless
# FSR 3 in both eyes (take 382): two FidelityFX-shaped passes with a
# single-layer history between them (tests/ffx_pass1/2.comp), the input drawn
# with tests/view_color.frag so its layers differ (red 0.25 / 0.75). Red of
# the output: 0.375 in layer 0 and 0.875 in layer 1 with the fix; layer 1 at
# 0 with X4VR_FFX_STEREO=0, take 381's black right eye. Checked by hand once
# (no permanent knob for it): without the history's right-eye copy, layer 0
# reads 0.875 -- the right eye's pass 1 overwrote the shared history.
VCF="$BUILD/tests/view_color.frag.spv"
ffx_case() {
    local label="$1" knob="$2" want="$3" order="${4:-}"
    local out got
    out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_BINDLESS_MIRROR=1 \
        X4VR_RES=64x64 X4VR_TEST_COMPUTE=ffx "X4VR_FFX_STEREO=$knob" \
        "X4VR_TEST_FFX_ORDER=$order" "$BIN" "$VS" "$VCF" "$SF" 2>&1)
    got="$(sed -n 's/^FFX_OUT_LAYERS=//p' <<<"$out")"
    got="$got $(sed -n 's/^FFX_R0=//p' <<<"$out")"
    got="$got $(sed -n 's/^FFX_R1=//p' <<<"$out")"
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s layers/red0/red1=%s\n' "$label" "$got"
    else
        printf 'FAIL %-38s layers/red0/red1=%s, wanted %s\n' "$label" "$got" "$want"
        fails=$((fails + 1))
    fi
}
ffx_case "FSR 3: each eye from its own layer" 1 "2 0.375 0.875"
ffx_case "...and without the fix layer 1 is 0" 0 "2 0.375 0.000"
# Take 382: FidelityFX's ping-pong history is read before it is first
# written. The layer at 6160a37 cached "keep the left eye's" for that view and
# gave 0.875 0.875 here (the right eye jumped between its picture and the
# left's in VR).
ffx_case "...history read before it is written" 1 "2 0.375 0.875" reader-first
ffx_log_case() {
    local label="$1" want="$2" out
    out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_BINDLESS_MIRROR=1 \
        X4VR_RES=64x64 X4VR_TEST_COMPUTE=ffx "$BIN" "$VS" "$VCF" "$SF" 2>&1)
    if grep -qF -- "$want" <<<"$out"; then
        printf 'ok   %-38s %s\n' "$label" "$want"
    else
        printf 'FAIL %-38s no line containing "%s"\n' "$label" "$want"
        fails=$((fails + 1))
    fi
}
ffx_log_case "...both passes recognised" \
    "ffx: module (1688 bytes, writes rw_upscaled_output) is a FidelityFX pass"
ffx_log_case "...the history gets a right-eye copy" \
    "ffx: img #1 64x64 mips=1 fmt=97 usage=0xc: a right-eye copy"
ffx_log_case "...dispatches run again" \
    "ffx: dispatch 8x8x1 of a FidelityFX pass run again for the right eye through 1 twin set binding(s) (1 so far)"
# The test exits without tearing the device down, so no `ffx final` line:
# the failure lines must be absent instead.
out=$(env "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=3 X4VR_BINDLESS_MIRROR=1 \
    X4VR_RES=64x64 X4VR_TEST_COMPUTE=ffx "$BIN" "$VS" "$VCF" "$SF" 2>&1)
if grep -qE "ffx: .*(one-eyed|NOT|could NOT|never seen)" <<<"$out"; then
    printf 'FAIL %-38s %s\n' "...nothing left one-eyed" \
        "$(grep -m1 -E 'ffx: .*(one-eyed|NOT|never seen)' <<<"$out")"
    fails=$((fails + 1))
else
    printf 'ok   %-38s %s\n' "...nothing left one-eyed" "no ffx failure line"
fi
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

# --- task #40: procedural-fullscreen, the canvas's real exclusion -----------
#
# #30's canvas keyed on World and got the right answer, because every World
# module has vertex attributes. But "World" is a statement about a per-object
# matrix and says nothing about whether a quad is procedural, and the two
# readings differ on five of X4's modules -- NonWorld UI with real attributes,
# which #30 could not move and #40 must. This is the predicate that replaces
# it, so it is pinned against committed bytes rather than left to the sweep
# over /tmp dumps that nobody else can re-run.
proc_case() {
    local label="$1" want="$2" shader="$3"
    local got
    got=$("$PATCHER" classify "$BUILD/tests/$shader" 2>&1 |
        sed -n 's/.*PROCEDURAL=\([01]\).*/\1/p')
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s procedural=%s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want procedural=%s, got %s\n' \
            "$label" "$want" "${got:-?}"
        fails=$((fails + 1))
    fi
}
# gl_VertexIndex and no attributes: the shape of every fullscreen post pass,
# and the one thing the canvas must never touch.
proc_case "procedural: fullscreen triangle"      1 fullscreen.vert.spv
# Real vertex data positions these, so they are draws that can be placed.
proc_case "procedural: camera block is not"      0 sample_camera_block.vert.spv
proc_case "procedural: light volume is not"      0 sample_light_volume.vert.spv
# A fragment-only module has no vertex stage to judge; false, so it is never
# excluded on these grounds and the vertex-side rules stay the only ones that
# speak. Returning true here would silently drop it from the canvas.
proc_case "procedural: fragment-only is false"   0 sample_invproj.frag.spv

# --- X4VR_MV_PROBE_MAX: the probe must be boundable ------------------------
#
# Take 176 probed every frame for a whole session: 30 s freezes with about ten
# usable frames between them, and Patola hit the target he was asked to look at
# by luck. The information the probe carries is bounded -- one hash per per-eye
# image -- so the cost must be too.
#
# Asserted where the sample is TAKEN rather than on sweep completion, because
# sweep completion is only observable from the present path and nothing here
# presents. A bound that cannot be exercised is a bound that ships broken.
# NOTE the limit of this harness: it renders one pass, so this shows the cap
# BRANCH is taken instead of the sampling branch, not a long-run count.
probe_cap_case() { # label max want_cap
    local log; log="$(mktemp)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 X4VR_MV_PROBE=1 \
        ${2:+X4VR_MV_PROBE_MAX=$2} X4VR_LOG="$log" \
        timeout 120 "$BIN" >/dev/null 2>&1
    local got; got=$(grep -c "sample cap reached" "$log")
    rm -f "$log"
    if [[ "$got" == "$3" ]]; then
        printf 'ok   %-38s cap-fired=%s\n' "$1" "$got"
    else
        printf 'FAIL %-38s want cap-fired=%s, got %s\n' "$1" "$3" "$got"
        fails=$((fails + 1))
    fi
}
# The readback budget. At 4224x4224 an unbounded sample is 285 MB across PCIe
# and then hashed byte-wise on the CPU: take 178 measured 30-50 s per sample and
# 4.4 s for the 1056x1056 images in the same run, cost tracking pixel count. The
# budget is in KB, not MB, precisely so this harness can exercise it -- its image
# is 128 KB and would sit under any megabyte budget, making the shrink path
# testable only by reasoning about it.
probe_win_case() { # label kb want_w
    local log; log="$(mktemp)"; local pre; pre="$(mktemp -u)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 X4VR_MV_PROBE=1 \
        X4VR_MV_DUMP_IMG=0 X4VR_MV_DUMP="$pre" \
        ${2:+X4VR_MV_PROBE_KB=$2} X4VR_LOG="$log" \
        timeout 120 "$BIN" >/dev/null 2>&1
    local got
    got=$(python3 -c "
from PIL import Image; import glob, sys
f=glob.glob('$pre-img0-n0-layer0.png')
print(Image.open(f[0]).size[0] if f else 0)" 2>/dev/null)
    rm -f "$log" "$pre"-*
    if [[ "$got" == "$3" ]]; then
        printf 'ok   %-38s window=%sx%s\n' "$1" "$got" "$got"
    else
        printf 'FAIL %-38s want %s, got %s\n' "$1" "$3" "${got:-?}"
        fails=$((fails + 1))
    fi
}
# X4VR_MV_DUMP_AFTER_RP: capture the eye image at the end of a NAMED pass.
# Neither existing dump can see between two present passes -- the probe fires at
# the end of the first pass that ends with that attachment (rp #0 for the eye
# image), the present dump after the last -- and #49 needs exactly that gap.
# A LIST is round-robined, so bisecting five passes is one run, not five.
after_rp_case() { # label value want_files want_named
    local log; log="$(mktemp)"; local pre; pre="$(mktemp -u)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 \
        X4VR_MV_DUMP_AFTER_RP="$2" X4VR_MV_DUMP="$pre" X4VR_LOG="$log" \
        timeout 120 "$BIN" >/dev/null 2>&1
    local n; n=$(ls "$pre"-* 2>/dev/null | wc -l)
    local nm; nm=$(ls "$pre"-* 2>/dev/null | head -1 | sed 's|.*/||')
    # A capture that produces nothing must SAY so: silence is indistinguishable
    # from "there was nothing to capture", and that ambiguity costs runs.
    local said; said=$(grep -c "no after-rp file was written" "$log")
    rm -f "$log" "$pre"-*
    if [[ "$n" == "$3" && ( "$3" != 0 || "$said" -ge 1 ) && "$nm" == *"$4"* ]]; then
        printf 'ok   %-38s files=%s %s\n' "$1" "$n" "${nm:-said-nothing}"
    else
        printf 'FAIL %-38s want %s files matching "%s" (said=%s), got %s %s\n' \
            "$1" "$3" "$4" "$said" "$n" "${nm:-none}"
        fails=$((fails + 1))
    fi
}
after_rp_case "after-rp: existing pass captured"  0      2 "afterrp0"
after_rp_case "after-rp: absent pass says so"     5      0 ""
after_rp_case "after-rp: list round-robins"       0,5,9  2 "afterrp0"

# Take 179 asked for six targets, got no files, and the report named only
# element [0] -- "X4VR_MV_DUMP_AFTER_RP=1 produced NOTHING" for a six-target
# run, saying nothing at all about 4, 7, 10, 33 and 51. Worse, the first
# version of the fix reported a pass that HAD been reached (and logged as
# reached) as "NEVER REACHED", because reached and captured were one flag.
# Every target must get its own verdict, and the verdicts must be distinct.
verdict_case() { # label target want_verdict_substring
    local log; log="$(mktemp)"; local pre; pre="$(mktemp -u)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 \
        X4VR_MV_DUMP_AFTER_RP=0,1,5 X4VR_MV_DUMP="$pre" X4VR_LOG="$log" \
        timeout 120 "$BIN" >/dev/null 2>&1
    local line; line=$(grep -a "target rp #$2:" "$log" | head -1 | sed 's/.*target rp/target rp/')
    rm -f "$log" "$pre"-*
    if [[ "$line" == *"$3"* ]]; then
        printf 'ok   %-38s %s\n' "$1" "$(echo "$line" | cut -c1-48)"
    else
        printf 'FAIL %-38s want "%s", got: %s\n' "$1" "$3" "${line:-no verdict line}"
        fails=$((fails + 1))
    fi
}
# A doubled image that no framebuffer names must be REPORTED, not omitted.
# Take 182 had 45 of them and the report said "5 of 29" -- silent about the
# rest -- because the writer map only gained an entry at framebuffer creation.
# The harness now creates one deliberately (ORPHAN_DOUBLED).
orphan_case() {
    local log; log="$(mktemp)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 X4VR_MV_INVENTORY=1 \
        X4VR_LOG="$log" timeout 120 "$BIN" >/dev/null 2>&1
    local named; named=$(grep -ac "NO RENDER PASS WROTE IT" "$log")
    local storage; storage=$(grep -ac "NO RENDER PASS WROTE IT.*STORAGE" "$log")
    local tot; tot=$(grep -ao "doubled images — [0-9]* total" "$log" | head -1 |
                     grep -o "[0-9]*" | head -1)
    rm -f "$log"
    # The count must include the orphan: a report that enumerates only the
    # images it happened to see attached is the defect this case exists for.
    if [[ "$named" -ge 1 && "$storage" -ge 1 && "${tot:-0}" -ge 3 ]]; then
        printf 'ok   %-38s %s reported, %s total\n' \
            "orphan doubled image is reported" "$named" "$tot"
    else
        printf 'FAIL %-38s named=%s storage=%s total=%s\n' \
            "orphan doubled image is reported" "$named" "$storage" "${tot:-none}"
        fails=$((fails + 1))
    fi
}
orphan_case

# A canted off-axis target displaces where a world direction lands inside each
# eye image, in opposite directions, and X4 keys screen-space effects to that
# position -- takes 183/184 are the controlled pair (#49, #52). The cant may
# well be wanted again once the render extent shrinks to match it, so the layer
# warns rather than refusing. What must not happen is switching it on silently,
# and what must ALSO not happen is crying wolf on the identity control, which is
# a legitimate everyday configuration.
cant_case() {
    local label="$1" oa="$2" want="$3" log got
    log="$(mktemp)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 X4VR_PROJ_LIVE=1 \
        X4VR_FOV=1.4917 X4VR_OFFAXIS="$oa" \
        X4VR_LOG="$log" timeout 120 "$BIN" >/dev/null 2>&1
    got=$(grep -ac "target is CANTED" "$log")
    rm -f "$log"
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s warned=%s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want warned=%s, got %s\n' "$label" "$want" "$got"
        fails=$((fails + 1))
    fi
}
cant_case "cant: canted target warns"     "-54,40,44,-55"  1
cant_case "cant: identity stays silent"   "-55,55,55,-55"  0

# Task #53: the per-camera M_view report, driven on a real GPU under
# validation. X4VR_TEST_VIEW_ARENA=1 makes the harness build two range-1792
# camera blocks at set 1 -- block 0 with an identity M_view and 60 draws,
# block 1 with take 178's logged M_view and 10 -- so the loser is the one that
# is not identity. That asymmetry IS the test: the report this replaces read
# only the winner, and that is how a camera with a 15 deg yaw sat in 9 stored
# runs for five weeks without anyone seeing it.
#
# Three assertions, because two of them can pass while the instrument is
# useless: that it reports the identity camera (so silence cannot be mistaken
# for a clean result), that it reports the non-identity one AT ALL, and that
# it names the angle and offset rather than merely flagging it.
view_arena_case() {
    local log out
    log="$(mktemp)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 X4VR_DUMP_MATRICES=1 \
        X4VR_LOG="$log" timeout 120 "$BIN" "$VS" "$FS" "$SF" >/dev/null 2>&1
    out=$(cat "$log"); rm -f "$log"
    local id non ang
    id=$(grep -ac 'proj VIEW cam#[0-9]*: M_view IDENTITY' <<<"$out")
    non=$(grep -ac 'proj VIEW cam#[0-9]*: M_view is NOT IDENTITY' <<<"$out")
    ang=$(grep -ao 'NOT IDENTITY -- 14\.[0-9]* deg' <<<"$out" | head -1)
    if [[ "$id" == 1 && "$non" == 1 && -n "$ang" ]]; then
        printf 'ok   %-38s identity=%s non-identity=%s (%s)\n' \
            "view: both M_view paths reported" "$id" "$non" "$ang"
    else
        printf 'FAIL %-38s want identity=1 non-identity=1 with an angle, got %s/%s "%s"\n' \
            "view: both M_view paths reported" "$id" "$non" "$ang"
        grep -a 'proj VIEW\|proj CAMERA' <<<"$out" | sed 's/^/       | /' | head -6
        fails=$((fails + 1))
    fi
    # The offset must survive too. Reading the arena with the wrong storage
    # order still returns a rotated camera and loses the translation entirely
    # -- a wrong answer that looks like a finding -- so the metres are checked,
    # not just the degrees.
    if grep -aq 'offset 8\.70[0-9] m' <<<"$out"; then
        printf 'ok   %-38s offset 8.704 m recovered\n' "view: translation survives"
    else
        printf 'FAIL %-38s offset 8.704 m not reported\n' "view: translation survives"
        fails=$((fails + 1))
    fi
    # Task #56, rewritten at take 201. These cases used to assert
    # `cascade 0 spans 500.0 m ...`, computed from camera-block members 9/10 --
    # `M_shadowCSM0Clip`, which 370 of the 409 dumped modules DECLARE and ZERO
    # sample. The layer was reading a buffer X4 has no reason to maintain, and
    # this case was confirming the arithmetic over a source that is never live:
    # a green test on a dead member.
    #
    # The arithmetic was never wrong, so it moved to tests/view_math.cpp where
    # the matrix is passed directly. What belongs HERE is the refusal: with no
    # live cascade source the layer must report the scalars and decline the
    # cascade, rather than dress up whatever the buffer held as a measurement.
    if grep -aq 'csm0-scale=0 csm1-scale=0  <- csm scales read 0' <<<"$out"; then
        printf 'ok   %-38s scalars kept, cascade declined\n' \
            "shadow: cascade refuses a dead source"
    else
        printf 'FAIL %-38s expected the UNMEASURED refusal\n' \
            "shadow: cascade refuses a dead source"
        grep -a 'proj SHADOW' <<<"$out" | sed 's/^/       | /' | head -4
        fails=$((fails + 1))
    fi
    # ...and it must NOT print the matrix line at all, because printing it
    # implies a measurement was made.
    #
    # This grep names the phrase the CURRENT layer emits. It used to look for
    # 'cascade 0 spans', which the take-202 rewrite deleted -- leaving a guard
    # that could no longer fail, passing on every run for the wrong reason.
    # Whenever that line is reworded again, reword this with it, or the case
    # goes quiet instead of red.
    if ! grep -aq 'object-block member 3 |row0|=' <<<"$out"; then
        printf 'ok   %-38s and prints no span at all\n' \
            "shadow: no span without a source"
    else
        printf 'FAIL %-38s printed a span from an unmaintained member\n' \
            "shadow: no span without a source"
        fails=$((fails + 1))
    fi
    # The fixture POISONS camera-block members 9/10 with 0.009 (a 111 m
    # cascade, produced by nothing else here). They used to hold 0.002 -- the
    # same value the object block now carries -- so reading the wrong member
    # gave exactly the right answer and this case passed for as long as it
    # existed. If the layer ever reads member 9 again, the number names itself.
    if ! grep -aqE 'csm0-scale=0\.009|spans 111' <<<"$out"; then
        printf 'ok   %-38s dead member 9 is not read\n' \
            "shadow: the poison is not reported"
    else
        printf 'FAIL %-38s reported 0.009/111 m — that is camera member 9\n' \
            "shadow: the poison is not reported"
        fails=$((fails + 1))
    fi
}
view_arena_case

# Task #59: the per-frame pose channel, proven rather than merely built.
#
# The identity case is the one that matters most. A channel that never arrives
# and a channel carrying identity produce the SAME picture, so "it looks right"
# is not evidence of anything -- which is why the second case drives a value
# whose absence is visible: a clip-space x offset of 2 pushes the fullscreen
# triangle entirely off screen, and coverage goes to zero.
#
# That pair also caught the real bug in this work. The first version put the
# patch beside the four vertex paths, below `if (!have_k) return` -- so with
# X4VR_POSE and no X4VR_STEREO nothing was patched, the channel came up, every
# counter read OK, and the probe moved nothing. Every report said success.
pose_case() {
    local label="$1" want_drawn="$2"; shift 2
    local log out drawn final
    log="$(mktemp)"
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_POSE=1 "$@" X4VR_LOG="$log" \
        timeout 120 "$BIN" "$VS" "$FS" "$SF" 2>&1)
    drawn=$(sed -n 's/^LAYER0_DRAWN=//p' <<<"$out")
    final=$(grep -ao 'pose final: .*' "$log" | tail -1)
    rm -f "$log"
    if [[ "$drawn" == "$want_drawn" && "$final" == *"OK" ]]; then
        printf 'ok   %-38s drawn=%s\n' "$label" "$drawn"
    else
        printf 'FAIL %-38s want drawn=%s, got %s; final="%s"\n' \
            "$label" "$want_drawn" "${drawn:-?}" "$final"
        fails=$((fails + 1))
    fi
}
pose_case "pose: identity is inert"          1
pose_case "pose: the value arrives"          0 X4VR_POSE_TEST_DX=2

# **This case asserts an EXCLUSION, and it started life asserting the
# opposite.** With a shear configured, a pose-patched module still goes through
# needs_original(), so a pass the shear excludes gets no pose either. Both of
# this harness's passes are unsheared, so the probe moves nothing and drawn
# stays 1 -- and that is correct, not a miss: the exclusions are the shadow
# passes, whose transform is light-space and must never be head-rotated, and
# the UI passes, which are #30's canvas and must stay screen-locked.
#
# The first version of this case expected drawn=0 on the reasoning that
# "delivery and classification answer different questions". They do at the
# have_k gate -- which was a real bug and is fixed above -- and they do NOT at
# the variant selection, where the shear's exclusions are exactly the passes
# #33 must also leave alone. Asserting the log line as well as the number, so
# the case documents the mechanism rather than pinning a value.
pose_excluded_case() {
    local log out drawn
    log="$(mktemp)"
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_POSE=1 X4VR_STEREO=1 X4VR_POSE_TEST_DX=2 \
        X4VR_LOG="$log" timeout 120 "$BIN" "$VS" "$FS" "$SF" 2>&1)
    drawn=$(sed -n 's/^LAYER0_DRAWN=//p' <<<"$out")
    local excl
    # Matches the stable prefix, not the whole sentence. The old form
    # pinned the words "using unpatched modules", which were WRONG (the
    # twin keeps every fragment edit) -- so correcting the layer's message
    # broke this case, and a case that breaks when a false statement is
    # corrected was pinning the wrong thing.
    excl=$(grep -ac 'unsheared pipeline:' "$log")
    rm -f "$log"
    if [[ "$drawn" == 1 && "$excl" -ge 1 ]]; then
        printf 'ok   %-38s unsheared pass takes original\n' \
            "pose: the shear's exclusions hold"
    else
        printf 'FAIL %-38s drawn=%s exclusion-lines=%s\n' \
            "pose: the shear's exclusions hold" "${drawn:-?}" "$excl"
        fails=$((fails + 1))
    fi
}
pose_excluded_case

# ---- task #56: the OBJECT block, at set 3 and a non-1792 range ------------
#
# The layer's camera tracking filters descriptor writes on range == 1792, so a
# block of any other size is invisible to it at EVERY set index -- which is why
# X4VR_VIEW_SET=3 was never going to work. The object path records whatever
# range the write declares and bounds its read by that.
#
# The fixture binds 576 deliberately: if it were 1792 the slot would ride the
# camera path and this would prove nothing.
objblock_case() {
    local log out
    log="$(mktemp)"
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 X4VR_TEST_OBJ_BLOCK=1 \
        X4VR_DUMP_MATRICES=1 X4VR_LOG="$log" timeout 120 "$BIN" 2>&1)
    local logtext; logtext=$(cat "$log"); rm -f "$log"
    # The live member: 0.002 at object member 3, 0.0005 at member 4.
    if grep -aq 'csm0-scale=0.002 csm1-scale=0.0005' <<<"$logtext"; then
        printf 'ok   %-38s member 3 read, member 4 too\n' \
            "objblock: the live cascade is measured"
    else
        printf 'FAIL %-38s want csm0=0.002 csm1=0.0005\n' \
            "objblock: the live cascade is measured"
        grep -a 'proj SHADOW' <<<"$logtext" | sed 's/^/       | /' | head -2
        fails=$((fails + 1))
    fi
    # ...and the arithmetic the old layer case used to assert, now over the
    # member X4 actually samples.
    if grep -aq 'object-block member 3 |row0|=0.002 -> 500.0 m, 1024 texels, 0.4883' \
            <<<"$logtext"; then
        printf 'ok   %-38s 500 m / 1024 texels = 0.4883 m\n' \
            "objblock: the arithmetic survives the move"
    else
        printf 'FAIL %-38s expected 500.0 m / 1024 / 0.4883\n' \
            "objblock: the arithmetic survives the move"
        fails=$((fails + 1))
    fi
    # The range is RECORDED. The camera block proves range is not the struct
    # size (last member at 1556, X4 binds 1792), so nothing predicts 576 and
    # the run has to report what it saw.
    if grep -aq 'set3/range576' <<<"$logtext"; then
        printf 'ok   %-38s set3/range576, observed not assumed\n' \
            "objblock: the bound range is recorded"
    else
        printf 'FAIL %-38s no set3/range576 in the inventory\n' \
            "objblock: the bound range is recorded"
        grep -a 'proj OBJBLOCK' <<<"$logtext" | sed 's/^/       | /' | head -2
        fails=$((fails + 1))
    fi
    # The guard against the assumption this could otherwise smuggle in: that
    # any one object slot speaks for all of them. One slot here, so exactly one
    # distinct scale, and a spread >1 must say so rather than average.
    #
    # The second grep names the CURRENT phrase. It used to look for
    # 'OBJBLOCK: WARNING', which the take-202 rewrite deleted along with the
    # false invariant that message asserted -- so it could no longer fail.
    if grep -aq '1 distinct cascade scale(s) this frame' <<<"$logtext" &&
       ! grep -aq 'distinct |row0| across the object slots' <<<"$logtext"; then
        printf 'ok   %-38s one slot, one scale, no spread note\n' \
            "objblock: the spread guard agrees"
    else
        printf 'FAIL %-38s spread guard disagreed\n' \
            "objblock: the spread guard agrees"
        grep -a 'proj OBJBLOCK' <<<"$logtext" | sed 's/^/       | /' | head -2
        fails=$((fails + 1))
    fi
    # And the poison again, from the arm that DOES measure: proving the number
    # came from set 3 rather than from camera member 9 happening to be right.
    if ! grep -aqE 'csm0-scale=0\.009|spans 111' <<<"$logtext"; then
        printf 'ok   %-38s and still not from member 9\n' \
            "objblock: measured from set 3, not the camera"
    else
        printf 'FAIL %-38s the value came from the poisoned dead member\n' \
            "objblock: measured from set 3, not the camera"
        fails=$((fails + 1))
    fi
}
objblock_case

# ---- take 202's defect: a descriptor write REPLACES, it does not accumulate --
#
# X4 re-points set 3 binding 0 at a fresh sub-allocation per draw -- 13748 binds
# in take 202. The layer appended each write to a per-set vector and credited
# the whole vector on every draw, so by the report it held 167 slots, about half
# of them regions X4 had already recycled. Those read as `csm0-scale=inf`, the
# frame appeared to carry two cascades, and the run FAILed on a defect that was
# entirely the instrument's.
#
# The fixture writes binding 0 twice, at offset 0 then offset 256. Only the
# second is live afterwards, so the one correct answer is 0.008 / 125 m / ONE
# distinct scale. The old code reports both regions and picks by draw count.
objblock_rewrite_case() {
    local log logtext
    log="$(mktemp)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 X4VR_TEST_OBJ_BLOCK=1 \
        X4VR_TEST_OBJ_REWRITE=1 \
        X4VR_DUMP_MATRICES=1 X4VR_LOG="$log" timeout 120 "$BIN" >/dev/null 2>&1
    logtext=$(cat "$log"); rm -f "$log"
    # The SECOND region, and only it.
    if grep -aq 'csm0-scale=0.008 csm1-scale=0.002' <<<"$logtext"; then
        printf 'ok   %-38s latest write wins\n' \
            "objblock: a rewrite replaces the slot"
    else
        printf 'FAIL %-38s want csm0=0.008 (the second write)\n' \
            "objblock: a rewrite replaces the slot"
        grep -a 'proj SHADOW' <<<"$logtext" | sed 's/^/       | /' | head -2
        fails=$((fails + 1))
    fi
    # The superseded region must be GONE, not outvoted. 0.002 at member 3 is
    # the first write's cascade and nothing else here produces it.
    if ! grep -aq 'csm0-scale=0.002 csm1-scale=0.0005' <<<"$logtext"; then
        printf 'ok   %-38s the superseded region is dropped\n' \
            "objblock: no stale slot survives"
    else
        printf 'FAIL %-38s the first write is still being read\n' \
            "objblock: no stale slot survives"
        fails=$((fails + 1))
    fi
    # Two writes, one live binding: still one scale. This is the assertion that
    # take 202 got wrong, and it is what a revert breaks first.
    if grep -aq '1 distinct cascade scale(s) this frame' <<<"$logtext"; then
        printf 'ok   %-38s two writes, one live slot\n' \
            "objblock: rewriting does not inflate the spread"
    else
        printf 'FAIL %-38s a rewrite inflated the spread\n' \
            "objblock: rewriting does not inflate the spread"
        grep -a 'proj OBJBLOCK' <<<"$logtext" | sed 's/^/       | /' | head -2
        fails=$((fails + 1))
    fi
}
objblock_rewrite_case

# ---- task #67: the two camera walks, reported side by side -----------------
#
# `g_track.sets` appends one entry per descriptor write, dedupes on
# (buffer, offset), and is never cleared or capped — so a set handle X4
# re-points at a new arena region keeps every region it has ever held, and
# credit_draw credits all of them on every draw. Take 202 decoded 600 slots
# with 125 of them carrying three distinct projections each.
#
# `sets_live` is the same record with vkUpdateDescriptorSets' actual semantics.
# NOTHING is reported off it yet: the point is to measure the difference on a
# real X4 frame before moving camera attribution underneath settled results.
#
# Two runs, because a comparison that has only ever printed AGREES is not known
# to be able to print anything else.
livesets_case() {
    local log line
    for mode in same rewrite; do
        log="$(mktemp)"
        env VK_ADD_LAYER_PATH="$BUILD/layer" \
            VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
            X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 X4VR_DUMP_MATRICES=1 \
            ${mode:+$([ "$mode" = rewrite ] && echo X4VR_TEST_VIEW_REWRITE=1)} \
            X4VR_LOG="$log" timeout 120 "$BIN" >/dev/null 2>&1
        line=$(grep -a -o 'LIVESETS: accumulating [0-9]* slot(s), winner offset=[0-9]* draws=[0-9]* | superseding [0-9]* slot(s), winner offset=[0-9]* draws=[0-9]* | winner [A-Z]*' "$log" | head -1)
        rm -f "$log"
        local acc sup verdict
        acc=$(sed -n 's/.*accumulating \([0-9]*\) slot.*/\1/p' <<<"$line")
        sup=$(sed -n 's/.*superseding \([0-9]*\) slot.*/\1/p' <<<"$line")
        verdict=${line##*winner }
        if [ "$mode" = same ]; then
            # Written once each, the two records ARE the same. If this run
            # disagrees, the instrument invents differences and nothing it
            # says about X4 can be trusted.
            if [ -n "$line" ] && [ "$acc" = "$sup" ] && [ "$verdict" = AGREES ]; then
                printf 'ok   %-38s %s slots both ways, AGREES\n' \
                    "livesets: no rewrite, no difference" "$acc"
            else
                printf 'FAIL %-38s want equal counts and AGREES\n' \
                    "livesets: no rewrite, no difference"
                printf '       | %s\n' "${line:-<no LIVESETS line at all>}"
                fails=$((fails + 1))
            fi
        else
            # One binding rewritten to a region nothing else names: the stale
            # entry must be dropped AND must stop winning.
            if [ -n "$line" ] && [ "$sup" -lt "$acc" ] && [ "$verdict" = DIFFERS ]; then
                printf 'ok   %-38s %s -> %s slots, winner moves\n' \
                    "livesets: a rewrite is seen to differ" "$acc" "$sup"
            else
                printf 'FAIL %-38s want superseding < accumulating, DIFFERS\n' \
                    "livesets: a rewrite is seen to differ"
                printf '       | %s\n' "${line:-<no LIVESETS line at all>}"
                fails=$((fails + 1))
            fi
        fi
    done
}
livesets_case


# With the channel unset, none of it exists: no patch, no set, no line.
pose_off_case() {
    local log out n drawn
    log="$(mktemp)"
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_LOG="$log" timeout 120 "$BIN" "$VS" "$FS" "$SF" 2>&1)
    n=$(grep -ac 'pose' "$log")
    drawn=$(sed -n 's/^LAYER0_DRAWN=//p' <<<"$out")
    rm -f "$log"
    if [[ "$n" == 0 && "$drawn" == 1 ]]; then
        printf 'ok   %-38s no pose line, frame unchanged\n' \
            "pose: unset changes nothing"
    else
        printf 'FAIL %-38s %s pose line(s), drawn=%s\n' \
            "pose: unset changes nothing" "$n" "$drawn"
        fails=$((fails + 1))
    fi
}
pose_off_case

# Task #33's conjugation, on committed bytes. sample_camera_block.vert has the
# camera block at set 1 binding 0 with M_projection as member 1, which is the
# shape the patch verifies before it emits anything; fullscreen.vert has no
# camera block at all and must REFUSE.
#
# The refusal is half the test. A module with no camera block cannot be given a
# rotation at all -- a baked one would be the wrong geometry, not a degraded
# rotation -- so "refused" is the correct answer for the 43 of X4's 409 modules
# that are in that class, and it must not read as a failure.
pose_rotate_patch_case() {
    local label="$1" src="$2" want="$3" out spv
    spv="$(mktemp).spv"
    out=$("$BUILD/tests/x4vr_test_spirv_patch" vert-pose-rotate "$src" "$spv" 2>&1)
    local got="${out%% *}"
    local valid=skipped
    if [[ "$got" == "REFUSED=0" ]]; then
        spirv-val "$spv" >/dev/null 2>&1 && valid=ok || valid=INVALID
    fi
    rm -f "$spv"
    if [[ "$got" == "$want" && "$valid" != INVALID ]]; then
        printf 'ok   %-38s %s spirv-val=%s\n' "$label" "$got" "$valid"
    else
        printf 'FAIL %-38s want %s, got "%s" spirv-val=%s\n' \
            "$label" "$want" "$out" "$valid"
        fails=$((fails + 1))
    fi
}
pose_rotate_patch_case "rotate: camera block is patched" \
    tests/sample_camera_block.vert.spv REFUSED=0
pose_rotate_patch_case "rotate: no camera block refuses" \
    tests/fullscreen.vert.spv REFUSED=1
# Task #62: the object-block fallback. sample_light_volume.vert declares the
# set-3 per-object block and no camera block -- the shape of the 14 World
# modules that would otherwise sit still while everything around them turned.
pose_rotate_patch_case "rotate: object block is the fallback" \
    tests/sample_light_volume.vert.spv REFUSED=0

# Task #61: R^-1 into the deferred reconstruction. sample_invproj.frag is the
# committed stand-in for X4's reconstructing modules; over the real dumps this
# patches 244 and refuses 165, and 244 is exactly the count of fragment entry
# points that load member 2 (tools/uniform_block_scan.py).
frag_pose_case() {
    local label="$1" src="$2" want="$3" out spv valid=skipped
    spv="$(mktemp).spv"
    out=$("$BUILD/tests/x4vr_test_spirv_patch" frag-invproj-pose "$src" "$spv" 2>&1)
    local got="${out%% *}"
    if [[ "$got" == "REFUSED=0" ]]; then
        spirv-val "$spv" >/dev/null 2>&1 && valid=ok || valid=INVALID
    fi
    rm -f "$spv"
    if [[ "$got" == "$want" && "$valid" != INVALID ]]; then
        printf 'ok   %-38s %s spirv-val=%s\n' "$label" "$got" "$valid"
    else
        printf 'FAIL %-38s want %s, got "%s" spirv-val=%s\n' \
            "$label" "$want" "$out" "$valid"
        fails=$((fails + 1))
    fi
}
frag_pose_case "rinv: reconstruction takes R-inverse" \
    tests/sample_invproj.frag.spv REFUSED=0
frag_pose_case "rinv: a non-reconstructor refuses" \
    tests/sample.frag.spv REFUSED=1

# Task #60: the conjugation EXECUTING, on a real GPU. Everything above says the
# emitted module is well-formed; none of it says the arithmetic is the
# conjugation. This does, and it does it with a discriminator that the OTHER
# implementation fails:
#
#   180 deg of yaw, conjugation -> v'z = -vz, so w' = -1 for every vertex at
#                                  w = 1. Everything is behind the eye and
#                                  clips. Nothing draws.
#   180 deg of yaw, #59 transport form -> mirrors x and z, w untouched, the
#                                  triangle still covers the screen.
#
# So drawn=0 is evidence for the conjugation specifically, not for "a transform
# happened". A test that only asserted "the picture changed" would pass on the
# wrong implementation.
pose_exec_case() {
    local label="$1" want="$2"; shift 2
    local out got
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 X4VR_POSE=1 "$@" \
        X4VR_LOG=/dev/null timeout 120 "$BIN" "$VS" "$FS" "$SF" 2>&1)
    got=$(sed -n 's/^SAMPLED_NONZERO=//p' <<<"$out")
    if [[ "$got" == "$want" ]]; then
        printf 'ok   %-38s output drawn=%s\n' "$label" "$got"
    else
        printf 'FAIL %-38s want drawn=%s, got %s\n' "$label" "$want" "${got:-?}"
        fails=$((fails + 1))
    fi
}
pose_exec_case "exec: yaw 0 is the control"    1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=0
pose_exec_case "exec: yaw 30 still covers"     1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=30
pose_exec_case "exec: yaw 180 clips every vertex" 0 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=180
# The same angle through #59's transport form must NOT clip. This is the case
# that makes the one above mean something.
pose_exec_case "exec: transport form does not"  1 X4VR_POSE_YAW=180

# --- the width of X4's own pipeline layout -----------------------------------
#
# **Every case above passed while take 186b rendered garbage.** They were all
# run against a pipeline layout declaring one or two descriptor sets, so the
# pose channel's set index always fit. X4's main layout -- carrying 1109 of its
# 1135 pipelines -- declares SIX, the index was 5, and X4 owned it. Our patched
# modules read X4's set 5 as a matrix, and our 2.3M binds at index 5 disturbed
# X4's own binding there for X4's own draws.
#
# The index was chosen by reading the 409 dumped SHADER MODULES and observing
# they only ever declare sets 0-4. A module declares the sets it READS; a
# pipeline layout declares the sets X4 BINDS. Wrong object, and no case here
# could tell, because no case here was as wide as the real thing.
#
# X4VR_TEST_PAD_SETS makes the harness build at X4's width. The pair below is a
# discriminator: identical in every respect except the set index, and the
# observable flips. That is what distinguishes "the fix works" from "the
# configuration was never hard enough to fail".
pose_width_case() {
    local label="$1" want_drawn="$2" want_collide="$3"; shift 3
    local log out drawn final collide verdict
    log="$(mktemp)"
    out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 X4VR_POSE=1 X4VR_TEST_PAD_SETS=6 \
        "$@" X4VR_LOG="$log" timeout 120 "$BIN" "$VS" "$FS" "$SF" 2>&1)
    drawn=$(sed -n 's/^SAMPLED_NONZERO=//p' <<<"$out")
    final=$(grep -ao 'pose final: .*' "$log" | tail -1)
    collide=$(grep -c 'pose: COLLISION' "$log")
    rm -f "$log"
    # A collision must read FAIL and a clean run must read OK -- the counters
    # and the collision flag have to agree, because 186b's scorer passed a run
    # whose layer had already printed FAIL.
    if [[ "$want_collide" == 1 ]]; then
        verdict=$([[ "$final" == *FAIL* ]] && echo y || echo n)
    else
        verdict=$([[ "$final" == *OK ]] && echo y || echo n)
    fi
    if [[ "$drawn" == "$want_drawn" && "$collide" == "$want_collide" &&
          "$verdict" == y ]]; then
        printf 'ok   %-38s drawn=%s collisions=%s\n' "$label" "$drawn" "$collide"
    else
        printf 'FAIL %-38s want drawn=%s collisions=%s; got drawn=%s collisions=%s final="%s"\n' \
            "$label" "$want_drawn" "$want_collide" "${drawn:-?}" "$collide" "$final"
        fails=$((fails + 1))
    fi
}
# The derived index sits at maxBoundDescriptorSets-1, so a 6-set layout is no
# obstacle and the conjugation still clips at 180 deg exactly as it does at
# width 2. This is the case that fails if the index ever goes back to a guess.
pose_width_case "width: 6-set layout, derived index" 0 0 \
    X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=180
# Take 186b, reproduced. Forcing the old index against X4's real width must
# collide -- and the collision must UN-ROTATE the geometry, not corrupt it:
# fail-closed stops the patch, so the output pass draws normally. drawn=1 here
# against drawn=0 above is the whole discriminator.
pose_width_case "width: index 5 collides, fails closed" 1 1 \
    X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=180 X4VR_POSE_SET=5

# --- bind-time attribution: which pass USED a module, not which CREATED it ---
#
# Take 192 spent a run on the pipe inventory and answered the wrong question.
# A pipeline records the render pass it was CREATED against and Vulkan permits
# it to be used with any COMPATIBLE pass, so X4 creates against rp #26/#28/#30
# and draws through #27/#29/#31 -- and a creation-keyed inventory named the
# three passes that never execute while missing the three that do.
#
# X4VR_TEST_XRP builds exactly that: one pipeline, created against `rp`, bound
# and drawn inside `rp_alias`, a distinct but compatible pass. A creation-keyed
# record attributes the draw to `rp`; a bind-time record attributes it to
# `rp_alias`. They disagree by construction.
#
# The assertion is on the ALIAS's serial, because that is the one only a
# bind-time record can produce. This case fails on every build that does not
# have one -- which is the whole reason it is written before the feature.
bindtime_case() { # label
    local label="$1" log on off
    # Distinct render passes that BOUND a pipeline, with the alias and without.
    # A discriminator rather than a fixed serial: the alias is created straight
    # after `rp`, so hard-coding "the last pass" was wrong once already -- it is
    # serial 1, not the highest. Counting the SET removes the guess.
    count_binds() {
        log="$(mktemp)"
        env VK_ADD_LAYER_PATH="$BUILD/layer" \
            VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
            X4VR_MV=1 X4VR_PIPE_INVENTORY=1 X4VR_MV_INVENTORY=1 "$@" \
            X4VR_LOG="$log" timeout 120 "$BIN" >/dev/null 2>&1
        grep -ao 'bind rp #[0-9]*' "$log" | sort -u | wc -l
        rm -f "$log"
    }
    off=$(count_binds)
    on=$(count_binds X4VR_TEST_XRP=1)
    if [[ "$on" -eq $((off + 1)) ]]; then
        printf 'ok   %-38s %s pass(es) bind, %s with the alias\n' "$label" "$off" "$on"
    else
        printf 'FAIL %-38s wanted %s+1 binding passes with the alias, got %s\n' \
            "$label" "$off" "$on"
        fails=$((fails + 1))
    fi
}
bindtime_case "bind-time: alias pass is attributed"

# **The case above counts PASSES and says nothing about CONTENT**, so it passed
# on the build that produced take 193 -- where all 179 bind lines read
# "(no inventory entry)" because the description was keyed on `out[i]` before
# vkCreateGraphicsPipelines had filled it. Proving attribution is not proving
# the join. This asserts a bind line actually RESOLVES to a module.
bindtime_detail_case() {
    local label="$1" log n_bind n_detail
    log="$(mktemp)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_MV=1 X4VR_STEREO=1 X4VR_TEST_XRP=1 X4VR_PIPE_INVENTORY=1 \
        X4VR_MV_INVENTORY=1 X4VR_LOG="$log" timeout 120 "$BIN" \
        "$VS" "$FS" "$SF" >/dev/null 2>&1
    n_bind=$(grep -ac 'bind rp #' "$log")
    n_detail=$(grep -a 'bind rp #' "$log" | grep -c 'mod#')
    rm -f "$log"
    if [[ "${n_bind:-0}" -gt 0 && "${n_detail:-0}" -gt 0 ]]; then
        printf 'ok   %-38s %s/%s bind line(s) name a module\n' \
            "$label" "$n_detail" "$n_bind"
    else
        printf 'FAIL %-38s %s bind line(s), %s naming a module\n' \
            "$label" "${n_bind:-0}" "${n_detail:-0}"
        fails=$((fails + 1))
    fi
}
bindtime_detail_case "bind-time: the line names a module"

# --- the round-robin's give-up, which had no test and cost take 179 ---------
#
# The three verdict cases below use the fixture 0,1,5 -- the CAPTURABLE target
# FIRST -- so the index always advances and nothing can ever starve. They passed
# for months over a give-up that did not exist, and then over one that could not
# fire.
#
# This pair is the discriminator: same list, same targets, ONE knob apart.
#   1 frame   -> rp #0 is starved by the unreachable rp #5 ahead of it
#   12 frames -> the give-up abandons #5 and #0 is captured
#
# It needs X4VR_TEST_FRAMES because a single-frame harness can show starvation
# but never recovery: every pass ends exactly once, so a target reached only
# after the round-robin moves on has no second chance. That is why the fix and
# the test had to land together.
starve_case() { # label frames want_substring_for_rp0
    local label="$1" frames="$2" want="$3"
    local log pre line
    log="$(mktemp)"; pre="$(mktemp -u)"
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 \
        X4VR_MV_DUMP_AFTER_RP=5,0 X4VR_TEST_FRAMES="$frames" \
        X4VR_MV_DUMP_AFTER_RP_EVERY=2 X4VR_MV_DUMP="$pre" X4VR_LOG="$log" \
        timeout 180 "$BIN" >/dev/null 2>&1
    line=$(grep -a "target rp #0:" "$log" | head -1 | sed 's/.*target rp/target rp/')
    rm -f "$log" "$pre"-*
    if [[ "$line" == *"$want"* ]]; then
        printf 'ok   %-38s %s\n' "$label" "$(echo "$line" | cut -c1-40)"
    else
        printf 'FAIL %-38s want "%s", got: %s\n' "$label" "$want" "${line:-no verdict}"
        fails=$((fails + 1))
    fi
}
starve_case "round-robin: one frame starves rp #0"  1  "NEVER REACHED"
starve_case "round-robin: give-up recovers rp #0"   12 "captured"

verdict_case "verdict: captured"        0 "captured"
verdict_case "verdict: reached, no file" 1 "REACHED but never captured"
verdict_case "verdict: never reached"   5 "NEVER REACHED"
# NOT COVERED HERE: the fourth verdict, "SKIPPED -- no doubled colour
# attachment", and the give-up that advances the round-robin past such a
# target. Reaching it needs a pass that this harness EXECUTES whose framebuffer
# has no doubled attachment, and the harness executes exactly two passes, both
# with one. rp #3 and #5 are created for classification and destroyed unused.
# Say so plainly: an un-run path listed among passing cases reads as verified.

probe_win_case "probe window: unset = full image"  ""   128
probe_win_case "probe window: 0 = unlimited"       0    128
probe_win_case "probe window: 64 KB halves it"     64   64

probe_cap_case "probe cap: unset means unlimited"  ""  0
probe_cap_case "probe cap: 0 means unlimited"      0   0
probe_cap_case "probe cap: 1 stops the probe"      1   1

# --- validation cleanliness, asserted rather than assumed --------------------
#
# **This suite is cited as proof that a patch is validation-clean, and until
# now it never enabled the validation layer.** The only mention of VUID in this
# file was a grep inside a failure branch, which reports VUIDs for a case that
# already failed for another reason and says nothing about a passing one. The
# accepted method — compare the VUID set with a feature on against off — was
# being run by hand, and by hand it missed that the harness had a PERMANENT
# VUID-vkCmdDraw-None-08114: the bindless table at set 0 binding 0 declares
# eight descriptors and writes four, and an ordinary array binding promises all
# eight are valid. Fixed at the source with PARTIALLY_BOUND (which is what X4
# must declare for its 53306-entry table anyway), and pinned here so it cannot
# come back.
#
# A standing baseline error is worse than a noisy one: "unchanged" then reads as
# clean, anything new sharing the ID is hidden, and readers learn to skip VUID
# lines. So the assertion is an EMPTY set, not a matching set.
#
# Cost is ~1 ms per case, measured, so the configuration list is chosen for
# coverage rather than for speed.
#
# SCOPE, stated so nobody reads more into a green block than it earns: these
# cases draw with the stock `$SF`. The other shaders the suite DRAWS -- the twin
# pair, the patched variants, sample_combined -- were checked clean by hand.
# sample_two, sample_bindless, sample_invproj and sample_alias_binding are NOT
# clean if drawn (VkGraphicsPipelineCreateInfo-layout-07988: they declare
# bindings this harness's pipeline layout does not), and that is fine only
# because they never are: they go to patch_case / idx_case / inv_case, which
# run the SPIR-V CLI and spirv-val and never create a device. Hand one of them
# to frag_case or mirror_case and it needs a matching layout first.
validation_manifest() {
    local d f
    local -a dirs=()
    IFS=':' read -r -a dirs <<<"${VK_LAYER_PATH:-}"
    dirs+=("${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/explicit_layer.d"
           /usr/local/share/vulkan/explicit_layer.d
           /usr/share/vulkan/explicit_layer.d
           /etc/vulkan/explicit_layer.d)
    for d in "${dirs[@]}"; do
        [[ -n "$d" ]] || continue
        for f in "$d"/VkLayer_khronos_validation.json; do
            [[ -e "$f" ]] && return 0
        done
    done
    return 1
}

# The presence check is NOT optional politeness. VK_LOADER_LAYERS_ENABLE is a
# glob-enable: naming a layer that is not installed is silently ignored, the run
# succeeds, and the VUID set comes back empty. That is a pass produced by the
# absence of the instrument -- so without a positive control this whole block
# would be a check that cannot fail.
if ! validation_manifest; then
    printf 'skip %-38s validation layer not installed — these cases assert\n' \
        "validation: (all)"
    printf '     %-38s nothing, they did not pass\n' ""
else
    vuid_case() {
        local label="$1"; shift
        local out vuids
        out=$(env VK_ADD_LAYER_PATH="$BUILD/layer" \
            VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation,VK_LAYER_X4VR_core \
            "$@" X4VR_LOG=/dev/null timeout 180 "$BIN" "$VS" "$FS" "$SF" 2>&1)
        vuids=$(grep -oE 'VUID-[A-Za-z0-9-]+' <<<"$out" | sort -u | tr '\n' ' ')
        if [[ -z "$vuids" ]]; then
            printf 'ok   %-38s no VUIDs\n' "$label"
        else
            printf 'FAIL %-38s %s\n' "$label" "$vuids"
            grep -m2 -A1 'Validation Error' <<<"$out" | sed 's/^/       | /'
            fails=$((fails + 1))
        fi
    }
    vuid_case "validation: plain"          X4VR_MV=1
    vuid_case "validation: view arena"     X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1
    vuid_case "validation: bindless mirror" X4VR_MV=1 X4VR_BINDLESS_MIRROR=1 \
        X4VR_MIRROR_OFFSET=4
    vuid_case "validation: stereo shear"   X4VR_MV=1 X4VR_STEREO=1 X4VR_IPD=0.064
    vuid_case "validation: pose rotate"    X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 \
        X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=10
    # X4's real layout width, and the collision path -- the two configurations
    # take 186b proved the harness had never run.
    vuid_case "validation: X4's 6-set width" X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 \
        X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_TEST_PAD_SETS=6
    vuid_case "validation: pose collision"  X4VR_MV=1 X4VR_TEST_VIEW_ARENA=1 \
        X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_TEST_PAD_SETS=6 X4VR_POSE_SET=5
    # FSR 3's right-eye pass (take 382): twin sets, layer-1 views, the
    # history's copy and the barriers repeated on it. Checked by hand that this
    # catches a missing one: without the repeated barriers it reports
    # VUID-vkCmdDispatch-imageLayout-00344.
    FS="$VCF" vuid_case "validation: FSR 3 both eyes" X4VR_MV=1 X4VR_MV_MASK=3 \
        X4VR_BINDLESS_MIRROR=1 X4VR_RES=64x64 X4VR_TEST_COMPUTE=ffx
    vuid_case "validation: FSR 1 both eyes" X4VR_MV=1 X4VR_MV_MASK=3 \
        X4VR_BINDLESS_MIRROR=1 X4VR_RES=64x64 X4VR_MIRROR_OFFSET=32 \
        X4VR_TEST_COMPUTE=fsrbindless
fi

# --- take 357: the cinematic view's twin pipelines ------------------------
#
# In F6 the layer binds each canvas pipeline's twin (built with the module
# the non-canvas path gives) so X4's overlay is drawn as X4 laid it out.
# ui_attr.vert has one (unfed) vertex input, so the layer calls it UI and
# builds its canvas variant -- fullscreen.vert is procedural and gets none.
# X4VR_TEST_CINEMATIC=1 stands in for the injector's flag. Asserted: the
# twin is built in both arms, BOUND only in the cinematic one, and adds no
# validation error against the control arm (the unfed input costs both arms
# the same VUIDs). With the pose channel on, as in the shipping line.
UIVS="$BUILD/tests/ui_attr.vert.spv"
CV_ERRS=0
CV_LOG=""
cv_run() {
    CV_LOG="$(mktemp)"
    CV_ERRS=$(env X4VR_LOG="$CV_LOG" X4VR_MV=1 X4VR_STEREO=1 X4VR_CANVAS_M=8 \
        X4VR_MV_INVENTORY=1 X4VR_POSE=1 "$@" \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
        timeout 120 "$BIN" "$UIVS" "$FS" "$SF" 2>&1 |
        grep -c 'Validation Error' || true)
}
cv_run
cv_off=$CV_ERRS
cv_off_built=$(grep -c 'ui twin: 1 built' "$CV_LOG" || true)
cv_off_bound=$(grep -c 'ui twin: first twin BOUND' "$CV_LOG" || true)
rm -f "$CV_LOG"
cv_run X4VR_TEST_CINEMATIC=1
cv_on=$CV_ERRS
cv_on_built=$(grep -c 'ui twin: 1 built' "$CV_LOG" || true)
cv_on_bound=$(grep -c 'ui twin: first twin BOUND' "$CV_LOG" || true)
rm -f "$CV_LOG"
if [[ "$cv_off_built" == 1 && "$cv_on_built" == 1 ]]; then
    printf 'ok   %-38s one twin per canvas pipeline, both arms\n' "cinematic: twin built"
else
    printf 'FAIL %-38s want 1/1 built, got %s/%s\n' "cinematic: twin built" \
        "$cv_off_built" "$cv_on_built"
    fails=$((fails + 1))
fi
if [[ "$cv_off_bound" == 0 && "$cv_on_bound" == 1 ]]; then
    printf 'ok   %-38s bound only while cinematic\n' "cinematic: twin bound"
else
    printf 'FAIL %-38s want 0 bound off, 1 on; got %s/%s\n' \
        "cinematic: twin bound" "$cv_off_bound" "$cv_on_bound"
    fails=$((fails + 1))
fi
if [[ "$cv_on" == "$cv_off" ]]; then
    printf 'ok   %-38s %s VUID(s) with, %s without — adds none\n' \
        "cinematic: validation-clean" "$cv_on" "$cv_off"
else
    printf 'FAIL %-38s %s VUID(s) cinematic, %s control\n' \
        "cinematic: validation-clean" "$cv_on" "$cv_off"
    fails=$((fails + 1))
fi

# **The guard was at line 1441 and three case groups ran after it**, counting
# failures into a variable nothing looked at again before the unconditional
# "all cases passed" below. Those cases could not fail. Checked here, at the
# end, which is the only place a suite-wide verdict means anything.
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
echo "all cases passed"
