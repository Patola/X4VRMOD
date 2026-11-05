# Packaging and launcher plan

Decisions recorded with Patola from 2026-10-31 (after v0.9.6), the launcher's
design on 2026-11-05 (after v0.9.9). Items marked **to verify** are assumptions
until a test or a reading of X4 settles them. Built so far: Phase 0, the version
in every log, the static runtime, the single extension.

## Decisions

| # | Question | Decision |
|---|---|---|
| 1 | What comes first | **The shipping line becomes the default.** It is the first wrap-up task, before any package or launcher. |
| 2 | Launcher architecture (Widgets since 2026-11-05) | **A core plus a command-line tool, with a Qt 6 Widgets window on top.** Widgets follow the desktop theme by default, where QML only matches on KDE; help needs only `QTextBrowser`, so `qt6-base` is the only dependency. |
| 3 | Using it with the headset on | **Desktop setup, and a Play button that waits for the headset.** No in-game panel for now. No launcher window in VR. |
| 4 | Resolution | **Only the launcher sets it.** X4's in-game resolution menu is not the control in VR. |
| 5 | X4 extensions | **One extension, `x4vrmod`**: the look-around limits and the sway removal in one settings diff. **`x4vrmod_headlook` is dropped**, with its bisect switch `X4VR_HEADLOOK_SPEED_EXT`. |
| 6 | Default upscaler (2026-10-31; revised twice) | **No automatic upscaler in the defaults:** `none` at every eye size unless `X4VR_UPSCALE` asks. **The graphical launcher pairs them visibly** (Phase 3, *Settings*). The history: a defaults rule switched FSR on above 2816² per eye, first FSR 3 Balanced, then FSR 1 Quality (take 385: 13.24 ms against 16.72 at 4224², judged equal to look at). After take 386 Patola dropped it, because it made 3072² native impossible without knowing to override it. **In the launcher (2026-11-05):** going above 3200² per eye with no upscaler selects FSR 1 Quality, and the player can change it back. |
| 7 | When to package (2026-10-31) | **The packages come with the core and its CLI (Phase 2),** which installs into system directories and activates the mod (extensions, Vulkan layer). Version embedding and the single extension stay early: they are easy. |
| 8 | Is X4 compatible? (2026-11-05) | **No checksum.** The launcher runs the mod's own byte checks against the X4 file and reports per feature, so a hotfix that leaves the code in place stays green with no mod release. See *The X4 build is part of compatibility*. |
| 9 | An unknown X4 build (2026-11-05) | **Amber "Play anyway",** naming what turns off. Play is green only when everything is met, and grey only when it can't launch. |
| 10 | Who creates the custom config files? (2026-11-05) | **The mod, as today; the launcher never does.** Steam launch options and the command line bypass the launcher, so the mod must do it anyway, and it learns the real profile path when X4 opens it. Settings reach the mod as environment variables, served in memory. The launcher only reports and deletes the files. |
| 11 | Distribution and names (2026-11-05) | **Packages and a zip.** CLI `x4vrmod`, window `x4vrmod-launcher`, zip data directory `x4vrmod-data/`. |
| 12 | Which VR runtime to suggest (2026-11-05) | **WiVRn.** The window's button and text put it first whenever it's installed; SteamVR is offered first only when there's no WiVRn, as for a headset wired to the PC. |

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

**As built (2026-10-31; take 385 is the acceptance).** The defaults live in
one table, `launch/defaults.conf`: 44 rows generated from take 375's logged
run line minus its instruments, each marked `setting` or `internal`.
`launch/x4vr-defaults.sh` applies it at the top of the launcher. That one
script will serve the core and the packages too.
* **Where the defaults live.** The launcher exports every variable in the
  table that is not set. The C++ defaults do **not** move, unlike the plan
  above. The GPU harness drives the layer directly and its control cases
  rely on "unset = off"; moving ~45 defaults in the code would have changed
  every one of them. The behaviour Patola sees is the same.
* **Derived values:** `X4VR_W` = 2 × `X4VR_H`, `X4VR_RES` = `X4VR_H`×`X4VR_H`.
  `X4VR_UPSCALE` is a plain row, `none` (decision 6).
