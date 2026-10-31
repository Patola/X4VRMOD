# extension

The X4 extension that is part of X4VRMOD, kept here as its source of truth.

`launch/x4vr-launch.sh` with `X4VR_EXTENSIONS=1` (a default) copies it into
X4's `extensions/` folder when the installed copy differs, and the injector
enables it for the modded session only: it serves the profile's
`content.xml` from its own fork (`content-x4vrmod.xml`) with the extension
set in memory. The `content.xml` that vanilla X4 reads is never written, and
the manifest's `enabled="0"` keeps it off in a vanilla session.

Enabling a non-Egosoft extension marks saves made in that session as
modified. Since 2026-09-26 that is allowed (see "DIRECTIVE CHANGE" in
`docs/x4-quirks.md`).

`x4vrmod` changes, in `libraries/parameters.xml`:

* **the cockpit free-look limits,** 65/35 -> 180/75 degrees. The head-look
  servo has its own limits, `X4VR_HEADLOOK_YAW_LIMIT=180
  X4VR_HEADLOOK_PITCH_LIMIT=75` (the defaults), or it stops at 65/35 anyway;
* **X4's cockpit camera sway** (`<playercamera><input>`: the G-force lean
  and drift as the ship turns and accelerates) and the boost shake, set to
  zero. In VR the view moves only with the head.

Until v0.9.8 these were two extensions, `x4vrmod_lookaround` and
`x4vrmod_nosway`, with a third, `x4vrmod_headlook` (the look follower's
speed at zero for the whole session), dropped because it froze the external
views' numpad orbit (take 350); the injector holds that speed per camera
mode instead. The launcher removes the three old directories from X4's
folder.
