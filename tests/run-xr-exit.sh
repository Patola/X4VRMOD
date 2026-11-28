#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# X4 calling exit() with the VR session running (take 497L's first attempt): tests/xr_exit.cpp
# against a real runtime, Monado's, with its null compositor and simulated headset -- no
# display, no headset. The service runs in a private XDG_RUNTIME_DIR of its own, so a WiVRn or
# Monado the player has running is never touched. SKIP without monado-service or a session.
#
# X4VR_OLD_LAYER=<dir of a layer build from before the fix> adds the control that must FAIL:
# against d3d260c's layer, exit status 139 (SIGSEGV, the XR thread in unmapped code).
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1
BUILD="${X4VR_BUILD:-build}"
BIN="$BUILD/tests/x4vr_test_xr_exit"
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

[ -x "$BIN" ] || { echo "FAIL  $BIN not built"; exit 1; }
SERVICE=$(command -v monado-service) || { echo "SKIP  no monado-service"; exit 0; }
JSON=""
for j in /usr/share/openxr/1/openxr_monado.json /usr/local/share/openxr/1/openxr_monado.json; do
    [ -f "$j" ] && { JSON=$j; break; }
done
[ -n "$JSON" ] || { echo "SKIP  no openxr_monado.json"; exit 0; }

tmp=$(mktemp -d)
mkdir -m 700 "$tmp/run"
svc=""
cleanup() {
    [ -n "$svc" ] && kill "$svc" 2>/dev/null && wait "$svc" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT
XDG_RUNTIME_DIR="$tmp/run" XRT_COMPOSITOR_NULL=1 SIMULATED_ENABLE=1 XRT_NO_STDIN=1 \
    "$SERVICE" >"$tmp/service.log" 2>&1 &
svc=$!
for _ in $(seq 100); do
    [ -S "$tmp/run/monado_comp_ipc" ] && break
    kill -0 "$svc" 2>/dev/null || break
    sleep 0.1
done
if [ ! -S "$tmp/run/monado_comp_ipc" ]; then
    echo "SKIP  monado-service did not come up:"
    tail -3 "$tmp/service.log" | sed 's/^/        | /'
    exit 0
fi

# run <layer dir> <log> [quit]: sets out and rc.
run() {
    out=$(env XDG_RUNTIME_DIR="$tmp/run" XR_RUNTIME_JSON="$JSON" VK_ADD_LAYER_PATH="$1" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core X4VR_VR=1 X4VR_LOG="$2" \
        timeout 60 "$BIN" ${3:+"$3"} 2>&1)
    rc=$?
}

run "$BUILD/layer" "$tmp/x4vr.log"
if grep -q '^SKIP' <<<"$out"; then
    grep '^SKIP' <<<"$out" | sed 's/^SKIP /SKIP  /'
    exit 0
fi
if [ "$rc" -eq 3 ] && grep -q "the last exit handler returned" <<<"$out"; then
    ok "exit() with the session running: main's status 3, every exit handler ran"
else
    bad "exit() with the session running: exit status $rc"
    tail -4 <<<"$out" | sed 's/^/        | /'
fi
if grep -q "vr: X4 is exiting with the session running" "$tmp/x4vr.log" &&
    grep -q "the XR thread stopped" "$tmp/x4vr.log"; then
    ok "the layer stopped its XR thread at exit, before the runtime closed"
else
    bad "the layer's log does not say its XR thread stopped at exit"
    grep -E "vr: X4 is exiting|XR thread" "$tmp/x4vr.log" | tail -3 | sed 's/^/        | /'
fi

# A normal quit, unchanged: the device destroyed first, so vr_shutdown ends the session and the
# stop at exit has nothing to do.
run "$BUILD/layer" "$tmp/quit.log" quit
if [ "$rc" -eq 0 ] && grep -q "the last exit handler returned" <<<"$out" &&
    grep -q "vr summary (final)" "$tmp/quit.log" && ! grep -q "vr: X4 is exiting" "$tmp/quit.log"; then
    ok "a normal quit: exit 0, the session ended by vr_shutdown, nothing stopped at exit"
else
    bad "a normal quit: exit status $rc"
    { tail -2 <<<"$out"; grep -E "vr summary \(final\)|vr: X4 is exiting" "$tmp/quit.log"; } |
        sed 's/^/        | /'
fi

if [ -n "${X4VR_OLD_LAYER:-}" ]; then
    run "$X4VR_OLD_LAYER" "$tmp/old.log"
    if grep -q '^SKIP' <<<"$out"; then
        bad "control, old layer: no session ran, so it shows nothing"
    elif [ "$rc" -ne 3 ]; then
        ok "control, old layer: exit status $rc"
    else
        bad "control, old layer: exited 3 — the test cannot see the crash"
    fi
fi

exit "$fail"
