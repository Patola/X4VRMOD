#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# x4vr-launch.sh — run X4 (or any command) with the X4VRMOD harness.
#
# Modes:
#   no arguments          direct mode: launch X4 from its install dir with
#                         SteamAppId set and the standard dev args
#                         (-skipintro -nocputhrottle -nosoundthrottle)
#   with arguments        wrapper mode: run the given command (e.g. Steam
#                         launch options:  .../x4vr-launch.sh %command%)
#
# Env knobs:
#   X4VR_GAME=<path>      X4 binary (default: the /nvme SteamLibrary install)
#   X4VR_BUILD=<dir>      build dir (default: <repo>/build)
#   X4VR_LOG=<file>       log destination; empty/unset in wrapper mode keeps
#                         the default /tmp/x4vr.log; set X4VR_LOG= (empty) to
#                         send mod logs to stderr for `2>&1 | tee` capture
#   X4VR_GAMESCOPE=1      wrap in gamescope at X4VR_W x X4VR_H (default off).
#                         Needed for an exact render size: X4 ignores
#                         res_width/res_height while borderless and sizes to
#                         the display instead (see common/x4vr_sbs.hpp).
#   X4VR_W / X4VR_H       gamescope size (default: the SBS size in
#                         common/x4vr_sbs.hpp)
#   X4VR_SBS=1            side-by-side composite: copy the left half of each
#                         frame over the right half. Both halves are the same
#                         eye for now -- this validates the container, not the
#                         stereo. Best with X4VR_GAMESCOPE=1.
#   X4VR_ONE_EYE=1        render a single eye at half the SBS width, in a
#                         window of exactly that size. No faked surface
#                         extent, no WSI dependence -- what an OpenXR mirror
#                         shows. Incompatible with X4VR_SBS.
#   X4VR_RES=WxH          force X4's render resolution (config res_width /
#                         res_height); set automatically by X4VR_ONE_EYE
#   X4VR_NODCC=0          stop forcing RADV_DEBUG=nodcc (default: forced).
#                         Without it, antialiasing=none paints the frame with
#                         saturated RGB blocks -- stale DCC metadata. Turning
#                         it off is only useful for measuring its cost.
#   X4VR_GRAB_CURSOR=0   stop forcing gamescope --force-grab-cursor (default:
#                         forced). Without it the first-person view pins to
#                         the floor after leaving the pilot seat.
#   X4VR_DECORATED=1      leave gamescope's host window decorated. The
#                         titlebar shortens it by 23px, so gamescope scales
#                         its nested display to fit and pads the sides.
#   X4VR_ZEROVRAM=1       RADV_DEBUG=zerovram — zero VRAM allocations. Works
#                         around X4 reading uninitialised memory (saturated
#                         RGB blocks) when antialiasing is off. Costs a little
#                         allocation time, so it is opt-in.
#   X4VR_SDL_DRIVER=<d>   SDL video driver for the game inside gamescope
#                         (default x11). The SBS split render REQUIRES x11:
#                         a Wayland surface reports no preferred extent, and
#                         the split works by halving that extent.
#   X4VR_X11=1            clear WAYLAND_DISPLAY for the game (force X11/SDL-x11;
#                         X4's Wayland output is new and may misbehave)
#   X4VR_FOSSILIZE=1      keep Valve's fossilize layer (default: disabled to
#                         keep the Vulkan layer chain clean during dev)
#   X4VR_MV=1             Phase 4b stage 1: render the frame into two array
#                         layers with the SAME eye matrix for both. Nothing on
#                         screen may change -- that is the test. Off by
#                         default. See docs/phase4b-test-plan.md.
#   X4VR_MV_PRESENT_LAYER=1
#                         make every read of a doubled image come from the
#                         second view instead of the first, so the whole frame
#                         becomes a blink comparator. With one K the image
#                         must look identical; black means the second view is
#                         never being shaded.
#   X4VR_MV_MASK=<m>      which views a masked pass renders (default 0x3).
#                         0x2 maps view 0 to array layer 1, so the frame is
#                         rendered into layer 1 alone and X4 reads layer 0
#                         through its own views -- a test of the write path
#                         that needs no redirect at all.
#   X4VR_MV_PROBE=1       hash layer 0 and layer 1 of one per-eye colour
#                         attachment per frame, cycling through them, and log
#                         both. The copy rides X4's own command buffer right
#                         after the masked pass ends, where the layout is
#                         known rather than guessed. Answers "are the two
#                         layers the same bytes" as a number instead of as an
#                         inference from what appeared on screen.
#   X4VR_STEREO=1         bake BOTH eyes into every world vertex shader and
#                         let gl_ViewIndex pick, so one multiview draw
#                         produces two different eyes. Uses the same
#                         make_eye_shear derivation as the one-eye X4VR_EYE
#                         path, with X4VR_IPD / X4VR_PROJ_SX / X4VR_PROJ_NEAR.
#                         Needs X4VR_MV=1: without a view mask there is only
#                         ever view 0, and the result is the left eye twice.
#   X4VR_MASK_TONEMAP=1   mask the tonemap resolve (rp #40/#52 -> #103) so it
#                         renders into both array layers. Keyed on the SRGB
#                         attachment format, which is what separates the
#                         tonemap from the UNORM blit chain that follows it.
#                         Masking is now a separate question from shearing:
#                         this pass draws a fullscreen triangle, so it must
#                         NOT get K, and it still needs both layers.
#                         Alone this changes nothing on screen -- the chain
#                         reading #103 is still mono. What it does change is
#                         that #103 starts appearing in X4VR_MV_PROBE, where
#                         it should read IDENTICAL: the same picture drawn
#                         twice, which is correct until the shader is patched.
#   X4VR_SBS_LAYERS=2     give the image X4 renders into a second array layer.
#   X4VR_SBS_RIGHT_LAYER=1
#                         take the right half of the composite from that
#                         second layer. Separate from the line above on
#                         purpose: allocating a layer and having something
#                         render into it are different claims, and copying an
#                         unwritten layer to screen is garbage, not stereo.
#   X4VR_PRESENT_MODE=<n> override the swapchain present mode, for measurement
#                         runs only (0=IMMEDIATE, 1=MAILBOX, 2=FIFO). X4 asks
#                         for FIFO, so frame times are pinned to the display
#                         and both sides of an A/B come back at the refresh
#                         rate. Ignored if the surface does not support the
#                         mode, and that is logged -- an uncapped run that
#                         silently stayed capped would produce a perf claim
#                         about the monitor.
#   X4VR_MULTIVIEW=0      stop the layer enabling the multiview device feature
#                         (default: enabled). X4 declares Vulkan 1.2, where
#                         multiview is core, but leaves the feature off; the
#                         second eye needs it on. Enabling it alone changes
#                         nothing until a render pass carries a view mask.
#   X4VR_VALIDATE=1       add VK_LAYER_KHRONOS_validation. Slower, but it is
#                         the oracle for most of the predicted failure modes
#                         in docs/phase4b-test-plan.md -- undersized memory
#                         binds, view/layer mismatches, incompatible passes --
#                         and it prints them by name instead of leaving an
#                         artifact to interpret.
#   X4VR_VALIDATE_LOG=<f> where validation writes (default
#                         /tmp/x4vr-validation.log). Its messages do not go
#                         through X4VR_LOG.
#   X4VR_NO_LAYER=1       skip the Vulkan layer
#   X4VR_NO_INJECT=1      skip the LD_PRELOAD injector
#
# Notes:
#   * core dumps are enabled (ulimit -c unlimited) in direct mode.
#   * gamescope on a Plasma Wayland session needs the game itself started
#     with WAYLAND_DISPLAY cleared (it then uses gamescope's XWayland); we
#     always do that inside gamescope.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${X4VR_BUILD:-$ROOT/build}"

