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

drawn() {  # drawn <ndc-half-span> <chan_win_w> <eye> [arm A|B] [reachable-half-span]
    python3 - "$1" "$2" "$3" "${4:-A}" "${5:-}" <<'PYX'
import sys
s, w, e, arm = float(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
r = sys.argv[5]
pct = round(s * 100)
st = "ON" if arm == "A" else "OFF"
# Without a 5th argument this is the line as the layer wrote it before the
# reachable box existed, so older logs stay covered.
tail = (f" | reachable x={-float(r):.4f}..{float(r):.4f} "
        f"y={-float(r):.4f}..{float(r):.4f}") if r else ""
print(f"[  223999.000] layer   cursor drawn (final) ARM {arm} (canvas {st}): "
      f"ndc x={-s:.4f}..{s:.4f} y={-s:.4f}..{s:.4f} = {pct}%x{pct}% of the "
      f"eye | 1000 frame(s) | channel says the window is {w} wide against a "
      f"{e} eye{tail}")
PYX
}

phase() {  # phase <n> <A|B>
    st="ON"; [ "$2" = B ] && st="OFF"
    printf '[  223990.000] layer   cursor canvas: phase %s — ARM %s (canvas %s). x' "$1" "$2" "$st"
}

# THE MEASURED CASE: a=0.5252 confines the cursor to the central 52.5%. The
# GEOMETRIC claim comes from the map's reachable box, which the layer prints
# beside the drawn extent. The drawn extent alone depends on where the mouse
# went exactly as the pointer range does -- the old verdict said otherwise and
# take 313 (41%x43% drawn under a 52.5% map) was the counterexample.
plant /tmp/census-drawn-confined.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 0.5252 2816 2816 A 0.5252)"
want "confined -> reports the drawn span"  /tmp/census-drawn-confined.log cursor "ARM A (canvas ON): drawn over ndc"
want "confined -> reports the map's box"   /tmp/census-drawn-confined.log cursor "the map can reach x=-0.5252..0.5252"
want "confined -> names it CONFINED BY THE MAP" /tmp/census-drawn-confined.log cursor "=> CONFINED BY THE MAP"
want "confined -> gives the lost area"     /tmp/census-drawn-confined.log cursor "72% of the AREA unreachable"
want "confined -> points at the canvas"    /tmp/census-drawn-confined.log cursor "whether it SHOULD"
absent "confined, swept to the edge -> no protocol caveat" /tmp/census-drawn-confined.log "cursor     => and the pointer was not swept"

# Take 313's shape: the map allows 52.5% and the cursor was drawn over 42%.
# The confinement is still the map's; the 42% is the protocol and must be
# called that, not reported as the limit.
plant /tmp/census-drawn-unswept.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 0.42 2816 2816 A 0.5252)"
want "unswept -> still CONFINED BY THE MAP" /tmp/census-drawn-unswept.log cursor "=> CONFINED BY THE MAP"
want "unswept -> map box, not drawn box"    /tmp/census-drawn-unswept.log cursor "at most 53%x53% of the eye"
want "unswept -> drawn span is the PROTOCOL" /tmp/census-drawn-unswept.log cursor "is the PROTOCOL, not the limit"

# A log from before the reachable box: nothing may be claimed as geometry.
plant /tmp/census-drawn-old.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 0.5252 2816 2816)"
want "old log -> refuses the geometric claim" /tmp/census-drawn-old.log cursor "predates the reachable box"
if python3 tools/score_run.py /tmp/census-drawn-old.log 2>&1 | grep -qE "^cursor.*(=> CONFINED|This is geometry)"; then
    bad "old log -> still calls a drawn extent geometry"
else
    ok "old log -> does not call a drawn extent geometry"
fi

# An unconfined cursor must NOT trip it, or the verdict is an off switch --
# with the box, and on an old log without it.
plant /tmp/census-drawn-full.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 1.0 2816 2816 A 1.0)"
want "unconfined -> says nothing confines it" /tmp/census-drawn-full.log cursor "nothing is confining it"
if python3 tools/score_run.py /tmp/census-drawn-full.log 2>&1 | grep -qE "^cursor.*=> CONFINED"; then
    bad "unconfined -> still claims CONFINED"
else
    ok "unconfined -> does not claim CONFINED"
fi
plant /tmp/census-drawn-full-old.log "$(cur 'drew 1000' 0 2816 0 2816 moved)" "$(drawn 1.0 2816 2816)"
want "unconfined, old log -> says nothing confines it" /tmp/census-drawn-full-old.log cursor "nothing is confining it"

