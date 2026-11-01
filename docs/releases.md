# Releases

Versions are `vMAJOR.MINOR.PATCH` from this release on, and a take's
known-good state is bookmarked as `take-NNN-name`.

**Tags renamed on 2026-10-28** (Patola's choice for v0.9.0). The earlier
`v0.<n>-name` bookmarks sorted above `v0.9.0` (57, 320 … 343 > 9), so they
moved out of the version namespace. Each new tag is on the same commit with
the same message:

| was | now |
|---|---|
| `v0.77-roll-stereo` (task #77) | `take-221-roll-stereo` |
| `v0.76-ui-field` (task #76) | `take-268-ui-field` |
| `v0.57-canvas-far` | `take-274-canvas-far` |
| `v0.78-steamvr` | `take-303-steamvr` |
| `v0.320-shadows-pivot` … `v0.343-right-click` | `take-320-shadows-pivot` … `take-343-right-click` (same names) |

v0.9.0–v0.9.5 stayed local; they were first published together with v0.9.6
(2026-10-30). The three old `v0.<n>-name` tags were already public and stay
on the remote beside their `take-NNN-name` twins.

## Unreleased

* **The shipping settings are the defaults** (packaging plan, Phase 0).
  `./launch/x4vr-launch.sh` with nothing set plays like v0.9.8's command
  line, at 2816² per eye. The table is `launch/defaults.conf`.
  * An explicit setting wins; `X4VR_DEFAULTS=0` applies none.
  * `X4VR_H` alone sets the eye size: `X4VR_W` and `X4VR_RES` follow.
  * No upscaler unless `X4VR_UPSCALE` asks for one, at any eye size. (For
    takes 385–386 the defaults turned FSR on above 2816²; that made 3072²
    native impossible without an override, so the pairing moves to the
    graphical launcher.)
  * Every run's log line records all values, so old take lines still replay
    exactly; to replay one from before take 385 as it ran, add
    `X4VR_DEFAULTS=0`.

  | Variable | New default | Code default (`X4VR_DEFAULTS=0`) |
  |---|---|---|
  | `X4VR_H` / `X4VR_W` / `X4VR_RES` | 2816 / 2×H / H×H | from the config's SBS size |
  | `X4VR_UPSCALE` | none | none |
  | `X4VR_FOV` | 1.4917 | the profile's |
  | `X4VR_IPD` | 0.064 | 0.064 |
  | `X4VR_HEAD_6DOF`, `X4VR_VR` | 1 | off |
  | `X4VR_GAMESCOPE`, `X4VR_EXTENSIONS`, `X4VR_SBS`, `X4VR_MV`, `X4VR_STEREO`, `X4VR_POSE`, `X4VR_PROJ_LIVE`, `X4VR_BINDLESS_PATCH`, `X4VR_BINDLESS_MIRROR`, `X4VR_MASK_PRESENT`, `X4VR_CAMREAD`, `X4VR_CAMLOOP`, `X4VR_HEADLOOK`, `X4VR_HEADLOOK_WRITE`, `X4VR_HEADLOOK_EXTERNAL`, `X4VR_HEADLOOK_FOOT`, `X4VR_HUD_MOUSE`, `X4VR_STEER_PICK`, `X4VR_DECLARE_RENDERED_POS` | 1 | off |
  | `X4VR_PROJ_INVPROJ`, `X4VR_ROLL_STEREO`, `X4VR_HEADLOOK_MENU_CENTRE` | 1 | on (unchanged) |
  | `X4VR_SBS_LAYERS` / `X4VR_SBS_RIGHT_LAYER` | 2 / 1 | 1 / 0 |
  | `X4VR_DECLARE_RENDERED_ROT` | 2 | 0 |
  | `X4VR_CURSOR_CANVAS` | auto | off |
  | `X4VR_STEER` | a | off |
  | `X4VR_OFFAXIS` | off | the compiled canting |
  | `X4VR_CSM_COVER` | 0.663 | unpatched |
  | `X4VR_CANVAS_M` | 1000 | 0.7 |
  | `X4VR_UI_FOV` | 1.0 | 1.0 |
  | `X4VR_UI_WGATE` / `X4VR_UI_WGATE_W1` | 0.02 / 0 | 0.02 / on |
  | `X4VR_HEADLOOK_GAIN` / `_KP` / `_LEAD` | 0.115 / 0.25 / 1.0 | 0 / 0 / 0 |
  | `X4VR_HEADLOOK_YAW_LIMIT` / `_PITCH_LIMIT` | 180 / 75 | X4's 65 / 35 |
  | `X4VR_HEADLOOK_RAW` | 191 (F13) | none |
* **One extension, `x4vrmod`,** replaces three (packaging plan, decision 5):
  the look-around limits (180/75) and the camera sway removal in one
  settings diff, the same twelve settings as before.
  * `x4vrmod_lookaround` and `x4vrmod_nosway` are merged into it;
    `x4vrmod_headlook` is dropped, with its switch `X4VR_HEADLOOK_SPEED_EXT`
    (unused since take 351: the injector holds X4's look follower still per
    camera mode).
  * Its manifest says `enabled="0"`: a vanilla session that finds it leaves
    it off. Mod sessions enable it through the content.xml fork, as before.
  * Migration: with `X4VR_EXTENSIONS=1` (a default) the launcher removes the
    three old directories from X4's `extensions/` folder and says so, and
    the injector serves an old id still listed in your fork as disabled. A
    vanilla `content.xml` is still never written.
* **The per-frame head values reach X4's shaders consistently** (for the
  interior-light flicker of takes 391–398; take 399 checks it). The values
  that turn the eye offset with head tilt and move it with the seated head
  were written into a ring of only 4 buffer regions shared by every command
  buffer X4 records, so a pass could read a newer frame's values than the
  pass before it, and lighting flickered on grazing surfaces. The ring is now
  256 regions with one set of values per X4 frame, and the lighting passes'
  position reconstruction also undoes the seated forward offset, which it
  ignored. `X4VR_POSE_FRAME=0` restores the old ring.
* **Every world pass follows head tilt and head position** (take 397). Inside ships
  such as the Manticore, whole lights switched on and off many times a
  second, but only with roll compensation on. X4's depth-only passes and a
  few others draw through a second eye-offset route that ignored head tilt
  and head position, so under any tilt they drew slightly off every other
  pass. That route now applies both, like the rest. `X4VR_MVP_ROLL=0`
  restores the old behaviour for comparison.
* **Both libraries carry their version** (`git describe`) and print it on
  their first log line: `instance created (app=X4) … [X4VRMOD v…]` and
  `injector loaded into pid … [X4VRMOD v…]`. The scorer prints a `build`
  line and FAILs a run whose layer and injector differ.
* **The C++ runtime is linked in.** The layer and the injector link
  libstdc++ and libgcc statically and export only their entry points (4 and
  39 symbols), so a different C++ runtime in X4's process, Steam's for
  instance, cannot clash with ours. `tests/check-exports.sh` checks both
  after every build.

## v0.9.8 — 2026-10-31 — FSR 1 and FSR 3 in both eyes; the Pause key stays immersive

The state of take 384 (`take-384-pause-key`, code identical to commit
ca8e364). Patola:
* take 380: *"It worked phenomenally well… I saw images in each eye every time
  with correct depth"*;
* take 383: *"FSR3 seems to be working well now, I saw no smearing, no
  jittering, no artifacts. It seems to be as good as FSR1"*;
* take 384: *"It worked! We have the pause fix and FSR3 implemented"*.

* **New: X4's upscalers in VR.** `X4VR_UPSCALE=<mode>` picks one; the README
  section *Resolution and upscaling* lists every mode and its render size.
  Frame generation stays off.
  * **FSR 1** (`fsr_*`, `X4VR_FSR_STEREO`, on by default). X4's own FSR 1
    pass (EASU then RCAS) reads and writes through X4's bindless tables,
    so it wrote the left eye only and the right eye's scene was black.
    The layer now creates those two shaders patched to add
    WorkGroupID.z × offset to their table indices, and runs each eye-sized
    dispatch twice over, the second half for the right eye's layer.
  * **FSR 3** (`fsr3_*`, `X4VR_FFX_STEREO`, on by default). FSR 3 is AMD's
    FidelityFX SDK linked into X4: 11 compute passes with their own
    descriptor sets and their own history images. The layer records every
    FidelityFX dispatch a second time for the right eye, through twin
    descriptor sets:
    * images already doubled are used at layer 1;
    * each single-layer image FidelityFX writes gets a right-eye copy, and
      every barrier and clear reaches the copy too.

    No shader is changed.
  * Measured at 4224² per eye on an RX 7900 XTX (different scenes, so rough):

    | Mode | Frame time |
    |---|---|
    | none | 29.1 ms |
    | FSR 1 Quality | 13.24 ms |
    | FSR 3 Balanced | 16.72 ms (60 fps) |
  * FSR 3's right-eye copies take about 730 MiB of video memory per
    FidelityFX context. X4 creates two, so 1548 MiB in all.
* **Fixed: the Pause key no longer turns the cockpit and walking into a
  window.** A pause with no full-screen menu up keeps the view, as F2/F4
  always did; the ESC menu is still the window in front.
* **Fixed:** `tests/run-no-exit-destructors.sh` had failed since take 372;
  both binaries registered exit-time destructors again.
* **Instruments:**
  * `X4VR_COMPLOG=1` logs compute modules and dispatches with the images
    bound. It survives the driver reusing a destroyed module's handle,
    which FidelityFX causes.
  * The scorer gains `fsr`, `ffx` and `pause` blocks.

**Known limitations:**
* **Motion vectors.** X4 computes its motion vectors before the per-eye
  offset, so each eye's carry half the eye separation as motion. It is
  invisible in takes 383–384 (most likely the cockpit writes none); if FSR 3
  ever smears in both eyes, this is the cause.
* **SteamVR at high resolution:** at 4224² per eye the image is garbled
  under SteamVR, clean under WiVRn.
* **The map** has depth up to about 4068² per eye and is flat above.
* The start menu shows as a window. F6 entered on foot keeps the small
  overlay.

**How to run.** The play command is unchanged from v0.9.6 (2816² per eye, no
upscaler; see v0.9.7 below). To upscale, add `X4VR_UPSCALE=<mode>`. For
larger eyes, change `X4VR_H`, `X4VR_W` and `X4VR_RES` together. Take 384's
line in docs/known-good-runs.md is 4224² with FSR 3 Balanced, with two
instruments on.

## v0.9.7 — 2026-10-31 — larger eyes: the HUD stays put and the map draws

The state of take 378 (`take-378-map-pool`, code identical to commit
4936e46). Patola: *"It worked! The map was presented perfectly. I scroll
through it, did zoom in and out, and I put it in perspective to check if
everything was working and it was."*

* **Fixed: the lower HUD no longer moves with the resolution.**
  * X4 places the left panel, the radar and the right panel with a
    correction for screens narrower than 4:3, which our square eyes always
    are. With a UI scale below 1 it grows without bound as the height grows:
    at 4224² per eye it threw the left panel across to the right.
  * The mod now serves X4's "HUD scale" option off in every mod session, and
    the placement no longer depends on the height. Menus keep your UI scale.
* **Fixed: the map at large eye sizes** (sectors, ships and routes were
  invisible above about 4060² per eye).
  * **The memory threshold.** X4 takes the memory for images it creates
    after start-up from blocks of 255.8 MiB, and has no other way to place
    them. An image that needs more is thrown away and created again on the
    next frame, forever.
  * The layer gives the right eye its own layer in X4's render targets, which
    doubles their memory. The map's 3D view (screen-sized, 16-bit
    floating-point colour) then needs 268 MiB at 4224² per eye. X4 never
    allocated it, re-created it about 40 times a second, and drew the map
    into nothing. It is not the card's memory: 10.9 of 24 GiB were in use.
  * **The fix:** an image that, doubled, would need more than 248 MiB keeps
    the single layer X4 asked for. X4's main eye targets are exempt; they
    live in 512 MiB blocks. The map's view then fits, and both eyes see the
    left eye's copy of it.
  * **So the map has depth up to about 4068² per eye (4000² does, 4096²
    does not) and is flat above.** Nothing changes below that size.
  * If an image kept single-layer this way were ever drawn per eye, the layer
    says so (`mv: POOL-SINGLE ...`) and the scorer fails the run.
* **New: `X4VR_UPSCALE`** selects X4's upscaler (`<upmode>`, one of the ten
  values X4 9.00 knows); `none` when unset, and frame generation always off.
  **Not usable in VR yet:** with any FSR mode the right eye is not drawn,
  because X4's upscaler runs as compute and the layer does not yet run
  compute once per eye.
