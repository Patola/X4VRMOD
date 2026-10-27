#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Every X4 function the injector calls must be safe to call while X4 is
# tearing down -- or be guarded.
#
# **Why this exists.** X4's exit crash was ours, twice, from the same rule
# broken at two sites: IsHUDActive (fixed after take 313) and then, one call
# over, IsInPanelMode (take 316). Both load a global with
# `mov disp32(%rip),%reg` and dereference it without testing it; X4 NULLs such
# globals at shutdown while still pumping events, and headlook_tick calls them
# from SDL_PumpEvents. A code comment had declared IsInPanelMode safe. A
# backtrace names a RULE, not a location -- so this checks the rule over every
# call site's target, from X4's own bytes:
#
#   for each dlsym(RTLD_DEFAULT, "<name>") in injector/x4vr_inject.cpp:
#     disassemble <name>; for each `mov <disp>(%rip),%<reg>` in its first
#     instructions, follow <reg>: a `test %reg,%reg` first = guarded; a memory
#     operand `(%reg)` first = UNGUARDED.
#   every UNGUARDED function must carry `X4GUARD: <name>` in the injector.
#
# Controls, so it cannot pass by classifying nothing: IsInPanelMode must come
# out UNGUARDED and IsExternalViewActive guarded.
set -uo pipefail
CDPATH=
LC_ALL=C
export LC_ALL
cd "$(dirname "$0")/.." || exit 1

X4="/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4"
SRC=injector/x4vr_inject.cpp
if [ ! -f "$X4" ] || ! command -v objdump >/dev/null; then
    echo "SKIP  no X4 binary or no objdump — the rule is unchecked"
    exit 0
fi

fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

