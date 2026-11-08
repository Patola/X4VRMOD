#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The launcher's words, collected from the code for translators:
#
#   tools/i18n-update.sh          writes launcher/po/x4vrmod.pot and merges it into
#                                 every launcher/po/<lang>.po
#   tools/i18n-update.sh --check  changes nothing; fails if the .pot is behind the
#                                 code (tests/run-i18n.sh)
#
# What's collected is what the code marks: tr_, trn_ and N_ in launcher/core,
# qtr and qtrn in launcher/gui (launcher/core/x4vr_i18n.hpp). Each is a Qt-style
# template, %1..%9, which msgfmt --check holds every translation to. Locations
# are files without line numbers, so moving code doesn't change the .pot.
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PO="$ROOT/launcher/po"
POT="$PO/x4vrmod.pot"
check=0
[ "${1:-}" = "--check" ] && check=1

cd "$ROOT"
mapfile -t src < <(find launcher/core launcher/gui -name '*.cpp' -o -name '*.hpp' | LC_ALL=C sort)
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
args=(--language=C++ --qt --from-code=UTF-8 --add-comments=TRANSLATORS: --add-location=file
      --sort-by-file --package-name=x4vrmod
      --msgid-bugs-address=https://github.com/Patola/X4VRMOD/issues
      --keyword= --keyword=tr_ --keyword=trn_:1,2 --keyword=N_
      --keyword=qtr --keyword=qtrn:1,2)
for k in tr_:1 trn_:1 trn_:2 N_:1 qtr:1 qtrn:1 qtrn:2; do
    args+=("--flag=$k:qt-format")
done
xgettext "${args[@]}" -o "$T/x4vrmod.pot" "${src[@]}"

# Its date is the only line that changes from one run to the next.
same() { diff -q <(grep -v '^"POT-Creation-Date:' "$1") <(grep -v '^"POT-Creation-Date:' "$2") >/dev/null; }

if [ "$check" = 1 ]; then
    if [ ! -f "$POT" ] || ! same "$T/x4vrmod.pot" "$POT"; then
        echo "i18n: launcher/po/x4vrmod.pot is behind the code; run tools/i18n-update.sh" >&2
        diff <(grep -v '^"POT-Creation-Date:' "$POT" 2>/dev/null) \
             <(grep -v '^"POT-Creation-Date:' "$T/x4vrmod.pot") | head -40 >&2 || true
        exit 1
    fi
    exit 0
fi

mkdir -p "$PO"
if [ -f "$POT" ] && same "$T/x4vrmod.pot" "$POT"; then
    echo "x4vrmod.pot: unchanged"
else
    cp "$T/x4vrmod.pot" "$POT"
    echo "x4vrmod.pot: $(grep -c '^msgid ' "$POT") entries"
fi
for po in "$PO"/*.po; do
    [ -e "$po" ] || continue
    msgmerge --quiet --update --backup=none --add-location=file --sort-by-file "$po" "$POT"
    # What the code no longer says: git has it, and a translator needn't read past it.
    msgattrib --no-obsolete --add-location=file --sort-by-file -o "$po" "$po"
    printf '%s: ' "${po##*/}"
    msgfmt --check --statistics -o /dev/null "$po" 2>&1
done