* **What a run records.** The injector's `env: run =` line carries every
  value, defaulted or not, so the scorer's intent gates read them unchanged.
  Replaying an old run line reproduces it whatever the table says later.
  `env: defaults: N from launch/defaults.conf: …` says which came from the
  table.
* **The escape hatch.** `X4VR_DEFAULTS=0` applies nothing: the code's own
  defaults, as every take before Phase 0 ran.
* **Tests:** tests/run-launch-defaults.sh (14 cases); scorer `env` line and
  census.
* **Not done yet:** the "three kinds" are only marked in the table, and the
  X4 path is still this PC's (Phase 2).

## Phase 1 — packages

### What gets installed

| What | Where (system package) | Notes |
|---|---|---|
| Vulkan layer `libVkLayer_X4VR_core.so` | `/usr/lib/x4vrmod/` | |
| Layer manifest `VK_LAYER_X4VR_core.json` | `/usr/share/vulkan/explicit_layer.d/` | **Explicit**: loaded only when our launcher names it, never in another program. |
| Injector `libx4vr_inject.so` | `/usr/lib/x4vrmod/` | Preloaded by our launcher only. |
| Launcher `x4vrmod` (CLI), `x4vrmod-launcher` (GUI) | `/usr/bin/` | Phases 2 and 3. Until then, `x4vr-launch.sh` as `/usr/bin/x4vrmod-launch`. |
| Extension `x4vrmod` | `/usr/share/x4vrmod/extensions/` | Copied into place per user by the launcher, never by the package. |
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
   optional, `qt6-base` for the GUI (a split package, so the CLI installs
   without Qt).
2. **deb and rpm in GitHub Actions:** CPack's DEB and RPM generators from the
   same CMake build, in Ubuntu, Debian and Fedora containers. Also a **zip** for
   everything else (*Phase 3*, *Install and uninstall*).
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
* **The version can be read, but it can't decide this.** `version.dat` beside
  the binary holds `900`, and Steam's appmanifest gives the `buildid`; the
  launcher shows both. But the binary is non-relocatable (`EXEC`) and about 330
  references in the injector are fixed addresses, and a second build of the same
  9.00 does move them (GOG's, below).
* **So the launcher asks the bytes, with no checksum** (decision 8). It runs the
  mod's own site checks (the `verify_sites` templates, over a file reader that
  maps addresses through the ELF LOAD segments) and reports per feature:
  head-look, walking, external views, HUD clicks, steering, menus, shadows.
  All match: green. Some don't: amber, naming them. A different major version
  in `version.dat`: amber, "not tested".
* **Later, to survive hotfixes and GOG: signature search.** Each site found by a
  unique byte pattern with its displacements wildcarded, requiring exactly one
  match or refusing. As safe as today, and it makes GOG work without separate
  tables. Real work (~330 references, plus stubs that hard-code addresses).
* **GOG's X4 9.00, measured 2026-11-05 (never launched, binary only).** Heroic puts
  it at `<install_path>/game/X4`, and it doesn't link `libsteam_api`.
  * Not byte-identical: the same 28,644 exports, 53 KB less code, and all 8 feature
    groups differ. So today it's amber with every feature off.
  * It looks like the same compile with the Steam code taken out. Of the 164 code
    addresses in our sources, 159 are found by their surrounding bytes, with 20 or
    more unique 8-byte windows agreeing, at 26 different offsets (functions are in a
    different order in places, so no single shift maps one onto the other). Of those
    159, 109 start with the same 16 bytes. In the 10 of the other 50 inspected, only
    addresses inside instructions differ. That's what signature search wildcards.
  * Not strongly found: `0xb7f1f0`, `0xbb0860`, `0xc02dc0` (a menus site),
    `0x1de5df0`, `0x65a000`. To look at; they may just be short functions.
  * It starts the same way: `testandlaunch` is the same file in both copies, ending
    in `./X4 "$@"` with `lib` on `LD_LIBRARY_PATH`. Nothing GOG-specific.
  * **Still to verify, after one vanilla launch:** where its profile folder is.
* **Steam's container: `x4vrmod run %command%` works** (take 404: PASS, its log in
  `~/.local/state/x4vrmod/logs/`).
