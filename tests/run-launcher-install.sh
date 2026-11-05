#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Install, uninstall and run, from a real zip, under a fake home and a fake game.
# The rule under test is what must NOT happen: nothing of the player's changes, in
# X4's folder or in X4's profile, with or without --all. Nothing here runs X4 or
# touches the real home.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
want() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

zip=$(tools/make-zip.sh "$T/out") || { echo "make-zip failed"; exit 1; }
ver=$(build/launcher/x4vrmod version)
want "the zip is named by the version" "[ '$zip' = '$T/out/x4vrmod-$ver.zip' ]"
(cd "$T" && unzip -q "$zip")
U="$T/x4vrmod-$ver"
want "the zip holds the launcher" "[ -x '$U/x4vrmod' ]"
want "the zip holds its data folder" "[ -f '$U/x4vrmod-data/manifest' ]"
want "the launch script keeps its exec bit" "[ -x '$U/x4vrmod-data/launch/x4vr-launch.sh' ]"
if unzip -l "$zip" | grep -qE '\.(cat|dat|xpl)$|x4-shadersrc|x4-ui'; then
    bad "the zip holds X4 content"
else
    ok "no X4 content in the zip"
fi

# A fake X4 that says what it was started with.
G="$T/game"
mkdir -p "$G/extensions/someone_elses_mod" "$G/extensions/x4vrmod_lookaround_old"
echo mine > "$G/extensions/someone_elses_mod/content.xml"
echo mine > "$G/extensions/x4vrmod_lookaround_old/content.xml"
mkdir -p "$G/extensions/x4vrmod_lookaround"   # a retired one of ours
echo old > "$G/extensions/x4vrmod_lookaround/content.xml"
echo 900 > "$G/version.dat"
cat > "$T/fake.c" <<'SRC'
#include <stdio.h>
#include <stdlib.h>
int main(int c, char **v) {
    const char *s = getenv("SteamAppId");
    const char *l = getenv("X4VR_LOG");
    printf("FAKE args=%d steam=%s x4vr_h=%s log=%s self=%s\n", c - 1, s ? s : "unset",
           getenv("X4VR_H"), l ? l : "unset", v[0]);
    return 0;
}
SRC
cc -o "$G/X4" "$T/fake.c" || { echo "no compiler"; exit 1; }

H="$T/home"
P="$H/.config/EgoSoft/X4/123"
mkdir -p "$P/save"
for f in config.xml content.xml inputmap.xml config.xml.prevanilla save/quicksave.xml.gz \
         uidata.xml; do
    echo "player's $f" > "$P/$f"
done
for f in config-x4vrmod.xml content-x4vrmod.xml inputmap-x4vrmod.xml \
         config-x4vrmod-mousesteeringline.was; do
    echo "fork $f" > "$P/$f"
done
snapshot() { (cd "$P" && find . -type f \( -name '*x4vrmod*' -prune -o -print \) | sort | xargs md5sum;
              cd "$G/extensions" && md5sum someone_elses_mod/content.xml x4vrmod_lookaround_old/content.xml; ); }
before=$(snapshot)

X="env HOME=$H XDG_CONFIG_HOME= XDG_STATE_HOME= X4VR_GAME= $U/x4vrmod"
mode=$($X doctor --game "$G" --no-vr 2>&1)
grep -q '(zip)' <<<"$mode" && ok "doctor knows it's the zip" || bad "not the zip: $mode"
grep -q '^block Vulkan layer not installed' <<<"$mode" && ok "before: the layer isn't installed" \
    || bad "layer row: $mode"
grep -q '^block Mod not installed' <<<"$mode" && ok "before: the mod isn't installed" \
    || bad "mod row: $mode"

out=$($X run --game "$G" 2>&1); rc=$?
[ "$rc" != 0 ] && grep -q "isn't installed" <<<"$out" && ok "run before install says so" \
    || bad "run before install: rc=$rc $out"

