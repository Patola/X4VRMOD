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
#   * picks the upscaler when X4VR_UPSCALE is unset (decision 6, Patola
#     2026-10-31): fsr3_balanced above 2816 per eye, none otherwise;
#   * lists the variables it set in X4VR_DEFAULTED, which the injector logs
#     beside the run line.
# X4VR_DEFAULTS=0 applies none of it: the code's own defaults, as every take
# before Phase 0 ran.

# Above this eye size the default upscaler is FSR 3 Balanced (decision 6).
X4VR_NATIVE_MAX=2816

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
    if (( X4VR_H > X4VR_NATIVE_MAX )); then
        _x4vr_default X4VR_UPSCALE fsr3_balanced
    else
        _x4vr_default X4VR_UPSCALE none
    fi
    return 0
}