# The final census is written at teardown, after the chains are gone, and
# take 313's read "against a 0x0 eye". The scorer must borrow the extent from
# an earlier census and say so, never divide by zero into "0% of the eye".
cur0() {
    printf '[  224000.000] layer   cursor census (final): 1000 present(s) — drew 1000 | pointer x=0..2563 y=0..2509 against a 0x0 eye (moved)'
}
plant /tmp/census-eye0.log "$(cur 'drew 900' 0 2563 0 2509 moved)" "$(cur0)"
want "0x0 final -> borrows the earlier extent" /tmp/census-eye0.log cursor "using 2816x2816 from the last census"
want "0x0 final -> reports real coverage"      /tmp/census-eye0.log cursor "91% of the eye in x and 89% in y"
if python3 tools/score_run.py /tmp/census-eye0.log 2>&1 | grep -qE "^cursor.*(against a 0x0|across 0% of the eye)"; then
    bad "0x0 final -> still reads a 0x0 eye"
else
    ok "0x0 final -> does not read a 0x0 eye"
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

# The knob verdict reads the MAP's boxes, not the drawn extents. Take 314's
# ARM B was swept over less of the eye than a full sweep would give; a run
# that barely moved the pointer in ARM B would have read "did not act" off the
# drawn extents while the map was genuinely different.
plant /tmp/census-ab-unswept.log "$(cur 'drew 2000' 0 2816 0 2816 moved)" \
    "$(phase 0 A)" "$(phase 1 B)" \
    "$(drawn 0.4 2816 2816 A 0.5252)" "$(drawn 0.2 2816 2816 B 1.0)"
want "unswept ARM B -> still says the knob acted" /tmp/census-ab-unswept.log cursor "THE KNOB ACTED. The map lets ARM A reach 53%"
plant /tmp/census-ab-samebox.log "$(cur 'drew 2000' 0 2816 0 2816 moved)" \
    "$(phase 0 A)" "$(phase 1 B)" \
    "$(drawn 0.5 2816 2816 A 0.5252)" "$(drawn 0.3 2816 2816 B 0.5252)"
want "equal boxes -> the knob did NOT act" /tmp/census-ab-samebox.log cursor "THE KNOB DID NOT ACT. Both arms' maps"

# X4's hover rate per arm. The arm moves only our drawing, so the rate must
# match; take 314 read 1.97 vs 1.89/s. Timestamped lines, because the split
# is by the phase schedule.
tphase() {  # tphase <t> <n> <A|B>
    st="ON"; [ "$3" = B ] && st="OFF"
    printf '[ %s] layer   cursor canvas: phase %s — ARM %s (canvas %s). x' "$1" "$2" "$3" "$st"
}
tsets() {  # tsets <t> <changes>
    printf '[ %s] inject  cursor sets: %s set(s) — handle found %s, NOT found 0, null 0 | %s were a CHANGE (0 while head-look was STEERING, %s while it was NOT; %s in the COCKPIT, 0 elsewhere) | 3 mouse button press(es)' "$1" "$2" "$2" "$2" "$2" "$2"
}
plant /tmp/census-rate-same.log "$(tphase 224000.000 0 A)" "$(tsets 224001.000 0)" \
    "$(tsets 224011.000 20)" "$(tphase 224020.000 1 B)" "$(tsets 224021.000 22)" \
    "$(tsets 224031.000 41)"
want "same rate -> per-arm line"  /tmp/census-rate-same.log cursor-set "per arm: ARM A 20 change(s)"
want "same rate -> says it must match" /tmp/census-rate-same.log cursor-set "same rate in both arms"
plant /tmp/census-rate-diff.log "$(tphase 224000.000 0 A)" "$(tsets 224001.000 0)" \
    "$(tsets 224011.000 40)" "$(tphase 224020.000 1 B)" "$(tsets 224021.000 42)" \
    "$(tsets 224031.000 45)"
want "different rate -> flags it" /tmp/census-rate-diff.log cursor-set "HOVER RATE DEPENDS ON THE ARM"

