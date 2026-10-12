#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# watch_run.sh --relay: every NEW `milestone:` line reaches Patola as its own
# notification, word for word, and nothing else does.
#
# Why it exists: take 315's protocol is paced by the log, not a clock -- *"my
# view is completely obstructed and I can't check a chronometer"* -- so a
# relay that drops, repeats or reorders a milestone tells him the wrong thing
# to do next while he is in the headset.
#
# notify-send is replaced by a stub on PATH, so running this puts nothing on
# the desktop. X4 is played by a `sleep` whose argv matches the watcher's X4
# detector. This must be run as a FILE: the first attempt ran inline, the
# test shell's own argv contained the X4 pattern, and the watcher waited on
# the test forever -- the watcher-observes-itself trap from the other side.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1

if pgrep -f 'X4 Foundations/X4' >/dev/null 2>&1; then
    echo "SKIP  a real X4 is running; the watcher would watch it instead"
    exit 0
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
cat > "$T/bin/notify-send" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$T/notified"
EOF
chmod +x "$T/bin/notify-send"
: > "$T/take.log"

( exec -a "/fake/X4 Foundations/X4" sleep 9 ) &
fake=$!
PATH="$T/bin:$PATH" bash tools/watch_run.sh "$T/take.log" --poll 1 \
    --startup 20 --relay 'milestone: ' > "$T/stdout" 2>&1 &
watcher=$!

sleep 2
echo '[ 1.0] inject  milestone: PART 1 DONE. Now HOLD a numpad key.' >> "$T/take.log"
sleep 2
echo '[ 2.0] inject  cursor sets: an unrelated line' >> "$T/take.log"
echo '[ 3.0] inject  milestone: left/right done.' >> "$T/take.log"
sleep 4
# In X4's last second: the poll after exit must still relay it.
echo '[ 4.0] inject  milestone: PART 2 DONE. You can quit X4 now.' >> "$T/take.log"
wait "$fake"
# Bounded: a watcher that never exits is the failure, not a slow pass.
for _ in $(seq 1 15); do
    kill -0 "$watcher" 2>/dev/null || break
    sleep 1
done
if kill -0 "$watcher" 2>/dev/null; then
    kill "$watcher"
    exited=0
else
    exited=1
fi

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

got=$(sed 's/^-u normal -t 8000 -a x4vr X4VR take //' "$T/notified" 2>/dev/null)
want='log watcher started
PART 1 DONE. Now HOLD a numpad key.
left/right done.
PART 2 DONE. You can quit X4 now.
X4 exited
watchdog finished'
if [ "$got" = "$want" ]; then
    ok "relays each milestone once, verbatim, in order, and nothing else"
else
    bad "notifications differ from what was written"
    printf '%s\n' "$got" | sed 's/^/        got: /'
fi
[ "$exited" = 1 ] && ok "exits when X4 exits" || bad "still running after X4 exited"
grep -q 'unbound variable' "$T/stdout" && bad "set -u tripped: $(grep -m1 unbound "$T/stdout")" \
    || ok "no unbound variable with --relay alone (no --want)"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
