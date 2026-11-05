#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# config-x4vrmod.xml's creation, driven through the REAL injector without X4: ordinary
# tools read a fake profile with libx4vr_inject.so preloaded. `sed` reads with fopen, as
# X4 reads config.xml (the logs say so, and only that path is hooked for it); `cat` reads
# content.xml by the open family. The font scale is chosen at creation and never again; a
# brand-new profile still gets a fork; and the player's own files stay as they were, or
# absent.
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
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
run() { env X4VR_LOG="$T/log" LD_PRELOAD="$SO" "$@" 2>/dev/null; }
scale='<uiscale>0.6</uiscale>'

# A player's config.xml with X4's own value.
P="$T/a/EgoSoft/X4/111"
mkdir -p "$P"
printf '<?xml version="1.0"?>\n<root>\n  <uiscale>1.0</uiscale>\n  <other>keep</other>\n</root>\n' > "$P/config.xml"
cp "$P/config.xml" "$T/original"
out=$(run sed -n p "$P/config.xml")
grep -qF "$scale" <<<"$out" && ok "from 1.0: X4 is served 0.6" || bad "from 1.0: $out"
grep -qF "$scale" "$P/config-x4vrmod.xml" && ok "...and the fork starts at 0.6" || bad "the fork: $(cat "$P/config-x4vrmod.xml")"
grep -qF '<other>keep</other>' "$P/config-x4vrmod.xml" && ok "...with the player's other settings" || bad "other settings lost"
cmp -s "$P/config.xml" "$T/original" && ok "...and config.xml is byte-identical" || bad "config.xml changed"

# A later choice in X4's options stays: the fork isn't touched again.
sed -i 's#<uiscale>0.6</uiscale>#<uiscale>0.8</uiscale>#' "$P/config-x4vrmod.xml"
out=$(run sed -n p "$P/config.xml")
grep -qF '<uiscale>0.8</uiscale>' <<<"$out" && ok "an existing fork's own value is served (fopen)" || bad "existing fork: $out"
grep -qF '<uiscale>0.8</uiscale>' "$P/config-x4vrmod.xml" && ok "...and kept" || bad "the fork was changed"

# A config.xml with no such setting, as X4 leaves out defaults.
P2="$T/b/EgoSoft/X4/222"
mkdir -p "$P2"
printf '<root>\n  <other>keep</other>\n</root>\n' > "$P2/config.xml"
run sed -n p "$P2/config.xml" >/dev/null
grep -qF "$scale" "$P2/config-x4vrmod.xml" && ok "no setting at all: it's added" || bad "not added"

# A brand-new profile: never started, so no files at all.
N="$T/new/EgoSoft/X4/anything"
mkdir -p "$N"
out=$(run sed -n p "$N/config.xml")
grep -qF "$scale" <<<"$out" && ok "new profile: X4 is served a config, at 0.6" || bad "new profile: $out"
[ -f "$N/config-x4vrmod.xml" ] && ok "...and the fork is created" || bad "no fork"
[ ! -e "$N/config.xml" ] && ok "...and config.xml is still absent" || bad "config.xml was created"
run sh -c "echo saved-by-x4 > '$N/config.xml'"
[ ! -e "$N/config.xml" ] && ok "X4's first save doesn't create config.xml" || bad "a save reached config.xml"
grep -q saved-by-x4 "$N/config-x4vrmod.xml" && ok "...it lands in the fork" || bad "the save was lost"

out=$(env X4VR_EXTENSIONS=1 X4VR_LOG="$T/log" LD_PRELOAD="$SO" cat "$N/content.xml" 2>/dev/null)
grep -qF '<extension id="x4vrmod" enabled="true"/>' <<<"$out" && ok "new profile: our extension is on" || bad "content: $out"
[ ! -e "$N/content.xml" ] && ok "...and content.xml is still absent" || bad "content.xml was created"
env X4VR_EXTENSIONS=1 X4VR_LOG="$T/log" LD_PRELOAD="$SO" sh -c "echo saved > '$N/content.xml'" 2>/dev/null
[ ! -e "$N/content.xml" ] && grep -q saved "$N/content-x4vrmod.xml" \
    && ok "...and X4's save lands in the fork" || bad "content save"