# The UI-vs-cursor shift check. It looked for a `canvas shift` line the
# overlay stopped printing, and told take 314 -- 18135 draws -- that the
# overlay never drew. The map is on the first-draw line; read that.
fdraw() {  # fdraw <ax> <bx>
    printf '[  223999.500] layer   cursor: drawing 32x32 hot=(15,15) into 2 layer(s) of the 2816x2816 eye — first at x=915.0 y=463.0 (channel says visible), texture B8G8R8A8_UNORM into an eye of B8G8R8A8_UNORM, canvas eye0 A=(%s,%s) B=(%s,+0.00000) -> hotspot moves +1.0,+1.0 px' "$1" "$1" "$2"
}
plant /tmp/census-shift-agree.log "$(fdraw 0.5252 +0.00002)"
want "shift agrees -> says so" /tmp/census-shift-agree.log canvas "pointer and UI agree"
absent "shift agrees -> no 'never drew'" /tmp/census-shift-agree.log "canvas  \(the cursor overlay never drew"
plant /tmp/census-shift-bad.log "$(fdraw 0.5252 +0.01000)"
want "shift disagrees -> FAIL" /tmp/census-shift-bad.log FAIL "the pointer sits off every button"
plant /tmp/census-shift-raw.log "$(fdraw 1.0000 +0.00000)"
want "raw-pointer first draw -> not comparable" /tmp/census-shift-raw.log canvas "NO canvas map (raw pointer arm)"
if python3 tools/score_run.py /tmp/census-shift-raw.log 2>&1 | grep -qE "^FAIL.*pointer sits off"; then
    bad "raw-pointer first draw -> FAILed for a shift it never carried"
else
    ok "raw-pointer first draw -> not failed for a shift it never carried"
fi

# #74 with the pose channel off: the layer refuses the shift, and the scorer
# must FAIL it rather than read the zeros as a head that never leaned.
plant /tmp/census-translate-off.log '[  223999.000] layer   head translate: ASKED FOR BUT INACTIVE — X4VR_POSE is off, so the shaders cannot shift the eye.'
want "translate without pose -> FAIL names X4VR_POSE" /tmp/census-translate-off.log FAIL "Add X4VR_POSE=1 — it is not implied. This run says nothing about #74"

# ------------------------------------ gate on intent: translate and roll
# The base run carries X4VR_POSE=0 and no roll knob. These rewrite ITS env
# line, because the verdicts below hinge on what the run asked for.
plant_env() {  # plant_env <path> <sed-expr> <extra lines...>
    local path=$1 expr=$2; shift 2
    { sed "0,/env: run =/{/env: run =/$expr}" "$BASE"; printf '%s\n' "$@"; } > "$path"
}
refusal='[  224300.000] layer   head translate: ASKED FOR BUT INACTIVE — X4VR_POSE is off, so the shaders cannot shift the eye.'
# Take 315's build printed this at TEARDOWN on a run with X4VR_POSE=1 that had
# published real shifts; that is not a refused run.
plant_env /tmp/census-translate-teardown.log 's/X4VR_POSE=0/X4VR_POSE=1 X4VR_HEAD_TRANSLATE=1/' "$refusal"
want "refusal line with POSE=1 -> teardown note" /tmp/census-translate-teardown.log translate "not a refusal"
if python3 tools/score_run.py /tmp/census-translate-teardown.log 2>&1 | grep -qE "^FAIL.*This run says nothing about #74"; then
    bad "refusal line with POSE=1 -> still FAILs the run"
else
    ok "refusal line with POSE=1 -> does not FAIL the run"
fi
rollline='[  224300.000] layer   roll: head 1.20 deg, weight 1.000 -> published 1.20 deg as (cos,sin)=(0.9998,0.0209)'
plant /tmp/census-roll-level.log "$rollline"
want "level head, roll not asked -> note, not FAIL" /tmp/census-roll-level.log roll "not exercised"
if python3 tools/score_run.py /tmp/census-roll-level.log 2>&1 | grep -qE "^FAIL.*never rolled"; then
    bad "level head, roll not asked -> still FAILs"
else
    ok "level head, roll not asked -> no FAIL"
fi
plant_env /tmp/census-roll-asked.log 's/X4VR_POSE=0/X4VR_POSE=1 X4VR_ROLL_AB=20/' "$rollline"
want "level head, roll ASKED -> FAIL" /tmp/census-roll-asked.log FAIL "never rolled more than 1.20 deg"

# ------------------------------------------- hover by view (X4VR_HOVER_VIEW)
hview() {  # hview <straight> <yaw> <pitch>
    printf '[  224200.000] inject  hover view: cursor changes with the view straight %s | turned left/right (|yaw|>=30) %s | turned up/down (|pitch|>=20) %s | in transit 3 | view unknown 0' "$1" "$2" "$3"
}
plant /tmp/census-hview-turned.log "$(hview 40 12 7)" \
    '[  224150.000] inject  milestone: PART 1 DONE (30 hovers, view straight). Now HOLD a numpad key.'