* **To check later (Patola, take 404):** started from Steam, a brief Egosoft logo
  plays, which our direct launch doesn't show. Steam's `%command%` is a plain `./X4`
  (`env: argv = ./X4` in the log), and wrapper mode doesn't add the
  `-skipintro -nocputhrottle -nosoundthrottle` that direct mode does. The two throttle
  flags are missing with it; the take scored PASS and Patola saw nothing else wrong.
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
  * X4 patches match (the bytes, per feature; decision 8).
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

## Phase 3 — `x4vrmod-launcher`, the Qt 6 Widgets window

About 800 wide, under an 800×250 banner cut from Patola's picture (he chose it on
2026-11-05, replacing a banner drawn here; its stylised X⁴ resembles Egosoft's logo, so the
window's version line, the About box and the help all say unofficial, not affiliated with
Egosoft):

    +----------------------------------------------------------------------+
    |                 [ banner 800x200: "X4 VR Mod" artwork ]              |
    +----------------------------------------------------------------------+
    | Game  [Steam - /nvme/SteamLibrary/.../X4 Foundations        v] [Browse]
    |                                                                      |
    | Eye resolution                 | Ready to play                       |
    | [2816 x 2816 (default)     v]  | (ok) X4 found    9.00, build 23660954|
    | Upscaler                       | (ok) Steam running                  |
    | [None (full resolution)    v]  | (ok) gamescope installed     3.17.0 |
    | Renders 2816 x 2816.           | (x)  Headset not connected  [Retry] |
    | Eye distance                   | (ok) Vulkan layer installed  v0.9.9 |
    | [64.0 mm                   ^]  | (ok) Mod installed                  |
    | [x] Lean in the cockpit        |                                     |
    |----------------------------------------------------------------------|
    | [Help] [Launch options] [Report a bug] [More v]  1 to fix  [  PLAY  ]|
    | v0.9.9 - supports X4 9.00                                            |
    +----------------------------------------------------------------------+

* **Play:** green "Play" when every condition is met; amber "Play anyway" when it
  can launch with features off; grey "Play" when it can't, with the reason beside
  it. While running, "X4 is running"; afterwards, a line about the last session.
* **Ready to play:** the core's checks. A failed row carries its fix: Install,
  Start Steam, Start WiVRn or SteamVR, Retry, Browse. Rows refresh while the window
  is open, so starting WiVRn turns its row green without a click (see *The VR
  probe*).
* **More:** Play with debug log, Open logs, Add to application menu (zip only),
  Advanced (field of view, extra environment variables), Reset to defaults, Uninstall,
  About. Uninstall is here so it can't be hit by accident.
* **WiVRn is what we suggest** (Patola, 2026-11-05), and the window says so: with WiVRn
  installed the VR row's button is Start WiVRn, even when SteamVR is the active runtime.
  Start SteamVR appears only without WiVRn, because a headset wired to the PC has no other
  way. His reasons, in the help: a clearer picture than SteamVR's on the same headset, a
  codec to choose, and no jumbled picture at 4224.
* **Eye distance is manual for now:** the mock's "Automatic, from headset" needs layer work.
  `configured_ipd()` goes into shear constants while pipelines are built, and OpenXR only
  reports it with a session.
* **Launch options:** copyable Steam line (`x4vrmod run %command%`) and terminal
  line, with a picture of where they go in Steam.
* **Help:** `QTextBrowser` over HTML and PNGs in the data directory. Each section
  says what and why; each checklist row links to its section. Patola's suggestion: a
  table of the common headsets (Quest 3, Quest 3S, Quest 2, Valve Index, Steam Frame,
  HTC Vive and others) with low, mid and high settings for each, from their
  resolutions. Their numbers to be checked when it's written; the probe reports what
  the connected headset asks for (a Quest 3 on WiVRn: 2889 x 3026 per eye).
* Later, not in the first version: waiting until the headset is worn (it needs an
  OpenXR session), update checks, translations.

### Finding X4
* **Steam:** `~/.steam/root/steamapps/libraryfolders.vdf` lists the libraries and
  marks which holds `392160`; `appmanifest_392160.acf` gives `installdir` and
  `buildid`. No mount scanning. Also the Flatpak Steam path.
* **GOG:** `~/GOG Games/*` and Heroic's `gog_store/installed.json`; otherwise
  Browse, which takes a folder or a file and looks for `X4`, then `game/X4` (Heroic's
  layout, confirmed 2026-11-05).
