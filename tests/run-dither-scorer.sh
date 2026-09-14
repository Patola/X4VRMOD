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
    mu="$(printf '%s' "$out" | sed -nE 's/.*\(([0-9.]+) \+- ([0-9.]+) ticks\).*/\1/p')"
    sd="$(printf '%s' "$out" | sed -nE 's/.*\(([0-9.]+) \+- ([0-9.]+) ticks\).*/\2/p')"
    if python3 -c "import sys; sys.exit(0 if abs($mu-$2)<1.5 and abs($sd-$3)<1.5 else 1)"; then
        echo "ok    $1 -> $mu +- $sd ticks (planted $2 +- $3)"
    else
        echo "FAIL  $1 -> $mu +- $sd ticks, planted $2 +- $3"; fail=1
    fi
}

# A fixed delay must come back with ZERO width: a band that invents a spread
# cannot tell a jittering plant from a steady one, which is the whole question.
check fixed2  2.0 0.0
check fixed3  3.0 0.0
check band2-6 4.0 1.41
check band1-7 4.0 2.00

[ "$fail" = 0 ] && echo "all cases passed" || echo "FAILURES"
exit "$fail"
