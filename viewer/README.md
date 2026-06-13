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
| `X4VR_CYL_ANGLE`  | 100 | horizontal arc wrapped (degrees) |
| `X4VR_CYL_ASPECT` | 1.6 | width:height; **2.0 un-squishes the 2:1 view**, lower = taller |
| `X4VR_QUAD_W/H/DIST` | 2.4/2.4/1.4 | flat-quad size + distance (m) |

Note: each eye holds X4's 2:1 (2560×1280) view squeezed into a square, so
`ASPECT=2.0` is geometrically correct; for natural 1:1 scale also raise
X4's in-game FOV toward the wrap angle. Edge-to-edge "inside the world"
immersion needs the in-engine path (see CLAUDE.md), not a virtual screen.

## Roadmap

- [x] OpenXR+Vulkan session, head-locked quad layers, per-eye test pattern
- [x] Consume the vkShade shm SBS frame; left half→L eye, right half→R eye
- [ ] HMD pose → OpenTrack UDP :4242 (absorb `bridge/xr2x4`; no headless, so
      it works on SteamVR too)
- [ ] Recenter, quad distance/size tuning, config flags
- [ ] Drop the per-frame queue wait / per-frame cmd-buffer alloc if needed
- [ ] Launcher that sets ENABLE_VKSHADE + X4VR_EXPORT + SDL Wayland + flags
