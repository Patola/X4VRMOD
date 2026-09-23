#!/usr/bin/env bash
# Control suite for score_run.py's `cursor` and `pad` blocks (#46/#64/#71).
#
# Both blocks were written against logs that do not exist yet -- the censuses
# ship before the run that carries them. That is exactly the situation in which
# a scorer gets one verdict right and the other four wrong without anyone
# noticing, so every branch is planted here with a log whose answer is known by
# construction, INCLUDING the silent case.
#
# Appended to a real take because the scorer needs a render-pass inventory
# before it reads anything at all.
set -u
CDPATH=
LC_ALL=C          # pl_PL renders decimals with a comma and the patterns are C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1

BASE=/tmp/x4vr-take306.log
if [ ! -f "$BASE" ]; then
    echo "SKIP  $BASE absent — the planted logs are appended to a real run"
    exit 0
fi

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

# plant <path> <extra log lines...>
plant() {
    local path=$1; shift
    { cat "$BASE"; printf '%s\n' "$@"; } > "$path"
}

# want <name> <logpath> <prefix> <substring>
want() {
    local name=$1 log=$2 pre=$3 sub=$4 got
    got=$(python3 tools/score_run.py "$log" 2>&1 | grep -E "^${pre}")
    if printf '%s' "$got" | grep -qF "$sub"; then
        ok "$name"
    else
        bad "$name — wanted '$sub'"
        printf '%s\n' "$got" | sed 's/^/        got: /' | head -6
    fi
}

# absent <name> <logpath> <prefix>
absent() {
    local name=$1 log=$2 pre=$3 got
    got=$(python3 tools/score_run.py "$log" 2>&1 | grep -E "^${pre}" || true)
    if [ -z "$got" ]; then
        ok "$name"
    else
        bad "$name — expected silence, got:"
        printf '%s\n' "$got" | sed 's/^/        /' | head -4
    fi
}

cur() {  # cur <bins> <xlo> <xhi> <ylo> <yhi> <moved-text>
    printf '[  223999.000] layer   cursor census (final): 1000 present(s) — %s | pointer x=%s..%s y=%s..%s against a 2816x2816 eye (%s)' \
        "$1" "$2" "$3" "$4" "$5" "$6"
}

# ---------------------------------------------------------------- the cursor
#
# The case the census was BUILT for: the position never moves, because
# publish_cursor's only caller is a function X4 never calls.
plant /tmp/census-dead.log "$(cur 'no-cursor-image 966 | drew 34' 0 0 0 0 'NEVER MOVED — the channel is not being fed')"
want "dead channel -> names the dead position channel" /tmp/census-dead.log cursor "THE POSITION CHANNEL IS DEAD"
# ...and it must OUTRANK the drew/never-drew branches, or a run reports a
# placement verdict built on a position that never existed.
want "dead channel -> does not also report coverage" /tmp/census-dead.log cursor "CANNOT be read from this run"

# Never drew, and the census names which branch took it.
plant /tmp/census-nodraw.log "$(cur 'no-cursor-image 1000' 10 900 10 900 moved)"
want "never drew -> names the branch" /tmp/census-nodraw.log cursor "NEVER DREW, and the reason is 'no-cursor-image'"

# Drew, pointer covers the eye: no coverage complaint.
plant /tmp/census-full.log "$(cur 'drew 1000' 0 2816 0 2816 moved)"
want "full coverage -> DREW"            /tmp/census-full.log cursor "=> DREW"
absent "full coverage -> no reach complaint" /tmp/census-full.log "cursor     => AND CANNOT"

# Drew, but half the eye in BOTH axes -- the factor-of-2 predicted for #46.
plant /tmp/census-half.log "$(cur 'drew 1000' 0 1408 0 1408 moved)"
want "half in both axes -> flags the reach" /tmp/census-half.log cursor "NOT SEEN OVER THE WHOLE EYE"
want "half in both axes -> reports 50/50"   /tmp/census-half.log cursor "50% of the eye in x and 50% in y"
# The verdict must NOT promote "was not seen there" into "cannot go there".
# Take 308 read 0..1408 in both axes purely because X4 parks the pointer at
# the window centre for mouse-look and the player never moved it elsewhere --
# a limit invented from a protocol is the same error as reporting zero for an
# input nobody supplied.
if python3 tools/score_run.py /tmp/census-half.log 2>&1 | grep -qE "^cursor.*CANNOT REACH"; then
    bad "half in both axes -> still claims CANNOT reach from a sampled range"
else
    ok "half in both axes -> does not claim CANNOT from a sampled range"
fi
want "half in both axes -> says where it WENT not CAN go" /tmp/census-half.log cursor "not where it CAN go"
want "half in both axes -> names the mouse-look park" /tmp/census-half.log cursor "1408 = half of 2816"