* **Fixed:** `X4VR_AA` accepts X4 9.00's anti-aliasing values only. Since
  9.00 the upscaler has its own setting, so `X4VR_AA=fsr_quality` was
  silently ignored, and every "FSR" run so far rendered natively.
* **Fixed:** an image the driver refused to double was still recorded as
  doubled.
* **New instrument:** `X4VR_MEMLOG=1` logs memory requirements, allocations
  and binds for doubled and large images.
* **Plan:** docs/packaging-plan.md (packages, the `x4vrmod-config`
  launcher, debug mode and bug reports).

**Known limitations:**
* **FSR:** see above.
* **SteamVR at high resolution:** at 4224² per eye the image is garbled
  under SteamVR, clean under WiVRn. Lower resolutions are fine on both.
* **Cost:** 4224² per eye ran at ~34 fps on an RX 7900 XTX (take 378);
  2816² stays the default.
* The start menu shows as a window. F6 entered on foot keeps the small
  overlay.

**How to run.** The play command is unchanged from v0.9.6 (2816² per eye):

    X4VR_HEAD_6DOF=1 X4VR_STEER_PICK=1 X4VR_HUD_MOUSE=1 X4VR_HEADLOOK_EXTERNAL=1 X4VR_HEADLOOK_FOOT=1 X4VR_STEER=a X4VR_HEADLOOK_MENU_CENTRE=1 X4VR_CANVAS_M=1000 X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_STEREO=1 X4VR_HEADLOOK_PITCH_LIMIT=75 X4VR_BINDLESS_PATCH=1 X4VR_H=2816 X4VR_DECLARE_RENDERED_POS=1 X4VR_W=5632 X4VR_ROLL_STEREO=1 X4VR_HEADLOOK_LEAD=1.0 X4VR_HEADLOOK_WRITE=1 X4VR_RES=2816x2816 X4VR_VR=1 X4VR_OFFAXIS=off X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_HEADLOOK=1 X4VR_EXTENSIONS=1 X4VR_SBS_LAYERS=2 X4VR_UI_WGATE=0.02 X4VR_POSE=1 X4VR_HEADLOOK_RAW=191 X4VR_HEADLOOK_KP=0.25 X4VR_UI_WGATE_W1=0 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_PROJ_INVPROJ=1 X4VR_LOG=/tmp/x4vr-play.log X4VR_DECLARE_RENDERED_ROT=2 X4VR_MASK_PRESENT=1 X4VR_UI_FOV=1.0 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_CAMREAD=1 X4VR_CAMLOOP=1 X4VR_CURSOR_CANVAS=auto X4VR_CSM_COVER=0.663 X4VR_HEADLOOK_GAIN=0.115 ./launch/x4vr-launch.sh