* **The profile folder** is never named in the code: X4 asks `XDG_CONFIG_HOME`, then
  `HOME`, and the folder in `EgoSoft/X4/` is the player's own (Steam's is their account
  number). The mod puts its forks beside whatever file X4 opens, and uninstall looks in
  `EgoSoft/X4/` and every folder in it, under both.
* **Proton:** picking `X4.exe`, or a folder holding only that (Steam with forced
  Proton), explains that the mod is native Linux and can't work there.
* **Steam running** (Steam copy only): `~/.steam/steam.pid` and a live
  `/proc/<pid>/exe`, because the file outlives Steam. `SteamAppId` is set for the
  Steam copy only.

### The VR probe
`x4vrmod probe xr` runs in a child process with a timeout, so a broken
runtime can't hang or crash the window. Instance, `xrGetSystem`, view sizes,
destroy; no session. It reuses `common/x4vr_xr.hpp` and tells apart: loader
missing; no runtime; runtime up but no headset; ready, with the headset's name and
recommended size. `~/.config/openxr/1/active_runtime.json` is a cheap first step:
here it exists only while WiVRn runs.

**WiVRn with no headset gives no answer at all** rather than
`XR_ERROR_FORM_FACTOR_UNAVAILABLE`: the probe times out at 5 s, and answers at once
with a headset connected (2026-11-05). So a timeout from WiVRn reads as "Headset not
connected", and a timeout from anything else keeps its generic wording.

**Asking a SteamVR that isn't running starts its `vrserver`** (2026-11-05, SteamVR's
own `xrclient_x4vrmod.txt`: "Starting vrserver process"). With no headset connected it
refused the client (`VRInitError_Driver_WirelessHmdNotConnected`, which reaches us as
`XR_ERROR_RUNTIME_FAILURE`) and was killed 5 s later, so no window appeared and nothing
stayed. With a headset it may well stay up. So `doctor` and the window never ask a
SteamVR that isn't running: they say "SteamVR isn't running" from the runtime file.
Not yet tried with SteamVR running.

### Install and uninstall
**Which layout:** `x4vrmod-data/manifest` beside `/proc/self/exe` means the zip;
otherwise the compiled-in prefix means a package. `X4VRMOD_DATA` overrides, for
development.

| | Package | Zip |
|---|---|---|
| Libraries | `/usr/lib/x4vrmod/` | `x4vrmod-data/lib/`, copied to `~/.local/share/x4vrmod/` |
| Layer manifest | `/usr/share/vulkan/explicit_layer.d/` | `~/.local/share/vulkan/explicit_layer.d/` |
| Data, help, extension | `/usr/share/x4vrmod/` | `x4vrmod-data/` |
| Install does | Extension only | Copy, manifest, extension |

* The zip copies because Steam launch options and the layer manifest need stable
  paths, even if the unpacked folder moves.
* `~/.local/share/x4vrmod/installed.json` records every file written, so uninstall
  removes exactly those. The layer stays explicit: it never loads into other programs.
* The Install dialog discloses that X4 marks saves made with a non-Egosoft
  extension as modified.
* **Uninstall** lists what goes, with a checkbox off by default: "Also delete my VR
  settings: this can't be undone". That covers `config-x4vrmod.xml`,
  `content-x4vrmod.xml`, `inputmap-x4vrmod.xml` and `*-x4vrmod-*.was` in every
  profile folder. Saves, `config.xml` and anything else are never touched. A package
  install also shows the package-manager command.
* **The zip and Qt:** built against the oldest Qt 6 in CI (6.2). The zip uses the
  system Qt when present, for the native theme, and falls back to a bundled copy
  (Fusion style). The CLI has no Qt dependency.

### Launching
`x4vrmod run [%command%]` builds the environment from the profile and execs
`x4vr-launch.sh`: one proven launch path, not a C++ rewrite of it. The script gets
its paths parameterised (`BUILD`, `ROOT`, the `/nvme` default, `SteamAppId`).

## Resolution

* **The launcher owns it** (decision 4). The layer, the eye images and
  gamescope's virtual screen are all sized at launch, and X4 applies its own
  menu's choice immediately, so an in-game change breaks the session.
