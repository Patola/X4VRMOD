# x4vr-viewer

Standalone **OpenXR** application that delivers the SBS image produced by the
DIBR layer (vkShade + SuperDepth3D) to the headset, per-eye, head-locked —
and (later) sends HMD pose to X4's OpenTrack listener for 6DOF look-around.
Pure OpenXR, so it runs on **WiVRn/Monado and SteamVR** alike.

## Status: milestone 1 — head-locked stereo quad layers (test pattern)

Brings up OpenXR + Vulkan (via `XR_KHR_vulkan_enable2`), creates two
quad-layer swapchains, and submits them as head-locked
`XrCompositionLayerQuad`s with `eyeVisibility` LEFT/RIGHT. The test pattern
clears the **left eye red** and the **right eye blue** to verify the
runtime handshake and per-eye mapping before real content is wired in.

```sh
cmake -S . -B build && cmake --build build
# with the headset connected to WiVRn/Monado (or SteamVR) running:
XR_RUNTIME_JSON=~/.config/openxr/1/active_runtime.json ./build/x4vr-viewer
```

Expect: a square panel floating ~1.8 m ahead; **left eye sees red, right eye
sees blue**. Close one eye at a time to confirm. If they are swapped or you
see purple in both, that is the signal to fix eye mapping before proceeding.

## Roadmap

- [x] OpenXR+Vulkan session, two head-locked quad layers, per-eye test pattern
- [ ] PipeWire capture of the X4 SBS window → left half→L eye, right half→R eye
- [ ] HMD pose → OpenTrack UDP :4242 (absorb `bridge/xr2x4`; no headless, so
      it works on SteamVR too)
- [ ] Recenter, quad distance/size tuning, config flags
- [ ] Launcher that sets ENABLE_VKSHADE + SDL Wayland + flags + vkShade profile
