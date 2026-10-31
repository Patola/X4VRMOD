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

# A committed base since take 329 (tests/make-census-base.sh): the old one,
# /tmp/x4vr-take306.log, was cleaned out of /tmp by age, and this script then
# SKIPped with exit 0 -- a suite that passes by testing nothing.
BASE=${X4VR_CENSUS_BASE:-tests/data/census-base.log}
if [ ! -f "$BASE" ]; then
    echo "FAIL  $BASE absent — the planted logs are appended to a real run; rebuild it with tests/make-census-base.sh"
    exit 1
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

# ------------------------------------------------ our extensions (content.xml)
srv='[  224600.000] inject  content: serving /h/.config/EgoSoft/X4/1/content-x4vrmod.xml in memory via fopen — x4vrmod_lookaround ENABLED (was off); your /h/.config/EgoSoft/X4/1/content.xml is not touched'
plant_env /tmp/census-ext-ok.log 's/X4VR_POSE=0/X4VR_POSE=0 X4VR_EXTENSIONS=1/' "$srv"
want "extensions served -> names the path and state" /tmp/census-ext-ok.log extensions "via fopen — x4vrmod_lookaround ENABLED (was off)"
plant_env /tmp/census-ext-missing.log 's/X4VR_POSE=0/X4VR_POSE=0 X4VR_EXTENSIONS=1/'
want "extensions asked, never served -> FAIL" /tmp/census-ext-missing.log FAIL "never read content.xml through the fork"
absent "extensions not asked -> silent" "$BASE" extensions

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
want "hop2 code refused -> FAIL" /tmp/census-cw2-refused.log FAIL "X4VR_CAMWATCH refused: code at 0x191d185"
# A refused CAMERA CLASS then a verified one is take 317: transient, no FAIL.
cls='[  224500.000] inject  camwatch hop2: REFUSED — controller class differs from take 316'"'"'s (its vtable+0x1b0 is not 0x191d0f0) — the chain layout is unverified, refusing'
ver='[  224501.000] inject  camwatch hop2: chain — controller class verified, chain read'
plant /tmp/census-cw2-transient.log "$cls" "$ver"
want "hop2 transient class refusal -> noted" /tmp/census-cw2-transient.log "camwatch hop2 refused" "transient, expected"
if python3 tools/score_run.py /tmp/census-cw2-transient.log 2>&1 | grep -qE "^FAIL.*CAMWATCH"; then
    bad "hop2 transient class refusal -> still FAILs"
else
    ok "hop2 transient class refusal -> no FAIL"
fi
plant /tmp/census-cw2-never.log "$cls"
want "hop2 never verified -> FAIL" /tmp/census-cw2-never.log FAIL "never verified the camera controller"
# HOW FAR, not only when: take 317's sway matrix, and a real free-look.
comovem() {  # comovem <label> <ticks> <cam> <with> <alone> <maxrot>
    printf '[  224500.000] inject  camwatch hop2 co-move [%s] @0x7f0000001000: %s tick(s), camera moved in %s, the matrix moved WITH it in %s, ALONE in %s | max rotation %s deg | 3x3 ranges 0..1' "$@"
}
span='[  224502.000] inject  camwatch: 117 event(s), 1 sampling, 0 arm failure(s) | 6357 store(s) counted, 6277 sampled, 0 LOST | 1 distinct writer ip(s) | camera yaw span 131 deg'
plant /tmp/census-cw2-mag.log "$span" "$(comovem controller 2348 1983 1342 26 9.30)" "$(comovem settings.rot 2348 1983 1900 10 64.00)"
want "hop2 sway (9 deg vs +-66) -> TOO SMALL" /tmp/census-cw2-mag.log "camwatch hop2 \[controller\]" "TOO SMALL to be the free-look"
want "hop2 64 deg, follows -> free-look candidate" /tmp/census-cw2-mag.log "camwatch hop2 \[settings.rot\]" "spans its range"
# Take 317's own line shape: no magnitude, estimated from the ranges.
old317='[  224500.000] inject  camwatch hop2 co-move [controller] @0x7ed0fd976ea0: 2348 tick(s), camera moved in 1983, the matrix moved WITH it in 1342, ALONE in 26 | 3x3 ranges 0.985..1.000 -0.037..0.054 -0.111..0.161 | -0.053..0.037 0.998..1.000 -0.070..0.052 | -0.161..0.111 -0.053..0.070 0.987..1.000'
plant /tmp/census-cw2-317.log "$span" "$old317"
want "hop2 older log -> magnitude estimated from ranges" /tmp/census-cw2-317.log "camwatch hop2 \[controller\]" "max rotation 9.3 deg"

# Take 318: the census read the controller after X4 freed it at exit -- all
# zeros, trace angle exactly 120 deg -- and the last line carried that. The
# scorer must drop it and score the line before.
span360='[  224502.000] inject  camwatch: 117 event(s), 1 sampling, 0 arm failure(s) | 9742 store(s) counted, 9695 sampled, 0 LOST | 1 distinct writer ip(s) | camera yaw span 360 deg'
good318='[  224500.000] inject  camwatch hop2 co-move [controller] @0x7677803278a0: 3680 tick(s), camera moved in 3231, the matrix moved WITH it in 2188, ALONE in 21 | max rotation 19.26 deg | 3x3 ranges 0.944..1.000 -0.266..0.330 -0.059..0.304 | -0.330..0.266 0.944..1.000 -0.089..0.121 | -0.307..0.059 -0.121..0.088 0.952..1.000'
freed318='[  224501.000] inject  camwatch hop2 co-move [controller] @0x7677803278a0: 3722 tick(s), camera moved in 3231, the matrix moved WITH it in 2188, ALONE in 22 | max rotation 120.00 deg | 3x3 ranges 0.000..1.000 -0.266..0.330 -0.059..0.304 | -0.330..0.266 0.000..1.000 -0.089..0.121 | -0.307..0.059 -0.121..0.088 0.000..1.000'
plant /tmp/census-cw2-freed.log "$span360" "$good318" "$freed318"
want "hop2 freed final read -> named" /tmp/census-cw2-freed.log "camwatch hop2 \[controller\]" "read a FREED object"
want "hop2 freed final read -> scored on the line before (sway)" /tmp/census-cw2-freed.log "camwatch hop2 \[controller\] 3680" "TOO SMALL to be the free-look"
if python3 tools/score_run.py /tmp/census-cw2-freed.log 2>&1 | grep -q "FOLLOWS THE CAMERA"; then
    bad "hop2 freed final read -> still called the free-look"
else
    ok "hop2 freed final read -> not called the free-look"
fi
# Hop 3: does its angle EQUAL the camera's?
comove3() {  # comove3 <label> <maxrot> <bad> <far> <within3> <med> <p90>
    printf '[  224500.000] inject  camwatch hop2 co-move [%s] @0x7f0000002000: 3000 tick(s), camera moved in 2500, the matrix moved WITH it in 2400, ALONE in 5 | max rotation %s deg | %s not-a-transform read(s) skipped | match: far %s, within 3 deg %s, |dtheta| median <%s p90 <%s deg | 3x3 ranges 0..1' "$@"
}
plant /tmp/census-cw3.log "$span360" "$(comove3 camera.rot 178.0 0 1500 1300 2 5)" \
    "$(comove3 controller 19.0 3 1500 10 31 58)"
want "hop3 equal angle -> the free-look" /tmp/census-cw3.log "camwatch hop3 \[camera.rot\]" "HAS THE CAMERA'S ANGLE"
want "hop3 sway -> does not have the angle" /tmp/census-cw3.log "camwatch hop3 \[controller\]" "does NOT have the camera's angle"
want "hop3 freed reads counted -> shown" /tmp/census-cw3.log "camwatch hop2 \[controller\]" "3 freed/not-a-transform read(s) kept out"
plant /tmp/census-cw3-near.log "$span360" "$(comove3 camera.rot 40.0 0 120 100 2 4)"
want "hop3 too few far ticks -> not exercised" /tmp/census-cw3-near.log "camwatch hop3 \[camera.rot\]" "angle match NOT EXERCISED"
plant /tmp/census-cw3-mid.log "$span360" "$(comove3 camera.rot 90.0 0 900 300 6 20)"
want "hop3 in between -> partly, read the samples" /tmp/census-cw3-mid.log "camwatch hop3 \[camera.rot\]" "PARTLY"
# Take 319: camera.pos stayed at zero, but its move counts included the
# rotation rows after it -- static, not "moves with the camera".
plant /tmp/census-cw3-pos.log "$span360" '[  224500.000] inject  camwatch hop2 co-move [camera.pos] @0x7f0000003000: 2110 tick(s), camera moved in 1884, the matrix moved WITH it in 1854, ALONE in 18 | max displacement 0.0000 | 0 not-a-transform read(s) skipped | 3x3 ranges 0..1'
want "hop3 zero translation with moving neighbours -> STATIC" /tmp/census-cw3-pos.log "camwatch hop2 \[camera.pos\]" "STATIC — the translation never left its first value"
# Asked for hop 3 (the run line says so) and never censused -> FAIL.
sed 's/env: run = /env: run = X4VR_CAMWATCH=4 /' "$BASE" > /tmp/census-cw3-asked.log
want "hop3 asked, never censused -> FAIL" /tmp/census-cw3-asked.log FAIL "asked for hop 3"
absent "hop3 not asked -> silent" "$BASE" "camwatch hop3"

# ------------------------------------------ head-look's two idle predicates
#
# Take 318: menu opened (view AND HUD idle), then quit -- the view came back
# "cockpit" during teardown while the HUD stayed down. An identity pose inside
# a HUD-down window is head-look idle by design; one after the NULL guard
# announced "HUD is now up" is the defect, and must say TEARDOWN.
drv='[  999990.000] layer   vr pose: shared=1 cam_valid=1 declared yaw=1.63 pitch=13.87 deg (X4 1.63,13.87 + residual 0.00,0.00 applied, budget 0.00, 0/19280 clamped)'
idn='[  999995.000] layer   vr pose: shared=1 cam_valid=0 declared yaw=0.00 pitch=0.00 deg (X4 0.00,0.00 + residual 0.00,0.00 applied, budget 0.00, 0/22880 clamped)  <- IDENTITY, the image is world-locked'
vext='[  999991.000] inject  headlook: view is now external/floating — idle'
hdn='[  999991.100] inject  headlook: HUD is now down (IsInPanelMode=0) — idle, mouse handed back'
vck='[  999993.000] inject  headlook: view is now the cockpit — steering'
nul='[  999993.100] inject  camread: X4'"'"'s HUD global is NULL — X4 is tearing the HUD down (exit or load); IsHUDActive is not called while it stays NULL'
hup='[  999993.100] inject  headlook: HUD is now up (IsInPanelMode=0) — steering'
plant /tmp/census-hud-idle.log "$drv" "$vext" "$hdn" "$vck" "$nul" "$idn"
if python3 tools/score_run.py /tmp/census-hud-idle.log 2>&1 | grep -q "^FAIL  the pose fell back"; then
    bad "identity inside a HUD-down window -> still FAILs"
else
    ok "identity inside a HUD-down window -> excused"
fi
plant /tmp/census-hud-teardown.log "$drv" "$vext" "$hdn" "$vck" "$nul" "$hup" "$idn"
want "identity after the NULL guard said HUD up -> FAIL names TEARDOWN" /tmp/census-hud-teardown.log FAIL "at TEARDOWN"
# (take 306, the base, ends with the HUD down: close both windows first.)
vck0='[  999989.000] inject  headlook: view is now the cockpit — steering'
hup0='[  999989.100] inject  headlook: HUD is now up (IsInPanelMode=0) — steering'
plant /tmp/census-hud-midrun.log "$vck0" "$hup0" "$drv" "$idn"
want "identity with no announcement -> FAIL says mid-run" /tmp/census-hud-midrun.log FAIL "mid-run: the gate dropped out"
# Take 335: off the pilot seat the write stands down by design; identity
# there is correct. And a NULL HUD at LOAD (before any driven pose) is not
# teardown -- that run's FAIL said "at TEARDOWN" for frames 2 minutes later.
sdn='[  999994.000] inject  headlook: out of the pilot seat — the write stands down (on foot X4 does not render through the cockpit'"'"'s free-look, take 334); the picture is a window fixed in front'
sup='[  999996.000] inject  headlook: in the pilot seat — the write may act'
plant /tmp/census-seat-idle.log "$vck0" "$hup0" "$drv" "$sdn" "$idn" "$sup"
if python3 tools/score_run.py /tmp/census-seat-idle.log 2>&1 | grep -q "^FAIL  the pose fell back"; then
    bad "identity off the pilot seat -> still FAILs (take 335)"
else
    ok "identity off the pilot seat -> excused (take 335)"
fi
nul0='[  999988.000] inject  camread: X4'"'"'s HUD global is NULL — X4 is tearing the HUD down (exit or load); IsHUDActive is not called while it stays NULL'
plant /tmp/census-nul-load.log "$nul0" "$vck0" "$hup0" "$drv" "$idn"
want "NULL at load, identity later -> mid-run, not TEARDOWN (take 335)" /tmp/census-nul-load.log FAIL "mid-run: the gate dropped out"

# ------------------------------------------------ yaw differences wrap at 180
#
# Take 318 turned a full circle; an unwrapped |declared - X4| read 359.656 for
# a frame 0.344 off.
pl='[  999990.500] layer   poselag: declared=-179.8280,3.3230 x4=179.8280,3.3230 d=-359.6560,0.0000 age=0.0050 valid=1 rawvalid=1 residual=0.0000,0.0000 cmd=-179.8000,3.3000 rot=2'
plant /tmp/census-wrap.log "$pl" "$pl" "$pl"
if python3 tools/score_run.py /tmp/census-wrap.log 2>&1 | grep -E "^poselag  \|declared - X4\| yaw" | grep -q "max 359"; then
    bad "poselag across +-180 -> still reads ~360"
else
    ok "poselag across +-180 -> wrapped (no ~360 max)"
fi


# ------------------------------------------ head-look without MOUSELOOK
#
# X4VR_HEADLOOK_WRITE gates on intent: asked and never armed, never written,
# MOUSELOOK pressed, or x4vrmod_headlook not served -- each a FAIL. The base
# run's own lagsamples stand in for tracking.
sed 's/env: run = /env: run = X4VR_HEADLOOK_WRITE=1 /' "$BASE" > /tmp/census-hw-base.log
hwarm='[  999990.000] inject  headlook write: ARMED on camera 0x7f0000005000 (mode 0) — X4 reads maxpitch 75.0000 maxyaw 180.0000 (degrees), anglespeed 0.0000 angleacc 500.0000; base identity; attached 0x1 ref 0x2. MOUSELOOK will not be pressed'
hwext='[  999989.000] inject  content: serving /x/content-x4vrmod.xml in memory via fopen — x4vrmod_lookaround already enabled, x4vrmod_headlook ENABLED (was off); your /x/content.xml is not touched'
hwrep() { printf '[  999995.000] inject  headlook write: %s write(s), 0 failed, 12 refused tick(s) | last written yaw 12.50 pitch -3.00 | MOUSELOOK pressed %s time(s)' "$1" "$2"; }
{ cat /tmp/census-hw-base.log; printf '%s\n' "$hwext" "$hwarm" "$(hwrep 4000 0)"; } > /tmp/census-hw-ok.log
want "hwrite armed and writing -> reported" /tmp/census-hw-ok.log hwrite "4000 write(s)"
if python3 tools/score_run.py /tmp/census-hw-ok.log 2>&1 | grep -qE "^FAIL.*(HEADLOOK_WRITE|MOUSELOOK|write)"; then
    bad "hwrite healthy -> still FAILs"
else
    ok "hwrite healthy -> no write FAIL"
fi
{ cat /tmp/census-hw-base.log; printf '%s\n' "$hwext" '[  999990.000] inject  headlook write: REFUSED — anglespeed is not zero -- x4vrmod_headlook is not in force' "$(hwrep 0 0)"; } > /tmp/census-hw-never.log
want "hwrite never armed -> FAIL" /tmp/census-hw-never.log FAIL "never ARMED"
want "hwrite never armed -> the reason is printed" /tmp/census-hw-never.log hwrite "anglespeed is not zero"
{ cat /tmp/census-hw-base.log; printf '%s\n' "$hwext" "$hwarm" "$(hwrep 4000 3)"; } > /tmp/census-hw-ml.log
want "hwrite pressed MOUSELOOK -> FAIL" /tmp/census-hw-ml.log FAIL "pressed MOUSELOOK 3 time(s)"
{ cat /tmp/census-hw-base.log; printf '%s\n' "$hwarm" "$(hwrep 4000 0)"; } > /tmp/census-hw-noext.log
want "hwrite with neither the speed owner nor the extension -> FAIL" /tmp/census-hw-noext.log FAIL "neither the injector's per-mode speed (headlook speed) nor x4vrmod_headlook held X4's follower"
# Take 351: the injector holds the speed per camera mode instead.
hwspd='[  999985.500] inject  headlook speed: X4'"'"'s own <lookaround><internal> anglespeed 3.4907 (angleacc 8.7266) — held at 0 while the camera is in mode 0 (cockpit, walking) so a written angle stays, given back in every other mode so the external views orbit'
hwsp0='[  999985.600] inject  headlook speed: camera mode 0 — anglespeed held at 0 (0.0000)'
hwsp2='[  999988.000] inject  headlook speed: camera mode 2 — anglespeed given back to X4 (3.4907)'
{ cat /tmp/census-hw-base.log; printf '%s\n' "$hwspd" "$hwsp0" "$hwarm" "$hwsp2" "$(hwrep 4000 0)"; } > /tmp/census-hw-speed.log
want "hwrite with the per-mode speed -> reported" /tmp/census-hw-speed.log hwrite "follower speed: X4's own 3.4907, held at 0 in mode 0 (1 time(s)), given back in other modes (1 time(s))"
if python3 tools/score_run.py /tmp/census-hw-speed.log 2>&1 | grep -q "held X4's follower, so writes"; then
    bad "hwrite with the per-mode speed -> FAILs"
else
    ok "hwrite with the per-mode speed -> no follower FAIL"
fi
grep -qF '"headlook speed: X4'"'"'s own <lookaround><internal> anglespeed "' injector/x4vr_inject.cpp &&
grep -qF 'X4VR_LOG("headlook speed: camera mode %d — anglespeed %s (%.4f)",' injector/x4vr_inject.cpp &&
    ok "hwrite speed: the planted lines match the injector's formats" ||
    bad "hwrite speed: the injector's format changed — replant the lines above"
cp /tmp/census-hw-base.log /tmp/census-hw-silent.log
want "hwrite asked, mode never ran -> FAIL" /tmp/census-hw-silent.log FAIL "no write report"
plant /tmp/census-hw-session.log '[  999990.000] inject  headlook write: REFUSED for the session — code at 0x1935f1e differs from the analysed build; nothing will be written'
want "hwrite code mismatch -> FAIL" /tmp/census-hw-session.log FAIL "refused for the whole session"
absent "hwrite not asked -> silent" "$BASE" hwrite
# Take 321: the clamp block judged X4's stop on a written angle (which X4 does
# not clamp), and the cursor census called 599 hover changes MOUSELOOK
# suppression. Both are gated on intent now.
want "write mode -> clamp says it measures no stop" /tmp/census-hw-ok.log "clamp" "write mode: the written angle bypasses X4's look clamp"
if python3 tools/score_run.py /tmp/census-hw-ok.log 2>&1 | grep -q "clamp   DID NOT MOVE"; then
    bad "write mode -> clamp still says DID NOT MOVE"
else
    ok "write mode -> no DID NOT MOVE verdict"
