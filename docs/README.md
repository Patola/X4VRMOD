# docs

Findings and plans. See [../DESIGN.md](../DESIGN.md) for the architecture.

| Doc | What it is |
|---|---|
| [frame-analysis.md](frame-analysis.md) | What X4's renderer actually does — the empirical basis for the stereo mechanism. Grows as phases land. |
| [x4-quirks.md](x4-quirks.md) | Pitfalls and traps. Every entry leads with the **symptom**, so it can be found by what went wrong rather than by what caused it. |
| [phase4b-test-plan.md](phase4b-test-plan.md) | How the frame-doubling change gets verified, written before the change. |

## Convention

Predictions are committed **before** the measurement that tests them, and
wrong turns are recorded alongside the findings. A doc that only lists
conclusions cannot say which ones were nearly missed, and the near-misses are
what stop the same mistake twice.
