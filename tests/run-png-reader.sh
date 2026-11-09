#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# common/x4vr_png.hpp's reader against Pillow, on the pictures the layer embeds
# (launcher/art): the same size and the same RGBA bytes, by CRC-32. Pillow is the
# oracle because someone else wrote it (tests/png_reader.cpp says why).
set -uo pipefail
CDPATH=
cd "$(dirname "$0")/.." || exit 1
BUILD="${X4VR_BUILD:-$PWD/build}"
files=(launcher/art/*.png)
want=$(python3 - "${files[@]}" <<'EOF'
import sys, zlib
from PIL import Image
for f in sys.argv[1:]:
    im = Image.open(f).convert("RGBA")
    print(f"{f} {im.width} {im.height} {zlib.crc32(im.tobytes()):08x}")
EOF
) || { echo "FAIL Pillow couldn't read the pictures"; exit 1; }
got=$("$BUILD/tests/x4vr_test_png_reader" "${files[@]}") || { echo "FAIL the reader:"; echo "$got"; exit 1; }
fail=0
while read -r f w h c; do
    line=$(grep -F "$f " <<<"$got" | head -1)
    if [ "$line" = "$f $w $h $c" ]; then
        echo "ok   $f: ${w}x${h}, the same bytes as Pillow's"
    else
        echo "FAIL $f: Pillow ${w}x${h} $c, ours: ${line:-nothing}"
        fail=1
    fi
done <<<"$want"
grep -E 'round trip|refused' <<<"$got" | sed 's/^/     /'
grep -qE 'FAIL' <<<"$got" && fail=1
exit $fail