fi
csets='[  999996.000] inject  cursor sets: 606 set(s) — handle found 606, NOT found 0, null 0 | 599 were a CHANGE (599 while head-look was STEERING, 0 while it was NOT; 599 in the COCKPIT, 0 elsewhere) | 17 mouse button press(es)'
{ cat /tmp/census-hw-ok.log; printf '%s\n' "$csets"; } > /tmp/census-hw-hover.log
want "write mode -> hover while steering is hover working" /tmp/census-hw-hover.log "cursor-set =>" "WHILE HEAD-LOOK WROTE THE CAMERA"
plant /tmp/census-ml-hover.log "$csets"
want "MOUSELOOK mode -> the old verdict stands" /tmp/census-ml-hover.log "cursor-set =>" "EVERY CHANGE CAME WHILE HEAD-LOOK WAS STEERING"
# Take 322's sway readout, judged on intent: nosway served and the sway
# still moving is a FAIL; at rest it is the extension working.
nsx='[  999989.500] inject  content: serving /x/content-x4vrmod.xml in memory via fopen — x4vrmod_lookaround already enabled, x4vrmod_nosway ENABLED (added), x4vrmod_headlook ENABLED (was off); your /x/content.xml is not touched'
swrep() { printf '[  999996.500] inject  headlook write: 4000 write(s), 0 failed, 12 refused tick(s) | last written yaw 1.00 pitch 2.00 | MOUSELOOK pressed 0 time(s) | sway max %s deg %s m this window, %s deg %s m run' "$1" "$2" "$1" "$2"; }
{ cat /tmp/census-hw-base.log; printf '%s\n' "$nsx" "$hwarm" "$(swrep 0.00 0.000)"; } > /tmp/census-sw-off.log
want "nosway served, sway at rest -> OFF" /tmp/census-sw-off.log hwrite "sway OFF"
{ cat /tmp/census-hw-base.log; printf '%s\n' "$nsx" "$hwarm" "$(swrep 18.40 0.060)"; } > /tmp/census-sw-on.log
want "nosway served, sway moving -> FAIL" /tmp/census-sw-on.log FAIL "still swayed 18.40 deg"
{ cat /tmp/census-hw-base.log; printf '%s\n' "$hwext" "$hwarm" "$(swrep 18.40 0.060)"; } > /tmp/census-sw-x4.log
want "nosway not served -> sway reported, no FAIL" /tmp/census-sw-x4.log hwrite "x4vrmod_nosway NOT served"


# ------------------------------------------ #70: the base-field A/B
sed 's/env: run = /env: run = X4VR_SHADOWFOV_AB=45 /' "$BASE" > /tmp/census-sf-base.log
sfarm='[  999990.000] inject  shadowfov: ARMED on camera 0x7f0000006000 — base 1.28700 effective 1.91982 (rendered field 1.91982 rad = 110.00 deg); phases of 45 s, CONTROL first'
sfp() { printf '[  %s] inject  shadowfov: phase %s %s — x' "$1" "$2" "$3"; }
sfr='[  999999.000] inject  shadowfov: phase 1 FIX | base 1.91982 effective 1.91982 | 3 base write(s), 0 effective restore(s), 0 failed'
{ cat /tmp/census-sf-base.log; printf '%s\n' "$sfarm" "$(sfp 999990.100 0 CONTROL)" "$(sfp 999995.000 1 FIX)" "$sfr"; } > /tmp/census-sf-ok.log
want "shadowfov switched -> phases counted" /tmp/census-sf-ok.log shadowfov "2 phase(s), 1 FIX / 1 CONTROL"
if python3 tools/score_run.py /tmp/census-sf-ok.log 2>&1 | grep -q "^FAIL.*SHADOWFOV"; then
    bad "shadowfov healthy -> still FAILs"
else
    ok "shadowfov healthy -> no FAIL"
fi
{ cat /tmp/census-sf-base.log; printf '%s\n' '[  999990.000] inject  shadowfov: REFUSED — camera 0x1 reads base 0.50000 effective 1.00000, not X4'"'"'s default 1.28700 and option 1.4917 x it'; } > /tmp/census-sf-never.log
want "shadowfov never armed -> FAIL" /tmp/census-sf-never.log FAIL "never ARMED"
{ cat /tmp/census-sf-base.log; printf '%s\n' "$sfarm" "$(sfp 999990.100 0 CONTROL)"; } > /tmp/census-sf-short.log
want "shadowfov ended before FIX -> FAIL" /tmp/census-sf-short.log FAIL "before the first FIX phase"
sfbad='[  999999.500] inject  shadowfov: phase 2 CONTROL | base 1.91982 effective 1.91982 | 818 base write(s), 0 effective restore(s), 0 failed'
{ cat /tmp/census-sf-base.log; printf '%s\n' "$sfarm" "$(sfp 999990.100 0 CONTROL)" "$(sfp 999995.000 1 FIX)" "$sfr" "$(sfp 999999.200 2 CONTROL)" "$sfbad"; } > /tmp/census-sf-collapse.log
want "shadowfov CONTROL reading the FIX value -> FAIL (take 322)" /tmp/census-sf-collapse.log FAIL "the arms collapsed"
absent "shadowfov not asked -> silent" "$BASE" shadowfov

# ------------------------------------------ #70: the rendered-field A/B
sed 's/env: run = /env: run = X4VR_FIELD_AB=45 /' "$BASE" > /tmp/census-fa-base.log
faarm='[  999980.000] inject  fieldab: ARMED on camera 0x7f0000007000 — A 1.4917 (1.91982 rad, 110.00 deg, sx 0.70024), B 1.2400 (1.59588 rad, 91.44 deg, sx 0.97522); phases of 45 s once game time runs'
fast='[  999985.000] inject  fieldab: STARTED — game time 812.0 s running, cockpit camera up'
fap() { printf '[  %s] inject  fieldab: phase %s %s — x' "$1" "$2" "$3"; }
farep='[  999999.000] inject  fieldab: phase 1 B | effective 1.59588 | held A 500/500 B 500/500 tick(s) | 2 write(s), 0 reverted by X4, 0 failed'
cuj() { printf '[  %s] layer   camuj: main cam@0 draws=110 proj sx %s sy %s | proj_uj sx %s sy %s | inv*proj 1.0e-06, inv_uj*proj_uj 1.0e-06, inv_uj*proj %s | 3 slot pair(s): %s/%s x3 (inv_uj miss 0, worst 1.0e-06)' "$1" "$2" "$2" "$3" "$3" "$4" "$2" "$3"; }
{ cat /tmp/census-fa-base.log; printf '%s\n' "$faarm" "$fast" "$(fap 999985.000 0 A)" "$(cuj 999987.000 0.70024 0.70024 2.0e-04)" "$(fap 999990.000 1 B)" "$(cuj 999993.000 0.97522 0.97522 2.0e-04)" "$farep"; } > /tmp/census-fa-ok.log
want "fieldab followed -> A drew 0.70024" /tmp/census-fa-ok.log "fieldab A:" "X4 DREW with sx 0.70024"
want "fieldab followed -> B drew 0.97522" /tmp/census-fa-ok.log "fieldab B:" "X4 DREW with sx 0.97522"
want "fieldab consistent inverse -> not a reconstruction error" /tmp/census-fa-ok.log "fieldab A:" "not a reconstruction error"
if python3 tools/score_run.py /tmp/census-fa-ok.log 2>&1 | grep -q "^FAIL.*fieldab"; then
    bad "fieldab healthy -> still FAILs"
else
    ok "fieldab healthy -> no FAIL"
fi
{ cat /tmp/census-fa-base.log; printf '%s\n' "$faarm" "$fast" "$(fap 999985.000 0 A)" "$(cuj 999987.000 0.70024 0.70024 2.0e-04)" "$(fap 999990.000 1 B)" "$(cuj 999993.000 0.70024 0.70024 2.0e-04)" "$farep"; } > /tmp/census-fa-stuck.log
want "fieldab B drew the wide field -> FAIL (the arms did not differ)" /tmp/census-fa-stuck.log FAIL "did not follow the switch"
{ cat /tmp/census-fa-base.log; printf '%s\n' "$faarm" "$fast" "$(fap 999985.000 0 A)" "$(cuj 999987.000 0.70024 1.33333 4.7e-01)" "$(fap 999990.000 1 B)" "$(cuj 999993.000 0.97522 1.33333 2.6e-01)" "$farep"; } > /tmp/census-fa-mis.log
want "fieldab shadow inverse on another field -> MISMATCH named" /tmp/census-fa-mis.log "fieldab A:" "MISMATCH"
want "camuj disagreeing slot pair -> listed" /tmp/census-fa-mis.log camuj "DISAGREE: 0.70024/1.33333"
{ cat /tmp/census-fa-base.log; printf '%s\n' "$faarm" '[  999990.000] inject  gametime: 0.00 s (running 0)'; } > /tmp/census-fa-never.log
want "fieldab never started (load screen only) -> FAIL" /tmp/census-fa-never.log FAIL "phases never started"
farev='[  999999.000] inject  fieldab: phase 1 B | effective 1.91982 | held A 500/500 B 100/500 tick(s) | 400 write(s), 399 reverted by X4, 0 failed'
{ cat /tmp/census-fa-base.log; printf '%s\n' "$faarm" "$fast" "$(fap 999985.000 0 A)" "$(fap 999990.000 1 B)" "$farev"; } > /tmp/census-fa-rev.log
want "fieldab X4 reverting the write -> FAIL" /tmp/census-fa-rev.log FAIL "phase B held its field on only 20%"
absent "fieldab not asked -> silent" "$BASE" fieldab
# Take 323: the most-drawn block sat at X4's default field (sx 1.33333) in both
# phases while ~30 blocks per reading followed the switch. Judged by the
# most-drawn block, the scorer FAILed a switch Patola saw happen.
cuj323() {  # cuj323 <t> <sx> <ladder sx>
    printf '[  %s] layer   camuj: main cam@84224 draws=117 proj sx 1.33333 sy -1.33333 | proj_uj sx 1.33333 sy -1.33333 | inv*proj 0.0e+00, inv_uj*proj_uj 0.0e+00, inv_uj*proj 0.0e+00 | 4 slot pair(s): %s/%s x1 (inv_uj miss 1, worst 4.6e+03), 0.00142/0.00142 x1 (inv_uj miss 0, worst 6.0e-08), %s/%s x30 (inv_uj miss 0, worst 0.0e+00), 1.33333/1.33333 x3 (inv_uj miss 0, worst 0.0e+00)' "$1" "$3" "$3" "$2" "$2"; }
{ cat /tmp/census-fa-base.log; printf '%s\n' "$faarm" "$fast" "$(fap 999985.000 0 A)" "$(cuj323 999987.000 0.70024 0.47213)" "$(fap 999990.000 1 B)" "$(cuj323 999993.000 0.97522 0.38799)" "$farep"; } > /tmp/census-fa-323.log
want "fieldab most-drawn block at 1.33333, blocks followed -> A drew 0.70024" /tmp/census-fa-323.log "fieldab A:" "X4 DREW with sx 0.70024 in 30"
want "fieldab most-drawn block named as context only" /tmp/census-fa-323.log "fieldab B:" "most-drawn block read sx 1.33333"
if python3 tools/score_run.py /tmp/census-fa-323.log 2>&1 | grep -q "^FAIL.*fieldab"; then
    bad "fieldab take-323 shape -> still FAILs"
else
    ok "fieldab take-323 shape -> no FAIL"
fi
want "fieldab a block that moved with the field -> listed" /tmp/census-fa-323.log "fieldab other" "A 0.47213 / B 0.38799"
want "fieldab the block that did not move -> not listed" /tmp/census-fa-323.log "fieldab   as 2/sx" "A 4.236 / B 5.155"
want "fieldab consistent blocks -> the instrument's range stated" /tmp/census-fa-323.log "fieldab A:" "checked against ITSELF"

# ------------------------------------------ #70's fix: the cascade fit's divisors
sed 's/env: run = /env: run = X4VR_CSM_COVER=0.663 X4VR_CSM_AB=45 X4VR_CAMDIFF=1 /' "$BASE" > /tmp/census-cf-base.log
cfpatch='[  999980.000] inject  csmfit: PATCHED — the cascade fit'"'"'s 4 reads of X4'"'"'s aspect (0x4533b00/04) now read 0x3f0000/0x3f0004; coverage 0.663, CONTROL/FIX phases once game time runs'
cfstart='[  999985.000] inject  csmfit: STARTED — game time 812.0 s running, cockpit camera up'
cfph() { printf '[  %s] inject  csmfit: phase %s %s — x' "$1" "$2" "$3"; }
cfrep() {  # cfrep <t> <phase> <arm> <coverage> <predicted sx>
    printf '[  %s] inject  csmfit: phase %s %s | field 1.91982 aspect 1.0000/1.0000 | divisors 1.00000/1.00000 | coverage %s | predicted cascade-0 sx %s (r 2.118 m, far 3.060) | ticks FIX 1 CONTROL 1' "$1" "$2" "$3" "$4" "$5"; }
{ cat /tmp/census-cf-base.log; printf '%s\n' "$cfpatch" "$cfstart" "$(cfph 999985.000 0 CONTROL)" "$(cfrep 999986.500 0 CONTROL 0.245 0.47215)" "$(cuj323 999987.000 0.70024 0.47213)" "$(cfph 999990.000 1 FIX)" "$(cfrep 999991.500 1 FIX 0.663 0.22954)" "$(cuj323 999993.000 0.70024 0.22954)"; } > /tmp/census-cf-ok.log
want "csmfit patched -> said" /tmp/census-cf-ok.log csmfit "PATCHED — the fit's 4 reads retargeted, coverage 0.663"
want "csmfit FIX followed -> X4 drew 0.22954" /tmp/census-cf-ok.log "csmfit FIX" "X4 drew cascade 0 at sx 0.22954"
want "csmfit CONTROL -> X4's own 0.47213" /tmp/census-cf-ok.log "csmfit CONTROL" "X4 drew cascade 0 at sx 0.47213"
if python3 tools/score_run.py /tmp/census-cf-ok.log 2>&1 | grep -q "^FAIL.*csmfit"; then
    bad "csmfit healthy -> still FAILs"
else
    ok "csmfit healthy -> no FAIL"
fi
{ cat /tmp/census-cf-base.log; printf '%s\n' "$cfpatch" "$cfstart" "$(cfph 999985.000 0 CONTROL)" "$(cfrep 999986.500 0 CONTROL 0.245 0.47215)" "$(cuj323 999987.000 0.70024 0.47213)" "$(cfph 999990.000 1 FIX)" "$(cfrep 999991.500 1 FIX 0.663 0.22954)" "$(cuj323 999993.000 0.70024 0.47213)"; } > /tmp/census-cf-collapse.log
want "csmfit FIX drawing X4's own box -> FAIL (arms did not differ)" /tmp/census-cf-collapse.log FAIL "did not follow the divisors"
{ cat /tmp/census-cf-base.log; printf '%s\n' '[  999980.000] inject  csmfit: REFUSED — the fit'"'"'s read at 0x209539f differs from X4 9.00. X4'"'"'s cascade fit is untouched.'; } > /tmp/census-cf-refused.log
want "csmfit refused -> FAIL" /tmp/census-cf-refused.log FAIL "X4VR_CSM_COVER refused: the fit's read at 0x209539f"
cp /tmp/census-cf-base.log /tmp/census-cf-nopatch.log
want "csmfit asked, never patched -> FAIL" /tmp/census-cf-nopatch.log FAIL "no PATCHED line"
{ cat /tmp/census-cf-base.log; printf '%s\n' "$cfpatch"; } > /tmp/census-cf-never.log
want "csmfit phases never started -> FAIL" /tmp/census-cf-never.log FAIL "X4VR_CSM_AB asked, but the phases never started"
absent "csmfit not asked -> silent" "$BASE" csmfit
# The FIX-always form (no X4VR_CSM_AB, take 326 on): menus and loading have no
# player camera, the fit runs on X4's own values there, and the reports say so
# (field 0, no prediction). Those readings must not count as FIX misses.
sed 's/env: run = /env: run = X4VR_CSM_COVER=0.663 X4VR_CAMDIFF=1 /' "$BASE" > /tmp/census-cf1-base.log
cf1patch='[  999980.000] inject  csmfit: PATCHED — the cascade fit'"'"'s 4 reads of X4'"'"'s aspect (0x4533b00/04) now read 0x3f0000/0x3f0004; coverage 0.663, FIX always'
cfmenu='[  999981.000] inject  csmfit: phase 0 CONTROL | field 0.00000 aspect 1.0000/1.0000 | divisors 1.00000/1.00000 | coverage 0.000 | predicted cascade-0 sx 0.00000 (r 0.000 m, far 3.060) | ticks FIX 0 CONTROL 1'
{ cat /tmp/census-cf1-base.log; printf '%s\n' "$cf1patch" "$cfmenu" "$(cuj323 999982.000 1.33333 0.30841)" "$(cfrep 999990.000 0 FIX 0.663 0.22954)" "$(cuj323 999991.000 0.70024 0.22954)"; } > /tmp/census-cf1-ok.log
want "csmfit FIX always -> said" /tmp/census-cf1-ok.log csmfit "coverage 0.663, FIX always"
want "csmfit FIX always -> the flight reading judged" /tmp/census-cf1-ok.log "csmfit FIX" "1/1 within 1%"
want "csmfit FIX always -> the menu reading set aside" /tmp/census-cf1-ok.log "csmfit 1 reading" "not judged"
if python3 tools/score_run.py /tmp/census-cf1-ok.log 2>&1 | grep -q "^FAIL.*csmfit"; then
    bad "csmfit FIX always, healthy with a menu -> still FAILs"
else
    ok "csmfit FIX always, healthy with a menu -> no FAIL"
fi
{ cat /tmp/census-cf1-base.log; printf '%s\n' "$cf1patch" "$(cfrep 999990.000 0 FIX 0.663 0.22954)" "$(cuj323 999991.000 0.70024 0.47213)"; } > /tmp/census-cf1-bad.log
want "csmfit FIX always, X4 drew its own box -> FAIL" /tmp/census-cf1-bad.log FAIL "did not follow the divisors"
# Take 326: some frames draw only the far cascades. The largest block listed is
# then cascade 3, far BELOW the prediction -- set aside, not a miss.
{ cat /tmp/census-cf1-base.log; printf '%s\n' "$cf1patch" "$(cfrep 999990.000 0 FIX 0.663 0.22954)" "$(cuj323 999991.000 0.70024 0.22954)" "$(cuj323 999992.000 0.70024 0.00091)" "$(cuj323 999993.000 0.70024 0.00091)"; } > /tmp/census-cf1-far.log
want "csmfit a frame with only far cascades -> set aside" /tmp/census-cf1-far.log "csmfit 2 reading" "without cascade 0"
want "csmfit a frame with only far cascades -> not a miss" /tmp/census-cf1-far.log "csmfit FIX" "1/1 within 1%"

# ------------------------------------------ X4VR_CURSOR_CANVAS=auto
sed 's/env: run = /env: run = X4VR_CURSOR_CANVAS=auto /' "$BASE" > /tmp/census-ca-base.log
caup='[  999990.000] inject  ui: full-screen menu UP — the cursor is drawn through the UI canvas'
caon='[  999990.020] layer   cursor canvas: AUTO — ON (a full-screen menu is up: the cursor follows X4'"'"'s UI)'
cadown='[  999995.000] inject  ui: full-screen menu DOWN — the cursor is drawn at the raw pointer'
caoff='[  999995.020] layer   cursor canvas: AUTO — OFF (no full-screen menu: the cursor at the raw pointer, where X4 picks the world)'
{ cat /tmp/census-ca-base.log; printf '%s\n' "$caup" "$caon" "$cadown" "$caoff"; } > /tmp/census-ca-ok.log
want "cursor auto: menu seen and switched -> counted" /tmp/census-ca-ok.log cursor-auto "full-screen menu 1 time(s); the layer switched the cursor to the canvas 1 time(s)"
if python3 tools/score_run.py /tmp/census-ca-ok.log 2>&1 | grep -q "^FAIL.*CURSOR_CANVAS"; then
    bad "cursor auto healthy -> still FAILs"
