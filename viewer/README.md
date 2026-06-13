# x4vr-viewer

Standalone **OpenXR** application that delivers the SBS image produced by the
DIBR layer (vkShade + SuperDepth3D) to the headset, per-eye, head-locked —
and (later) sends HMD pose to X4's OpenTrack listener for 6DOF look-around.
Pure OpenXR, so it runs on **WiVRn/Monado and SteamVR** alike.

## Status: milestone 2 — head-locked stereo from X4's live SBS frame

Reads the SBS frame the DIBR layer (vkShade, `X4VR_EXPORT=1`) publishes to
POSIX shm (`shared/x4vr_shm.h`), uploads it to one full-width OpenXR
swapchain, and presents it as two head-locked `XrCompositionLayerQuad`s
whose `imageRect` selects the left / right half per eye. Milestone 1's
test pattern proved the runtime handshake and L/R mapping; this replaces it
with real content.

```sh
cmake -S . -B build && cmake --build build
# 1) launch X4 with ENABLE_VKSHADE=1 X4VR_EXPORT=1 (SuperDepth3D enabled)
# 2) then, headset connected to WiVRn/Monado (or SteamVR):
XR_RUNTIME_JSON=~/.config/openxr/1/active_runtime.json ./build/x4vr-viewer
```

The viewer waits up to 60 s for the shm, sizes itself from the header
(per-eye = width/2 × height), and streams. Expect the X4 cockpit in stereo,
head-locked. If colors look off (too dark/bright), the swapchain format's
sRGB-ness likely needs flipping — see `shm_to_vk_format()`.

### Display surface (env-tunable, no rebuild)

Defaults to a **cylinder** layer (curved wrap-around screen) when the
runtime supports `XR_KHR_composition_layer_cylinder`, else a flat quad.

| Env | Default | Meaning |
|-----|---------|---------|
| `X4VR_LAYER` | cylinder | `cylinder` or `quad` |
| `X4VR_CYL_RADIUS` | 1.4 | cylinder distance (m) |
| `X4VR_CYL_ANGLE`  | 120 | horizontal arc wrapped (deg) — set = X4's in-game FOV |
| `X4VR_CYL_ASPECT` | auto | image width:height; **defaults to the source aspect (sbs_w/sbs_h)** — override only to experiment |
| `X4VR_QUAD_W` / `_DIST` | 2.4 / 1.4 | flat-quad width + distance (m); H auto from source aspect |
| `X4VR_QUAD_H` | auto | override quad height (else W ÷ source aspect) |
| `X4VR_VOFFSET` | 0 | vertical image offset (m; **negative = down**) to recenter |

**Vertical recenter:** headsets (Quest 3 included) have an asymmetric
vertical FOV — more visible *below* the horizontal axis than above — so a
symmetric layer centered at eye level shows a black gap at the *bottom*.
Nudge `X4VR_VOFFSET` negative (e.g. -0.15) to slide the image down and
close it.

**ASPECT must equal the game window's aspect or geometry is wrong** (a
circle/loading-spinner renders oval; people too thin). Each eye holds the
full window view squeezed into its half, so a 2:1 window (2560×1280) needs
a 2:1 display. The viewer auto-derives this from the shm dimensions — you
normally never set `CYL_ASPECT`.

**ANGLE** just scales the angular size (set ≈ X4's FOV for 1:1 scale;
larger magnifies and fills more, overspilling your FOV).

Black top/bottom bars at correct aspect are **inherent to a wide window in
a squarish (~1:1) headset FOV** — not a tuning miss. Displayed proportions
equal the *game window* aspect, so to fill the vertical FOV without
distortion, render X4 at a **near-square** window.

Choosing the window resolution — match the **per-eye** shape, NOT the
combined panel:
- Quest 3 per-eye FOV ≈ 110°×96° → **~1.15:1** → e.g. `1656×1440`.
- Per-eye panel 2064×2208 → 0.935:1 (slightly taller) → e.g. `1346×1440`.
- Do **not** target the combined 4128×2208 (1.869:1) — that's two eyes; our
  SBS splits horizontally, so a 1.869:1 window is *wide* and letterboxes
  more.

Inherent trade-off (capture approach): the horizontal SBS split halves
per-eye horizontal resolution, so "fill the FOV" (near-square, ~828px/eye
wide) and "native sharpness" (needs a ~4128-wide window) pull apart. The
in-engine path (CLAUDE.md) renders each eye full-res natively and escapes
this; here, pick your balance via the window resolution.

## Roadmap

- [x] OpenXR+Vulkan session, head-locked quad layers, per-eye test pattern
- [x] Consume the vkShade shm SBS frame; left half→L eye, right half→R eye
- [ ] HMD pose → OpenTrack UDP :4242 (absorb `bridge/xr2x4`; no headless, so
      it works on SteamVR too)
- [ ] Recenter, quad distance/size tuning, config flags
- [ ] Drop the per-frame queue wait / per-frame cmd-buffer alloc if needed
- [ ] Launcher that sets ENABLE_VKSHADE + X4VR_EXPORT + SDL Wayland + flags