DEFAULT_GAME="/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4"
GAME="${X4VR_GAME:-$DEFAULT_GAME}"

# X4VR_LOG: respect an explicitly-set value, including empty (=stderr).
if [[ -z "${X4VR_LOG+x}" ]]; then
    export X4VR_LOG="/tmp/x4vr.log"
else
    export X4VR_LOG
fi

if [[ "${X4VR_NO_LAYER:-0}" != 1 ]]; then
    if [[ ! -f "$BUILD/layer/VK_LAYER_X4VR_core.json" ]]; then
        echo "x4vr-launch: layer manifest not found in $BUILD/layer — build first" >&2
        exit 1
    fi
    export VK_ADD_LAYER_PATH="$BUILD/layer${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
    export VK_INSTANCE_LAYERS="VK_LAYER_X4VR_core${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
    export VK_LOADER_LAYERS_ENABLE="VK_LAYER_X4VR_core${VK_LOADER_LAYERS_ENABLE:+,$VK_LOADER_LAYERS_ENABLE}"
fi

if [[ "${X4VR_VALIDATE:-0}" == 1 ]]; then
    export VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
    export VK_LOADER_LAYERS_ENABLE="VK_LAYER_KHRONOS_validation${VK_LOADER_LAYERS_ENABLE:+,$VK_LOADER_LAYERS_ENABLE}"
    # Validation reports through its own channel, not ours: without this it
    # goes to stderr and never reaches X4VR_LOG, so "no errors in the log"
    # means only that we were not looking where they are printed.
    export VK_KHRONOS_VALIDATION_LOG_FILENAME="${X4VR_VALIDATE_LOG:-/tmp/x4vr-validation.log}"
    export VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG
    : > "$VK_KHRONOS_VALIDATION_LOG_FILENAME" || true
    echo "x4vr-launch: validation -> $VK_KHRONOS_VALIDATION_LOG_FILENAME" >&2