else
    ok "cursor auto healthy -> no FAIL"
fi
{ cat /tmp/census-ca-base.log; printf '%s\n' "$caup" "$cadown"; } > /tmp/census-ca-stuck.log
want "cursor auto: menu seen, layer never switched -> FAIL" /tmp/census-ca-stuck.log FAIL "the switch did not reach the layer"
want "cursor auto: no map opened -> said" /tmp/census-ca-base.log cursor-auto "the switch was not exercised"
absent "cursor auto not asked -> silent" "$BASE" cursor-auto

# ------------------------------------------ cursor stand-in and Lua reconnaissance
cdlines='[  999980.000] inject  sdl: X4 has set no cursor yet — showing its first-built one (#0, 32x32 hot=(7,8)) until it picks one'
{ cat "$BASE"; printf '%s\n' "$cdlines" '[  999990.000] inject  sdl: X4 set cursor #5' '[  999991.000] inject  sdl: X4 set cursor #0' '[  999992.000] inject  sdl: X4 set cursor #5'; } > /tmp/census-cd.log
want "cursor stand-in shown -> said" /tmp/census-cd.log cursor-default "X4's cursor #0 (32x32, hot 7,8) shown until X4 set one"
want "cursor stand-in not X4's first choice -> NOTE" /tmp/census-cd.log cursor-default "X4's own first choice was #5, not the stand-in #0"
{ cat "$BASE"; printf '%s\n' '[  999980.000] inject  sdl: X4 has set no cursor yet — showing its arrow (#5, 32x32 hot=(11,6)) until it picks one' '[  999990.000] inject  sdl: X4 set cursor #24 at pointer 500,166'; } > /tmp/census-cd5.log
want "cursor stand-in #5 -> said" /tmp/census-cd5.log cursor-default "X4's cursor #5 (32x32, hot 11,6) shown until X4 set one"
lua1='[  999970.000] inject  lua[buf] ui/core/lua/crosshair handling.lua (51234 bytes, bytecode, L=0x1000)'
lua2='[  999971.000] inject  lua[buf] ui/addons/ego_interactmenu/menu_interactmenu.lua (99999 bytes, bytecode, L=0x2000)'
{ cat "$BASE"; printf '%s\n' "$lua1" "$lua2"; } > /tmp/census-lua.log
want "lua: two states -> DIFFERENT named" /tmp/census-lua.log lua "DIFFERENT Lua states"
want "lua: kinds counted" /tmp/census-lua.log lua "2 chunk(s): bytecode 2 | 2 Lua state(s)"
absent "lua: no X4VR_LUA -> silent" "$BASE" "lua     "
{ cat "$BASE"; printf '%s\n' '[  999990.000] inject  sdl: X4 set cursor #7 at pointer 400,700 [full-screen menu]' '[  999991.000] inject  sdl: X4 set cursor #0 at pointer 2400,2100 [full-screen menu]' '[  999992.000] inject  sdl: X4 set cursor #7 at pointer 1400,1400'; } > /tmp/census-edges.log
want "cursor edges: menu changes bounded" /tmp/census-edges.log cursor-edges "2 shape change(s) inside full-screen menus: pointer x 400..2400, y 700..2100"

# ------------------------------------------ menucam: X4's camera under a menu
# Take 328B: the map opened under head-look write kept X4's camera at the
# head's angle, and 13.6% of X4's UI projected outside the window.
sed 's/env: run = /env: run = X4VR_HEADLOOK_WRITE=1 /' "$BASE" > /tmp/census-mc-base.log
mcarm='[  999980.000] inject  headlook write: ARMED on camera 0x1234 (mode 0) — X4 reads maxpitch 1.3090 maxyaw 3.1416 (radians), anglespeed 0.0000 angleacc 8.7266; base identity; attached 0x1 ref 0x2. MOUSELOOK will not be pressed'
mcdown='[  999989.900] inject  headlook: view is now the cockpit — steering
[  999990.000] inject  headlook: HUD is now down (IsInPanelMode=0) — idle, mouse handed back'
mcup='[  999995.000] inject  headlook: HUD is now up (IsInPanelMode=0) — steering'
mcpose() {  # mcpose <t> <yaw> <pitch>
    printf '[  %s] layer   poselag: declared=0.0000,0.0000 x4=%s,%s d=0,0 age=0.0085 valid=0 rawvalid=1 residual=0.0000,0.0000 cmd=0,0 rot=2' "$1" "$2" "$3"
}
mcrep='[  999996.000] inject  headlook write: 900 write(s), 0 failed, 0 refused tick(s) | last written yaw 0.00 pitch 0.00 | MOUSELOOK pressed 0 time(s) | menu centring 1 period(s) 300 write(s) | sway not read'
mccen='[  999990.020] inject  headlook: HUD down in the cockpit — X4'"'"'s camera CENTRED from yaw -1.99 pitch 6.37 to 0,0, so X4'"'"'s menus fill the window'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$(mcpose 999990.500 -1.9851 6.3677)" "$(mcpose 999991.500 -1.9851 6.3677)" "$(mcpose 999992.500 -1.9851 6.3677)" "$mcup"; } > /tmp/census-mc-frozen.log
want "menucam 328B shape -> the share out of reach" /tmp/census-mc-frozen.log menucam "OFF-CENTRE  13.6% of X4's UI out of reach"
want "menucam 328B shape -> FROZEN named" /tmp/census-mc-frozen.log "menucam =>" "FROZEN: in 1 cockpit or ESC-pause period(s)"
if python3 tools/score_run.py /tmp/census-mc-frozen.log 2>&1 | grep -q "^FAIL.*MENU_CENTRE"; then
    bad "menucam on a build without the centring -> FAILs (gated on outcome)"
else
    ok "menucam on a build without the centring -> no FAIL"
fi
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mccen" "$(mcpose 999990.500 0.0000 0.0000)" "$(mcpose 999991.500 0.0000 0.0000)" "$mcup" "$mcrep"; } > /tmp/census-mc-centred.log
want "menucam centred -> every period CENTRED" /tmp/census-mc-centred.log "menucam =>" "every cockpit and ESC-pause menu period CENTRED"
want "menucam centred -> the centring counted" /tmp/census-mc-centred.log menucam "1 period(s) centred by X4VR_HEADLOOK_MENU_CENTRE, from -1.99,+6.37"
want "menucam centred -> 0.0% out of reach" /tmp/census-mc-centred.log menucam "CENTRED      0.0% of X4's UI out of reach"
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$(mcpose 999990.500 -1.9851 6.3677)" "$(mcpose 999991.500 -1.9851 6.3677)" "$mcup" "$mcrep"; } > /tmp/census-mc-stuck.log
want "menucam asked, still frozen -> FAIL" /tmp/census-mc-stuck.log FAIL "the centring did not act"
sed 's/env: run = /env: run = X4VR_HEADLOOK_MENU_CENTRE=0 /' /tmp/census-mc-stuck.log > /tmp/census-mc-off.log
if python3 tools/score_run.py /tmp/census-mc-off.log 2>&1 | grep -q "^FAIL.*MENU_CENTRE"; then
    bad "menucam switched off -> still FAILs"
else
    ok "menucam switched off -> no FAIL"
fi
# Take 368: a centred map, then X4 switched to an external view with the
# map still up; the period's last 0.4 s read the orbit camera (-68.44). Cut
# at the view change: no FAIL. The negative control keeps the same off-centre
# angle in the cockpit and must still FAIL.
mcext='[  999993.000] inject  headlook: view is now external — idle'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mccen" "$(mcpose 999990.500 0.0000 0.0000)" "$(mcpose 999991.500 0.0000 0.0000)" "$(mcpose 999992.500 0.0000 0.0000)" "$mcext" "$(mcpose 999993.500 -68.4390 -0.5800)" "$(mcpose 999994.700 -68.4390 -0.5800)" "$mcup" "$mcrep"; } > /tmp/census-mc-ext.log
if python3 tools/score_run.py /tmp/census-mc-ext.log 2>&1 | grep -q "the centring did not act"; then
    bad "menucam: centred, then an external view -> FAILs (judged the orbit camera)"
else
    ok "menucam: centred, then an external view -> no FAIL"
fi
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mccen" "$(mcpose 999990.500 0.0000 0.0000)" "$(mcpose 999991.500 0.0000 0.0000)" "$(mcpose 999993.500 -68.4390 -0.5800)" "$(mcpose 999994.700 -68.4390 -0.5800)" "$mcup" "$mcrep"; } > /tmp/census-mc-extctl.log
want "menucam: the same angle in the cockpit -> still FAIL" /tmp/census-mc-extctl.log FAIL "the centring did not act"
absent "menucam: no write-mode arming -> silent" "$BASE" menucam
# Take 337: an ESC menu ON FOOT read X4's camera at the BODY's +60.9 deg
# (the mouse turns the body there) and FAILed as "frozen" with no head-look
# on foot at all. Off the seat: not judged without X4VR_HEADLOOK_FOOT; judged
# by head-look's centring line with it.
mcsdn='[  999985.000] inject  headlook: out of the pilot seat — the write stands down (on foot X4 does not render through the cockpit'"'"'s free-look, take 334); the picture is a window fixed in front'
mcscont='[  999985.000] inject  headlook: out of the pilot seat — head-look continues on foot (X4VR_HEADLOOK_FOOT): the head looks, the mouse and arrows turn the body, the mouse'"'"'s pitch is zeroed'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcsdn" "$mcdown" "$(mcpose 999990.500 60.9000 8.1400)" "$(mcpose 999991.500 60.9000 8.1400)" "$mcup" "$mcrep"; } > /tmp/census-mc-foot.log
want "menucam on foot, no head-look there -> not judged (take 337)" /tmp/census-mc-foot.log menucam "off the pilot seat — X4's own camera (no head-look on foot), not judged"
if python3 tools/score_run.py /tmp/census-mc-foot.log 2>&1 | grep -q "^FAIL.*MENU_CENTRE"; then
    bad "menucam on foot, no head-look there -> still FAILs (take 337)"
else
    ok "menucam on foot, no head-look there -> no FAIL (take 337)"
fi
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcscont" "$mcdown" "$mccen" "$(mcpose 999990.500 60.9000 8.1400)" "$mcup" "$mcrep"; } > /tmp/census-mc-foot-cen.log
want "menucam on foot with head-look, centred -> judged by the centring line" /tmp/census-mc-foot-cen.log menucam "off the pilot seat — head-look CENTRED it"
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcscont" "$mcdown" "$(mcpose 999990.500 60.9000 8.1400)" "$mcup" "$mcrep"; } > /tmp/census-mc-foot-stuck.log
want "menucam on foot with head-look, not centred -> FAIL" /tmp/census-mc-foot-stuck.log FAIL "the centring did not act"
# Take 329: the ESC menu PAUSES, and the pre-329 view line called that
# "external/floating" -- the centring, gated on the cockpit, never acted.
mcpaused='[  999989.950] inject  headlook: view is now PAUSED (IsGamePaused: the ESC menu) — idle'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mcpaused" "$(mcpose 999990.500 2.1700 9.2400)" "$(mcpose 999991.500 2.1700 9.2400)" "$mcup" "$mcrep"; } > /tmp/census-mc-esc-stuck.log
want "menucam ESC pause frozen -> counted as a menu period" /tmp/census-mc-esc-stuck.log "menucam =>" "FROZEN: in 1 cockpit or ESC-pause period(s)"
want "menucam ESC pause frozen, asked -> FAIL" /tmp/census-mc-esc-stuck.log FAIL "the centring did not act"
mccenp='[  999990.020] inject  headlook: HUD down in the cockpit (PAUSED) — X4'"'"'s camera CENTRED from yaw 2.17 pitch 9.24 to 0,0, so X4'"'"'s menus fill the window'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mcpaused" "$mccenp" "$(mcpose 999990.500 0.0000 0.0000)" "$(mcpose 999991.500 0.0000 0.0000)" "$mcup" "$mcrep"; } > /tmp/census-mc-esc-ok.log
want "menucam ESC pause centred -> CENTRED" /tmp/census-mc-esc-ok.log "menucam =>" "every cockpit and ESC-pause menu period CENTRED"
want "menucam ESC pause centred -> its (PAUSED) line counted" /tmp/census-mc-esc-ok.log menucam "1 period(s) centred by X4VR_HEADLOOK_MENU_CENTRE, from +2.17,+9.24"
mcold='[  999989.950] inject  headlook: view is now external/floating — idle'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mcold" "$(mcpose 999990.500 2.1700 9.2400)" "$mcup"; } > /tmp/census-mc-oldlabel.log
want "menucam pre-329 label -> NOTE names the pause" /tmp/census-mc-oldlabel.log "menucam NOTE" "pre-329 label"

# ------------------------------------------ X4VR_STEER: the steering target
sed 's/env: run = /env: run = X4VR_STEER=b /' "$BASE" > /tmp/census-st-base.log
stok='[  999970.000] inject  steer: all 16 ShouldShowMouseSteeringHelperLine sites match X4 9.00 — the input object is at global 0x3daaaf8; X4'"'"'s own line is hidden and the layer draws the target'
stcfg='[  999970.100] inject  config: mousesteeringline: '"'"'absent'"'"' -> '"'"'false'"'"' (X4VR_STEER: the layer draws the steering line; your value is kept in /x/config-x4vrmod-mousesteeringline.was)'
ston='[  999980.000] inject  steer: overlay ON, starting in B — the apostrophe key switches A/B in the cockpit'
stgon='[  999990.000] inject  steer: X4'"'"'s steering line would be ON — the layer draws the target'
stgoff='[  999995.000] inject  steer: X4'"'"'s steering line would be OFF'
sttogA='[  999991.000] inject  milestone: steering target A — fixed to the cockpit (apostrophe for B)'
sttogB='[  999993.000] inject  milestone: steering target B — the line from where you look (apostrophe for A)'
stdrawn='[  999996.000] layer   steer drawn (final): B 300 frame(s), A 120 frame(s) | line in 400 | A with the nose behind the eye -> hidden in 7'
stdrawnB='[  999996.000] layer   steer drawn (final): B 300 frame(s), A 0 frame(s) | line in 280 | A asked but nose off the eye -> B in 0'
{ cat /tmp/census-st-base.log; printf '%s\n' "$stok" "$stcfg" "$ston" "$stgon" "$sttogA" "$sttogB" "$stgoff" "$stdrawn"; } > /tmp/census-st-ok.log
want "steer healthy -> sites, start mode, line hidden" /tmp/census-st-ok.log steer "16 code sites match; overlay ON from B; X4's own line hidden through the config fork"
want "steer healthy -> toggles counted in order" /tmp/census-st-ok.log steer "apostrophe toggles: 2 (A -> B)"
want "steer healthy -> frames by mode" /tmp/census-st-ok.log steer "drew the target in 420 frame(s): B 300, A 120 | with a line 400 | A with the nose behind the eye, nothing drawn: 7"
streclines='[  999989.000] layer   steer: both eyes show x -0.588..0.588 y -0.676..1.000 of the rendered eye — mode A falls back to B outside it
[  999990.010] inject  steer: mouse steering engaged — pointer recentred from 125,62 to 1408,1408 so the ship starts straight'
{ cat /tmp/census-st-ok.log; printf '%s\n' "$streclines"; } > /tmp/census-st-rec.log
want "steer -> take 332's sight box reported as removed" /tmp/census-st-rec.log steer "(take 332's fallback box, since removed)"
want "steer -> recentres counted with their origin" /tmp/census-st-rec.log steer "engaged 1 time(s) with the pointer recentred, from 125,62"
{ cat /tmp/census-st-base.log; printf '%s\n' "$stok" "$stcfg" "$ston" "$stgon" "$stgoff" '[  999996.000] layer   steer drawn (final): B 7056 frame(s), A 3399 frame(s) | line in 10384 | A asked but nose off the eye -> B in 0'; } > /tmp/census-st-331.log
want "steer, take 331's wording -> still read" /tmp/census-st-331.log steer "drew the target in 10455 frame(s): B 7056, A 3399"
want "steer, take 331's wording -> named as the old fallback" /tmp/census-st-331.log steer "A fell back to B (the pre-333 fallback): 0"
if python3 tools/score_run.py /tmp/census-st-ok.log 2>&1 | grep -q "^FAIL.*X4VR_STEER"; then
    bad "steer healthy -> still FAILs"
else
    ok "steer healthy -> no FAIL"
fi
{ cat /tmp/census-st-base.log; printf '%s\n' '[  999970.000] inject  steer: REFUSED — X4'"'"'s code at 0xa45ce7 differs from the analysed build; X4VR_STEER does nothing and X4'"'"'s own line stays'; } > /tmp/census-st-ref.log
want "steer refused -> FAIL" /tmp/census-st-ref.log FAIL "X4's code differs from the analysed build"
{ cat /tmp/census-st-base.log; printf '%s\n' "$stok" "$ston" "$stgon" "$stdrawnB"; } > /tmp/census-st-nocfg.log
want "steer, X4's line not hidden -> FAIL" /tmp/census-st-nocfg.log FAIL "X4's own steering line was never hidden"
{ cat /tmp/census-st-base.log; printf '%s\n' "$stok" "$stcfg" "$ston" "$stgon" "$stgoff"; } > /tmp/census-st-nodraw.log
want "steer, X4 showed its line, layer never drew -> FAIL" /tmp/census-st-nodraw.log FAIL "the layer never drew the target"
{ cat /tmp/census-st-base.log; printf '%s\n' "$stok" "$stcfg" "$ston" "$stgon" "$sttogA" "$stgoff" "$stdrawnB"; } > /tmp/census-st-noA.log
want "steer, A selected but never drawn -> FAIL" /tmp/census-st-noA.log FAIL "A was selected but the layer never drew A"
{ cat /tmp/census-st-base.log; printf '%s\n' "$stok" "$stcfg" "$ston"; } > /tmp/census-st-idle.log
want "steer asked, no mouse steering -> not exercised" /tmp/census-st-idle.log steer "the overlay was not exercised"
{ cat "$BASE"; printf '%s\n' '[  999970.100] inject  config: mousesteeringline: '"'"'false'"'"' -> '"'"'true'"'"' — restored (no X4VR_STEER this run)'; } > /tmp/census-st-rest.log
want "steer not asked, value restored -> said" /tmp/census-st-rest.log steer "not asked; mousesteeringline restored to 'true'"
absent "steer not asked -> silent" "$BASE" "steer "

# ------------------------------------------ the session gate without X4VR_MV
# The render-pass inventory is printed only under X4VR_MV, so a flatscreen run
# (take 323) was refused as "stopped early" with its measurement in the file.
grep -v "mv: X4 uses" "$BASE" | sed 's/ X4VR_MV=1//' > /tmp/census-mono.log
want "no X4VR_MV, one X4 instance -> scored as mono" /tmp/census-mono.log mono "a flatscreen run"
if python3 tools/score_run.py /tmp/census-mono.log 2>&1 | grep -q "^UNSCORABLE"; then
    bad "no X4VR_MV -> still UNSCORABLE"
else
    ok "no X4VR_MV -> not UNSCORABLE"
fi
grep -v "mv: X4 uses" "$BASE" > /tmp/census-mv-noinv.log
want "X4VR_MV=1 but no inventory -> still stopped early" /tmp/census-mv-noinv.log UNSCORABLE "stopped early"
absent "X4VR_MV=1 -> no mono line" "$BASE" mono

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

