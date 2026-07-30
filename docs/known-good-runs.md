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

## Why the knobs alone are not enough either

Take forty-three found the masked set is *content dependent*: the same knobs
produced **3** `+PRESENT-CAND` passes in take thirty-three and **6** in take
forty-three. `subpass_is_present()` is a heuristic over whatever render passes
the current scene happens to build, so "the same command" does not guarantee
"the same passes masked". Until that predicate is pinned down, a recorded
command is necessary and not sufficient — record the run's pass tallies with it.
