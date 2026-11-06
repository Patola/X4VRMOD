#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Builds the zip from an existing build tree:
#
#   tools/make-zip.sh [OUTDIR]       writes OUTDIR/x4vrmod-<version>.zip
#
# Unpacked, it holds the launcher with x4vrmod-data/ beside it, which is how the
# launcher knows it's the zip. x4vrmod-data/ mirrors the repo's launch/ and
# extension/, so x4vr-launch.sh finds them the same way it does in a source tree.
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
OUT="${1:-$ROOT}"
CLI="$BUILD/launcher/x4vrmod"
[ -x "$CLI" ] || { echo "make-zip: build first ($CLI)" >&2; exit 1; }
ver="$("$CLI" version)"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
S="$T/x4vrmod-$ver"
D="$S/x4vrmod-data"
mkdir -p "$D/lib" "$D/launch" "$D/extension"
cp "$CLI" "$S/"
[ -x "$BUILD/launcher/x4vrmod-launcher" ] && cp "$BUILD/launcher/x4vrmod-launcher" "$S/"
cp "$BUILD/layer/libVkLayer_X4VR_core.so" "$BUILD/injector/libx4vr_inject.so" "$D/lib/"
cp "$ROOT/layer/VK_LAYER_X4VR_core.json.in" "$D/lib/"
cp "$ROOT/launch/x4vr-launch.sh" "$ROOT/launch/x4vr-defaults.sh" \
   "$ROOT/launch/deploy-extensions.sh" "$ROOT/launch/defaults.conf" "$D/launch/"
cp -r "$BUILD/extension/x4vrmod" "$D/extension/"
[ -d "$ROOT/launcher/help" ] && cp -r "$ROOT/launcher/help" "$D/"
cp "$ROOT/LICENSE" "$ROOT/LICENSE.exception" "$ROOT/README.md" "$ROOT/docs/releases.md" "$S/"
printf 'version=%s\n' "$ver" > "$D/manifest"

mkdir -p "$OUT"
OUT="$(CDPATH= cd -- "$OUT" && pwd)"
rm -f "$OUT/x4vrmod-$ver.zip"
(cd "$T" && zip -qry "$OUT/x4vrmod-$ver.zip" "x4vrmod-$ver")
echo "$OUT/x4vrmod-$ver.zip"
