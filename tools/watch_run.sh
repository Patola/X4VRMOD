#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Watch a take's log for the evidence the run was FOR, and say out loud whether
# it is arriving.
#
# **Why this exists.** For take 257 I handed over "abort early if, after a
# minute, this grep returns 0". Patola: *"when I am in VR it is very difficult
# to take other inputs or do other things, my senses are taken; that is even why
# we have the directive to minimize tests in VR. So any action at that stage
# must be automatic if possible, not relying on a simultaneous visual input from
# elsewhere."*
#
# He is right, and the fix is not a better-worded instruction. A check a player
# in a headset cannot perform is not a check. **Speech is the only channel that
# reaches him**, so this speaks; it never asks him to look at anything.
#
# It also speaks on SUCCESS. Silence that means "fine" and silence that means
# "the watchdog died" are the same silence, and a watcher that can only report
# bad news cannot be trusted to be running at all -- the same defect as a guard
# that can only ever agree with itself.
#
#   tools/watch_run.sh /tmp/x4vr-take258.log \
#       --want 'ui dump: frame'      --by 90  --say 'you eye dumps' \
#       --want 'ui: TARGET img'      --by 60  --say 'you eye target'
#
# --kill additionally ends X4 when a check fails its deadline: the take is
# worthless without its instrument, so continuing only spends the thing this
# project is shortest of, which is Patola's time in a headset. OFF by default,
# because ending a run he is in the middle of is his call and not mine.
set -uo pipefail

LOG=""; KILL=0; POLL=5
declare -a WANT SAYS BY

usage() {
    cat >&2 <<'USAGE'
usage: watch_run.sh <logfile> [--kill] [--poll SEC]
                    (--want PATTERN --by SECONDS --say PHRASE)...
Speaks when each pattern first appears, and speaks a warning if it has not
appeared by its deadline. --kill also terminates X4 on a failed deadline.
USAGE
    exit 2
}

[[ $# -ge 1 ]] || usage
LOG="$1"; shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kill) KILL=1; shift ;;
        --poll) POLL="$2"; shift 2 ;;
        --want) WANT+=("$2"); shift 2 ;;
        --by)   BY+=("$2");   shift 2 ;;
        --say)  SAYS+=("$2"); shift 2 ;;
        *) usage ;;
    esac
done
[[ ${#WANT[@]} -gt 0 && ${#WANT[@]} -eq ${#BY[@]} && ${#WANT[@]} -eq ${#SAYS[@]} ]] || usage

# Spoken, not printed. Falls back through what is installed rather than assuming
# one of them: a watchdog that dies because a package is missing is worse than
# no watchdog, because it looks like everything is fine.
say() {
    printf '[watch_run] %s\n' "$1"
    if command -v spd-say >/dev/null; then spd-say -w -- "$1" 2>/dev/null && return; fi
    if command -v espeak-ng >/dev/null; then espeak-ng -- "$1" 2>/dev/null && return; fi
    # No speech anywhere: at least make a noise, so the failure is still audible.
    if command -v paplay >/dev/null; then
        paplay /usr/share/sounds/freedesktop/stereo/dialog-warning.oga 2>/dev/null
    fi
}

# X4's process is "Main()", so `pkill -x X4` never matches it.
x4_pids() { pgrep -f 'X4 Foundations/X4' 2>/dev/null; }

say "x4 v r watchdog armed"

start=$SECONDS
declare -a done_flag
for i in "${!WANT[@]}"; do done_flag[i]=0; done
remaining=${#WANT[@]}
seen_x4=0

while [[ $remaining -gt 0 ]]; do
    # **"X4 is not running" means two opposite things, and they must not be
    # confused.** Before it starts, it means wait; after it started, it means
    # the run is over. The first draft tested only the second, so launching the
    # watchdog alongside the game -- the obvious way to use it -- made it
    # announce "x4 exited" and quit before X4 had drawn a frame. Caught by
    # running it, which is the only way this kind of thing gets caught.
    if x4_pids >/dev/null; then
        seen_x4=1
    elif [[ $seen_x4 == 1 ]]; then
        say "x4 exited"
        break
    fi
    # Waiting on the PIDs and not on a pattern that could match this script's
    # own argv: six waiters once idled about three hours because `pgrep -f`
    # found themselves.
    elapsed=$((SECONDS - start))
    for i in "${!WANT[@]}"; do
        [[ ${done_flag[i]} == 1 ]] && continue
        if [[ -f "$LOG" ]] && grep -qF -- "${WANT[i]}" "$LOG"; then
            done_flag[i]=1; remaining=$((remaining - 1))
            say "${SAYS[i]} are recording"
        elif [[ $elapsed -ge ${BY[i]} ]]; then
            done_flag[i]=1; remaining=$((remaining - 1))
            if [[ $KILL == 1 ]]; then
                say "${SAYS[i]} missing. ending the run."
                # TERM, not KILL: X4 gets to close its files, and the log stays
                # readable afterwards -- which is the only thing that will
                # explain what went wrong.
                x4_pids | xargs -r kill 2>/dev/null
            else
                say "warning. ${SAYS[i]} missing. this take will not measure."
            fi
        fi
    done
    sleep "$POLL"
done

# Never leaves without saying so, for the reason in the header: silence must
# not be ambiguous.
say "watchdog finished"
