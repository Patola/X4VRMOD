<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception -->
# Known-good runs

A working state is **code plus knobs**. The project has tagged code since the
beginning and never recorded the knobs, so `stage2-sbs-working` preserves half a
state: take forty-two proved that tree still behaves exactly as this one, and
neither reproduces take thirty-three, because what was lost was the command.

This file closes that gap. **A run is not "known good" until its `env: run =`
line is pasted here**, verbatim, from the log it produced. The injector prints
that line at X4 startup (since take thirty-four); copy it, do not retype it.

Tag the code at the same moment and name the tag here, so the pair can be
restored together.

| date | tag | verified by | `env: run =` |
|---|---|---|---|
| 2026-07-29 | `stage2-sbs-working` | true SBS on screen, framerate acceptable | **LOST** — predates the run-recording line |
| 2026-07-31 | `stage2-duplicate-restored` | `score_run.py` exit 0, grade DUPLICATE, 19/19 settled samples bit-identical; Patola: "two complete side-by-side screens again … no black screen" | `X4VR_TAKE=46-B3 X4VR_STEREO=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take46.log X4VR_MV_PROBE=1 X4VR_MASK_PRESENT=1 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1` |

`X4VR_RES` is set by `launch/x4vr-launch.sh` from the SBS geometry, not typed;
it appears in the line because the injector dumps every `X4VR_` variable in the
environment. The knobs actually typed were:

```
X4VR_TAKE=46-B3 X4VR_LOG=/tmp/x4vr-take46.log \
X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 \
X4VR_MV=1 X4VR_STEREO=1 X4VR_MASK_PRESENT=1 X4VR_BINDLESS_MIRROR=1 \
X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 \
./launch/x4vr-launch.sh
```

| 2026-07-31 | `stage2-stereo-first` | `score_run.py` exit 0, layer 1 at 100% fill with 16 of 23 settled samples carrying a real difference; Patola confirms parallax on screen | `X4VR_TAKE=49-P57 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take49.log X4VR_MV_PROBE=1 X4VR_MASK_PRESENT=1 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1` |

**`stage2-stereo-first` is the first run with real stereo on screen.** It is
`stage2-duplicate-restored` plus `X4VR_BINDLESS_PATCH=1`, on the build that
fixes the aliased-variable patch (take forty-eight). The grade reads `MIXED`
rather than `STEREO` only because the splash-screen frames are legitimately
bit-identical — X4 draws the same pixels to both layers there.

| 2026-07-31 | `stage2-stereo-verified` | `score_run.py` exit 0; take 50 control proves the per-view sampling bit-exact and take 51 proves the remaining per-eye difference is geometric; Patola: "the shadows are only slightly different, as I would expect from stereo view" | `X4VR_TAKE=51-P58 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take51.log X4VR_MV_PROBE=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.016 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1` |

**`stage2-stereo-verified` is the state to resume from.** Same code and same
knobs as `stage2-stereo-first`, plus `X4VR_IPD=0.016`, and it is the tag to
prefer because two controls stand behind it rather than one screenshot:

* **take 50** — the shear off (`X4VR_STEREO` **omitted**, not set to 0 —
  see below) with the patch on: every probed image
  `IDENTICAL`, 19 of 19 settled swapchain samples bit-exact. The mirror, the
  offset and the aliased-variable fix are provably not a variable.
* **take 51** — quarter IPD: every image's `DIFFER` drops, screen-space buffers
  hardest (`#97` by 37×). The remaining per-eye difference is geometric.

**Correction, found while writing task #23's shader patch.** This file used to
describe take 50's control as `X4VR_STEREO=0`. It was not: that run **omitted**
the variable, and the layer tested `getenv("X4VR_STEREO")` for *presence*, so
`X4VR_STEREO=0` would have baked the shear and made the control look broken.
Fifteen other knobs in the layer test the value; this one did not. The check is
now value-sensitive, so both spellings do what they read as — but the recorded
command is the authority, which is the reason this file demands the verbatim
`env: run =` line rather than a description of it.

