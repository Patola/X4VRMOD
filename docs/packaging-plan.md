# Packaging and launcher plan

A plan, not code. Decisions recorded 2026-10-31 with Patola, after v0.9.6.
Nothing here is built yet. Items marked **to verify** are assumptions until a
test or a reading of X4 settles them.

## Decisions

| # | Question | Decision |
|---|---|---|
| 1 | What comes first | **The shipping line becomes the default.** It is the first wrap-up task, before any package or launcher. |
| 2 | Launcher architecture | **A core plus a command-line tool, with the Qt6/QML window on top.** |
| 3 | Using it with the headset on | **Desktop setup, and a Play button that waits for the headset.** No in-game panel for now. No launcher window in VR. |
| 4 | Resolution | **Only the launcher sets it.** X4's in-game resolution menu is not the control in VR. |
| 5 | X4 extensions | **One extension, `x4vrmod`**: the look-around limits and the sway removal in one settings diff. **`x4vrmod_headlook` is dropped**, with its bisect switch `X4VR_HEADLOOK_SPEED_EXT`. |

Clarifications from Patola:
* **DLSS:** the development PC has an AMD GPU, so no DLSS here. A friend with
  an NVIDIA RTX 5090 will test it once the launcher can offer it.
* **Debug and bug reporting:** design the debug feature properly, and think
  about automating bug reports (below).
* **SteamVR's desktop view** may not be a problem at all. SteamVR users are
  used to peeking through the headset at the flat screen. Nothing VR-side is
  planned for it.

## Phase 0 — the shipping line becomes the default

Today v0.9.6 needs about 50 environment variables
(docs/known-good-runs.md, "Take 375"). A package cannot ship that, and a
launcher should not have to know it.

* **`launch/x4vr-launch.sh` with no variables behaves like v0.9.6.**
  * Every behaviour knob in the shipping line gets that value as its default,
    in the code that reads it (injector, layer, launcher).
  * An explicit setting still wins, so every recorded take's command line
    still means what it meant. Write `=0` to turn a feature off for a bisect.
  * The defaults move in one change, with a table in docs/releases.md
    (variable, old default, new default). Two defaults have moved under stored
    command lines before (X4VR_PROJ_INVPROJ, X4VR_UI_FOV).
* **Three kinds of variables**, after the move:
  * **Settings a user changes:** resolution, upscaler, field of view, IPD,
    VR runtime, lean on/off. The launcher's settings map to these.
  * **Internals:** shear, canvas, w-gate, servo gains, patch switches. They
    keep their variables for development and do not appear in the launcher.
  * **Instruments:** LAGLOG, POSELOG, CAMSTAMP, CAMDIFF, HOVERLOG, FOOTLOG,
    WATCH, PTRWATCH, the dumps. Off by default; debug mode turns them on.