# ------------------------------------------ X4VR_FOOTLOG: the on-foot census
# The logs come from x4vr_test_foot --emit, i.e. the injector's own
# accumulation and formatters, so the regex is checked against what is
# printed rather than a hand copy of it.
FOOT=build/tests/x4vr_test_foot
if [ ! -x "$FOOT" ]; then
    bad "foot: $FOOT not built — the census cases cannot run"
else
    sed 's/env: run = /env: run = X4VR_FOOTLOG=1 /' "$BASE" > /tmp/census-ft-base.log
    { cat /tmp/census-ft-base.log; "$FOOT" --emit follows; } > /tmp/census-ft-follows.log
    want "foot follows -> census and regimes" /tmp/census-ft-follows.log "foot " "census ON: 2 regime line(s), 30 window(s)"
    want "foot follows -> seated head-only: local follows the head (the write)" /tmp/census-ft-follows.log "foot " "head-only (6 windows): local yaw FOLLOWS the head"
    want "foot follows -> seated pointer moves nothing" /tmp/census-ft-follows.log "foot " "mouse-only (4 windows): local yaw does not follow mouse dx"
    want "foot follows -> off-seat verdict names the mouse's matrices" /tmp/census-ft-follows.log "foot =>" "mode 1 off the seat: the mouse turns rot yaw, rot pitch, local pitch, base yaw; the head turns nothing"
    want "foot follows -> the gain is read with its sign" /tmp/census-ft-follows.log "foot " "base yaw FOLLOWS mouse dx (r -1.00, -0.1000 deg/count)"
    { cat /tmp/census-ft-base.log; "$FOOT" --emit nothing; } > /tmp/census-ft-nothing.log
    want "foot nothing -> says the view lives elsewhere" /tmp/census-ft-nothing.log "foot =>" "the mouse turns NOTHING we read (the walking view lives elsewhere — the attached object)"
    { cat /tmp/census-ft-base.log; "$FOOT" --emit dropped; } > /tmp/census-ft-dropped.log
    want "foot mouse zeroed by head-look -> no verdict on the mouse (take 334)" /tmp/census-ft-dropped.log "foot =>" "the mouse turns ? (head-look ZEROED 200 mouse motion event(s) before X4 saw them)"
    sed 's/; sampled BEFORE head-look.s write//' /tmp/census-ft-follows.log | sed 's/env: run = /env: run = X4VR_HEADLOOK_WRITE=1 /' > /tmp/census-ft-after.log
    want "foot sampled after the write -> local column flagged as our own" /tmp/census-ft-after.log "foot " "the local column is our own write read back"
    sed 's/env: run = /env: run = X4VR_HEADLOOK_WRITE=1 /' /tmp/census-ft-follows.log > /tmp/census-ft-nostand.log
    want "foot off the seat, write never stood down -> FAIL" /tmp/census-ft-nostand.log "FAIL|  - " "the write never stood down"
    { cat /tmp/census-ft-nostand.log
      echo '[  999010.040] inject  headlook: out of the pilot seat — the write stands down (on foot X4 does not render through the cockpit'"'"'s free-look, take 334); the picture is a window fixed in front'; } > /tmp/census-ft-stand.log
    want "foot stood down -> counted" /tmp/census-ft-stand.log "foot " "seat: the write stood down 1 time(s) off the seat, continued on foot 0 time(s), resumed 0 time(s)"
    # B: X4VR_HEADLOOK_FOOT. The patch line, the seat line, and the head now
    # turning X4's camera on foot.
    sed 's/env: run = /env: run = X4VR_HEADLOOK_WRITE=1 X4VR_HEADLOOK_FOOT=1 /' "$BASE" > /tmp/census-fp-base.log
    fpok='[  999000.000] inject  foot: PATCHED — 0x1929f70'"'"'s walking exit (je at 0x1929ffd, the camera with no controller) now goes through a stub at 0x7c00000: the player'"'"'s camera composes its own rotation (0x192a284), any other returns (0x192a216); head-look writes off the pilot seat, the mouse'"'"'s pitch is zeroed there'
    fpv1='[  999000.000] inject  foot: PATCHED — 0x1929f70'"'"'s walking branch (jne at 0x192a2f3) now composes the camera'"'"'s own rotation (0x192a284) instead of returning (0x192a216); head-look writes off the pilot seat, the mouse'"'"'s pitch is zeroed there'
    fpcont='[  999010.040] inject  headlook: out of the pilot seat — head-look continues on foot (X4VR_HEADLOOK_FOOT): the head looks, the mouse and arrows turn the body, the mouse'"'"'s pitch is zeroed'
    { cat /tmp/census-fp-base.log; echo "$fpok"; echo "$fpcont"; "$FOOT" --emit patched; } > /tmp/census-fp-ok.log
    want "foot patched -> the patch is reported" /tmp/census-fp-ok.log "foot " "patch IN (the NULL-controller exit, through the stub): walking composes"
    { cat /tmp/census-fp-base.log; echo "$fpv1"; echo "$fpcont"; "$FOOT" --emit patched; } > /tmp/census-fp-v1.log
    want "foot take-336 patch line -> named the retired cockpit branch" /tmp/census-fp-v1.log "foot " "take 336's retarget of the COCKPIT's branch — retired"
    want "foot patched -> the head turns X4's camera on foot" /tmp/census-fp-ok.log "foot =>" "the mouse turns rot yaw, the body's yaw (rot - local); the head turns rot yaw, rot pitch, local yaw, local pitch"
    want "foot patched -> the body read as rot - local follows the mouse" /tmp/census-fp-ok.log "foot " "body yaw FOLLOWS mouse dx"
    want "foot patched -> the mouse's pitch zeroing is counted" /tmp/census-fp-ok.log "foot " "the mouse's pitch zeroed in 96 motion event(s) off the seat"
    want "foot patched -> continued on foot counted" /tmp/census-fp-ok.log "foot " "continued on foot 1 time(s)"
    if python3 tools/score_run.py /tmp/census-fp-ok.log 2>&1 | grep -q -E "^FAIL  (foot|X4VR_HEADLOOK_FOOT)"; then
        bad "foot patched healthy -> a foot FAIL"
    else
        ok "foot patched healthy -> no foot FAIL"
    fi
    { cat /tmp/census-fp-base.log; "$FOOT" --emit patched; } > /tmp/census-fp-nopatch.log
    want "foot asked, never patched -> FAIL" /tmp/census-fp-nopatch.log "FAIL|  - " "X4VR_HEADLOOK_FOOT asked but 0x1929f70 was never patched"
    { cat /tmp/census-fp-base.log; echo "$fpok"; "$FOOT" --emit patched; } > /tmp/census-fp-nocont.log
    want "foot patched, off seat, never continued -> FAIL" /tmp/census-fp-nocont.log "FAIL|  - " "never said it continues on foot"
    { cat /tmp/census-ft-base.log; "$FOOT" --emit seated; } > /tmp/census-ft-seated.log
    want "foot seated only -> NOT EXERCISED" /tmp/census-ft-seated.log "foot =>" "NOT EXERCISED: no window off the pilot seat"
    want "foot asked, never started -> FAIL" /tmp/census-ft-base.log "FAIL|  - " "X4VR_FOOTLOG asked but the census never started"
    absent "foot not asked, no census -> silent" "$BASE" "foot "
fi

# ------------------------------------------ foot-menu: on-foot menus (take 338)
# The interact menu and NPC dialogs on foot hit-test in X4's UI; take 338 drew
# the cursor raw over them. The lines are written by foot_menu_tick and
# cursor_canvas; this guard fails the case set if either format moved.
# Each fragment on its own: grep -F with a multi-line pattern matches ANY line.
fmfmt=1
for frag in '"ui: on-foot menu %llu OPEN — X4 freed the pointer%s; the "' \
            'fm.resumed ? " (still free after a HUD-down menu)" : "",' \
            '"ui: on-foot menu %llu — pointer recentred from "' \
            '"%s%.0f,%.0f to %.0f,%.0f (%s)",' \
            '? "a conversation: where its options are"' \
            '"cursor is drawn %s | head-look yaw %.1f pitch %.1f | "' \
            '"pointer %.0f,%.0f",' \
            '"through the UI canvas (arm A)"' \
            ': "at the raw pointer (arm B, take 338'"'"'s)",' \
            '"ui: on-foot menu %llu closed — the cursor is drawn at "'; do
    grep -qF -- "$frag" injector/x4vr_inject.cpp || { fmfmt=0; echo "        missing: $frag"; }
done
grep -qF -- '? "ON (an on-foot menu has the pointer: the cursor "' layer/x4vr_layer.cpp ||
    { fmfmt=0; echo "        missing: the layer's on-foot ON"; }
if [ $fmfmt -eq 1 ]; then
    ok "foot-menu: the planted lines match the injector's and the layer's formats"
else
    bad "foot-menu: the injector's or the layer's format changed — replant the lines below"
fi
sed 's/env: run = /env: run = X4VR_CURSOR_CANVAS=auto X4VR_CURSOR_FOOT_AB=1 X4VR_HEADLOOK_FOOT=1 /' "$BASE" > /tmp/census-fm-base.log
fmpat='[  999970.000] inject  foot: PATCHED — 0x1929f70'"'"'s walking exit (je at 0x1929ffd, the camera with no controller) now goes through a stub at 0x3e0000'
fmout='[  999971.000] inject  headlook: out of the pilot seat — head-look continues on foot (X4VR_HEADLOOK_FOOT): the head looks, the mouse and arrows turn the body, the mouse'"'"'s pitch is zeroed'
fmoA='[  999975.000] inject  ui: on-foot menu 1 OPEN — X4 freed the pointer; the cursor is drawn through the UI canvas (arm A) | head-look yaw -49.2 pitch -20.1 | pointer 2255,1621'
fmlA='[  999975.020] layer   cursor canvas: AUTO — ON (an on-foot menu has the pointer: the cursor follows X4'"'"'s UI)'
fmcA='[  999981.000] inject  ui: on-foot menu 1 closed — the cursor is drawn at the raw pointer'
fmoff='[  999981.020] layer   cursor canvas: AUTO — OFF (no full-screen menu: the cursor at the raw pointer, where X4 picks the world)'
fmoB='[  999990.000] inject  ui: on-foot menu 2 OPEN — X4 freed the pointer; the cursor is drawn at the raw pointer (arm B, take 338'"'"'s) | head-look yaw 7.3 pitch -20.4 | pointer 2276,1698'
fmcB='[  999998.000] inject  ui: on-foot menu 2 closed — the cursor is drawn at the raw pointer'
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" "$fmout" "$fmoA" "$fmlA" "$fmcA" "$fmoff" "$fmoB" "$fmcB"; } > /tmp/census-fm-ab.log
want "foot-menu A/B -> arm A opening reached the layer" /tmp/census-fm-ab.log foot-menu "#1   t   999975.0  arm A    6.0 s  head-look  -49.2  -20.1  pointer   2255,  1621  canvas ON"
want "foot-menu A/B -> arm B opening drawn raw" /tmp/census-fm-ab.log foot-menu "#2   t   999990.0  arm B    8.0 s  head-look   +7.3  -20.4  pointer   2276,  1698"
want "foot-menu A/B -> both arms counted" /tmp/census-fm-ab.log foot-menu "2 opening(s): arm A (canvas) 1, arm B (raw) 1 — alternating, X4VR_CURSOR_FOOT_AB"
want "foot-menu A/B -> a foot ON is not a full-screen switch" /tmp/census-fm-ab.log cursor-auto "the layer switched the cursor to the canvas 0 time(s)"
if python3 tools/score_run.py /tmp/census-fm-ab.log 2>&1 | grep -q "canvas-arm opening"; then
    bad "foot-menu A/B healthy -> FAILs"
else
    ok "foot-menu A/B healthy -> no FAIL"
fi
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" "$fmout" "$fmoA" "$fmcA"; } > /tmp/census-fm-nolayer.log
want "foot-menu arm A never reached the layer -> FAIL" /tmp/census-fm-nolayer.log "FAIL|  - " "1 canvas-arm opening(s) never reached the layer"
want "foot-menu one arm only -> said" /tmp/census-fm-nolayer.log foot-menu "ONLY ONE ARM opened"
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" "$fmout" '[  999975.000] inject  sdl: relative mouse mode ON (fold stands down)' '[  999976.000] inject  sdl: relative mouse mode off (fold applies)'; } > /tmp/census-fm-old.log
want "foot-menu pointer freed, nothing logged -> an older build" /tmp/census-fm-old.log foot-menu "X4 freed the pointer 1 time(s) off the seat and no on-foot menu was logged"
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" '[  999969.000] inject  sdl: relative mouse mode off (fold applies)' "$fmout"; } > /tmp/census-fm-none.log
want "foot-menu pointer freed only in the seat -> not exercised" /tmp/census-fm-none.log foot-menu "no on-foot menu opened"
absent "foot-menu: no canvas auto, no foot patch -> silent" "$BASE" foot-menu

# ------------------------------------------ hud-menu: HUD-down menus off the seat
# Take 339's station teleporters: head-look centred the camera, X4 called no
# menu full-screen, and the cursor stayed raw. Same guard on the formats.
hmfmt=1
for frag in '"ui: menu with the HUD down %llu OPEN off "' \
            '"the pilot seat — the cursor is drawn %s",' \
            '"ui: menu with the HUD down %llu closed — the cursor is "' \
            '? "through the UI canvas (arm A)"' \
            ': "at the raw pointer (arm B, take 339'"'"'s)");' \
            'seated ? "in the cockpit" : "off the pilot seat",'; do
    grep -qF -- "$frag" injector/x4vr_inject.cpp || { hmfmt=0; echo "        missing: $frag"; }
done
grep -qF -- '? "ON (a menu with the HUD down, X4'"'"'s camera centred: "' layer/x4vr_layer.cpp ||
    { hmfmt=0; echo "        missing: the layer's HUD-down ON"; }
if [ $hmfmt -eq 1 ]; then
    ok "hud-menu: the planted lines match the injector's and the layer's formats"
else
    bad "hud-menu: the injector's or the layer's format changed — replant the lines below"
fi
sed 's/env: run = /env: run = X4VR_CURSOR_CANVAS=auto X4VR_CURSOR_CENTRED_AB=1 X4VR_HEADLOOK_FOOT=1 X4VR_HEADLOOK_WRITE=1 /' "$BASE" > /tmp/census-hm-base.log
hmcen='[  999975.000] inject  headlook: HUD down off the pilot seat — X4'"'"'s camera CENTRED from yaw 77.17 pitch -23.32 to 0,0, so X4'"'"'s menus fill the window the pointer is confined to'
hmoA='[  999975.001] inject  ui: menu with the HUD down 1 OPEN off the pilot seat — the cursor is drawn through the UI canvas (arm A)'
hmlA='[  999975.020] layer   cursor canvas: AUTO — ON (a menu with the HUD down, X4'"'"'s camera centred: the cursor follows X4'"'"'s UI)'
hmcA='[  999982.000] inject  ui: menu with the HUD down 1 closed — the cursor is drawn at the raw pointer'
hmoB='[  999990.001] inject  ui: menu with the HUD down 2 OPEN off the pilot seat — the cursor is drawn at the raw pointer (arm B, take 339'"'"'s)'
hmcB='[  999994.000] inject  ui: menu with the HUD down 2 closed — the cursor is drawn at the raw pointer'
hmarm=$(printf '%s' "$mcarm" | sed 's/999980\.000/999970.500/')
hmdown='[  999974.990] inject  headlook: HUD is now down (IsInPanelMode=0) — idle, mouse handed back'
hmup='[  999982.000] inject  headlook: HUD is now up (IsInPanelMode=0) — steering'
{ cat /tmp/census-hm-base.log; printf '%s\n' "$hmarm" "$fmout" "$hmdown" "$hmcen" "$hmoA" "$hmlA" "$(mcpose 999976.000 0.0000 0.0000)" "$hmcA" "$hmup" "$fmoff" "$hmoB" "$hmcB"; } > /tmp/census-hm-ab.log
want "hud-menu A/B -> arm A reached the layer" /tmp/census-hm-ab.log hud-menu "#1   t   999975.0  arm A    7.0 s  canvas ON"
want "hud-menu A/B -> arm B drawn raw" /tmp/census-hm-ab.log hud-menu "#2   t   999990.0  arm B    4.0 s"
want "hud-menu A/B -> both arms counted" /tmp/census-hm-ab.log hud-menu "2 period(s) off the seat: arm A (canvas) 1, arm B (raw) 1 — alternating"
want "hud-menu -> a HUD-down ON is not a full-screen switch" /tmp/census-hm-ab.log cursor-auto "the layer switched the cursor to the canvas 0 time(s)"
want "hud-menu -> the off-seat centring line counts for menucam" /tmp/census-hm-ab.log menucam "1 period(s) centred by X4VR_HEADLOOK_MENU_CENTRE, from +77.17,-23.32"
if python3 tools/score_run.py /tmp/census-hm-ab.log 2>&1 | grep -q "canvas-arm period"; then
    bad "hud-menu A/B healthy -> FAILs"
else
    ok "hud-menu A/B healthy -> no FAIL"
fi
{ cat /tmp/census-hm-base.log; printf '%s\n' "$mcarm" "$fmout" "$hmcen" "$hmoA" "$hmcA"; } > /tmp/census-hm-nolayer.log
want "hud-menu arm A never reached the layer -> FAIL" /tmp/census-hm-nolayer.log "FAIL|  - " "1 canvas-arm period(s) never reached the layer"
want "hud-menu one arm only -> said" /tmp/census-hm-nolayer.log hud-menu "ONLY ONE ARM"
# ------------------------------------------ menus: X4's open-menu table (take 342)
mnfmt=1
for frag in '"menus: IsConversationActive and the open-menu lookup "' \
            'X4VR_LOG("ui: X4'"'"'s open menus: %s%s", line,' \
            '"menus: REFUSED — X4'"'"'s code at %p differs from the "'; do
    grep -qF -- "$frag" injector/x4vr_inject.cpp || { mnfmt=0; echo "        missing: $frag"; }
done
if [ $mnfmt -eq 1 ]; then ok "menus: the planted lines match the injector's formats"
else bad "menus: the injector's format changed — replant the lines below"; fi
mnok='[  999970.000] inject  menus: IsConversationActive and the open-menu lookup match X4 9.00 — the table'"'"'s root at 0x3dfc208 and the conversation at 0x3db8300 are READ, never called'
mnseat='[  999970.500] inject  headlook: in the pilot seat — the write may act'
mn1='[  999971.000] inject  ui: X4'"'"'s open menus: none'
mn2='[  999972.000] inject  ui: X4'"'"'s open menus: InteractMenu'
mn3='[  999975.000] inject  ui: X4'"'"'s open menus: MapMenu[full]'
{ cat "$BASE"; printf '%s\n' "$mnok" "$mnseat" "$mn1" "$mn2" "$mn3"; } > /tmp/census-mn-yes.log
want "menus -> changes listed with the seat" /tmp/census-mn-yes.log menus "t   999972.0  seat  InteractMenu"
want "menus -> names seen" /tmp/census-mn-yes.log "menus   names" "InteractMenu (seat), MapMenu (seat)"
want "menus -> InteractMenu found" /tmp/census-mn-yes.log "menus   =>" "InteractMenu IS in X4's table"
{ cat "$BASE"; printf '%s\n' "$mnok" "$mnseat" "$mn1" "$mn3"; } > /tmp/census-mn-no.log
want "menus -> InteractMenu absent said" /tmp/census-mn-no.log "menus   =>" "InteractMenu never appeared"
absent "menus: not read -> silent" "$BASE" "menus "
# X4VR_DUMP_SHADERS: take 346 asked, wrote nothing, and PASSed.
sed 's/env: run = /env: run = X4VR_DUMP_SHADERS=\/tmp\/x4vr-shaders /' "$BASE" > /tmp/census-sd-base.log
{ cat /tmp/census-sd-base.log; echo '[  999970.000] layer   WARNING: X4VR_DUMP_SHADERS=/tmp/x4vr-shaders is not writable'; } > /tmp/census-sd-none.log
want "shader dumps asked, none written -> FAIL" /tmp/census-sd-none.log "FAIL|  - " "X4VR_DUMP_SHADERS=/tmp/x4vr-shaders asked but no module was written (the layer said it is not writable)"
{ cat /tmp/census-sd-base.log; echo '[  999970.000] layer   modules: dumping X4'"'"'s shader modules to /tmp/x4vr-shaders/mod-NNNN.spv'; } > /tmp/census-sd-ok.log
want "shader dumps written -> said" /tmp/census-sd-ok.log shaders "shaders dumped to /tmp/x4vr-shaders/mod-NNNN.spv"
if python3 tools/score_run.py /tmp/census-sd-ok.log 2>&1 | grep -q "asked but no module"; then
    bad "shader dumps written -> FAILs"