out=$($X install --game "$G" 2>&1); rc=$?
[ "$rc" = 0 ] && ok "install succeeds" || bad "install: $out"
grep -q 'modified' <<<"$out" && ok "install tells about modified saves" || bad "no disclosure: $out"
M="$H/.local/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json"
want "the layer is registered" "[ -f '$M' ]"
want "the manifest names the installed library" \
     "grep -qF '$H/.local/share/x4vrmod/x4vrmod-data/lib/libVkLayer_X4VR_core.so' '$M'"
want "no placeholder is left" "! grep -q '@' '$M'"
want "the extension is in X4's folder" "[ -f '$G/extensions/x4vrmod/content.xml' ]"
want "the retired extension is gone" "[ ! -e '$G/extensions/x4vrmod_lookaround' ]"
want "a record is kept" "[ -f '$H/.local/share/x4vrmod/installed.json' ]"
[ "$before" = "$(snapshot)" ] && ok "install: the player's files are untouched" \
    || bad "install changed the player's files"

mv "$U" "$T/moved"
I="env HOME=$H XDG_CONFIG_HOME= XDG_STATE_HOME= X4VR_GAME= $H/.local/share/x4vrmod/x4vrmod"
after=$($I doctor --game "$G" --no-vr 2>&1)
grep -q '^ok    Vulkan layer installed' <<<"$after" && ok "after: the layer, with the zip's folder moved away" \
    || bad "layer row: $after"
grep -q '^ok    Mod installed' <<<"$after" && ok "after: the mod" || bad "mod row: $after"
grep -qF "$H/.local/share/x4vrmod/x4vrmod run %command%" <<<"$($I launch-options)" \
    && ok "Steam's line uses the stable path" || bad "launch-options: $($I launch-options)"

# Settings reach the game; Steam's only for a copy that links it. Without gamescope
# or the preload, so this runs the fake X4 and nothing else.
$I config set eye 3072 >/dev/null
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_LOG= $I run --game "$G" 2>/dev/null)
grep -q 'steam=unset x4vr_h=3072 ' <<<"$out" \
    && ok "run: direct mode, the profile's eye, no Steam for a non-Steam X4" || bad "run: $out"
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_H=2048 X4VR_LOG= $I run --game "$G" 2>/dev/null)
grep -q 'x4vr_h=2048 ' <<<"$out" && ok "run: an explicit variable beats the profile" || bad "run: $out"
# The upscaler has to arrive, "none" included: the launch script and the injector
# take a set variable as the player's choice.
$I config set eye 4224 >/dev/null
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_LOG= $I run --game "$G" 2>&1 | grep 'x4vr-launch: .* setting')
grep -q 'eye 4224, upscaler fsr_quality' <<<"$out" && ok "run: above 3200 the rule's upscaler reaches the script" || bad "rule: $out"
$I config set upscaler none >/dev/null
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_LOG= $I run --game "$G" 2>&1 | grep 'x4vr-launch: .* setting')
grep -q 'eye 4224, upscaler none' <<<"$out" && ok "run: unselected, it stays off" || bad "unselected: $out"
$I config set eye 3072 >/dev/null
# Two copies: what's asked for must be what runs, whatever the environment says.
cp -r "$G" "$T/game2"
E="env HOME=$H XDG_CONFIG_HOME= XDG_STATE_HOME= X4VR_GAME=$T/game2/X4 $H/.local/share/x4vrmod/x4vrmod"
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_LOG= $E run --game "$G" 2>/dev/null)
grep -qF "self=$G/X4" <<<"$out" && ok "run: --game beats X4VR_GAME" || bad "--game: $out"
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_LOG= $E run 2>/dev/null)
grep -qF "self=$T/game2/X4" <<<"$out" && ok "run: X4VR_GAME is used when nothing is asked for" || bad "env: $out"
out=$(env X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 X4VR_LOG= $I run "$G/X4" a b 2>/dev/null)
grep -q '^FAKE args=2 ' <<<"$out" && ok "run: wrapper mode passes %command% through" || bad "wrapper: $out"
# Nothing writes the log with the layer and injector off, so ask what X4 was told.
out=$(env -u X4VR_LOG X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 $I run --game "$G" 2>/dev/null)
grep -qE "log=$H/\.local/state/x4vrmod/logs/x4vrmod-[0-9]{8}-[0-9]{6}\.log " <<<"$out" \
    && ok "run: a log of its own, under the state folder" || bad "run: $out"
