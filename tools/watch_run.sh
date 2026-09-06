#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Watch a take's log for the evidence the run was FOR, and report whether it is
# arriving -- as a DESKTOP NOTIFICATION, which is what reaches Patola in VR.
#
# **Why this exists.** For take 257 I handed over "abort early if, after a
# minute, this grep returns 0". Patola: *"when I am in VR it is very difficult
# to take other inputs or do other things, my senses are taken; that is even why
# we have the directive to minimize tests in VR. So any action at that stage
# must be automatic if possible, not relying on a simultaneous visual input from
# elsewhere."*
#
# He is right, and the fix is not a better-worded instruction. A check a player
# in a headset cannot perform is not a check.
#
# **notify-send, NOT speech.** The first version used spd-say. Patola, from
# inside VR: *"it talked an error message continuously to me, because it is
# apparently not properly configured."* So the first thing this tool ever did
# was intrude on the session it existed to protect -- speech-dispatcher is
# misconfigured here, spd-say still exits 0, and my fallback chain therefore
# never fired. An output channel that reports success while producing garbage is
# worse than one that plainly fails.
#
# His answer: *"an useful way to reach me when I'm there is using the
# notification system, it seems either WiVRn or WayVR do this because I receive
# some floating notification pop-ups when I am in VR. So notify-send should
# work."* One notification per event, never a stream.
#
# It notifies on SUCCESS too. Silence that means "fine" and silence that means
# "the watchdog died" are the same silence, and a watcher that can only report
# bad news cannot be trusted to be running at all -- the same defect as a guard
# that can only ever agree with itself.
#
#   tools/watch_run.sh /tmp/x4vr-take258.log \
#       --want 'ui dump: frame'      --by 90  --say 'UI dumps' \
#       --want 'ui: TARGET img'      --by 60  --say 'UI target'
#
# --kill additionally ends X4 when a check fails its deadline: the take is
# worthless without its instrument, so continuing only spends the thing this
# project is shortest of, which is Patola's time in a headset. OFF by default,
# because ending a run he is in the middle of is his call and not mine.
set -uo pipefail

LOG=""; KILL=0; POLL=5; STARTUP=180
declare -a WANT SAYS BY

usage() {
    cat >&2 <<'USAGE'
usage: watch_run.sh <logfile> [--kill] [--poll SEC]
                    (--want PATTERN --by SECONDS --say LABEL)...
Sends a desktop notification when each pattern first appears, and a critical
one if it has not appeared by its deadline. --kill also terminates X4 on a
failed deadline. LABEL names the thing in the notification, e.g. 'UI dumps'.
USAGE
    exit 2
}

[[ $# -ge 1 ]] || usage
LOG="$1"; shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kill) KILL=1; shift ;;
        --poll) POLL="$2"; shift 2 ;;
        --startup) STARTUP="$2"; shift 2 ;;
        --want) WANT+=("$2"); shift 2 ;;
        --by)   BY+=("$2");   shift 2 ;;
        --say)  SAYS+=("$2"); shift 2 ;;
        *) usage ;;
    esac
done
[[ ${#WANT[@]} -gt 0 && ${#WANT[@]} -eq ${#BY[@]} && ${#WANT[@]} -eq ${#SAYS[@]} ]] || usage

# One desktop notification per event, and always a line on stdout so the
# terminal transcript stands on its own afterwards.
#
# `urgency` is critical only for failures: in VR a critical notification is the
# one worth interrupting for, and marking everything critical would train it to
# be ignored. -t 0 lets a critical one persist until dismissed; the successes
# time out on their own.
notify() {
    local urgency="$1" body="$2"
    printf '[watch_run] %s: %s\n' "$urgency" "$body"
    command -v notify-send >/dev/null || return 0
    if [[ $urgency == critical ]]; then
        notify-send -u critical -t 0 -a x4vr "X4VR take" "$body" 2>/dev/null
    else
        notify-send -u normal -t 8000 -a x4vr "X4VR take" "$body" 2>/dev/null
    fi
}

# X4's process is "Main()", so `pkill -x X4` never matches it.
x4_pids() { pgrep -f 'X4 Foundations/X4' 2>/dev/null; }

notify normal "watchdog armed"

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
        notify normal "X4 exited"
        break
    elif [[ $((SECONDS - start)) -ge $STARTUP ]]; then
        # **A watchdog that outlives a failed launch is the bug it exists to
        # catch.** Wired into the launcher, this starts before X4 does; if X4
        # never appears -- a crash on startup, a bad knob -- waiting forever
        # would leave exactly the kind of stale process that idled for ten
        # hours on takes 255/256. Give up, say so, and go.
        notify critical "X4 never started within ${STARTUP}s — watchdog exiting"
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
            notify normal "${SAYS[i]}: recording"
        elif [[ $elapsed -ge ${BY[i]} ]]; then
            done_flag[i]=1; remaining=$((remaining - 1))
            if [[ $KILL == 1 ]]; then
                notify critical "${SAYS[i]}: MISSING — ending the run"
                # TERM, not KILL: X4 gets to close its files, and the log stays
                # readable afterwards -- which is the only thing that will
                # explain what went wrong.
                x4_pids | xargs -r kill 2>/dev/null
            else
                notify critical "${SAYS[i]}: MISSING — this take will not measure"
            fi
        fi
    done
    sleep "$POLL"
done

# Never leaves without saying so, for the reason in the header: silence must
# not be ambiguous.
notify normal "watchdog finished"
