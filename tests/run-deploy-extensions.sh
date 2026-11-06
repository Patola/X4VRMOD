#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# launch/deploy-extensions.sh against a fake X4 folder: x4vrmod deployed when
# missing or different and left alone when current; our ids until v0.9.8
# removed; every other extension -- including one whose name starts with ours
# -- left byte for byte. The negative control: a foreign directory named like
# a retired id plus a suffix must survive.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
G="$T/game"
mkdir -p "$G/extensions"
for d in x4vrmod_lookaround x4vrmod_nosway x4vrmod_headlook ws_1666869624 x4vrmod_lookaround_old; do
    mkdir -p "$G/extensions/$d/libraries"
    echo "<content id=\"$d\"/>" > "$G/extensions/$d/content.xml"
done
(cd "$G/extensions" && find ws_1666869624 x4vrmod_lookaround_old -type f -exec md5sum {} + | sort) > "$T/foreign"

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
# With no build's copy, what ships is the repo's.
deploy() { X4VR_BUILD="$T/nobuild" launch/deploy-extensions.sh "$G" 2>&1; }

out=$(deploy); rc=$?
[ "$rc" = 0 ] && ok "first deploy exits 0" || bad "first deploy exited $rc: $out"
diff -rq extension/x4vrmod "$G/extensions/x4vrmod" >/dev/null 2>&1 \
    && ok "x4vrmod deployed, identical to the repo's" || bad "x4vrmod not deployed as shipped"
[[ "$out" == *"x4vrmod DEPLOYED to $G/extensions/x4vrmod"* ]] \
    && ok "says DEPLOYED" || bad "no DEPLOYED line — got: $out"
for d in x4vrmod_lookaround x4vrmod_nosway x4vrmod_headlook; do
    [ ! -e "$G/extensions/$d" ] && [[ "$out" == *"$d REMOVED (retired after v0.9.8)"* ]] \
        && ok "retired $d removed, and said" || bad "retired $d not removed/said — got: $out"
done
cmp -s "$T/foreign" <(cd "$G/extensions" && find ws_1666869624 x4vrmod_lookaround_old -type f -exec md5sum {} + | sort) \
    && ok "foreign extensions untouched, incl. x4vrmod_lookaround_old (negative control)" \
    || bad "a foreign extension changed"

out=$(deploy); rc=$?
[ "$rc" = 0 ] && [[ "$out" == "deploy-extensions: x4vrmod already current" ]] \
    && ok "second deploy: already current, nothing else said" \
    || bad "second deploy not quiet (rc $rc) — got: $out"

echo stale >> "$G/extensions/x4vrmod/content.xml"
touch "$G/extensions/x4vrmod/libraries/leftover.xml"
out=$(deploy)
diff -rq extension/x4vrmod "$G/extensions/x4vrmod" >/dev/null 2>&1 && [[ "$out" == *DEPLOYED* ]] \
    && ok "a stale copy is redeployed and a file the repo does not ship removed" \
    || bad "stale copy not repaired — got: $out"

out=$(launch/deploy-extensions.sh "$G" --without x4vrmod 2>&1); rc=$?
[ "$rc" = 2 ] && ok "an extra argument is refused (exit 2), not ignored" \
    || bad "extra argument exit $rc — got: $out"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
