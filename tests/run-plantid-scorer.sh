#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# **score_run.py's plantid verdict, driven against known plants.**
#
# The scorer's plantid block carried a comment saying "tests/plantid.cpp checks
# that it does, against simulated plants of each kind." It did not. That file
# checked its OWN statistics, computed from its own in-memory trace, and never
# produced a log line the scorer could read -- two separate pieces of arithmetic,
# one of them tested, and the untested one is the one that names the plant on the
# day the take is spent.
#
# So: emit the injector's exact log format from each candidate plant, run the
# real scorer on it, and require the right name. A run whose acceptance check has
# never been shown to discriminate is a run that cannot fail.
#
# It also found the gate: score_run.py refuses any file without a render-pass
# inventory line before it reaches ANY instrument block, so a plantid take must
# carry X4VR_MV=1 or the scorer prints UNSCORABLE and never scores the thing the
# take was spent on.
set -u
# **`CDPATH=`, and it is a fix rather than a precaution.** Patola's environment
# has CDPATH=.:/home/patola:/dados, and `cd` PRINTS the directory whenever it
# resolves through CDPATH -- so `$(cd ... && pwd)` captures that echo as well as
# the pwd and `$root` becomes two lines. This script reported "SKIP  not built"
# with the binary sitting right there. Six other scripts in this project stayed
# broken for weeks because I worked around it at my own call sites instead of
# fixing it where it lives.
CDPATH=

root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
bin="$root/build/tests/x4vr_test_plantid"
if [ ! -x "$bin" ]; then
    # A missing binary is only "skip" when nothing was built at all. With a
    # build tree present it is a failure, because a silent skip in a suite reads
    # exactly like a pass.
    if [ -d "$root/build" ]; then
        echo "FAIL  $bin is missing from an existing build tree"
        exit 1
    fi
    echo "SKIP  no build tree — x4vr_test_plantid not built"
    exit 0
fi
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT

"$bin" --emit "$dir" >/dev/null || { echo "FAIL  could not emit logs"; exit 1; }

fail=0
check() { # <truth> <expected substring>
    local out
    out="$(python3 "$root/tools/score_run.py" "$dir/synth-$1.log" 2>&1)"
    if printf '%s' "$out" | grep -q "UNSCORABLE"; then
        echo "FAIL  $1: scorer refused the file entirely"
        printf '%s\n' "$out" | head -2 | sed 's/^/      /'
        fail=1
        return
    fi
    if printf '%s' "$out" | grep -q "=> $2"; then
        echo "ok    $1 -> $2"
    else
        echo "FAIL  $1: expected '=> $2', got:"
        printf '%s\n' "$out" | grep -E "signature|=>" | head -3 | sed 's/^/      /'
        fail=1
    fi
}

# Each plant must get its OWN name. Two of three would mean the verdict is a
# coin flip dressed as a diagnosis.
#
# **The delayed pair is the control this set was missing**, and its absence let
# the scorer call take 288 "SMOOTHED / MOMENTUM" when the response was a pure
# integrator arriving whole, three ticks late. Coast cannot tell those apart --
# during COAST a delayed integrator is still paying out the impulse -- so the
# verdict now reads the response WIDTH, and these two cases are what force it
# to. `delayed-integrator` and `smoother` returning the same name is the exact
# failure to guard against; they are adjacent here on purpose.
check integrator         "PURE INTEGRATOR, no delay"
check smoother           "SMOOTHED / MOMENTUM"
check capped             "PER-STEP CAP"
check delayed-integrator "INTEGRATOR WITH TRANSPORT DELAY"
check delayed-smoother   "SMOOTHED / MOMENTUM"

# **Take 289's actual condition: the readback refreshing 3x slower than the tick.**
# The same plant must get the same NAME at either sampling rate. Before the
# sequence was moved into the readback's time base it did not: identical hardware
# read INTEGRATOR WITH TRANSPORT DELAY at 30 Hz and SMOOTHED / MOMENTUM at 90,
# and that is the whole reason take 289 was void. The DELAY is now reported in
# camera updates, so 3 ticks of dead time sampled every 3 ticks is 1 update --
# the same physical lag, stated in the only unit that does not move with our own
# tick rate.
check delayed-integrator-sampled3 "INTEGRATOR WITH TRANSPORT DELAY"
check smoother-sampled3           "SMOOTHED / MOMENTUM"

if [ "$fail" = 0 ]; then
    echo "all cases passed"
else
    echo "FAILURES"
fi
exit "$fail"
