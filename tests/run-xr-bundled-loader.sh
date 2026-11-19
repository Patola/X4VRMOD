#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The mod's own OpenXR loader (take 441).
#
# Started from Steam, X4 runs inside Steam's runtime container, which has no OpenXR loader, and
# the host's can't be loaded there (Arch's needs libjsoncpp.so.27). The build makes one with its
# jsoncpp and C++ runtime inside (cmake/x4vr_openxr_loader.cmake) and puts it beside the layer;
# api_open falls back to it. Here the code that opens it is x4vr_test_xr_bundled, copied into a
# folder:
#   * the control: no loader beside it, and the system's missing -> it says no, naming both;
#   * the loader beside it -> it opens that one, also when started through a symlink from
#     elsewhere (the folder is the real one, as for a layer Steam's container links in);
#   * with the system's loader there -> the system's goes first;
#   * the shipped file needs nothing the container lacks, and Arch's package pins the same
#     commit as the build.
set -uo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"
PROG="$BUILD/tests/x4vr_test_xr_bundled"
LOADER="$BUILD/layer/libopenxr_loader.so.1"
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }
if [ ! -x "$PROG" ] && [ ! -f "$LOADER" ]; then
    echo "skip  neither $PROG nor the loader: built without OpenXR headers"
    exit 0
fi
if [ ! -x "$PROG" ]; then
    bad "no $PROG, though the build made the loader"
    exit 1
fi
if [ ! -f "$LOADER" ]; then
    bad "no $LOADER: the build didn't make the loader the mod ships (X4VR_BUNDLE_OPENXR_LOADER)"
    exit 1
fi
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/mod" "$T/elsewhere"
cp "$PROG" "$T/mod/prog"

out=$("$T/mod/prog" nosystem 2>&1)
if grep -q '^failed: dlopen libopenxr_loader.so.1: ' <<<"$out" &&
    grep -qF "the mod's own: $T/mod/libopenxr_loader.so.1: cannot open shared object file" <<<"$out"; then
    ok "control: no loader anywhere -> it says no, naming the system's and the mod's own"
else
    bad "control: no loader anywhere -> $out"
fi

cp "$LOADER" "$T/mod/"
out=$("$T/mod/prog" nosystem 2>&1)
if grep -qxF "opened: $T/mod/libopenxr_loader.so.1" <<<"$out"; then
    ok "no system loader -> the mod's own, beside it"
else
    bad "no system loader -> $out"
fi

ln -s "$T/mod/prog" "$T/elsewhere/prog"
out=$("$T/elsewhere/prog" nosystem 2>&1)
if grep -qxF "opened: $T/mod/libopenxr_loader.so.1" <<<"$out"; then
    ok "started through a symlink -> the real folder's loader"
else
    bad "started through a symlink -> $out"
fi

# Read whole first: grep -q stops at the first match, ldconfig dies of SIGPIPE, and pipefail
# would call that "not there".
cache=$(ldconfig -p 2>/dev/null)
if grep -q 'libopenxr_loader\.so\.1 ' <<<"$cache"; then
    out=$("$T/mod/prog" 2>&1)
    if grep -qxF "opened: libopenxr_loader.so.1" <<<"$out"; then
        ok "the system's loader there -> it goes first"
    else
        bad "the system's loader there -> $out"
    fi
else
    echo "skip  no system OpenXR loader here, so which goes first isn't shown"
fi

# Steam's container has glibc, but neither jsoncpp nor, necessarily, as new a libstdc++.
dyn=$(readelf -d "$LOADER")
needed=$(sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' <<<"$dyn" | tr '\n' ' ')
extra=$(tr ' ' '\n' <<<"$needed" | grep -vE '^(libc|libm|libdl|libpthread|librt)\.so\.[0-9]+$|^ld-linux-x86-64\.so\.2$|^$')
if [ -z "$extra" ]; then
    ok "the shipped loader needs only glibc: $needed"
else
    bad "the shipped loader needs $extra (all: $needed)"
fi
if grep -q 'SONAME.*\[libopenxr_loader\.so\.1\]' <<<"$dyn"; then
    ok "the shipped loader is libopenxr_loader.so.1"
else
    bad "the shipped loader's soname: $(grep SONAME <<<"$dyn")"
fi

cm=$(sed -n 's/^set(X4VR_OPENXR_SDK_COMMIT "\([0-9a-f]*\)")/\1/p' "$ROOT/cmake/x4vr_openxr_loader.cmake")
pk=$(sed -n 's/.*OpenXR-SDK\.git#commit=\([0-9a-f]*\)".*/\1/p' "$ROOT/packaging/arch/PKGBUILD")
if [ -n "$cm" ] && [ "$cm" = "$pk" ]; then
    ok "Arch's package builds the loader from the build's commit, $cm"
else
    bad "the loader's commit: cmake '$cm', PKGBUILD '$pk'"
fi

exit $fail
