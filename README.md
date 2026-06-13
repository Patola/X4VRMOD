# X4VRMOD

VR-ish immersion for the **native Linux** build of X4: Foundations, on top of
the game's built-in OpenTrack head-tracking support (added in X4 7.50).

## What we know about the binary (X4 9.00, Linux native)

* Links only SDL3 / Vulkan / OpenAL / LuaJIT / ffmpeg / steam_api —
  **no OpenVR, OpenXR or SteamVR linkage whatsoever**. The VR strings inside
  (`forcehmd`, `HEADTRACK_DUMMYVR`, Oculus Touch input sources…) are dormant
  remnants of the shared X-engine codebase; the HMD render backend is not
  compiled into the Linux build. True per-eye stereo cannot be enabled by a
  config flag or simple injector.
* Head tracking is a first-class feature: `HEADTRACK_OPENTRACK / TRACKIR /
  FREETRACK / TOBII`, a UDP listener compatible with OpenTrack's
  "UDP over network" output (port 4242), and a full set of tunables
  (deadzones, angle/position factors, filter strength) exposed to the UI.

## Components

### `bridge/` — xr2x4 (headset → game pose bridge)

OpenXR **headless** client (`XR_MND_headless`, works on WiVRn / Monado) that
polls the real HMD pose and streams it to X4 in OpenTrack wire format
(six little-endian doubles: x_cm y_cm z_cm yaw° pitch° roll°). No OpenTrack
install needed; X4 itself is the receiver.

```sh
cd bridge && make
XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_wivrn.json ./xr2x4 -v
```

In X4: **Controls → enable OpenTrack Support** (port 4242). Recenter with
Enter or `kill -USR1`. If an axis is mirrored in-game, flip it with
`--ix/--iy/--iz/--iyaw/--ipitch/--iroll`.

Intended use: game window shown as a **head-locked screen** in the headset
(e.g. `wlx-overlay-s` on WiVRn, or XR glasses in mirror mode) while the
bridge drives the in-game cockpit camera with 6DOF — look-around without
stereo.

### `vendor/vkShade` + SuperDepth3D — stereo (active path)

Stereoscopic SBS is produced by **Depth-Image-Based Rendering**: the
vkShade Vulkan post-processing layer (depth-buffer aware, with a ReShade FX
compiler) runs the SuperDepth3D shader on each frame. No synchronization,
no locked frame rate, FreeSync stays on. See `docs/dibr-vkshade.md` and
`CLAUDE.md` for the full setup, current blocker, and target architecture
(SBS stereo + xr2x4 head-look + a head-locked SBS viewer in the headset).

### `probe/` — AER feasibility probe (historical)

Route 1 (Luke-Ross-style **Alternate Eye Rendering** over the OpenTrack
channel) was **abandoned**: technically promising but too many user-side
requirements (locked vsync, FreeSync off, halved frame rate, frame-perfect
parity) to be practical for a distributable project. These scripts and
`probe/RESULTS.md` are retained as the record of that investigation.

## Roadmap

- [x] Binary analysis (no stereo backend on Linux; OpenTrack path confirmed)
- [x] xr2x4 pose bridge (full 6DOF verified)
- [x] AER probe + measurements (Route 1, now abandoned — see CLAUDE.md)
- [x] vkShade built & patched; SuperDepth3D compiles/loads
- [ ] **Blocker:** effects produce no visible change — diagnose vkShade
      compositing vs SuperDepth3D-specific (Test A/B in CLAUDE.md)
- [ ] Confirm depth access in X4 (DisplayDepth shows real scene depth)
- [ ] Tune SuperDepth3D SBS; survey artifacts (cockpit MFDs, HUD, trails)
- [ ] Head-locked SBS delivery to headset (wlx-overlay-s or OpenXR quads)
- [ ] Compose with xr2x4 head-look; package for distribution
