<!-- SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception -->
# Bisection plan: restoring the true-SBS configuration

Written at the end of the take-forty-four session so a cold start can run this
without re-deriving anything.

## What is known, and is not to be re-litigated

* **The code is not the problem.** Take forty-two ran the same command against
  the `stage2-sbs-working` tree and it failed identically. Everything added
  since that tag is observation, plus knobs that default off.
* **The system is stable.** 59 and 58 distinct render-pass serials across two
  runs; 3 `+PRESENT-CAND` per session in both take thirty-three and take
  forty-three. An earlier claim of scene-dependence was a log file holding two
  sessions — see the retraction in `frame-analysis.md`.
* **The second eye exists.** World images `#97`/`#98` probe `DIFFER` at 6–9%.
  The scene is rendered twice.
* **It is lost before the screen.** Present targets `#50`–`#53` have layer 1 at
  4–5% of layer 0 — the HUD, and nothing else.
* **It is not the all-LDR masking.** `X4VR_MASK_LDR=1` masked four more passes
  and moved the ratio not at all (take forty-four).

So: some knob subset carries the scene into layer 1 of the present target, and
the current subset does not.

## Rules for every run

1. **A fresh `X4VR_LOG` per run.** `X4VR_LOG` appends. Two sessions in one file
   is what produced the retraction above, and `score_run.py` will refuse the
   file rather than average across them.
2. **Score with the script, not the screen**: `tools/score_run.py <log>`.
   Exit 0 = candidate, 1 = failed a check, 2 = unscorable.
3. `X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1` on every run — they are what the
   script reads. Load a savegame and sit still for ~15 seconds; the probe
   cycles one image per frame and needs a few hundred frames to reach `#50`–
   `#53`.
4. **A run that scores 0 goes into `known-good-runs.md` immediately**, with its
   `env: run =` line pasted verbatim and a code tag made the same minute.

## The runs, in order

Baseline for all of them (never varied):

```
X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
```

| # | vary | why |
|---|---|---|
| B1 | `MASK_PRESENT=1 BINDLESS_MIRROR=1 BINDLESS_PATCH=1` | drops `MASK_TONEMAP` and `MASK_LDR` — the two knobs added on my own initiative, never evidenced in take thirty-three |
| B2 | B1 + `MASK_TONEMAP=1` | puts back the one with a documented purpose (task #4) |
| B3 | B1 without `BINDLESS_PATCH` | the patch makes view 1 sample the twin; if layer 1 fills without it, the twin region is the suspect, not the masking |
| B4 | B1 without `MASK_PRESENT` | the composite's own mask; a present pass masked while its source is mono could be writing an empty layer 1 over a good one |

Stop at the first run scoring 0. If none does, the fault is not in this knob
space and the next question is the one the probe already frames: **which image
between `#97` and `#50` is the last to carry a second eye.** The probe reports
every per-eye image, so that is a read of an existing log, not a new run.

## What not to do

Do not add a knob to fix a run that failed. Four of the last five takes did
that, each on a theory built to explain the previous take, and none survived
its own measurement. The search above is finite; exhausting it is cheaper than
one more hypothesis.
