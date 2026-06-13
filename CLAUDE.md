# X4VRMOD — project context for Claude

Goal: bring a usable VR/stereoscopic + head-tracked experience to the
**native Linux** build of **X4: Foundations** (Egosoft), as a **project for
others to use**, not a personal one-off. "For others" is a hard design
constraint: every requirement we impose on the end user is a barrier, so we
favor approaches that work with default hardware/driver setups and minimal
fiddling.

User: Cláudio "Patola" Sampaio. Arch Linux, KDE Plasma on **Wayland**
(`kwin_wayland`), Meta **Quest 3** via **WiVRn**, X4 **v9.00** native (no
Proton; Vulkan renderer; SDL3). Friend "Beko" (beko.famkos.net) is the
reference for Linux head-tracking/SimPit work.

## Hard facts about the X4 Linux binary (established by inspection)

- Links only SDL3 / Vulkan / OpenAL / LuaJIT / ffmpeg / steam_api. **No
  OpenVR / OpenXR / SteamVR linkage at all** — not in NEEDED, not via any
  dlopen string. The VR strings in the binary (`forcehmd`,
  `HEADTRACK_DUMMYVR`, Oculus Touch input sources, `IsVROculusTouchActive`)
  are dormant remnants of the shared X-engine codebase; **the stereoscopic
  HMD render backend is not compiled into the Linux build.** True per-eye
  stereo cannot come from the game itself.
- **Head tracking is first-class and works**: `HEADTRACK_OPENTRACK /
  TRACKIR / FREETRACK / TOBII`, a UDP listener speaking OpenTrack's "UDP
  over network" format on **port 4242**, full tunables (deadzone, angle/
  position factor, filter strength). Added by Egosoft in X4 7.50.
- Head tracking is **cockpit-only**: it drives the first-person cockpit
  camera. The external/third-person camera and the map do **not** respond.

## Decision log

### ROUTE 1 — Alternate Eye Rendering (AER), Luke-Ross style — **ABANDONED (2026-06-12)**

Idea: drive the in-game camera left/right by ±IPD/2 on alternating frames
via the OpenTrack UDP channel, capture each frame tagged by eye parity,
reproject the stale eye, submit both to the headset.

We tested it and it was **technically promising** (see `probe/RESULTS.md`):
- The in-game head-tracking filter does NOT flatten a per-frame ±IPD/2
  square wave **once `opentrackfilterstrength=0`** (set via config.xml; UI
  floor is 5). Per-frame lateral response confirmed.
- No positional deadzone; amplitude preserved (genuine stereo pair in step
  mode); soak stable **only at a locked, sustainable frame rate** (72 fps
  worked; unsteady 144 broke parity).

**Why abandoned (user decision):** impractical to distribute. It demands
locked vsync, FreeSync OFF, a frame rate the GPU can always hold, halved
per-eye frame rate, frame-perfect parity (a present-hook Vulkan layer +
OpenXR compositor still to be built), and external clocks drift → periodic
eye-swaps. Too many requirements/pitfalls; very few users could satisfy
them all. **Do not resume unless explicitly asked.** The probe scripts
(`probe/`) and the bridge are kept; the bridge is reused by Route 2.

### ROUTE 2 — Depth-Image-Based Rendering (DIBR) — **ACTIVE PATH**

Synthesize both eyes from one rendered frame + its depth buffer, using the
**vkShade** Vulkan post-processing layer (vkBasalt-like, but with depth
access + an embedded ReShade FX compiler) running **SuperDepth3D**
(BlueSkyDefender) in Side-by-Side mode.