# classify <name> -> prints GUARDED, UNGUARDED:<reg>, or NOLOAD
#
# Scanned over the function's OWN bytes, from its symbol size. A fixed 0x100
# window (widened from 0x40 for GetCameraRotation's late test) ran past the end
# of GetCurrentGameTime -- 0x33 bytes -- into the next function's
# `mov 0x3db6948(%rip),%rdx` and called it UNGUARDED. A window is a guess
# about where a function ends; the symbol table knows.
# classify_listing reads an objdump listing on stdin, so the controls below can
# feed it listings X4 does not happen to contain.
#
# **`lea` is arithmetic, not a read.** GetPlayerControlledShipID (and its
# siblings) open with `mov <global>(%rip),%rdx; lea 0x238(%rdx),%rax; test
# %rdx,%rdx; ...; cmove <fallback>,%rax`: the lea only forms an address and the
# cmove replaces it when the global is NULL. The first version read the lea's
# `(%rdx)` as a dereference and called it UNGUARDED. But a register FORMED from
# the global is as dangerous as the global itself, so a lea's destination is
# tracked, and reading through it before the test is UNGUARDED too.
classify_listing() {
    awk '
        /:\t[a-z]/ {
            ins = $0
            sub(/^[^\t]*\t/, "", ins);     # drop "  a45780:" (colon + TAB)
            sub(/ *#.*/, "", ins);          # drop objdump'"'"'s "# addr <sym>"
            if (reg == "" && match(ins, /^mov +0x[0-9a-f]+\(%rip\),%r[a-z0-9]+/)) {
                reg = ins; sub(/.*,%/, "", reg); sub(/[ \t].*/, "", reg)
                nd = 0
                next
            }
            if (reg != "") {
                t = "test +%" reg ",%" reg
                if (ins ~ t) { print "GUARDED"; done = 1; exit }
                if (ins ~ /^lea /) {
                    m = "\\(%" reg "[,)]"
                    if (ins ~ m) {
                        d = ins; sub(/.*,%/, "", d); sub(/[ \t].*/, "", d)
                        der[++nd] = d
                    }
                    next
                }
                m = "\\(%" reg "[,)]"
                if (ins ~ m) { print "UNGUARDED:" reg; done = 1; exit }
                for (i = 1; i <= nd; i++) {
                    if (der[i] == "") continue
                    m = "\\(%" der[i] "[,)]"
                    if (ins ~ m) { print "UNGUARDED:" reg; done = 1; exit }
                    if (ins ~ (",%" der[i] "$")) der[i] = ""
                }
                w = ",%" reg "$"
                if (ins ~ w) reg = ""       # overwritten before any use
            }
        }
        END {
            # A load seen and never resolved is NOT "no load": the first
            # version said NOLOAD for GetCameraRotation because a 0x40-byte
            # window cut its `test %rax,%rax` in half. Unknown is unsafe.
            if (!done) print (reg != "" ? "UNDECIDED:" reg : "NOLOAD")
        }'
}

classify() {
    local name=$1 addr size
    read -r addr size < <(nm -D -S --defined-only "$X4" |
        awk -v n="$name" '$4==n{print $1, $2}')
    [ -n "$addr" ] && [ -n "$size" ] || { echo "MISSING"; return; }
    objdump -d --no-show-raw-insn --start-address=0x"$addr" \
        --stop-address=$(printf '0x%x' $((0x$addr + 0x$size))) "$X4" |
    classify_listing
}

# Controls first: the classifier must see both kinds.
c=$(classify IsInPanelMode)
[ "$c" = "UNGUARDED:rax" ] && ok "control: IsInPanelMode is UNGUARDED (rax)" \
    || bad "control: IsInPanelMode classified '$c', expected UNGUARDED:rax"
c=$(classify IsExternalViewActive)
[ "$c" = "GUARDED" ] && ok "control: IsExternalViewActive is guarded" \
    || bad "control: IsExternalViewActive classified '$c', expected GUARDED"
# Its load sits behind four pushes and a stack adjust, and its test further
# still: the case that exposed the too-short window.
c=$(classify GetCameraRotation)
[ "$c" = "GUARDED" ] && ok "control: GetCameraRotation (late load) is guarded" \
    || bad "control: GetCameraRotation classified '$c', expected GUARDED"

# A lea off the global before its test: arithmetic, then the test -- guarded.
c=$(classify GetPlayerControlledShipID)
[ "$c" = "GUARDED" ] && ok "control: GetPlayerControlledShipID (lea, then test) is guarded" \
    || bad "control: GetPlayerControlledShipID classified '$c', expected GUARDED"
# The same shape with a READ through the lea's register before the test. X4
# has no such function to point at, so it is fed a listing.
c=$(printf '  1:\tmov    0x10(%%rip),%%rdx\n  2:\tlea    0x238(%%rdx),%%rax\n  3:\tmov    (%%rax),%%rdi\n  4:\ttest   %%rdx,%%rdx\n' |
    classify_listing)
[ "$c" = "UNGUARDED:rdx" ] && ok "control: a read through a lea of the global before its test is UNGUARDED" \
    || bad "control: lea-then-read classified '$c', expected UNGUARDED:rdx"
c=$(printf '  1:\tmov    0x10(%%rip),%%rdx\n  2:\tlea    0x238(%%rdx),%%rax\n  3:\tmov    0x8(%%rsp),%%rax\n  4:\tmov    (%%rax),%%rdi\n  5:\ttest   %%rdx,%%rdx\n' |
    classify_listing)
[ "$c" = "GUARDED" ] && ok "control: a lea register overwritten before the read is no longer the global's" \
    || bad "control: lea-overwritten classified '$c', expected GUARDED"

# 0x33 bytes, and its neighbour loads 0x3db6948 into %rdx 0x12 bytes past its
# last `ret`: the function that exposed the fixed window.
c=$(classify GetCurrentGameTime)
[ "$c" = "NOLOAD" ] && ok "control: GetCurrentGameTime ends before its neighbour's load" \
    || bad "control: GetCurrentGameTime classified '$c', expected NOLOAD"

names=$(grep -o -E 'dlsym\(RTLD_DEFAULT, "[A-Za-z0-9_]+"\)' "$SRC" |
        sed -E 's/.*"([^"]+)".*/\1/' | sort -u)
[ -n "$names" ] || bad "found no dlsym(RTLD_DEFAULT, ...) calls in $SRC"
for n in $names; do
    c=$(classify "$n")
    case "$c" in
        UNGUARDED:*|UNDECIDED:*)
            if grep -q "X4GUARD: $n\b" "$SRC"; then
                ok "$n dereferences its global unchecked — guarded by us (X4GUARD)"
            else
                bad "$n: $c — dereferences (or may dereference) its global unchecked and has NO X4GUARD; it can crash X4 at exit"
            fi ;;
        GUARDED) ok "$n tests its own global" ;;
        NOLOAD)  ok "$n loads no global pointer in its prologue" ;;
        *)       bad "$n: $c" ;;
    esac
done

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