For a larger eye, change X4VR_W, X4VR_H and X4VR_RES together (for 4000²:
`X4VR_W=8000 X4VR_H=4000 X4VR_RES=4000x4000`). The scored shipping line is
in docs/known-good-runs.md, "Take 375".

## v0.9.6 — 2026-10-30 — the HUD clicks where it is drawn; lean; steering targets the ring

The state of take 375 (`take-375-hud-split`, code identical to commit
16ad52d). Patola: *"It worked. Used the lower HUD successfully with highlights
and clicking (the four elements -- left panel, radar in the center, right
panel, and dialogue choices in the center), mouse mode worked both when
looking straight and looking to the side, steering mode too. Map still
perfect."*

* **Fixed: the cockpit HUD answers where it is drawn** (`X4VR_HUD_MOUSE`).
  * Affected: the left panel, the radar/minimap, the right panel's alerts
    and the dialogue choices.
  * Before, they answered only at about twice their drawn distance from the
    centre. In VR the canvas map draws X4's perspective UI at the angle X4
    meant (#76), but X4 kept hit-testing it at its own scale.
  * The object markers, drawn at full size, keep the pointer as it is.
* **New: steering mode targets at the steering ring** (`X4VR_STEER_PICK`).
  Hover, click-select and the gimballed weapons' aim follow the ring, not
  the centre of the view, with the head turned or leaning.
* **New: lean in the cockpit** (`X4VR_HEAD_6DOF`). The head's position moves
  the eye in the world shaders, forward included. X4's own eye stays put, so
  the HUD stays with the cockpit and clicks stay lined up.
* **Fixed: the cursor works right after a load.** X4 left the pointer at the
  window's corner, outside the headset's view, until a steering toggle. It
  is now recentred once per load.
* **Fixed:** while the cockpit right-click menu is open, head-look holds the
  view, so turning the head no longer sweeps the cursor across the menu.
* **Three more approved code patches**, applied in memory to X4 9.00 and
  checked byte for byte first (refused on any other build):
  * X4's pick ray at 0x1d6e0e1 (steering);
  * the UI's mouse feed at 0xa70725 (the ring's shift);
  * X4's layer pick at 0xa24e79 (the perspective branch's scale).
