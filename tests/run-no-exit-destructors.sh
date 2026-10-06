#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Neither of our libraries may have an exit-time destructor.
#
# **Why this exists.** Take 323: gamescope aborted at exit inside
# x4vr_DestroyImageView. Our libraries live in other people's processes, and
# those processes call into them after exit() has begun -- gamescope destroys
# its image views from its own exit-time destructors, and anyone may call the
# libc functions the injector interposes. A static destructor of ours is
# registered when the library loads, after the host's, so it runs first; every
# later call then reads a freed table. The rule (common/x4vr_immortal.hpp):
# nothing of ours is ever destroyed.
#
# Two checks, each with a control so it cannot pass by seeing nothing:
#   1. static: no call or tail jump to __cxa_atexit in either .so (the control is a tiny
#      library with one static std::map, which must count 1);
#   2. runtime: tests/exit_teardown.cpp destroys Vulkan objects from a global's
#      destructor through the layer -- as gamescope, and as X4 -- and must exit
#      0 (the control runs it without the layer). X4VR_OLD_LAYER=<dir of a
#      layer build from before the fix> adds the control that must FAIL:
#      against 71fe6e7's layer it printed "terminate called after throwing an
#      instance of 'std::out_of_range' / unordered_map::at", gamescope's crash.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1
BUILD="${X4VR_BUILD:-build}"

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
count() { objdump -d "$1" | grep -cE '(call|jmp)[^<]*<__cxa_atexit'; }

if ! command -v objdump >/dev/null; then
    echo "SKIP  no objdump"
    exit 0
fi

# ---- 1. static
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '#include <map>\nstd::map<int, int> g;\nint f() { return (int)g.size(); }\n' > "$tmp/ctl.cpp"
if c++ -O2 -fPIC -shared "$tmp/ctl.cpp" -o "$tmp/libctl.so" 2>/dev/null; then
    n=$(count "$tmp/libctl.so")
    if [ "$n" -ge 1 ]; then ok "control: a static std::map registers $n destructor(s)"
    else bad "control: a static std::map registered none — the count cannot see"; fi
else
    bad "control: could not build the control library"
fi
for so in "$BUILD/layer/libVkLayer_X4VR_core.so" "$BUILD/injector/libx4vr_inject.so"; do
    if [ ! -f "$so" ]; then bad "$so not built"; continue; fi
    n=$(count "$so")
    if [ "$n" -eq 0 ]; then ok "$(basename "$so"): no exit-time destructor"
    else
        bad "$(basename "$so"): $n exit-time destructor(s) registered, by:"
        objdump -dC --no-show-raw-insn "$so" |
            awk '/^[0-9a-f]+ <.*>:$/{fn=$0} /(call|jmp)[^<]*<__cxa_atexit/{print fn}' |
            sort | uniq -c | cut -c1-150 | sed 's/^/        /'
    fi
done

# ---- 2. runtime
BIN="$BUILD/tests/x4vr_test_exit_teardown"
if [ ! -x "$BIN" ]; then
    bad "$BIN not built"
else
    run() {  # run <label> <env...>
        local label=$1; shift
        local out rc
        out=$(env "$@" X4VR_LOG= "$BIN" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "teardown returned"; then
            ok "$label: exit 0 after destroying from a global's destructor"
        else
            bad "$label: exit status $rc"
            printf '%s\n' "$out" | tail -4 | sed 's/^/        | /'
        fi
    }
    run "control, no layer" VK_LOADER_LAYERS_ENABLE= X4VR_TEST_APP_NAME=gamescope
    run "layer, as gamescope" "VK_ADD_LAYER_PATH=$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core X4VR_TEST_APP_NAME=gamescope
    run "layer, as X4" "VK_ADD_LAYER_PATH=$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core X4VR_TEST_APP_NAME=X4
    if [ -n "${X4VR_OLD_LAYER:-}" ]; then
        out=$(env "VK_ADD_LAYER_PATH=$X4VR_OLD_LAYER" \
            VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
            X4VR_TEST_APP_NAME=gamescope X4VR_LOG= "$BIN" 2>&1); rc=$?
        if [ "$rc" -ne 0 ]; then
            ok "control, old layer: exit status $rc ($(printf '%s' "$out" | grep -m1 -o 'unordered_map::at\|terminate' || echo 'no message'))"
        else
            bad "control, old layer: exited 0 — the test cannot see the crash"
        fi
    fi
fi

exit "$fail"