# Half in x ALONE is the SBS fold, a different cause; the line must say so
# rather than claiming the factor of 2 whatever the numbers are.
plant /tmp/census-halfx.log "$(cur 'drew 1000' 0 1408 0 2816 moved)"
want "half in x only -> still flags reach" /tmp/census-halfx.log cursor "NOT SEEN OVER THE WHOLE EYE"
want "half in x only -> reports 50/100"    /tmp/census-halfx.log cursor "50% of the eye in x and 100% in y"

# The off-eye cull note rides any of them.
plant /tmp/census-offeye.log "$(cur 'drew 900 | off-eye 100' 0 2816 0 2816 moved)"
want "off-eye -> notes the pre-map cull" /tmp/census-offeye.log cursor "culled off-eye"

# NEGATIVE CONTROL: a log with no census line must stay silent, or "not
# measured" and "measured zero" become the same reading -- the exact defect
# this block was written to end.
absent "no census line -> silent" "$BASE" cursor

# ------------------------------------------------------------------ the pads
#
# pad <enum> <seen> <openjoy> <openpad> <polled> <joy-ev> <pad-ev> <btn> <added> <maps>
pad() { printf '[  223999.000] inject  pad census: enumerate %s (last saw %s device(s)) | opened joystick %s gamepad %s | POLLED axis reads %s | EVENTS joy-axis %s pad-axis %s buttons %s added %s | mapping files %s' "$@"; }

plant /tmp/census-pad-none.log "$(pad 0 -1 0 0 0 0 0 0 0 1)"
want "no device -> says nothing reached X4" /tmp/census-pad-none.log pad "X4 SEES NO DEVICE"

plant /tmp/census-pad-declined.log "$(pad 0 -1 0 0 0 0 0 0 2 1)"
want "announced, never opened -> mapping not binding" /tmp/census-pad-declined.log pad "ANNOUNCED BUT NEVER OPENED"

plant /tmp/census-pad-idle.log "$(pad 0 -1 0 1 0 0 0 0 1 1)" \
    '[  223998.000] inject  pad: X4 opened gamepad id=2 name="Xbox 360 Controller"'
want "opened, no traffic -> says so"  /tmp/census-pad-idle.log pad "OPENED, NO AXIS TRAFFIC"
want "opened, no traffic -> hedges"   /tmp/census-pad-idle.log pad "if they were not, this line says nothing"
want "opened -> names the device"     /tmp/census-pad-idle.log pad 'Xbox 360 Controller'

# TAKE 307's EXACT SHAPE, now that the event channel is counted: zero polls,
# zero enumerations, one gamepad opened, and the axis values arriving as
# events. The old block called this "no binding could have acted" while Patola
# was steering the ship, so it is planted here permanently.
plant /tmp/census-pad-events.log "$(pad 0 -1 0 1 0 0 8451 37 1 1)" \
    '[  223998.000] inject  pad: X4 opened gamepad id=2 name="Xbox 360 Controller"' \
    '[  223998.500] inject  pad axes (gamepad, device id 2): 0=LEFTX:-32768..32767 1=LEFTY:-32768..32767 2=RIGHTX:-31000..30144'
want "take 307 shape -> values ARE reaching X4" /tmp/census-pad-events.log pad "AXIS VALUES ARE REACHING X4"
want "take 307 shape -> names the channel"      /tmp/census-pad-events.log pad "by EVENTS"
want "take 307 shape -> points at bindings"     /tmp/census-pad-events.log pad "BINDING question"
want "take 307 shape -> prints the axis travel" /tmp/census-pad-events.log pad "2=RIGHTX:-31000..30144"
# ...and must NOT print the old false verdict.
if python3 tools/score_run.py /tmp/census-pad-events.log 2>&1 | grep -qE "NEVER POLLED|no binding could have acted"; then
    bad "take 307 shape -> still prints the refuted 'never polled' verdict"
else
    ok "take 307 shape -> does not print the refuted verdict"
fi

# Polling only, which is what the FIRST version of the census assumed was the
# only channel. It must still be reported correctly rather than dropped.
plant /tmp/census-pad-polled.log "$(pad 12 1 1 0 45678 0 0 0 1 1)"
want "polled only -> names POLLING" /tmp/census-pad-polled.log pad "by POLLING"

# Both families carrying axis values: two devices, and their indices mean
# different hardware. This is the X52-plus-emulated-pad configuration.
plant /tmp/census-pad-both.log "$(pad 0 -1 1 1 0 2200 8451 12 2 1)" \
    '[  223998.400] inject  pad axes (joystick, device id 3): 0:-32768..32767 1:-32768..32767' \
    '[  223998.500] inject  pad axes (gamepad, device id 2): 2=RIGHTX:-31000..30144'
want "two families -> warns against pooling" /tmp/census-pad-both.log pad "do not pool their indices"
want "two families -> both axis lines print" /tmp/census-pad-both.log pad "joystick, device 3"

absent "no pad line -> silent" "$BASE" pad

if [ $fail -eq 0 ]; then echo; echo "all cases passed"; else echo; echo "FAILED"; fi
exit $fail
