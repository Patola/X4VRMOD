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

Nothing here is published until the repository is pushed.

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