`X4VR_IPD=0.016` is **not** a calibrated value — it is a quarter of the default,
chosen to test whether the artifact scaled. It does. The right value is unknown
until `sx` and the near plane are measured from X4's projection instead of
assumed (task #23), and that is the next work.

The earlier note here recorded the cockpit lighting difference as an open
defect. It was not a defect: takes 50 and 51 closed task #22 with no code
change. Keeping the correction visible, because "we saw something odd and it
turned out to be correct" is as worth recording as a bug.

Known defects that do ride along: the cursor is confined to a centred square
(tasks #17, #19) and X4's logo is clipped at its right edge (task #21).

| 2026-07-31 | `stage2-sx-measured` | `score_run.py` exit 0 on both sessions; `proj SHEAR` reports `baked is 1.000x` against X4's live projection | `X4VR_TAKE=53-P60 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take53.log X4VR_MV_PROBE=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.016 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1` |

**`stage2-sx-measured` supersedes `stage2-stereo-verified` as the resume
point.** Same code path, plus `X4VR_PROJ_SX=1.3333` — read out of X4's own
projection matrix rather than inherited from a 2816×1408 measurement.

**Correction from take 54: 1.3333 is the *menu's* projection.** Reconstructing
that session shows `sx = 1.33333` holds only for the first ninety-six seconds,
before the savegame loads; gameplay reads 3.78085. Takes 52 and 53 both sampled
that early window, so this tag is calibrated against the menu and is roughly
2.8× too small in flight. It is still a real, scoring, reproducible state and
still better than `stage2-stereo-verified` — but the sentence this replaces
claimed it matched X4's camera, and it matched the wrong one.

It is tagged because the next change is the riskiest in a while (a new SPIR-V
transform), and the rule is that a good state gets tagged before the risk, not
after. It is emphatically *not* the finished article:

* `sx` moves by 33× when the player zooms, and this tag does not track it. At
  full zoom the baked shear is 28× too small. It is correct at rest and wrong
  in motion, where `stage2-stereo-verified` was wrong by 1.5× everywhere.
* `X4VR_IPD=0.016` is still not a calibrated value, for the same reason as
  before: it was a quarter of the default, chosen to test scaling.

Both sessions in that log score PASS/MIXED independently — the file holds two
X4 runs and must be split before scoring, which `score_run.py` will insist on
anyway.

**What `stage2-duplicate-restored` is and is not.** Both eyes have a picture and
the framerate is acceptable; the right eye is a bit-exact copy of the left, so
there is no parallax in it. It is the state this project had in take
thirty-three and lost for twelve takes. It is a floor to return to, not the
goal. Two known defects ride along with it: the cursor is confined to a centred
square (task #19), and X4's logo is clipped at its right edge (task #21).

## Retracted: "the knobs are not enough either"

This file previously said the masked set was content-dependent — 3
`+PRESENT-CAND` in take thirty-three against 6 in take forty-three, same knobs
— and concluded that a recorded command could not restore a state.

**That was one log file holding two X4 sessions.** `X4VR_LOG` appends;
take forty-three's file has two `mv: X4 uses vkCreateRenderPass` lines. Per
session the count is 3, the same as take thirty-three. The distinct render-pass
serials are 59 and 58 across those runs — the pass population is *stable*.

So the configuration **is** restorable, and the pessimism above was an artifact
of a miscount. Score every run with `tools/score_run.py`, which refuses to read
a log containing more than one session before it looks at anything else.

Use a fresh `X4VR_LOG` per run. Always.

## `stage2-stereo-shading-correct` — take 83

The first state in this project where both eyes are **correctly lit** and the
parallax is real. Patola, on the screen: both frames bright, and the IPD shift
visible.

    X4VR_TAKE=83-UJ X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2
    X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1
    X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_LOG=/tmp/x4vr-take83.log
    ./launch/x4vr-launch.sh

`X4VR_PROJ_INVPROJ=1` was passed explicitly on the run itself; it is **on by
default from this commit**, so the line above omits it. `X4VR_MV_DUMP`,
`X4VR_MV_DUMP_IMG`, `X4VR_MV_PROBE` and `X4VR_MV_INVENTORY` were measurement
only and are dropped here.

**What to check in the log:**

    invproj final: per-eye M_invprojection — 224 modules corrected
    invproj final: per-eye M_invprojection_uj — 2 modules corrected

The second line is the one that matters. A `0` there is the pre-83 defect, and
it will not show up in the first line, which stays healthy at ~224 either way.

**Scored, not described:** `#57` whole-frame tile ratio `p1/p50/p99 =
0.573/0.993/1.536` against take 82's `0.662/1.016/4.078`; confidently-matched
flagged area 1.8% against 14.4%; worst blob median `1.00` against `1.33` and p90
`1.85` against `28.91`.

**Known defects riding along**, none of them new: the cursor is confined to a
centred square (task #19), X4's logo is clipped at its right edge (task #21),
the doubling overshoot is untightened, and `run-multiview-render.sh` has 10
pre-existing failures. The residual 1.8% is above the `IPD=0` control's 0.0%,
part of which is genuine one-eye occlusion.

**Reproducing an older X4 take needs `X4VR_PROJ_INVPROJ=0`.** The default
changed here, and every earlier take ran with the correction off by omitting the
variable.

**That caveat does not extend to the offline test suite.** `run-multiview-render.sh`
was run both ways and the pass/fail set is byte-identical (`md5 634f9b7c…`),
including its 10 pre-existing failures. The suite drives `have_k` through
`X4VR_CLIP_K`/`X4VR_CLIP_K_RIGHT`, so the invproj code does execute — but its
synthetic shaders declare no camera block at `set 1, binding 0`, so the patch
matches nothing and returns false. The knob is inert there.

This holds only while that is true. **If a render-test case is ever given a
shader with a camera block at `set 1, binding 0`, the default starts mattering
and that case must pin `X4VR_PROJ_INVPROJ` explicitly** rather than inherit it.

## `stage3-cursor-in-eye` — takes 95 and 96

Correct stereo, plus a pointer that is part of the frame. Everything
`stage2-stereo-shading-correct` had, and the mouse now works in side-by-side:
one cursor per eye, X4's own bitmap, changing shape with context, selecting on
its exact location. Patola exercised the map exhaustively — icons, pull-downs,
text, collapsible sections, and the 3D map rotated — with every hitbox landing.

    X4VR_TAKE=96-CURSOR X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2
    X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1
    X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_LOG=/tmp/x4vr-take96.log
    ./launch/x4vr-launch.sh

**What to check in the log:**

    cursor: overlay pipeline built for a B8G8R8A8_UNORM eye
    cursor: drawing 32x32 hot=(h,v) into 2 layer(s) of the 1408x1408 eye
    sdl: SDL_HideCursor() -> 1

`into 2 layer(s)` is the one to read. A `1` there means the pointer reaches one
eye only — the same class of defect as a black right eye, in a new place. The
`hot=` pair must also vary as X4 switches cursor; a hot spot stuck at one value
would mean `SDL_SetCursor` stopped publishing and the shim is drawing a stale
bitmap.

`X4VR_CURSOR=1 X4VR_HIDE_CURSOR=1` were passed explicitly on the run itself;
both are **on by default from the commit that follows**, so the line above omits
them. See the dated decision at the end of this file for why hiding defaults on
despite leaving no pointer at all when the overlay fails — that is the intended
signal, not an oversight.

The invproj check from `stage2-stereo-shading-correct` still applies unchanged.

## `stage4-ui-canvas` — takes 98, 99 and 100

Everything `stage3-cursor-in-eye` had, plus the UI on a floating canvas at a
chosen distance instead of pinned at infinity. The knob is **off by default**;
this state is the one it produces when asked for.

    X4VR_TAKE=100-AIM X4VR_CANVAS_M=2 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1
    X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take100.log
    ./launch/x4vr-launch.sh

**What to check in the log:**

    canvas: 2.000 m -> s=0.02133 NDC (L=+s R=-s), 15.0 px per eye
    canvas final: N variant(s) built, 0 REFUSED, swapped into 18 pipeline stage(s)
    cursor: ... canvas shift 0.02133 NDC (15.0 px per eye)

**The swap count is the line to read, and 0 is the failure that looks like
success.** Variants built with nothing bound reproduces the pre-canvas frame
exactly, so a run that "looks like take 96" is either a working canvas or an
absent one and only this number tells them apart. `N` is the number of World
modules X4 happened to compile — 340, 346 and 348 across the three takes, all
near the 320 the offline sweep over the dumps predicted.

The cursor's shift must equal the canvas's. They are set from one variable, so a
mismatch means the overlay read it before it was published — and the symptom
would be a pointer a fixed distance from every button it activates, in a run
where both features report success.

**This is an interaction-safe configuration.** No `X4VR_MV_PROBE`, no
`X4VR_MV_DUMP`, no `X4VR_MV_DUMP_PRESENT`: each of those drains the GPU queue,
and take 99 stuttered on the probe's 4.87 s period badly enough that aiming at a
target was impractical. Add them only to a take whose evidence is a dump.

The invproj and `into 2 layer(s)` checks from the two stages below still apply
unchanged.

## `stage5-wide-field` — takes 106, 107 and 108

Everything `stage4-ui-canvas` had, plus a **chosen field of view** and a baked
`sx` that matches the camera the player looks through. X4's `<fov>` config tag
multiplies the horizontal field as an angle, linearly:

    horizontal FOV = fov x 73.7399 deg   =>   fov = target / 73.7399

so 1.437 is 106 degrees. The injector already owns every read of `config.xml`,
so this is an override rather than a request that the player change a setting.

    X4VR_TAKE=108-BAKED-SX X4VR_FOV=1.437 X4VR_PROJ_SX=0.75405
    X4VR_DUMP_MATRICES=1 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take108.log
    ./launch/x4vr-launch.sh

**The two knobs move together.** `X4VR_PROJ_SX` is the value the world modules
with no camera block bake, and it must be the scene camera's `sx` at the field
being asked for -- `cot(fov x 73.7399 / 2)` at a 1:1 eye:

    fov 1.1111  ->  sx 1.15174        fov 1.437  ->  sx 0.75405
    fov 1.356   ->  sx 0.83923        fov 1.500  ->  sx 0.69231

The 1.437 entry is the value X4 was *measured* at in takes 105, 106 and 108; the
formula gives 0.75404. They agree to the five decimals the log prints, and the
measured one is written here because that is what the comparison in the score is
against. The other three are computed and the two that were measured -- 1.15174
and 0.69231 -- came back exact.

Leaving `X4VR_PROJ_SX=1.3333` while widening the field is not a stale constant
but a depth conflict: at fov 1.437 those modules separate 1.77x too much, which
places their geometry at 0.57x its true distance while everything around it is
correct.

**What to check in the log** -- `python3 tools/score_run.py` prints all of it:

    proj  sx=... = fov 1.437   <- honours X4VR_FOV
    proj  sy tracks sx on the fov camera -- |sy/sx|=1.0000 against an eye aspect
          of 1.0000, so the field is widened, not stretched
    stereo  X4VR_PROJ_SX=0.75405 against a scene sx of 0.75405: ... matches the
            scene camera

The `<- honours X4VR_FOV` marker is the one that says the tag was accepted at
all. A value X4's XML reader rejects leaves the options menu on "--" and the
engine on its default, and the run then looks exactly like a measurement.

**Interaction-safe**, on the same grounds as `stage4-ui-canvas`: no probe, no
dumps. `X4VR_DUMP_MATRICES=1` is the exception and stays -- it costs no queue
wait and no readback, and it is what produces every line above.

**Not verified in this state:** whether the depth and scale feel right. That
needs the two array layers submitted to an OpenXR projection layer, which does
not exist yet. A side-by-side pair on a flat monitor cannot answer it.

## `stage6-sx-per-draw` — takes 109 and 110

Everything `stage5-wide-field` had, and **no module shears by a constant any
more**. The twelve World modules that declare no camera block recover `sx` from
their own `M_worldviewprojection`:

    sx = |row0(MVP).xyz| / |row3(MVP).xyz|

`X4VR_PROJ_MVP` is on by default from this state; `=0` restores stage5 exactly.

    X4VR_TAKE=110-MVP-ON X4VR_FOV=1.437 X4VR_DUMP_MATRICES=1 X4VR_STEREO=1
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1
    X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take110.log
    ./launch/x4vr-launch.sh

**`X4VR_PROJ_SX` is gone from that line, and that is the point.** stage5 required
it to be kept in step with `X4VR_FOV` by hand -- 0.75405 for fov 1.437, 1.15174
for 1.1111 -- because twelve modules baked it. Nothing bakes it now, so the pair
that had to agree no longer exists. It remains readable as insurance: if a future
X4 build ships a World module that refuses *both* patches, `baked-sx=` stops
being 0 and the scorer says so.

**What to check in the log** -- `python3 tools/score_run.py` prints all of it:

    patched vertex shader #400 (world, per-view)
      [world=338 nonworld=62 stereo=338 live-sx=326 mvp-sx=12 baked-sx=0]
    stereo  X4VR_PROJ_SX=... is unused — no module fell back to it, every one
            reads sx per draw (task #23)
    extents X4 renders 1408x1408, eye 1408x1408 (from X4VR_RES),
            composite 2816x1408, window 2816x1408

`baked-sx=0` is the state. `mvp-sx=` should carry what stage5's `baked-sx=`
carried at the same module index, and **`live-sx=` must be unchanged** -- if it
moved, the fallback patch ran on a module that could already see the camera,
which is not what it is for.

Also required: **no `WARNING: driver rejected patched module`**. `spirv-val`
accepts 328 of take 74's 397 modules offline, but the driver is what compiles
them, and this transform is newer than every other one in the layer.

**Interaction-safe**, same as the two stages below: no probe, no dumps.

**Not verified in this state, and not verifiable on a flatscreen:** whether the
correction is visible. What it changes is disparity -- 0.34 px to 13.15 px per
eye at 50 m at the zoom stop -- which is a depth cue and no monoscopic
difference at all. Takes 109 and 110 were run back to back on the same scenario
and Patola could not tell them apart, which is what a flat side-by-side pair can
be expected to show. Settling it needs `X4VR_MV_DUMP_PRESENT` at high zoom read
through `tools/eye_stereo.py`, or VR.

## `stage7-xr-session-proven` — 2026-08-09, in a headset

**Not an X4 take.** X4 does not run in this state and there is no `env: run =`
line, because nothing of this has reached the layer yet. It is recorded here
anyway, under the same rule as the rest: it is a state that worked, it is code
plus knobs, and the way back to it must not depend on remembering.

What is proven: an `XrSession` on a `VkDevice` the *runtime* created from an
X4-shaped `VkInstanceCreateInfo`, a 2-layer swapchain at the runtime's own
recommended extent, and a projection layer with per-view pose and FOV accepted
by the compositor and displayed correctly in a Quest 3 over WiVRn.

    cd /home/patola/workspace/claude/X4VRMOD && cmake --build build -j8
    ./tests/run-xr-probe.sh 20

with WiVRn running and the headset connected. Output tees to
`/tmp/x4vr-xrprobe.txt`; run 1 is kept as `/tmp/x4vr-xrprobe-run1.txt`.

**What to check in the output** — the card half runs with no runtime at all and
must print `KEY_SELFTEST_FAILURES=0` with all three cases going the right way
(one correct, two caught). The live half:

    KEY_VIEWS=2                 KEY_PHYSICAL_CHOSEN=0      KEY_SPACE=STAGE
    KEY_FOV_ASYMMETRIC=1        KEY_IPD_M=0.0630
    KEY_FOV0=-54.0000,40.0000,44.0000,-55.0000
    KEY_FOV1=-40.0000,54.0000,44.0000,-55.0000
    KEY_FRAMES / KEY_LOCATED / KEY_SUBMITTED   all within one of each other
    KEY_HEAD_SPAN_M                            non-zero on every axis

`KEY_LOCATED` far below `KEY_FRAMES` means the tracking dropped out;
`KEY_SUBMITTED` far below `KEY_LOCATED` means the compositor was rejecting
layers. A `KEY_HEAD_SPAN_M` of zeros means the pose never moved, which is the
failure this whole task exists to avoid and which an average would hide.

**And the part only a person can check** — P5's gate, confirmed 2026-08-09:

* left eye: **blue** background, wide marker bar off to the **left**
* right eye: **green** background, wide marker bar off to the **right**
* both eyes: the thin centre bar fuses into **one** bar, floating **about 2 m
  away** — nearer than the background, not further

Patola confirmed all three. That settles eye order (`imageArrayIndex` N = view
N = the runtime's own view N), the disparity **sign** (converged reads as
nearer), and the scale: the bar was painted at ±0.9°, which for the measured
0.0630 m IPD is 2.0 m, and it was seen at about 2 m.

**Not proven by this state:** anything inside X4. The layer does not create a
session, X4's frames do not reach a swapchain, and the per-eye frustum
correction (#35) is not written — the card sidesteps it by placing itself in
the runtime's own angular space, which X4's renderer cannot do.

## Decision, 2026-08-08 — the cursor knobs default on, and hiding gates on intent

`X4VR_CURSOR` and `X4VR_HIDE_CURSOR` are **both on by default** as of this date,
following takes 95/96. Same precedent as `X4VR_PROJ_INVPROJ` after take 83: a
confirmed correction becomes the behaviour, and a run reproducing anything
earlier has to say so explicitly.

The second one deserves its reasoning written down, because the obvious design
is wrong.

**The rejected design.** Have the layer report "I am drawing a cursor" through
the channel, and let the injector hide the compositor's pointer only then. It
reads as robustness. It is a **fallback**: when the overlay fails to build — bad
driver, unknown pixel format, injector not preloaded — gamescope's pointer
quietly comes back, the game looks fine, and the defect is invisible. Every
expensive mistake recorded in `frame-analysis.md` has that shape. An aggregate
reported "no defect" on a 2.4x per-eye shading error. A predicate excluded its
own suspect. A metric could not tell correct stereo from broken. Adding another
one and calling it safety would have been the fourth.

**The rule instead: gate on intent, never on outcome.**

| gate | question | effect |
|---|---|---|
| intent | did this run *ask* for a drawn cursor? | configuration — conceals nothing, because a run that never asked has no defect to conceal |
| outcome | did the draw *succeed*? | fallback — converts a defect into a working-looking system |

So the injector stands down only for declared configuration, each with a log
line naming which:

    X4VR_HIDE_CURSOR=0
    X4VR_NO_LAYER=1     nothing is loaded that could draw one
    X4VR_SBS off        no composite
    X4VR_SBS_SPLIT=0    X4 renders full width, so there is no eye image
    X4VR_CURSOR=0       the overlay is off

Each of those is a variable **the layer reads too**, through the same
`x4vr::env_on` in `common/x4vr_env.hpp`, so the two components cannot come to
disagree about what "on" means. Two hand-written spellings of one rule is how
they drift, and `tests/cursor_place.cpp` pins the semantics.

The case that stays loud is the one worth being loud about: layer on, SBS on,
cursor on, overlay failed → **no pointer at all**, with the reason on the line
above it. That is a fair trade only because the failure is escapable (the
keyboard, Esc and the menus still work) and diagnosable (every failure path names
itself). If either stopped being true, the trade would stop being fair.

**When to revisit.** This is a development-phase default, chosen because we are
chasing defects and want them to announce themselves. It should flip to the
outcome-gate the moment anyone other than Patola runs this mod: at that point a
silent fallback to the compositor's pointer protects a user instead of hiding a
bug, and the same design that is wrong today becomes right. The channel already
anticipates the reversed direction — `common/x4vr_share.hpp` says to add a second
struct rather than make that one bidirectional.
