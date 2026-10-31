# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# x4vr-defaults.sh — the shipping line as defaults (docs/packaging-plan.md,
# Phase 0). Sourced by launch/x4vr-launch.sh and by
# tests/run-launch-defaults.sh; it changes nothing until called.
#
# x4vr_apply_defaults <defaults.conf>
#   * exports every variable of the table that is not set (an explicit
#     setting always wins, empty included);
#   * derives X4VR_W = 2 x X4VR_H and X4VR_RES = X4VR_H x X4VR_H when unset;
#   * no upscaler unless asked (decision 6, after take 386): a rule here
#     that switched FSR on above 2816 per eye made 3072² native impossible
#     without knowing to override it. Pairing a resolution with an upscaler
#     is the graphical launcher's job, where the user sees it;
#   * lists the variables it set in X4VR_DEFAULTED, which the injector logs
#     beside the run line.
# X4VR_DEFAULTS=0 applies none of it: the code's own defaults, as every take
# before Phase 0 ran.

_x4vr_default() {
    if [[ -z "${!1+x}" ]]; then
        export "$1=$2"
        X4VR_DEFAULTED+="${X4VR_DEFAULTED:+ }$1"
    fi
}

x4vr_apply_defaults() {
    local conf=$1 kind var val rest
    X4VR_DEFAULTED=""
    export X4VR_DEFAULTED
    [[ "${X4VR_DEFAULTS:-1}" == 0 ]] && return 0
    if [[ ! -r "$conf" ]]; then
        echo "x4vr-launch: defaults table $conf not readable" >&2
        return 1
    fi
    while read -r kind var val rest; do
        [[ -z "${kind:-}" || "$kind" == \#* ]] && continue
        _x4vr_default "$var" "$val"
    done < "$conf"
    if [[ ! "$X4VR_H" =~ ^[0-9]+$ ]]; then
        echo "x4vr-launch: X4VR_H='$X4VR_H' is not an eye size in pixels" >&2
        return 1
    fi
    _x4vr_default X4VR_W "$((2 * X4VR_H))"
    _x4vr_default X4VR_RES "${X4VR_H}x${X4VR_H}"
    return 0
}
