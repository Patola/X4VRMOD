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

# What X4 really does, which `sed` and `sh` don't: it asks whether config.xml exists before
# opening it, through the calls its binary imports, and it saves with fopen. A real first
# run (2026-11-06) was told no, never opened it, and rendered with none of the overrides;
# and its fopen save of content.xml reached the player's file.
cat > "$T/x4like.c" <<'SRC'
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* Imported as X4's binary imports them, so the dynamic linker binds them as it binds X4's.
 * dlvsym wouldn't do: it matches the version strictly and skips an interposer. */
__asm__(".symver x_xstat, __xstat@GLIBC_2.2.5");
__asm__(".symver x_xstat64, __xstat64@GLIBC_2.2.5");
__asm__(".symver x_lxstat, __lxstat@GLIBC_2.2.5");
extern int x_xstat(int, const char *, struct stat *);
extern int x_xstat64(int, const char *, struct stat *);
extern int x_lxstat(int, const char *, struct stat *);
int main(int c, char **v) {
    const char *op = v[1], *p = v[2];
    if (!strcmp(op, "access"))
        return access(p, F_OK) == 0 ? 0 : 1;
    if (!strcmp(op, "__xstat") || !strcmp(op, "__xstat64") || !strcmp(op, "__lxstat")) {
        int (*fn)(int, const char *, struct stat *) =
            !strcmp(op, "__xstat") ? x_xstat : !strcmp(op, "__xstat64") ? x_xstat64 : x_lxstat;
        struct stat st;
        if (fn(1, p, &st) != 0)
            return 1;
        printf("%lld\n", (long long)st.st_size);
        return 0;
    }
    if (!strcmp(op, "fopen-wb") || !strcmp(op, "fopen64-wb")) {
        FILE *f = !strcmp(op, "fopen-wb") ? fopen(p, "wb") : fopen64(p, "wb");
        if (!f)
            return 1;
        fputs(v[3], f);
        fclose(f);
        return 0;
    }
    return 2;
}
SRC
cc -o "$T/x4like" "$T/x4like.c" || { echo "no compiler"; exit 1; }
XL="$T/x4like"
A="$T/asked/EgoSoft/X4/333"
mkdir -p "$A"
for how in access __xstat __xstat64 __lxstat; do
    "$XL" "$how" "$A/config.xml" >/dev/null && bad "control: no mod, but $how finds it" \
        || ok "control: with no mod, $how finds no config.xml on a new profile"
done
for how in access __xstat __xstat64 __lxstat; do
    rm -f "$A/config-x4vrmod.xml"
    if run "$XL" "$how" "$A/config.xml" >/dev/null; then
        ok "new profile: X4's $how says config.xml is there"
    else
        bad "new profile: X4's $how says it isn't, so X4 would never open it"
    fi
    grep -qF "$scale" "$A/config-x4vrmod.xml" 2>/dev/null && ok "...the fork exists, at 0.6" || bad "...$how: no fork"
    [ ! -e "$A/config.xml" ] && ok "...and config.xml is still absent" || bad "...$how made a config.xml"
done
size=$(run "$XL" __xstat "$A/config.xml")
[ -n "$size" ] && [ "$size" = "$(stat -c %s "$A/config-x4vrmod.xml")" ] && ok "...its size is the fork's" || bad "...size '$size'"
out=$(run sed -n p "$A/config.xml")
grep -qF "$scale" <<<"$out" && ok "...and X4's next step, opening it, gets the config" || bad "open after asking: $out"

# It only ever lies about this one thing.
E="$T/existing/EgoSoft/X4/444"
mkdir -p "$E"
printf '<root>\n  <who>player</who>\n</root>\n' > "$E/config.xml"
size=$(run "$XL" __xstat "$E/config.xml")
[ -n "$size" ] && [ "$size" = "$(stat -c %s "$E/config.xml")" ] && ok "a real config.xml is answered as itself" || bad "existing: size '$size'"
[ ! -e "$E/config-x4vrmod.xml" ] && ok "...and asking doesn't fork it" || bad "asking forked it"
run "$XL" __xstat "$E/nothing.xml" >/dev/null && bad "another missing file in the profile exists?" || ok "another missing file stays missing"
run "$XL" access "$T/other/config.xml" && bad "a config.xml elsewhere exists?" || ok "a config.xml elsewhere stays missing"
for how in access __xstat __xstat64 __lxstat; do
    run "$XL" "$how" "$E" >/dev/null && ok "ordinary paths still answer ($how)" || bad "the real $how doesn't work through the hook"
done
Z="$T/noconfig/EgoSoft/X4/555"
mkdir -p "$Z"
if env X4VR_NO_CONFIG=1 X4VR_LOG="$T/log" LD_PRELOAD="$SO" "$XL" access "$Z/config.xml"; then
    bad "X4VR_NO_CONFIG=1: it was still answered"
else
    ok "X4VR_NO_CONFIG=1: left alone"
fi

# X4's save of content.xml, through fopen and fopen64.
C="$T/save/EgoSoft/X4/666"
mkdir -p "$C"
ext() { env X4VR_EXTENSIONS=1 X4VR_LOG="$T/log" LD_PRELOAD="$SO" "$@" 2>/dev/null; }
ext cat "$C/content.xml" >/dev/null
for how in fopen-wb fopen64-wb; do
    ext "$XL" "$how" "$C/content.xml" "<content><!-- $how --></content>"
    [ ! -e "$C/content.xml" ] && ok "$how: a save doesn't create the player's content.xml" || bad "$how: it reached content.xml"
    grep -q -- "$how" "$C/content-x4vrmod.xml" && ok "...it lands in the fork" || bad "...$how: not in the fork"
    rm -f "$C/content.xml"
done
printf '<content>\n  <extension id="mine" enabled="true"/>\n</content>\n' > "$C/content.xml"
cp "$C/content.xml" "$T/content-original"
ext "$XL" fopen-wb "$C/content.xml" "<content><!-- saved --></content>"
cmp -s "$C/content.xml" "$T/content-original" && ok "an existing content.xml stays byte-identical" || bad "content.xml was overwritten"
run "$XL" fopen-wb "$E/plain.xml" "plain"
grep -q plain "$E/plain.xml" && ok "other files save as usual" || bad "another file's save was lost"

# Something else called config.xml, missing, isn't ours to invent.
O="$T/other/thing"
mkdir -p "$O"
out=$(run sed -n p "$O/config.xml"); rc=$?
[ "$rc" != 0 ] && [ ! -e "$O/config-x4vrmod.xml" ] && ok "a missing config.xml elsewhere stays missing" \
    || bad "elsewhere: rc=$rc $out, $(ls "$O")"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
