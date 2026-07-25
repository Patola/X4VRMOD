# X4VRMOD — design & roadmap

A VR mod for **X4: Foundations** (Egosoft), native Linux build, Vulkan + SDL3
+ LuaJIT, version 9.0. This is a **from-scratch second attempt**; the earlier
proof of concept (OpenTrack head-tracking + vkShade/SuperDepth3D DIBR) is
preserved at git tag `v0.1` and was abandoned as a dead end (wrong pivot,
gimbal lock, fragile ReShade setup, DIBR artifacts).

The new approach does **true in-engine stereo** by intercepting X4's own
Vulkan rendering and driving its cameras, rather than reconstructing depth
after the fact.

## Confirmed environment (this machine)

- X4 `9.0`, native Linux, links `libSDL3`, `libSDL3_ttf`, `libluajit-5.1`,
  `libvulkan`. No OpenVR/OpenXR in the binary.
- Vulkan **1.4 / RADV**, AMD RX 7900 XTX. `VK_KHR_multiview` supported
  (maxViewCount 8).
- Installed: gamescope 3.16, renderdoc 1.45, WiVRn 26.6 + Monado + envision.
  (There is an unrelated `BetterVR_Layer` and the old `vkShade` layer on the
  system; neither is used by this project.)
- Config: `~/.config/EgoSoft/X4/<steam_id>/config.xml`, resolution in
  `res_width` / `res_height`.

## Guiding principles

1. **Flatscreen-first.** Prove every rendering capability as side-by-side on
   the flat monitor before touching VR.
2. **Non-intrusive.** No user setting changes: force resolution and disable
   effects by intercepting `config.xml` in memory; get super-resolution and
   correct mouse confinement from **gamescope**. The player just runs a
   launcher.
3. **Known-good tags.** Every working milestone gets an annotated git tag
   (listed below). Regressions reset to the last good tag; we do not stack
   fixes-over-fixes blindly.
4. **Discovery before commitment.** The stereo mechanism is chosen after
   mapping X4's real render graph in renderdoc, not guessed.
5. **Honesty about difficulty.** Injected true-stereo with per-eye lighting
   in a deferred Vulkan engine is research-grade; the roadmap front-loads the
   risk.

## Architecture — three cooperating components

```
        gamescope (nested 2816x1408 virtual display; >monitor; mouse-correct)
          |
          v
  +-----------------------------  X4 process  ----------------------------+
  |                                                                       |
  |  injector  (libx4vr_inject.so, via LD_PRELOAD)                        |
  |   - interpose open()/read() of config.xml -> res 2816x1408, effects   |
  |     off (in memory; non-intrusive)                                    |
  |   - LuaJIT FFI hooks: read game mode (cockpit/walk/map/menu/load),    |
  |     camera params, metadata; optionally patch/inject UI Lua           |
  |   - SDL3 hooks: cursor state/visibility, input, window                |
  |   - publishes a shared state struct -> the Vulkan layer               |
  |                                                                       |
  |  Vulkan layer  (VK_LAYER_x4vr)      <-- reads shared state --         |
  |   - maps X4's render graph; per-eye double-render into SBS            |
  |   - per-eye camera (IPD + projection); per-eye lighting               |
  |   - flatscreen mode: present SBS to the swapchain                     |
  |   - VR mode: submit eyes to OpenXR/WiVRn projection layers            |
  |   - menu mode: world-locked quad layer                                |
  |   - draws the VR cursor                                               |
  |                                                                       |
  |  cursor shim  (in injector + layer)                                   |
  |   - intercept SDL system cursor -> draw a cursor in VR at the right   |
  |     place (menu quad, floating dialogs, map)                          |
  +-----------------------------------------------------------------------+
                          | OpenXR
                          v
                  WiVRn -> headset (6DOF)
```