* **The play command adds** `X4VR_HEAD_6DOF=1 X4VR_STEER_PICK=1
  X4VR_HUD_MOUSE=1`.
* Removed: `X4VR_HUD_RAY` and `X4VR_HUD_PICK`, the two attempts that did not
  move the HUD (takes 369–372).

**Known limitations:**
* The start menu shows as a window.
* F6 entered on foot keeps the small overlay.
* Frame pacing at 2816² per eye: a 90 Hz display is starved of about 10–20%
  of its frames (WiVRn shows it as ghosting).

**How to run.** For scored takes, the shipping line (docs/known-good-runs.md,
"Take 375"):

    X4VR_TAKE=<n> X4VR_HEAD_6DOF=1 X4VR_STEER_PICK=1 X4VR_HUD_MOUSE=1 X4VR_HEADLOOK_EXTERNAL=1 X4VR_HEADLOOK_FOOT=1 X4VR_STEER=a X4VR_HEADLOOK_MENU_CENTRE=1 X4VR_HEADLOOK_LAGLOG=1 X4VR_CANVAS_M=1000 X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_STEREO=1 X4VR_HEADLOOK_PITCH_LIMIT=75 X4VR_BINDLESS_PATCH=1 X4VR_H=2816 X4VR_DECLARE_RENDERED_POS=1 X4VR_W=5632 X4VR_ROLL_STEREO=1 X4VR_HEADLOOK_LEAD=1.0 X4VR_HEADLOOK_WRITE=1 X4VR_RES=2816x2816 X4VR_VR=1 X4VR_OFFAXIS=off X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_HEADLOOK=1 X4VR_EXTENSIONS=1 X4VR_SBS_LAYERS=2 X4VR_UI_WGATE=0.02 X4VR_POSE=1 X4VR_HEADLOOK_RAW=191 X4VR_HEADLOOK_KP=0.25 X4VR_POSELOG=1 X4VR_WATCH=1 X4VR_UI_WGATE_W1=0 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_PROJ_INVPROJ=1 X4VR_CAMDIFF=1 X4VR_LOG=/tmp/x4vr-take<n>.log X4VR_CAMSTAMP=1 X4VR_DECLARE_RENDERED_ROT=2 X4VR_MASK_PRESENT=1 X4VR_UI_FOV=1.0 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_CAMREAD=1 X4VR_CAMLOOP=1 X4VR_CURSOR_CANVAS=auto X4VR_CSM_COVER=0.663 X4VR_HEADLOOK_GAIN=0.115 ./launch/x4vr-launch.sh

