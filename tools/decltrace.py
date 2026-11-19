#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Reads X4VR_DECLTRACE lines: per view, how far X4's camera (as drawn) strays from
head-look's command (as declared) from one frame to the next, and which delay of
the command explains X4's camera best.

    python3 tools/decltrace.py <log>

Takes 447 and 447b (a Steam Frame and a Quest 3): in every view X4's camera
stepped by the command's step ONE frame earlier, to 0.000 deg at p90 -- X4 draws
head-look's command a frame late.
"""
import re
import statistics as st
import sys

RX = re.compile(r"\[\s*([\d.]+)\]\s+layer\s+decltrace: f=(\d+) (seat|foot|ext) "
                r"cam=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) cmd=(-?[\d.]+),(-?[\d.]+)( \(idle\))? "
                r"pass=([\d.]+) tick=(-?[\d.]+) ms ago, (\d+) tick")


def wrap(a):
    while a > 180.0:
        a -= 360.0
    while a < -180.0:
        a += 360.0
    return a


def pct(xs, q):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


rows = []
for line in open(sys.argv[1], errors="replace"):
    m = RX.search(line)
    if m:
        t, f, v, cy, cp, cr, my, mp, idle, pas, tick, ticks = m.groups()
        rows.append(dict(t=float(t), f=int(f), v=v, cy=float(cy), cp=float(cp), cr=float(cr),
                         my=float(my), mp=float(mp), idle=bool(idle), tick=float(tick)))
print(f"{len(rows)} frame(s) traced")

# Runs of consecutive frames in one view with head-look driving (not idle).
runs, cur = [], []
for r in rows:
    if cur and (r["v"] != cur[-1]["v"] or r["f"] != cur[-1]["f"] + 1 or r["idle"]):
        if len(cur) > 10:
            runs.append(cur)
        cur = []
    if not r["idle"]:
        cur.append(r)
if len(cur) > 10:
    runs.append(cur)

for view in ("seat", "foot", "ext"):
    vr = [run for run in runs if run[0]["v"] == view]
    if not vr:
        continue
    n = sum(len(x) for x in vr)
    # Frame-to-frame change of (camera - command): what a declaration of the command misses.
    dd_y, dd_p, roll_d, head_spd = [], [], [], []
    lagerr = {L: ([], []) for L in range(4)}
    for run in vr:
        for i in range(1, len(run)):
            a, b = run[i - 1], run[i]
            dd_y.append(abs(wrap((b["cy"] - b["my"]) - (a["cy"] - a["my"]))))
            dd_p.append(abs((b["cp"] - b["mp"]) - (a["cp"] - a["mp"])))
            roll_d.append(abs(wrap(b["cr"] - a["cr"])))
            head_spd.append(abs(wrap(b["my"] - a["my"])))
        for L in range(4):
            for i in range(L + 1, len(run)):
                dcam_y = wrap(run[i]["cy"] - run[i - 1]["cy"])
                dcam_p = run[i]["cp"] - run[i - 1]["cp"]
                dcmd_y = wrap(run[i - L]["my"] - run[i - L - 1]["my"])
                dcmd_p = run[i - L]["mp"] - run[i - L - 1]["mp"]
                lagerr[L][0].append(abs(dcam_y - dcmd_y))
                lagerr[L][1].append(abs(dcam_p - dcmd_p))
    print(f"\n== {view}: {n} frame(s) in {len(vr)} run(s); head-look moved median "
          f"{st.median(head_spd):.3f} p90 {pct(head_spd, .9):.3f} deg/frame")
    print(f"   (camera - command) changed per frame: yaw median {st.median(dd_y):.3f} "
          f"p90 {pct(dd_y, .9):.3f} max {max(dd_y):.3f} | pitch median {st.median(dd_p):.3f} "
          f"p90 {pct(dd_p, .9):.3f} max {max(dd_p):.3f} deg")
    print(f"   camera roll changed per frame: median {st.median(roll_d):.3f} "
          f"p90 {pct(roll_d, .9):.3f} max {max(roll_d):.3f} deg")
    for L in range(4):
        ey, ep = lagerr[L]
        print(f"   camera step vs the command's step {L} frame(s) earlier: yaw mean "
              f"{st.mean(ey):.3f} p90 {pct(ey, .9):.3f} | pitch mean {st.mean(ep):.3f} "
              f"p90 {pct(ep, .9):.3f} deg")
