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
#                         (default x11). This used to say the SBS split render
#                         REQUIRES x11. It does not: a Wayland surface reports
#                         no preferred extent, so X4 falls back to res_width
#                         and the split works there too -- which is the path
#                         take thirty-three actually ran on, x11 request
#                         notwithstanding. Setting this is a preference SDL
#                         may decline; the layer now reports which WSI was
#                         really used (wsi= in the surface caps line).
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
#   X4VR_MV_PROBE_KB=N    kilobytes per layer the probe may read back
#                         (default 8192; 0 = unlimited, the pre-179 behaviour).
#                         Each sample copies both layers to host memory and
#                         hashes them byte-wise, so the cost tracks the image:
#                         at 4224x4224 an unbounded sample is 285 MB and took
#                         30-50 s in take 178, which did not merely feel bad --
#                         Patola could not hold the target in frame through the
#                         stall cycles, so the run failed to measure. The layer
#                         reads a CENTRED WINDOW instead, and says so; at the
#                         default that is 528x528 of a 4224x4224 image, 4.4 MB
#                         per sample. **Dumps written by the probe are that
#                         window, not the whole image** -- centre what you want
#                         measured. Present dumps are unaffected and stay full
#                         frame.
#   X4VR_MV_PROBE_MAX=N   stop probing after N samples (0/unset = unlimited).
#                         The probe carries BOUNDED information -- one hash, and
#                         with X4VR_MV_DUMP_AUTO one dump, per per-eye image --
#                         but unbounded cost: take 176 probed every frame for a
#                         whole session, froze the game for 30 s at a time and
#                         left ~10 usable frames between. Set it a little above
#                         the number of per-eye images (40 is comfortable) and
#                         the game is playable once the sweep is done. The layer
#                         logs the line where it stops.
#   X4VR_MV_DUMP=PREFIX   PREFIX for every image this layer writes. A PATH, NOT
#                         A TRIGGER: setting it alone dumps nothing. It used to
#                         also switch on an opportunistic dump of every image
#                         whose layers differ, so take 176 -- which set it only
#                         to give the present dumps a per-run name -- wrote
#                         ~20 extra image pairs in bursts, froze the game for
#                         30 s at a time and left about 10 usable frames in
#                         between. Files are PREFIX-imgN-layer{0,1}.png.
#   X4VR_MV_DUMP_AUTO=1   the opportunistic dump, now opt-in: one pair for each
#                         probed image whose two layers DIFFER. Expensive; pair
#                         it with a short session.
#   X4VR_DUMP_PPM=1       write uncompressed .ppm instead of .png. PNG is the
#                         default and is LOSSLESS -- these dumps are photometry
#                         (tools/bright_object.py takes luminance ratios off
#                         them), so jpeg is not an option: its artefacts would
#                         land exactly on a saturated Sun where the ratio is
#                         measured. PNG is also 5-10x smaller, which is most of
#                         why the stalls got survivable.
#                         Half-float and 8-bit BGRA/RGBA both.
#   X4VR_MV_DUMP_IMG=N    dump image #N specifically, whether or not its two
#                         layers differ. Without it the dump goes to the first
#                         image that DIFFERs, which cannot answer "why is this
#                         one the same?" -- the question that costs runs.
#   X4VR_STEREO=1         bake BOTH eyes into every world vertex shader and
#                         let gl_ViewIndex pick, so one multiview draw
#                         produces two different eyes. Uses the same
#                         make_eye_shear derivation as the one-eye X4VR_EYE
#                         path, with X4VR_IPD / X4VR_PROJ_SX / X4VR_PROJ_NEAR.
#   X4VR_PROJ_MVP=0       task #23: the World modules that declare no camera
#                         block cannot read sx per draw and fall back to the
#                         baked X4VR_PROJ_SX, which is right at one zoom level
#                         and wrong across the rest of the 0.75405..29.18689
#                         range the scene camera covers. With this on they
#                         recover sx from the per-object M_worldviewprojection
#                         instead:
#                           sx = |row0(MVP).xyz| / |row3(MVP).xyz|
#                         which is exact for a rigid view and a uniformly
#                         scaled object, the object's own scale cancelling.
#                         Validated in tests/view_math.cpp over every measured
#                         sx and scales from 0.01 to 3000 (worst relative error
#                         2.4e-07), and spirv-val passes all 328 modules it
#                         accepts out of take 74's 397.
#                         Needs X4VR_PROJ_LIVE=1 -- it is the same fallback
#                         chain and only runs where the camera-block patch has
#                         already refused. ON BY DEFAULT since takes 109/110,
#                         which moved all twelve modules with zero driver
#                         rejections and no measurable frame-time change; set it
#                         to 0 to put them back on the baked constant, which is
#                         stage5-wide-field's behaviour exactly.
#                         Read it in the "patched vertex shader" line:
#                         mvp-sx= carries what baked-sx= used to.
#                         Needs X4VR_MV=1: without a view mask there is only
#                         ever view 0, and the result is the left eye twice.
#   X4VR_VR=1             task #34: bring an OpenXR session up on X4's own
#                         instance, device and queue. OFF by default.
#                         The extensions the runtime needs (external_memory_fd,
#                         timeline_semaphore, and eight more) are merged into
#                         X4's own VkInstanceCreateInfo and VkDeviceCreateInfo
#                         before those objects exist -- the same edit already
#                         made for multiview. That is the XR_KHR_vulkan_enable
#                         (v1) contract, and it is used in preference to
#                         enable2 for a specific reason: enable2 wants a
#                         pfnGetInstanceProcAddr, a Vulkan layer only has a
#                         DOWN-CHAIN one, and handing that to the runtime puts
#                         its handles in a space the loader's public entry
#                         points reject. Take 111 aborted X4 that way; take 112
#                         then hit XR_ERROR_VALIDATION_FAILURE.
#                         The session binds the VkPhysicalDevice that
#                         xrGetVulkanGraphicsDeviceKHR returns, checked against
#                         the loader's own enumeration for the GPU X4 chose. If
#                         those disagree the session is refused and logged, not
#                         forced: X4 is running, and a VR knob must not take it
#                         down.
#                         THIS STEP SUBMITS NOTHING. The headset shows the
#                         runtime's own idle scene; X4 renders to the monitor
#                         exactly as before. It exists to separate "the game
#                         survives the runtime's extensions" from "the game
#                         appears in the headset", which are different risks.
#                         The frame loop runs on its own thread, not out of
#                         vkQueuePresentKHR: xrWaitFrame blocks until the
#                         runtime's next frame boundary, and driving it from
#                         the present hook would peg X4's frame rate to the
#                         headset's refresh.
#                         Needs a runtime RUNNING (WiVRn or SteamVR) --
#                         active_runtime.json exists only while one is up. With
#                         none, the layer says NO SESSION THIS RUN and X4
#                         continues flat; score_run.py fails the run, X4 does
#                         not. Read it in the "vr summary (final):" line.
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
#   X4VR_BINDLESS_SURVEY=1
#                         measure X4's bindless descriptor table. Changes no
#                         behaviour; prints, at first present and at exit:
#                           bindless: layout #L binding N type=T count=C flags=..
#                           bindless: allocated variable count N
#                           bindless: layout #L binding N — S distinct slots,
#                                     range A..B, P holding a per-eye image
#                           bindless: ... per-eye slots: P in A..B, showing S: ..
#                           bindless: N template updates, M via image templates
#                         Keyed by layout, not by bare binding number, because a
#                         binding number alone conflates every set that uses it.
#                         The per-eye line says which table elements hold a
#                         doubled image and therefore have to be mirrored, and
#                         reports its own extent because it truncates.
#                         The template line must read 0: anything else means X4
#                         also writes image descriptors through
#                         vkUpdateDescriptorSetWithTemplate and every count above
#                         is an undercount.
#                         Works with X4VR_MV=0 (everything reads 0 per-eye,
#                         which is the control), but pair it with X4VR_MV=1 to
#                         get the real answer.
#   X4VR_BINDLESS_MIRROR=1
#                         step A of the index offset. Duplicates every
#                         image-descriptor write into slot + OFFSET, using a view
#                         of layer 1 where the image is doubled and the identical
#                         descriptor otherwise. Patches no shader, so nothing
#                         reads the twin region and THE FRAME MUST NOT CHANGE --
#                         which is what makes any frame-time delta the mirror's
#                         cost alone. Prints:
#                           bindless mirror: offset O, W twin writes, D twin
#                                     descriptors, L of them layer-1, S skipped
#                           bindless mirror: layout #L — N set(s) allocated
#                         Refuses to run alongside X4VR_MV_PRESENT_LAYER: the
#                         redirect retargets X4's own descriptor and the mirror
#                         adds a twin, so running both would corrupt view 0.
#                         Disables itself, loudly, if X4's own writes ever reach
#                         OFFSET -- that would mean the twins are landing on
#                         descriptors X4 is still using.
#   X4VR_BINDLESS_PATCH=1
#                         step B, the mechanism itself. Rewrites every fragment
#                         module that samples a bindless table so the element
#                         index becomes
#                             element = index + gl_ViewIndex * OFFSET
#                         which is where the mirror put a view of layer 1. Both
#                         tables (set 0 bindings 5 and 7) are patched; 228 of
#                         409 dumped modules declare two, and patching one would
#                         leave the other sampling view 0's slot in both eyes.
#                         REQUIRES X4VR_BINDLESS_MIRROR=1 and is ignored, with a
#                         log line, without it -- otherwise view 1 reads table
#                         slots nobody ever wrote.
#                         Gate: #103 flips from IDENTICAL to DIFFER in
#                         X4VR_MV_PROBE.
#   X4VR_MIRROR_OFFSET=N  where the twin region starts. Default 26653, half of
#                         X4's declared 53306, against a measured high-water mark
#                         of 10980. Only worth changing if the disable above
#                         fires.
#   X4VR_CURSOR=0         stop blending X4's own pointer into the eye image
#                         (task #17). ON BY DEFAULT since takes 95/96. The
#                         pointer is drawn before the side-by-side duplication,
#                         so it lands in both halves at the same in-eye position
#                         and picks exactly what X4 hit-tests. Needs the injector
#                         in the same process -- that is where the cursor bitmap
#                         and the position come from -- and says so once in the
#                         log if it is missing.
#   X4VR_HIDE_CURSOR=0    stop suppressing the pointer gamescope draws. ON BY
#                         DEFAULT: without it you get two pointers, X4's drawn
#                         one and the compositor's.
#                         It stands down on its own when the run has not asked
#                         for a drawn cursor -- X4VR_NO_LAYER=1, X4VR_SBS off,
#                         X4VR_SBS_SPLIT=0, X4VR_CURSOR=0 -- logging which.
#                         It does NOT stand down when the overlay merely fails,
#                         and that is deliberate: no pointer at all is the
#                         signal. See the dated decision in
#                         docs/known-good-runs.md.
#   X4VR_DUMP_SHADERS=<dir>
#                         write every module X4 creates as <dir>/mod-NNNN.spv.
#                         On its own that is ~1300 files and no help; the point
#                         is the line it goes with, which only X4VR_MV_INVENTORY
#                         needs to be on for:
#                           mv final: rp #31 <- frag module #NNN (mod-0NNN.spv)
#                                     samples set A binding B
#                         printed once per (pass, fragment module) pair, for
#                         every pass -- not only the ones that reach the screen.
#                         Read it with the `fb rp #N: ... imgs=[...]` lines: the
#                         framebuffer line gives a pass its images, this gives it
#                         its shaders, and together they turn "which shader
#                         writes image #57" into a lookup instead of a search
#                         through the whole dump. It also flags a texture that
#                         is already an array or is a depth sampler -- the two
#                         shapes the fragment patch refuses.
#                         Serials are per-run: a module number from one log may
#                         only be opened in the dump directory of that same run.
#   X4VR_SHEAR_LIGHTS=1   widen the World predicate to include geometry the
#                         *camera* positions rather than a per-object matrix:
#                         no set-3 block, and the vertex stage reads camera
#                         member 0, 1, 7 or 8. That is what X4's instanced
#                         deferred light volumes are (mod-0207: IO_center,
#                         IO_radius, IO_lightcolor, six instance locations).
#                         Without it they draw UNSHEARED while the geometry
#                         they light is sheared, so the light lands on the
#                         wrong pixels in view 1 -- task #22, P70.
#                         Measured offline over 397 modules: +18 World, six of
#                         them in the lighting passes, ZERO fullscreen and ZERO
#                         present-pass modules, so it cannot move the HUD.
#                         Default off: with it unset the predicate is
#                         bit-identical to the known-good behaviour (320 World).
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
#   X4VR_MASK_LDR=1       mask EVERY pass whose colour attachments are all LDR,
#                         instead of only the ones subpass_is_present() guesses
#                         are the composite. Take forty-three found 8 all-LDR
#                         passes masked by no rule at all, and the scene reached
#                         the screen through them -- right eye HUD-only, 3D
#                         black. It also found the guess is scene-dependent (3
#                         candidates in take 33, 6 in take 43), which is why a
#                         working configuration could not be restored from its
#                         knobs. This rule does not depend on the scene.
#   X4VR_WINDOWS_FULLSCREEN=1
#                         gamescope --force-windows-fullscreen: make X4's X11
#                         window the size of the nested display instead of the
#                         size of res_width. Input and output are two different
#                         surfaces inside gamescope (see take thirty-seven), and
#                         this is what makes the input one match the display.
#   X4VR_TAKE=<label>     a name for this run. Means nothing to the code; the
#                         injector prints it with every other X4VR_* variable
#                         as one copy-pasteable "env: run = ..." line, so a
#                         log segment says which configuration produced it
#                         instead of having to be reverse-engineered from
#                         which features left traces.
#
# Notes:
#   * core dumps are enabled (ulimit -c unlimited) in direct mode.
#   * gamescope on a Plasma Wayland session needs the game itself started
#     with WAYLAND_DISPLAY cleared (it then uses gamescope's XWayland); we
#     always do that inside gamescope.