For play, the same without the per-frame logging and the instruments:

    X4VR_HEAD_6DOF=1 X4VR_STEER_PICK=1 X4VR_HUD_MOUSE=1 X4VR_HEADLOOK_EXTERNAL=1 X4VR_HEADLOOK_FOOT=1 X4VR_STEER=a X4VR_HEADLOOK_MENU_CENTRE=1 X4VR_CANVAS_M=1000 X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_STEREO=1 X4VR_HEADLOOK_PITCH_LIMIT=75 X4VR_BINDLESS_PATCH=1 X4VR_H=2816 X4VR_DECLARE_RENDERED_POS=1 X4VR_W=5632 X4VR_ROLL_STEREO=1 X4VR_HEADLOOK_LEAD=1.0 X4VR_HEADLOOK_WRITE=1 X4VR_RES=2816x2816 X4VR_VR=1 X4VR_OFFAXIS=off X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_HEADLOOK=1 X4VR_EXTENSIONS=1 X4VR_SBS_LAYERS=2 X4VR_UI_WGATE=0.02 X4VR_POSE=1 X4VR_HEADLOOK_RAW=191 X4VR_HEADLOOK_KP=0.25 X4VR_UI_WGATE_W1=0 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_PROJ_INVPROJ=1 X4VR_LOG=/tmp/x4vr-play.log X4VR_DECLARE_RENDERED_ROT=2 X4VR_MASK_PRESENT=1 X4VR_UI_FOV=1.0 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_CAMREAD=1 X4VR_CAMLOOP=1 X4VR_CURSOR_CANVAS=auto X4VR_CSM_COVER=0.663 X4VR_HEADLOOK_GAIN=0.115 ./launch/x4vr-launch.sh