* **Offered as absolute sizes** (2816 default, 3072, 3200, 3600, 4000, 4224,
  custom), marking the one nearest the headset's request: the probe reads the
  runtime's recommended per-eye size (`xrEnumerateViewConfigurationViews`); WiVRn
  asked for 3096×3243 here. The launcher sets `X4VR_H`, which derives the rest.
* **With the upscaler** (decision 6):
  * going from ≤3200² to >3200² with upscaler None selects FSR 1 Quality (DLSS once
    it exists), with a note saying why and how to undo it;
  * a manual upscaler choice is respected, and moving between larger sizes changes
    nothing;
  * going back to ≤3200² returns to None only if the selection was automatic;
  * the render size is always shown.
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
* **Above ~4068² per eye the map is flat** (4000² keeps depth, 4096² does not). X4 places images it creates
  after start-up in 255.8 MiB blocks; the map's 3D view, doubled for two
  eyes, outgrows them there, so the layer keeps it single-layer (takes
  376–378). The launcher says so next to the resolution choices that cross
  it.
* **The lower HUD's placement depends on the height** through X4's own
  formula, unless X4's HUD scale is off (docs/x4-quirks.md, "The lower HUD
  moves with the resolution"). If the HUD-scale test holds, the mod serves it
  off in VR, and every resolution the launcher offers keeps the same HUD.
* **X4's in-game menu:** left alone for now. Narrowing its list to the current
  mode (the injector already sits on SDL) is a possible later polish.

## Upscaling: FSR and DLSS

* **X4 9.00 selects its upscaler with `<upmode>`** (GetUpscalingOption).
  `<antialiasing>` is anti-aliasing only: none, FXAA, MSAA, SSAA 2x/4x,
  temporal.
  * Before 9.00, the anti-aliasing tag carried the FSR modes. Take 163's
    FSR3 used that route; the 2026-10-31 runs with `X4VR_AA=fsr_quality`
    rendered natively because 9.00 ignores it. `X4VR_AA` now rejects those
    values, and every run logs its effective `upmode`.
  * `<upmode>` values: `none`; FSR3 (temporal) `fsr3_native`, `fsr3_quality`,
    `fsr3_balanced`, `fsr3_performance`, `fsr3_ultra_performance`; FSR 1
    (spatial) `fsr_ultra_quality`, `fsr_quality`, `fsr_balanced`,
    `fsr_performance`. X4 greys out what the GPU does not support
    (IsUpscalingOptionSupported).
  * **`X4VR_UPSCALE`** (2026-10-31) serves it: one of the ten values, `none`
    when unset, so the mod owns the setting and an in-game change lasts one
    session. Frame generation is served off with it. The launcher's
    upscaler setting maps to this variable.
  * **Works in VR since v0.9.8** (takes 380, 383 and 384). FSR 1 is X4's
    own compute pass, patched to write both eyes (`X4VR_FSR_STEREO`). FSR 3
    is AMD FidelityFX, and every one of its dispatches runs again for the
    right eye (`X4VR_FFX_STEREO`). Both are on by default. Before that
    (take 379, take 381) the right eye's scene was black.
* **The launcher offers:** off, and the FSR modes X4 reports as supported,
  temporal and spatial separately. With an upscaler, the resolution setting
  is the output; the internal render is X4's (quality = output / 1.5 per
  axis).
* **Tested in VR on 9.00:**
  * FSR 1 Quality (take 380): 13.24 ms at 4224² per eye.
  * FSR 3 Balanced (takes 383–384): 16.72 ms at 4224² per eye. Patola: "as
    good as FSR1", no smearing.
  * Native at 4224²: 29.1 ms.
  * The other modes use the same code and differ only in X4's render size.
* **DLSS — to verify, then to test on an RTX card:**
  * X4's config has `<dlss>`, `<dlssmode>` and `<dlssg>` (frame
    generation). Which of them selects DLSS upscaling, and its values, have
    to be read from X4's Lua first.
  * Offered only when the GPU is NVIDIA RTX (Vulkan vendor ID 0x10de and the
    DLSS requirements met).
  * Tested by Patola's friend with an RTX 5090: the debug build and a
    protocol, scored like a take.
* **Frame generation stays off,** for FSR and DLSS alike. An interpolated
  frame has no head pose and adds latency, which is harmful in VR.
