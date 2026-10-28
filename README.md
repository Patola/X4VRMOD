# X4VRMOD

A VR mod for **X4: Foundations** on native **Linux**, aiming at true
in-engine stereoscopic 3D by intercepting the game's own Vulkan rendering —
rather than reconstructing depth after the fact.

Three components: a **Vulkan layer** (stereo, cameras, VR submission), an
**`LD_PRELOAD` injector** (config, game state, effects), and a **cursor shim**
(so menus and dialogs stay usable in VR). Design and phased roadmap in
**[DESIGN.md](DESIGN.md)**; engine reverse-engineering notes in
**[docs/frame-analysis.md](docs/frame-analysis.md)**; the traps X4 sets for
modders in **[docs/x4-quirks.md](docs/x4-quirks.md)**.

> **Status: v0.9.4 — playable in VR** ([release notes](docs/releases.md)).
> X4 runs in VR with in-engine stereo (per-eye lighting), head-look in the
> cockpit and on foot, the UI on a canvas at the angle X4 intended, and every
> menu reachable and lined up with the cursor, over WiVRn or SteamVR. The
> external views (F2/F3/F4) are the whole scene too, and the cinematic view
> (F6) a steady window. Open work: F6's overlay is half-size, and the start
> menu still shows as a window. An earlier proof of
> concept using OpenTrack head-tracking + vkShade/SuperDepth3D depth-image
> reconstruction is preserved
> at tag `v0.1`; it was abandoned as a dead end (wrong pivot, gimbal lock,
> fragile setup, DIBR artifacts).

## How it works

X4 renders one mono view. To make two eye views *with correct per-eye
lighting*, the mod intercepts the engine rather than post-processing it:

- The **injector** gives X4 its own settings file. On first run it forks your
  `config.xml` into `config-x4vrmod.xml`; after that, every read is answered
  from the profile with the VR overrides applied **in memory** (2:1
  side-by-side — 5632×2816 for two 2816² eyes in the current setup — with
  VR-hostile effects off), and every write X4 makes
  is redirected into the profile. **Your `config.xml` is read once and never
  written**, so vanilla launches are untouched — and settings you change
  in-game while modded still persist. To uninstall, delete the profile.
- **gamescope** provides a virtual display so the game can render larger than
  the monitor, with the mouse still confined correctly. It is **required**,
  not a convenience: X4 honours `res_width`/`res_height` only when
  `borderless` is off, and then loses the titlebar height (1408 → 1385);
  with `borderless` on it ignores them and sizes to the display. Setting the
  display to exactly the requested size is what makes the two agree, and the layer
  warns if the swapchain comes up any other size.
- The **Vulkan layer** patches X4's vertex shaders (SPIR-V) to apply a
  constant clip-space matrix per eye, and patches the per-view constants so
  the deferred lighting matches each eye.

### The key insight

X4 renders **camera-relative** — `M_view` is identity, and geometry is
positioned by a per-object, CPU-baked `M_worldviewprojection`. That means an
eye offset `d` is a pure view-space translation, so the whole per-eye
transform collapses to a single constant matrix applied in clip space:

```
clip_eye = K_eye · clip        K_eye = P · T(−d) · P⁻¹
```

`K_eye` only changes when IPD, FOV or resolution change, so it is **baked
into the patched shaders** — the per-frame cost of stereo geometry is zero.
This is why the mod does not need to touch the ~1300 per-object constant
blocks every frame.

## Where it stands

