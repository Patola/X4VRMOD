#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The launcher's compatibility answer, end to end through the real CLI, on a temp
# copy of the real X4 (never committed; skipped where there is none). One flipped
# byte in head-look's code must turn exactly that feature off and make Play amber;
# a different version.dat must say "not tested"; an unchanged copy must stay green.
# No checksum is involved: a copy is a different file only by where it lives.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1
CLI=build/launcher/x4vrmod
[ -x "$CLI" ] || { echo "build first"; exit 1; }

X4=$("$CLI" probe x4 2>/dev/null | sed -n 's/^program  //p')
if [ -z "$X4" ] || ! "$CLI" probe x4 >/dev/null 2>&1; then
    echo "skip  no analysed X4 here"
    exit 0
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

G="$T/game"
mkdir -p "$G/extensions"
cp "$X4" "$G/X4" || { echo "skip  no room for a copy of X4"; exit 0; }
printf '900\n' > "$G/version.dat"
cp -r extension/x4vrmod "$G/extensions/x4vrmod"

out=$("$CLI" probe x4 "$G"); rc=$?
[ "$rc" = 0 ] && ok "an identical copy: every feature fits" || bad "an identical copy failed: $out"
out=$("$CLI" doctor --game "$G" --no-vr)
grep -q '^ok    The mod fits this X4' <<<"$out" && ok "an identical copy: doctor agrees" \
    || bad "doctor on the copy: $out"

# Head-look's first site, through the file's own LOAD headers.
python3 - "$G/X4" <<'PY'
import struct, sys
VA = 0x1933dd4
with open(sys.argv[1], "r+b") as f:
    h = f.read(64)
    phoff = struct.unpack_from("<Q", h, 0x20)[0]
    ent, num = struct.unpack_from("<HH", h, 0x36)
    for i in range(num):
        f.seek(phoff + i * ent)
        p = f.read(56)
        t = struct.unpack_from("<I", p, 0)[0]
        off, va = struct.unpack_from("<QQ", p, 8)
        fsz = struct.unpack_from("<Q", p, 32)[0]
        if t == 1 and va <= VA < va + fsz:
            f.seek(off + VA - va)
            b = f.read(1)
            f.seek(off + VA - va)
            f.write(bytes([b[0] ^ 0xff]))
            break
    else:
        sys.exit("no segment holds the site")
PY
out=$("$CLI" probe x4 "$G"); rc=$?
[ "$rc" = 1 ] && ok "one flipped byte: probe x4 exits 1" || bad "probe x4 exited $rc"
grep -q '^differs  headlook' <<<"$out" && ok "one flipped byte: head-look differs" \
    || bad "head-look not reported: $out"
[ "$(grep -c '^differs' <<<"$out")" = 1 ] && ok "one flipped byte: nothing else does" \
    || bad "more than head-look differs: $out"
out=$("$CLI" doctor --game "$G" --no-vr); rc=$?
grep -q '^warn  X4 build not supported yet: These turn off: Head tracking in the cockpit\.$' <<<"$out" \
    && ok "doctor: amber, naming the feature" || bad "doctor's row: $out"
[ "$rc" != 0 ] && ok "doctor: not green" || bad "doctor stayed green"

# Put the byte back; only the version differs now.
cp "$X4" "$G/X4"
printf '1000\n' > "$G/version.dat"
out=$("$CLI" doctor --game "$G" --no-vr)
grep -q '^warn  Not tested with X4 10\.00' <<<"$out" && ok "a new major version: not tested" \
    || bad "version row: $out"
printf '905\n' > "$G/version.dat"
out=$("$CLI" doctor --game "$G" --no-vr)
grep -q '^ok    The mod fits this X4' <<<"$out" \
    && ok "a hotfix that keeps the code stays green (no checksum)" || bad "hotfix row: $out"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
