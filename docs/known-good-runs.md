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