Separation of concerns: the **layer** owns pixels and cameras (only it sees
Vulkan); the **injector** owns game knowledge and config (only it can read
X4's Lua/engine state and interpose libc); the **cursor shim** bridges
input to a visible VR cursor. They communicate through a small shared-memory
struct: game mode, the view/projection matrices X4 is using, cursor
position/visibility, effect flags, IPD/convergence.

## The central problem: how to make stereo

X4 renders one mono view; stereo needs two eye views **with per-eye
lighting** (a deferred engine computes view-dependent lighting, so splitting
only the final image is wrong). Candidate techniques:

| Technique | How | Feasibility | Cost |
|---|---|---|---|
| **A. Double-render (replay)** — *primary* | Intercept the scene submit; per eye, patch the camera UBO (IPD offset + projection), set viewport to the L/R half, replay the whole pipeline (geometry + lighting) | Robust; no shader changes; per-eye lighting by construction | ~2x GPU |
| **B. Multiview (`VK_KHR_multiview`)** — *probe only* | One pass, per-view matrices via `gl_ViewIndex` | Driver supports it, but X4's shaders don't use it -> would need SPIR-V patching of every scene shader. Low feasibility without source | ~1.3x if it worked |
| **C. Reprojection / DIBR** | *Rejected* — the `v0.1` dead end | — | — |

Primary path is **A**, but the exact interception point (which submit, which
UBO holds view/proj, deferred vs forward, where the UI is drawn) is unknown
until we map the frame in renderdoc. That mapping is an explicit gate — no
stereo code before it.

## Two runtime modes

- **Game mode** — immersive stereoscopic, face-locked screen, headset 6DOF to
  look around inside cockpit/station scenes. Full double-render stereo.
- **Menu mode** — map, main menu, loading, large dialog screens. World-locked
  quad at a comfortable distance; **mono is acceptable** (cheaper, sharper
  text, no vergence strain). Driven by the injector's game-mode detection.

## Effects disabled initially (via in-memory config/engine)

Antialiasing, SSAO, chromatic aberration, color correction, glow, uiglow,
distortion. Re-enabled later only where they are VR-safe.

## Roadmap (each milestone -> a git tag)

| Phase | Deliverable | Tag |
|---|---|---|
| 0. Harness | Repo + CMake (C++17) + GPLv3 + linking exception; passthrough Vulkan layer X4 loads; no-op LD_PRELOAD injector; gamescope launcher; logging; this DESIGN.md | `harness_up` |
| 1. Config + effects (flat, mono) | Injector rewrites the `config.xml` read -> 2816x1408; disables the six+one effects in memory. X4 runs wide-mono, effects off, mouse works, menus reachable | `config_intercept_done` |
| 2. Frame map | Renderdoc captures (cockpit/walk/map/menu/loading); document the render graph, deferred-vs-forward, camera-matrix location, UI pass, post chain. **Decision point for the stereo mechanism** | `frame_mapped` |
| 3. Camera control | Layer/injector modifies the mono camera -> the view visibly changes. Proves we can drive the camera | `camera_control_proven` |
| 4. True SBS on flatscreen (the big one) | Double-render per eye with per-eye camera + viewport, **including lighting**, into 2816x1408 SBS on the swapchain. Cross-eye / SBS-viewer check; mouse still works | `sbs_lighting_done` |
| 5. VR game mode | Submit the two eyes to OpenXR/WiVRn projection layers; feed headset 6DOF into the per-eye cameras | `vr_gamemode_done` |
| 6. Menu mode | Game-mode detection -> world-locked mono quad for map/menu/loading | `menu_mode_done` |
| 7. Cursor | VR-visible cursor for floating dialogs (game mode) and map/menu selection | `cursor_visible_done` |
| 8. Comfort & scale | IPD/convergence config, recenter, re-enable safe effects, push resolution past 2816x1408, letterboxing/aspect fixes, packaging | `letterboxing_fixed`, ..., `v1.0` |

Phase 4 is the real project; 0–3 de-risk it; 5–8 are the VR/UX layer on top.

## Test plan

**Standing harness (built in Phase 0, used throughout):**
- A **flatscreen SBS preview** kept as a permanent, toggleable debug output —
  the cheapest way to see correctness even in VR builds.
- **Renderdoc** captures as regression artifacts per phase.
- **Validation layer + `VK_LAYER_LUNARG_api_dump`** in debug builds.
- **MangoHud** frametime overlay for the ~2x cost budget.
- **Input smoke test**: mouse reaches every screen corner (gamescope), menu
  clicks land, keyboard works.
- Optional **frame-hash / reference-screenshot** regression per tag.

**Per-phase gates (must pass before tagging):**
- P1: X4 boots at 2816x1408, the effects visibly off, cursor reaches every
  corner, a save loads.
- P2: the documented matrix location is reproducible across two captures.
- P3: a scripted camera offset produces the expected on-screen view shift.
- P4: the two halves differ correctly (near objects shift between halves,
  distant stars do not; specular/lighting differs per eye), a real object
  fuses in an SBS viewer, mouse still works, fps >= target.
- P5: in-headset correct eye mapping (left eye sees the left view), stable
  6DOF, no swapped/inverted eyes.
- P6: entering the map switches to the world-locked quad; leaving returns to
  stereo.
- P7: cursor visible and correctly placed on a floating dialog and on the map.

Because we now drive the camera as a matrix from the headset pose (no
OpenTrack/Euler), the `v0.1` comfort bugs (wrong pivot, gimbal lock, roll
coupling) are gone by design; P5 verifies that.

## Repository layout

```
common/     shared state struct, IPC, logging, TOML config
layer/      VK_LAYER_x4vr (C++17) + manifest json
injector/   libx4vr_inject.so (LD_PRELOAD): config/effects, LuaJIT FFI, SDL
cursor/     VR cursor rendering + SDL cursor interposition
launch/     gamescope + env launcher; Steam launch-option snippet
tools/      renderdoc capture notes, SBS preview helper, analysis scripts
docs/       per-phase findings (frame map, matrix offsets, mode detection)
LICENSE, LICENSE.exception   GPLv3 + Linux-only linking exception (X4)
```

## Top risks / unknowns (resolve early)

1. **Command-buffer replay fidelity** — X4's passes may render into full-size
   intermediate targets; per-eye-into-halves may need per-eye target sets or
   full-eye render + composite. Phase 2 decides.
2. **Finding view/projection matrices** reliably across scenes/patches
   (heuristic UBO detection vs. Lua/engine hook).
3. **UI/HUD in stereo** — cockpit HUD in the same frame; per-eye vs a single
   depth plane. Menu mode sidesteps this for big menus.
4. **Deferred lighting correctness** per eye — the whole reason for
   double-render.
5. **Swapchain/resolution churn** — alt-tab, resize, gamescope interactions.
6. **Multiview** almost certainly needs shader patching — a stretch
   experiment, not a dependency.

## Suggestions / additions

- Reuse the `X4-rest-server` (Beko) LuaJIT-FFI approach for the injector's
  game-state reads (game mode, camera, metadata) — closest relevant prior art.
- Keep the flatscreen SBS preview forever as a first-class debug view.
- A small in-world debug HUD (mode, eye matrices, IPD, fps) drawn by the
  layer.
- Make IPD, convergence, eye-quad distances, and effect toggles runtime-
  tunable via hot-reloaded TOML.
- Settle reversed-Z / depth handedness once, up front.
- `X4VR_FORCE_FLAT` and `X4VR_FORCE_MONO` kill-switch env vars for debugging.
- X4 is single-player with no anti-cheat, so injection is safe to distribute.
