# X4VRMOD

A VR mod for **X4: Foundations** on native **Linux**, doing true in-engine
stereoscopic 3D by intercepting the game's own Vulkan rendering — a Vulkan
layer for stereo/cameras/VR, an `LD_PRELOAD` injector for game state and
config, and a cursor shim so menus and dialogs are usable in VR.

> **Status: early development (v2 rewrite).** The full design and phased
> roadmap are in **[DESIGN.md](DESIGN.md)**. The earlier proof of concept
> (OpenTrack + vkShade/SuperDepth3D DIBR) is preserved at tag `v0.1` and was
> abandoned as a dead end.

## Approach in one paragraph

Run X4 inside **gamescope** at a 2:1 super-resolution (starting 2816×1408 =
two 1408×1408 eyes). A Vulkan layer renders the scene **twice per frame**,
once per eye with its own camera and **per-eye lighting**, into a
side-by-side image — validated on the flat monitor first, then delivered to
the headset via **OpenXR / WiVRn** with 6DOF. An injector forces the
resolution and disables VR-hostile effects **in memory** (no user setting
changes) and reads X4's game mode so menus/map switch to comfortable
world-locked quads. See [DESIGN.md](DESIGN.md).

## License

**GPLv3** (see [LICENSE](LICENSE)) with a **Section-7 additional permission**
([LICENSE.exception](LICENSE.exception)) allowing linking / loading /
hooking / injecting / overlaying against the proprietary X4: Foundations
Linux binary and its shipped libraries — **GNU/Linux only** (no exception for
macOS or Windows).

## Credits

Developed with Claude Code (Claude Opus 4.8). Prior art for game-state
introspection: Beko's `X4-rest-server`. X4: Foundations © Egosoft GmbH.
