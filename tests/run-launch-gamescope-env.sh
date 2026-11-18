#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# What X4 is given under gamescope, privileged or not (issue #4).
#
# A gamescope with file capabilities (Nobara's has CAP_SYS_NICE) runs in glibc's
# secure-execution mode, which strips LD_PRELOAD and the rest of ld.so(8)'s list from
# the environment it hands its children: X4 came up with the layer, through Vulkan, and
# without the injector. launch/x4vr-launch.sh now puts them on gamescope's command line.
# Stand-ins, so nothing starts:
#   * gamescope: runs what follows `--`; with STUB_SECURE=1 it first drops what
#     secure-execution mode drops -- the first control shows that it does;
#   * gamemoderun: upstream's three lines;
#   * X4: a script that prints what it was given.
# What must hold: under the privileged stand-in X4 gets what the unprivileged one gives,
# with and without GameMode, and that is what it got before (the injector, GameMode's
# library first). The second control runs rc4's launch script, which must FAIL it.
set -uo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { printf 'ok    %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fail=1; }

STRIPPED='GCONV_PATH GETCONF_DIR HOSTALIASES LOCALDOMAIN LD_AUDIT LD_DEBUG LD_DEBUG_OUTPUT
LD_DYNAMIC_WEAK LD_HWCAP_MASK LD_LIBRARY_PATH LD_ORIGIN_PATH LD_PRELOAD LD_PROFILE
LD_SHOW_AUXV LOCPATH MALLOC_TRACE NIS_PATH NLSPATH RESOLV_HOST_CONF RES_OPTIONS TMPDIR TZDIR'

mkdir -p "$T/bin" "$T/home" "$T/tmp"
cat > "$T/bin/gamescope" <<SH
#!/usr/bin/env bash
if [[ \${STUB_SECURE:-0} == 1 ]]; then
    for v in $(echo $STRIPPED); do unset "\$v"; done
fi
while [[ \$# -gt 0 && \$1 != -- ]]; do shift; done
shift
exec "\$@"
SH
cat > "$T/bin/gamemoderun" <<'SH'
#!/bin/sh
GAMEMODEAUTO_NAME="libgamemodeauto.so.0"
LD_PRELOAD="${GAMEMODEAUTO_NAME}${LD_PRELOAD:+:$LD_PRELOAD}"
exec env LD_PRELOAD="${LD_PRELOAD}" $GAMEMODERUNEXEC "$@"
SH
cat > "$T/x4" <<'SH'
#!/bin/sh
echo "X4 preload=${LD_PRELOAD-<unset>}"
echo "X4 libpath=${LD_LIBRARY_PATH-<unset>}"
echo "X4 tmpdir=${TMPDIR-<unset>}"
echo "X4 wayland=${WAYLAND_DISPLAY-<unset>} sdl=${SDL_VIDEODRIVER-<unset>}"
SH
chmod +x "$T/bin/gamescope" "$T/bin/gamemoderun" "$T/x4"
cc -shared -o "$T/inj.so" -x c /dev/null || { echo "no compiler"; exit 1; }

# launch <script> [VAR=value...]: X4's view, from a clean environment.
launch() {
    local script=$1
    shift
    env -i HOME="$T/home" PATH="$T/bin:/usr/bin:/bin" WAYLAND_DISPLAY=wayland-9 \
        X4VR_DEFAULTS=0 X4VR_GAMESCOPE=1 X4VR_NO_LAYER=1 X4VR_INJECTOR="$T/inj.so" \
        X4VR_W=200 X4VR_H=100 X4VR_LOG="$T/run.log" "$@" \
        bash "$script" "$T/x4" 2>"$T/err" | grep '^X4 '
}
line() { grep "^X4 $1=" <<<"$2" | head -1; }

# Control 1: the privileged stand-in really strips, so a pass below is the script's doing.
out=$(env -i PATH="$T/bin:/usr/bin:/bin" STUB_SECURE=1 LD_PRELOAD="$T/inj.so" TMPDIR="$T/tmp" \
    gamescope -w 1 -- "$T/x4")
[[ "$(line preload "$out")" == "X4 preload=<unset>" && "$(line tmpdir "$out")" == "X4 tmpdir=<unset>" ]] \
    && ok "control: the privileged stand-in strips LD_PRELOAD and TMPDIR" \
    || bad "control: the stand-in doesn't strip: $out"

S="$ROOT/launch/x4vr-launch.sh"
for gm in 0 1; do
    want="X4 preload=$T/inj.so"
    [[ $gm == 1 ]] && want="X4 preload=libgamemodeauto.so.0:$T/inj.so"
    plain=$(launch "$S" X4VR_GAMEMODE=$gm)
    priv=$(launch "$S" X4VR_GAMEMODE=$gm STUB_SECURE=1)
    [[ "$(line preload "$plain")" == "$want" ]] \
        && ok "gamemode=$gm, plain gamescope: X4 preloads what it did before" \
        || bad "gamemode=$gm plain: $(line preload "$plain") (wanted ${want#X4 preload=}); $(tail -3 "$T/err")"
    [[ "$(line preload "$priv")" == "$want" ]] \
        && ok "gamescope=privileged gamemode=$gm: X4 still preloads the injector" \
        || bad "gamemode=$gm privileged: $(line preload "$priv") (wanted ${want#X4 preload=})"
    [[ "$plain" == "$priv" ]] && ok "gamemode=$gm: X4's whole view is the same either way" \
        || bad "gamemode=$gm: plain and privileged differ: $plain // $priv"
done

out=$(launch "$S" STUB_SECURE=1 LD_LIBRARY_PATH=/opt/x/lib TMPDIR="$T/tmp")
[[ "$(line libpath "$out")" == "X4 libpath=/opt/x/lib" && "$(line tmpdir "$out")" == "X4 tmpdir=$T/tmp" ]] \
    && ok "privileged: LD_LIBRARY_PATH and TMPDIR reach X4" || bad "libpath/tmpdir: $out"
out=$(launch "$S" STUB_SECURE=1)
[[ "$(line libpath "$out")" == "X4 libpath=<unset>" && "$(line tmpdir "$out")" == "X4 tmpdir=<unset>" ]] \
    && ok "privileged: what wasn't set isn't invented" || bad "invented: $out"
[[ "$(line wayland "$out")" == "X4 wayland=<unset> sdl=x11" ]] \
    && ok "X4 still gets no WAYLAND_DISPLAY and SDL on x11" || bad "wayland/sdl: $out"

# Control 2: rc4's script, from before the fix, must lose the injector here.
old="$T/old"
mkdir -p "$old/launch" "$old/common"
if git -C "$ROOT" show v1.0.0-rc4:launch/x4vr-launch.sh > "$old/launch/x4vr-launch.sh" 2>/dev/null; then
    cp "$ROOT/launch/x4vr-defaults.sh" "$ROOT/launch/defaults.conf" "$old/launch/"
    cp "$ROOT/common/x4vr_sbs.hpp" "$old/common/"
    out=$(launch "$old/launch/x4vr-launch.sh" STUB_SECURE=1)
    [[ "$(line preload "$out")" == "X4 preload=<unset>" ]] \
        && ok "control: rc4's script loses the injector under the privileged stand-in" \
        || bad "control: rc4's script kept it, so this test can't tell: $out"
    # And where nothing strips, X4's view is rc4's, word for word.
    for gm in 0 1; do
        before=$(launch "$old/launch/x4vr-launch.sh" X4VR_GAMEMODE=$gm LD_LIBRARY_PATH=/opt/x/lib)
        now=$(launch "$S" X4VR_GAMEMODE=$gm LD_LIBRARY_PATH=/opt/x/lib)
        [[ -n "$before" && "$before" == "$now" ]] \
            && ok "gamemode=$gm, plain gamescope: X4's view is rc4's" \
            || bad "gamemode=$gm: rc4 gave $before // now $now"
    done
else
    printf 'skip  %s\n' "control: no tag v1.0.0-rc4 in this checkout"
fi

echo
if [ "$fail" = 0 ]; then echo "all cases passed"; else echo "SOME CASES FAILED"; fi
exit "$fail"
