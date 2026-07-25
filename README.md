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

> **Status: early development.** The rendering mechanism is proven but stereo
> itself is not implemented yet — see *Where it stands* below. An earlier
> proof of concept using OpenTrack head-tracking + vkShade/SuperDepth3D
> depth-image reconstruction is preserved at tag `v0.1`; it was abandoned as a
> dead end (wrong pivot, gimbal lock, fragile setup, DIBR artifacts).

## How it works

X4 renders one mono view. To make two eye views *with correct per-eye
lighting*, the mod intercepts the engine rather than post-processing it:

- The **injector** gives X4 its own settings file. On first run it forks your
  `config.xml` into `config-x4vrmod.xml`; after that, every read is answered
  from the profile with the VR overrides applied **in memory** (2:1
  side-by-side at 2816×1408, VR-hostile effects off), and every write X4 makes
  is redirected into the profile. **Your `config.xml` is read once and never
  written**, so vanilla launches are untouched — and settings you change
  in-game while modded still persist. To uninstall, delete the profile.
- **gamescope** provides a virtual display so the game can render larger than
  the monitor, with the mouse still confined correctly. It is **required**,
  not a convenience: X4 honours `res_width`/`res_height` only when
  `borderless` is off, and then loses the titlebar height (1408 → 1385);
  with `borderless` on it ignores them and sizes to the display. Setting the
  display to exactly 2816×1408 is what makes the two agree, and the layer
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

| Phase | State | Tag |
|---|---|---|
| Harness: layer + injector + launcher load into X4 | done | `harness_up` |
| Frame mapped: 47-pass deferred pipeline, constant-buffer layouts | done | `frame_mapped` |
| Resolution + effects forced in memory (2816×1408 exact) | done | `config_intercept_done` |
| Clip-space injection proven live (136/140 shaders, `spirv-val` clean) | done | `clipspace_shift_proven` |
| Shear is real `1/z` parallax, measured against a mono capture | done | `eye_shear_validated` |
| Mod runs off its own `config-x4vrmod.xml`; yours is never written | done | `config_profile_done` |
| SBS container: frame composited into both halves at present time | done | `sbs_container_done` |
| **Stereo**: per-eye `K`, square eye views, per-eye lighting | next | `sbs_lighting_done` |
| VR output via OpenXR/WiVRn, menu mode, VR cursor | planned | — |

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
- Build: CMake ≥ 3.20, a C++17 compiler, Vulkan headers

## Build & run

```sh
cmake -S . -B build && cmake --build build
./launch/x4vr-launch.sh                 # launches X4 with layer + injector
X4VR_GAMESCOPE=1 ./launch/x4vr-launch.sh   # …inside gamescope at 2816×1408
```

The launcher sets `SteamAppId`, the usual dev flags
(`-skipintro -nocputhrottle -nosoundthrottle`), core dumps, and the layer /
preload environment. It also works as a Steam launch-option wrapper:
`/path/to/launch/x4vr-launch.sh %command%`. Logs go to `/tmp/x4vr.log`
(`X4VR_LOG=` sends them to stderr instead).

Useful switches: `X4VR_NO_LAYER=1`, `X4VR_NO_INJECT=1`, `X4VR_NO_CONFIG=1`
(bisect a problem), `X4VR_X11=1` (force the SDL X11 path),
`X4VR_CLIP_SHIFT=<x>` (clip-space test shift), `X4VR_DUMP_MATRICES=1`.

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