out=$(env X4VR_LOG=/tmp/mine.log X4VR_GAMESCOPE=0 X4VR_NO_INJECT=1 X4VR_NO_LAYER=1 $I run --game "$G" 2>/dev/null)
grep -q 'log=/tmp/mine.log ' <<<"$out" && ok "run: an explicit X4VR_LOG is kept" || bad "run: $out"

# The shipping path itself, up to gamescope: everything a real run is given, with
# no switch turned off. The stub stands in for gamescope, so nothing starts.
mkdir -p "$T/bin"
cat > "$T/bin/gamescope" <<'SH'
#!/bin/sh
echo "GS args=$*"
echo "GS preload=$LD_PRELOAD"
echo "GS addpath=$VK_ADD_LAYER_PATH"
echo "GS layers=$VK_INSTANCE_LAYERS"
echo "GS game=$X4VR_GAME"
SH
chmod +x "$T/bin/gamescope"
out=$(env -u X4VR_GAMESCOPE PATH="$T/bin:$PATH" X4VR_LOG= $I run --game "$G" 2>&1); rc=$?
[ "$rc" = 0 ] && grep -q '^GS args=' <<<"$out" && ok "shipping path: reaches gamescope" || bad "shipping: rc=$rc $out"
grep -qF "GS preload=$H/.local/share/x4vrmod/x4vrmod-data/lib/libx4vr_inject.so" <<<"$out" \
    && ok "shipping path: preloads the installed injector" || bad "preload: $out"
grep -qF "GS addpath=$H/.local/share/vulkan/explicit_layer.d" <<<"$out" \
    && ok "shipping path: the installed layer's folder" || bad "addpath: $out"
grep -q '^GS layers=VK_LAYER_X4VR_core' <<<"$out" && ok "shipping path: the layer is enabled" || bad "layers: $out"
grep -qF "GS game=$G/X4" <<<"$out" && ok "shipping path: the chosen X4" || bad "game: $out"
grep -qE "No such file|not found|command not found" <<<"$out" \
    && bad "shipping path: something is missing: $(grep -E 'No such file|not found' <<<"$out")" \
    || ok "shipping path: nothing missing from the zip"

# The record can't be used to delete something else.
victim="$T/important"
echo keep > "$victim"
sed -i "s|\"installed\": \[|\"installed\": [\n    {\"path\": \"$victim\"},|" "$H/.local/share/x4vrmod/installed.json"

dry=$($I uninstall --dry-run 2>&1)
want "dry run: the extension is listed" "grep -qF '$G/extensions/x4vrmod' <<<\"\$dry\""
grep -qF "$victim" <<<"$dry" && bad "a tampered record is obeyed" || ok "a tampered record is ignored"
want "dry run: nothing removed" "[ -f '$M' ] && [ -d '$G/extensions/x4vrmod' ]"

out=$(echo n | $I uninstall 2>&1); rc=$?
[ "$rc" = 1 ] && [ -f "$M" ] && ok "answering no removes nothing" || bad "answered no: rc=$rc"

out=$($I uninstall --yes 2>&1); rc=$?
[ "$rc" = 0 ] && ok "uninstall succeeds" || bad "uninstall: $out"
want "the layer's manifest is gone" "[ ! -e '$M' ]"
want "the installed copy is gone" "[ ! -e '$H/.local/share/x4vrmod' ]"
want "the extension is gone" "[ ! -e '$G/extensions/x4vrmod' ]"
want "the record's other path survives" "[ -f '$victim' ]"
for f in config-x4vrmod.xml content-x4vrmod.xml inputmap-x4vrmod.xml config-x4vrmod-mousesteeringline.was; do
    want "without --all, $f stays" "[ -f '$P/$f' ]"
