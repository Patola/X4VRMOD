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
CS = re.compile(r"^\[\s*(\d+\.\d+)\]\s+layer\s+camstamp: atdraw=(-?[\d.]+),", re.M)

WILD_DEG = 15.0   # p99 |dyaw| above this is an oscillating camera, not steering


def windows(path):
    """(tick Hz, camera Hz, X4 fps, p99|dyaw|) for each servo-active window."""
    with open(path, errors="replace") as fh:
        text = fh.read()
    active = [(float(end) - float(dur), float(end), float(tick), float(cam))
              for end, dur, tick, steer, _sent, dhz, cam in IR.findall(text)
              if float(steer) > 50.0 and float(dhz) > 5.0 and float(cam) > 0.0]
    stamps = sorted((float(t), float(y)) for t, y in CS.findall(text))
    times = [t for t, _ in stamps]
    out = []
    for lo, hi, tick, cam in active:
        i = bisect.bisect_left(times, lo)
        j = bisect.bisect_right(times, hi)
        yaw = [y for _, y in stamps[i:j]]
        if len(yaw) < 30:
            continue
        moves = [abs(yaw[k + 1] - yaw[k]) for k in range(len(yaw) - 1)
                 if abs(yaw[k + 1] - yaw[k]) > 1e-9]
        if not moves:
            continue
        out.append((tick, cam, len(yaw) / (hi - lo),
                    sorted(moves)[int(0.99 * len(moves))]))
    return out


def main(paths):
    pool = []
    for p in paths:
        rows = windows(p)
        name = os.path.basename(p).replace("x4vr-take", "").replace(".log", "")
        wild = [r for r in rows if r[3] > WILD_DEG]
        calm = [r for r in rows if r[3] <= WILD_DEG]
        print(f"=== {name}: {len(rows)} servo-active window(s), "
              f"{len(wild)} wild ({100 * len(wild) / max(len(rows), 1):.0f}%)")
        if rows:
            print("    " + "".join("W" if r[3] > WILD_DEG else "." for r in rows))
        for label, grp in (("WILD", wild), ("calm", calm)):
            if not grp:
                continue
            n = len(grp)
            print(f"      {label}  tick {sum(r[0] for r in grp) / n:5.1f} Hz   "
                  f"camera {sum(r[1] for r in grp) / n:5.1f} Hz   "
                  f"tk/obs {sum(r[0] for r in grp) / sum(r[1] for r in grp):5.2f}   "
                  f"X4 {sum(r[2] for r in grp) / n:5.1f} fps   "
                  f"p99|dyaw| {sum(r[3] for r in grp) / n:6.2f} deg")
        pool += [(r[0], r[1], r[0] / r[1], r[3] > WILD_DEG) for r in rows]

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
    print(f"{'tk/obs':<12}" + "".join(f"{f'tick {a}-{b}':>14}" for a, b in tb)
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
