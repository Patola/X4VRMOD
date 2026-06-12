#!/usr/bin/env python3
"""
analyze — clock-drift / slip-rate analysis for the AER probe.

Inputs:
  1. probe log:    aer_probe.py --mode aer --fps 72 --ipd 6.4 --log > probe.log
  2. MangoHud CSV: MANGOHUD_CONFIG=...,autostart_log=1,log_duration=330,output_folder=...
                   (per-frame frametime log of the same period)

The two files come from different clocks, so absolute phase cannot be
aligned — but the *rates* can be compared precisely. The probe and the
game both believe they run at the same nominal rate; their relative rate
error (ppm) determines how often the accumulated phase slips by one full
frame, flipping eye parity.

Usage:  ./analyze.py probe.log /tmp/mango/X4_*.csv
"""

import csv
import statistics
import sys


def load_probe(path):
    """Timestamps (s) of flips from `--log` lines: '<t>  flip -> <val>'."""
    ts = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "flip":
                try:
                    ts.append(float(parts[0]))
                except ValueError:
                    pass
    if len(ts) < 100:
        sys.exit(f"{path}: only {len(ts)} flip lines — need a few minutes of --log output")
    return ts


def load_mangohud(path):
    """Per-frame frametimes (ms) from a MangoHud CSV (header lines vary)."""
    ft = []
    with open(path) as f:
        rows = list(csv.reader(f))
    # find the header row containing 'frametime'
    hdr_i = next((i for i, r in enumerate(rows)
                  if any("frametime" in c.lower() for c in r)), None)
    if hdr_i is None:
        sys.exit(f"{path}: no 'frametime' column found — is this a MangoHud log?")
    col = next(j for j, c in enumerate(rows[hdr_i]) if "frametime" in c.lower())
    for r in rows[hdr_i + 1:]:
        try:
            ft.append(float(r[col]))
        except (ValueError, IndexError):
            pass
    if len(ft) < 100:
        sys.exit(f"{path}: only {len(ft)} frames logged")
    return ft


def stats_ms(intervals_ms, label):
    mean = statistics.mean(intervals_ms)
    sd = statistics.stdev(intervals_ms)
    print(f"{label}:")
    print(f"  n={len(intervals_ms)}  mean {mean:.4f} ms  stdev {sd*1000:.1f} us"
          f"  min {min(intervals_ms):.3f}  max {max(intervals_ms):.3f}")
    return mean, sd


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    flips = load_probe(sys.argv[1])
    probe_iv = [(b - a) * 1000 for a, b in zip(flips, flips[1:])]
    frame_iv = load_mangohud(sys.argv[2])

    stats_ms(probe_iv, "probe packets")
    stats_ms(frame_iv, "game frames (MangoHud)")

    # medians: robust against missed-frame outliers that skew the mean
    p_med = statistics.median(probe_iv)
    f_med = statistics.median(frame_iv)
    period = f_med  # one frame
    drift_ppm = (p_med - f_med) / f_med * 1e6
    print(f"\nrelative rate error (median-based): {drift_ppm:+.1f} ppm")
    if abs(drift_ppm) < 0.5:
        print("rates indistinguishable at this measurement length")
    else:
        # time for accumulated phase error to reach one full frame period
        slip_s = (period / 1000.0) / (abs(drift_ppm) * 1e-6)
        print(f"predicted drift parity slip: one frame every {slip_s:.0f} s "
              f"({slip_s/60:.1f} min)")

    # frame-time tail: every frame much longer than the period is a missed
    # vsync interval = an immediate parity flip
    long_frames = [v for v in frame_iv if v > period * 1.5]
    print(f"\nframes >1.5x period (missed-cadence events): {len(long_frames)}"
          f" of {len(frame_iv)}"
          f" ({len(long_frames)/len(frame_iv)*100:.3f}%)")
    if long_frames:
        total_s = sum(frame_iv) / 1000.0
        print(f"  -> about one cadence slip every {total_s/len(long_frames):.1f} s")

    print("\nVERDICT GUIDE:")
    print("  drift slips + cadence slips together give the expected interval")
    print("  between eye-swaps for an external sender. The phase-3 present-")
    print("  hook eliminates drift slips entirely and turns cadence slips")
    print("  into stale-but-correct frames instead of parity errors.")


if __name__ == "__main__":
    main()