want "hover view turned -> hovered turned" /tmp/census-hview-turned.log hoverview "X4 HOVERED WITH ITS CAMERA TURNED (19 change(s))"
want "hover view turned -> defers the ON-object call" /tmp/census-hview-turned.log hoverview "Patola's to say"
want "hover view -> lists milestones" /tmp/census-hview-turned.log hoverview "milestone: PART 1 DONE"
plant /tmp/census-hview-none.log "$(hview 40 0 0)"
want "hover view none turned -> names both readings" /tmp/census-hview-none.log hoverview "NO HOVER WITH THE VIEW TURNED"
absent "hover view absent -> silent" "$BASE" hoverview

# ------------------------------------------ the camera write-watch (camwatch)
cwsum() {  # cwsum <events> <sampling> <stores> <sampled> <writers> <yawspan>
    printf '[  224400.000] inject  camwatch: %s event(s), %s sampling, 0 arm failure(s) | %s store(s) counted, %s sampled, 0 LOST | %s distinct writer ip(s) | camera yaw span %s deg' "$@"
}
cwwriter='[  224399.000] inject  camwatch: WRITER #1 ip=1a2b3c4 (the store is the instruction BEFORE this address) thread 4242 '"'"'X4 Main'"'"' buf 1 | rax=0 rbx=1 | stack->X4 text: +0x18:1a2c000 +0x40:72f657'
plant /tmp/census-cw-found.log "$(cwsum 300 1 12000 4000 1 90)" "$cwwriter"
want "camwatch writers -> names them" /tmp/census-cw-found.log camwatch "1 INSTRUCTION(S) WRITE the camera orientation"
want "camwatch writers -> lists ip, thread, returns" /tmp/census-cw-found.log camwatch "#1 ip=1a2b3c4 thread 4242 'X4 Main' buf 1 | returns: +0x18:1a2c000"
plant /tmp/census-cw-null.log "$(cwsum 300 0 0 0 0 90)"
want "camwatch no store, camera turned -> bounded null" /tmp/census-cw-null.log camwatch "a BOUNDED null about these addresses"
plant /tmp/census-cw-still.log "$(cwsum 300 0 0 0 0 5)"
want "camwatch no store, still camera -> not exercised" /tmp/census-cw-still.log camwatch "NOT EXERCISED"
plant /tmp/census-cw-nosample.log "$(cwsum 300 0 500 0 0 90)"
want "camwatch counted not sampled -> says the upgrade failed" /tmp/census-cw-nosample.log camwatch "NONE sampled"
plant /tmp/census-cw-noarm.log "$(cwsum 0 0 0 0 0 90)"
want "camwatch nothing armed -> FAIL" /tmp/census-cw-noarm.log FAIL "armed NO event"
plant /tmp/census-cw-refused.log '[  224400.000] inject  camwatch: anchors REFUSED — fetch (nil), buffer-index globals (nil) (nil) (fetch prologue differs from X4 9.00'"'"'s — another build; refusing to aim at a guessed layout)'
want "camwatch refused -> FAIL names the build" /tmp/census-cw-refused.log FAIL "not the one the probe was written against"
absent "camwatch absent -> silent" "$BASE" camwatch
# Hop 2: the verdict per stored matrix.
comove() {  # comove <label> <ticks> <cam> <with> <alone>
    printf '[  224500.000] inject  camwatch hop2 co-move [%s] @0x7f0000001000: %s tick(s), camera moved in %s, the matrix moved WITH it in %s, ALONE in %s | 3x3 ranges 0..1' "$@"
}
plant /tmp/census-cw2.log "$(comove controller 3000 900 0 0)" "$(comove child0 3000 900 880 12)" \
    "$(comove child1 3000 900 20 1500)" "$(comove child2 3000 900 500 400)"
want "hop2 static controller" /tmp/census-cw2.log "camwatch hop2 \[controller\]" "STATIC"
want "hop2 free-look child"   /tmp/census-cw2.log "camwatch hop2 \[child0\]" "MOVES WITH THE CAMERA"
want "hop2 shaking child"     /tmp/census-cw2.log "camwatch hop2 \[child1\]" "moves ON ITS OWN"
want "hop2 mixed child"       /tmp/census-cw2.log "camwatch hop2 \[child2\]" "MIXED"
plant /tmp/census-cw2-still.log "$(comove child0 3000 10 10 0)"
want "hop2 still camera -> not exercised" /tmp/census-cw2-still.log "camwatch hop2 \[child0\]" "NOT EXERCISED"
plant /tmp/census-cw2-refused.log '[  224500.000] inject  camwatch hop2: REFUSED — code at 0x191d185 differs from the analysed build'
want "hop2 refused -> FAIL" /tmp/census-cw2-refused.log FAIL "X4VR_CAMWATCH=2 refused"