## v0.9.5 — 2026-10-29 — F6's overlay fills its window

The state of take 359 (`take-359-f6-overlay`, code identical to commit
b4197a8). Patola: *"now the overlay covers the whole F6 window, and no
glitch for using F4→F6, and right-click behavior is still working as
before."*

* **Fixed:** the cinematic view's (F6) information overlay covered only
  about half of its window, unreadable and out of keeping with X4's design
  (it represents the fictional screen the scene is shown on).
  * The overlay is X4's UI laid out for the whole flat screen. The canvas
    map that puts the HUD at the angle X4 meant is baked into the UI
    pipelines when they are created.
  * Each such pipeline now has a twin built with X4's own UI vertex stage,
    and the layer binds the twins while F6 is up. In F6, X4's UI is drawn
    exactly as X4 laid it out; everywhere else nothing changes. No shader
    changes.
  * F6 is recognised from X4's state: in the pilot seat, the HUD down, not
    paused, no conversation, and only X4's TopLevelMenu open, held 0.3 s.
  * The canvas cursor is off in F6.
* Tested on the GPU harness under validation (a twin built, bound only in
  F6, no added validation errors) before the take.
* The play command is unchanged.

**Known limitations:** the start menu shows as a window. F6 entered on foot
keeps the small overlay (the signal requires the pilot seat).

## v0.9.4 — 2026-10-29 — F6 is a steady window again

The state of take 358 (code: v0.9.3 plus commit 679e52c). Patola: *"F2, F3,
F4 are now behaving as expected, including the HUD appearing in F2 and F4
and following my head"*; F6 entered from F4 and from the cockpit both
worked.

* **Fixed:** in v0.9.3, the cinematic view (F6) entered from F4 was a window
  glued to the face that followed the head. F6 is drawn through another
  camera; entered from F4, X4 left the player's camera in its external mode,
  so the head was turned into a camera X4 was not drawing. Now the head
  stands down whenever the HUD is down in an external view (F6 hides it), so
  F6 is a steady window, *"like in a television"*.
* The HUD in F2 and F4 follows the head; that HUD is X4's own (vanilla shows
  it in F2/F4, not in F3).
* The play command is unchanged from v0.9.3.

**Known limitations:**
* F6's information overlay covers only about half of its window. It is X4's
  UI laid out for the whole flat screen, shrunk by the canvas scale that puts
  the HUD at the angle X4 meant. Next to be fixed.
* The start menu shows as a window.

## v0.9.3 — 2026-10-28 — the external views are the whole scene

The state of take 354 (`take-354-external-head`, code identical to commit
a8c3b8a). Patola: *"F2, F3 and F4 now worked perfectly, I could even click
elements inside the cameras."*

* **New:** in F2, F3 and F4 the head looks around inside the view, as in the
  cockpit. Each is now the whole scene, not a window in front.
  * X4 culls and hit-tests with the turned camera: elements in the view can
    be clicked.
  * The numpad still orbits F2/F3/F4; X4's orbit is never written.
* **A third approved code patch** (`X4VR_HEADLOOK_EXTERNAL`), applied in
  memory to X4 9.00 and checked byte for byte first. Two sites in X4's
  camera evaluation (0x1929f70):
  * X4's early return at 0x192a27f, the path the external views take;
  * the orbit compose at 0x192a295, which the external views did not reach
    in take 354 and which is kept for completeness.
  For the player's camera outside the cockpit/walking mode, the head's
  rotation is multiplied into X4's result. The view is declared head-turned
  only once that has happened.
