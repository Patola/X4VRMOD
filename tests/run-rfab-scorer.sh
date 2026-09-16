#!/usr/bin/env bash
# Control suite for score_run.py's `rfab` block — the rate-fix A/B, take 295.
#
# Plants lagsample streams whose lag per arm is KNOWN by construction (the
# camera is the head delayed by exactly N ms) and checks the block recovers it
# and reaches each of its three verdicts. A scorer that has only ever been seen
# to print one of them is worth nothing: take 294's kpab block printed a null
# that was an artifact, and nobody had ever made it print anything else.
#
# Appended to a real take's log because the scorer needs a render-pass inventory
# before it will read anything at all.
set -u
CDPATH=
cd "$(dirname "$0")/.." || exit 1

BASE=/tmp/x4vr-take294.log
if [ ! -f "$BASE" ]; then
    echo "SKIP  $BASE absent — the planted logs are appended to a real run"
    exit 0
fi

fail=0
ok()   { printf 'ok    %s\n' "$1"; }
bad()  { printf 'FAIL  %s\n' "$1"; fail=1; }

plant() {   # plant <path> <control_ms> <armb_ms> [which: rf|ug]
    python3 - "$1" "$2" "$3" "$BASE" "${4:-rf}" <<'PY'
import math, random, sys
path, ctl, b, base = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
which = sys.argv[5]
out, t, hist, rng = [], 930000.0, [], random.Random(7)
for ph in range(6):
    arm = ph & 1
    ug = arm if which == 'ug' else 0
    lag = (b if arm else ctl) / 1000.0
    for _ in range(900):
        t += 0.020
        head = 45.0*math.sin(2*math.pi*0.3*t) + 6.0*math.sin(2*math.pi*1.1*t)
        hist.append((t, head))
        tp, cam = t - lag, head
        for k in range(len(hist)-1, 0, -1):
            if hist[k-1][0] <= tp <= hist[k][0]:
                f = (tp-hist[k-1][0])/(hist[k][0]-hist[k-1][0])
                cam = hist[k-1][1] + f*(hist[k][1]-hist[k-1][1]); break
        cam += rng.gauss(0, 0.02)
        out.append(f"[ {t:12.3f}] inject  lagsample: arm=1 view=cockpit steer=1 "
                   f"cam=1 head={head:.3f},0.000 x4={cam:.3f},0.000 kparm=0 "
                   f"kp=0.250 ylim=85.0 rfarm={arm} rftau=110 "
                   f"ugarm={ug}")
open(path, "w").write(open(base, errors="replace").read() + "\n".join(out) + "\n")
PY
}

check() {   # check <name> <control_ms> <armb_ms> <verdict> [rf|ug]
    local name=$1 ctl=$2 armb=$3 want=$4 which=${5:-rf}
    local log=/tmp/rfab-ctl-$name.log
    plant "$log" "$ctl" "$armb" "$which"
    local got
    got=$(python3 tools/score_run.py "$log" 2>&1 | grep -E "^${which}ab   =>")
    if [ -z "$got" ]; then
        bad "$name -> rfab printed no verdict at all"
        return
    fi
    # the recovered lag must match what was planted, to 3 ms (rf only — the
    # ugab block reports per-phase values rather than a median line)
    local med
    med=$(printf '%s\n' "$got" | sed -n 's/.*median lag \([0-9]*\) ms control, \([0-9]*\) ms ARM_B.*/\1 \2/p')
    set -- $med
    if [ "$which" = ug ]; then :
    elif [ $# -eq 2 ] && [ "$(( $1 > ${ctl%.*} ? $1 - ${ctl%.*} : ${ctl%.*} - $1 ))" -le 3 ] \
                    && [ "$(( $2 > ${armb%.*} ? $2 - ${armb%.*} : ${armb%.*} - $2 ))" -le 3 ]; then
        ok "$name -> recovered ${1}/${2} ms (planted ${ctl%.*}/${armb%.*})"
    else
        bad "$name -> recovered '$med', planted ${ctl%.*}/${armb%.*}"
    fi
    if printf '%s\n' "$got" | grep -qF "$want"; then
        ok "$name -> $want"
    else
        bad "$name -> wanted '$want', got: $(printf '%s' "$got" | tr '\n' ' ')"
    fi
}

check works 80 50 "SEPARATED — every ARM_B phase beats every control phase"
check null  80 80 "the per-phase ranges OVERLAP"
check worse 50 80 "SEPARATED THE WRONG WAY"

# and the same three for the freshness-gate block, which shares the stream
check ug-works 117 53 "SEPARATED — every ungated phase has less lag" ug
check ug-null  117 117 "the per-phase ranges OVERLAP" ug
check ug-worse  53 117 "SEPARATED THE WRONG WAY — ungating made the lag worse" ug

if [ $fail -eq 0 ]; then echo; echo "all cases passed"; else echo; echo "FAILED"; fi
exit $fail