done
want "without --all, the profile stays" "[ -f '$H/.config/x4vrmod/profile.toml' ]"
[ "$before" = "$(snapshot)" ] && ok "uninstall: the player's files are untouched" \
    || bad "uninstall changed the player's files"

# Again, from the moved zip, then everything.
Z="env HOME=$H XDG_CONFIG_HOME= XDG_STATE_HOME= X4VR_GAME= $T/moved/x4vrmod"
$Z install --game "$G" >/dev/null 2>&1 && ok "installs again from a moved folder" || bad "reinstall"
list=$($Z uninstall --all --dry-run 2>&1)
grep -q "can't be undone" <<<"$list" && ok "--all warns it can't be undone" || bad "no warning: $list"
# Another player's number, XDG_CONFIG_HOME, and no folder for the player at all.
XD="$T/xdg/EgoSoft/X4"
mkdir -p "$XD/7654321" "$P/../gogname"
echo fork > "$XD/7654321/config-x4vrmod.xml"
echo mine > "$XD/7654321/config.xml"
echo fork > "$XD/content-x4vrmod.xml"
echo fork > "$P/../gogname/inputmap-x4vrmod.xml"
echo mine > "$P/../gogname/inputmap.xml"
list=$(env XDG_CONFIG_HOME="$T/xdg" HOME="$H" XDG_STATE_HOME= X4VR_GAME= "$T/moved/x4vrmod" uninstall --all --dry-run 2>&1)
for f in "$XD/7654321/config-x4vrmod.xml" "$XD/content-x4vrmod.xml"; do
    grep -qF "$f" <<<"$list" && ok "--all finds $(basename "$f") under XDG_CONFIG_HOME" || bad "not listed: $f"
done
grep -qF "gogname/inputmap-x4vrmod.xml" <<<"$list" && ok "--all finds it under any folder name" || bad "gogname: $list"
env XDG_CONFIG_HOME="$T/xdg" HOME="$H" XDG_STATE_HOME= X4VR_GAME= "$T/moved/x4vrmod" uninstall --all --yes >/dev/null 2>&1
want "--all removes XDG_CONFIG_HOME's forks" "[ ! -e '$XD/7654321/config-x4vrmod.xml' ] && [ ! -e '$XD/content-x4vrmod.xml' ]"
want "...and the player's own there stay" "[ -f '$XD/7654321/config.xml' ] && [ -f '$P/../gogname/inputmap.xml' ]"
$Z install --game "$G" >/dev/null 2>&1
for f in config-x4vrmod.xml content-x4vrmod.xml inputmap-x4vrmod.xml config-x4vrmod-mousesteeringline.was; do
    echo "fork $f" > "$P/$f"
done
out=$($Z uninstall --all --yes 2>&1); rc=$?
[ "$rc" = 0 ] && ok "uninstall --all succeeds" || bad "uninstall --all: $out"
for f in config-x4vrmod.xml content-x4vrmod.xml inputmap-x4vrmod.xml config-x4vrmod-mousesteeringline.was; do
    want "--all removes $f" "[ ! -e '$P/$f' ]"
done
want "--all removes the launcher's profile" "[ ! -e '$H/.config/x4vrmod/profile.toml' ]"
[ "$before" = "$(snapshot)" ] && ok "uninstall --all: the player's files are untouched" \
    || bad "uninstall --all changed the player's files: $(diff <(echo "$before") <(snapshot))"
want "someone else's extension survives" "[ -f '$G/extensions/someone_elses_mod/content.xml' ]"
want "a name that only starts like ours survives" "[ -f '$G/extensions/x4vrmod_lookaround_old/content.xml' ]"
out=$($Z uninstall --yes 2>&1)
grep -q "Nothing of the mod's" <<<"$out" && ok "uninstalling twice is harmless" || bad "twice: $out"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
