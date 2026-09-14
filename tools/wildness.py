#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""What actually decides whether X4's camera goes wild -- task #75.

Answers the question "what makes 285b-s1 different from s2" by refusing its
premise: wildness is not a property of a session. s2 contains a contiguous
episode; the rest of s2 is indistinguishable from s1.

Two things this does that the earlier comparisons did not:

* **Windows come from the program's own record of what it was doing** --
  `inputrate` lines with steering above 50% and deltas going out above 5 Hz.
  Takes 280-284 have no such window at all, and the old control table was built
  on windows where the servo was not running. [[x4vr-window-from-system-state]]

* **The severity statistic can express the failure.** The log's ">20 deg per
  camera update" slam counter reads 0.0% on a camera pinned to its stop, because
  a pinned camera does not move -- `servo_rate.cpp` section 5 records it doing
  exactly that. This uses p99 of |dyaw| over `camstamp`'s per-frame atdraw
  stream, which rises when the camera oscillates and does not care whether it
  reaches a clamp. [[statistic-cannot-express-the-alternative]]

**`tk/obs` IS NOT A PREDICTOR. The first version of this file said it was, and
that finding is withdrawn.** The camera rate came from counting readbacks that
CHANGED, and three separate things make a changed readback vanish exactly when
the camera is slamming:

* it was **pinned on X4's yaw stop** (65 deg) -- no change to report;
* it was **pinned on the pitch stop** (35 deg) -- likewise;
* only **pitch** moved, and the rate was read from yaw alone.

All three depress the denominator of tk/obs during precisely the episodes it was
being used to predict. Counting a publish as an update when either axis moved or
either axis sat on its stop:

      285b-s2   wild 25.6 -> 63.4 Hz    tk/obs 3.22 -> 1.30   calm 1.22
      287b      wild 25.5 -> 63.7 Hz    tk/obs 3.15 -> 1.26   calm 1.33  INVERTED
      292       wild 28.1 -> 48.7 Hz    tk/obs 2.66 -> 1.54   calm 1.22

In two sessions of three the camera rate during the wild episode is **higher**
than during that session's own calm windows, and in 287b the corrected tk/obs is
LOWER when wild. It does not separate the two states.
[[a-ratio-against-its-own-denominator]]

**What still stands** is only the description: s1 has no wild window, s2's
wildness is one contiguous episode, and s1 matches s2's calm phase. That comes
from p99 |dyaw| on the camera stream and never used tk/obs. **Nothing measured so
far explains what starts an episode.**

