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
