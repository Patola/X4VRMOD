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

# GOG's build, where the code is elsewhere: every feature fits, and the checks stay as strict.
GOGX=$("$CLI" locate --all | awk -F'\t' '$1 == "GOG" && $2 == "linux" { print $3; exit }')
if [ -z "$GOGX" ]; then
    echo "skip  no GOG copy of X4: the other build's cases are unchecked"
else
    out=$("$CLI" probe x4 "$GOGX"); rc=$?
    [ "$rc" = 0 ] && [ "$(grep -c '^ok ' <<<"$out")" = 8 ] && ok "GOG: all 8 features fit" || bad "GOG: $out"
    "$CLI" probe x4 "$GOGX" --map | grep -q '^found [0-9]* missing 0$' && ok "GOG: every block is found" || bad "GOG: a block is missing"
    "$CLI" probe x4 "$X4" --map | awk '$1 == "code" || $1 == "data" { if ($3 != 0) m = 1 } END { exit m }' \
        && ok "Steam: nothing is moved" || bad "Steam: something moved"
    H="$T/gog"
    mkdir -p "$H"
    cp "$(dirname "$GOGX")/version.dat" "$H/"
    # Flips one byte of a copy, at Steam's address VA plus what the map says that block moved.
    flip() { # flip <steam address> <offset>
        cp "$GOGX" "$H/X4"
        "$CLI" probe x4 "$GOGX" --map > "$H/map"
        python3 - "$H/X4" "$H/map" "$1" "$2" <<'PY'
import struct, sys
path, mapf, steam, extra = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4])
blocks = sorted((int(l.split()[1], 16), int(l.split()[2])) for l in open(mapf) if l.startswith("code"))
delta = [d for lo, d in blocks if lo <= steam][-1]
va_new = steam + delta + extra
with open(path, "r+b") as f:
    h = f.read(64)
    phoff = struct.unpack_from("<Q", h, 0x20)[0]
    ent, num = struct.unpack_from("<HH", h, 0x36)
    for i in range(num):
        f.seek(phoff + i * ent)
        p = f.read(56)
        t = struct.unpack_from("<I", p, 0)[0]
        off, va = struct.unpack_from("<QQ", p, 8)
        fsz = struct.unpack_from("<Q", p, 32)[0]
        if t == 1 and va <= va_new < va + fsz:
            f.seek(off + va_new - va)
            b = f.read(1)
            f.seek(off + va_new - va)
            f.write(bytes([b[0] ^ 0xff]))
            break
PY
    }
    # An instruction's opcode: a real difference.
    flip 0x1933dd4 0
    out=$("$CLI" probe x4 "$H")
    grep -q '^differs  headlook' <<<"$out" && [ "$(grep -c '^differs' <<<"$out")" = 1 ] \
        && ok "GOG, one opcode changed: head-look off, nothing else" || bad "GOG opcode: $out"
    # The address inside that instruction: it no longer points at the same thing.
    flip 0x1933dd4 4
    out=$("$CLI" probe x4 "$H")
    grep -q '^differs  headlook' <<<"$out" && [ "$(grep -c '^differs' <<<"$out")" = 1 ] \
        && ok "GOG, an address aimed elsewhere: head-look off, nothing else" || bad "GOG address: $out"
fi

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