# ------------------------------------------- the camera-state scan (camscan)
csum() {  # csum <samples> <yawspan> <pitchspan> <survivors>
    printf '[  224100.000] inject  camscan: %s sample(s), camera yaw span %s deg, pitch span %s deg | 50 region(s) 110592 bytes readable | discoveries 2, alive %s, eliminated 900 | SURVIVORS %s' "$1" "$2" "$3" "$4" "$4"
}
plant /tmp/census-camscan-still.log "$(csum 600 4.0 2.0 0)"
want "camscan still camera -> NOT EXERCISED" /tmp/census-camscan-still.log camscan "NOT EXERCISED"
absent "camscan still camera -> no bounded-null claim" /tmp/census-camscan-still.log "camscan  => NOTHING TRACKED"
plant /tmp/census-camscan-null.log "$(csum 600 80.0 30.0 0)"
want "camscan swept, none -> bounded null" /tmp/census-camscan-null.log camscan "A bounded null, not 'there is none'"
plant /tmp/census-camscan-found.log "$(csum 600 80.0 30.0 2)" \
    '[  224100.000] inject  camscan: CANDIDATE cam->[0x58]+0x40 yaw +deg float — 300 hit(s) at 61 distinct angle(s), 1 miss(es)'
want "camscan found -> names the candidates" /tmp/census-camscan-found.log camscan "SLOT(S) TRACKED"
want "camscan found -> lists the slot" /tmp/census-camscan-found.log camscan "cam->[0x58]+0x40 yaw +deg float"
plant /tmp/census-camscan-off.log '[  224100.000] inject  camscan: GetCameraRotation resolved, root global (nil) — its +0xd did NOT decode as mov disp32(%rip),%rax, so the scan is OFF rather than aimed at a guess'
want "camscan undecodable -> FAIL" /tmp/census-camscan-off.log FAIL "the scan never started"
absent "camscan absent -> silent" "$BASE" camscan

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

# ...the same line with the view guard tripped, which is what a
# X4VR_HEADLOOK=0 run produces.
sets_unk() {  # same args
    printf '[  223995.000] inject  cursor sets: %s set(s) — handle found %s, NOT found %s, null %s | %s were a CHANGE (%s while head-look was STEERING, %s while it was NOT; %s in the COCKPIT, %s elsewhere — VIEW NEVER STAMPED, the split is meaningless) | %s mouse button press(es)' "$@"
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
want "no change -> exonerates the canvas" /tmp/census-set-nohover.log cursor-set "expected reading, not a placement result"

plant /tmp/census-set-hover.log "$(sets 400 400 0 0 37 5 32 30 7 2)"
want "changes -> hover registered"   /tmp/census-set-hover.log cursor-set "DID CHANGE THE CURSOR 37 time(s)"
want "changes -> flags the conflict" /tmp/census-set-hover.log cursor-set "only the player can say whether it was on the object"

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

# **The X4VR_HEADLOOK=0 configuration, which is the one that can answer the
# cockpit question -- and the one that silently breaks the view split.**
# headlook_tick stamps g_in_cockpit and returns early when head-look is off, so
# the flag sits at its `false` initialiser and every change bins as "elsewhere".
# Without the guard the scorer would print NOT ONE CHANGE IN THE COCKPIT for a
# run spent entirely in the cockpit -- a confident verdict from a default.
plant /tmp/census-set-unstamped.log "$(sets_unk 60 60 0 0 40 0 40 0 40 5)"
want "view unstamped -> refuses the split" /tmp/census-set-unstamped.log cursor-set "THE VIEW WAS NEVER STAMPED"
want "view unstamped -> keeps what stands" /tmp/census-set-unstamped.log cursor-set "still stand; WHERE they happened does not"
if python3 tools/score_run.py /tmp/census-set-unstamped.log 2>&1 | grep -qE "NOT ONE CHANGE IN THE COCKPIT"; then
    bad "view unstamped -> still claims no cockpit change"
else
    ok "view unstamped -> does not claim no cockpit change"
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
