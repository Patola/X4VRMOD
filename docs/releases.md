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