* The launcher can offer FSR. Still open: a frame-rate figure for each mode
  at the sizes the launcher lists, measured in one scene so they compare.

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

### What's built (2026-11-05)
* **Debug runs:** `x4vrmod run --debug`, and Play with debug log in the window, set
  `X4VR_DEBUG=1`, which applies the `debug` rows of `launch/defaults.conf`: take 375's
  instruments, logging only, in a log named `*-debug.log`. For that run only.
* **The report:** below, as a `.tar.gz` (GitHub doesn't take `.tar.zst`), without the
  scorer (the log reaches us, and we run it), and with the launch's output, the settings
  and a crash's backtrace from `coredumpctl`. The issue form is
  `.github/ISSUE_TEMPLATE/bug_report.yml`; "Open a GitHub issue" fills it in, once that
  file is on the default branch.
* **Scrubbing, checked on real logs:** in take 380's 46 MB log, the user name, home path
  and profile number appeared 10 times each and in the report 0 times. It packed to 5 MB
  in about a second. Longer logs keep their first 20 MB and last 100 MB.
* **Not built yet:** the flight recorder and its mark key, the scorer's first diagnosis
  in the report, "this is known" answers, and creating the issue through `gh`.

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
  * the bundle is a `.tar.gz` in `~/x4vrmod-reports/`;
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

**As built (2026-10-31).**
* `extension/x4vrmod/`: `content.xml` with `enabled="0" save="0"`, and one
  `libraries/parameters.xml` holding the same twelve replaces as the two old
  diffs (the look-around limits, then the sway and the boost shake).
  `tests/run-extension-selectors.sh` checks each against X4's archives.
* **Version: not the mod's.** The manifest's `version="1"` is the
  extension's own revision. X4's manifests write that attribute as an
  integer in hundredths (its 9.00 DLCs say `900`), which a `vX.Y.Z` does
  not map onto cleanly. "Current" is decided by comparing files, as
  `launch/deploy-extensions.sh` already does.
* **Migration, today's launcher:** `deploy-extensions.sh` removes the three
  old directories from X4's `extensions/` by exact name and says
  `REMOVED (retired after v0.9.8)`. Its `--without` option went with
  `x4vrmod_headlook`, its only user.
* **The content fork:** `x4vrmod` is forced on. An old id the fork still
  lists is forced off and named in the `content: serving` line
  (`x4vrmod_nosway retired, DISABLED (was on)`); one it does not list is
  never added, since X4 writes content.xml back into the fork. So the old
  names are switched off, not deleted from the fork.
* **Tests:** `tests/run-deploy-extensions.sh` (new; a foreign directory
  named like an old id plus a suffix is the negative control),
  `tests/run-content-fork.sh` and `x4vr_test_content_fork` (old ids off when
  listed, never added; `x4vrmod` is a prefix of the old ids and must not
  match them), census cases for the scorer reading old and new log lines.

## Not planned (now)
* An in-game panel drawn by the layer (decision 3).
* A launcher window in VR, or SteamVR/OpenXR overlays.
* Changing X4's in-game resolution menu.
* Frame generation.
* Flatpak and AppImage.

## Order of work
1. Phase 0: defaults flip, the `effective:` line, the scorer reading it.
   Accepted by a take with almost no variables.
2. Version embedding and static runtime; the single `x4vrmod` extension.
   **Done 2026-10-31** (below, and Phase 1's build choices: both libraries
   print `[X4VRMOD v…]` on their first log line and export 4 and 39
   symbols).
3. Phase 2's core and CLI: discovery, the patch check, the probe and `doctor`
   first (they answer the "to verify" items on real machines).
   **Done 2026-11-05** (`launcher/`).
4. `install`, `uninstall`, `run`; the script's paths parameterised; the zip layout
   and **the Arch PKGBUILD** (decision 7). **Done 2026-11-05**: takes 403 (terminal)
   and 404 (Steam's launch options) scored PASS.
5. Phase 3's Qt 6 Widgets window over the core, with its art and help. **Code done
   2026-11-05**, with debug runs and bug reports from step 8; not yet played from.
6. CI: tests, census, deb/rpm/zip/Arch packages on tags.
7. Signature search, for hotfixes and GOG. The second binary is here (GOG's, above).
8. `report` and the flight recorder; DLSS: read the tag, wire it, the RTX 5090 test.
