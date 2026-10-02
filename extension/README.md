# extension

X4 extensions that are part of X4VRMOD, kept here as their source of truth.

`launch/x4vr-launch.sh` with `X4VR_EXTENSIONS=1` copies each directory into
X4's `extensions/` folder when the installed copy differs, and the injector
enables them for the modded session only: it serves the profile's
`content.xml` from its own fork (`content-x4vrmod.xml`) with these extensions
set in memory. The `content.xml` that vanilla X4 reads is never written.

Enabling a non-Egosoft extension marks saves made in that session as
modified. Since 2026-09-26 that is allowed (see "DIRECTIVE CHANGE" in
`docs/x4-quirks.md`).

| extension | what it changes |
|---|---|
| `x4vrmod_lookaround` | cockpit free-look limits 65/35 -> 180/75 degrees (`libraries/parameters.xml`). Pair with `X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_HEADLOOK_PITCH_LIMIT=75`, or the head-look servo stops at 65/35 anyway. |
| `x4vrmod_nosway` | X4's cockpit camera sway (`<playercamera><input>`: the G-force lean and drift as the ship turns and accelerates) and the boost shake set to zero. In VR the view moves only with the head. Enabled with the extensions, like the look-around limits. |
| `x4vrmod_headlook` | cockpit look-around follower speed (`anglespeed`) 200 -> 0, so the view holds the angle `X4VR_HEADLOOK_WRITE=1` writes instead of springing home. **Disabled by default** (`enabled="0"` in its manifest) and forced on only in a write-mode session, off in every other; while on, the numpad camera keys do not turn the cockpit view. |
