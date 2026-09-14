#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# **score_run.py's dither recovery, driven against planted bands.**
#
# tests/dither_id.cpp validates a C++ recovery. score_run.py contains a SECOND
# implementation of the same arithmetic, in Python, and that is the one that
# reads the take. The C++ one being right says nothing about it -- the same trap
# the plantid verdict fell into, where a comment credited a test that checked its
# own statistics on its own in-memory trace and never produced a line the scorer
# could read.
#
# So: emit the injector's exact log format from plants whose latency band is
# known by construction, run the real scorer, and require the band back.
set -u
# CDPATH is set in this environment and `cd` PRINTS the directory when it
# resolves through it, so `$(cd ... && pwd)` captures two lines. Fixed here
# rather than worked around at the call site.
CDPATH=

root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
bin="$root/build/tests/x4vr_test_dither_id"
if [ ! -x "$bin" ]; then
    if [ -d "$root/build" ]; then
        echo "FAIL  $bin missing from an existing build tree"; exit 1
    fi
    echo "SKIP  no build tree"; exit 0
fi
dir="$(mktemp -d)"; trap 'rm -rf "$dir"' EXIT
"$bin" --emit "$dir" >/dev/null || { echo "FAIL  could not emit"; exit 1; }

fail=0
check() { # <name> <true mean ticks> <true sd ticks>
    local out mu sd
    out="$(python3 "$root/tools/score_run.py" "$dir/dither-$1.log" 2>&1 | grep '^dither')"
    if ! printf '%s' "$out" | grep -q "=> IN RANGE"; then
        echo "FAIL  $1: not IN RANGE"
        printf '%s\n' "$out" | grep '=>' | sed 's/^/      /'; fail=1; return
    fi
    mu="$(printf '%s' "$out" | sed -nE 's/.*mean \(([0-9.]+) ticks\).*/\1/p')"
    sd="$(printf '%s' "$out" | sed -nE 's/.*tap spread [0-9]+ ms \/ ([0-9.]+) ticks.*/\1/p')"
    if [ -z "$mu" ] || [ -z "$sd" ]; then
        # A format change that silently empties these would make every arithmetic
        # comparison below vacuous, and `FAIL ... -> +- ticks` is what that looks
        # like. Name it as a parse failure so it is not mistaken for a regression.
        echo "FAIL  $1: could not parse the scorer's dither lines (format change?)"
        printf '%s\n' "$out" | sed 's/^/      /'; fail=1; return
    fi
    if python3 -c "import sys; sys.exit(0 if abs($mu-$2)<1.5 and abs($sd-$3)<1.5 else 1)"; then
        echo "ok    $1 -> $mu +- $sd ticks (planted $2 +- $3)"
    else
        echo "FAIL  $1 -> $mu +- $sd ticks, planted $2 +- $3"; fail=1
    fi
}

# A fixed delay must come back with ZERO width: a band that invents a spread
# cannot tell a jittering plant from a steady one, which is the whole question.
#
# **These four are the QUIET regime and the spread assertion is only valid here.**
# Every one drives a 20 deg 0.3 Hz sine at a metronome-uniform 70 Hz. Take 291 was
# ordinary play -- the player's own steering carried 51x the instrument's variance
# and the tick wandered from 9 to 51 ms -- and under those conditions the tap
# spread stops tracking the plant's width entirely. Replaying take 291's own data
# with bands of equal mean and widths from 0 to 35 ms moved the recovered spread
# less than changing the random seed did. So passing here is a guard on the
# ARITHMETIC and is not a licence to read a spread off a take; score_run.py prints
# that caveat next to the number for the same reason.
check fixed2  2.0 0.0
check fixed3  3.0 0.0
check band2-6 4.0 1.41
check band1-7 4.0 2.00

# **The other half of a range statement: it must REFUSE.** A wandering tick plus
# steering that reaches X4's 65 deg stop is outside what a deconvolution can read,
# and the old scorer answered that case with a confident "222.1 ms mean, 210.1 ms
# spread" — take 291, whose data was fine. An instrument that cannot decline is
# not an instrument.
refuses() { # <name> <why>
    local out
    out="$(python3 "$root/tools/score_run.py" "$dir/dither-$1.log" 2>&1 | grep '^dither')"
    if printf '%s' "$out" | grep -qE "=> (ILL-CONDITIONED|OUT OF REACH|OUT OF RANGE)"; then
        echo "ok    $1 -> refused ($2)"
    else
        echo "FAIL  $1: returned a number where it should have refused ($2)"
        printf '%s\n' "$out" | grep '=>' | sed 's/^/      /'; fail=1
    fi
}
refuses slam-fixed "clamped loop, fixed band"
refuses slam-band  "clamped loop, jittering band"

[ "$fail" = 0 ] && echo "all cases passed" || echo "FAILURES"
exit "$fail"