* **The play command adds `X4VR_HEADLOOK_EXTERNAL=1`** (below).

**Known limitations:** the cinematic view (F6) and the start menu still show
as a window. F6 is drawn through another camera than the player's.

**How to run** (docs/known-good-runs.md, "Take 354 — THE SHIPPING LINE"):

    X4VR_TAKE=<n> X4VR_HEADLOOK_EXTERNAL=1 X4VR_HEADLOOK_FOOT=1 X4VR_STEER=a X4VR_HEADLOOK_MENU_CENTRE=1 X4VR_HEADLOOK_LAGLOG=1 X4VR_CANVAS_M=1000 X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_STEREO=1 X4VR_HEADLOOK_PITCH_LIMIT=75 X4VR_BINDLESS_PATCH=1 X4VR_H=2816 X4VR_DECLARE_RENDERED_POS=1 X4VR_W=5632 X4VR_ROLL_STEREO=1 X4VR_HEADLOOK_LEAD=1.0 X4VR_HEADLOOK_WRITE=1 X4VR_RES=2816x2816 X4VR_VR=1 X4VR_OFFAXIS=off X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_HEADLOOK=1 X4VR_EXTENSIONS=1 X4VR_SBS_LAYERS=2 X4VR_UI_WGATE=0.02 X4VR_POSE=1 X4VR_HEADLOOK_RAW=191 X4VR_HEADLOOK_KP=0.25 X4VR_POSELOG=1 X4VR_WATCH=1 X4VR_UI_WGATE_W1=0 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_PROJ_INVPROJ=1 X4VR_CAMDIFF=1 X4VR_LOG=/tmp/x4vr-take<n>.log X4VR_CAMSTAMP=1 X4VR_DECLARE_RENDERED_ROT=2 X4VR_MASK_PRESENT=1 X4VR_UI_FOV=1.0 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_CAMREAD=1 X4VR_CAMLOOP=1 X4VR_CURSOR_CANVAS=auto X4VR_CSM_COVER=0.663 X4VR_HEADLOOK_GAIN=0.115 ./launch/x4vr-launch.sh

## v0.9.2 — 2026-10-28 — the numpad orbits the external views again

The state of take 352 (code identical to commit 1db94d7). Patola: *"F2, F3,
F4 with no drift. So, it's fixed."*

* **Fixed:** in F2 (external) and F3 (target) the numpad no longer orbited
  the view around the ship or the target; it pitched once and froze.
  * The `x4vrmod_headlook` extension zeroed X4's `<lookaround><internal>`
    anglespeed for the whole session, so head-look's written angle would
    stay. The same speed drives those views' orbit.
  * Now X4 keeps its own value. The injector holds it at 0 only while the
    camera is in its cockpit/walking mode, and gives it back in every other
    view.
  * `X4VR_HEADLOOK_SPEED_EXT=1` restores the old extension.
* The play command is unchanged.

**Known limitations:** the external, external-static, target and cinematic
views (F2/F3/F4/F6) and the start menu show as a window.

## v0.9.1 — 2026-10-28 — the volumetric nebula in stereo

The state of take 349 (code identical to commit 46ae135). Patola:
*"Looked from many directions and in many views, from all of them the nebula
looked right. It didn't distort."*

* **Fixed:** in sectors with X4's volumetric fog (Matrix #79B, CEO's Doubt)
  the nebula sat in different parts of the sky in each eye and sheared when
  the head rolled.
  * The fog's ray march takes each pixel's direction by un-projecting the
    near plane.
  * The mod's per-eye inverse projection, right for every shader that
    rebuilds a position, turned that direction by ~18° in opposite senses
    per eye.
  * That module now keeps X4's own inverse projections. The fog has no
    stereo depth of its own, which suits a cloud kilometres away.
  * `X4VR_FOG_RAY_PER_EYE=1` restores the old behaviour.
* The play command is unchanged.

**Known limitations:** the external, external-static, target and cinematic
views (F2/F3/F4/F6) and the start menu show as a window, and the numpad does
not turn the F2/F3/F4 views.

