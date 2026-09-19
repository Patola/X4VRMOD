#!/usr/bin/env bash
# Control suite for score_run.py's `pace` block — the display-period
# instrument added after takes 303a-d.
#
# Why this exists. Takes 303a-d were scored by dividing the new-content rate
# by a panel rate that came from a REPORTED headset setting, not a
# measurement. Normalising by our own submit rate instead put the four runs in
# a different order. Two denominators, two orderings, and nothing in the log
# could say which one the display actually used — so the block that now
# reports it has to be seen reaching every one of its verdicts BEFORE a take
# leans on it. A scorer that has only ever printed one branch is worth
# nothing: take 294's kpab block printed a null that was an artifact and
# nobody had ever made it print anything else.
#
# Planted lines are APPENDED to a real take's log: the scorer needs a render
# pass inventory before it reads anything at all, and both the `vr summary`
# and `vr copy` matchers keep the LAST line they see, so an appended pair
# wins over the real one without editing it.
set -u
CDPATH=
# printf's %f follows the locale, and this machine runs pl_PL where the
# decimal separator is a comma -- "11.11" is then an invalid number and every
# planted pace line silently failed to be written, so eight checks "passed"
# by reading the no-instrument branch instead. Formatting is done in python
# below and interpolated as %s; this pins the rest.
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1

BASE=/tmp/x4vr-take303.log
if [ ! -f "$BASE" ]; then
    echo "SKIP  $BASE absent — the planted logs are appended to a real run"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

# plant <name> <blits> <submitted> <period_ms> <changes> [omit_pace] [strip]
#
# `strip` removes the base's PERIODIC `vr copy` lines, leaving only the
# planted one. That is what a run too short to produce a rate series looks
# like, and it is the only way to exercise the totals fallback now that the
# per-window statistic takes precedence over it. Take 304a is why the
# precedence exists: its session totals said 14.8% repeats for a run whose
# flight phase never repeated a frame.
plant() {
    local name=$1 blits=$2 sub=$3 per=$4 chg=$5 omit=${6:-no} strip=${7:-no}
    local f="$WORK/$name.log"
    if [ "$strip" = strip ]; then
        grep -av 'vr copy (' "$BASE" > "$f"
    else
        cp "$BASE" "$f"
    fi
    {
        printf '[ 999999.000] layer   vr summary (planted): runtime=%s session=1 focused=1 frames=%d located=%d submitted=%d span=0.1000,0.1000,0.1000\n' \
            'PLANTED' "$sub" "$sub" "$sub"
        printf '[ 999999.001] layer   vr copy (planted): swapchain=1 blits=%d released=%d acquire_failed=0 refused=0\n' \
            "$blits" "$blits"
        if [ "$omit" = no ]; then
            local ms hz
            ms=$(python3 -c "print(f'{$per:.2f}')")
            hz=$(python3 -c "p=$per; print(f'{1000.0/p:.1f}' if p>0 else '0.0')")
            printf '[ 999999.002] layer   vr pace (planted): display period %s ms (%s Hz), changed %d time(s) mid-session\n' \
                "$ms" "$hz" "$chg"
        fi
    } >> "$f"
    python3 tools/score_run.py "$f" 2>&1
}

want() {  # want <label> <output> <regex>
    if printf '%s' "$2" | grep -qE "$3"; then ok "$1"; else
        bad "$1 — expected /$3/"
        printf '%s\n' "$2" | grep -aE '^    pace' | sed 's/^/        got: /'
    fi
}

# --- the per-window rate, which is the number to read ------------------
#
# Take 303's own periodic series is the base here, so these assert the
# statistic that take 304a needed and the session totals could not give.
out=$(plant windowed 14000 20000 11.11 0)
want "90.0 Hz read back from an 11.11 ms period" "$out" 'display 90\.0 Hz'
want "a per-window rate is reported at all"      "$out" 'new content [0-9.]+ Hz median'
want "and says it is not a total"                "$out" 'NOT a total'
want "the starved fraction is reported"          "$out" 'STARVED in [0-9]+% of windows'
want "with a run-wide deficit in Hz"             "$out" '[0-9.]+ Hz of new content missing run-wide'