else
    ok "shader dumps written -> no FAIL"
fi
absent "shader dumps not asked -> silent" "$BASE" shaders
# The fog ray march kept on X4's own inverses (take 349).
if grep -qF '"invproj %s: fog ray march — %llu module(s) kept on X4'"'"'s "' layer/x4vr_layer.cpp; then
    ok "fog: the planted line matches the layer's format"
else
    bad "fog: the layer's format changed — replant the line below"
fi
{ cat "$BASE"; echo '[  999970.000] layer   invproj final: fog ray march — 1 module(s) kept on X4'"'"'s own inverse projections'; } > /tmp/census-fog.log
want "fog: the kept module counted" /tmp/census-fog.log fog "1 module(s) kept on X4's own inverse projections"
absent "fog: none -> silent" "$BASE" "fog "
# ------------------------------------------ take 343's two false FAILs
# The map check: X4 resumes walking mouselook a few ms before the HUD comes
# back (the hand-back) -- not take 147's mouselook in the middle of the map.
mpkey='[  999970.000] inject  key[peep] type=0x300 scancode=16 keycode=0x6d mod=0x0 which=0 window=2 down=1 repeat=0 raw=58 ts=1 action=2'
mpdn='[  999971.000] inject  headlook: HUD is now down (IsInPanelMode=0) — idle, mouse handed back'
mpup='[  999975.000] inject  headlook: HUD is now up (IsInPanelMode=0) — steering'
{ cat "$BASE"; printf '%s\n' "$mpkey" "$mpdn" '[  999974.989] inject  sdl: relative mouse mode ON (fold stands down)' "$mpup"; } > /tmp/census-map-handback.log
want "map: mouselook 11 ms before the HUD returns is the hand-back" /tmp/census-map-handback.log "map   " "1 HUD-down interval(s), 1 of them free of mouselook"
{ cat "$BASE"; printf '%s\n' "$mpkey" "$mpdn" '[  999973.000] inject  sdl: relative mouse mode ON (fold stands down)' "$mpup"; } > /tmp/census-map-relapse.log
want "map: mouselook in the middle of the menu still FAILs" /tmp/census-map-relapse.log "FAIL|  - " "X4 re-entered mouselook inside 1 HUD-down"
# Take 353: off the pilot seat relative mode is X4's walking -- a teleporter
# closed, X4 resumed walking 0.5 s before landing him in the seat.
mpout='[  999969.000] inject  headlook: out of the pilot seat — head-look continues on foot (X4VR_HEADLOOK_FOOT): the head looks'
mpin='[  999974.900] inject  headlook: in the pilot seat — the write may act'
{ cat "$BASE"; printf '%s\n' "$mpout" "$mpkey" "$mpdn" '[  999974.400] inject  sdl: relative mouse mode ON (fold stands down)' "$mpin" "$mpup"; } > /tmp/census-map-walk.log
want "map: walking resumed off the seat -> set aside" /tmp/census-map-walk.log "map   " "1 relative-mode entry off the pilot seat set aside (X4's walking)"
if python3 tools/score_run.py /tmp/census-map-walk.log 2>&1 | grep -q "re-entered mouselook"; then
    bad "map: walking resumed off the seat -> FAILs (take 353's false FAIL)"
else
    ok "map: walking resumed off the seat -> no FAIL"
fi
mpin0='[  999969.500] inject  headlook: in the pilot seat — the write may act'
{ cat "$BASE"; printf '%s\n' "$mpout" "$mpin0" "$mpkey" "$mpdn" '[  999973.000] inject  sdl: relative mouse mode ON (fold stands down)' "$mpup"; } > /tmp/census-map-seated.log
want "map: back in the seat, mouselook mid-menu still FAILs" /tmp/census-map-seated.log "FAIL|  - " "X4 re-entered mouselook inside 1 HUD-down"
if grep -qF '"headlook: %s",' injector/x4vr_inject.cpp && grep -qF '"in the pilot seat — the write may act"' injector/x4vr_inject.cpp && grep -qF '"out of the pilot seat — head-look continues on "' injector/x4vr_inject.cpp; then
    ok "map: the planted seat lines match the injector's format"
else
    bad "map: the injector's seat lines moved"
fi
# menucam: X4 easing the seat's camera to 0 after he sits down is centred at
# the end; a camera left at the head's angle is still off there.
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mccen" "$(mcpose 999990.300 -2.3650 23.1370)" "$(mcpose 999990.500 -1.0620 10.7140)" "$(mcpose 999990.700 -0.5290 5.3140)" "$(mcpose 999990.900 -0.0730 0.7310)" "$(mcpose 999991.000 -0.0160 0.1630)" "$(mcpose 999994.700 0.0000 0.0000)" "$(mcpose 999994.900 0.0000 0.0000)" "$mcup" "$mcrep"; } > /tmp/census-mc-ease.log
# Take 349: centred in the map, then teleported out of it -- on foot 1.2 s
# before the HUD came back, X4's camera reading his body at -90 deg.
mcsout='[  999993.800] inject  headlook: out of the pilot seat — head-look continues on foot (X4VR_HEADLOOK_FOOT): the head looks, the mouse and arrows turn the body, the mouse'"'"'s pitch is zeroed'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mccen" "$(mcpose 999990.500 0.0000 0.0000)" "$(mcpose 999993.000 0.0000 0.0000)" "$(mcpose 999993.500 0.0000 0.0000)" "$mcsout" "$(mcpose 999994.200 -90.0000 0.0000)" "$(mcpose 999994.800 -90.0000 0.0000)" "$mcup" "$mcrep"; } > /tmp/census-mc-teleport.log
want "menucam: a seat change inside the period ends what is judged" /tmp/census-mc-teleport.log menucam "CENTRED      0.0% of X4's UI out of reach"
if python3 tools/score_run.py /tmp/census-mc-teleport.log 2>&1 | grep -q "the centring did not act"; then
    bad "menucam: the on-foot tail of a cockpit period FAILs (take 349's false FAIL)"
else
    ok "menucam: the on-foot tail of a cockpit period does not FAIL"
fi
# ...and a camera still frozen BEFORE the seat change still FAILs.
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$(mcpose 999990.500 -1.9851 6.3677)" "$(mcpose 999993.500 -1.9851 6.3677)" "$mcsout" "$(mcpose 999994.800 0.0000 0.0000)" "$mcup" "$mcrep"; } > /tmp/census-mc-frozen-then-out.log
want "menucam: frozen before the seat change still FAILs" /tmp/census-mc-frozen-then-out.log FAIL "the centring did not act"
want "menucam: an easing camera that reaches 0 is CENTRED" /tmp/census-mc-ease.log menucam "CENTRED      0.0% of X4's UI out of reach"
if python3 tools/score_run.py /tmp/census-mc-ease.log 2>&1 | grep -q "the centring did not act"; then
    bad "menucam: an easing camera FAILs (take 343's false FAIL)"
else
    ok "menucam: an easing camera does not FAIL"
fi
# ------------------------------------------ rclick: X4's right-click menu (take 343)
rcfmt=1
for frag in 'X4VR_LOG("ui: X4'"'"'s right-click menu %s",' \
            'interact ? "OPEN — the cursor is drawn through the UI "' \
            '"canvas"'; do
    grep -qF -- "$frag" injector/x4vr_inject.cpp || { rcfmt=0; echo "        missing: $frag"; }
done
grep -qF -- '? "ON (X4'"'"'s right-click menu is open: the cursor "' layer/x4vr_layer.cpp ||
    { rcfmt=0; echo "        missing: the layer's right-click ON"; }
if [ $rcfmt -eq 1 ]; then ok "rclick: the planted lines match the injector's and the layer's formats"
else bad "rclick: a format changed — replant the lines below"; fi
rco='[  999972.000] inject  ui: X4'"'"'s right-click menu OPEN — the cursor is drawn through the UI canvas'
rcl='[  999972.040] layer   cursor canvas: AUTO — ON (X4'"'"'s right-click menu is open: the cursor follows X4'"'"'s UI)'
rcc='[  999980.000] inject  ui: X4'"'"'s right-click menu closed'
rcoff='[  999980.020] layer   cursor canvas: AUTO — OFF (no full-screen menu: the cursor at the raw pointer, where X4 picks the world)'
{ cat "$BASE"; printf '%s\n' "$rco" "$rcl" "$rcc" "$rcoff"; } > /tmp/census-rc-ok.log
want "rclick -> period with the canvas" /tmp/census-rc-ok.log rclick "t   999972.0    8.0 s  canvas ON (X4's right-click menu)"
want "rclick -> counted" /tmp/census-rc-ok.log rclick "1 right-click menu period(s)"
want "rclick -> not a full-screen switch" /tmp/census-rc-ok.log cursor-auto "the layer switched the cursor to the canvas 0 time(s)"
if python3 tools/score_run.py /tmp/census-rc-ok.log 2>&1 | grep -q "right-click menu: "; then
    bad "rclick healthy -> FAILs"
else
    ok "rclick healthy -> no FAIL"
fi
{ cat "$BASE"; printf '%s\n' "$rco" "$rcc"; } > /tmp/census-rc-late.log
want "rclick without the canvas -> FAIL" /tmp/census-rc-late.log "FAIL|  - " "1 period(s) without the canvas 0.2 s after X4's table showed it"
absent "rclick: none -> silent" "$BASE" rclick
absent "hud-menu: nothing off the seat -> silent" "$BASE" hud-menu
# Take 340's map and crafting bench: full-screen, so the layer's canvas was
# already ON (no HUD-down ON line). That FAILed them; it must not.
hmfs='[  999975.000] layer   cursor canvas: AUTO — ON (a full-screen menu is up: the cursor follows X4'"'"'s UI)'
{ cat /tmp/census-hm-base.log; printf '%s\n' "$hmarm" "$fmout" "$hmfs" "$hmdown" "$hmcen" "$hmoA" "$hmcA" "$hmup"; } > /tmp/census-hm-full.log
want "hud-menu full-screen under A -> canvas held, either arm" /tmp/census-hm-full.log hud-menu "#1   t   999975.0  arm A    7.0 s  canvas ON (full-screen: either arm)"
want "hud-menu full-screen -> said it tests neither arm" /tmp/census-hm-full.log hud-menu "1 of them were full-screen"
if python3 tools/score_run.py /tmp/census-hm-full.log 2>&1 | grep -q "canvas-arm period"; then
    bad "hud-menu full-screen under A -> FAILs (take 340's false FAIL)"
else
    ok "hud-menu full-screen under A -> no FAIL"
fi
# Take 340's vendor: back from the purchase list with the pointer free.
fmres='[  999995.000] inject  ui: on-foot menu 3 OPEN — X4 freed the pointer (still free after a HUD-down menu); the cursor is drawn through the UI canvas (arm A) | head-look yaw 1.0 pitch -5.0 | pointer 1400,1400'
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" "$fmout" "$fmoA" "$fmlA" "$fmcA" "$fmoff" "$fmres" '[  999995.020] layer   cursor canvas: AUTO — ON (an on-foot menu has the pointer: the cursor follows X4'"'"'s UI)'; } > /tmp/census-fm-res.log
fmrec='[  999975.030] inject  ui: on-foot menu 1 — pointer recentred from 2255,1621 to 1408,1408'
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" "$fmout" "$fmoA" "$fmrec" "$fmlA" "$fmcA" "$fmoff" "$fmoB" "$fmcB"; } > /tmp/census-fm-rec.log
want "foot-menu recentred opening -> marked" /tmp/census-fm-rec.log foot-menu "pointer   2255,  1621  canvas ON  recentred"
want "foot-menu recentres counted against fresh openings" /tmp/census-fm-rec.log foot-menu "pointer recentred at 1 of 2 fresh opening(s)"
fmrec2='[  999990.030] inject  ui: on-foot menu 2 — pointer recentred from 2276,1698 to 1408,2070 (a conversation: where its options are)'
{ cat /tmp/census-fm-base.log; printf '%s\n' "$fmpat" "$fmout" "$fmoA" "$fmrec" "$fmlA" "$fmcA" "$fmoff" "$fmoB" "$fmrec2" "$fmcB"; } > /tmp/census-fm-rec2.log
want "foot-menu recentre kinds counted" /tmp/census-fm-rec2.log foot-menu "to a conversation's options 1, the centre 0 (no conversation) + 0 (unknown)"
want "foot-menu resumed opening -> listed and marked" /tmp/census-fm-res.log foot-menu "#3   t   999995.0  arm A    open at exit  head-look   +1.0   -5.0  pointer   1400,  1400  canvas ON  (resumed after a HUD-down menu)"

# ------------------------------------ X4VR_HEADLOOK_EXTERNAL (extview)
# The lines the injector and layer write, planted; every branch of the block.
for f in 'extview: PATCHED — 0x1929f70'"'"'s external-view compose (jmp at ' \
         'extview: period %llu ON — external view, camera mode %d: ' \
         'extview: period %llu — the stub composed the head after ' \
         'extview: period %llu OFF — %s; the stub composed the head ' \
         'extview: period %llu — the stub has NOT composed in %.1f s ' \
         'extview: period %llu — camera mode %d -> %d' \
         'extview: patch REFUSED — %s. The external views stay as '; do
    if grep -qF "$f" injector/x4vr_inject.cpp; then
        ok "extview: the planted line matches the injector's format ($f)"
    else
        bad "extview: the injector no longer writes '$f'"
    fi
done
if grep -qF '"EXTERNAL VIEW — declaring head-look'"'"'s "' layer/x4vr_layer.cpp &&
   grep -qF '"ext=%u",' layer/x4vr_layer.cpp; then
    ok "extview: the layer's declrot and poselag ext= formats are the planted ones"
else
    bad "extview: the layer's declrot or poselag ext= format moved"
fi
sed 's/env: run = /env: run = X4VR_HEADLOOK_EXTERNAL=1 /' "$BASE" > /tmp/census-ev-base.log
evpat='[  999960.000] inject  extview: PATCHED — 0x1929f70'"'"'s external-view compose (jmp at 0x192a295, camera modes 2-5) now goes through a 269-byte stub at 0x3e0000: for the player'"'"'s camera'
evref='[  999960.000] inject  extview: patch REFUSED — 0x1929f70'"'"'s code at 0x192a1bf differs from X4 9.00. The external views stay as they were: a window fixed in front.'
evview='[  999961.000] inject  headlook: view is now external — idle'
evon='[  999961.010] inject  extview: period 1 ON — external view, camera mode 2: the head'"'"'s rows published for the stub; the layer declares the head once the stub has composed them'
evdecl='[  999961.030] inject  extview: period 1 — the stub composed the head after 0.02 s: the layer declares head-look'"'"'s angle, the external view is the whole scene'
evlay='[  999961.040] layer   declrot: EXTERNAL VIEW — declaring head-look'"'"'s angle, the stub'"'"'s composition (X4'"'"'s camera there includes its orbit)'
evmode='[  999965.000] inject  extview: period 1 — camera mode 2 -> 4'
evoff='[  999970.000] inject  extview: period 1 OFF — the external view was left; the stub composed the head 812 time(s) in 9.0 s'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon" "$evdecl" "$evlay" "$evmode" "$evoff"; } > /tmp/census-ev-ok.log
want "extview: a period composed and declared -> listed" /tmp/census-ev-ok.log extview "#1   t   999961.0  mode 2->4    9.0 s  composed    812x  declared after 0.02 s  ended: the external view was left"
want "extview: the totals" /tmp/census-ev-ok.log extview "1 period(s) over 1 external-view entry; 1 declared; the layer switched 1 time(s)"
if python3 tools/score_run.py /tmp/census-ev-ok.log 2>&1 | grep -qE "^  - .*(external|stub|X4VR_HEADLOOK_EXTERNAL)"; then
    bad "extview: a healthy period -> FAILs"
else
    ok "extview: a healthy period -> no FAIL"
fi
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evref" "$evview"; } > /tmp/census-ev-refused.log
want "extview: asked, refused -> FAIL with the reason" /tmp/census-ev-refused.log "FAIL|  - " "X4VR_HEADLOOK_EXTERNAL=1 but the patch was REFUSED — 0x1929f70's code at 0x192a1bf differs from X4 9.00"
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview"; } > /tmp/census-ev-noperiod.log
want "extview: an external view with no period -> FAIL" /tmp/census-ev-noperiod.log "FAIL|  - " "1 external view(s) shown and the head turned none of them"
evon6='[  999961.010] inject  extview: period 1 ON — external view, camera mode 6: the head'"'"'s rows published for the stub; the layer declares the head once the stub has composed them'
evnever='[  999963.100] inject  extview: period 1 — the stub has NOT composed in 2.1 s (camera mode 6): X4 evaluates this camera by a path the patch does not cover; the picture stays a window in front'
evoff0='[  999966.000] inject  extview: period 1 OFF — the external view was left; the stub composed the head 0 time(s) in 5.0 s'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon6" "$evnever" "$evoff0"; } > /tmp/census-ev-never.log
want "extview: never composed -> FAIL naming the mode" /tmp/census-ev-never.log "FAIL|  - " "external view 1 (camera mode 6): the stub never composed the head"
want "extview: never composed -> listed as never declared" /tmp/census-ev-never.log extview "NEVER declared"
evoffq='[  999961.200] inject  extview: period 1 OFF — the external view was left; the stub composed the head 0 time(s) in 0.2 s'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon" "$evoffq"; } > /tmp/census-ev-brief.log
if python3 tools/score_run.py /tmp/census-ev-brief.log 2>&1 | grep -q "the stub never composed"; then
    bad "extview: a 0.2 s period (F2 tapped through) -> FAILs"
else
    ok "extview: a 0.2 s period never composed -> no FAIL"
fi
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon" "$evdecl" "$evoff"; } > /tmp/census-ev-nolayer.log
want "extview: composed but the layer never declared -> FAIL" /tmp/census-ev-nolayer.log "FAIL|  - " "the layer never declared it"
{ cat "$BASE"; printf '%s\n' "$evpat" "$evview" "$evon"; } > /tmp/census-ev-unasked.log
want "extview: acted without being asked -> FAIL" /tmp/census-ev-unasked.log "FAIL|  - " "the external-view patch acted in a run that did not ask for it"
absent "extview: not asked, nothing -> silent" "$BASE" extview
# Take 354 on: the OFF line says which path the player's camera took.
evoffp='[  999970.000] inject  extview: period 1 OFF — the external view was left; the stub composed the head 812 time(s) in 9.0 s | the player'"'"'s camera reached the orbit compose 0 time(s), X4'"'"'s early return 812 time(s)'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon" "$evdecl" "$evlay" "$evoffp"; } > /tmp/census-ev-path.log
want "extview: the path the camera took -> listed" /tmp/census-ev-path.log extview "| path: orbit compose 0, early return 812"
evoffn='[  999966.000] inject  extview: period 1 OFF — the external view was left; the stub composed the head 0 time(s) in 5.0 s | the player'"'"'s camera reached the orbit compose 0 time(s), X4'"'"'s early return 0 time(s)'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon6" "$evnever" "$evoffn"; } > /tmp/census-ev-neither.log
want "extview: neither site reached -> FAIL says so" /tmp/census-ev-neither.log "FAIL|  - " "the stub never composed the head — X4 took a path the patch does not cover"
if grep -qF '"%llu time(s) in %.1f s | the player'"'"'s camera reached the "' injector/x4vr_inject.cpp; then
    ok "extview: the planted path counts match the injector's format"
