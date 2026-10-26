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
want "hwrite without the extension served -> FAIL" /tmp/census-hw-noext.log FAIL "never served x4vrmod_headlook ENABLED"
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
absent "menucam: no write-mode arming -> silent" "$BASE" menucam
# Take 329: the ESC menu PAUSES, and the pre-329 view line called that
# "external/floating" -- the centring, gated on the cockpit, never acted.
mcpaused='[  999989.950] inject  headlook: view is now PAUSED (IsGamePaused: the ESC menu) — idle'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mcpaused" "$(mcpose 999990.500 2.1700 9.2400)" "$(mcpose 999991.500 2.1700 9.2400)" "$mcup" "$mcrep"; } > /tmp/census-mc-esc-stuck.log
want "menucam ESC pause frozen -> counted as a menu period" /tmp/census-mc-esc-stuck.log "menucam =>" "FROZEN: in 1 cockpit or ESC-pause period(s)"
want "menucam ESC pause frozen, asked -> FAIL" /tmp/census-mc-esc-stuck.log FAIL "the centring did not act"
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mcpaused" "$(mcpose 999990.500 0.0000 0.0000)" "$(mcpose 999991.500 0.0000 0.0000)" "$mcup" "$mcrep"; } > /tmp/census-mc-esc-ok.log
want "menucam ESC pause centred -> CENTRED" /tmp/census-mc-esc-ok.log "menucam =>" "every cockpit and ESC-pause menu period CENTRED"
mcold='[  999989.950] inject  headlook: view is now external/floating — idle'
{ cat /tmp/census-mc-base.log; printf '%s\n' "$mcarm" "$mcdown" "$mcold" "$(mcpose 999990.500 2.1700 9.2400)" "$mcup"; } > /tmp/census-mc-oldlabel.log
want "menucam pre-329 label -> NOTE names the pause" /tmp/census-mc-oldlabel.log "menucam NOTE" "pre-329 label"

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

if [ $fail -eq 0 ]; then echo; echo "all cases passed"; else echo; echo "FAILED"; fi
exit $fail
