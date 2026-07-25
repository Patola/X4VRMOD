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
only the final image is wrong).

Phases 2–3 replaced guesswork with measurement, and the conclusion is not
what this document originally assumed. Full evidence in
[docs/frame-analysis.md](docs/frame-analysis.md); the essentials:

- X4 renders **camera-relative**: `M_view` is *identity* in every view block
  (a space sim must do this for float precision at astronomical distances).
- Geometry is positioned by a **per-object, CPU-baked
  `M_worldviewprojection`** in descriptor **set 3**
  (`BLOCK_BUFFER_BINDING_SLOT_WORLD`), not by the camera block.
- The **set 1** camera block (`BLOCK_BUFFER_BINDING_SLOT_CAMERA`, 64 members,
  1792 B) feeds **lighting and post** (`M_invprojection`, `V_cameraposition`,
  `V_light_direction_view`, …). Patching it alone moves nothing; wiping it
  blackens the screen.
- Layouts are known exactly: X4 ships shaders **with SPIR-V debug names**, so
  every block's members and offsets are readable (`tools/extract_shaders.py`).

### The key simplification (and our main performance lever)

Because `M_view = I`, an eye offset `d` is a pure **view-space translation**,
so the per-eye transform is a *single constant matrix applied in clip space*:

```
MVP_eye = P · T(−d) · P⁻¹ · MVP        =>      clip_eye = K_eye · clip
K_eye   = P · T(−d) · P⁻¹              (one 4×4, changes only with IPD/FOV/res)
```

This means **we never have to touch the ~1300 per-object constant blocks**.
One matrix multiply appended to the vertex stage produces the eye view. That
single fact is what makes a performant implementation realistic.

### Candidate techniques (performance-ranked)

| Technique | How | Cost | Verdict |
|---|---|---|---|
| **Multiview + SPIR-V patch** — *target* | Make scene render targets 2-layer arrays, enable `VK_KHR_multiview` on the render passes, patch vertex shaders to apply `K[gl_ViewIndex]` in clip space; lighting/post indexed per view | **~1.2–1.4×** — one draw stream, vertex work amortised, no CPU-side duplication, no sync | Driver supports it (`maxMultiviewViewCount = 8` on RADV). Highest engineering cost, best result |
| **Double submit (replay)** | Submit X4's command buffers twice with different `K`, capturing each eye's output | ~2× GPU **plus** a CPU/GPU sync between eyes and extra full-frame copies | Fallback / correctness reference only |
| **Per-object CPU patching** | Rewrite every `M_worldviewprojection` per eye | ~83 KB of scattered writes/frame *plus* serialization | Rejected — needless once the clip-space identity is used |
| **Reprojection / DIBR** | — | — | Rejected (the `v0.1` dead end) |

`K_eye` is constant while IPD, FOV and resolution are constant, so it can be
**baked into the patched pipelines** and only recompiled when those change
(rare). Shadow matrices (`M_shadowCSM*`) stay shared — they are light-space
and view-independent, so shadow passes are *not* duplicated.

### Performance principles (this is a mod others will run)

1. **No per-frame CPU work proportional to object count.** The clip-space
   `K_eye` exists precisely to avoid it.
2. **No CPU↔GPU sync points** in the frame loop. Anything requiring
   "submit, wait, patch, submit" is a correctness crutch, not a shipping
   design.
3. **Do work once, at pipeline/render-pass creation**, not per draw.
4. **No extra full-frame copies** beyond the final SBS composite.
5. **Never duplicate view-independent work**: shadow cascades, and any
   luminance/exposure passes, render once.
6. **Measure every phase** against a mono baseline (MangoHud frametimes) and
   record it; a phase that regresses frametime without a correctness reason
   does not get tagged.

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
| 3. Camera control | Prove we can move the rendered view. **Superseded in practice**: measurement showed the camera block does not position geometry; the equivalent proof is now 3b below | `camera_control_proven` |
| 3b. Clip-space eye transform | SPIR-V-patch the scene vertex shaders to apply a constant `K` in clip space; a non-identity `K` visibly shifts the image. This is the real "we can drive the view" proof, and it is already the Phase-4 mechanism | `clipspace_shift_proven` |
| 4. True SBS on flatscreen (the big one) | Two eye views with **per-eye lighting**, composited 2816x1408 SBS on the swapchain. Implemented in two steps: **4a** correctness via the simplest working path, **4b** the multiview single-pass path once 4a is a reference to diff against. Cross-eye / SBS-viewer check; mouse still works; frametime recorded vs mono | `sbs_lighting_done` (4a), `sbs_multiview_done` (4b) |
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
- P3b: a non-identity clip-space `K` visibly shifts/skews the image, and
  `K = I` is pixel-identical to an unpatched run.
- P4: the two halves differ correctly (near objects shift between halves,
  distant stars do not; specular/lighting differs per eye), a real object
  fuses in an SBS viewer, mouse still works, fps >= target.
- P4 performance gate: record mono vs 4a vs 4b frametimes in
  `docs/perf.md`. 4b must be materially cheaper than 4a or it is not worth
  its complexity — and if 4a already meets the frametime budget on this
  hardware, say so plainly rather than building 4b on principle.
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