* **The X4 path:** the launcher's built-in path is this PC's Steam library.
  It becomes detection (Phase 2's core), with X4VR_GAME as the override.
* **The scorer must read the effective values, not the command line.** It
  gates on intent from the `env: run =` line. Once defaults move, a knob a
  run did not write is still on, so the injector and layer must log one
  `effective:` line with every setting's value and where it came from
  (default, environment, profile). The scorer reads that line, and the census
  gets cases for each source.
* **Acceptance:** a take run with only `X4VR_TAKE` and `X4VR_LOG` set scores
  like take 375, and Patola sees no difference in play.

## Phase 1 — packages

### What gets installed

| What | Where (system package) | Notes |
|---|---|---|
| Vulkan layer `libVkLayer_X4VR_core.so` | `/usr/lib/x4vrmod/` | |
| Layer manifest `VK_LAYER_X4VR_core.json` | `/usr/share/vulkan/explicit_layer.d/` | **Explicit**: loaded only when our launcher names it, never in another program. |
| Injector `libx4vr_inject.so` | `/usr/lib/x4vrmod/` | Preloaded by our launcher only. |
| Launcher `x4vrmod` (CLI), `x4vrmod-config` (GUI) | `/usr/bin/` | Phases 2 and 3. Until then, `x4vr-launch.sh` as `/usr/bin/x4vrmod-launch`. |
| Extensions (lookaround, nosway, headlook) | `/usr/share/x4vrmod/extensions/` | Copied into place per user by the launcher, never by the package. |
| OpenXR probe (today `x4vr_test_xr_probe`) | `/usr/lib/x4vrmod/` | The launcher's "is the headset answering" check. |
| Docs, licence | `/usr/share/doc/x4vrmod/`, `/usr/share/licenses/x4vrmod/` | GPL-3.0 with the linking exception. |

The scorer and the take tools (`tools/`) stay out of the main package. The
bug-report feature needs `score_run.py`, so it ships under
`/usr/lib/x4vrmod/` as a diagnostic, with python3 as an optional dependency.

### Build choices
* **Link libstdc++ and libgcc statically into the layer and the injector**,
  and export only what must be exported. Both run inside X4's process,
  possibly under Steam's runtime libraries, where a mismatched C++ runtime is
  a classic crash.
* **Embed the version** (`git describe`) in both libraries and print it on
  their first log line. Bug reports need it, and today nothing records it.
* **Build on the oldest supported distro** in CI (e.g. Ubuntu 22.04), so the
  tarball runs on newer glibc too.

### Formats
1. **Arch first:** a PKGBUILD on the AUR, `x4vrmod` (release tags) and
   `x4vrmod-git`. Dependencies: `vulkan-icd-loader`, `gamescope`, `openxr`
   (the loader is dlopen'd, so it is optional at build time), `libnotify`
   optional, `qt6-declarative` for the GUI (a split package, so the CLI
   installs without Qt).
2. **deb and rpm in GitHub Actions:** CPack's DEB and RPM generators from the
   same CMake build, in Ubuntu, Debian and Fedora containers. Also a plain
   `.tar.zst` with an install script for everything else.
3. **The Arch package in CI too,** in an archlinux container with makepkg,
   so the AUR PKGBUILD is tested on every tag.
4. **Releases:** a tag `vX.Y.Z` triggers the workflow, which attaches all
   packages to a GitHub Release whose body is that version's section of
   docs/releases.md.
5. **Not planned:** Flatpak and AppImage. A Vulkan layer plus an LD_PRELOAD
   into another program's process does not fit a sandboxed bundle.

CI cannot run X4 (proprietary), so CI runs the unit tests and the census.
The tests that read X4's binary skip there, as they already do when the
binary is missing.

### The X4 build is part of compatibility
* Every code patch checks X4's bytes and refuses on any other build. After an
  X4 update the mod still starts, but HUD clicks, the steering pick, walking,
  the shadow fix and the external-view head turn off.
* So each release states the X4 builds it supports, by binary hash. The
  launcher checks the hash and says which features are off on an unsupported
  build, rather than letting the player find out in the cockpit.
* **To verify — GOG:** whether GOG's X4 binary is byte-identical to Steam's
  9.00. Every patch was checked against Steam's. If not, GOG needs its own
  signatures or stays unsupported.
* **To verify — Steam's container:** whether Steam runs native X4 inside a
  Steam Linux Runtime container. That decides whether `x4vrmod run %command%`
  works from Steam's launch options as it does from a terminal.
* **To verify — extensions in the user folder:** whether X4 9.00 loads
  extensions from `~/.config/EgoSoft/X4/<id>/extensions/`. If it does, the
  launcher puts them there and never writes into Steam's game folder (today
  `X4VR_EXTENSIONS=1` copies them into the install).

## Phase 2 — the core and the command-line tool

One C++ core library, used by a command-line tool and by the GUI. Every
check and action exists once, in the core; the GUI only displays it.

    x4vrmod doctor              every check, one line each, exit code = worst
    x4vrmod install             per-user: layer registration (if no system
                                package), extensions, profile defaults
    x4vrmod uninstall [--all]   the reverse; --all also removes the mod's
                                config forks, after confirming
    x4vrmod run [-- %command%]  launch with the saved profile; with %command%
                                it is a Steam/Heroic launch-option wrapper
    x4vrmod config get|set      the profile, scriptable
    x4vrmod report              build a bug-report bundle (below)
    x4vrmod probe xr|vulkan|x4  the individual probes, for debugging the checks

* **The profile** is `~/.config/x4vrmod/profile.toml`: the user settings
  only. `run` turns it into the environment, adding the internals' defaults
  only where the code does not already have them.
* **Steam's launch options become one short line**, `x4vrmod run %command%`,
  shown by the GUI with a copy button. A settings change needs no edit there.
* **Checks** (`doctor`), each with a status, a one-line reason and, where
  possible, a fix:
  * X4 found: Steam's `libraryfolders.vdf`, GOG and Heroic paths, X4VR_GAME.
  * X4 build supported (the hash).
  * X4 profile folder found; the mod's forks present or created on first run.
  * gamescope installed (version); a Wayland session.
  * Vulkan: the GPU, driver version, Vulkan 1.3, multiview, at least 32
    descriptor sets (the layer uses set 31).
  * The layer registered and loadable, with the same version as the injector.
  * Extensions deployed and current.
  * OpenXR: the active runtime (`XR_RUNTIME_JSON`, then
    `~/.config/openxr/1/active_runtime.json`, then the system one).
  * **The runtime answering and a headset connected,** told apart: an
    instance that will not create (no runtime running) versus a system that
    is not available (runtime up, no headset).
  * Free space for logs, in debug mode only.
* **IPD from the headset** where the runtime reports the eye poses, instead of
  the fixed 0.064 m. The setting stays as an override.

## Phase 3 — `x4vrmod-config`, the Qt6/QML window

A small window (about 640×480), three areas:
* **Status:** the `doctor` checks, green/amber/red, a fix button where the
  core has one. A refresh every few seconds while open, so starting WiVRn or
  SteamVR turns its row green without a click.
* **Settings:** resolution, upscaler, field of view, IPD (auto/manual), lean,
  VR runtime (auto/WiVRn/SteamVR), debug mode. Each with a one-line
  explanation of what it does, by behaviour.
* **Play:** a large button, enabled only when every required check is green.
  * Pressed, it waits for the headset: it shows "put on the headset" and
    starts X4 when the runtime reports the headset worn (OpenXR session state,
    or the proximity sensor where the runtime exposes it), with a "start
    now" override.
  * Beside it: "Copy Steam launch options" (`x4vrmod run %command%`),
    "Install/repair", "Uninstall".

The window is not needed once the game runs. SteamVR's choppy desktop view
is acceptable for this; nothing is drawn in VR for the launcher.

## Resolution

* **The launcher owns it** (decision 4). The layer, the eye images and
  gamescope's virtual screen are all sized at launch, and X4 applies its own
  menu's choice immediately, so an in-game change breaks the session.
* **Offered relative to the headset,** not as monitor sizes:
  * the probe reads the runtime's recommended per-eye size
    (`xrEnumerateViewConfigurationViews`); WiVRn asked for 3096×3243 here;
  * the setting is a scale of it: 75 / 100 / 125 / 150 %;
  * the launcher derives X4VR_W, X4VR_H (two eyes side by side) and
    X4VR_RES.
* **Square eyes for now.** The pipeline renders a square, symmetric field.
  Non-square eyes (the headset's own aspect) are possible in principle, and
  no take has run one. That is a later test, not a launcher feature.
* **Limits are checked, not discovered:** the launcher refuses a size the GPU
  or the runtime cannot take (Vulkan `maxImageDimension2D`; whatever limit
  explains the SteamVR report below, once it is known).
* **Warn about SteamVR at high resolution.** Patola, 2026-10-31: the
  4224²-per-eye line with FSR gave "a jumbled mess of graphics" under
  SteamVR. The same line is clean under WiVRn, and stepping the resolution
  down fixed it under SteamVR. The launcher shows a warning when SteamVR is
  the runtime and the resolution is above the last size known to work there
  (to be measured). The release notes say the same for players.
* **The lower HUD's placement depends on the height** through X4's own
  formula, unless X4's HUD scale is off (docs/x4-quirks.md, "The lower HUD
  moves with the resolution"). If the HUD-scale test holds, the mod serves it
  off in VR, and every resolution the launcher offers keeps the same HUD.
* **X4's in-game menu:** left alone for now. Narrowing its list to the current
  mode (the injector already sits on SDL) is a possible later polish.

## Upscaling: FSR and DLSS

* X4 selects its upscaler through `<antialiasing>`, which the mod serves
  (`X4VR_AA`). The FSR modes are known: `fsr`, `fsr_ultra_quality`,
  `fsr_quality`, `fsr_balanced`, `fsr_performance`. FSR3 worked in VR in
  take 163, an old build.
* **The launcher offers:** off, FSR quality, FSR balanced, FSR performance.
  With an upscaler, the resolution setting is the output; the internal render
  is X4's (quality = output / 1.5 per axis).
* **DLSS — to verify, then to test on an RTX card:**
  * X4's binary carries DLSS, but the `<antialiasing>` list read from X4's
    Lua has only `dlaa` (anti-aliasing, no upscaling). The tag that selects
    DLSS upscaling has to be read from X4's Lua first.
  * Offered only when the GPU is NVIDIA RTX (Vulkan vendor ID 0x10de and the
    DLSS requirements met).
  * Tested by Patola's friend with an RTX 5090: the debug build and a
    protocol, scored like a take.
* **Frame generation stays off,** for FSR and DLSS alike. An interpolated
  frame has no head pose and adds latency, which is harmful in VR.
* Before the launcher offers FSR, it needs a take on the current build: both
  eyes' lighting identical, no smearing, and the frame rate at each mode.

## Debug mode and bug reports

### Three levels
| Level | What runs | Cost |
|---|---|---|
| **Normal** | The event log: start-up, settings, patches applied or refused, mode changes, warnings. | None measurable. |
| **Debug** | Normal plus the instruments a take uses (lag samples, pose log, hover log, foot census), at the shipping line's take settings. | A few percent; the log grows by MB per minute. |
| **Capture** | Debug plus matrix dumps and PNG frame dumps on request. | Large; for one reproduction, then off. |

### A flight recorder, always on
* The high-rate data the instruments log (head pose, X4's camera, pick
  shifts, cursor, frame timing) is kept in memory for the last ~30 s, in
  every level, and written nowhere in normal play.
* It is flushed to disk on:
  * **a mark key** (a key X4 leaves free, chosen in the launcher). The player
    presses it the moment something looks wrong. The flush also logs a
    `mark:` line, so the report points at the moment;
  * **a crash** (a signal handler that writes the ring before the core);
  * **a refused patch or an internal FAIL condition.**
* This is what makes reports from normal play useful: most bugs (Patola's
  weapon, the flickering) happen when nobody is recording.
* Core dumps: the launcher keeps them on (`ulimit -c`), and the report
  includes systemd-coredump's backtrace when one exists for X4.

### The report bundle (`x4vrmod report`, or the GUI's button)
* **What goes in:**
  * the X4VRMOD version (embedded), the X4 build hash, Steam or GOG;
  * the system: kernel, distro, GPU, driver and Mesa versions, gamescope's
    version, the VR runtime and its version (the log already has WiVRn's),
    the headset's name from OpenXR;
  * the profile and the effective settings line;
  * the session log(s), the flight-recorder flushes with their marks;
  * `doctor`'s output;
  * **`score_run.py` run on the log**, so its FAIL lines are an automatic
    first diagnosis;
  * optional: the frame dump at a mark, a short note from the player, a video
    file the player attaches.
* **Scrubbed before anyone sees it:** home paths become `~`, the X4 profile
  number (a Steam account ID) and the user and host names are replaced.
  **The player sees the whole bundle before it goes anywhere.**
* **Delivery, never automatic:**
  * the bundle is a `.tar.zst` in `~/x4vrmod-reports/`;
  * "Report on GitHub" opens a new issue in the browser, with the scrubbed
    summary (versions, settings, the scorer's FAIL lines) in the body. The
    player drags the bundle in. With `gh` installed and logged in, the core
    can create the issue and attach it, after confirming;
  * no telemetry, no upload server.
* **A GitHub issue form** (`.github/ISSUE_TEMPLATE/bug_report.yml`) with the
  same fields, for reports made without the tool.
* **Known problems answered before a report:** the scorer already recognises
  several (an unsupported X4 build's refused patches, a missing runtime).
  `report` shows "this is known: …" with the fix and still lets the player
  file.

### For development
The same bundle serves takes: a take is a debug-level run plus a protocol,
and the scorer is the same one. Patola's play sessions can run at normal
level with the mark key, instead of a take's full command line.

## One extension, `x4vrmod` (decision 5)

* **Today there are three** (`extension/`):
  * `x4vrmod_lookaround`: cockpit look-around limits 65/35 -> 180/75 degrees.
    Always enabled in a mod session.
  * `x4vrmod_nosway`: the camera's G-force sway and boost shake off. Always
    enabled.
  * `x4vrmod_headlook`: look-around return speed (`anglespeed`) 0 for the
    whole session. Obsolete since take 351: the injector holds that speed at
    zero only in camera mode 0 (cockpit, walking) and gives X4 its value back
    elsewhere, because the extension froze the numpad orbit of F2/F3 (take
    350). Served only with `X4VR_HEADLOOK_SPEED_EXT=1`.
* **For release: one extension, `x4vrmod`,** with both remaining diffs in one
  `libraries/parameters.xml`. `x4vrmod_headlook` and its switch are removed,
  not merged: folding them in would bring the frozen orbit back.
* **Its manifest:**
  * `enabled="0"`, so a vanilla session that finds it leaves it off. The
    injector's content fork turns it on for mod sessions only.
  * `save="0"`, so no save depends on it.
  * Its `version` is the mod's, and the launcher's "extensions current" check
    compares the two.
* **Lost:** turning the look-around and the sway on and off separately. Both
  are always wanted in VR; a bisect can edit the file in a dev build.
* **Migration:** `x4vrmod install` and `uninstall` also remove the three old
  directories from X4's `extensions/` (where `X4VR_EXTENSIONS=1` deployed them
  until now) and drop the three names from the content fork.
* **Changes together:** the injector's extension table (`kOurExtensions`),
  the head-look write's checks that name `x4vrmod_lookaround`, the scorer's
  `extensions` and `hwrite` lines, the census, `launch/deploy-extensions.sh`.
* **Location:** wherever "extensions in the user folder" (Phase 1, to
  verify) decides.

## Not planned (now)
* An in-game panel drawn by the layer (decision 3).
* A launcher window in VR, or SteamVR/OpenXR overlays.
* Changing X4's in-game resolution menu.
* Frame generation.
* Flatpak and AppImage.

## Order of work
1. Phase 0: defaults flip, the `effective:` line, the scorer reading it.
   Accepted by a take with almost no variables.
2. Version embedding and static runtime; the single `x4vrmod` extension; the
   Arch PKGBUILD from the current launcher script.
3. Phase 2's core and CLI: `doctor` first (it answers the "to verify" items
   on real machines), then `run`, `install`/`uninstall`, `report` with the
   flight recorder.
4. CI: tests, census, deb/rpm/tarball/Arch packages on tags.
5. Phase 3's Qt6/QML window over the core.
6. DLSS: read the tag, wire it, the RTX 5090 test.
