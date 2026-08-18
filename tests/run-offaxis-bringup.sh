#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Does the off-axis target survive X4's bring-up ORDER?
#
# This exists because takes 166b and 167b were both nulls, for two different
# reasons, and both were questions about ordering rather than about maths:
#
#   166b  the target was latched at the first shader module that needed one,
#         which task #39 moved EARLIER than the vertex patch -- early enough
#         that X4 was compiling shaders before the XrSession existed. Latched
#         OFF, silently, and A and B looked identical in the headset.
#
#   167b  the bounded wait added to fix that deadlocked on itself: it blocked
#         the thread that calls vkGetDeviceQueue, which is what spawns the
#         session, so the session started 1 ms AFTER the wait gave up.
#
#   168b  starting the session FROM the latch did not help either: the thread
#         was spawned and then executed nothing for five seconds, because
#         vr_session_thread re-enters the Vulkan loader and X4's thread was
#         parked inside vkCreateShaderModule. The race cannot be won from
#         inside a loader chain call, only avoided -- so the runtime source is
#         now expected to lose it, and says so with a way out.
#
# THIS SUITE GAVE A FALSE GREEN ONCE, which is worth more than any case below:
# with the fix deleted it passed all eight, because it grepped a log line that
# printed either way, and because the harness called vkGetDeviceQueue before
# any shader module -- not X4's order, and the whole substance of the bug.
# X4VR_TEST_EARLY_SHADER is what makes the ordering real. A suite for a race
# has to be shown to fail.
#
# Neither needed a headset to find. Both needed a run, because nothing in the
# suite drove the layer's bring-up path at all -- so this drives it, against
# the real runtime when one is up, in about a tenth of a second.
#
# The measurement that matters is TIME as much as content: a wait that works by
# stalling is not working.
set -uo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_multiview_render"
LOGDIR="$(mktemp -d)"
trap 'rm -rf "$LOGDIR"' EXIT

[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

fails=0

# $1 label, $2 log name, rest: extra env. Prints elapsed seconds.
drive() {
    local label="$1" log="$2"; shift 2
    local t0 t1
    t0=$(date +%s%N)
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=1 X4VR_OFFAXIS=runtime X4VR_STEREO=1 X4VR_PROJ_LIVE=1 \
        X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_MV=1 \
        X4VR_TEST_EARLY_SHADER=1 X4VR_LOG="$LOGDIR/$log" "$@" \
        "$BIN" "$BUILD/tests/fullscreen.vert.spv" \
        "$BUILD/tests/solid.frag.spv" "$BUILD/tests/sample.frag.spv" \
        >/dev/null 2>&1
    t1=$(date +%s%N)
    echo $(( (t1 - t0) / 1000000 ))
}

say() { # label ok? detail
    if [[ "$2" == 1 ]]; then
        printf 'ok   %-42s %s\n' "$1" "$3"
    else
        printf 'FAIL %-42s %s\n' "$1" "$3"
        fails=$((fails + 1))
    fi
}

# --- no runtime at all -----------------------------------------------------
#
# The cap must not be spent. vr_session_thread returns silently from four
# different failure paths, and before session_settled existed this case took
# 5.014 s -- a five-second stall on every machine with no headset, bought by a
# fix for a ten-millisecond race.
ms=$(drive "no runtime" nort.log XR_RUNTIME_JSON=/nonexistent/runtime.json)
say "no runtime: does not spend the cap" \
    "$([[ $ms -lt 2000 ]] && echo 1 || echo 0)" "${ms} ms"
grep -q "no view had been located" "$LOGDIR/nort.log" && ok=1 || ok=0
say "no runtime: says so, and refuses" "$ok" "logged"

# --- a live runtime --------------------------------------------------------
#
# Skipped rather than failed when nothing is up, and SAID rather than passed
# silently: active_runtime.json only exists while WiVRn or SteamVR is running,
# so a green run on a machine with the headset off would otherwise be reporting
# on a case it never executed.
if [[ -e "$HOME/.config/openxr/1/active_runtime.json" ]] ||
   [[ -n "${XR_RUNTIME_JSON:-}" && -e "${XR_RUNTIME_JSON:-}" ]]; then
    ms=$(drive "live runtime" rt.log)
    say "runtime source: does not stall" \
        "$([[ $ms -lt 2000 ]] && echo 1 || echo 0)" "${ms} ms"

    # The runtime source is EXPECTED to lose the race, so that is what is
    # asserted -- and that its refusal names the way out, because a refusal
    # nobody can act on is how three takes were spent.
    grep -q 'Use X4VR_OFFAXIS=' "$LOGDIR/rt.log" && ok=1 || ok=0
    say "runtime source: refusal names the way out" "$ok" "explicit angles"

    # Task #40's canvas comes up with it, and on the numbers view_math pins for
    # these frusta. Asserting the VALUES and not merely the line is what
    # separates "a canvas was built" from "a canvas was built correctly" --
    # A_x is the one that would silently be 1.0155 if the screen field were
    # taken from assumed_proj_sx() instead of X4VR_FOV.
    ms=$(drive "explicit angles" oa.log X4VR_OFFAXIS="-54,40,44,-55")
    say "explicit angles: latch is immediate" \
        "$([[ $ms -lt 2000 ]] && echo 1 || echo 0)" "${ms} ms"
    grep -q "offaxis: target from X4VR_OFFAXIS" "$LOGDIR/oa.log" && ok=1 || ok=0
    say "explicit angles: the affine is ON" "$ok" "no runtime needed"
    dis=$(cat "$LOGDIR/oa.log")
    [[ "$dis" == *"A_x=1.2892 A_y=1.1931"* ]] && ok=1 || ok=0
    say "explicit angles: canvas A_x/A_y are view_math's" "$ok" "1.2892 / 1.1931"
    [[ "$dis" == *"eye0 x offset +0.24251 eye1 -0.24251"* ]] && ok=1 || ok=0
    say "explicit angles: canvas B_x mirrors per eye" "$ok" "+/-0.24251"
else
    printf 'skip %-42s %s\n' "live runtime cases" \
        "no active_runtime.json — start WiVRn/SteamVR to run these"
fi

echo
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
echo "all cases passed"