else
    bad "extview: the injector's path-count format moved"
fi

# Take 355: F6 from F4 -- the head declared on with the HUD down.
evhd='[  999963.000] inject  headlook: HUD is now down (IsInPanelMode=0) — idle, mouse handed back'
evhu='[  999969.000] inject  headlook: HUD is now up (IsInPanelMode=0) — steering'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon" "$evdecl" "$evlay" "$evhd" "$evhu" "$evoff"; } > /tmp/census-ev-hud.log
want "extview: declared on with the HUD down -> FAIL" /tmp/census-ev-hud.log "FAIL|  - " "external view 1: the head was declared for 6.0 s with the HUD down"
evoffh='[  999963.010] inject  extview: period 1 OFF — the HUD is down (the cinematic view F6, or a menu): a window in front; the stub composed the head 100 time(s) in 2.0 s | the player'"'"'s camera reached the orbit compose 0 time(s), X4'"'"'s early return 100 time(s)'
{ cat /tmp/census-ev-base.log; printf '%s\n' "$evpat" "$evview" "$evon" "$evdecl" "$evlay" "$evhd" "$evoffh" "$evhu"; } > /tmp/census-ev-hudok.log
if python3 tools/score_run.py /tmp/census-ev-hudok.log 2>&1 | grep -q "with the HUD down"; then
    bad "extview: the period ended at the HUD going down -> FAILs"
else
    ok "extview: the period ended at the HUD going down -> no FAIL"
fi
if grep -qF '"the HUD is down (the cinematic view F6, or a menu): a "' injector/x4vr_inject.cpp; then
    ok "extview: the injector ends a period when the HUD goes down"
else
    bad "extview: the injector's HUD-down reason moved"
fi

# Take 357: the cinematic view's overlay -- twins switched and BOUND.
if grep -qF '"ui: cinematic view %s",' injector/x4vr_inject.cpp && grep -qF '"ON — the HUD down with only X4' injector/x4vr_inject.cpp && grep -qF '"ui twin: first twin BOUND — the cinematic view' layer/x4vr_layer.cpp && grep -qF '"CINEMATIC — X4' layer/x4vr_layer.cpp; then
    ok "cinematic: the planted lines match the injector's and the layer's formats"
else
    bad "cinematic: a format moved"
fi
cvon='[  999970.000] inject  ui: cinematic view ON — the HUD down with only X4'"'"'s TopLevelMenu open in the pilot seat: X4'"'"'s UI drawn as X4 laid it out, so its overlay fills the window'
cvsw='[  999970.020] layer   ui twin: CINEMATIC — X4'"'"'s UI pipelines bound as X4 built them, the overlay at the full window (12 twin(s) built, 0 failed, 0 twin bind(s) so far)'
cvbd='[  999970.021] layer   ui twin: first twin BOUND — the cinematic view'"'"'s UI drawn by X4'"'"'s own vertex stage'
{ cat "$BASE"; printf '%s\n' "$cvon" "$cvsw" "$cvbd"; } > /tmp/census-cv-ok.log
want "cinematic: switched and bound -> said" /tmp/census-cv-ok.log cinematic "1 cinematic-view period(s); the layer switched to the twins 1 time(s); twins BOUND"
if python3 tools/score_run.py /tmp/census-cv-ok.log 2>&1 | grep -qE "^  - .*(cinematic|twin)"; then
    bad "cinematic: switched and bound -> FAILs"
else
    ok "cinematic: switched and bound -> no FAIL"
fi
{ cat "$BASE"; printf '%s\n' "$cvon" "$cvsw"; } > /tmp/census-cv-unbound.log
want "cinematic: switched but never bound -> FAIL" /tmp/census-cv-unbound.log "FAIL|  - " "no twin pipeline was ever bound"
{ cat "$BASE"; printf '%s\n' "$cvon"; } > /tmp/census-cv-late.log
want "cinematic: never reached the layer -> FAIL" /tmp/census-cv-late.log "FAIL|  - " "the cinematic view: 1 period(s) never reached the layer"
absent "cinematic: none -> silent" "$BASE" cinematic

# ------------------------------------ X4VR_HEAD_6DOF (head6)
for f in '"head 6dof: neutral latched at (%.3f, %.3f, "' '"head 6dof: offset (%.3f, %.3f, %.3f) m in the "' '"head 6dof: REFUSED — it needs X4VR_POSE=1 and "' '"ON — the world drawn from the head' '"off — not steering in the cockpit (a menu, "'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "head6: the planted line matches a format ($f)"
    else
        bad "head6: a format moved ($f)"
    fi
done
sed 's/env: run = /env: run = X4VR_HEAD_6DOF=1 /' "$BASE" > /tmp/census-h6-base.log
h6l='[  999960.000] layer   head 6dof: neutral latched at (0.010, 1.100, -0.020) m, the first frame head-look steered in the cockpit; the world shaders move the eye from here, held to 0.35 m'
h6on='[  999960.010] layer   head 6dof: ON — the world drawn from the head'"'"'s position in the cockpit'
h6of='[  999962.000] layer   head 6dof: offset (0.060, -0.080, 0.000) m in the room -> view right 0.060 up -0.080 forward 0.000'
h6off='[  999970.000] layer   head 6dof: off — not steering in the cockpit (a menu, an external view, on foot): the eye at neutral'
{ cat /tmp/census-h6-base.log; printf '%s\n' "$h6l" "$h6on" "$h6of" "$h6off"; } > /tmp/census-h6-ok.log
want "head6: a healthy run -> summarised" /tmp/census-h6-ok.log head6 "neutral latched; 1 period(s) ON, 1 off; largest sampled offset 0.100 m"
if python3 tools/score_run.py /tmp/census-h6-ok.log 2>&1 | grep -qE "^  - .*6DOF"; then
    bad "head6: a healthy run -> FAILs"
else
    ok "head6: a healthy run -> no FAIL"
fi
{ cat /tmp/census-h6-base.log; printf '%s\n' "$h6l"; } > /tmp/census-h6-never.log
want "head6: latched but never ON -> FAIL" /tmp/census-h6-never.log "FAIL|  - " "the eye was never moved"
want "head6: never latched -> FAIL" /tmp/census-h6-base.log "FAIL|  - " "the layer never latched a neutral"
{ cat /tmp/census-h6-base.log; echo '[  999960.000] layer   head 6dof: REFUSED — it needs X4VR_POSE=1 and X4VR_ROLL_STEREO (the world shaders'"'"' head slot); pose OFF, roll stereo on. Nothing is moved or declared'; } > /tmp/census-h6-ref.log
want "head6: refused -> FAIL" /tmp/census-h6-ref.log "FAIL|  - " "X4VR_HEAD_6DOF REFUSED — it needs X4VR_POSE=1"
absent "head6: not asked -> silent" "$BASE" head6

# 6DOF option B's check: UI depths, per draw (take 364).
if grep -qF '"ui depth (6dof B, per draw): %llu perspective UI "' layer/x4vr_layer.cpp; then
    ok "ui-depth: the planted line matches the layer's format"
else
    bad "ui-depth: the layer's format moved"
fi
sed 's/env: run = /env: run = X4VR_DUMP_MATRICES=1 /' "$BASE" > /tmp/census-ud-base.log
udl='[  999960.000] layer   ui depth (6dof B, per draw): 90 perspective UI draw(s) by the view depth of their origin — <0.1 m 40 | 0.1-0.5 0 | 0.5-2 0 | 2-20 10 | 20-200 10 | >200 30; nearest 0.0310 m, farthest 5400.0 m; 300 orthographic, 0 unreadable; first depths under 2 m: 0.031 0.032'
{ cat /tmp/census-ud-base.log; printf '%s\n' "$udl" "$udl"; } > /tmp/census-ud-ok.log
want "ui-depth: samples summed" /tmp/census-ud-ok.log ui-depth "2 sample(s); perspective UI draws by depth <0.1 m 80 | 0.1-0.5 0 | 0.5-2 0 | 2-20 20 | 20-200 20 | >200 60; nearest 0.0310 m, farthest 5400.0 m; 600 orthographic, 0 unreadable"
want "ui-depth: near depths listed" /tmp/census-ud-ok.log ui-depth "depths under 2 m seen: 0.031 0.032 0.031 0.032"
want "ui-depth: asked, nothing -> FAIL" /tmp/census-ud-base.log "FAIL|  - " "X4VR_DUMP_MATRICES=1 but no UI depth was measured"
absent "ui-depth: not asked -> silent" "$BASE" ui-depth
# Take 363: the right-click menu holds head-look.
if grep -qF '"HELD — X4' injector/x4vr_inject.cpp; then
    ok "rclick-hold: the injector's HELD line is the planted one"
else
    bad "rclick-hold: the injector's HELD line moved"
fi
sed 's/env: run = /env: run = X4VR_TAKE=364 X4VR_HEADLOOK_WRITE=1 /' "$BASE" > /tmp/census-rh-base.log
rho='[  999970.000] inject  ui: X4'"'"'s right-click menu OPEN — the cursor is drawn through the UI canvas'
rhh='[  999970.030] inject  headlook: HELD — X4'"'"'s right-click menu is open: X4'"'"'s camera stays where it was, the cursor moves only with the mouse'
{ cat /tmp/census-rh-base.log; printf '%s\n' "$rho" "$rhh"; } > /tmp/census-rh-ok.log
want "rclick-hold: held -> counted" /tmp/census-rh-ok.log rclick-hold "1 right-click menu opening(s), head-look held for 1"
{ cat /tmp/census-rh-base.log; printf '%s\n' "$rho"; } > /tmp/census-rh-no.log
want "rclick-hold: not held -> FAIL" /tmp/census-rh-no.log "FAIL|  - " "1 right-click menu opening(s) without head-look held"
# X4VR_STEER_PICK: X4's targeting at the steering ring (take 367).
for f in '"aimpick: PATCHED — X4' '"aimpick: ON #%u — X4' '"aimpick: OFF #%u after %.1f s — %s; X4' "time(s) in the period, %llu shifted\"" '"aimpick: patch REFUSED — %s. X4 targets'; do
    if grep -qF "$f" injector/x4vr_inject.cpp; then
        ok "aimpick: the injector still prints $f"
    else
        bad "aimpick: the injector's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_STEER_PICK=1 /' "$BASE" > /tmp/census-ap-base.log
app='[  999950.000] inject  aimpick: PATCHED — X4'"'"'s pick ray (0x1d6e080: hover, click-select, IsPointingWithinAimingRange) goes through a 60-byte stub at 0x3e0000'
apr='[  999960.000] inject  steer: mouse steering engaged — pointer recentred from 1,2 to 1408,1408 so the ship starts straight'
apon='[  999960.050] inject  aimpick: ON #1 — X4'"'"'s targeting follows the ring: shift -731,+418 px (nose -0.519,0.297 NDC)'
apmid='[  999965.050] inject  aimpick: ON #1 5 s — shift -700,+400 px; the ray ran 200 time(s), 199 shifted'
apoff='[  999967.050] inject  aimpick: OFF #1 after 7.0 s — mouse steering off, or not the cockpit with the HUD; X4'"'"'s own targeting. The ray ran 280 time(s) in the period, 279 shifted'
apoff0='[  999967.050] inject  aimpick: OFF #1 after 7.0 s — mouse steering off, or not the cockpit with the HUD; X4'"'"'s own targeting. The ray ran 280 time(s) in the period, 0 shifted'
{ cat /tmp/census-ap-base.log; printf '%s\n' "$app" "$apr" "$apon" "$apmid" "$apoff"; } > /tmp/census-ap-ok.log
want "aimpick: a healthy period -> summarised" /tmp/census-ap-ok.log aimpick "1 period(s) with X4's targeting at the ring; the ray ran 280 time(s) in them, 279 shifted"
if python3 tools/score_run.py /tmp/census-ap-ok.log 2>&1 | grep -qE "^  - .*(aimpick|STEER_PICK|targeting)"; then
    bad "aimpick: a healthy period -> FAILs"
else
    ok "aimpick: a healthy period -> no FAIL"
fi
{ cat /tmp/census-ap-base.log; printf '%s\n' "$app" "$apr" "$apon"; } > /tmp/census-ap-open.log
want "aimpick: a period open at exit -> listed, not failed" /tmp/census-ap-open.log aimpick "ended: open at exit"
{ cat /tmp/census-ap-base.log; printf '%s\n' "$app" "$apr" "$apon" "$apoff0"; } > /tmp/census-ap-zero.log
want "aimpick: on 7 s, never shifted -> FAIL" /tmp/census-ap-zero.log "FAIL|  - " "the stub never shifted X4's ray — patched, not executed"
{ cat /tmp/census-ap-base.log; printf '%s\n' "$app" "$apr"; } > /tmp/census-ap-never.log
want "aimpick: steering but never ON -> FAIL" /tmp/census-ap-never.log "FAIL|  - " "X4's targeting never moved to the ring"
{ cat /tmp/census-ap-base.log; echo '[  999950.000] inject  aimpick: patch REFUSED — the pick ray'"'"'s code at 0x1d6e0c2 differs from X4 9.00. X4 targets as it always has: at the gaze plus the pointer'"'"'s offset.'; } > /tmp/census-ap-ref.log
want "aimpick: refused -> FAIL" /tmp/census-ap-ref.log "FAIL|  - " "X4VR_STEER_PICK=1 but the patch was REFUSED — the pick ray's code at 0x1d6e0c2 differs from X4 9.00"
want "aimpick: asked, no PATCHED line -> FAIL" /tmp/census-ap-base.log "FAIL|  - " "X4VR_STEER_PICK=1 but the patch was never reported PATCHED"
{ cat "$BASE"; printf '%s\n' "$app"; } > /tmp/census-ap-unasked.log
want "aimpick: patched unasked -> FAIL" /tmp/census-ap-unasked.log "FAIL|  - " "the targeting patch acted in a run that did not ask for it"
absent "aimpick: not asked -> silent" "$BASE" aimpick
# Take 367: X4's mouse-capture flag against ARMED and the first steering.
if grep -qF '"steer: X4'"'"'s mouse capture (in[0x9f33]) is now %s — "' injector/x4vr_inject.cpp; then
    ok "capture: the injector's line is the planted one"
else
    bad "capture: the injector's format moved"
fi
cpa='[  999950.000] inject  milestone: head-look write ARMED — the view now follows your head without mouse-look.'
cpe='[  999961.000] inject  steer: mouse steering engaged — pointer recentred from 19,21 to 1408,1408 so the ship starts straight'
cpon='[  999961.010] inject  steer: X4'"'"'s mouse capture (in[0x9f33]) is now ON: its centred pointer follows the mouse — cockpit, HUD up'
sed 's/env: run = /env: run = X4VR_STEER=a /' "$BASE" > /tmp/census-cp-base.log
{ cat /tmp/census-cp-base.log; printf '%s\n' "$cpa" "$cpe" "$cpon"; } > /tmp/census-cp-steer.log
want "capture: with the steering toggle -> said so" /tmp/census-cp-steer.log steer "X4 captured the mouse +11.0 s from head-look ARMED; the first mouse steering was at +11.0 s — capture came WITH the steering toggle"
cpon2='[  999951.000] inject  steer: X4'"'"'s mouse capture (in[0x9f33]) is now ON: its centred pointer follows the mouse — cockpit, HUD up'
{ cat /tmp/census-cp-base.log; printf '%s\n' "$cpa" "$cpon2" "$cpe"; } > /tmp/census-cp-load.log
if python3 tools/score_run.py /tmp/census-cp-load.log 2>&1 | grep -qF "capture came WITH the steering toggle"; then
    bad "capture: at load -> not blamed on the toggle"
else
    ok "capture: at load -> not blamed on the toggle"
fi
{ cat /tmp/census-cp-base.log; printf '%s\n' "$cpa" "$cpe" '[  999970.000] inject  steer: X4'"'"'s mouse capture (in[0x9f33]) is now OFF: its centred pointer reads 0 (hover at the view'"'"'s centre, no cursor set) — cockpit, HUD up'; } > /tmp/census-cp-never.log
want "capture: only OFF -> never captured" /tmp/census-cp-never.log steer "X4 never captured the mouse after head-look ARMED"
# Take 368: the pointer recentred once per load.
if grep -qF '"pointer: recentred after the load from %s%.0f,%.0f to %.0f,%.0f "' injector/x4vr_inject.cpp; then
    ok "load-recentre: the injector's line is the planted one"
else
    bad "load-recentre: the injector's format moved"
fi
sed 's/env: run = /env: run = X4VR_TAKE=369 X4VR_HEADLOOK_WRITE=1 /' "$BASE" > /tmp/census-lr-base.log
lra='[  999950.000] inject  milestone: head-look write ARMED — the view now follows your head without mouse-look.'
lrr='[  999950.100] inject  pointer: recentred after the load from 1756,311 to 1408,1408 — X4'"'"'s start leaves it at the window'"'"'s edge, outside what the headset shows'
{ cat /tmp/census-lr-base.log; printf '%s\n' "$lra" "$lrr"; } > /tmp/census-lr-ok.log
want "load-recentre: recentred -> reported" /tmp/census-lr-ok.log pointer "pointer recentred after 1 load(s): from 1756,311 (+0.1 s from head-look ARMED)"
{ cat /tmp/census-lr-base.log; printf '%s\n' "$lra"; } > /tmp/census-lr-no.log
want "load-recentre: armed, never recentred -> FAIL" /tmp/census-lr-no.log "FAIL|  - " "the pointer was never recentred after the load"
sed 's/env: run = /env: run = X4VR_TAKE=368 X4VR_HEADLOOK_WRITE=1 /' "$BASE" > /tmp/census-lr-old.log
{ cat /tmp/census-lr-old.log; printf '%s\n' "$lra"; } > /tmp/census-lr-old2.log
if python3 tools/score_run.py /tmp/census-lr-old2.log 2>&1 | grep -qF "never recentred after the load"; then
    bad "load-recentre: a take before 369 -> not failed"
else
    ok "load-recentre: a take before 369 -> not failed"
fi
# X4VR_HOVERLOG: X4's hover hit records, read only (take 370).
if grep -qF '"hover: pointer %+.0f,%+.0f | %s | %s | %s | %s | id %d | t5 "' injector/x4vr_inject.cpp && grep -qF '"%c d %.3f comp %p/%p vt %p pt %.2f,%.2f,%.2f"' injector/x4vr_inject.cpp; then
    ok "hover: the injector's line is the planted one"
else
    bad "hover: the injector's format moved"
