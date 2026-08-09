#!/usr/bin/env bash
# The OpenXR bring-up, in two halves.
#
#   selftest  the eye test card on a plain 2-layer image, no runtime needed.
#             Runs always, under validation, and includes the case that must
#             fail (view 1 painted as a copy of view 0). If this half is red,
#             nothing the headset shows can be trusted to mean what it looks
#             like.
#   live      an XrSession on a Vulkan device the runtime created, the card
#             submitted as a projection layer, and every number the next design
#             step depends on. Needs a runtime RUNNING and a headset connected:
#             active_runtime.json is written when WiVRn or SteamVR starts and
#             removed when it stops, so its absence here is "not started", not
#             "not installed".
#
# Usage:  tests/run-xr-probe.sh [seconds]     (default 20)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
PROBE="$BUILD/tests/x4vr_test_xr_probe"
SECONDS_ARG="${1:-20}"
OUT="${X4VR_XR_OUT:-/tmp/x4vr-xrprobe.txt}"

if [[ ! -x "$PROBE" ]]; then
    echo "FAIL  $PROBE not built — cmake --build build"
    exit 1
fi

echo "== card selftest (no runtime needed) =="
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation "$PROBE" selftest
card_rc=$?
if [[ $card_rc -ne 0 ]]; then
    echo
    echo "FAIL  the test card is wrong on this machine — stop here."
    exit 1
fi

echo
echo "== live session (${SECONDS_ARG}s) =="

# Which runtime would the loader pick? Said before the run, because "it used a
# different runtime than you thought" and "the runtime misbehaved" look
# identical in the output otherwise.
for d in "${XDG_CONFIG_HOME:-$HOME/.config}/openxr/1" /etc/xdg/openxr/1 \
         /usr/local/etc/xdg/openxr/1 /usr/share/openxr/1; do
    if [[ -e "$d/active_runtime.json" ]]; then
        echo "active runtime: $d/active_runtime.json"
        sed -n 's/.*"name"[^"]*"\([^"]*\)".*/  name: \1/p;s/.*"library_path"[^"]*"\([^"]*\)".*/  library: \1/p' \
            "$d/active_runtime.json"
        break
    fi
done
if [[ -n "${XR_RUNTIME_JSON:-}" ]]; then
    echo "XR_RUNTIME_JSON overrides the above: $XR_RUNTIME_JSON"
fi

"$PROBE" "$SECONDS_ARG" 2>&1 | tee "$OUT"
live_rc=${PIPESTATUS[0]}

echo
if [[ $live_rc -ne 0 ]]; then
    echo "FAIL  live session did not complete — see $OUT"
    grep '^FAIL=' "$OUT" | sed 's/^/      /'
    if grep -q 'no active runtime' "$OUT"; then
        echo "      Start WiVRn (wivrn-dashboard, then connect the headset) or"
        echo "      SteamVR, and run this again."
    fi
    exit 1
fi

echo "== what it found =="
grep '^KEY_' "$OUT" | sed 's/^KEY_/  /'
echo
echo "full output: $OUT"