## v0.9.0 — 2026-10-28 — playable in VR

The state of take 343 (`take-343-right-click`, code identical to commit
8ce88d5). Patola, after take 343: *"All the menus, interactions and movement
are good now ... I think the game is entirely playable in VR now."*

**What works**
* In-engine stereo with per-eye lighting at a 110° field, with stable
  shadows (X4 sizes its cascades by 1/tan; the fit is patched in memory).
* **Cockpit:** head-look writes X4's own free-look, so hover and targeting
  keep working. The HUD stays fixed to the cockpit. The mouse-steering
  target is fixed to the cockpit (steering A, the only mode since take 339).
* **Walking:** the head looks around independently; the mouse and arrows
  turn the body; W walks where the body faces. The mouse's up/down is left
  to the head.
* **Menus, all lined up with the cursor:**
  * the map and other full-screen menus;
  * the ESC menu;
  * the cockpit right-click menu, followed from X4's own table of open menus;
  * the compass menus;
  * on foot: interaction menus, NPC dialogues (the cursor starts at the
    options), teleporters, vendors (including Cancel back to the dialogue)
    and the crafting bench.
* WiVRn (default) or SteamVR. gamescope.

**How it touches the game**
* Your own config, input map and content files are opened read-only, once,
  and never written. The mod serves its own forks: `config-x4vrmod.xml`,
  `inputmap-x4vrmod.xml`, `content-x4vrmod.xml`.
* Three data-only extensions are enabled for the mod's sessions only:
  * `x4vrmod_lookaround` (look-around limits);
  * `x4vrmod_nosway` (camera sway off);
  * `x4vrmod_headlook` (holds X4's free-look follower).
* Two approved code patches, applied in memory to X4 9.00 (native Linux).
  Each is checked byte for byte first and refused on any other build:
  * the shadow-cascade fit (`X4VR_CSM_COVER`);
  * the walking camera (`X4VR_HEADLOOK_FOOT`).
* X4's own state is otherwise READ, never called, where X4's function has no
  null check: the steering line, the open-menu table, the conversation.
* Protected UI Mode is untouched; no Lua is installed or run.

**Known limitations (the next work)**
* The external, external-static, target and cinematic views (F2/F3/F4/F6)
  and the start menu show as a window, not the whole scene.
* In some sectors (Matrix #79B, CEO's Doubt) the skybox differs between the
  eyes.

**How to run** (X4VR_TAKE and X4VR_LOG name the log; the same line as
docs/known-good-runs.md, "Take 339 — THE SHIPPING LINE"):

    X4VR_TAKE=<n> X4VR_HEADLOOK_FOOT=1 X4VR_STEER=a X4VR_HEADLOOK_MENU_CENTRE=1 X4VR_HEADLOOK_LAGLOG=1 X4VR_CANVAS_M=1000 X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_STEREO=1 X4VR_HEADLOOK_PITCH_LIMIT=75 X4VR_BINDLESS_PATCH=1 X4VR_H=2816 X4VR_DECLARE_RENDERED_POS=1 X4VR_W=5632 X4VR_ROLL_STEREO=1 X4VR_HEADLOOK_LEAD=1.0 X4VR_HEADLOOK_WRITE=1 X4VR_RES=2816x2816 X4VR_VR=1 X4VR_OFFAXIS=off X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_HEADLOOK=1 X4VR_EXTENSIONS=1 X4VR_SBS_LAYERS=2 X4VR_UI_WGATE=0.02 X4VR_POSE=1 X4VR_HEADLOOK_RAW=191 X4VR_HEADLOOK_KP=0.25 X4VR_POSELOG=1 X4VR_WATCH=1 X4VR_UI_WGATE_W1=0 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_PROJ_INVPROJ=1 X4VR_CAMDIFF=1 X4VR_LOG=/tmp/x4vr-take<n>.log X4VR_CAMSTAMP=1 X4VR_DECLARE_RENDERED_ROT=2 X4VR_MASK_PRESENT=1 X4VR_UI_FOV=1.0 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_CAMREAD=1 X4VR_CAMLOOP=1 X4VR_CURSOR_CANVAS=auto X4VR_CSM_COVER=0.663 X4VR_HEADLOOK_GAIN=0.115 ./launch/x4vr-launch.sh
