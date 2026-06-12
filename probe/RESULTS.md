# Probe results

## Test environment (2026-06-12)

* X4: Foundations 9.00 native Linux, windowed **1280x1280**, vsync ON,
  FreeSync OFF, low details, AA off.
* Launch: `MANGOHUD_CONFIG=fps_limit=144 mangohud gamemoderun %command%
  -nocputhrottle -nosoundthrottle -skipintro`
* Headset: Meta Quest 3 on WiVRn (90 Hz), runtime manifest at
  `~/.config/openxr/1/active_runtime.json` (symlink WiVRn creates while the
  client is connected — use this instead of the /usr/share path).

## Test 1 — bridge (xr2x4) end-to-end: **PASS**

Full 6DOF confirmed in-cockpit: tilt, yaw, pitch, crouch/stand, strafe,
lean forward/back all map correctly. HMD detected as "Meta Quest 3 on
WiVRn" via XR_MND_headless. No axis flips needed.

## Test 2 — AER responsiveness (probe `--mode aer`): **PASS**

Per-frame lateral vibration of the cockpit camera clearly visible with
in-game filter/deadzones at minimum. X4's OpenTrack path applies
per-packet pose changes at frame rate — the head-tracking filter does
NOT flatten a ±IPD/2 square wave.

## Outstanding before the instrumented phase test

- [ ] Amplitude (screenshot pairs: step mode vs aer mode cluster separation)
- [ ] Deadzone floor (`--ipd 1.0` still moves?)
- [ ] Soak (5+ min aer mode; watch for stutters/phase hiccups)
- [ ] Scope check (tracking active only in cockpit view; UI static)
- [ ] Clock-drift measurement (MangoHud CSV + probe `--log`)
- [ ] Pose→frame latency jitter (high-fps capture)
