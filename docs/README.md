# docs

Findings and plans. See [../DESIGN.md](../DESIGN.md) for the architecture.

| Doc | What it is |
|---|---|
| [frame-analysis.md](frame-analysis.md) | What X4's renderer actually does — the empirical basis for the stereo mechanism. Grows as phases land. |
| [x4-quirks.md](x4-quirks.md) | Pitfalls and traps. Every entry leads with the **symptom**, so it can be found by what went wrong rather than by what caused it. |
| [phase4b-test-plan.md](phase4b-test-plan.md) | How the frame-doubling change gets verified, written before the change. |

## Knobs that are known harmful

`X4VR_TICK_HZ` — **do not set it.** It moves the servo tick off X4's event drain,
which measured ~17 ms of added queue latency and made the camera slam between X4's
stops in 3 of the 4 sessions that used it. Kept only to reproduce those takes. See
*"`X4VR_TICK_HZ` is harmful"* in [x4-quirks.md](x4-quirks.md).

## Convention

Predictions are committed **before** the measurement that tests them, and
wrong turns are recorded alongside the findings. A doc that only lists
conclusions cannot say which ones were nearly missed, and the near-misses are
what stop the same mistake twice.