Why this fits "for others": **no synchronization requirements** — both eyes
from every frame, so FreeSync stays on, full frame rate, no parity, no
locked vsync. Covers all views (it's pure post-processing). Trade-off:
approximated stereo — disocclusion edges are inpainted; alpha-blended
content (holo cockpit MFDs, engine trails, nebulae) gets wrong depth; HUD
needs masking. Space scenes are otherwise DIBR-friendly (stars at infinity
= zero parallax).

**Target architecture (all independent, no sync between them):**
1. `vkShade + SuperDepth3D` → SBS stereo image, full frame rate.
2. `bridge/xr2x4` → 6DOF head-look fed to X4's OpenTrack listener (WORKS).
3. SBS delivery to headset: prefer `wlx-overlay-s` SBS/stereo screen mode
   if it has one; else a minimal OpenXR viewer (two
   `XrCompositionLayerQuad`, `eyeVisibility` LEFT/RIGHT, fed by PipeWire
   window capture). Head-locked so OpenTrack look-around and the screen
   don't fight.

## Components in this repo

- `bridge/xr2x4.c` + `Makefile` — OpenXR **headless** (`XR_MND_headless`)
  client; polls HMD pose from WiVRn/Monado, sends OpenTrack UDP (6 LE
  doubles: x_cm y_cm z_cm yaw° pitch° roll°) to 127.0.0.1:4242. **Verified:
  full 6DOF in cockpit on Quest 3 / WiVRn.** Recenter = Enter or SIGUSR1;
  `--ix/--iyaw/...` flip axes. Build binary is gitignored.
- `probe/` — AER feasibility instrument (Route 1, historical): `aer_probe.py`
  (synthetic pose square waves), `analyze.py` (clock-drift/slip from probe
  log + MangoHud CSV), `RESULTS.md` (the test findings above).
- `vendor/vkShade/` — built from source, **gitignored**. Reproduce with
  `meson setup --buildtype=release --prefix=$HOME/.local build-release &&
  ninja -C build-release install`. Apply `patches/0001-vkshade-fixes.patch`
  first (vendor is a fresh clone of github.com/slobodaapl/vkShade).
- `patches/0001-vkshade-fixes.patch` — our vkShade fixes (upstreamable).
- `docs/dibr-vkshade.md` — DIBR setup + smoke-test procedure.

## vkShade state & fixes already made (in patches/0001-vkshade-fixes.patch)

vkShade installs user-locally to `~/.local/{lib,bin,share/vulkan/
implicit_layer.d}`; layer gated on `ENABLE_VKSHADE=1`. Shaders live in
`~/.config/vkShade/reshade/Shaders/` (crosire base `.fxh` + SuperDepth3D +
standard ReShade `.fx`); per-game config `~/.config/vkShade/configs/X4.conf`.

Fixes landed:
1. **Wayland-surface requirement.** X4 defaults to SDL3/X11 under XWayland →
   `vkCreateXlibSurfaceKHR` → vkShade passthrough bug → black screen. Must
   launch with `SDL_VIDEO_DRIVER=wayland,x11` + `-prefer-wayland`.
2. **Preprocessor error logging** — print the real ReShade pp error instead
   of the generic "filepath/spaces" hint.
3. **Predefined-macro override semantics** — an injected macro is a default
   that the effect file's own `#define` overrides silently.
4. **Define-corruption fix** — vkShade harvested all ~291 used macros into
   X4.conf; its config parser strips spaces/quotes, decaying string macros
   (e.g. `G_Note`) each session until the corrupted token soup crashed
   SPIR-V codegen (`is_in_function() && is_in_block()` assert). Now only
   user-*changed* defines are saved/injected. Also added `--define
   NAME=VALUE` and `--dump-defines` to `vkshade-shader-check` for offline
   repro. Stale `SuperDepth3D@` lines were purged from X4.conf (backup:
   X4.conf.bak).

5. **Multi-technique rendering.** vkShade hardcoded
   `module.techniques[0].passes` in both the backbuffer-write count loop and
   the pass-build loop, so it rendered only the FIRST technique's passes.
   SuperDepth3D declares two techniques: `Information_SD` (1 offscreen pass →
   `Info_Tex`) then `SuperDepth3D` (the ~14-pass stereo pipeline). vkShade
   built only `Information_SD`'s `InfoOut` pass → no visible output. Fixed by
   flattening ALL techniques' passes into one ordered chain (textures resolve
   globally by `unique_name`, so the cross-technique `Info_Tex` feed works).
   Single-technique effects unaffected; descriptor pool sizing is independent
   of pass count (sets are pre-allocated from samplers/images and reused).
   Limitation: no per-technique enable UI yet — every technique in a file
   runs (correct for helper+main; could double-composite a file of several
   independent full-screen techniques).

Confirmed working on this setup: **vkShade compositing is alive and depth
access works in X4** — Daltonize shows a clear color shift; DisplayDepth
shows a real B/W scene-depth view. This is the green light for DIBR.

## CURRENT STATE (next step)

Multi-technique fix built & installed; awaiting in-game retest of
SuperDepth3D. Expected: enabling it now produces a doubled/SBS image.
- If it renders: go to the **Depth Map** category, set polarity so near=dark
  / far=light, then Stereoscopic Mode → **Side by Side**, low divergence
  (~25) to start; cross-eye check on the monitor. SBS packs both eyes into
  the existing window — it does NOT widen the window; resolution/fullscreen
  is irrelevant to whether it works.
- If still blank or wrong: capture both
  `grep -E "err|technique|pass|pipeline|creating|Reconstruction"
  /tmp/vkshade.log | tail -60` (should now list MANY passes + two
  techniques) and note any Vulkan validation errors.

After SBS is confirmed: tune artifacts, then build head-locked SBS delivery
to the headset and compose with `bridge/xr2x4` head-look.

## Standard launch line (DIBR path, flat-screen test)

Steam launch options:
```
ENABLE_VKSHADE=1 SDL_VIDEO_DRIVER=wayland,x11 mangohud gamemoderun %command% -nocputhrottle -nosoundthrottle -skipintro -prefer-wayland
```
Debug: prepend `VKSHADE_LOG_LEVEL=debug VKSHADE_LOG_FILE=/tmp/vkshade.log`.
No fps lock needed; FreeSync may stay ON (that's the whole point of Route 2).

## Manual game config (X4 UI can't set these)

Edit `~/.config/EgoSoft/X4/<steam_id>/config.xml` (game closed):
`res_width`/`res_height` (custom window res), `opentrackfilterstrength`
(UI floor 5; Route-1 testing used 0 — for Route 2, head-look wants light
smoothing, tune to taste).

## Conventions

Commit messages end with the Co-Authored-By trailer. Keep vkShade changes
mirrored into `patches/0001-vkshade-fixes.patch` (vendor/ is gitignored) via
`(cd vendor/vkShade && git diff) > patches/0001-vkshade-fixes.patch`.
