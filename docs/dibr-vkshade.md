# DIBR track — vkShade + SuperDepth3D

Lateral alternative to AER: synthesize both eyes from one frame + the depth
buffer (Depth-Image-Based Rendering), via the vkShade post-processing layer
(vkBasalt-like, but with depth access and a ReShade FX compiler) running
BlueSkyDefender's SuperDepth3D shader in Side-by-Side mode.

No synchronization requirements at all: both eyes derive from every frame,
so no parity, no vsync discipline, FreeSync OK, full frame rate, and it
covers ALL views (external camera and map included — it is pure
post-processing). Trade-off: approximated stereo — disocclusion edges are
inpainted, alpha-blended content (holo cockpit displays, engine trails,
nebulae) carries the depth of what is behind it, and the HUD needs masking.

## Installed setup (2026-06-12)

* vkShade built from source (`vendor/vkShade`, gitignored) with
  `meson setup --buildtype=release --prefix=$HOME/.local`; installs:
  - `~/.local/lib/libvkshade.so`
  - `~/.local/share/vulkan/implicit_layer.d/vkShade.json`
    (gated on `ENABLE_VKSHADE=1`)
  - `~/.local/bin/vkshade-shader-check`, `~/.local/bin/vkshade-run`
* Shaders in `~/.config/vkShade/reshade/Shaders/`:
  base `.fxh` from crosire/reshade-shaders + `SuperDepth3D.fx` (+ deps)
  from BlueSkyDefender/Depth3D.
* `vkshade-shader-check SuperDepth3D.fx` → `success: true, usesDepth: true`.

## Finding: vkShade requires a native Wayland surface

First attempt (X4 default = SDL3 X11 video driver under XWayland) produced
a black screen: vkShade logs `unsupported non-Wayland Vulkan surface via
vkCreateXlibSurfaceKHR; vkShade will pass through only`, and its
pass-through still interposes the swapchain — black screen instead of a
clean fallback (upstream bug worth reporting). Overlay input is also
Wayland-only. Fix: run X4 with SDL3's Wayland driver
(`SDL_VIDEO_DRIVER=wayland,x11` + `-prefer-wayland`). Note Egosoft's
caveat: Steam Input does not work on Wayland (SDL handles controllers
directly instead).

## Smoke-test procedure (flat screen, no headset)

Steam launch options:

```
ENABLE_VKSHADE=1 SDL_VIDEO_DRIVER=wayland,x11 mangohud gamemoderun %command% -nocputhrottle -nosoundthrottle -skipintro -prefer-wayland
```

(no fps lock needed for this track; FreeSync may stay on)

1. **Survival**: game reaches the menu and a cockpit with the layer active.
   If it crashes, retry with `VKSHADE_LOG_LEVEL=debug
   VKSHADE_LOG_FILE=/tmp/vkshade.log` and collect the log.
2. **Overlay**: press `Home` → vkShade overlay. Shader Manager should list
   the reshade directory; add the SuperDepth3D effect.
3. **Depth sanity**: in the effect's "Depth Map" category, use
   "·Depth Map Selection·" / "Near Plane Adjustment" and the depth-view
   debug toggle to confirm the layer found the real scene depth buffer
   (cockpit near = one shade, ships/stations mid, skybox far). A garbage
   or static image = wrong buffer (try other Depth Map Selection values;
   X4 is deferred, several candidates may exist).
   Tip: reversed depth (white = near) is normal — fix in shader settings.
4. **SBS**: set Stereoscopic Mode = "Side by Side". Cross-eye check on the
   monitor: near cockpit geometry should differ between halves, stars
   should not. Tune Divergence/Convergence (start low: divergence ~25).
5. **Artifact survey**: note behavior of holo cockpit MFDs, target
   brackets, engine trails, map view, external view, docking UI.

## Recommended SuperDepth3D preset for X4 (tuned in-headset, Quest 3)

First values that gave convincing, comfortable depth on the headset
(stored in `~/.config/vkShade/configs/X4.conf`; tentative, expect
community refinement):

| Control (label)            | vkShade conf key                  | Value  | Note |
|----------------------------|-----------------------------------|--------|------|
| Near Plane Adjustment      | `SuperDepth3D.Depth_Map_Adjust`   | ~14    | default 7.5; **the dominant depth lever** |
| Zero Parallax Distance     | `SuperDepth3D.Zero_Parallax_Distance` | 0.028 | default 0.025; compromise between space and station-landing |
| ·Depth Adjustment·         | (default, unsaved)                | 50     | changing it barely moved perceived depth |
| ·Depth Map Selection·      | (default, unsaved)                | DM0 (Normal) | DM1 is inverted/wrong for X4 |
| In-game FOV (X4 options)   | n/a                               | 120 (max) | wider = more immersive; set cylinder ANGLE to match |
| Tip: enter exact values    | n/a                               | Ctrl+Click | type into any vkShade slider; right-click resets |

**Key insight:** for X4, **Near Plane Adjustment** matters far more than
Depth Adjustment. It sets how X4's depth buffer is linearized; too low and
the whole scene's depth compresses into a tiny range (looks flat), so
spreading it (≈2× default) is what unlocks usable parallax. Depth
Adjustment merely scales existing parallax and was near-imperceptible to
change here. ZPD must trade off: ~0.05 looks great in open space but wrong
when landed on a station; ~0.032 is a livable middle.

Verify depth on the FLAT MONITOR only (Depth Map View = Normal): sky dark
navy, cockpit a near gradient. Do NOT view the depth map in the headset —
the viewer splits every frame L/R, so a non-SBS debug image sends each eye
a different half.

## Open items

- [x] Depth heuristic finds the true scene depth in X4 (DM0, Near Plane ~15)
- [x] SBS pair looks correct; preset tuned in-headset (above)
- [x] Headset delivery: `viewer/` OpenXR app (shm → per-eye quads)
- [ ] Artifact survey: holo MFDs / brackets / engine trails / map / external
- [ ] Compose with bridge/xr2x4 (6DOF head-look) — fold pose→UDP into viewer
- [ ] Per-eye aspect/quad-geometry tuning (square eye holds 2:1 view squeezed)
