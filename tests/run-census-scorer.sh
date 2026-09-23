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

drawn() {  # drawn <ndc-half-span> <chan_win_w> <eye> [arm A|B]
    python3 - "$1" "$2" "$3" "${4:-A}" <<'PYX'
import sys
s, w, e, arm = float(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
pct = round(s * 100)
st = "ON" if arm == "A" else "OFF"
print(f"[  223999.000] layer   cursor drawn (final) ARM {arm} (canvas {st}): "
      f"ndc x={-s:.4f}..{s:.4f} y={-s:.4f}..{s:.4f} = {pct}%x{pct}% of the "
      f"eye | 1000 frame(s) | channel says the window is {w} wide against a "
      f"{e} eye")
PYX
}

phase() {  # phase <n> <A|B>
    st="ON"; [ "$2" = B ] && st="OFF"
    printf '[  223990.000] layer   cursor canvas: phase %s — ARM %s (canvas %s). x' "$1" "$2" "$st"
}

# THE MEASURED CASE: a=0.5252 confines the cursor to the central 52.5%. This
# is #46's mechanism and it is geometry, not protocol -- it holds however the
# player moved the mouse, which is exactly what the pointer range could not
# say.
plant /tmp/census-drawn-confined.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 0.5252 2816 2816)"
want "confined -> reports the drawn span"  /tmp/census-drawn-confined.log cursor "ARM A (canvas ON): drawn over ndc"
want "confined -> names it CONFINED"       /tmp/census-drawn-confined.log cursor "=> CONFINED"
want "confined -> gives the border"        /tmp/census-drawn-confined.log cursor "border of about 24%"
want "confined -> gives the lost area"     /tmp/census-drawn-confined.log cursor "72% of the AREA unreachable"
want "confined -> points at the canvas"    /tmp/census-drawn-confined.log cursor "whether it SHOULD"

# An unconfined cursor must NOT trip it, or the verdict is an off switch.
plant /tmp/census-drawn-full.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 1.0 2816 2816)"
want "unconfined -> says nothing confines it" /tmp/census-drawn-full.log cursor "nothing is confining it"
if python3 tools/score_run.py /tmp/census-drawn-full.log 2>&1 | grep -qE "^cursor.*=> CONFINED"; then
    bad "unconfined -> still claims CONFINED"
else
    ok "unconfined -> does not claim CONFINED"
fi

# The window/eye seam OUTRANKS the confinement verdict: if the pointer and the
# image are in different spaces, every placement number is measured across it.
plant /tmp/census-drawn-seam.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 0.5252 5632 2816)"
want "seam -> flags the disagreement" /tmp/census-drawn-seam.log cursor "INPUT PATH AND THE IMAGE DISAGREE"
if python3 tools/score_run.py /tmp/census-drawn-seam.log 2>&1 | grep -qE "^cursor.*=> CONFINED"; then
    bad "seam -> reports confinement across an unsettled coordinate seam"
else
    ok "seam -> does not report confinement across the seam"
fi

# --- the A/B itself: both arms present, and the knob must be SEEN to act ---
plant /tmp/census-ab-good.log "$(cur 'drew 2000' 0 2816 0 2816 moved)" \
    "$(phase 0 A)" "$(phase 1 B)" "$(phase 2 A)" "$(phase 3 B)" \
    "$(drawn 0.5252 2816 2816 A)" "$(drawn 1.0 2816 2816 B)"
want "A/B -> counts the phases"     /tmp/census-ab-good.log cursor "A/B ran 4 phase(s)"
want "A/B -> reports ARM A's span"  /tmp/census-ab-good.log cursor "ARM A (canvas ON): drawn over ndc"
want "A/B -> reports ARM B's span"  /tmp/census-ab-good.log cursor "ARM B (canvas OFF): drawn over ndc"
want "A/B -> says the knob acted"   /tmp/census-ab-good.log cursor "THE KNOB ACTED"
want "A/B -> defers the verdict"    /tmp/census-ab-good.log cursor "report decides which arm is BETTER"

# The failure this guards: the knob is set but nothing changes. Two arms that
# drew over the SAME region mean whatever was observed was not the canvas, and
# a run scored as if it were would blame the wrong thing.
plant /tmp/census-ab-inert.log "$(cur 'drew 2000' 0 2816 0 2816 moved)" \
    "$(phase 0 A)" "$(phase 1 B)" \
    "$(drawn 0.5252 2816 2816 A)" "$(drawn 0.5252 2816 2816 B)"
want "inert knob -> says it did NOT act" /tmp/census-ab-inert.log cursor "THE KNOB DID NOT ACT"
if python3 tools/score_run.py /tmp/census-ab-inert.log 2>&1 | grep -qE "^cursor.*THE KNOB ACTED"; then
    bad "inert knob -> also claims it acted"
else
    ok "inert knob -> does not also claim it acted"
fi

# One arm only: no comparison exists, and saying so beats reporting one arm as
# if it were a result.
plant /tmp/census-ab-onearm.log "$(cur 'drew 2000' 0 2816 0 2816 moved)" \
    "$(phase 0 A)" "$(drawn 0.5252 2816 2816 A)"
