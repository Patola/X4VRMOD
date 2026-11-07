#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The zip holds what the launcher needs, and knows it's the zip when it's unpacked.
#     packaging/ci/check-zip.sh <zip>
set -euo pipefail
zip=$(realpath "$1")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(".")' "$zip"
U=$(echo x4vrmod-*)
chmod +x "$U/x4vrmod"
for f in x4vrmod x4vrmod-launcher x4vrmod-data/manifest x4vrmod-data/lib/libVkLayer_X4VR_core.so \
    x4vrmod-data/lib/libx4vr_inject.so x4vrmod-data/launch/x4vr-launch.sh \
    x4vrmod-data/extension/x4vrmod/ext_01.cat; do
    [ -e "$U/$f" ] || { echo "missing: $f"; exit 1; }
done
out=$("$U/x4vrmod" doctor --no-vr 2>&1 || true)
echo "$out"
grep -q '(zip)' <<<"$out" || { echo "doctor doesn't know it's the zip"; exit 1; }
# The window, on the oldest Qt 6 it's built for: still up after a few seconds is a pass.
chmod +x "$U/x4vrmod-launcher"
rc=0
HOME="$T/home" QT_QPA_PLATFORM=offscreen timeout 5 "$U/x4vrmod-launcher" || rc=$?
[ "$rc" = 124 ] || { echo "the window didn't stay up: exit $rc"; exit 1; }
echo "the window starts"
