#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The layer writes PNG; every analyser must read it, and read it IDENTICALLY.
#
# Take 176 wrote gigabytes of uncompressed PPM in bursts and froze the game for
# 30 s at a time. Take 177 switched to PNG -- and then FAILED with "no present
# dumps were found" while twenty of them sat on disk, because tools/eye_stereo.py
# globbed `.ppm` and score_run.py trusted it. An intent gate that fires on the
# ANALYSER's staleness accuses the run of a fault it does not have, which is the
# exact failure the gate exists to prevent.
#
# So this asserts the two halves that broke:
#   1. the layer's PNG and PPM outputs decode to the SAME pixels -- lossless, as
#      photometry requires (tools/bright_object.py takes luminance ratios off
#      these, so a lossy format would corrupt the measurement, not just the file);
#   2. both analysers read both formats and agree.
set -uo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
BIN="$BUILD/tests/x4vr_test_multiview_render"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

[[ -x "$BIN" ]] || { echo "build first: cmake --build $BUILD" >&2; exit 1; }

fails=0
say() {
    if [[ "$2" == 1 ]]; then printf 'ok   %-44s %s\n' "$1" "$3"
    else printf 'FAIL %-44s %s\n' "$1" "$3"; fails=$((fails + 1)); fi
}

# X4VR_MV_DUMP_IMG forces the dump: the harness's two layers are identical, so
# the opportunistic path (X4VR_MV_DUMP_AUTO) correctly declines to write.
for fmt in png ppm; do
    extra=()
    [[ $fmt == ppm ]] && extra=(X4VR_DUMP_PPM=1)
    env VK_ADD_LAYER_PATH="$BUILD/layer" \
        VK_LOADER_LAYERS_ENABLE=VK_LAYER_X4VR_core \
        X4VR_VR=0 X4VR_STEREO=1 X4VR_MV=1 X4VR_MV_PROBE=1 \
        X4VR_MV_DUMP_IMG=0 X4VR_MV_DUMP="$OUT/$fmt" \
        "${extra[@]}" X4VR_LOG="$OUT/$fmt.log" \
        timeout 120 "$BIN" >/dev/null 2>&1
done

a="$OUT/png-img0-n0-layer0.png"
b="$OUT/ppm-img0-n0-layer0.ppm"
if [[ -r "$a" && -r "$b" ]]; then
    say "layer writes both formats" 1 \
        "$(stat -c%s "$a") B png, $(stat -c%s "$b") B ppm"
    # CAPTURED and judged. Printed and ignored, this case could not fail --
    # which is the defect it exists to catch, in the file that catches it.
    res=$(python3 - "$a" "$b" "$ROOT" <<'PY'
import sys, numpy as np
png, ppm, root = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, f"{root}/tools")
import eye_stereo, bright_object
ok = True
# 1. lossless: the two files decode to the same pixels
e0, e1 = eye_stereo.read_ppm(png), eye_stereo.read_ppm(ppm)
if e0.shape != e1.shape or not np.array_equal(e0, e1):
    print(f"FAIL png/ppm differ via eye_stereo: {e0.shape} vs {e1.shape}"); ok = False
# 2. the other analyser agrees with itself across formats
b0, b1 = bright_object.read_ppm(png), bright_object.read_ppm(ppm)
if b0[:2] != b1[:2] or bytes(b0[2]) != bytes(b1[2]):
    print("FAIL png/ppm differ via bright_object"); ok = False
# 3. and the two analysers agree with EACH OTHER on the same file
if not np.array_equal(np.frombuffer(b0[2], np.uint8).reshape(e0.shape),
                      e0.astype(np.uint8)):
    print("FAIL eye_stereo and bright_object disagree on the same png"); ok = False
print("PIXELS-IDENTICAL" if ok else "PIXELS-DIFFER")
PY
)
    [[ "$res" == *PIXELS-IDENTICAL* ]] && ok=1 || ok=0
    say "png decodes identically to ppm" "$ok" "$(echo "$res" | tail -1)"
else
    say "layer writes both formats" 0 "missing $a or $b"
fi

echo
if (( fails )); then echo "$fails case(s) failed"; exit 1; fi
echo "all cases passed"
