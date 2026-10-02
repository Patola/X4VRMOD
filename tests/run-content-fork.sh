#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The content.xml fork, driven through the REAL injector without X4: ordinary
# tools read a fake profile with libx4vr_inject.so preloaded. `cat` reads with
# the open() family, `sed` with fopen -- the two paths X4 might use, since nothing on
# record says which. The player's file must end byte-identical whatever
# happens; that is the non-intrusive rule this fork exists to keep.
#
# (This header first said `cat` reads with openat. It uses open() here; the
# injector's log said so and the test's first draft was wrong, not the hook.)
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1

SO=$(find build -name libx4vr_inject.so | head -1)
[ -n "$SO" ] || { echo "build first"; exit 1; }
SO=$PWD/$SO

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PROF="$T/home/.config/EgoSoft/X4/123"
mkdir -p "$PROF" "$T/game/extensions/x4vrmod_lookaround"
cat > "$PROF/content.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<content>
  <extension id="ws_1666869624" enabled="true"/>
  <extension id="x4vrmod_lookaround" enabled="false"/>
</content>
EOF
cp "$PROF/content.xml" "$T/original"
MANI="$T/game/extensions/x4vrmod_lookaround/content.xml"
echo '<content id="x4vrmod_lookaround" name="n"/>' > "$MANI"
cp "$MANI" "$T/manifest-original"

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
run() { env X4VR_LOG="$T/log" LD_PRELOAD="$SO" "$@" 2>/dev/null; }

on='<extension id="x4vrmod_lookaround" enabled="true"/>'
off='<extension id="x4vrmod_lookaround" enabled="false"/>'

# Knob off: nothing changes, nothing is forked.
out=$(run cat "$PROF/content.xml")
[[ "$out" == *"$off"* ]] && ok "knob off: X4 would see the player's own setting" \
    || bad "knob off: the file was altered"
[ ! -e "$PROF/content-x4vrmod.xml" ] && ok "knob off: no fork created" \
    || bad "knob off: a fork was created"

export X4VR_EXTENSIONS=1
out=$(run cat "$PROF/content.xml")
[[ "$out" == *"$on"* ]] && ok "open family (cat): our extension served ENABLED" \
    || bad "open family (cat): not enabled — got: $out"
[[ "$out" == *'<extension id="ws_1666869624" enabled="true"/>'* ]] \
    && ok "open family (cat): the other extension is served as it was" \
    || bad "open family (cat): the other extension changed"
out=$(run sed -n p "$PROF/content.xml")
[[ "$out" == *"$on"* ]] && ok "fopen (sed): our extension served ENABLED" \
    || bad "fopen (sed): not enabled — got: $out"
grep -q -E 'content: serving .* via (open|open64|openat|openat64) ' "$T/log" \
    && ok "log names the open-family path ($(grep -o -E 'via (open|open64|openat|openat64) ' "$T/log" | head -1))" \
    || bad "log does not name an open-family path"
grep -q 'content: serving .* via fopen' "$T/log" && ok "log names the fopen path" \
    || bad "log does not name the fopen path"

cmp -s "$PROF/content.xml" "$T/original" && ok "player's content.xml byte-identical after reads" \
    || bad "player's content.xml was MODIFIED by a read"
cmp -s "$PROF/content-x4vrmod.xml" "$T/original" \
    && ok "fork seeded with the player's file (patch is in memory only)" \
    || bad "fork is not a verbatim seed"

# A write (X4 toggling extensions in its menu) lands in the fork.
run sh -c "printf '<content>written</content>\n' > '$PROF/content.xml'"
cmp -s "$PROF/content.xml" "$T/original" && ok "a write does NOT reach the player's file" \
    || bad "a write reached the player's content.xml"
grep -q 'written' "$PROF/content-x4vrmod.xml" && ok "the write went into the fork" \
    || bad "the write went nowhere visible"

# x4vrmod_nosway rides with X4VR_EXTENSIONS like the look-around limits.
nson='<extension id="x4vrmod_nosway" enabled="true"/>'
out=$(run cat "$PROF/content.xml")
[[ "$out" == *"$nson"* ]] && ok "x4vrmod_nosway served ON with the extensions" \
    || bad "x4vrmod_nosway not served on — got: $out"

# x4vrmod_headlook rides with X4VR_HEADLOOK_WRITE only, and is forced OFF
# otherwise -- including when an earlier write session left it on in the fork.
hon='<extension id="x4vrmod_headlook" enabled="true"/>'
hoff='<extension id="x4vrmod_headlook" enabled="false"/>'
out=$(run cat "$PROF/content.xml")
[[ "$out" == *"$hoff"* ]] && ok "no write mode: x4vrmod_headlook served OFF" \
    || bad "no write mode: x4vrmod_headlook not served off — got: $out"
out=$(X4VR_HEADLOOK_WRITE=1 run cat "$PROF/content.xml")
[[ "$out" == *"$hon"* && "$out" == *"$on"* ]] \
    && ok "write mode: x4vrmod_headlook AND x4vrmod_lookaround served ON" \
    || bad "write mode: not both on — got: $out"
printf '<content>\n  %s\n  %s\n</content>\n' "$on" "$hon" > "$PROF/content-x4vrmod.xml"
out=$(run cat "$PROF/content.xml")
[[ "$out" == *"$hoff"* ]] \
    && ok "a fork left with headlook ON by a write session is served OFF without it" \
    || bad "headlook leaked into a non-write session — got: $out"
cmp -s "$PROF/content.xml" "$T/original" && ok "player's content.xml still byte-identical" \
    || bad "player's content.xml was MODIFIED"

# An extension's own manifest is never claimed.
out=$(run cat "$MANI")
cmp -s <(printf '%s\n' "$out") "$T/manifest-original" \
    && ok "an extension's own content.xml passes through untouched" \
    || bad "an extension manifest was rewritten"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