| Milestone | State | Tag |
|---|---|---|
| Harness, frame mapped, config forked in memory (yours is never written) | done | `config_profile_done` |
| Correct stereo with per-eye lighting; X4's own pointer in each eye | done | `stage2-stereo-shading-correct`, `stage3-cursor-in-eye` |
| X4 in a headset: eye image submitted to an OpenXR runtime | done | `stage9-first-light` |
| Head tracking, closed loop on X4's own camera read-back | done | `stage12-headtracking-working` |
| Stereo stays horizontal under head roll | done — but see note | `take-221-roll-stereo` |
| UI at the angle X4 lays it out for (the sliding HUD fixed) | done | `take-268-ui-field` |
| UI fuses wherever there is a HUD | done | `take-274-canvas-far` |
| SteamVR as well as WiVRn | done | `take-303-steamvr` |
| Shadows stable at 110° (X4 sizes its cascades by 1/tan) | done | `take-325-shadows-fixed` |
| Head-look by writing X4's free-look (hover works with it) | done | `take-321-headlook-write` |
| Cursor shim: pointer usable in cockpit and menus | done | `take-330-menus-reachable`, `take-343-right-click` |
| Mouse-steering target fixed to the cockpit | done | `take-333-steering-a` |
| Walking: the head looks, the mouse and arrows turn the body | done | `take-338-walking-b` |
| On-foot menus, dialogues, teleporters and vendors lined up | done | `take-341-foot-menu-start` |
| **Playable in VR** | **release** | **`v0.9.0`** |
| F6 (cinematic) a steady window, as X4 intends | done | **`v0.9.4`** |
| F6's overlay fills its window | open | — |
| The start menu as the whole scene | open | — |
| Volumetric nebula in stereo (Matrix #79B, CEO's Doubt) | done | **`v0.9.1`** |
| The numpad orbits the external (F2) and target (F3) views again | done | **`v0.9.2`** |
| F2/F3/F4 as the whole scene: the head turns X4's camera inside them | done | **`v0.9.3`** |
| Frame pacing at 2816² per eye (a 90 Hz display is starved) | open | — |
| Head translation (lean) | open — X/Y built and switched off, Z needs depth | — |

**Note on roll stereo:** it needs `X4VR_POSE=1`. Between takes 260 and 315
the working command line carried `X4VR_POSE=0`, so it was inactive (see *"Roll
stereo has been OFF since take 260"* in [docs/x4-quirks.md](docs/x4-quirks.md));
the v0.9.0 command line carries `X4VR_POSE=1` and `X4VR_ROLL_STEREO=1`.

The engineering record — every take, prediction, and wrong turn — is
[docs/x4-quirks.md](docs/x4-quirks.md); the tagged states and their exact
command lines are in [docs/known-good-runs.md](docs/known-good-runs.md).

X4's UI shaders don't merely declare the same descriptor set as world
geometry — they genuinely *read* it, so no static shader analysis can tell
them apart. Both the UI and the shadow cascades are instead excluded at
**pipeline creation**, where they are cleanly separable by render pass. This
matters beyond appearance: X4 hit-tests its UI on the CPU in unshifted screen
space, so shifting UI on the GPU alone moves what you see away from what you
can click. Any VR repositioning of menus must therefore go through the cursor
shim, not the layer — see [docs/frame-analysis.md](docs/frame-analysis.md).

## Requirements

- **X4: Foundations**, native Linux build (developed against 9.00)
- A **Wayland** session, **gamescope**, and a Vulkan 1.3+ driver
- For the VR phases: an OpenXR runtime (**WiVRn**/Monado, or SteamVR)
- Build: CMake ≥ 3.20, a C++17 compiler, Vulkan headers, OpenXR headers
  (optional — without them the VR paths are skipped, and the OpenXR *loader* is
  never linked in any case: it is `dlopen`'d, so a machine with no runtime
  installed still runs the mod flatscreen)

## Build & run

```sh
cmake -S . -B build && cmake --build build
./launch/x4vr-launch.sh                 # launches X4 with layer + injector
X4VR_GAMESCOPE=1 ./launch/x4vr-launch.sh   # …inside gamescope at X4VR_W×X4VR_H
```

The launcher sets `SteamAppId`, the usual dev flags
(`-skipintro -nocputhrottle -nosoundthrottle`), core dumps, and the layer /
preload environment. It also works as a Steam launch-option wrapper:
`/path/to/launch/x4vr-launch.sh %command%`. Logs go to `/tmp/x4vr.log`
(`X4VR_LOG=` sends them to stderr instead).

Useful switches: `X4VR_NO_LAYER=1`, `X4VR_NO_INJECT=1`, `X4VR_NO_CONFIG=1`
(bisect a problem), `X4VR_X11=1` (force the SDL X11 path),
`X4VR_CLIP_SHIFT=<x>` (clip-space test shift), `X4VR_DUMP_MATRICES=1`,
`X4VR_FOV=<decimal>` (serve X4's `<fov>` from the profile — the field of view is
a config setting, so widening it needs no projection patch),
`X4VR_LUA=1` (log every Lua chunk X4 loads, and which of them mention a camera
API — reconnaissance for reading X4's camera back instead of driving it blind),
`X4VR_HEADLOOK=1` (serve X4's `inputmap.xml` from a fork with free-look rebound
onto a key of our own, so the head can drive it without spending shift and the
middle mouse button; `X4VR_HEADLOOK_KEY=<INPUT_KEYCODE_…>` picks the key,
default `INPUT_KEYCODE_F13`, which works — X4 knows F13-F15 and no keyboard can
emit them, so the mod steals no usable key; `X4VR_HEADLOOK_RAW=191` supplies its
X11 keycode, which is the evdev code plus 8),
`X4VR_CAMREAD=1` (read X4's own camera back through its exported
`GetCameraRotation`) and `X4VR_CAMLOOP=1` (let that reading *drive* the servo
rather than only be reported — this is what makes head tracking track),
`X4VR_PROJ_MVP=0` (turn *off* the per-draw `sx` recovery for the World shaders
that declare no camera block — on by default since takes 109/110; with it off
they shear by a constant that is only right at one zoom level),
`X4VR_VR=1` (bring an OpenXR session up on X4's own device — off by default,
needs a runtime *running*; submits nothing yet, so the headset shows the
runtime's idle scene while X4 renders to the monitor as usual).

## Repository layout

```
layer/      VK_LAYER_X4VR_core — Vulkan layer (stereo, SPIR-V patching)
injector/   libx4vr_inject.so — LD_PRELOAD (config.xml, game state)
common/     shared headers: logging, matrices/view block, SPIR-V patcher
launch/     launcher script (gamescope, env wiring, Steam wrapper)
tools/      renderdoc capture parser, shader extractor
docs/       frame analysis, engine notes, and X4's modding quirks/traps
cursor/     VR cursor (planned)
```

`tools/` is worth knowing about: `parse_capture.py` turns a renderdoc capture
into a render-pass/buffer report, and `extract_shaders.py` recovers X4's
constant-buffer layouts from the shipped SPIR-V (X4 ships shaders **with
debug names**, so the layouts are readable exactly rather than guessed).

## License

**GPLv3** ([LICENSE](LICENSE)) with a **Section-7 additional permission**
([LICENSE.exception](LICENSE.exception)) allowing the Program to be linked,
loaded, injected, interposed or otherwise combined with the **official
GNU/Linux version of X4: Foundations** published by Egosoft GmbH — including
via ELF dynamic loading, symbol interposition, runtime code injection,
function hooking, and Vulkan layer/overlay mechanisms.

The exception does not extend to the Windows, macOS or any other
non-GNU/Linux version of X4, nor to any other GPL-incompatible program, and
it grants no rights in X4: Foundations, its trademarks, or any Egosoft code.

## Credits

Developed with Claude Code (Claude Opus 4.8). Prior art for game-state
introspection: Beko's `X4-rest-server`. X4: Foundations © Egosoft GmbH.
