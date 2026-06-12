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

## Manual config required (X4 UI floors/limits)

Edit `~/.config/EgoSoft/X4/<steam_id>/config.xml` while the game is closed:

* `res_width` / `res_height` — custom window resolutions (e.g. 1280x1280)
  are not offered in the video menu; set them here.
* `opentrackfilterstrength` — **the UI does not go below 5; set `0` in the
  XML** for a fully unfiltered head-tracking path. This is likely why the
  per-frame square wave survives intact.

## Test A — amplitude: **PASS**

* Step mode (±3.2 cm, 1 s/side): screenshot pair shows the same cockpit
  scene from two slightly different viewpoints — a genuine stereo pair.
* AER mode at 144: screenshots fall into two clusters; no perceptible
  shrink of the separation (judgement limited by starfield backdrop and
  involuntary head/camera motion, but no attenuation was observed).

## Test B — deadzone floor: **PASS (no positional deadzone)**

±0.5 cm square wave still registers (sub-pixel flicker on cockpit edges
that starts/stops exactly with the probe). Small offsets are applied.

## Test C — soak: **PASS at locked 72 fps; FAIL at unsteady 144**

* At fps_limit=144 the game could not hold cadence; the probe lost phase:
  the camera visited intermediate positions and jolted side to side —
  confirming that cadence slips break the alternation, as predicted.
* Re-locked to **72 fps** (game limiter + MangoHud fps_limit=72, probe
  `--fps 72`): jitter remained constant and uniform for 5+ minutes,
  including wild piloting. Stable parity at a sustainable, locked rate.

Lesson: the alternation cadence is only as good as the game's frame-time
floor. 72 fps (a clean divisor of the 144 Hz monitor, and a refresh rate
the Quest 3 natively supports) is the working operating point.

## Test D — scope: head tracking is cockpit-only

Map, menus, and the external (third-person) camera do not respond to
OpenTrack input. An AER mode built on this channel renders stereo only in
first-person cockpit view; external/map views stay mono (flat at screen
depth in the headset) — same class of limitation as other AER mods.

## Outstanding — instrumented stage (at locked 72 fps)

- [ ] Clock-drift / slip-rate measurement (MangoHud CSV + probe `--log`
      → `analyze.py`)
- [ ] Pose→frame latency jitter (slow-mo capture of screen + flip log)
