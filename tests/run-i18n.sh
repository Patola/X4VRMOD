#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The launcher's translations (launcher/core/x4vr_i18n.hpp):
#   - launcher/po/x4vrmod.pot is what the code marks today;
#   - every language translates all of it, each %1..%9 kept (msgfmt --check);
#   - the build compiled each one;
#   - the x4vrmod command prints English under any language, because diagnostics read
#     it. The control is the same sentence through the same catalogue under the same
#     locale, which must come out translated: without it, a system that can't show a
#     translation at all would pass this for nothing.
# The window's side, each language fitting it and each help page, is in
# x4vr_test_launcher_gui.
set -uo pipefail
CDPATH=
cd "$(dirname "$0")/.." || exit 1
BUILD="${X4VR_BUILD:-$PWD/build}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

for tool in xgettext msgmerge msgattrib msgfmt; do
    command -v "$tool" >/dev/null || { echo "FAIL  no $tool: install gettext"; exit 1; }
done

if out=$(LC_ALL=C tools/i18n-update.sh --check 2>&1); then
    ok "x4vrmod.pot is what the code marks"
else
    bad "x4vrmod.pot is what the code marks"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

langs=()
for po in launcher/po/*.po; do
    l=$(basename "$po" .po)
    langs+=("$l")
    msgmerge --quiet -o "$T/$l.po" "$po" launcher/po/x4vrmod.pot
    stats=$(LC_ALL=C msgfmt --check --statistics -o /dev/null "$T/$l.po" 2>&1)
    if [ $? = 0 ] && ! grep -qE 'fuzzy|untranslated' <<<"$stats"; then
        ok "$l: every message translated, its values kept ($stats)"
    else
        bad "$l: $stats"
    fi
    if [ -f "$BUILD/locale/$l/LC_MESSAGES/x4vrmod.mo" ]; then
        ok "$l: built"
    else
        bad "$l: not built into $BUILD/locale"
    fi
done
[ "${#langs[@]}" -ge 4 ] && ok "${#langs[@]} languages" || bad "only ${#langs[@]} languages"

# A locale that shows a translation: gettext ignores LANGUAGE under C, and C.UTF-8 too.
loc=$(locale -a 2>/dev/null | grep -iE '^(en_US|pt_BR|de_DE|fr_FR|ru_RU)\.utf-?8$' | head -1)
msg="fov is X4's field of view, like 1.4917"
if [ -z "$loc" ] || ! command -v gettext >/dev/null; then
    echo "skip  the command stays English: no locale here to show a translation under"
else
    for l in "${langs[@]}"; do
        control=$(TEXTDOMAINDIR="$BUILD/locale" LANGUAGE=$l LC_ALL=$loc gettext -d x4vrmod "$msg")
        got=$(HOME="$T/home" XDG_CONFIG_HOME= LANGUAGE=$l LC_ALL=$loc LANG=$loc \
              "$BUILD/launcher/x4vrmod" config set fov 9 2>&1)
        if [ "$control" = "$msg" ]; then
            bad "$l: the control isn't translated under $loc, so this proves nothing"
        elif [[ "$got" == *"$msg"* ]]; then
            ok "$l: the command prints English (the catalogue says: $control)"
        else
            bad "$l: the command printed: $got"
        fi
    done
fi

exit $fail
