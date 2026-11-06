#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# The package layout: a real `cmake --install` with its own prefix, so the paths
# compiled into the program are the ones tested. run-launcher-install.sh covers the
# zip. A full build of the mod, without the tests, so about a minute.
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

U="$T/usr"
cmake -S . -B "$T/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$U" \
      -DX4VR_BUILD_TESTS=OFF >/dev/null || { echo "configure failed"; exit 1; }
cmake --build "$T/build" -j"$(nproc)" >/dev/null || { echo "build failed"; exit 1; }
cmake --install "$T/build" >/dev/null || { echo "install failed"; exit 1; }
want "no test programs are built" "[ ! -e '$T/build/tests' ]"

for f in bin/x4vrmod lib/x4vrmod/libVkLayer_X4VR_core.so lib/x4vrmod/libx4vr_inject.so \
         share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json \
         share/x4vrmod/launch/x4vr-launch.sh share/x4vrmod/launch/x4vr-defaults.sh \
         share/x4vrmod/launch/defaults.conf share/x4vrmod/launch/deploy-extensions.sh \
         share/x4vrmod/extension/x4vrmod/content.xml share/licenses/x4vrmod/LICENSE; do
    want "ships $f" "[ -f '$U/$f' ]"
done
want "the launch script is executable" "[ -x '$U/share/x4vrmod/launch/x4vr-launch.sh' ]"
if [ -x build/launcher/x4vrmod-launcher ]; then
    for f in share/x4vrmod/help/index.html share/x4vrmod/help/banner.png \
             share/applications/x4vrmod-launcher.desktop \
             share/icons/hicolor/256x256/apps/x4vrmod-launcher.png; do
        want "ships $f" "[ -f '$U/$f' ]"
    done
    want "ships the window" "[ -x '$U/bin/x4vrmod-launcher' ]"
    want "the package's window finds its own help" \
         "strings '$U/bin/x4vrmod-launcher' | grep -cF '$U' >/dev/null"
    if command -v desktop-file-validate >/dev/null; then
        want "the menu entry is valid" "desktop-file-validate '$U/share/applications/x4vrmod-launcher.desktop'"
    fi
fi
want "the manifest names the absolute library" \
     "grep -qF '\"library_path\": \"$U/lib/x4vrmod/libVkLayer_X4VR_core.so\"' '$U/share/vulkan/explicit_layer.d/VK_LAYER_X4VR_core.json'"
n=$(find "$U" -type f \( -name '*.cat' -o -name '*.dat' -o -name '*.xpl' \) ! -path '*/extension/x4vrmod/ext_01.*' | wc -l)
[ "$n" = 0 ] && ok "no X4 content" || bad "$n files of X4 content"

G="$T/game"
mkdir -p "$G/extensions/someone_elses_mod"
echo mine > "$G/extensions/someone_elses_mod/content.xml"
echo 900 > "$G/version.dat"
printf 'int main(void){return 0;}\n' > "$T/fake.c"
cc -o "$G/X4" "$T/fake.c" || { echo "no compiler"; exit 1; }
H="$T/home"
mkdir -p "$H"
X="env HOME=$H XDG_CONFIG_HOME= XDG_STATE_HOME= X4VR_GAME= $U/bin/x4vrmod"

out=$($X doctor --game "$G" --no-vr 2>&1)
grep -q '(package)' <<<"$out" && ok "doctor knows it's the package" || bad "layout: $out"
grep -q '^ok    Vulkan layer installed' <<<"$out" && ok "the package's layer counts as installed" || bad "layer: $out"
grep -q '^block Mod not installed' <<<"$out" && ok "the extension still needs installing" || bad "mod: $out"

out=$($X install --game "$G" 2>&1); rc=$?
[ "$rc" = 0 ] && ok "install succeeds" || bad "install: $out"
want "install puts the extension in X4's folder" "[ -f '$G/extensions/x4vrmod/content.xml' ]"
want "install writes no second manifest" "[ ! -e '$H/.local/share/vulkan' ]"
want "install copies no libraries" "[ -z \"\$(find '$H' -name '*.so')\" ]"
grep -q '^ok    Mod installed' <<<"$($X doctor --game "$G" --no-vr 2>&1)" && ok "doctor: the mod" || bad "doctor after install"

mkdir -p "$T/bin"
cat > "$T/bin/gamescope" <<'SH'
#!/bin/sh
echo "GS preload=$LD_PRELOAD"
echo "GS addpath=$VK_ADD_LAYER_PATH"
echo "GS layers=$VK_INSTANCE_LAYERS"
SH
chmod +x "$T/bin/gamescope"
out=$(env -u X4VR_GAMESCOPE PATH="$T/bin:$PATH" X4VR_LOG= $X run --game "$G" 2>&1); rc=$?
[ "$rc" = 0 ] && ok "shipping path: reaches gamescope" || bad "shipping: rc=$rc $out"
grep -qF "GS preload=$U/lib/x4vrmod/libx4vr_inject.so" <<<"$out" && ok "shipping path: the packaged injector" || bad "preload: $out"
grep -qF "GS addpath=$U/share/vulkan/explicit_layer.d" <<<"$out" && ok "shipping path: the packaged layer's folder" || bad "addpath: $out"
grep -qE "No such file|not found" <<<"$out" && bad "shipping path: something is missing: $out" \
    || ok "shipping path: nothing missing from the package"

grep -q '^Steam launch options:  x4vrmod run %command%' <<<"$($X launch-options)" \
    && ok "the Steam line uses the name on PATH" || bad "launch-options: $($X launch-options)"

$X uninstall --yes >/dev/null 2>&1
want "uninstall removes the extension" "[ ! -e '$G/extensions/x4vrmod' ]"
want "...and someone else's stays" "[ -f '$G/extensions/someone_elses_mod/content.xml' ]"
want "...and the package's own files stay" "[ -f '$U/lib/x4vrmod/libx4vr_inject.so' ] && [ -f '$U/bin/x4vrmod' ]"
grep -q 'package manager' <<<"$($X uninstall --dry-run 2>&1)" && ok "uninstall names the package manager" \
    || bad "no mention of the package manager"

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