out=$(env X4VR_HEADLOOK=1 X4VR_LOG="$T/log" LD_PRELOAD="$SO" sed -n p "$N/inputmap.xml" 2>/dev/null); rc=$?
[ "$rc" != 0 ] && [ -z "$out" ] && ok "new profile: no inputmap yet, so X4 keeps its defaults" || bad "inputmap read: $out"
[ ! -e "$N/inputmap-x4vrmod.xml" ] && ok "...and nothing is made up on disk" || bad "an inputmap fork appeared"
env X4VR_HEADLOOK=1 X4VR_LOG="$T/log" LD_PRELOAD="$SO" sh -c "echo saved > '$N/inputmap.xml'" 2>/dev/null
[ ! -e "$N/inputmap.xml" ] && grep -q saved "$N/inputmap-x4vrmod.xml" \
    && ok "...and X4's save lands in the fork" || bad "inputmap save"

# Both stores on one machine: Steam keeps a player's files in a numbered folder, and GOG
# in X4's folder itself (seen 2026-11-05). Each gets its own forks, beside its own files.
B="$T/both/EgoSoft/X4"
mkdir -p "$B/96679891"
printf '<root>\n  <who>gog</who>\n</root>\n' > "$B/config.xml"
printf '<root>\n  <who>steam</who>\n  <uiscale>0.8</uiscale>\n</root>\n' > "$B/96679891/config.xml"
printf '<root>\n  <who>steam</who>\n  <uiscale>0.7</uiscale>\n</root>\n' > "$B/96679891/config-x4vrmod.xml"
cp "$B/96679891/config-x4vrmod.xml" "$T/steam-fork"
cp "$B/config.xml" "$T/gog-config"
out=$(run sed -n p "$B/config.xml")
grep -q '<who>gog</who>' <<<"$out" && grep -qF "$scale" <<<"$out" && ok "GOG: served its own config, at 0.6" || bad "GOG: $out"
grep -q '<who>gog</who>' "$B/config-x4vrmod.xml" && ok "GOG: its fork is beside its config.xml" || bad "GOG's fork"
cmp -s "$B/config.xml" "$T/gog-config" && ok "GOG: config.xml is byte-identical" || bad "GOG's config.xml changed"
cmp -s "$B/96679891/config-x4vrmod.xml" "$T/steam-fork" && ok "...and Steam's fork isn't touched" || bad "Steam's fork changed"
out=$(run sed -n p "$B/96679891/config.xml")
grep -q '<who>steam</who>' <<<"$out" && grep -q '<uiscale>0.7</uiscale>' <<<"$out" \
    && ok "Steam: served its own fork, with its own scale" || bad "Steam: $out"
grep -q '<who>gog</who>' "$B/config-x4vrmod.xml" && ok "...and GOG's fork is still GOG's" || bad "GOG's fork changed"
run sh -c "echo saved-by-gog > '$B/config.xml'"
grep -q saved-by-gog "$B/config-x4vrmod.xml" && cmp -s "$B/96679891/config-x4vrmod.xml" "$T/steam-fork" \
    && cmp -s "$B/config.xml" "$T/gog-config" && ok "GOG's save lands in GOG's fork alone" || bad "GOG's save"

# Something else called config.xml, missing, isn't ours to invent.
O="$T/other/thing"
mkdir -p "$O"
out=$(run sed -n p "$O/config.xml"); rc=$?
[ "$rc" != 0 ] && [ ! -e "$O/config-x4vrmod.xml" ] && ok "a missing config.xml elsewhere stays missing" \
    || bad "elsewhere: rc=$rc $out, $(ls "$O")"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
