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

### `probe/` — AER feasibility probe

Tests whether Luke-Ross-style **Alternate Eye Rendering** could ever work
through the OpenTrack channel: send a per-frame square wave of ±IPD/2
lateral offsets and measure whether the camera follows it faithfully.

```sh
./probe/aer_probe.py --mode aer --fps 72 --ipd 6.4
```

Go/no-go criteria (details in the script header):

1. **Responsiveness** — camera must move on a *per-frame* basis with in-game
   filter strength / deadzones at minimum.
2. **Amplitude** — fast alternation must retain the full ±IPD/2 offset
   (compare against `--mode step`).
3. **Phase stability** — pose→frame latency jitter must stay well under one
   frame, else eye parity flips (instant nausea). Measure with `--log` +
   high-fps screen capture.

If (and only if) the probe passes, phase 3 would be a Vulkan implicit layer
in X4's process that phase-locks pose injection to `vkQueuePresentKHR`,
copies out each frame tagged with eye parity, and an OpenXR client that
reprojects the stale eye and submits both views to WiVRn. If the probe
fails (the expected outcome — the channel is asynchronous and filtered),
the honest ceiling on Linux remains mono + 6DOF head-look.

## Roadmap

- [x] Binary analysis (no stereo backend on Linux; OpenTrack path confirmed)
- [x] xr2x4 pose bridge
- [x] AER probe scripts
- [ ] Calibrate bridge axes against X4 (signs, scale, in-game factors)
- [ ] Run probe, record measurements in `probe/RESULTS.md`
- [ ] (conditional) Vulkan layer + OpenXR compositor for AER