fi
sed 's/env: run = /env: run = X4VR_HOVERLOG=1 /' "$BASE" > /tmp/census-hl-base.log
hl1='[  999960.000] inject  hover: pointer +489,+583 | A d 1.250 comp 0x7f0012345000/(nil) pt 1.00,2.00,3.00 | B d -1.000 comp (nil)/(nil) pt 0.00,0.00,0.00 | id 7 | t5 0.1200,-0.3400,0.0000'
hl2='[  999960.050] inject  hover: pointer +257,+306 | A d -1.000 comp (nil)/(nil) pt 0.00,0.00,0.00 | B d -1.000 comp (nil)/(nil) pt 0.00,0.00,0.00 | id -1 | t5 0.0000,0.0000,0.0000'
{ cat /tmp/census-hl-base.log; printf '%s\n' "$hl1" "$hl2"; } > /tmp/census-hl-ok.log
want "hover: lines -> summarised" /tmp/census-hl-ok.log hover "2 change line(s); 1 distinct record-A component(s), 0 record-B"
hl3='[  999960.100] inject  hover: pointer +250,+300 | A d 0.000 comp 0x7f0012345000/0x7f0012346000 vt 0x3a00000 pt 1.00,2.00,3.00 | B d -1.000 comp (nil)/(nil) vt (nil) pt 0.00,0.00,0.00 | C d -1.000 comp (nil)/(nil) vt (nil) pt 0.00,0.00,0.00 | D d -1.000 comp (nil)/(nil) vt (nil) pt 0.00,0.00,0.00 | id -1 | t5 0.0000,0.0000,0.0000'
{ cat /tmp/census-hl-base.log; printf '%s\n' "$hl3"; } > /tmp/census-hl-372.log
want "hover: take-372 line -> parsed, vtable counted" /tmp/census-hl-372.log hover "1 change line(s); 1 distinct record-A component(s), 0 record-B; the raw lines are the data (read against Patola's hovers); UI hits on 1 component type(s) (vtable)"
want "hover: asked, none -> FAIL" /tmp/census-hl-base.log "FAIL|  - " "X4VR_HOVERLOG=1 but no hover line was logged"
absent "hover: not asked -> silent" "$BASE" hover
# X4VR_PTRWATCH: who touches X4's mouse pointer (take 373).
for f in '"ptrwatch: ARMED — %d watch(es) on %zu thread(s) (%d failed%s%s): "' '"ptrwatch: NEW %s access at %#lx, [sp] %#lx, caller "' '"ptrwatch: REFUSED — no watch could be opened on %zu "'; do
    if grep -qF "$f" injector/x4vr_inject.cpp; then
        ok "ptrwatch: the injector still prints $f"
    else
        bad "ptrwatch: the injector's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_PTRWATCH=1 /' "$BASE" > /tmp/census-pw-base.log
pwa='[  999960.000] inject  ptrwatch: ARMED — 120 watch(es) on 60 thread(s) (0 failed): X4'"'"'s pointer 0x7f0000006718 and its reference 0x7f0000006734, every access sampled'
pwn1='[  999960.100] inject  ptrwatch: NEW pointer access at 0xcdfcaa, [sp] 0xa2ba64, caller 0xa2ba64, thread 4242 (X4)'
pwn2='[  999960.200] inject  ptrwatch: NEW pointer access at 0x72fbaf, [sp] 0x1, caller 0x72f7f0, thread 4242 (X4)'
pwn3='[  999960.300] inject  ptrwatch: NEW reference access at 0xcdfcb0, [sp] 0xa2ba64, caller 0xa2ba64, thread 4242 (X4)'
pww='[  999965.000] inject  ptrwatch: 5 s: 900 access(es), 3 site(s), lost 12 | P 0xcdfcaa<-0xa2ba64 x600; P 0x72fbaf<-0x1 x200; R 0xcdfcb0<-0xa2ba64 x100'
{ cat /tmp/census-pw-base.log; printf '%s\n' "$pwa" "$pwn1" "$pwn2" "$pwn3" "$pww"; } > /tmp/census-pw-ok.log
want "ptrwatch: armed -> summarised" /tmp/census-pw-ok.log ptrwatch "120 watch(es) on 60 thread(s); 3 distinct access site(s) at 3 instruction(s); lost 12 sample(s) at worst"
want "ptrwatch: asked, never armed -> FAIL" /tmp/census-pw-base.log "FAIL|  - " "X4VR_PTRWATCH=1 but the watch was never ARMED"
{ cat /tmp/census-pw-base.log; echo '[  999960.000] inject  ptrwatch: REFUSED — no watch could be opened on 60 thread(s): Permission denied'; } > /tmp/census-pw-ref.log
want "ptrwatch: refused -> FAIL" /tmp/census-pw-ref.log "FAIL|  - " "X4VR_PTRWATCH=1 but the watch was REFUSED — no watch could be opened on 60 thread(s): Permission denied"
absent "ptrwatch: not asked -> silent" "$BASE" ptrwatch
# X4VR_HUD_MOUSE: X4's UI takes the mouse where it is drawn (takes 374/375).
for f in '"uimouse: PATCHED — the mouse X4 hands its UI each frame (0xa704a0 "' '"uimouse: ON #%u — X4' '"uimouse: OFF #%u after %.1f s — %s; X4' '"the UI was fed %llu time(s), %llu through the shift; %llu "' '"perspective layer pick(s), %llu scaled",' '"uimouse: patch REFUSED — %s. X4'; do
    if grep -qF "$f" injector/x4vr_inject.cpp; then
        ok "uimouse: the injector still prints $f"
    else
        bad "uimouse: the injector's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_HUD_MOUSE=1 /' "$BASE" > /tmp/census-um-base.log
ump='[  999950.000] inject  uimouse: PATCHED — the mouse X4 hands its UI each frame (0xa704a0 -> 0x2317e00) goes through a 41-byte stub at 0xa70725'
umarm='[  999955.000] inject  milestone: head-look write ARMED — the view now follows your head without mouse-look.'
umon='[  999960.000] inject  uimouse: ON #1 — X4'"'"'s perspective UI takes the mouse x1.9040,1.9040 (canvas map 0.5252,0.5252), the markers as drawn'
umoff='[  999970.000] inject  uimouse: OFF #1 after 10.0 s — the cursor is drawn through the canvas (a menu); X4'"'"'s own mouse. the UI was fed 600 time(s), 600 through the shift; 300 perspective layer pick(s), 300 scaled'
umoff0='[  999970.000] inject  uimouse: OFF #1 after 10.0 s — the cursor is drawn through the canvas (a menu); X4'"'"'s own mouse. the UI was fed 600 time(s), 0 through the shift; 300 perspective layer pick(s), 300 scaled'
umoffns='[  1000000.000] inject  uimouse: OFF #1 after 40.0 s — the cursor is drawn through the canvas (a menu); X4'"'"'s own mouse. the UI was fed 2400 time(s), 2400 through the shift; 0 perspective layer pick(s), 0 scaled'
{ cat /tmp/census-um-base.log; printf '%s\n' "$ump" "$umarm" "$umon" "$umoff"; } > /tmp/census-um-ok.log
want "uimouse: a healthy period -> summarised" /tmp/census-um-ok.log uimouse "1 period(s); the UI was fed 600 time(s), 600 through the shift; 300 perspective layer pick(s), 300 scaled"
want "uimouse: a healthy period -> the markers are Patola's to judge too" /tmp/census-um-ok.log uimouse "and the markers where THEY are drawn"
if python3 tools/score_run.py /tmp/census-um-ok.log 2>&1 | grep -qE "^  - .*(uimouse|HUD_MOUSE|UI-mouse)"; then
    bad "uimouse: a healthy period -> FAILs"
else
    ok "uimouse: a healthy period -> no FAIL"
fi
{ cat /tmp/census-um-base.log; printf '%s\n' "$ump" "$umarm" "$umon" "$umoff0"; } > /tmp/census-um-zero.log
want "uimouse: on, the feed never passed -> FAIL" /tmp/census-um-zero.log "FAIL|  - " "the UI's mouse never passed the stub — patched, not executed"
{ cat /tmp/census-um-base.log; printf '%s\n' "$ump" "$umarm" "$umon" "$umoffns"; } > /tmp/census-um-nosplit.log
want "uimouse: 40 s on, no perspective pick scaled -> FAIL" /tmp/census-um-nosplit.log "FAIL|  - " "40 s on and X4's perspective layer pick never ran through the scale"
umon374='[  999960.000] inject  uimouse: ON #1 — X4'"'"'s UI library takes the mouse x1.9040,1.9040 (canvas map 0.5252,0.5252)'
umoff374='[  999970.000] inject  uimouse: OFF #1 after 10.0 s — the cursor is drawn through the canvas (a menu); X4'"'"'s own mouse. The UI library was fed 600 time(s), 600 moved'
{ cat /tmp/census-um-base.log; printf '%s\n' "$ump" "$umarm" "$umon374" "$umoff374"; } > /tmp/census-um-374.log
want "uimouse: take 374's format -> still summarised" /tmp/census-um-374.log uimouse "1 period(s); the UI library was fed 600 time(s), 600 moved"
want "uimouse: asked, no PATCHED line -> FAIL" /tmp/census-um-base.log "FAIL|  - " "X4VR_HUD_MOUSE=1 but the patch was never reported PATCHED"
{ cat "$BASE"; printf '%s\n' "$ump"; } > /tmp/census-um-unasked.log
want "uimouse: patched unasked -> FAIL" /tmp/census-um-unasked.log "FAIL|  - " "the UI-mouse patch acted in a run that did not ask for it"
absent "uimouse: not asked -> silent" "$BASE" uimouse

# X4VR_MEMLOG: X4's memory path for doubled and large images (take 377).
for f in '"mem: req %s size=%.1f MiB align=%llu types=0x%x%s"' '"mem: alloc %.1f MiB type %u%s%s -> %s (%d) memory %p"' '"mem: bind %s memory %p offset %.1f MiB -> %s (%d)"'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "memlog: the layer still prints $f"
    else
        bad "memlog: the layer's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_MEMLOG=1 /' "$BASE" > /tmp/census-ml-base.log
mlr1='[  999960.000] layer   mem: req img #12 4224x4149 fmt=97 DOUBLED size=267.5 MiB align=65536 types=0x7ad'
mla1='[  999960.001] layer   mem: alloc 512.0 MiB type 1 -> ok (0) memory 0x55'
mlr2='[  999960.002] layer   mem: req img #13 4224x4149 fmt=97 DOUBLED size=267.5 MiB align=65536 types=0x7ad dedicated preferred'
mla2='[  999960.003] layer   mem: alloc 267.5 MiB type 1, dedicated to img #13 4224x4149 fmt=97 DOUBLED -> FAILED (-2) memory (nil)'
mlb1='[  999960.004] layer   mem: bind img #12 4224x4149 fmt=97 DOUBLED memory 0x55 offset 0.0 MiB -> ok (0)'
{ cat /tmp/census-ml-base.log; printf '%s\n' "$mlr1" "$mla1" "$mlr2" "$mla2" "$mlb1"; } > /tmp/census-ml-ok.log
want "memlog: summarised" /tmp/census-ml-ok.log memlog "2 requirement(s), 2 logged allocation(s), 1 bind(s)"
want "memlog: the repeated signature counted" /tmp/census-ml-ok.log memlog "2 x 4224x4149 fmt=97 DOUBLED 267.5 MiB  (dedicated preferred in 1)"
want "memlog: the failed allocation named" /tmp/census-ml-ok.log memlog "FAILED: 1 allocation(s), 0 bind(s) — first: alloc 267.5 MiB type 1 result -2"
want "memlog: asked, no line -> FAIL" /tmp/census-ml-base.log "FAIL|  - " "X4VR_MEMLOG=1 but no mem: line"
absent "memlog: not asked -> silent" "$BASE" memlog

# The pool rule: images kept single-layer for X4's pool block (take 378).
for f in '"mv: %ux%u fmt=%u usage=0x%x kept SINGLE-LAYER — doubled it "' '"MiB): X4 would re-create it every frame and draw nothing. "' '"(%llu image(s) so far)",' '"mv: POOL-SINGLE img #%u is attachment %u of a "'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "mvpool: the layer still prints $f"
    else
        bad "mvpool: the layer's format moved: $f"
    fi
done
mpk1='[  999960.000] layer   mv: 4224x4149 fmt=97 usage=0x17 kept SINGLE-LAYER — doubled it needs 268.1 MiB, more than X4'"'"'s pool block holds (limit 248 MiB): X4 would re-create it every frame and draw nothing. Copies into it keep the left eye'"'"'s view, so it is flat in VR (1 image(s) so far)'
mpk2='[  999970.000] layer   mv: 4224x4149 fmt=97 usage=0x17 kept SINGLE-LAYER — doubled it needs 268.1 MiB, more than X4'"'"'s pool block holds (limit 248 MiB): X4 would re-create it every frame and draw nothing. Copies into it keep the left eye'"'"'s view, so it is flat in VR (1000 image(s) so far)'
mpl='[  999975.000] layer   mv: POOL-SINGLE img #693 is attachment 0 of a view-masked pass (rp #12) — the pool rule kept it single-layer and this framebuffer is invalid'
plant /tmp/census-mp-ok.log "$mpk1" "$mpk2"
want "mvpool: summarised" /tmp/census-mp-ok.log mvpool "1 signature(s) kept single-layer for X4's pool block, 1000 image(s): 4224x4149 fmt=97 usage=0x17 (268.1 MiB doubled)"
if python3 tools/score_run.py /tmp/census-mp-ok.log 2>&1 | grep -qE "^  - .*pool rule"; then
    bad "mvpool: kept, never attached -> FAILs"
else
    ok "mvpool: kept, never attached -> no FAIL"
fi
plant /tmp/census-mp-lost.log "$mpk1" "$mpl"
want "mvpool: attached to a masked pass -> FAIL" /tmp/census-mp-lost.log "FAIL|  - " "the pool rule kept 1 image(s) single-layer that a view-masked pass then attached (first img #693, rp #12)"
absent "mvpool: no rule fired -> silent" "$BASE" mvpool

# X4VR_COMPLOG: what each compute dispatch touches (take 379).
for f in '"comp: module c%u local %ux%ux%u, %zu bytes | resources:%s%s%s"' '"comp: dispatch %s"' '"img #%u %ux%u fmt=%u usage=0x%x %s, view %s from layer %u, %u layer(s)"'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "complog: the layer still prints $f"
    else
        bad "complog: the layer's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_COMPLOG=1 X4VR_UPSCALE=fsr_quality /' "$BASE" > /tmp/census-cl-base.log
clm1='[  999960.000] layer   comp: module c3 local 64x1x1, 9000 bytes | resources: S_input_rt@0.1 U_output_rt@0.2 | FSR: FsrEasuH'
clm2='[  999960.001] layer   comp: module c4 local 8x8x1, 4000 bytes | resources: U_froxels@0.0'
cld1='[  999961.000] layer   comp: dispatch module c3 groups 264x264x1 direct | S_input_rt@0.1: sampled img #40 2816x2816 fmt=97 usage=0x97 DOUBLED, view 2D from layer 0, 1 layer(s) | U_output_rt@0.2: storage img #90 4224x4224 fmt=97 usage=0xd single, view 2D from layer 0, 1 layer(s)'
cld2='[  999961.001] layer   comp: dispatch module c4 groups 20x20x1 direct | U_froxels@0.0: no image recorded'
{ cat /tmp/census-cl-base.log; printf '%s\n' "$clm1" "$clm2" "$cld1" "$cld2"; } > /tmp/census-cl-ok.log
want "complog: summarised" /tmp/census-cl-ok.log complog "2 compute module(s), 1 FSR; 2 distinct dispatch(es)"
want "complog: the FSR module named" /tmp/census-cl-ok.log complog "FSR c3: FsrEasuH | local 64x1x1, 9000 bytes | S_input_rt@0.1 U_output_rt@0.2"
want "complog: the FSR dispatch with its images" /tmp/census-cl-ok.log complog "dispatch c3 264x264x1 direct (1 logged) | S_input_rt@0.1: sampled img #40 2816x2816"
# X4's own FSR modules keep no names (take 379): recognised by signature, and
# a dispatch logged again only because a bindless array grew is one line.
clm3='[  999960.002] layer   comp: module c5 local 64x1x1, 13576 bytes | resources: S_sampler@0.4 S_sampler2D@0.5 U_output_rt_decl@0.6 BLOCK_BUFFER_BINDING_SLOT_DYNAMIC@4.0'
cld3='[  999961.002] layer   comp: dispatch module c5 groups 264x264x1 direct | S_sampler2D@0.5: array of 10843 (bindless, not resolved)'
cld4='[  999962.002] layer   comp: dispatch module c5 groups 264x264x1 direct | S_sampler2D@0.5: array of 10846 (bindless, not resolved)'
{ cat /tmp/census-cl-base.log; printf '%s\n' "$clm3" "$cld3" "$cld4"; } > /tmp/census-cl-sig.log
# FSR 3 (take 381): other modules' dispatches are listed, and a DOUBLED image
# reached through a one-layer view is named -- one eye only.
clm4='[  999960.003] layer   comp: module c6 local 8x8x1, 30000 bytes | resources: r_input_color_jittered@0.0 rw_upscaled_output@0.1 cbFSR3Upscaler@1.0'
cld5='[  999961.003] layer   comp: dispatch module c6 groups 528x528x1 direct | r_input_color_jittered@0.0: sampled img #40 2485x2485 fmt=97 usage=0x9f DOUBLED, view 2D from layer 0, 1 layer(s) | rw_upscaled_output@0.1: storage img #77 4224x4224 fmt=97 usage=0x9f single, view 2D from layer 0, 1 layer(s)'
{ cat /tmp/census-cl-base.log; printf '%s\n' "$clm4" "$cld5"; } > /tmp/census-cl-ffx.log
want "complog: FidelityFX-style names counted" /tmp/census-cl-ffx.log complog "1 other dispatch shape(s); 1 module(s) with FidelityFX-style resource names"
want "complog: the other dispatch listed" /tmp/census-cl-ffx.log complog "c6 (local 8x8x1, 30000 bytes) 528x528x1 direct (1 logged) | r_input_color_jittered@0.0: sampled img #N 2485x2485"
want "complog: one-eyed view of a doubled image named" /tmp/census-cl-ffx.log complog "r_input_color_jittered@0.0: sampled img #N 2485x2485 fmt=97 usage=0x9f DOUBLED, view 2D from layer 0, 1 layer(s)"
want "complog: X4's FSR module by signature" /tmp/census-cl-sig.log complog "FSR c5: by signature | local 64x1x1, 13576 bytes"
want "complog: array growth folded into one line" /tmp/census-cl-sig.log complog "dispatch c5 264x264x1 direct (2 logged) | S_sampler2D@0.5: array of N"
{ cat /tmp/census-cl-base.log; printf '%s\n' "$clm2" "$cld2"; } > /tmp/census-cl-nofsr.log
want "complog: upscaler asked, no FSR module -> note" /tmp/census-cl-nofsr.log complog "NOTE X4VR_UPSCALE=fsr_quality but no FSR compute module was compiled"
want "complog: asked, no module -> FAIL" /tmp/census-cl-base.log "FAIL|  - " "X4VR_COMPLOG=1 but no compute module was described"
absent "complog: not asked -> silent" "$BASE" complog

# The FSR fix: X4's FSR in both eyes (take 380).
for f in '"fsr: X4'"'"'s FSR pass (%zu bytes) patched for both eyes — %u table "' '"fsr: X4'"'"'s FSR pass (%zu bytes) REFUSED by the patch — it stays "' '"%ux%ux2, the second half the right eye (%llu so far)",' '"X4 asked, one-eyed (%llu so far)",' '"fsr %s: FSR in both eyes %s — %llu FSR pass(es) patched, %llu "'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "fsr: the layer still prints $f"
    else
        bad "fsr: the layer's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_UPSCALE=fsr_quality /' "$BASE" > /tmp/census-fs-base.log
