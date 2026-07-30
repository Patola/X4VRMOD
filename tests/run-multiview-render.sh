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


# The predicate split: "does K apply?" and "does this replicate?" used to be
# one question, and are no longer.
#
# rp #1 in this test is X4's tonemap in miniature -- a single colour
# attachment, LDR, consuming the per-eye chain -- which stage 1 left unmasked
# because the two verdicts were the same. It must now be maskable *without*
# becoming sheared, so the assertion is the whole line: MONO (no K) and
# +MASKED (replicates) at once. Either half alone would pass while the other
# regressed.
#
# The third case is the load-bearing one. It proves the carve-out keys on the
# SRGB format and not on "LDR" in general, which is the difference between
# masking X4's tonemap (#103, a normal doubled image) and masking the final
# blit, whose attachment is the presented image and cannot take a second array
# layer at all.
mask_case() {
    local label="$1" want="$2"; shift 2
    local out cls fb
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 X4VR_MV_INVENTORY=1 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$SF" 2>&1)
    if grep -q 'rp #1\.0:.*-> MONO (all-LDR/UI) +MASKED' <<<"$out"; then
        cls=masked
    elif grep -q 'rp #1\.0:.*-> MONO (all-LDR/UI)' <<<"$out"; then
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

mask_case "tonemap masks when SRGB"      masked "X4VR_TEST_OUT_SRGB=1" "X4VR_MASK_TONEMAP=1"
mask_case "...but not without the knob"  mono   "X4VR_TEST_OUT_SRGB=1"
mask_case "...and not for UNORM LDR"     mono   "X4VR_MASK_TONEMAP=1"
mask_case "LDR pass unmasked by default" mono


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
        "$BIN" "$VS" "$FS" "$shader" 2>&1)
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
        spirv-val "$PATCHDIR/out.spv" >/dev/null 2>&1 && val=OK || val=BAD
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
mirror_case() {
    local label="$1" want1="$2" shader="$3"; shift 3
    local out g1
    out=$(env "$@" X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=2 X4VR_TEST_OUT_SRGB=1 \
        X4VR_MASK_TONEMAP=1 X4VR_BINDLESS_MIRROR=1 X4VR_MIRROR_OFFSET=4 \
        "VK_ADD_LAYER_PATH=$BUILD/layer" \
        "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
        "$BIN" "$VS" "$FS" "$BUILD/tests/$shader" 2>&1)
    g1=$(sed -n 's/^OUT1_NONZERO=//p' <<<"$out")
    if [[ "$g1" == "$want1" ]]; then
        printf 'ok   %-38s out1=%s\n' "$label" "$g1"
    else
        printf 'FAIL %-38s want out1=%s, got out1=%s\n' "$label" "$want1" \
            "${g1:-?}"
        grep -E "^(OUT|FAIL)|bindless mirror|VUID" <<<"$out" | sed 's/^/       | /' |
            head -8
        fails=$((fails + 1))
    fi
}

mirror_case "mirror puts layer 1 in the twin"  1 sample_twin.frag.spv
mirror_case "...and leaves slot 0 on layer 0"  0 sample_twin_base.frag.spv

# The accounting, because the pair above would also pass if the mirror wrote the
# twin by some accident of aliasing. Four written descriptors, four twins, all
# four substituted to layer 1, nothing skipped.
mirror_stat=$(env X4VR_LOG= X4VR_MV=1 X4VR_MV_MASK=2 X4VR_TEST_OUT_SRGB=1 \
    X4VR_MASK_TONEMAP=1 X4VR_BINDLESS_MIRROR=1 X4VR_MIRROR_OFFSET=4 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "$BIN" "$VS" "$FS" "$BUILD/tests/sample_twin.frag.spv" 2>&1 |
    sed -n 's/.*bindless mirror final: offset 4, \(.*\)$/\1/p' | head -1)
if [[ "$mirror_stat" == "1 twin writes, 4 twin descriptors, 4 of them layer-1, 0 skipped for no room" ]]; then
    printf 'ok   %-38s %s\n' "mirror accounts for every twin" "$mirror_stat"
else
    printf 'FAIL %-38s got "%s"\n' "mirror accounts for every twin" \
        "${mirror_stat:-ABSENT}"
    fails=$((fails + 1))
fi

# The bounds check. At the default offset the 8-slot table has no room for a
# twin, so the mirror must decline rather than write off the end -- and must say
# it declined. Uses the non-array shader, which reads only slot 0, so nothing
# reads an unwritten descriptor.
mirror_room=$(env X4VR_LOG= X4VR_MV=1 X4VR_BINDLESS_MIRROR=1 \
    "VK_ADD_LAYER_PATH=$BUILD/layer" \
    "VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core" \
    "$BIN" "$VS" "$FS" "$SF" 2>&1 |
    sed -n 's/.*bindless mirror final: offset 26653, \(.*\)$/\1/p' | head -1)
if [[ "$mirror_room" == "0 twin writes, 0 twin descriptors, 0 of them layer-1, 1 skipped for no room" ]]; then
    printf 'ok   %-38s %s\n' "mirror declines a table with no room" \
        "${mirror_room##*, }"
else
    printf 'FAIL %-38s got "%s"\n' "mirror declines a table with no room" \
        "${mirror_room:-ABSENT}"
    fails=$((fails + 1))
fi

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

echo
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
echo "all cases passed"
