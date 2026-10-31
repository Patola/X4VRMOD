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
mkdir -p "$PROF" "$T/game/extensions/x4vrmod"
cat > "$PROF/content.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<content>
  <extension id="ws_1666869624" enabled="true"/>
  <extension id="x4vrmod" enabled="false"/>
</content>
EOF
cp "$PROF/content.xml" "$T/original"
MANI="$T/game/extensions/x4vrmod/content.xml"
echo '<content id="x4vrmod" name="n"/>' > "$MANI"
cp "$MANI" "$T/manifest-original"

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
run() { env X4VR_LOG="$T/log" LD_PRELOAD="$SO" "$@" 2>/dev/null; }

on='<extension id="x4vrmod" enabled="true"/>'
off='<extension id="x4vrmod" enabled="false"/>'

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

# Our ids until v0.9.8 (merged into x4vrmod; headlook dropped): never added
# to a fork that does not list them...
out=$(run cat "$PROF/content.xml")
[[ "$out" != *x4vrmod_* ]] && ok "retired ids: not added to a fork without them" \
    || bad "retired ids were added — got: $out"
out=$(X4VR_HEADLOOK_WRITE=1 run cat "$PROF/content.xml")
[[ "$out" == *"$on"* && "$out" != *x4vrmod_* ]] \
    && ok "write mode: x4vrmod ON, nothing else of ours" \
    || bad "write mode: not x4vrmod alone — got: $out"
# ...and served OFF when an earlier session left them in the fork, ON or OFF.
printf '<content>\n  %s\n  %s\n  %s\n  %s\n</content>\n' \
    '<extension id="x4vrmod_lookaround" enabled="true"/>' \
    '<extension id="x4vrmod_nosway" enabled="true"/>' \
    '<extension id="x4vrmod_headlook" enabled="false"/>' \
    '<extension id="x4vrmod_lookaround_old" enabled="true"/>' > "$PROF/content-x4vrmod.xml"
: > "$T/log"
for spd in "" 1; do
    out=$(X4VR_HEADLOOK_WRITE=1 X4VR_HEADLOOK_SPEED_EXT=$spd run cat "$PROF/content.xml")
    [[ "$out" == *'<extension id="x4vrmod_lookaround" enabled="false"/>'* &&
       "$out" == *'<extension id="x4vrmod_nosway" enabled="false"/>'* &&
       "$out" == *'<extension id="x4vrmod_headlook" enabled="false"/>'* &&
       "$out" == *"$on"* ]] \
        && ok "retired ids left in the fork: served OFF, x4vrmod ON (write mode, X4VR_HEADLOOK_SPEED_EXT='$spd')" \
        || bad "retired ids not all served off (X4VR_HEADLOOK_SPEED_EXT='$spd') — got: $out"
done
[[ "$out" == *'<extension id="x4vrmod_lookaround_old" enabled="true"/>'* ]] \
    && ok "an id that merely starts with a retired one is left alone" \
    || bad "a foreign id sharing our prefix was changed — got: $out"
grep -q 'x4vrmod ENABLED (added), x4vrmod_lookaround retired, DISABLED (was on), x4vrmod_nosway retired, DISABLED (was on), x4vrmod_headlook retired, already disabled;' "$T/log" \
    && ok "log names each retired id and what was done" \
    || bad "log does not name the retired ids — got: $(grep -m1 'content: serving' "$T/log")"
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
