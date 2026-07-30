#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Score one run's log, mechanically, so a person does not have to judge a screen.

Written after take forty-four, where a claim that drove a code change -- "the
same knobs produced 3 present candidates in one run and 6 in another, so the
predicate is scene-dependent" -- turned out to be one log file holding two X4
sessions. Per session both numbers were 3. The predicate was never unstable,
the configuration was always restorable, and a feature was built on a miscount.

That is the fourth time in this project a counting instrument has been read
without checking what it was counting over. So this script refuses to score a
log with more than one session in it, before looking at anything else.

Usage:  tools/score_run.py /tmp/x4vr-takeNN.log
Exit:   0 all checks pass, 1 a check failed, 2 the log cannot be scored
"""
import re
import sys

# A present target's layer 1 should hold roughly what layer 0 holds. It is not
# an equality: the two eyes see slightly different amounts of geometry, and
# every DIFFER line in a healthy run shows a small imbalance. Anything under
# this is the black-right-eye failure, where layer 1 has the HUD and nothing
# else (measured: 0.5%-30% across takes 41-44).
LAYER1_MIN_RATIO = 0.60


def main(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        print(f"cannot read {path}: {e}")
        return 2
    lines = text.splitlines()

    # Gate: one session, or nothing below this means anything.
    sessions = sum("mv: X4 uses" in ln for ln in lines)
    starts = sum("instance created (app=X4)" in ln for ln in lines)
    if sessions > 1 or starts > 1:
        print(f"UNSCORABLE  {sessions} X4 render-pass inventories, "
              f"{starts} X4 instances in this file.")
        print("  X4VR_LOG appends. Every tally below would be a sum over "
              "runs, which is how take forty-three produced a 3-versus-6 that "
              "was really 3-versus-3. Use a fresh X4VR_LOG per run.")
        return 2
    if sessions == 0:
        print("UNSCORABLE  no X4 render-pass inventory in this file "
              "(X4VR_MV_INVENTORY=1 not set, or the game never started).")
        return 2

    fails = []
    run = next((ln.split("env: run =", 1)[1].strip()
                for ln in lines if "env: run =" in ln), None)
    print(f"run  {run or '(not recorded — pre-take-34 build)'}")

    # 1. The split render. Without it nothing else is stereo.
    if any("SPLIT OFF" in ln for ln in lines):
        fails.append("SPLIT OFF — X4 did not render one eye")
    print(f"split  {'OFF (FAIL)' if fails else 'on'}")

    # 2. Every masked pass names the rule that masked it.
    if any("+MASKED(?)" in ln for ln in lines):
        fails.append("a pass was masked by no named rule")

    rules = {}
    for m in re.finditer(r"\+MASKED\((\w+)\)", text):
        rules[m.group(1)] = rules.get(m.group(1), 0) + 1
    print("masked  " + (", ".join(f"{k}={v}" for k, v in sorted(rules.items()))
                        or "nothing"))

    # 3. The one that says whether the right eye has a picture in it. Each
    #    present target is probed repeatedly; take the best sample per image,
    #    because a frame captured mid-fade is not evidence of a broken chain.
    best = {}
    for m in re.finditer(
            r"probe: img #(\d+) .*?non-empty (\d+)/(\d+)", text):
        img, l0, l1 = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if l0:
            best[img] = max(best.get(img, 0.0), l1 / l0)
    presents = {i: r for i, r in best.items() if 50 <= i <= 53}
    if not presents:
        fails.append("no probe samples for the present targets (#50-#53)")
    for img, ratio in sorted(presents.items()):
        ok = ratio >= LAYER1_MIN_RATIO
        print(f"img #{img}  layer1/layer0 non-empty = {ratio:6.1%}  "
              f"{'ok' if ok else 'FAIL — right eye is empty'}")
        if not ok:
            fails.append(f"img #{img} layer 1 holds {ratio:.1%} of layer 0")

    print()
    if fails:
        for f in fails:
            print(f"FAIL  {f}")
        return 1
    print("PASS  this run is a candidate for docs/known-good-runs.md")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