# PRECEDENCE. The totals verdict must NOT also fire when the window series
# exists: take 304a printed "not enough to explain a doubling complaint" from
# its totals directly beneath a 71%-starved window series. Two verdicts, one
# of them computed over loading, is worse than one.
if printf '%s' "$out" | grep -qE 'not enough to explain|NOT available as an explanation|one stale frame in five'; then
    bad "a totals verdict fired even though the window series was available"
else
    ok "totals verdict defers to the window series"
fi

# --- the totals fallback, for a run too short to produce a rate series ---
out=$(plant high 14000 20000 11.11 0 no strip)
want "30% repeats reported"                 "$out" '30\.0% of headset frames showed a REPEAT'
want "high repeat reaches the loud verdict" "$out" 'one stale frame in five or worse'
want "and no window series is invented"     "$out" 'display 90\.0 Hz'
if printf '%s' "$out" | grep -qE 'STARVED in'; then
    bad "a starved fraction was printed with no periodic series to compute it from"
else
    ok "no starved fraction without a series to compute it from"
fi

out=$(plant mid 18000 20000 11.11 0 no strip)
want "10% repeats reported"                 "$out" '10\.0% of headset frames showed a REPEAT'
want "mid repeat says it cannot explain it" "$out" 'not enough to explain a doubling complaint'

# 1%: the frame deficit must be explicitly REMOVED as an explanation, not
# merely left unmentioned — a silent block reads as absence of a defect.
out=$(plant low 19800 20000 11.11 0 no strip)
want "1% repeats reported"               "$out" '1\.0% of headset frames showed a REPEAT'
want "low repeat rules the deficit OUT"  "$out" 'NOT available as an explanation'

# 72 Hz panel: the arithmetic must follow the runtime, not a constant.
out=$(plant slowpanel 14000 20000 13.89 0)
want "72.0 Hz read back from a 13.89 ms period" "$out" 'display 72\.0 Hz'

# More new frames than the headset consumed. Not a repeat problem — waste on
# our side — and it must not print a negative repeat fraction.
out=$(plant excess 21000 20000 11.11 0)
want "blits over submitted is called out"   "$out" 'EXCEEDS'
want "and prints no negative repeat rate"   "$out" 'waste is on our side'
# Guarded on the block having RUN. Without this the check passes whenever the
# pace line is missing entirely, which is how it reported ok through eight
# broken siblings — a check that cannot fail is not a check.
if ! printf '%s' "$out" | grep -qE '^    pace  display'; then
    bad "excess case never reached the pace block, so the sign check is vacuous"
elif printf '%s' "$out" | grep -qE '\-[0-9.]+% of headset'; then
    bad "excess case printed a negative repeat fraction"
else
    ok "excess case printed no negative repeat fraction"
fi

# A runtime that changes its rate mid-session makes the denominator a moving
# target; a single averaged fraction would hide exactly that.
out=$(plant changed 14000 20000 11.11 3)
want "a mid-session rate change is flagged" "$out" 'CHANGED 3x mid-session'

# The runtime gave no period. Must NOT read as a fast display.
out=$(plant nozero 14000 20000 0 0)
want "a zero period is refused, not divided by" "$out" 'never supplied a display period'

# No pace line at all — every log before take 304. Must say so rather than
# assume a panel rate, which is the whole reason this block exists.
out=$(plant absent 14000 20000 11.11 0 omit)
want "a log without the instrument says so" "$out" 'pace  not measured'

if [ "$fail" -eq 0 ]; then echo; echo "all cases passed"; else echo; echo "FAILURES"; fi
exit "$fail"