fsa='[  999960.000] layer   fsr first present: FSR in both eyes armed — 0 FSR pass(es) patched, 0 REFUSED; 0 eye-covering dispatch(es) doubled, 0 smaller left one-eyed, 0 barrier(s) widened'
fso='[  999960.000] layer   fsr first present: FSR in both eyes OFF (needs X4VR_MV and the bindless mirror) — 0 FSR pass(es) patched, 0 REFUSED; 0 eye-covering dispatch(es) doubled, 0 smaller left one-eyed, 0 barrier(s) widened'
fsk='[  999960.000] layer   fsr first present: FSR in both eyes OFF (X4VR_FSR_STEREO=0) — 0 FSR pass(es) patched, 0 REFUSED; 0 eye-covering dispatch(es) doubled, 0 smaller left one-eyed, 0 barrier(s) widened'
fsp1='[  999961.000] layer   fsr: X4'"'"'s FSR pass (13576 bytes) patched for both eyes — 2 table access(es) + WorkGroupID.z * 26653 (sampled and storage tables)'
fsp2='[  999961.001] layer   fsr: X4'"'"'s FSR pass (55500 bytes) patched for both eyes — 2 table access(es) + WorkGroupID.z * 26653 (sampled and storage tables)'
fsr1='[  999961.001] layer   fsr: X4'"'"'s FSR pass (55500 bytes) REFUSED by the patch — it stays one-eyed; the right eye'"'"'s scene will be black with an upscaler'
fsd='[  999962.000] layer   fsr: dispatch 264x264x1 covers the 4224x4224 eye — run as 264x264x2, the second half the right eye (20000 so far)'
fsl='[  999962.001] layer   fsr: dispatch 48x32x1 does not cover the 4224x4224 eye — left as X4 asked, one-eyed (10000 so far)'
{ cat /tmp/census-fs-base.log; printf '%s\n' "$fsa" "$fsp1" "$fsp2" "$fsd" "$fsl"; } > /tmp/census-fs-ok.log
want "fsr: armed" /tmp/census-fs-ok.log "fsr " "FSR in both eyes: armed"
want "fsr: summarised" /tmp/census-fs-ok.log "fsr " "2 FSR pass(es) patched (13576 bytes, 55500 bytes), 0 refused; eye-covering dispatches doubled: 20000+; smaller ones left one-eyed: 10000+"
if python3 tools/score_run.py /tmp/census-fs-ok.log 2>&1 | grep -qE "^  - .*(FSR|X4VR_UPSCALE)"; then
    bad "fsr: patched and doubled -> FAILs"
else
    ok "fsr: patched and doubled -> no FAIL"
fi
{ cat /tmp/census-fs-base.log; printf '%s\n' "$fsa"; } > /tmp/census-fs-none.log
want "fsr: armed, nothing patched -> FAIL" /tmp/census-fs-none.log "FAIL|  - " "X4VR_UPSCALE=fsr_quality but no FSR pass was patched"
{ cat /tmp/census-fs-base.log; printf '%s\n' "$fsa" "$fsp1" "$fsr1" "$fsd"; } > /tmp/census-fs-ref.log
want "fsr: a refused pass -> FAIL" /tmp/census-fs-ref.log "FAIL|  - " "X4VR_UPSCALE=fsr_quality: 1 FSR pass(es) REFUSED by the patch"
{ cat /tmp/census-fs-base.log; printf '%s\n' "$fsa" "$fsp1" "$fsp2"; } > /tmp/census-fs-nodbl.log
want "fsr: patched, nothing doubled -> FAIL" /tmp/census-fs-nodbl.log "FAIL|  - " "FSR passes patched but no eye-covering dispatch was doubled"
{ cat /tmp/census-fs-base.log; printf '%s\n' "$fso"; } > /tmp/census-fs-off.log
want "fsr: off for want of the mirror -> FAIL" /tmp/census-fs-off.log "FAIL|  - " "the FSR fix is OFF (needs X4VR_MV and the bindless mirror)"
sed 's/env: run = /env: run = X4VR_FSR_STEREO=0 /' /tmp/census-fs-base.log > /tmp/census-fs-knob.log
printf '%s\n' "$fsk" >> /tmp/census-fs-knob.log
want "fsr: knob off -> reported" /tmp/census-fs-knob.log "fsr " "FSR in both eyes: OFF (X4VR_FSR_STEREO=0)"
if python3 tools/score_run.py /tmp/census-fs-knob.log 2>&1 | grep -qE "^  - .*(FSR|X4VR_UPSCALE)"; then
    bad "fsr: knob off on purpose -> FAILs"
else
    ok "fsr: knob off on purpose -> no FAIL"
fi
want "fsr: a log from before the fix -> not checked" /tmp/census-fs-base.log "fsr " "not checked: X4VR_UPSCALE=fsr_quality, but this log predates the FSR fix"
absent "fsr: not asked, no lines -> silent" "$BASE" "fsr "
sed 's/env: run = /env: run = X4VR_UPSCALE=fsr3_balanced /' "$BASE" > /tmp/census-fs-fsr3.log
printf '%s\n' "$fsa" >> /tmp/census-fs-fsr3.log
if python3 tools/score_run.py /tmp/census-fs-fsr3.log 2>&1 | grep -qE "^  - .*(FSR|X4VR_UPSCALE)"; then
    bad "fsr: an FSR 3 run, no FSR 1 pass -> FAILs"
else
    ok "fsr: an FSR 3 run, no FSR 1 pass -> no FAIL"
fi

# Phase 0 (take 385): the shadow fix is a default, X4VR_CAMDIFF an
# instrument. FIX always, no camuj readings: not judged without the
# instrument, FAIL with it.
cfp0='[  999960.000] inject  csmfit: PATCHED — the cascade fit'"'"'s 4 reads of X4'"'"'s aspect (0x4533b00/04) now read 0x3f0000/0x3f0004; coverage 0.663, FIX always'
cfr0='[  999961.000] inject  csmfit: phase 1 FIX | field 1.91982 aspect 1.0000/1.0000 | divisors 1.00000/1.00000 | coverage 0.663 | predicted cascade-0 sx 0.22954 (r 2.118 m, far 3.060) | ticks FIX 1 CONTROL 0'
# The base's run line carries X4VR_CAMDIFF=1: removed for the play run.
sed -e 's/env: run = /env: run = X4VR_CSM_COVER=0.663 /' -e 's/ X4VR_CAMDIFF=1//' \
    "$BASE" > /tmp/census-cf0.log
printf '%s\n' "$cfp0" "$cfr0" >> /tmp/census-cf0.log
want "csmfit without X4VR_CAMDIFF -> not judged" /tmp/census-cf0.log "csmfit FIX" "not judged — X4VR_CAMDIFF is off"
if python3 tools/score_run.py /tmp/census-cf0.log 2>&1 | grep -qE "^(FAIL|  -) +csmfit"; then
    bad "csmfit without X4VR_CAMDIFF -> FAILs"
else
    ok "csmfit without X4VR_CAMDIFF -> no FAIL"
fi
sed 's/env: run = /env: run = X4VR_CAMDIFF=1 /' /tmp/census-cf0.log > /tmp/census-cf1.log
want "csmfit with X4VR_CAMDIFF, no reading -> FAIL" /tmp/census-cf1.log "FAIL|  - " "csmfit: no camuj reading under a FIX report"

# The shipping line as defaults (Phase 0, take 385).
for f in '"env: defaults: %zu from launch/defaults.conf%s%s"'; do
    if grep -qF "$f" injector/x4vr_inject.cpp; then
        ok "env: the injector still prints $f"
    else
        bad "env: the injector's format moved: $f"
    fi
done
dfl='[  999960.000] inject  env: defaults: 47 from launch/defaults.conf: X4VR_H X4VR_FOV X4VR_W X4VR_RES X4VR_UPSCALE'
plant /tmp/census-df.log "$dfl"
want "env: defaults counted" /tmp/census-df.log "env " "47 setting(s) from launch/defaults.conf"
plant /tmp/census-df0.log '[  999960.000] inject  env: defaults: none recorded — not started through launch/x4vr-launch.sh, or a launcher from before Phase 0'
want "env: no defaults recorded -> said" /tmp/census-df0.log "env " "no defaults recorded"
absent "env: a log from before Phase 0 -> silent" "$BASE" "env "

# X4VR_UI_SS=2: the UI drawn at twice the resolution (take 389).
for f in '"ui ss: img #%u %ux%u (%u layer(s)) is drawn at %ux%u — %.1f "' '"ui ss: img #%u %ux%u — the UI stays at 1x: %s",' '"ui ss: a pipeline with its viewport or scissor baked in is "' '"ui ss %s: the UI at %s — %llu pass(es) drawn at 2x and blitted "'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "uiss: the layer still prints $f"
    else
        bad "uiss: the layer's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_UI_SS=2 /' "$BASE" > /tmp/census-ss-base.log
sson='[  999960.000] layer   ui ss: img #65 2816x2816 (2 layer(s)) is drawn at 5632x5632 — 242.0 MiB of ours — and blitted down with a linear filter at the end of each UI pass (X4VR_UI_SS=2)'
ssrf='[  999960.000] layer   ui ss: img #65 2816x2816 — the UI stays at 1x: the format cannot be blitted with a linear filter'
ssbk='[  999961.000] layer   ui ss: a pipeline with its viewport or scissor baked in is bound inside a 2x UI pass — it draws into a quarter of it'
{ cat /tmp/census-ss-base.log; printf '%s\n' "$sson"; } > /tmp/census-ss-ok.log
want "uiss: drawn at 2x, said" /tmp/census-ss-ok.log "uiss " "img #65 2816x2816 drawn at 5632x5632 (242.0 MiB of ours)"
if python3 tools/score_run.py /tmp/census-ss-ok.log 2>&1 | grep -qE "^(FAIL|  -) +(UI 2x|X4VR_UI_SS)"; then
    bad "uiss: drawn at 2x -> FAILs"
else
    ok "uiss: drawn at 2x -> no FAIL"
fi
want "uiss: asked, never drawn -> FAIL" /tmp/census-ss-base.log "FAIL|  - " "X4VR_UI_SS=2 but no UI pass was drawn at 2x"
{ cat /tmp/census-ss-base.log; printf '%s\n' "$ssrf"; } > /tmp/census-ss-rf.log
want "uiss: refused -> FAIL" /tmp/census-ss-rf.log "FAIL|  - " "X4VR_UI_SS=2 but the UI stays at 1x: the format cannot be blitted"
{ cat /tmp/census-ss-base.log; printf '%s\n' "$sson" "$ssbk"; } > /tmp/census-ss-bk.log
want "uiss: a baked viewport inside -> FAIL" /tmp/census-ss-bk.log "FAIL|  - " "UI 2x: a pipeline with its viewport or scissor baked in"
absent "uiss: not asked -> silent" "$BASE" "uiss "

# The Pause key stays immersive (take 384).
for f in '"paused with no menu up (the Pause key) — the view "' '"PAUSED (IsGamePaused: the ESC "' '"no longer paused without a menu"'; do
    if grep -qF "$f" injector/x4vr_inject.cpp; then
        ok "pause: the injector still prints $f"
    else
        bad "pause: the injector's format moved: $f"
    fi
done
pzk='[  999960.000] inject  headlook: paused with no menu up (the Pause key) — the view stays as it was, immersive'
pzk2='[  999965.000] inject  headlook: no longer paused without a menu'
pze='[  999970.000] inject  headlook: view is now PAUSED (IsGamePaused: the ESC menu) — idle'
pzu='[  999970.001] inject  ui: full-screen menu UP — the cursor is drawn through the UI canvas'
pzd='[  999975.000] inject  ui: full-screen menu DOWN — the cursor is drawn at the raw pointer'
pzb='[  999980.000] inject  headlook: view is now PAUSED (IsGamePaused: the ESC menu) — idle'
plant /tmp/census-pz-ok.log "$pzk" "$pzk2" "$pze" "$pzu" "$pzd"
want "pause: counted" /tmp/census-pz-ok.log "pause " "1 pause(s) with no menu kept immersive (the Pause key); 1 with the ESC menu (the window in front)"
if python3 tools/score_run.py /tmp/census-pz-ok.log 2>&1 | grep -qE "^  - .*pause"; then
    bad "pause: Pause key immersive, ESC with its menu -> FAILs"
else
    ok "pause: Pause key immersive, ESC with its menu -> no FAIL"
fi
plant /tmp/census-pz-bare.log "$pzk" "$pzk2" "$pzb"
want "pause: PAUSED with no menu -> FAIL" /tmp/census-pz-bare.log "FAIL|  - " "1 pause(s) read as PAUSED with no full-screen menu up (first at 999980.000)"
plant /tmp/census-pz-old.log "$pzb"
want "pause: before the fix -> a note" /tmp/census-pz-old.log "pause " "NOTE 1 pause(s) read as PAUSED with no full-screen menu up"
if python3 tools/score_run.py /tmp/census-pz-old.log 2>&1 | grep -qE "^  - .*pause"; then
    bad "pause: before the fix -> FAILs"
else
    ok "pause: before the fix -> no FAIL"
fi
absent "pause: no pause -> silent" "$BASE" "pause "

# FSR 3 in both eyes: AMD FidelityFX (take 382).
for f in '"ffx: module (%zu bytes, writes%s%s) is a FidelityFX pass — its "' '"ffx: img #%u %ux%u mips=%u fmt=%u usage=0x%x: a right-eye copy "' '"(%.1f MiB; %llu so far, %.1f MiB)",' '"ffx: dispatch %ux%ux%u of a FidelityFX pass run again for the "' '"right eye through %zu twin set binding(s) (%llu so far)",' '"ffx %s: FSR 3 in both eyes %s — %llu FidelityFX module(s), %llu "' '"ffx: a FidelityFX dispatch %s — left one-eyed",'; do
    if grep -qF "$f" layer/x4vr_layer.cpp; then
        ok "ffx: the layer still prints $f"
    else
        bad "ffx: the layer's format moved: $f"
    fi
done
sed 's/env: run = /env: run = X4VR_UPSCALE=fsr3_balanced /' "$BASE" > /tmp/census-f3-base.log
f3a='[  999960.000] layer   ffx first present: FSR 3 in both eyes armed — 0 FidelityFX module(s), 0 pipeline(s); 0 right-eye image copies (0.0 MiB), 0 failed; 0 twin set(s), 0 failed; 0 dispatch(es) run again for the right eye, 0 left one-eyed, 0 indirect not repeated; views: 0 layer 1, 0 copy, 0 kept; 0 barrier(s) and 0 clear(s) repeated on copies; 0 image copy command(s) touching a copy NOT repeated; 0 descriptor copies repeated'
f3o='[  999960.000] layer   ffx first present: FSR 3 in both eyes OFF (needs X4VR_MV and the bindless mirror) — 0 FidelityFX module(s), 0 pipeline(s); 0 right-eye image copies (0.0 MiB), 0 failed; 0 twin set(s), 0 failed; 0 dispatch(es) run again for the right eye, 0 left one-eyed, 0 indirect not repeated; views: 0 layer 1, 0 copy, 0 kept; 0 barrier(s) and 0 clear(s) repeated on copies; 0 image copy command(s) touching a copy NOT repeated; 0 descriptor copies repeated'
f3k='[  999960.000] layer   ffx first present: FSR 3 in both eyes OFF (X4VR_FFX_STEREO=0) — 0 FidelityFX module(s), 0 pipeline(s); 0 right-eye image copies (0.0 MiB), 0 failed; 0 twin set(s), 0 failed; 0 dispatch(es) run again for the right eye, 0 left one-eyed, 0 indirect not repeated; views: 0 layer 1, 0 copy, 0 kept; 0 barrier(s) and 0 clear(s) repeated on copies; 0 image copy command(s) touching a copy NOT repeated; 0 descriptor copies repeated'
f3m1='[  999961.000] layer   ffx: module (7316 bytes, writes rw_dilated_motion_vectors rw_dilated_depth rw_reconstructed_previous_nearest_depth …) is a FidelityFX pass — its dispatches run again for the right eye (1 so far)'
f3m2='[  999961.001] layer   ffx: module (18988 bytes, writes rw_upscaled_output) is a FidelityFX pass — its dispatches run again for the right eye (2 so far)'
f3t='[  999961.002] layer   ffx: img #94 1x1 mips=1 fmt=98 usage=0xf: a right-eye copy (0.0 MiB; 1 so far, 0.0 MiB)'
f3t2='[  999961.003] layer   ffx: img #88 4224x4224 mips=1 fmt=97 usage=0x1f: a right-eye copy (136.1 MiB; 2 so far, 136.1 MiB)'
f3d='[  999962.000] layer   ffx: dispatch 311x311x1 of a FidelityFX pass run again for the right eye through 1 twin set binding(s) (20000 so far)'
f3u='[  999962.001] layer   ffx: a FidelityFX dispatch whose set has no twin (never written through vkUpdateDescriptorSets) — left one-eyed'
{ cat /tmp/census-f3-base.log; printf '%s\n' "$f3a" "$f3m1" "$f3m2" "$f3t" "$f3t2" "$f3d"; } > /tmp/census-f3-ok.log
want "ffx: armed" /tmp/census-f3-ok.log "ffx " "FSR 3 in both eyes: armed"
want "ffx: summarised" /tmp/census-f3-ok.log "ffx " "2 FidelityFX module(s); 2 right-eye image copies (136.1 MiB in all); dispatches run again for the right eye: 20000+"
if python3 tools/score_run.py /tmp/census-f3-ok.log 2>&1 | grep -qE "^  - .*(FSR 3|FidelityFX|fsr3)"; then
    bad "ffx: modules, copies, dispatches run again -> FAILs"
else
    ok "ffx: modules, copies, dispatches run again -> no FAIL"
fi
{ cat /tmp/census-f3-base.log; printf '%s\n' "$f3a"; } > /tmp/census-f3-none.log
want "ffx: armed, no module -> FAIL" /tmp/census-f3-none.log "FAIL|  - " "X4VR_UPSCALE=fsr3_balanced but no FidelityFX module was recognised"
{ cat /tmp/census-f3-base.log; printf '%s\n' "$f3a" "$f3m1" "$f3m2"; } > /tmp/census-f3-norun.log
want "ffx: modules, nothing run again -> FAIL" /tmp/census-f3-norun.log "FAIL|  - " "FidelityFX modules recognised but no dispatch ran again for the right eye"
{ cat /tmp/census-f3-base.log; printf '%s\n' "$f3a" "$f3m1" "$f3d" "$f3u"; } > /tmp/census-f3-unp.log
want "ffx: a pass left one-eyed -> FAIL" /tmp/census-f3-unp.log "FAIL|  - " "FSR 3: a FidelityFX dispatch whose set has no twin"
want "ffx: ...and named" /tmp/census-f3-unp.log "ffx " "a FidelityFX dispatch whose set has no twin (never written through vkUpdateDescriptorSets) — left one-eyed"
{ cat /tmp/census-f3-base.log; printf '%s\n' "$f3o"; } > /tmp/census-f3-off.log
want "ffx: off for want of the mirror -> FAIL" /tmp/census-f3-off.log "FAIL|  - " "the FSR 3 fix is OFF (needs X4VR_MV and the bindless mirror)"
sed 's/env: run = /env: run = X4VR_FFX_STEREO=0 /' /tmp/census-f3-base.log > /tmp/census-f3-knob.log
printf '%s\n' "$f3k" >> /tmp/census-f3-knob.log
if python3 tools/score_run.py /tmp/census-f3-knob.log 2>&1 | grep -qE "^  - .*(FSR 3|FidelityFX|fsr3)"; then
    bad "ffx: knob off on purpose -> FAILs"
else
    ok "ffx: knob off on purpose -> no FAIL"
fi
want "ffx: a log from before the fix -> not checked" /tmp/census-f3-base.log "ffx " "not checked: X4VR_UPSCALE=fsr3_balanced, but this log predates the FSR 3 fix"
absent "ffx: not asked, no lines -> silent" "$BASE" "ffx "

if [ $fail -eq 0 ]; then echo; echo "all cases passed"; else echo; echo "FAILED"; fi
exit $fail
