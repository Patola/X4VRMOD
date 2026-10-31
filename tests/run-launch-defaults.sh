#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The shipping line as defaults (Phase 0): launch/x4vr-defaults.sh applied to
# launch/defaults.conf in a clean environment, case by case. The table itself
# is checked against take 375's run line by the acceptance take; here, the
# rules: explicit wins, the eye size drives W, RES and the upscaler
# (decision 6), X4VR_DEFAULTS=0 applies nothing.
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

# run <assignments...> -- prints "VAR=value" for the queried variables, and
# DEFAULTED=<list>, after applying the defaults in a clean environment.
q='X4VR_H X4VR_W X4VR_RES X4VR_UPSCALE X4VR_MV X4VR_POSE X4VR_OFFAXIS X4VR_FOV'
run() {
    env -i HOME="$HOME" PATH="$PATH" "$@" bash -c '
        source "$0/launch/x4vr-defaults.sh"
        x4vr_apply_defaults "$0/launch/defaults.conf" || { echo RC=1; exit 0; }
        for v in '"$q"'; do echo "$v=${!v-<unset>}"; done
        echo "DEFAULTED=$X4VR_DEFAULTED"
        echo "N=$(wc -w <<<"$X4VR_DEFAULTED")"' "$ROOT"
}
want() { # want <label> <output> <line>...
    local label=$1 out=$2; shift 2
    local l
    for l in "$@"; do
        if ! grep -qxF -- "$l" <<<"$out"; then
            bad "$label — wanted '$l'"
            sed 's/^/        got: /' <<<"$out" | head -12
            return
        fi
    done
    ok "$label"
}

out=$(run)
want "nothing set: the shipping line at 2816², native" "$out" \
    X4VR_H=2816 X4VR_W=5632 X4VR_RES=2816x2816 X4VR_UPSCALE=none \
    X4VR_MV=1 X4VR_POSE=1 X4VR_OFFAXIS=off X4VR_FOV=1.4917 N=47
out=$(run X4VR_H=4224)
want "X4VR_H=4224: W, RES follow, FSR 3 Balanced" "$out" \
    X4VR_H=4224 X4VR_W=8448 X4VR_RES=4224x4224 X4VR_UPSCALE=fsr3_balanced N=46
out=$(run X4VR_H=2817)
want "...just above 2816: FSR 3 Balanced" "$out" X4VR_UPSCALE=fsr3_balanced
out=$(run X4VR_H=2560)
want "...below 2816: native" "$out" X4VR_W=5120 X4VR_RES=2560x2560 X4VR_UPSCALE=none
out=$(run X4VR_H=4224 X4VR_UPSCALE=none)
want "an explicit upscaler wins over the eye size" "$out" X4VR_UPSCALE=none N=45
if grep -q "X4VR_UPSCALE" <<<"$(grep '^DEFAULTED=' <<<"$out")"; then
    bad "...and is not listed as defaulted"
else
    ok "...and is not listed as defaulted"
fi
out=$(run X4VR_H=2816 X4VR_UPSCALE=fsr_quality)
want "an explicit FSR 1 at 2816 wins" "$out" X4VR_UPSCALE=fsr_quality
out=$(run X4VR_MV=0 X4VR_OFFAXIS=)
want "explicit 0 and explicit empty both win" "$out" X4VR_MV=0 X4VR_OFFAXIS= N=45
out=$(run X4VR_W=6000)
want "an explicit X4VR_W wins, RES still follows H" "$out" X4VR_W=6000 X4VR_RES=2816x2816
out=$(run X4VR_DEFAULTS=0)
want "X4VR_DEFAULTS=0: nothing applied" "$out" X4VR_H='<unset>' X4VR_MV='<unset>' \
    X4VR_UPSCALE='<unset>' DEFAULTED= N=0
out=$(run X4VR_DEFAULTED="X4VR_STALE")
if grep -q STALE <<<"$out"; then
    bad "an inherited X4VR_DEFAULTED is replaced"
else
    ok "an inherited X4VR_DEFAULTED is replaced"
fi
out=$(run X4VR_H=big)
want "a non-numeric X4VR_H is refused" "$out" RC=1
# Every row of the table is applied: its variables all come back set.
rows=$(grep -cE '^(setting|internal) ' "$ROOT/launch/defaults.conf")
out=$(run)
n=$(sed -n 's/^N=//p' <<<"$out")
if [[ "$n" == "$((rows + 3))" ]]; then
    ok "every table row applied ($rows rows + W, RES, UPSCALE)"
else
    bad "table rows $rows + 3 derived, but $n applied"
fi

if [[ $fail -eq 0 ]]; then echo; echo "all cases passed"; else echo; echo "FAILED"; fi
exit $fail
