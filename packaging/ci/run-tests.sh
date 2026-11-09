#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Everything that can run with neither X4 nor a GPU, for CI. What checks patch sites against
# X4's bytes skips itself with no copy here. What drives the layer runs on Mesa's software
# Vulkan (lavapipe) when it's installed; run-xr-probe needs a VR runtime and isn't run.
#     packaging/ci/run-tests.sh [build dir]
set -uo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$(realpath "${1:-$ROOT/build}")"
cd "$ROOT"
export LC_ALL=C QT_QPA_PLATFORM=offscreen X4VR_BUILD="$BUILD"
failed=()

run() { # run <name> <command...>
    local name=$1 out rc
    shift
    out=$("$@" 2>&1)
    rc=$?
    if [ "$rc" = 0 ]; then
        printf 'ok    %-36s %s\n' "$name" "$(tail -1 <<<"$out" | cut -c1-60)"
    else
        printf 'FAIL  %s\n' "$name"
        grep -E '^(FAIL|bad)|FAIL ' <<<"$out" | head -20 | sed 's/^/      /'
        tail -5 <<<"$out" | sed 's/^/      | /'
        failed+=("$name")
    fi
}

# Every self-checking program, leaving out those that want a GPU, a runtime or arguments. The
# scripts below run the GPU ones.
for t in "$BUILD"/tests/x4vr_test_*; do
    [ -x "$t" ] && [ -f "$t" ] || continue
    case "${t##*/}" in
        x4vr_test_cursor_render | x4vr_test_menu_warp_render | x4vr_test_sbs_menu_warp | \
            x4vr_test_exit_teardown | x4vr_test_multiview_* | \
            x4vr_test_readback_speed | x4vr_test_spirv_patch | x4vr_test_wgate_patch | \
            x4vr_test_xr_probe) continue ;;
    esac
    run "${t##*/}" timeout 120 "$t"
done

# The shader patches, run in the interpreter: no GPU.
P="$BUILD/tests/x4vr_test_spirv_patch"
if [ -f "$BUILD/tests/sample_camera_block.vert.spv" ]; then
    run "spirv vert-eye-check" "$P" vert-eye-check "$BUILD/tests/sample_camera_block.vert.spv"
    run "spirv eye-roundtrip-check" "$P" eye-roundtrip-check \
        "$BUILD/tests/sample_camera_block.vert.spv" "$BUILD/tests/sample_invproj_clip.frag.spv"
fi

run check-exports tests/check-exports.sh "$BUILD/injector/libx4vr_inject.so"
for s in census-scorer config-fork content-fork deploy-extensions dither-scorer \
    extension-selectors i18n launch-defaults launcher-install launcher-package launcher-x4-bytes \
    make-extension pace-scorer plantid-scorer png-reader rfab-scorer watch-relay x4-call-guards; do
    run "$s" timeout 600 bash "tests/run-$s.sh"
done

lvp=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)
if [ -n "$lvp" ]; then
    export VK_DRIVER_FILES="$lvp" VK_ICD_FILENAMES="$lvp"
    for s in multiview-enable multiview-double multiview-render cursor menu-warp wgate \
        dump-format no-exit-destructors offaxis-bringup; do
        run "$s (lavapipe)" timeout 600 bash "tests/run-$s.sh"
    done
else
    echo "skip  no lavapipe: the layer's GPU tests aren't run"
fi

echo
if [ "${#failed[@]}" -gt 0 ]; then
    echo "${#failed[@]} failed: ${failed[*]}"
    exit 1
fi
echo "all passed"
