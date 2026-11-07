#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The package just installed on this system: it's all there, it starts, and it knows it's a
# package. No X4 or GPU here, so that's as far as it can go.
set -euo pipefail
for f in /usr/bin/x4vrmod /usr/bin/x4vrmod-launcher /usr/lib/x4vrmod/libVkLayer_X4VR_core.so \
    /usr/lib/x4vrmod/libx4vr_inject.so /usr/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json \
    /usr/share/x4vrmod/launch/x4vr-launch.sh /usr/share/x4vrmod/extension/x4vrmod/content.xml \
    /usr/share/x4vrmod/extension/x4vrmod/ext_01.cat /usr/share/x4vrmod/help/index.html \
    /usr/share/applications/x4vrmod-launcher.desktop; do
    [ -e "$f" ] || { echo "missing: $f"; exit 1; }
done
if ldd /usr/bin/x4vrmod-launcher | grep 'not found'; then
    exit 1
fi
x4vrmod version
out=$(x4vrmod doctor --no-vr 2>&1 || true)
echo "$out"
grep -q '(package)' <<<"$out" || { echo "doctor doesn't know it's a package"; exit 1; }
