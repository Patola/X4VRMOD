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
check integrator "PURE INTEGRATOR"
check smoother   "SMOOTHED / MOMENTUM"
check capped     "PER-STEP CAP"

if [ "$fail" = 0 ]; then
    echo "all cases passed"
else
    echo "FAILURES"
fi
exit "$fail"