Usage:  python3 tools/wildness.py <log> [<log> ...]
"""
import bisect
import os
import re
import sys

# `moved on N% of READS` is capitalised in the log; matching it lowercase is why
# the first run of this reported "0 servo-active windows" on every file.
IR = re.compile(
    r"^\[\s*(\d+\.\d+)\]\s+inject\s+inputrate: \d+ ticks in ([\d.]+) s = ([\d.]+) Hz \| "
    r"steering on ([\d.]+)% of ticks, delta sent on ([\d.]+)% of STEERING ticks \(=([\d.]+) Hz\) \| "
    r"camera read on [\d.]+% of ticks, moved on [\d.]+% of READS \(=([\d.]+) Hz\)", re.M)
CS = re.compile(r"^\[\s*(\d+\.\d+)\]\s+layer\s+camstamp: atdraw=(-?[\d.]+),(-?[\d.]+)", re.M)

WILD_DEG = 15.0   # p99 |dyaw| above this is an oscillating camera, not steering


def windows(path):
    """(tick, camera raw, camera corrected, X4 fps, p99|dyaw|) per window."""
    with open(path, errors="replace") as fh:
        text = fh.read()
    active = [(float(end) - float(dur), float(end), float(tick), float(cam))
              for end, dur, tick, steer, _sent, dhz, cam in IR.findall(text)
              if float(steer) > 50.0 and float(dhz) > 5.0 and float(cam) > 0.0]
    stamps = sorted((float(t), float(y), float(p)) for t, y, p in CS.findall(text))
    times = [t for t, _, _ in stamps]
    # This session's stop: the largest |yaw| the camera REPEATS consecutively,
    # i.e. a value it sat on rather than passed through. Not hard-coded 65 --
    # take 292 saturated at 79.64, which `<internal>`'s trackmaxyaw="85" allows,
    # and a fixed 65 would have mis-corrected it.
    # **A stop is where the camera DWELLS, and the test is dwell mass.** Two
    # earlier rules each worked on half the sessions. "Largest repeated value"
    # gave 285b-s1 a stop of 24.52 deg in a session that never clamped. Adding
    # ">= 0.90 * peak" fixed that and then excluded take 292's real stop: 292
    # sits at 65.00 for 1442 samples but its peak is 79.64, so the rule demanded
    # dwell above 71.68 and found none -- and that wrongly made 292's low camera
    # rate look real.
    #
    # Position does not separate them; dwell does. The most-repeated value away
    # from the at-rest zero, as a share of all samples:
    #
    #   285b-s2  65.00  3.77%      285b-s1   7.91  0.27%
    #   287b     65.00  2.82%      287a     43.00  0.12%
    #   292      65.00  3.86%      291       5.84  0.09%
    #
    # An order of magnitude apart, so 1% is a floor with room either side.
    repeats = {}
    allyaw = [y for _, y, _ in stamps]
    for k in range(len(allyaw) - 1):
        if abs(allyaw[k + 1] - allyaw[k]) <= 1e-9 and abs(allyaw[k]) > 5.0:
            key = round(abs(allyaw[k]), 2)
            repeats[key] = repeats.get(key, 0) + 1
    stop = None
    if repeats and allyaw:
        cand, hits = max(repeats.items(), key=lambda kv: kv[1])
        if hits >= 0.01 * len(allyaw):
            stop = cand
    out = []
    for lo, hi, tick, cam in active:
        i = bisect.bisect_left(times, lo)
        j = bisect.bisect_right(times, hi)
        seg = stamps[i:j]
        yaw = [y for _, y, _ in seg]
        pit = [p for _, _, p in seg]
        if len(yaw) < 30:
            continue
        moves = [abs(yaw[k + 1] - yaw[k]) for k in range(len(yaw) - 1)
                 if abs(yaw[k + 1] - yaw[k]) > 1e-9]
        if not moves:
            continue
        # **Both rates from camstamp, never one from each source.** The first
        # version took `raw` from `inputrate` (counted at the servo tick) and
        # `corr` from camstamp (per submitted frame) and printed them side by
        # side -- two different populations, which is why a "correction" that can
        # only ADD updates came out lower than what it corrected.
        n = len(yaw) - 1
        fps = len(yaw) / (hi - lo)
        moved = sum(1 for k in range(n) if abs(yaw[k + 1] - yaw[k]) > 1e-9)
        # **Either axis, and either stop.** Reading yaw alone counts a publish
        # that moved only pitch as "no update", and X4's pitch has its own stop at
        # maxpitch=35. Both omissions depress the rate exactly when the camera is
        # slamming, which is when it is pinned in one axis or both. Counting them
        # takes 285b-s2's wild windows from 25.6 Hz to 63.4 -- ABOVE its own calm
        # windows, not half of them.
        seen = sum(1 for k in range(n)
                   if abs(yaw[k + 1] - yaw[k]) > 1e-9
                   or abs(pit[k + 1] - pit[k]) > 1e-9
                   or (stop is not None and abs(abs(yaw[k]) - stop) < 0.5)
                   or abs(abs(pit[k]) - 35.0) < 0.5)
        out.append((tick, moved / n * fps, seen / n * fps, fps,
                    sorted(moves)[int(0.99 * len(moves))]))
    return out


def main(paths):
    pool = []
    for p in paths:
        rows = windows(p)
        name = os.path.basename(p).replace("x4vr-take", "").replace(".log", "")
        wild = [r for r in rows if r[4] > WILD_DEG]
        calm = [r for r in rows if r[4] <= WILD_DEG]
        print(f"=== {name}: {len(rows)} servo-active window(s), "
              f"{len(wild)} wild ({100 * len(wild) / max(len(rows), 1):.0f}%)")
        if rows:
            print("    " + "".join("W" if r[4] > WILD_DEG else "." for r in rows))
        for label, grp in (("WILD", wild), ("calm", calm)):
            if not grp:
                continue
            n = len(grp)
            print(f"      {label}  tick {sum(r[0] for r in grp) / n:5.1f} Hz   "
                  f"camera raw {sum(r[1] for r in grp) / n:5.1f} corr "
                  f"{sum(r[2] for r in grp) / n:5.1f} Hz   "
                  f"tk/obs raw {sum(r[0] for r in grp) / sum(r[1] for r in grp):5.2f} "
                  f"CORR {sum(r[0] for r in grp) / sum(r[2] for r in grp):5.2f}   "
                  f"p99|dyaw| {sum(r[4] for r in grp) / n:6.2f} deg")
        pool += [(r[0], r[2], r[0] / r[2], r[4] > WILD_DEG) for r in rows]

    if len(paths) < 2:
        return 0
    # **The 2x2 that matters.** Tick rate and ticks-per-observation are collinear
    # within any one session, so a single-variable table names whichever the
    # session happened to vary -- pooling 285b/287 alone made tick rate look like
    # a clean threshold (100% wild above 80 Hz), and take 292 inverted it.
    # Holding one and varying the other is the only way to tell them apart.
    tb = [(20, 60), (60, 70), (70, 78), (78, 95)]
    ob = [(0.9, 1.3), (1.3, 1.7), (1.7, 2.2), (2.2, 4.5)]
    print(f"\n{len(pool)} window(s) pooled. Share WILD, with the count:\n")
    print(f"{'tk/obs corr':<12}" + "".join(f"{f'tick {a}-{b}':>14}" for a, b in tb)
          + f"{'ROW':>12}")
    for olo, ohi in ob:
        row = f"{olo:.1f}-{ohi:<8.1f}"
        band = [p for p in pool if olo <= p[2] < ohi]
        for tlo, thi in tb:
            g = [p for p in band if tlo <= p[0] < thi]
            row += f"{(f'{100 * sum(1 for p in g if p[3]) / len(g):.0f}% ({len(g)})' if g else '-'):>14}"
        row += f"{(f'{100 * sum(1 for p in band if p[3]) / len(band):.0f}% ({len(band)})' if band else '-'):>12}"
        print(row)
    print(f"{'COLUMN':<12}", end="")
    for tlo, thi in tb:
        g = [p for p in pool if tlo <= p[0] < thi]
        print(f"{(f'{100 * sum(1 for p in g if p[3]) / len(g):.0f}% ({len(g)})' if g else '-'):>14}",
              end="")
    print("\n\nDown a column the tick rate is held and tk/obs varies; across a row"
          "\ntk/obs is held and the tick rate varies. Wildness follows the rows.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
