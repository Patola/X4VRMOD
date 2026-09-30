# extension

X4 extensions that are part of X4VRMOD, kept here as their source of truth.

`launch/x4vr-launch.sh` with `X4VR_EXTENSIONS=1` copies each directory into
X4's `extensions/` folder when the installed copy differs, and the injector
enables them for the modded session only: it serves the profile's
`content.xml` from its own fork (`content-x4vrmod.xml`) with these extensions
set `enabled="true"` in memory. The `content.xml` that vanilla X4 reads is
never written.

Enabling a non-Egosoft extension marks saves made in that session as
modified. Since 2026-09-26 that is allowed (see "DIRECTIVE CHANGE" in
`docs/x4-quirks.md`).

| extension | what it changes |
|---|---|
| `x4vrmod_lookaround` | cockpit free-look limits 65/35 -> 180/75 degrees (`libraries/parameters.xml`). Pair with `X4VR_HEADLOOK_YAW_LIMIT=180 X4VR_HEADLOOK_PITCH_LIMIT=75`, or the head-look servo stops at 65/35 anyway. |