fi

if [[ "${X4VR_NO_INJECT:-0}" != 1 ]]; then
    if [[ ! -f "$BUILD/injector/libx4vr_inject.so" ]]; then
        echo "x4vr-launch: injector not found in $BUILD/injector — build first" >&2
        exit 1
    fi
    export LD_PRELOAD="$BUILD/injector/libx4vr_inject.so${LD_PRELOAD:+:$LD_PRELOAD}"
fi

# Keep the Vulkan layer chain clean during dev unless explicitly kept.
if [[ "${X4VR_FOSSILIZE:-0}" != 1 ]]; then
    export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
fi

if [[ $# -eq 0 ]]; then
    # ---- direct mode: launch X4 ourselves ----
    if [[ ! -x "$GAME" ]]; then
        echo "x4vr-launch: X4 binary not found at '$GAME' (set X4VR_GAME)" >&2
        exit 1
    fi
    export SteamAppId=392160
    export SteamGameId=392160
    ulimit -c unlimited || true
    cd "$(dirname "$GAME")"
    set -- "$GAME" -skipintro -nocputhrottle -nosoundthrottle
fi

# X4 reads colour targets whose DCC (Delta Color Compression) metadata is
# stale, which paints the frame with a grid of saturated RGB blocks. It only
# bites when antialiasing is off -- which the mod forces -- so this is on by
# default; without it the mod is visibly broken out of the box. Confirmed by
# elimination on Mesa 26.1.5 / RX 7900 XTX: nodcc clears it completely, and
# zerovram (a different, older X4 bug) does not. Costs memory bandwidth.
if [[ "${X4VR_NODCC:-1}" == 1 ]]; then
    export RADV_DEBUG="nodcc${RADV_DEBUG:+,$RADV_DEBUG}"
fi

if [[ "${X4VR_ZEROVRAM:-0}" == 1 ]]; then
    export RADV_DEBUG="zerovram${RADV_DEBUG:+,$RADV_DEBUG}"
fi

if [[ "${X4VR_X11:-0}" == 1 ]]; then
    export WAYLAND_DISPLAY=""
fi

echo "x4vr-launch: log=${X4VR_LOG:-stderr} layer=$([[ ${X4VR_NO_LAYER:-0} != 1 ]] && echo on || echo off) inject=$([[ ${X4VR_NO_INJECT:-0} != 1 ]] && echo on || echo off) gamescope=${X4VR_GAMESCOPE:-0} sbs=${X4VR_SBS:-0} x11=${X4VR_X11:-0}" >&2

if [[ "${X4VR_SBS:-0}" == 1 && "${X4VR_GAMESCOPE:-0}" != 1 ]]; then
    echo "x4vr-launch: WARNING X4VR_SBS=1 without gamescope — X4 will size to" \
         "the display, so the halves will not be the SBS size" >&2
fi

if [[ "${X4VR_GAMESCOPE:-0}" == 1 ]]; then
    # Single source of truth for the SBS size lives in the C header, so the
    # launcher reads it from there rather than keeping a third copy.
    sbs_dim() { sed -n "s/^#define X4VR_SBS_$1[[:space:]]\+\([0-9]\+\).*/\1/p" \
        "$ROOT/common/x4vr_sbs.hpp"; }
    W="${X4VR_W:-$(sbs_dim WIDTH)}"
    H="${X4VR_H:-$(sbs_dim HEIGHT)}"
    # One-eye mode: size the window to a single eye and let X4 render at
    # exactly that size. Nothing is faked -- no halved surface capabilities,
    # no resize feedback on Wayland -- because the render and the window
    # agree. This is what an OpenXR mirror shows anyway, and it is the mode
    # to develop the second eye in.
    if [[ "${X4VR_ONE_EYE:-0}" == 1 ]]; then
        W=$(( $(sbs_dim WIDTH) / 2 ))
        H=$(sbs_dim HEIGHT)
        export X4VR_RES="${W}x${H}"
    fi
    if [[ -z "$W" || -z "$H" ]]; then
        echo "x4vr-launch: could not read the SBS size from" \
             "common/x4vr_sbs.hpp (set X4VR_W / X4VR_H)" >&2
        exit 1
    fi
    # Inside gamescope the game must NOT see the outer Wayland display; it
    # runs on gamescope's XWayland (empirically required on Plasma Wayland).
    #
    # Clearing WAYLAND_DISPLAY is not enough on its own: SDL's Wayland driver
    # still connects to a default socket, and X4 ends up with a Wayland
    # surface. That matters beyond which backend is used, because a Wayland
    # surface reports currentExtent = 0xFFFFFFFF ("no preferred size"), and
    # the SBS split render works by reporting *half* that extent so X4 sizes
    # its whole pipeline to one eye. With no extent to halve there is nothing
    # to intercept. X11 reports the real window size, so force the driver.
    # SDL_VIDEODRIVER is set on the child only -- gamescope itself is an SDL
    # app too and must keep its own backend. X4 links SDL3, which renamed the
    # variable to SDL_VIDEO_DRIVER; both are set so the value lands whichever
    # SDL the game ends up using.
    #
    # This is now an optimisation rather than a requirement: on Wayland X4
    # falls back to res_width/res_height for its size (the surface declines to
    # dictate one), and the injector sets those to the eye size, so the split
    # render works on either backend.
    # -b: gamescope's own window on the host must be undecorated. A
    # titlebar costs it 23px of height, and it then scales its square
    # nested display down to fit and pads the sides -- which shows up as
    # thin black bars left and right of the game.
    GS_DECOR=(-b)
    [[ "${X4VR_DECORATED:-0}" == 1 ]] && GS_DECOR=()
    # gamescope flips between relative and absolute mouse mode depending on
    # cursor visibility. X4 hides the cursor and switches to mouse-look when
    # you leave the pilot seat; if gamescope stays absolute, X4's warp to
    # centre does not take and it reads a constant offset -- the view pins to
    # the floor and cannot be raised. Forcing relative mode fixes it, and
    # verified live to leave everything else intact: mouse steering, direct
    # mouse steering, the map and the menus all behave. On by default.
    [[ "${X4VR_GRAB_CURSOR:-1}" == 1 ]] && GS_DECOR+=(--force-grab-cursor)
    exec gamescope "${GS_DECOR[@]}" -w "$W" -h "$H" -W "$W" -H "$H" \
        --backend sdl -- \
        env WAYLAND_DISPLAY= "SDL_VIDEODRIVER=${X4VR_SDL_DRIVER:-x11}" \
            "SDL_VIDEO_DRIVER=${X4VR_SDL_DRIVER:-x11}" "$@"
else
    exec "$@"
fi
