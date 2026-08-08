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

# Take forty-five: a sample is only evidence if the scene had stopped changing
# when it was taken.
#
# The first version of this script took the best sample per image, on the
# stated grounds that "a frame captured mid-fade is not evidence of a broken
# chain". That is exactly backwards. Taking the best sample means a frame
# captured mid-load IS read as evidence of a working chain -- and that is what
# happened: two swapchain images sampled 3.7 s and 7.8 s after X4 created its
# last world render pass read 24.1% and 80.5%, against 4.4% for the two
# sampled before it, in a run whose screen was black in the right eye. The rule
# written to prevent a false negative manufactured a false positive instead.
#
# So: a sample counts only if no render pass was created in the QUIET_MIN
# seconds before it. X4 creates render passes while it loads and stops when the
# scene is up, which makes "time since the last CreateRenderPass" a usable
# proxy for "the scene has settled" that costs nothing to compute.
QUIET_MIN = 10.0


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

    # 3. Every writer of a swapchain image must be masked. The layer already
    #    detects the split -- a masked pass writes both layers, an unmasked one
    #    writes layer 0, so a target with both kinds of writer has a layer 1
    #    missing whatever the unmasked pass drew. That is the black right eye
    #    stated structurally, and it does not depend on any sample at all.
    # Both lists non-empty is what "mixed" means. An image with only unmasked
    # writers is not this defect -- it is a mono target, and several of X4's
    # are (#70-#74). Only the images that have it both ways lose half a frame.
    mixed = sorted(set(
        (m, u) for m, u in re.findall(
            r"img #\d+ writers — masked rp \[([\d, ]*)\] unmasked rp \[([\d, ]*)\]",
            text)
        if m.strip() and u.strip()))
    # A warning, not a failure, and take forty-six is why. It fired there on a
    # run whose layer 1 was bit-identical to layer 0 -- so the pass it names
    # cannot have contributed anything, and the detector over-claims. It is
    # built from framebuffers *created*, and a framebuffer is not a draw. Worth
    # printing, because it names a pass that would lose half a frame if it ever
    # did draw; not worth failing a run over.
    for masked, unmasked in mixed:
        print(f"warn  mixed writers — rp [{unmasked}] writes a swapchain image "
              f"unmasked while rp [{masked}] writes it masked. Layer 1 misses "
              f"whatever rp [{unmasked}] draws, if it draws.")

    # 4. Whether the right eye has a picture in it. #50-#53 are the four
    #    swapchain images -- "image 0 of 4" .. "image 3 of 4" in the inventory
    #    -- so they are ONE target sampled at four moments, not four
    #    independent checks. Take forty-four's four FAIL lines were one finding
    #    printed four times, and take forty-five's mixed 4.4/4.6/24.1/80.5 was
    #    one target crossing a load boundary mid-round.
    rp_times = sorted(float(t) for t in re.findall(
        r"\[ *([\d.]+)\] layer +rp #\d+\.\d+:", text))

    def quiet_for(ts):
        prior = [t for t in rp_times if t <= ts]
        return ts - prior[-1] if prior else float("inf")

    # Take forty-six: IDENTICAL lines carry no "non-empty" field, and the first
    # version of this section matched on that field alone -- so a run whose
    # swapchain layer 1 was a bit-exact copy of layer 0 scored "no settled
    # samples" and FAILED. That is the best ratio the probe can report, read as
    # an absence of data. Same family as hole #16: the parser decided what
    # counted as evidence, and the shape it could not parse was the good one.
    #
    # IDENTICAL is not the goal, though, and must not be printed as if it were:
    # it means both eyes hold the same pixels, which on screen is a duplicated
    # left eye -- a picture in the right eye with no parallax in it. Graded
    # separately below.
    samples, unsettled = [], 0
    for m in re.finditer(
            r"\[ *([\d.]+)\] layer +mv probe: img #(\d+) [\dx]+ +"
            r"layer0=\w+(?: \(all zero\))? +layer1=\w+(?: \(all zero\))? +"
            r"(IDENTICAL|DIFFER \S+ \S+  non-empty (\d+)/(\d+))",
            text):
        ts, img = float(m.group(1)), int(m.group(2))
        identical = m.group(3).startswith("IDENTICAL")
        if not (50 <= img <= 53):
            continue
        if identical:
            ratio = 1.0
        else:
            l0, l1 = int(m.group(4)), int(m.group(5))
            if not l0:
                continue
            ratio = l1 / l0
        q = quiet_for(ts)
        if q < QUIET_MIN:
            unsettled += 1
            print(f"  (skipped img #{img} at {ts:.0f}: {q:.1f}s after a render "
                  f"pass was created — scene still loading, ratio {ratio:.1%})")
            continue
        samples.append((ts, img, ratio, identical))

    if unsettled:
        print(f"swapchain  {unsettled} sample(s) skipped as unsettled "
              f"(< {QUIET_MIN:.0f}s quiet)")
    if not samples:
        fails.append("no settled probe samples for the swapchain (#50-#53) — "
                     "load a savegame and sit still longer")
    else:
        ratios = [r for _, _, r, _ in samples]
        dupes = sum(1 for *_, ident in samples if ident)
        best, worst = max(ratios), min(ratios)
        print(f"swapchain  {len(samples)} settled sample(s), layer1/layer0 "
              f"non-empty {worst:.1%}..{best:.1%}, {dupes} bit-identical  "
              f"(imgs {sorted({i for _, i, _, _ in samples})})")
        # Judge on the WORST settled sample, not the best. "Best" was chosen to
        # tolerate transition frames; the quiet rule now does that job, so what
        # is left is signal. Takes forty-four and forty-five both reach 100% on
        # their splash-screen frames -- where X4 draws the same thing to both
        # layers -- while their cockpit frames sit at 0.4%. Scored on the best
        # sample they read as partial successes; on the worst they read as what
        # Patola saw, a black right eye.
        if worst < LAYER1_MIN_RATIO:
            fails.append(f"swapchain layer 1 holds {worst:.1%} of layer 0 in "
                         f"its worst settled frame — the right eye is the HUD "
                         f"and nothing else (best frame: {best:.1%}, usually "
                         f"the splash, where both eyes get the same pixels)")
        elif dupes == len(samples):
            # Not a failure. This is the state the project had working, and
            # getting back to it is the whole point of the bisection -- but it
            # is one eye shown twice, and calling it PASS would let the next
            # run mistake "a picture in both eyes" for "stereo".
            print("grade  DUPLICATE — layer 1 is a bit-exact copy of layer 0. "
                  "Both eyes have a picture; neither has parallax.")
        elif dupes:
            print(f"grade  MIXED — {dupes} of {len(samples)} samples are "
                  f"bit-identical, the rest carry a real difference.")
        else:
            print("grade  STEREO — every settled sample carries a real "
                  "per-eye difference.")

    # Task #30: the canvas, scored from the log rather than from the screen.
    #
    # Silent when no canvas was asked for. When one was, every way it can fail
    # produces a frame that looks correct or nearly so -- a mono UI, or a UI
    # that moved while the pointer did not -- so each is named here rather than
    # left to the eye.
    canvas_cfg = next((ln for ln in lines if "canvas: " in ln
                       and " m -> s=" in ln), None)
    canvas_refused_cfg = [ln for ln in lines if "canvas: REFUSED" in ln]
    canvas_tally = [ln for ln in lines if "canvas final: " in ln
                    or "canvas first present: " in ln]
    if not canvas_cfg and not canvas_refused_cfg and not canvas_tally:
        pass  # no canvas in this run; nothing to say
    else:
        for ln in canvas_refused_cfg:
            fails.append("canvas REFUSED — " + ln.split("REFUSED", 1)[1]
                         .lstrip("— -"))
        if canvas_cfg:
            print("canvas " + canvas_cfg.split("canvas: ", 1)[1])
        for ln in canvas_tally:
            print("canvas " + ln.split("canvas ", 1)[1])
        # Built but never bound is the failure mode that reproduces the old
        # frame exactly: the variants exist, no pipeline took one, and there is
        # nothing on screen to notice.
        final = next((ln for ln in canvas_tally if "canvas final: " in ln), None)
        if final:
            m = re.search(r"swapped into (\d+) pipeline stage", final)
            if m and int(m.group(1)) == 0:
                fails.append("canvas variants were built but no pipeline ever "
                             "bound one — the UI is still mono and the frame "
                             "is byte-for-byte the pre-canvas one")
            m = re.search(r"(\d+) REFUSED", final)
            if m and int(m.group(1)):
                fails.append(f"{m.group(1)} module(s) could not get a canvas "
                             "variant — their UI stays mono while the rest of "
                             "it moves")
        # The pointer and the UI must be given the SAME shift. They are set
        # from one variable, so a disagreement here means the cursor read it
        # before it was published -- which would put the pointer a fixed
        # distance from every button it activates, in a frame where both
        # features report success.
        cur = next((ln for ln in lines if "canvas shift" in ln), None)
        if canvas_cfg and cur:
            want = re.search(r" s=([\d.eE+-]+) NDC", canvas_cfg)
            got = re.search(r"canvas shift ([\d.eE+-]+) NDC", cur)
            if want and got and abs(float(want.group(1)) -
                                    float(got.group(1))) > 1e-6:
                fails.append(f"the UI was shifted by {want.group(1)} but the "
                             f"cursor by {got.group(1)} — the pointer sits off "
                             "every button it activates")
        elif canvas_cfg and not cur:
            print("canvas  (the cursor overlay never drew, so its shift is "
                  "unchecked)")

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