want "one arm -> refuses the comparison" /tmp/census-ab-onearm.log cursor "DID NOT ALTERNATE"

# NEGATIVE CONTROL: a log with no census line must stay silent, or "not
# measured" and "measured zero" become the same reading -- the exact defect
# this block was written to end.
absent "no census line -> silent" "$BASE" cursor

# ------------------------------------------------- the cursor-set census (#46)
#
# The vanilla control proved X4's hover signal EXISTS, so a run where X4 never
# sets a cursor is a positive finding and not a missing instrument. All four
# outcomes are planted, including the zero.
sets() {  # sets <n> <found> <missed> <null> <changed> <steer> <free> <cockpit> <elsewhere> <buttons>
    printf '[  223995.000] inject  cursor sets: %s set(s) — handle found %s, NOT found %s, null %s | %s were a CHANGE (%s while head-look was STEERING, %s while it was NOT; %s in the COCKPIT, %s elsewhere) | %s mouse button press(es)' "$@"
}
zero() { printf '[  223995.000] inject  cursor sets: 0 set(s) — X4 built 26 cursor(s) and has not SET one. x'; }

plant /tmp/census-set-zero.log "$(zero)"
want "sets=0 -> printed, not silent"  /tmp/census-set-zero.log cursor-set "NEVER SET A CURSOR AT ALL"
want "sets=0 -> says why it prints"   /tmp/census-set-zero.log cursor-set "looked like an old build"

plant /tmp/census-set-stale.log "$(sets 400 380 20 0 37 10 27 20 17 3)"
want "missed handles -> stale composite" /tmp/census-set-stale.log cursor-set "COMPOSITED A STALE IMAGE"
want "missed handles -> says it is OURS" /tmp/census-set-stale.log cursor-set "this is OURS"
# ...and it must OUTRANK the change reading, or a run reports a shape verdict
# built on an image that was never drawn.
if python3 tools/score_run.py /tmp/census-set-stale.log 2>&1 | grep -qE "^cursor-set => X4 (NEVER|DID) CHANGED?"; then
    bad "stale composite -> also reports a shape verdict"
else
    ok "stale composite -> does not also report a shape verdict"
fi

plant /tmp/census-set-nohover.log "$(sets 400 400 0 0 1 0 1 1 0 0)"
want "no change -> X4 never saw a hover" /tmp/census-set-nohover.log cursor-set "NEVER CHANGED THE CURSOR"
want "no change -> exonerates the canvas" /tmp/census-set-nohover.log cursor-set "canvas map is not the cause"

plant /tmp/census-set-hover.log "$(sets 400 400 0 0 37 5 32 30 7 2)"
want "changes -> hover registered"   /tmp/census-set-hover.log cursor-set "DID CHANGE THE CURSOR 37 time(s)"
want "changes -> flags the conflict" /tmp/census-set-hover.log cursor-set "contradicts takes 310/311"

# **Patola's first question made this case necessary.** Head-look holds
# MOUSELOOK and #71 established that holding MOUSELOOK is how X4 suspends mouse
# control -- so changes that ALL happened while steering say nothing about
# placement, and a scorer that reported them as hover would send the next run
# at the wrong defect.
plant /tmp/census-set-steeronly.log "$(sets 400 400 0 0 12 12 0 12 0 0)"
want "changes only while steering -> names #71" /tmp/census-set-steeronly.log cursor-set "EVERY CHANGE CAME WHILE HEAD-LOOK WAS STEERING"
want "changes only while steering -> refuses"   /tmp/census-set-steeronly.log cursor-set "cannot choose between them"
if python3 tools/score_run.py /tmp/census-set-steeronly.log 2>&1 | grep -qE "^cursor-set => X4 DID CHANGE"; then
    bad "steering-only -> also reports hover registered"
else
    ok "steering-only -> does not also report hover registered"
fi

# **His second question made this one necessary.** Clicking changes the shape
# too, so changes that could all be clicks are not attributable to hover.
plant /tmp/census-set-clicks.log "$(sets 400 400 0 0 4 0 4 4 0 6)"
want "changes <= clicks -> cautions" /tmp/census-set-clicks.log cursor-set "may ALL be clicks"

# **TAKE 312's EXACT SHAPE.** 50 changes, all while head-look was free, 121
# clicks -- and Patola reported the shape never changed in the cockpit once.
# The old counter had one "free" bin that mixed cockpit-with-MOUSELOOK-released
# together with map-is-open, so it could not contradict him. Split by view, the
# same numbers say what he said.
plant /tmp/census-set-312.log "$(sets 56 56 0 0 50 0 50 0 50 121)"
want "take 312 shape -> no cockpit change" /tmp/census-set-312.log cursor-set "NOT ONE CHANGE IN THE COCKPIT"
want "take 312 shape -> discounts the rest" /tmp/census-set-312.log cursor-set "are not evidence for it"
if python3 tools/score_run.py /tmp/census-set-312.log 2>&1 | grep -qE "^cursor-set => X4 DID CHANGE"; then
    bad "take 312 shape -> still reports hover registered"
else
    ok "take 312 shape -> does not report hover registered"
fi

absent "no cursor-set line -> silent" "$BASE" cursor-set

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