set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
    CDPATH= cd -- "$(dirname "$GAME")"
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
    if [[ "${X4VR_GAMESCOPE:-0}" == 1 ]]; then
        # gamescope is itself a Wayland client of the host compositor. Clearing
        # WAYLAND_DISPLAY here clears it for gamescope, not for the game, and
        # gamescope dies with "Failed to connect to wayland socket". The child
        # is already put on gamescope's XWayland further down -- that is the
        # right place and the only place.
        echo "x4vr-launch: X4VR_X11=1 ignored under gamescope — the child is" \
             "already forced to XWayland; clearing WAYLAND_DISPLAY here would" \
             "break gamescope's own connection to the host" >&2
    else
        # unset, not empty: an empty-but-set WAYLAND_DISPLAY makes clients call
        # wl_display_connect("") and fail with "Failed to connect to wayland
        # socket: ." rather than falling back to X11.
        unset WAYLAND_DISPLAY
    fi
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
    # Everything below derives from $W/$H, which already carry any X4VR_W /
    # X4VR_H override. Take 101 read sbs_dim() again here instead, so
    # `X4VR_W=2816 X4VR_H=792` moved gamescope's window and left X4 rendering
    # 1408x1408: the window, the render and the composite came out at three
    # different sizes, which is task #31 arriving through the launcher rather
    # than the layer. The aspect test that run was supposed to be measured
    # nothing, because the aspect never changed.
    if [[ "${X4VR_ONE_EYE:-0}" == 1 ]]; then
        W=$(( W / 2 ))
        export X4VR_RES="${W}x${H}"
    elif [[ "${X4VR_SBS:-0}" == 1 && "${X4VR_SBS_SPLIT:-1}" != 0 ]]; then
        # gamescope stays at the full SBS width -- that is the window both eyes
        # are presented into -- but X4 must render one eye. State it here rather
        # than leaving the injector to infer it, so the window size and the
        # render size are set in the same place and can be read together.
        export X4VR_RES="$(( W / 2 ))x${H}"
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
    # Make X4's X11 window the size of the nested display.
    #
    # Take thirty-seven established that X4 has two surfaces inside gamescope:
    # an X11 window on its XWayland (DISPLAY=:2, window 0x40002e), which is
    # where input comes from, and a native Wayland swapchain on gamescope-0
    # created by VkLayer_FROG_gamescope_wsi, which is where output goes. The
    # X11 window is 1408 wide because res_width says so, and gamescope centres
    # it in a 2816-wide nested display -- which is the 704 offset measured in
    # take thirty-three, from the other end.
    #
    # This flag makes that window 2816 too, so input space and display space
    # are the same space. X4 should keep rendering one eye regardless, because
    # it sizes from the surface (0xFFFFFFFF -> res_width) and not from the
    # window; if that is wrong, the split test fails and SPLIT OFF says so.
    [[ "${X4VR_WINDOWS_FULLSCREEN:-0}" == 1 ]] &&
        GS_DECOR+=(--force-windows-fullscreen)
    exec gamescope "${GS_DECOR[@]}" -w "$W" -h "$H" -W "$W" -H "$H" \
        --backend sdl -- \
        env -u WAYLAND_DISPLAY "SDL_VIDEODRIVER=${X4VR_SDL_DRIVER:-x11}" \
            "SDL_VIDEO_DRIVER=${X4VR_SDL_DRIVER:-x11}" "$@"
else
    exec "$@"
fi
