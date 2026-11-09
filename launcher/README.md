# launcher

The X4 VR Mod launcher: a Qt-free core (`core/`) with the `x4vrmod` command line
(`cli/`) and the `x4vrmod-launcher` window (`gui/`, Qt 6 Widgets, `qt6-base` alone) over
it. The design is in [docs/packaging-plan.md](../docs/packaging-plan.md), Phases 2 and 3.
The window only shows what the core decides, so it and `doctor` can't disagree. Without
Qt 6 the command line still builds. Its help and artwork are in `help/`; the pictures of
the window come from `x4vr_test_launcher_gui --shots launcher/help`, the rest from
`tools/make_launcher_art.py`.

    x4vrmod doctor            every condition for playing, one line each
    x4vrmod install           put the mod in place; says what it does to saves
    x4vrmod run [%command%]   launch X4 with the mod, from a terminal or from Steam;
                              --debug also logs the instruments, for a bug report;
                              --flat starts X4 by itself, a baseline with nothing of ours
    x4vrmod report            make a bug report to read and attach; nothing is sent
    x4vrmod launch-options    the lines to use for that
    x4vrmod config set K V    eye, upscaler, ipd, fov, gamemode, language, game, env.NAME;
                              `unset K` clears one
    x4vrmod uninstall         takes the mod out; --all also deletes the VR settings
    x4vrmod probe x4 [PATH]   X4's version and which of the mod's patches fit
    x4vrmod probe xr          ask the VR runtime
    x4vrmod probe vulkan      what the GPUs offer

**Is this X4 compatible?** There is no checksum. `probe x4` and `doctor` run the
same checks the injector makes in X4's memory at load, over the file, feature by
feature. Those checks live in `injector/` and `common/`, as templates over a reader.

**How it knows where it's running.** `x4vrmod-data/manifest` beside the program is
the zip (`tools/make-zip.sh`); inside a build tree is development; anything else is a
package, at the prefix it was built for (`packaging/arch/PKGBUILD`). `install` copies
the zip to `~/.local/share/x4vrmod/`, because Steam's launch line and the layer's
manifest need paths that survive the unpacked folder moving. A package has its files
already, so it only deploys the extension.

**What `run` does.** It chooses the X4 (what's asked for, `X4VR_GAME`, the profile,
then discovery), puts the profile (`~/.config/x4vrmod/profile.toml`) in the environment
under anything set explicitly, gives the run its own log, and execs
`launch/x4vr-launch.sh`: one launch path, whichever way it was started. The mod remains
the only creator of its `*-x4vrmod*` config files; the launcher only reports and
deletes them.

**Eye distance** comes from the headset, so the window doesn't ask for it.
`x4vrmod config set ipd 0.0635` overrides it, in metres, and `x4vrmod config unset ipd` goes
back to the headset's. The help says what each does.

**What it won't touch.** X4's saves, `config.xml` and anything else of the player's.
Uninstall only removes names it knows as its own, so a damaged install record can't
point it at something else. X4's own files are only ever read.

**Bug reports.** `report` writes plain text to `~/x4vrmod-reports/`, with a `.tar.gz` beside
it to attach to a GitHub issue. The home folder, user name, computer name and Steam account
number are taken out of everything first, and nothing is sent: the player attaches it.

**GameMode** is offered when `gamemoderun` is on the PATH. The launch script puts it in front
of the whole command, keeping the injector in `LD_PRELOAD`, and Flatscreen gets it too so the
baseline stays comparable. `run` passes everything after its own options on as the command to
start, which is how Steam's `%command%` reaches X4.

**DLSS** is offered only when the GPU the mod would run on is an NVIDIA RTX one, and the
gpu row says so. It hasn't been played in a headset, and the window says that too.

**Other builds of X4.** `probe x4` and `doctor` find the code wherever this build keeps it
(`common/x4vr_reloc.hpp`) before checking it. `x4vrmod probe x4 PATH --map` shows how far
each block moved.

**Languages.** The window and its help are in English, Brazilian Portuguese, French, Russian
and German: the system's language to begin with, the Launcher language list below GameMode after that
(profile key `language`). Words are marked in the code with `tr_`/`trn_`/`N_` in `core/` and
`qtr`/`qtrn` in `gui/` (`core/x4vr_i18n.hpp`); a sentence with values is one `%1`..`%9`
template, so a language can reorder them. `tools/i18n-update.sh` collects them into
`po/x4vrmod.pot` and merges that into each `po/<lang>.po`; the build compiles those with
msgfmt. Each help page is `help/<lang>/index.html` with the English page's sections, and its
pictures come from `x4vr_test_launcher_gui --shots launcher/help/<lang> --lang <lang>`. The `x4vrmod`
command never sets a locale, so what it prints stays English for diagnostics, and a bug report
is made in English on its own thread (`InEnglish`). Programs the window starts get the
environment it started with, so the window's language doesn't leak into X4, WiVRn or Steam.
`tests/run-i18n.sh` checks the catalogues; the window test checks every language fits.
The translations began as a best-effort draft: corrections from players are the plan.

**Probes** run in a child process with a timeout, so a broken VR runtime can't hang or
crash the launcher. The layer and the injector are never loaded: their version is read
from the file (`cmake/x4vr_version.cmake`'s marker), because loading them would run
their code.

Tests: `build/tests/x4vr_test_launcher`, `build/tests/x4vr_test_launcher_gui` (the window on
Qt's offscreen platform), `tests/run-launcher-x4-bytes.sh` (a temp copy of X4 with bytes
bent), `tests/run-launcher-install.sh` (a real zip, a fake home and a fake game) and
`tests/run-launcher-package.sh` (a real `cmake --install`). `tests/run-config-fork.sh` covers
how the mod's config fork is created, through the real injector.
