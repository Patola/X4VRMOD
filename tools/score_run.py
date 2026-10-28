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
import math
import os
import bisect
import re
import sys

# X4's <fov> config tag multiplies the horizontal field as an ANGLE, linearly:
# horizontal FOV = fov * BASE_DEG at a 1:1 eye. Measured in take 104, not
# assumed -- two pairs an order of magnitude apart (1.15174/0.69231 and
# 37.75372/27.96006) both solve to 73.7399 and 73.7397, which is 2*atan(0.75),
# i.e. sx 4/3 exactly. Used only to label the sx inventory below.
FOV_BASE_DEG = 73.7399

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


def wrap180(d):
    """A yaw difference the short way round. Since take 318 the extension lets
    the cockpit camera turn a full circle and X4 wraps at +-180, so an
    unwrapped difference reads ~360 for a frame that is off by a fraction of a
    degree (take 318's poselag max 359.656)."""
    return (d + 180.0) % 360.0 - 180.0


def sbs_default_eye():
    """The eye size a run gets when it sets no X4VR_RES.

    Read from common/x4vr_sbs.hpp rather than copied, for the same reason the
    launcher reads it there: a fourth copy of the SBS size is how the three
    extents became four. Falls back to the values that header has carried since
    the beginning if it cannot be found, and says which it used.
    """
    hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "common", "x4vr_sbs.hpp")
    dims = {}
    try:
        for m in re.finditer(r"#define X4VR_SBS_(WIDTH|HEIGHT)\s+(\d+)",
                             open(hdr, encoding="utf-8").read()):
            dims[m.group(1)] = int(m.group(2))
    except OSError:
        pass
    if "WIDTH" in dims and "HEIGHT" in dims:
        return dims["WIDTH"] // 2, dims["HEIGHT"], "common/x4vr_sbs.hpp"
    return 1408, 1408, "the built-in fallback, x4vr_sbs.hpp unreadable"


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
    # The inventory line is printed only under X4VR_MV, so a run that never
    # asked for stereo (take 323, flatscreen) has none and was refused as
    # "stopped early" with 88 lines of its own measurement in the file. Gate
    # on intent: without X4VR_MV the X4 instance is the session.
    run0 = next((ln.split("env: run =", 1)[1] for ln in lines
                 if "env: run =" in ln), "")
    mono = bool(run0) and not re.search(r"\bX4VR_MV=[^0\s]", run0)
    if mono and starts == 1 and sessions == 0:
        sessions = 1
        print("mono  X4VR_MV not set: a flatscreen run. The X4 instance is the "
              "session; the stereo checks below have nothing to check.")
    if sessions > 1 or starts > 1:
        print(f"UNSCORABLE  {sessions} X4 render-pass inventories, "
              f"{starts} X4 instances in this file.")
        print("  X4VR_LOG appends. Every tally below would be a sum over "
              "runs, which is how take forty-three produced a 3-versus-6 that "
              "was really 3-versus-3. Use a fresh X4VR_LOG per run.")
        return 2
    if sessions == 0:
        # "Never started" and "started, then died" are different problems with
        # the same missing line, and take 111 was the second while the message
        # said the first. What separates them is already in the file.
        if starts:
            last = next((ln for ln in reversed(lines) if ln.strip()), "")
            print("UNSCORABLE  X4's instance was created but no render-pass "
                  "inventory was ever printed — the game started and then "
                  "stopped early.")
            print(f"  last line: {last.strip()[:160]}")
            print("  If it crashed, the backtrace is the measurement: "
                  "coredumpctl list, then coredumpctl debug <PID>.")
        else:
            print("UNSCORABLE  no X4 render-pass inventory in this file "
                  "(X4VR_MV_INVENTORY=1 not set, or the game never started).")
        return 2

    fails = []
    # The poselag gain error, hoisted so the camstamp section can scale
    # against it whether or not the poselag branches that fill it ran.
    gain = []
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
    # Did this run *ask* for the probe? Take 103 scored FAIL on "the probe never
    # sampled a swapchain image" in a run that never enabled the probe -- so the
    # acceptance check reported a defect for a question the run did not pose,
    # which is the fastest way to teach someone to stop reading the scorer.
    # Gate on intent, never on outcome: the env line records what was asked for.
    m_probe = re.search(r"X4VR_MV_PROBE=(\S*)", run or "")
    probe_asked = bool(m_probe and m_probe.group(1) not in ("", "0"))
    if run is None:  # pre-take-34 build: no env line, so intent is unknowable
        probe_asked = any("mv probe:" in ln for ln in lines)

    # Task #31: the three extents, checked rather than left to be compared by
    # eye across three lines in three components. Take 101 ran with X4's render
    # at 1408x1408, the composite at 2816x1408 and the window at 2816x792, and
    # every component was behaving exactly as written -- which is why nothing
    # reported it and the aspect test that run existed for measured nothing.
    m_ext = re.search(
        r"extents: X4 renders (\d+)x(\d+), this run's eye is (\d+)x(\d+) "
        r"\(from ([^)]+)\), the composite presents (\d+)x(\d+) -- (.+)", text)
    m_win = re.search(r"sdl: SDL_GetWindowSize -> (\d+)x(\d+)", text)
    m_swap = re.search(r"WARNING swapchain is (\d+)x(\d+), expected (\d+)x(\d+)",
                       text)

    # Works on every log that has an env line and a window line, whatever build
    # wrote it -- it uses the run's own declared knobs and the window X4 was
    # actually given, not a layer message whose meaning changed between builds.
    # Checked offline over all 74 logs before being enabled: it fires on take
    # 101 and on nothing else, which is exactly the run this file records as
    # task #31.
    if run and "X4VR_SBS=1" in run and m_win:
        m_res = re.search(r"X4VR_RES=(\d+)x(\d+)", run)
        if m_res:
            ew, eh, src = int(m_res.group(1)), int(m_res.group(2)), "X4VR_RES"
        else:
            ew, eh, src = sbs_default_eye()
        ww, wh = int(m_win.group(1)), int(m_win.group(2))
        if ww != 2 * ew or wh != eh:
            fails.append(f"the eye is {ew}x{eh} (from {src}) but X4's window is "
                         f"{ww}x{wh} — a side-by-side frame of {2 * ew}x{eh} "
                         f"does not fit it, so the two are different rectangles "
                         f"and anything measured about aspect is about neither "
                         f"(take 101)")

    if m_ext:
        rw, rh, ew, eh, src, cw, ch, verdict = m_ext.groups()
        win = f", window {m_win.group(1)}x{m_win.group(2)}" if m_win else ""
        print(f"extents  X4 renders {rw}x{rh}, eye {ew}x{eh} (from {src}), "
              f"composite {cw}x{ch}{win}")
        if "DISAGREE" in verdict:
            fails.append(f"the extents disagree — X4 rendered {rw}x{rh} for an "
                         f"eye of {ew}x{eh}, so the composite duplicates the "
                         f"left half and nothing in this run is stereo")
        # The window is the one extent the layer's own line cannot see: it
        # belongs to gamescope and the WSI, and take 101's whole defect was the
        # composite being twice as tall as the window it was presented into.
        if m_win and (int(m_win.group(1)) != int(cw)
                      or int(m_win.group(2)) != int(ch)):
            fails.append(f"the composite is {cw}x{ch} but X4's window is "
                         f"{m_win.group(1)}x{m_win.group(2)} — the frame does "
                         f"not fit the surface it is presented into (take 101)")
    elif m_swap:
        # NOT a failure, and the first version of this made it one -- which
        # failed 60 of the 74 logs on disk, including every tagged known-good
        # state. 69 logs carry this warning and it stops at take 103, where the
        # launcher fix landed. In those builds the line compared against one
        # number and PRINTED another (the compiled constant), which is recorded
        # in the layer's own comment: take 60's reads "swapchain is 2816x1408,
        # expected 2816x1408" -- a warning whose two numbers are equal. What it
        # actually compared cannot be recovered from the log, so it cannot be
        # scored on, and treating it as evidence was reading an instrument
        # without checking what it measured.
        print(f"note  this log carries the pre-take-103 'WARNING swapchain "
              f"{m_swap.group(1)}x{m_swap.group(2)}, expected "
              f"{m_swap.group(3)}x{m_swap.group(4)}' line, whose 'expected' is "
              f"a constant rather than the value compared. Extents unjudged "
              f"here — the 'extents:' line is what judges them, from take 109 on")

    # Task #32. The present dumps answer the same question the probe walk does,
    # directly and without waiting for the walk to reach a swapchain image: they
    # are the finished eye image with both layers in it. Judged per region, not
    # in bulk -- see tools/eye_stereo.py for why the bulk ratio could not tell
    # a correct right eye from a copy of the left.
    m_mvd = re.search(r"X4VR_MV_DUMP=(\S+)", run or "")
    m_mvp = re.search(r"X4VR_MV_DUMP_PRESENT=(\S*)", run or "")
    dumps_asked = bool(m_mvd and m_mvp and m_mvp.group(1) not in ("", "0"))
    eye_frames = 0
    if dumps_asked:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import eye_stereo
        m_w = re.search(r"X4VR_RES=(\d+)x\d+", run or "")
        m_i = re.search(r"X4VR_IPD=([\d.]+)", run or "")
        m_s = re.search(r"X4VR_PROJ_SX=([\d.]+)", run or "")
        # PROJ_SX rather than the measured scene sx: the dumps show what the
        # shear actually did, and for the modules that bake it that IS the
        # constant. Where the two disagree the stereo line above says so.
        eye_frames = eye_stereo.report(
            m_mvd.group(1),
            w=int(m_w.group(1)) if m_w else None,
            sx=float(m_s.group(1)) if m_s else None,
            ipd=float(m_i.group(1)) if m_i else None)
        if not eye_frames:
            # Intent gate: this run asked for present dumps. Their absence is a
            # failure of the run, not a reason to stay quiet.
            fails.append(f"X4VR_MV_DUMP_PRESENT was set but no present dumps "
                         f"were found at {m_mvd.group(1)}-present-n*-layer*.{{png,ppm}} "
                         f"— the eye image was never written, so the right eye "
                         f"is unjudged in a run that asked to judge it")

    if not samples and not probe_asked:
        # Explicitly *not* silence, and explicitly not a pass. The right eye is
        # simply unjudged in this run, and saying so is the whole point.
        if not eye_frames:
            print("swapchain  unjudged — this run enabled neither "
                  "X4VR_MV_PROBE nor X4VR_MV_DUMP_PRESENT, so nothing here "
                  "speaks to the right eye's contents.")
    elif not samples:
        # Takes 97 and 98 both failed here while being perfectly healthy, and
        # the advice above ("sit still longer") sent the diagnosis the wrong
        # way. The probe walks the frame's images one at a time, roughly one
        # every 30 s, and in a 277 s run it reached eight of them -- none of
        # which were the swapchain. Sitting still does not help; the probe has
        # to get there, and in a short run it does not.
        #
        # Not softened into a pass: no evidence is no evidence. But it now says
        # which it is, and points at the instrument that answers the same
        # question directly, since X4VR_MV_DUMP_PRESENT writes the finished eye
        # image with both layers in it.
        if eye_frames:
            # Task #32, and this is the whole point of it: the present dumps
            # answered the right-eye question, so the probe's failure to walk as
            # far as a swapchain image is a redundancy that did not pay off, not
            # a defect in the run. Takes 97 and 98 carry 167 and 207 frames of
            # depth-varying parallax each and were failing on this line.
            #
            # Stated precisely rather than waved away, because the two
            # instruments do not watch the same link: the dumps prove the EYE
            # IMAGE is stereo, the probe would have proved the copy from it into
            # the swapchain kept both layers. That copy stays unverified here.
            print("warn  the probe never reached a swapchain image (#50-#53), "
                  "so the eye-image → swapchain copy is unverified in this run. "
                  "The eye image itself is judged above, from the present "
                  "dumps.")
        else:
            fails.append("the probe never sampled a swapchain image (#50-#53) — "
                         "it walks the frame one image at a time and a short run "
                         "does not reach them. This is no evidence, not a bad "
                         "eye: read the X4VR_MV_DUMP_PRESENT dumps instead, or "
                         "run for longer")
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
        #
        # The overlay stopped printing a `canvas shift` line long ago, and this
        # check then printed "the cursor overlay never drew" on every take --
        # including 314, which drew 18135 times. The map it applies is on its
        # first-draw line as `canvas eye0 A=(ax,ay) B=(bx,by)`; read that.
        cur = next((ln for ln in lines if "canvas shift" in ln), None)
        first = next((ln for ln in lines if "cursor: drawing" in ln), None)
        want = re.search(r" s=([\d.eE+-]+) NDC", canvas_cfg) if canvas_cfg \
            else None
        got = None
        if cur:
            m = re.search(r"canvas shift ([\d.eE+-]+) NDC", cur)
            got = float(m.group(1)) if m else None
        elif first:
            m = re.search(r"canvas eye0 A=\(([\d.eE+-]+),([\d.eE+-]+)\) "
                          r"B=\(([\d.eE+-]+),([\d.eE+-]+)\)", first)
            if m:
                ax, bx = float(m.group(1)), float(m.group(3))
                # An arm with no canvas map (X4VR_CURSOR_CANVAS=0, or ARM B of
                # the A/B) draws at the raw pointer: identity, and no shift to
                # compare. Saying so beats failing it for a shift it was never
                # meant to carry.
                if abs(ax - 1.0) < 1e-4 and abs(bx) < 1e-9:
                    print("canvas  cursor's first draw had NO canvas map (raw "
                          "pointer arm), so its shift is not comparable")
                else:
                    got = bx
        if canvas_cfg and want and got is not None:
            if abs(float(want.group(1)) - got) > 1e-6:
                fails.append(f"the UI was shifted by {want.group(1)} but the "
                             f"cursor by {got:+.5f} — the pointer sits off "
                             "every button it activates")
            else:
                print(f"canvas  cursor carries the UI's shift ({got:+.5f} "
                      f"NDC) — pointer and UI agree")
        elif canvas_cfg and not cur and not first:
            print("canvas  (the cursor overlay never drew, so its shift is "
                  "unchecked)")

    # Task #24/#23: X4's own projection, when the run asked to read it.
    #
    # The one number this whole task turns on is sx, and it has been quoted from
    # a screenshot exactly once -- three mutually inconsistent estimates, because
    # the planet shares the X4 logo's hue. So it is read from X4's matrices or
    # not at all, and if a run asks for the read and does not get it, that is a
    # failure rather than a quiet blank.
    m_dump = re.search(r"X4VR_DUMP_MATRICES=(\S*)", run or "")
    dump_asked = bool(m_dump and m_dump.group(1) not in ("", "0"))

    meas = re.search(r"proj MEASURED: sx=([\d.eE+-]+) sy=([\d.eE+-]+) "
                     r"near=([\d.eE+-]+)", text)
    unreadable = "proj: could not read terms" in text
    # sy is present only in logs from take 105 onward, near only from take 54,
    # and the cam# tag only from take 106. Named groups, because the numbered
    # ones already produced one silent bug here: findall yields '' for a group
    # that did not participate, never None, and adding an optional group at the
    # front renumbers every group after it.
    CHANGED_RE = re.compile(
        r"proj CHANGED\s+(?:cam#(?P<cam>\d+)\s+)?#(?P<i>\d+): "
        r"sx (?P<sx0>[\d.eE+-]+) -> (?P<sx>[\d.eE+-]+)"
        r"(?:\s+sy (?P<sy0>[\d.eE+-]+) -> (?P<sy>[\d.eE+-]+))?"
        r"(?:\s+near (?P<near0>[\d.eE+-]+) -> (?P<near>[\d.eE+-]+))?")

    def _sample(cam, sx, sy, near):
        return {"cam": int(cam) if cam else None,
                "sx": float(sx),
                "sy": float(sy) if sy else None,
                "near": float(near) if near else None}

    changes = [_sample(m.group("cam"), m.group("sx"), m.group("sy"),
                       m.group("near")) for m in CHANGED_RE.finditer(text)]
    # A camera's first sighting is a sample like any other, and from take 106 it
    # is the only line some cameras ever produce -- the steady ones, which are
    # exactly the ones that tell you which cameras ignore <fov>.
    changes += [_sample(m.group(1), m.group(2), m.group(3), m.group(4))
                for m in re.finditer(
                    r"proj CAMERA cam#(\d+): sx=([\d.eE+-]+) sy=([\d.eE+-]+) "
                    r"near=([\d.eE+-]+)", text)]

    if dump_asked or meas or unreadable:
        if unreadable:
            fails.append("X4's projection could not be read from "
                         "M_projectionUJ — the storage order was undetermined "
                         "or the block was unpopulated, and every sx below "
                         "would be a guess")
        elif not meas:
            fails.append("X4VR_DUMP_MATRICES was set but no 'proj MEASURED' "
                         "line appeared — the camera block was never credited "
                         "(needs 50+ draws through one block), so this run "
                         "measured nothing. Reach an actual scene, not the "
                         "splash")
        else:
            sx, sy, near = (float(g) for g in meas.groups())
            print(f"proj  sx={sx:.5f} sy={sy:.5f} near={near:.5f}")

            # Which extent did X4 build its projection from? |sy/sx| is the
            # aspect X4 used, and the run states the two candidates it could
            # have come from. This is task #31's question answered for free:
            # the render extent and the window are different rectangles here.
            if sx:
                got = abs(sy / sx)
                cands = []
                mr = re.search(r"X4VR_RES=(\d+)x(\d+)", run or "")
                if mr:
                    cands.append(("X4VR_RES render extent",
                                  int(mr.group(1)) / int(mr.group(2))))
                mw = re.search(r"X4VR_W=(\d+)", run or "")
                mh = re.search(r"X4VR_H=(\d+)", run or "")
                if mw and mh:
                    cands.append(("X4VR_W/H window",
                                  int(mw.group(1)) / int(mh.group(1))))
                hit = [n for n, a in cands if abs(a - got) < 0.01]
                desc = ", ".join(f"{n} {a:.3f}" for n, a in cands) or "none"
                print(f"proj  aspect |sy/sx| = {got:.3f}  (candidates: {desc})")
                if cands and not hit:
                    print("warn  X4's aspect matches neither extent this run "
                          "asked for — it sized its projection from something "
                          "else, and the shear derives from that number.")
                elif hit:
                    print(f"proj  X4 sized its projection from the "
                          f"{hit[0]}.")

            # A constant sx is only viable if sx never moves. Take 54 already
            # showed a 33x range under zoom; this reports the range actually
            # seen, so a run that never moved the camera cannot be mistaken for
            # evidence that sx is stable.
            if changes:
                # Take 54 recorded two samples with near=0.621 and sx≈0.001:
                # the layer's "most-drawn block wins" heuristic sometimes
                # credits a block that is not the camera the geometry drew
                # through. Blending those into the range turns a 3x zoom into a
                # 30000x one. They are separated, not dropped -- a silent filter
                # here would hide exactly the mis-credit that motivated moving
                # the shear into the shader.
                main = [c for c in changes
                        if c["near"] is None or abs(c["near"] - near) < 1e-3]
                other = [c for c in changes
                         if c["near"] is not None
                         and abs(c["near"] - near) >= 1e-3]
                vals = [c["sx"] for c in main] + [sx]
                print(f"proj  {len(changes)} change(s), sx range "
                      f"{min(vals):.5f}..{max(vals):.5f} "
                      f"({max(vals) / min(vals):.1f}x) over the near={near:.3f} "
                      f"camera")

                # The range line above is the one that nearly cost take 104 its
                # finding. X4 runs several cameras concurrently and the layer's
                # "most-drawn block wins" heuristic flips winners between submits
                # of one frame, so these samples interleave cameras. Three sx
                # values came back bit-identical across a 35% <fov> change while
                # others moved by exactly the knob's ratio -- min/max reads that
                # as "the range shifted" and loses it. So report the SET, with
                # the field angle each value implies and the <fov> tag that angle
                # corresponds to: a camera that honours the setting lands on the
                # tag this run asked for, and one that ignores it does not.
                # sy travels with sx in the same row, because the aspect is a
                # property of a camera and not of the run. Take 105 reported
                # "|sy/sx| ranges 1.000..1.500 — the field is being stretched"
                # and that was an artefact of aggregating: 0.75405 and 1.33333
                # were square to five decimals and a third camera, 3.78085, is
                # simply a 3:2 camera. The aggregate defect I had just fixed for
                # sx, still live for sy, one commit later.
                # Keyed on sx, with the sy values seen for it carried alongside
                # rather than folded into the key: pre-105 logs have no sy on
                # their CHANGED lines, and keying on the pair split one camera
                # into a with-sy row and a without-sy row. Two sy for one sx is
                # real information, so it is printed rather than collapsed.
                tally, sys_for = {}, {}
                for c in main + [{"sx": sx, "sy": sy, "near": near}]:
                    k = round(c["sx"], 5)
                    tally[k] = tally.get(k, 0) + 1
                    if c["sy"] is not None:
                        sys_for.setdefault(k, set()).add(round(c["sy"], 5))
                order = sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))
                m_fov = re.search(r"X4VR_FOV=([\d.]+)", run or "")
                asked = float(m_fov.group(1)) if m_fov else None
                shown = order[:8]
                print(f"proj  {len(order)} distinct sx — compare this SET "
                      f"against the previous take's, not the range:")
                eye = None
                for v, n in shown:
                    deg = 2 * math.degrees(math.atan(1.0 / v)) if v else 0.0
                    tag = deg / FOV_BASE_DEG
                    honours = asked is not None and abs(tag - asked) < 0.01
                    asps = sorted(abs(w / v) for w in sys_for.get(v, ())) if v \
                        else []
                    if honours and eye is None:
                        eye = (v, asps, n)
                    a = (" |sy/sx|=" + ",".join(f"{r:.4f}" for r in asps)
                         if asps else "")
                    print(f"      sx={v:<9.5f}{a} {deg:7.3f}° = fov {tag:.3f}"
                          f"   x{n}{'  <- honours X4VR_FOV' if honours else ''}")
                if len(order) > len(shown):
                    rest = sum(n for _, n in order[len(shown):])
                    print(f"      ... {len(order) - len(shown)} rarer value(s), "
                          f"{rest} sample(s), not shown")
                # Take 106: 36 CAMERA lines, 25 of them reading sx=1.33333.
                # cam#N is a UBO slot, and X4 multi-buffers one projection
                # across many of them -- so the slot count is a buffering
                # factor, not a camera count, and a projection that moves logs
                # its change once per slot it lives in. The distinct-sx set
                # above is the object to reason about; cam#N is not.
                slots = re.findall(
                    r"proj CAMERA cam#\d+: sx=([\d.eE+-]+) sy=([\d.eE+-]+) "
                    r"near=([\d.eE+-]+)", text)
                if slots:
                    print(f"proj  {len(slots)} slot(s) carried "
                          f"{len({s for s in slots})} distinct projection(s) — "
                          f"cam#N is a UBO slot, not a camera")

                # ---- task #53: what M_view holds, per credited slot ----
                #
                # This project believed M_view was always identity, from a
                # survey of the whole arena at ONE instant, and the layer
                # printed the matrix once from the most-drawn slot. 9 of 64
                # stored runs disagree. Same "cam#N is a slot" caveat as
                # above: report the distinct transforms, not the slot count.
                vid = re.findall(
                    r"proj VIEW cam#(\d+): M_view IDENTITY", text)
                vnon = re.findall(
                    r"proj VIEW cam#(\d+): M_view is NOT IDENTITY -- "
                    r"([\d.]+) deg about \(([-+\d.]+),([-+\d.]+),([-+\d.]+)\), "
                    r"offset ([\d.]+) m \(draws=(\d+), (\d+) in view-masked",
                    text)
                if not vid and not vnon:
                    # A note, NOT a fail. Every log this project has kept
                    # predates #53 and none of them can carry this line, and
                    # they are the scorer's own regression corpus -- a fail
                    # here would condemn 190 runs retroactively for a check
                    # that did not exist when they ran.
                    #
                    # What it must still not do is read as a clean result.
                    # "No non-identity cameras reported" and "nothing looked"
                    # are the same silence, and only one of them is evidence.
                    print("view  no 'proj VIEW' line — M_view was NOT decoded "
                          "this run (log predates #53, or the decode never "
                          "ran). This says NOTHING about whether X4 is "
                          "camera-relative; it is not 'all identity'")
                else:
                    print(f"view  {len(vid) + len(vnon)} slot(s) decoded, "
                          f"{len(vnon)} with a non-identity M_view")
                    distinct = {}
                    for _cam, ang, ax, ay, az, off, draws, mask in vnon:
                        key = (round(float(ang), 1), round(float(off), 2))
                        d, m, n = distinct.get(key, (0, 0, 0))
                        distinct[key] = (d + int(draws), m + int(mask), n + 1)
                    for (ang, off), (draws, mask, n) in sorted(
                            distinct.items(), key=lambda kv: -kv[1][0]):
                        print(f"      {ang:.1f}° / {off:.2f} m offset — "
                              f"{n} slot(s), {draws} draw(s), {mask} of them "
                              f"in view-masked passes")
                    if any(m for _d, m, _n in distinct.values()):
                        # A QUESTION, not a defect, and it is worded that way
                        # on purpose. The shear and the affine are derived
                        # from z_c==near and w_c==(that camera's) view z, and
                        # BOTH still hold for a camera with a non-identity
                        # M_view -- P*M_view still puts its own view z in w.
                        # What is not established is whether transforming such
                        # a camera per eye is what we want, since it is not
                        # the player's eye. Do not "fix" this before reading
                        # patch_vertex_eye_offset's derivation.
                        print("      ^ our per-eye transforms reach that "
                              "camera. NOT known to be a defect — the shear's "
                              "derivation holds in that camera's own frame — "
                              "but it is not the player's eye, so what it "
                              "draws is worth identifying (#53)")

                # ---- task #56: what the guard band costs in shadows ----
                #
                # The map size is already known fixed: 10 x 2048x2048 D16 in
                # every stored run from fov 1.0 to 1.5 and at both render
                # extents. What this reports is the cascade EXTENT, and the
                # number that answers #56 is its RATIO between two X4VR_FOV
                # values -- so it prints the raw scale to be compared, never a
                # verdict from one run.
                sh = re.findall(
                    r"proj SHADOW cam#(\d+): mapsize=([\d.]+) maxdist=([\d.]+) "
                    r"factors=\[([\d. ]+)\] thresholds=\[([-\d. ]+)\] "
                    r"csm0-scale=([\d.eE+-]+)", text)
                span = re.findall(
                    r"object-block member 3 \|row0\|=[\d.eE+-]+ -> ([\d.]+) m, "
                    r"([\d.]+) texels, ([\d.]+) m per texel", text)
                if sh:
                    sizes = {s[1] for s in sh}
                    scales = sorted({float(s[5]) for s in sh})
                    print(f"shadow  mapsize {'/'.join(sorted(sizes))}, "
                          f"maxdist {sh[0][2]}, factors [{sh[0][3]}]")
                    print(f"        csm0 scale (NDC per metre): "
                          f"{', '.join(f'{v:.6g}' for v in scales[:6])}")
                    # ---- what actually answers #56: V_csmthresholds --------
                    #
                    # Camera block float 232, live, and X4's own
                    # common_frag.glsl:1071 does `if (depth < V_csmthresholds[i])`
                    # -- so these are the cascade split distances in view metres,
                    # read from X4 rather than derived by us.
                    #
                    # This is the quantity fifty-odd takes should have been
                    # comparing. `csm0-scale` cannot answer it: member 3 is
                    # per-object and folds in the draw's own source frame.
                    REF = {"[5.2 23.9 195.1 1338.7]": "1.4917",
                           "[4.7 21.1 173.6 1189.6]": "1.75"}
                    FIXED = "[7.2 33.3 269.9 1856.4]"
                    tsets = sorted({f"[{s[4].strip()}]" for s in sh}
                                   - {"[0.0 0.0 0.0 0.0]"})
                    if tsets:
                        print(f"        V_csmthresholds (cascade splits, view "
                              f"metres): {'  '.join(tsets)}")
                        fov = re.search(r"X4VR_FOV=([\d.]+)", run or "")
                        for ts in tsets:
                            if ts == FIXED:
                                print(f"        {ts} is the fov-INDEPENDENT "
                                      f"set — identical at 1.4917 and 1.75, so "
                                      f"it is not evidence either way")
                            elif ts in REF:
                                same = fov and fov.group(1) == REF[ts]
                                print(f"        {ts} is the stored set for "
                                      f"X4VR_FOV={REF[ts]}"
                                      + ("  ✓ matches this run"
                                         if same else
                                         f"  ⚠ but this run is fov="
                                         f"{fov.group(1) if fov else '?'}"))
                            else:
                                print(f"        {ts} is a NEW set — no stored "
                                      f"take has it. Worth recording: the "
                                      f"splits are otherwise reproducible to "
                                      f"the printed precision")
                    if span:
                        m, t, mpt = span[0]
                        print(f"        object-block member 3: {m} m, {t} "
                              f"texels, {mpt} m per texel — provenance only, "
                              f"NOT a cascade (see objblock below)")
                        # Take 200 corrected this prompt. It used to say "the
                        # SAME line from a run at a different X4VR_FOV", and a
                        # PAIR CANNOT ANSWER IT: takes 197 and 199 have the
                        # same X4VR_FOV=1.4917 and report 0.9 m and 1.2 m. So
                        # 197 (0.9) against 200 (1.2, fov 1.75) would have
                        # read as "the span grows with the field" when the
                        # same difference appears at a FIXED field.
                        #
                        # 199 was run to test the seam at yaw=0 and happens to
                        # be the fixed-FOV control #56 needed. Without it this
                        # summary would have answered a fifty-take question
                        # wrongly, which is why the requirement is now stated
                        # as three runs and as SETS.
                        # Take 201 found this reading the wrong member and
                        # the capture that followed fixed it: the layer now
                        # takes M_shadowCSM0 from member 3 of the OBJECT block
                        # (set 3, 189 samplers) instead of M_shadowCSM0Clip
                        # from camera member 9 (370 declare, 0 sample).
                        #
                        # And this message asked for three runs the stored
                        # logs already contained. Sweeping /tmp for the line a
                        # proposed run would produce is the standing rule and
                        # it was skipped again.
                    print("        #56 is ANSWERED — no run needed, and this "
                          "section no longer asks for one. X4's splits DO "
                          "respond to <fov>: identical across 11 stored takes "
                          "at fov 1.4917 and different at 1.75, with takes 200 "
                          "and 202 differing in nothing else. They move NEARER "
                          "as the field widens (x0.888 for a x1.173 widening) "
                          "while mapsize/maxdist/factors do not move at all — "
                          "so the far cascades stretch and the guard band's "
                          "shadow price lands on the far field. Direction "
                          "established, LAW not: two points cannot fit one, "
                          "and the ratio matches neither tan nor 1/sx.")
                    if not tsets:
                        # Not "cascade extent NOT measured — M_shadowCSM0Clip
                        # read back as zeros", which is what stood here: the
                        # layer stopped reading that member at take 202 and the
                        # sentence outlived it by one commit.
                        print("        no nonzero V_csmthresholds in this log "
                              "— every shadow report carried [0 0 0 0], so "
                              "this run says nothing about #56")
                else:
                    # Same rule as the `view` line above: silence must not be
                    # readable as "the shadows are fine". A log without the
                    # instrument says nothing about them.
                    print("shadow  no 'proj SHADOW' line — the cascade extent "
                          "was NOT read this run (log predates #56, or the "
                          "decode never ran). #56 is unanswered by this log, "
                          "not answered in the negative")

                if asked is not None and eye is None:
                    # A FAIL, not a warn. #33's ceiling experiment asks exactly
                    # one question -- will X4 accept a <fov> wider than anything
                    # we have run -- and a scorer that prints a warning and then
                    # says PASS answers "no" in the voice of "yes". Worse, this
                    # block goes on to recommend the log to known-good-runs.md.
                    # Verified against a spliced control (take 105 with its tag
                    # rewritten to 2.2100, a field none of its cameras read):
                    # before this change it scored PASS with the warning above.
                    #
                    # The message carries the nearest tag actually seen, because
                    # a bare "not honoured" cannot separate the two causes the
                    # old text named. X4 rejecting the tag leaves its own default
                    # or the player's profile value, which is far away; the
                    # sx->degree law bending outside the 1.111..1.500 range it
                    # was calibrated over leaves something near the ask but off
                    # it. Those need different responses, so the scorer must not
                    # make the reader guess which happened.
                    near_tag, near_sx = min(
                        ((2 * math.degrees(math.atan(1.0 / v)) / FOV_BASE_DEG, v)
                         for v, _ in order if v),
                        key=lambda p: abs(p[0] - asked), default=(None, None))
                    # Say what was OBSERVED. The first version of this message
                    # ended "X4 fell back to a different field, so it rejected
                    # the tag" -- a cause the scorer cannot see -- and take 155
                    # is what that cost. It reported a rejection; Patola had
                    # watched a 163 degree field fill the cockpit, and
                    # registering his two screenshots put the rendered fov at
                    # 2.209 against 2.21 asked. X4 had honoured it exactly.
                    #
                    # What actually happened is in the layer, not in X4: it
                    # credits only the most-drawn block per frame, so a camera
                    # can render the whole scene and never be sampled. Take 156
                    # measured that directly -- at fov 1.4917 the honouring
                    # camera won 5 of 42 samples, at 2.21 it won 0 -- while
                    # both runs rendered the field asked for. Absence from the
                    # credit list is therefore evidence about the instrument
                    # and says nothing about X4's config parsing.
                    seen = ", ".join(
                        f"fov {2 * math.degrees(math.atan(1.0 / v)) / FOV_BASE_DEG:.3f}"
                        f" (sx={v:.5f}, x{n})" for v, n in order[:4] if v)
                    fails.append(
                        f"X4VR_FOV={asked:g} ({asked * FOV_BASE_DEG:.2f}°) was "
                        f"asked for and no CREDITED camera reads that field. "
                        f"Credited: {seen or 'nothing usable'}. This does NOT "
                        f"say X4 rejected the tag — the layer samples only the "
                        f"most-drawn block per frame and a wide field spreads "
                        f"draws across more blocks, which is exactly how take "
                        f"155 mis-read an honoured 2.21. Confirm against the "
                        f"rendered image before concluding anything about X4")

                # The aspect check, applied to ONE camera: the one that honours
                # the setting, since that is the one whose field the player is
                # looking through. If its sy does not track its sx, a widened
                # field is stretched rather than wider — a distortion no
                # screenshot reads reliably. Other cameras having other aspects
                # is X4's business, not a defect.
                if eye and eye[1]:
                    tgt = abs(sy / sx) if sx else 0.0
                    mr = re.search(r"X4VR_RES=(\d+)x(\d+)", run or "")
                    if mr:
                        tgt = int(mr.group(1)) / int(mr.group(2))
                    off = [r for r in eye[1] if abs(r - tgt) >= 0.01]
                    if not off:
                        print(f"proj  sy tracks sx on the fov camera — "
                              f"|sy/sx|={eye[1][0]:.4f} against an eye aspect "
                              f"of {tgt:.4f}, so the field is widened, not "
                              f"stretched")
                    else:
                        print(f"warn  the fov camera reads |sy/sx|="
                              f"{','.join(f'{r:.4f}' for r in off)} against an "
                              f"eye aspect of {tgt:.4f} — the vertical does not "
                              f"track the horizontal, so the field is being "
                              f"stretched")
                elif any(c["sy"] is not None for c in main):
                    print("proj  aspect per camera unjudged — no camera in this "
                          "run honoured X4VR_FOV, so there is no eye camera to "
                          "check")

                # Task #35 piece 1: the off-axis affine. Gated on the run
                # having asked for it, never on the outcome.
                #
                # The affine cannot be read off a screenshot as a number, so
                # what this section does is establish that it RAN and say what
                # it should have done, in the units the picture can be checked
                # against. A_x is the horizontal magnification and B_x the NDC
                # shift; A_x = 1 with B_x = 0 is the identity, which is the
                # negative control and must be indistinguishable from the same
                # run with the knob unset.
                m_oa = re.search(r"X4VR_OFFAXIS=\"?([-\d.,]+)\"?", run or "")
                # Piece 2: the target can also come from the runtime, with no
                # knob in the run line at all. Either way the layer names the
                # source it latched, and the declaration is built from the same
                # object -- so the scorer reports the source rather than
                # re-deriving what the picture should show from the env.
                src = re.search(r"offaxis: target from ([^—]+) —", text)
                # The piece-2 failure mode, and the ONLY line that marks it: a
                # VR run whose target should have come from the runtime and did
                # not. Keyed on the layer's own sentence rather than on
                # "X4VR_VR=1 and no offaxis line", which would fail every run
                # from a build that predates the feature.
                if "offaxis: OFF — X4VR_VR=1 but no view had been located" in text:
                    fails.append(
                        "X4VR_VR=1 and no view had been located when the first "
                        "shader that needed a target was patched, so the "
                        "off-axis target fell back to X4's symmetric field. "
                        "The picture is self-consistent — render and "
                        "declaration agree — but this run says nothing about "
                        "the runtime's frusta. Take 163 timed the margin at "
                        "0.8 s, so losing it means the runtime came up slower "
                        "than that here")
                # Task #39. The affine on gl_Position and its undoing in the
                # deferred reconstruction are ONE change in two places. With
                # the first done and the second not, 244 fragment modules
                # reconstruct a frame nothing is in, by opposite amounts in
                # the two eyes — which is what take 165b's per-eye menu
                # shadows were. Reachable through X4VR_PROJ_INVPROJ=0 and
                # through the bindless mirror being off, so it is checked
                # rather than assumed.
                # Keyed on the FINAL line. The first-present report legitimately
                # says "NOT undone" before any deferred module has been patched,
                # and matching anywhere in the log failed take 169b for a state
                # its own final line contradicted four thousand lines later.
                if "invproj final: off-axis affine NOT undone" in text:
                    fails.append(
                        "the off-axis affine is applied to gl_Position but "
                        "NOT undone in the deferred reconstruction — every "
                        "deferred pass reads M_invprojection for a frame the "
                        "affine has already moved, in opposite directions per "
                        "eye. Set X4VR_PROJ_INVPROJ=1 (and leave the bindless "
                        "mirror on), or turn the affine off entirely with "
                        "X4VR_OFFAXIS=off. Half-applied is worse than either")
                elif "invproj final: off-axis affine UNDONE" in text:
                    print("offaxis  deferred reconstruction corrected too — "
                          "T(d)·M_invprojection·A⁻¹, same latched target as "
                          "the vertex patches (task #39)")
                # Task #42's instrument, checked against the run's own
                # independent counters. The pipe inventory reports a per-module
                # patch path; the "patched vertex shader" summary counts the
                # same four paths at the same site. Two numbers for one fact,
                # and take 170 shipped with nothing comparing them: a missing
                # pair of braces stamped "clip" over every module and all 1007
                # pipeline lines agreed with each other while contradicting the
                # counter four lines away. An instrument that cannot be caught
                # lying is not evidence.
                pipes = re.findall(r"s\d+=mod#\d+/[0-9a-f]{8}\([^)]*?,"
                                   r"(live-sx|mvp-sx|baked-sx|clip)[,)]", text)
                cnt = re.findall(r"live-sx=(\d+) mvp-sx=(\d+) baked-sx=(\d+)",
                                 text)
                if pipes and cnt:
                    live, mvp, baked = (int(x) for x in cnt[-1])
                    seen = set(pipes)
                    # The counters say which paths the run actually used; the
                    # inventory must mention each of them at least once.
                    missing = [n for n, v in (("live-sx", live), ("mvp-sx", mvp),
                                              ("baked-sx", baked))
                               if v > 0 and n not in seen]
                    if missing:
                        fails.append(
                            f"the pipe inventory never reports "
                            f"{', '.join(missing)}, but the patch-site counters "
                            f"say live-sx={live} mvp-sx={mvp} baked-sx={baked}. "
                            f"The per-module path in every 'pipe:' line is "
                            f"WRONG — do not read it, and do not conclude "
                            f"anything about which draw got the affine from it")
                    else:
                        print(f"pipe     inventory paths {sorted(seen)} agree "
                              f"with the patch-site counters (live-sx={live} "
                              f"mvp-sx={mvp} baked-sx={baked})")

                # Task #48. The defect take 171 was read past: world geometry
                # positioned by the CAMERA (X4's instanced light volumes, and
                # p1_star -- the suns and bright stars) classifies NonWorld
                # under the narrow rule, so it takes K_nonworld (identity) and
                # no right-eye matrix. It then draws MONO and in X4's own
                # frustum inside a STEREO pass, while everything around it is
                # canted by the affine. That is the 30.07 deg divergence,
                # applied to a handful of objects instead of the whole frame,
                # and it is invisible to every counter that existed: baked-sx
                # is 0 in every run, and `clip` was not on the summary line.
                #
                # Two sources again, on purpose. cam-pos counts at the patch
                # site; the inventory names the passes. Either one alone can be
                # wrong -- together they have to agree.
                campos_cnt = re.search(r"clip=(\d+) cam-pos=(\d+)", text)
                stereo_hits, campos_passes = set(), set()
                for rp, sp, flags, rest in re.findall(
                        r"pipe: rp #(\d+|4294967295)\.(\d+) \[([^\]]*)\](.*)",
                        text):
                    for mm in re.finditer(
                            r"s\d+=mod#(\d+)/([0-9a-f]{8})\(([^)]*)\)->(\w+)",
                            rest):
                        if "camera-positioned" not in mm.group(3):
                            continue
                        campos_passes.add("rp #" + rp)
                        # "unsheared" passes substitute the original twin by
                        # design (shadow, fullscreen post) -- being uncanted
                        # there is correct, not a defect. Only a stereo pass
                        # makes it visible.
                        if "unsheared" not in flags and "+affine" not in \
                                mm.group(3):
                            stereo_hits.add((mm.group(2), "rp #" + rp))
                if stereo_hits:
                    where = sorted({p for _, p in stereo_hits})
                    fails.append(
                        f"{len(set(h for h, _ in stereo_hits))} camera-"
                        f"positioned module(s) draw in STEREO pass(es) "
                        f"{', '.join(where)} without the affine — that is "
                        f"world geometry left in X4's frustum, mono, while the "
                        f"rest of the pass is canted by 30.07°. p1_star (the "
                        f"suns and bright stars) is in this set. Re-run with "
                        f"X4VR_SHEAR_LIGHTS=1 to move them onto live-sx")
                elif campos_cnt and int(campos_cnt.group(2)) > 0 and \
                        not campos_passes:
                    fails.append(
                        f"the patch site counted cam-pos="
                        f"{campos_cnt.group(2)} camera-positioned module(s) "
                        f"but the pipe inventory tags none — the two readings "
                        f"disagree, so the inventory is not describing this "
                        f"run's dispatch. Do not read the kind field")
                elif campos_cnt:
                    print(f"pipe     clip={campos_cnt.group(1)} "
                          f"cam-pos={campos_cnt.group(2)}; no camera-positioned "
                          f"module draws uncanted in a stereo pass")

                # Task #40, the other half of the same idea. Screen-locked
                # draws have to be placed in the DECLARED frame or they sit at
                # the frustum centre in each eye, and the two centres are
                # mirrored. The layer names the state; this fails on it.
                if "off-axis affine NOT applied to screen-locked" in text:
                    fails.append(
                        "the declaration is canted and the UI is still in "
                        "X4's symmetric frame — nothing was drawn on the "
                        "canvas, so the HUD, the radar and the gauges sit at "
                        "the frustum centre in each eye and are asked to "
                        "diverge. Needs X4VR_STEREO=1 and a canvas pass; "
                        "X4VR_CANVAS_M=<metres> chooses a distance, and "
                        "without it the canvas is at infinity, which is "
                        "fusable")
                cvs = re.search(r"canvas: ([^,]+), CANTED — screen half-angle "
                                r"([\d.]+) deg \(sx=([\d.]+)\) -> A_x=([\d.]+) "
                                r"A_y=([\d.]+), eye0 x offset ([-+\d.]+) "
                                r"eye1 ([-+\d.]+)", text)
                if cvs:
                    where, halfd, sxs, cax, cay, e0, e1 = cvs.groups()
                    print(f"canvas   {where}, canted: screen half-angle "
                          f"{halfd}° (sx={sxs}) → A_x={cax} A_y={cay}, "
                          f"eye offsets {e0}/{e1} NDC")
                    # The identity property, checked rather than trusted: the
                    # screen's own field and the target's must give A_x = 1
                    # when they are the same field. A_x far from the log's own
                    # ax_num/sx would mean the canvas is being placed against
                    # a different field from the one the shader coefficients
                    # were built for -- which reads as "the UI is the wrong
                    # size" and not as a bug.
                    oaq = re.search(r"offaxis: eye0 ax_num=([-\d.]+)", text)
                    if oaq:
                        want = float(oaq.group(1)) / float(sxs)
                        if abs(want - float(cax)) > 5e-3:
                            fails.append(
                                f"the canvas's A_x is {cax} but the latched "
                                f"ax_num {oaq.group(1)} over the screen sx "
                                f"{sxs} is {want:.4f}. The UI is being placed "
                                f"against a different field from the one the "
                                f"shader coefficients were baked for")
                if src:
                    print(f"offaxis  target from {src.group(1).strip()}; the "
                          f"compositor is told the same frusta the shader "
                          f"coefficients were baked from")
                # **The check that would have failed take 165b before anyone
                # put the headset on.** The affine reaches World modules only.
                # Everything else -- every NonWorld module, the unsheared twin,
                # the loading screen -- stays in X4's symmetric frame, so it
                # sits at image centre in both eyes. (NOT the skybox: that was
                # measured after 165b and it carries the affine correctly. The
                # deferred reconstruction was on this list too until task #39
                # took it off; the check above is what proves it.)
                # Declared canted, image centre IS the frustum centre, and the
                # two centres are mirrored: the eyes are asked to diverge by
                # their separation. x4vr_view.hpp:291 records 30.07 deg for this
                # runtime's frusta as the unfusable negative control, and piece
                # 2 reproduced it for every draw the affine misses.
                #
                # Derived from the frusta the LOG carries, so it costs no run
                # and cannot be fooled by the env disagreeing with what latched.
                ang = re.search(
                    r"offaxis: target from [^\n]*eye0 l=([-\d.]+) r=([-\d.]+) "
                    r"u=([-\d.]+) d=([-\d.]+), eye1 l=([-\d.]+) r=([-\d.]+) ",
                    text)
                if ang and "X4VR_VR=1" in (run or ""):
                    l0, r0, _u, _d, l1, r1 = (float(g) for g in ang.groups())
                    def centre(a, b):
                        return math.degrees(math.atan(
                            (math.tan(math.radians(a)) +
                             math.tan(math.radians(b))) / 2.0))
                    c0, c1 = centre(l0, r0), centre(l1, r1)
                    div = abs(c1 - c0)
                    # Task #40 is what decides whether a canted declaration is
                    # a defect or a feature, so the check reads the canvas's
                    # own swap count rather than assuming. POSITIVE evidence is
                    # required: a build predating #40 prints no such line, and
                    # "no line" must read as "not covered", not as "fine".
                    swp = re.search(r"canvas final: \d+ variant\(s\) built, "
                                    r"\d+ REFUSED, swapped into (\d+) pipeline",
                                    text)
                    covered = bool(swp) and int(swp.group(1)) > 0
                    if div > 1.0 and not covered:
                        fails.append(
                            f"the declared frusta are canted {c0:+.2f}° and "
                            f"{c1:+.2f}°, and screen-locked content was NOT "
                            f"put on the canvas ({'0 swaps' if swp else 'no '
                            'canvas line at all'}) — so the HUD, the radar, "
                            f"the weapon gauges, the unsheared twin and the "
                            f"loading screen all sit at image centre in both "
                            f"eyes and are asked to DIVERGE by {div:.2f}°. "
                            f"Nobody fuses that; x4vr_view.hpp:291 already "
                            f"records it as the negative control. The world "
                            f"is correct and everything else is not")
                    elif div > 1.0:
                        print(f"offaxis  declared frusta centre {c0:+.2f}° / "
                              f"{c1:+.2f}° — canted by {div:.2f}°, and "
                              f"screen-locked draws are on the canvas "
                              f"({swp.group(1)} pipeline stage(s)), so they "
                              f"are placed in the declared frame too")
                    else:
                        print(f"offaxis  declared frusta centre {c0:+.2f}° / "
                              f"{c1:+.2f}° — screen-locked content diverges by "
                              f"{div:.2f}°, which is fusible")
                # The layer's own three verdicts, carried through verbatim
                # rather than re-derived. Each is computed from the LATCHED
                # frusta, which the scorer does not have; restating them from
                # the env would be a different calculation wearing the same
                # words.
                for pat, label in (
                        (r"offaxis: WARNING — ([^\n]*)", "warn  "),
                        (r"offaxis: (X4VR_FOV [\d.]+ covers[^\n]*)",
                         "offaxis  "),
                        (r"vr fov: WARNING — ([^\n]*)", "warn  ")):
                    m = re.search(pat, text)
                    if m:
                        print(label + m.group(1).strip()[:240])
                if m_oa or src:
                    ref = re.search(
                        r"offaxis: eye0 [^\n]*ax_num=([-\d.]+) bx=([-+\d.]+) "
                        r"ay_num=([-\d.]+) by=([-+\d.]+)", text)
                    refused = re.search(r"offaxis: REFUSED[^\n]*", text)
                    if not ref and not m_oa:
                        pass  # the runtime path already reported above
                    elif not ref:
                        # A knob that never ran cannot refute anything, and a
                        # run graded as if it had is how thirteen takes of
                        # nulls happened once already. The layer prints the
                        # precondition it was missing; carry it verbatim.
                        fails.append(
                            f"X4VR_OFFAXIS={m_oa.group(1)} was asked for and "
                            f"the affine never ran. "
                            + (refused.group(0).split("layer")[-1].strip()
                               if refused else
                               "No 'offaxis:' line at all — the layer never "
                               "reached shader-module creation with it set"))
                    else:
                        axn, bx, ayn, by = (float(g) for g in ref.groups())
                        # Against the camera that honours X4VR_FOV, because
                        # that is the one whose frustum the affine is remapping
                        # and the one the player looks through. A_x built from
                        # any other camera's sx is the take-50 mistake.
                        if eye:
                            # sy, not sx. The vertical coefficient divides by
                            # the camera's own sy, and X4's cameras are not all
                            # square -- the map camera reads |sy/sx| = 1.5, and
                            # dividing A_y by sx there would report a vertical
                            # magnification 50% off. sy is negative in every
                            # sample this project has taken, which is what makes
                            # A_y come out positive.
                            asp = eye[1][0] if eye[1] else 1.0
                            ax, ay = axn / eye[0], ayn / -(asp * eye[0])
                            ident = (abs(ax - 1) < 5e-3 and abs(bx) < 5e-3 and
                                     abs(ay - 1) < 5e-3 and abs(by) < 5e-3)
                            print(f"offaxis  eye0 A_x={ax:.4f} B_x={bx:+.4f}, "
                                  f"A_y={ay:.4f} B_y={by:+.4f} against the fov "
                                  f"camera (sx={eye[0]:.5f}, |sy/sx|={asp:.4f})")
                            if ident:
                                print("offaxis  this run is the IDENTITY "
                                      "control — the picture must be "
                                      "indistinguishable from the same run "
                                      "with X4VR_OFFAXIS unset; any visible "
                                      "difference is a defect in the emission")
                            else:
                                print(f"offaxis  the world should be "
                                      f"magnified {ax:.3f}x horizontally and "
                                      f"{ay:.3f}x vertically, and shifted "
                                      f"{bx * 50:+.1f}% / {by * 50:+.1f}% of "
                                      f"the image width and height; an "
                                      f"unchanged picture means the affine "
                                      f"reached no draw")
                        else:
                            print(f"offaxis  ran (ax_num={axn:.5f} "
                                  f"bx={bx:+.5f}) but A_x is unjudged — no "
                                  f"camera in this run honoured X4VR_FOV, so "
                                  f"there is no sx to divide by")
                        # The modules left behind. The affine rides on the two
                        # patches that read sx live; a world module that fell
                        # through to the baked matrix got the shear and NOT the
                        # affine, so it is still drawing X4's frustum while
                        # everything around it draws the runtime's.
                        nb = re.findall(r"baked-sx=(\d+)", text)
                        if nb and nb[-1] != "0":
                            print(f"warn  {nb[-1]} world module(s) took the "
                                  f"baked matrix, so they carry the shear "
                                  f"without the affine and render in X4's "
                                  f"frustum — expect misplaced geometry from "
                                  f"exactly those draws")
                        elif nb:
                            print("offaxis  every world module reads sx live, "
                                  "so every one of them got the affine")

                # Task #25. The stereo scale this run actually produced, from
                # its own numbers rather than from the arithmetic in the docs,
                # which was written at sx=1.3333 and is 1.8x off at fov 1.437.
                #
                # make_eye_shear puts NDC x' = x - sx*(ipd/2)/z per eye, so the
                # per-eye pixel offset is (W/2)*sx*(ipd/2)/z and the ANGLE
                # between the two eyes' images of a point is ipd/z, with sx
                # cancelling. That cancellation is the reassuring part: a wider
                # field changes how many pixels a given depth separation
                # occupies, not the vergence it asks the eyes for. It only stops
                # holding if the rendered field and the displayed field differ,
                # which is #31's question, not this one.
                #
                # Reported against the camera that honours the setting, or not
                # at all: cam#0 is whichever camera drew first, and pricing
                # stereo off the wrong camera is what put 1.3333 in the launcher
                # for fifty takes.
                m_ipd = re.search(r"X4VR_IPD=([\d.]+)", run or "")
                mr = re.search(r"X4VR_RES=(\d+)x(\d+)", run or "")
                if eye and m_ipd and mr:
                    ipd, w = float(m_ipd.group(1)), int(mr.group(1))
                    ssx = eye[0]
                    px = [(z, (w / 2) * ssx * (ipd / 2) / z)
                          for z in (1.0, 5.0, 10.0, 30.0)]
                    print(f"stereo  ipd={ipd:.3f} m on the fov camera "
                          f"(sx={ssx:.5f}, {w}px eye): per-eye offset "
                          + ", ".join(f"{v:.1f}px at {z:g}m" for z, v in px))
                    print(f"stereo  total disparity passes 1° at "
                          f"{ipd / math.radians(1.0):.2f} m and 2° at "
                          f"{ipd / math.radians(2.0):.2f} m — closer than that "
                          f"is a vergence load, and it is FOV-independent")
                    m_baked = re.search(r"X4VR_PROJ_SX=([\d.]+)", run or "")
                    m_cnt = re.findall(r"baked-sx=(\d+)", text)
                    if m_baked:
                        r = float(m_baked.group(1)) / ssx
                        n = f"{m_cnt[-1]} module(s)" if m_cnt else "the modules"
                        # Take 110 drove this count to zero, and the
                        # sentence then read "the 0 module(s) ... matches the
                        # scene camera", which is true and unreadable. A count
                        # of zero is a different statement: the constant is not
                        # consulted by anything at all.
                        if m_cnt and m_cnt[-1] == "0":
                            print(f"stereo  X4VR_PROJ_SX={m_baked.group(1)} is "
                                  f"unused — no module fell back to it, every "
                                  f"one reads sx per draw (task #23)")
                        else:
                            verdict = ("matches the scene camera"
                                       if abs(r - 1) < 0.02
                                       else f"separate {r:.2f}x too "
                                            f"{'much' if r > 1 else 'little'}")
                            print(f"stereo  X4VR_PROJ_SX={m_baked.group(1)} "
                                  f"against a scene sx of {ssx:.5f}: the {n} "
                                  f"that cannot read sx live {verdict} "
                                  f"(task #23)")
                if other:
                    nears = sorted({f"{c['near']:.3f}" for c in other})
                    shown_n = nears[:6]
                    tail = (f", +{len(nears) - len(shown_n)} more"
                            if len(nears) > len(shown_n) else "")
                    # len(nears) counts distinct near *values*, not blocks: one
                    # block whose near drifts contributes a hundred of them.
                    print(f"note  {len(other)} change(s) carried a different "
                          f"near, {len(nears)} distinct value(s) "
                          f"({', '.join(shown_n)}{tail}), and are excluded "
                          f"above — not the main camera (see take 54)")

                # The layer stops logging changes once its budget is spent, and
                # take 105 spent all 400 slots 181 s into a 382 s session on a
                # degenerate block whose near drifted by 1e-3 a sample. Nothing
                # in the score said so, and a prediction about a camera that had
                # not appeared yet was written up as untested when in truth the
                # instrument had gone dark. A cap that does not report itself is
                # indistinguishable from "nothing more happened".
                # Per-camera caps are the budget working, not failing: take 108
                # circled a station and two drifting slots hit 120 each while
                # every other camera kept logging. That is exactly the condition
                # that blinded take 105 under a single global budget. Reported
                # anyway -- a cap nobody mentions is how take 105 read as "the
                # camera did not appear".
                percam = re.findall(r"proj: cam#(\d+) reached \d+ changes", text)
                if percam:
                    print(f"note  {len(percam)} slot(s) hit the per-camera "
                          f"change cap (cam#{', cam#'.join(percam[:4])}"
                          f"{', …' if len(percam) > 4 else ''}) — contained to "
                          f"those slots, the rest kept logging")

                cap = re.search(
                    r"\[\s*([\d.]+)\]\s+layer\s+proj: (?:\d+ changes logged"
                    r"(?:,| across))", text)
                if cap:
                    stamps = re.findall(r"^\[\s*([\d.]+)\]", text, re.M)
                    if stamps:
                        t0, t1 = float(stamps[0]), float(stamps[-1])
                        blind = t1 - float(cap.group(1))
                        print(f"warn  the layer's change budget ran out "
                              f"{float(cap.group(1)) - t0:.0f} s into a "
                              f"{t1 - t0:.0f} s session — the last {blind:.0f} s "
                              f"produced no proj samples at all, so anything "
                              f"absent from the set above may simply not have "
                              f"been looked at")
            else:
                print("proj  sx never changed during this run (no zoom, or the "
                      "camera never moved through one)")

    # VR, and only when the run asked for it (task #34).
    #
    # Gated on intent, not on outcome: what makes this section apply is
    # X4VR_VR=1 on the command line, never whether a session happened to come
    # up. A run that asked for VR and got none is exactly the run this has to
    # fail, so "no session" must not be allowed to read as "not a VR run".
    asked_vr = bool(run and re.search(r"\bX4VR_VR=[^0\s]", run)) or \
        any("vr: X4VR_VR=1" in ln for ln in lines)
    if asked_vr:
        summary = None
        for ln in lines:
            m = re.search(r"vr summary \((\w+)\): runtime=(.*?) session=(\d+) "
                          r"focused=(\d+) frames=(\d+) located=(\d+) "
                          r"submitted=(\d+) span=([\d.]+),([\d.]+),([\d.]+)", ln)
            if m:
                summary = m
        if not summary:
            # Distinguish the two ways nothing can be here, because they need
            # different fixes and the same silence.
            if any("vr:" in ln for ln in lines):
                fails.append("X4VR_VR=1 but the layer printed no vr summary — "
                             "the run died before vkDestroyDevice")
            else:
                fails.append("X4VR_VR=1 but the layer said nothing about VR at "
                             "all — built without OpenXR headers? cmake -S . "
                             "-B build should print 'OpenXR headers found'")
            print("vr  asked for, and absent")
        else:
            (_when, rt_name, session, focused, frames, located, submitted,
             sx_, sy_, sz_) = summary.groups()
            session, focused = int(session), int(focused)
            frames, located = int(frames), int(located)
            submitted = int(submitted)
            span = (float(sx_), float(sy_), float(sz_))
            # **Did the run get the runtime it ASKED for?**
            #
            # XR_RUNTIME_JSON is how a take selects one, and a value that never
            # reaches the process leaves the default in place — silently, and
            # with every number below still looking healthy. Take 296 spent a
            # whole run that way on a different variable.
            _rj = re.search(r"XR_RUNTIME_JSON=(\S+)", text)
            if _rj:
                _want = _rj.group(1).lower()
                _got = (rt_name or "").lower()
                _key = ("steam" if "steamxr" in _want or "steamvr" in _want
                        else "wivrn" if "wivrn" in _want
                        else "monado" if "monado" in _want else None)
                if _key and _key not in _got:
                    print(f"vr  !! RUNTIME MISMATCH — the run asked for "
                          f"{_rj.group(1)} but the session came up on "
                          f"\"{rt_name}\". The variable did not reach the "
                          f"process, so this run is NOT the A/B it was "
                          f"specified as. Nothing below speaks to the runtime "
                          f"question.")
                elif _key:
                    print(f"vr  runtime override honoured — asked for {_key}, "
                          f"got \"{rt_name}\"")
            print(f"vr  runtime \"{rt_name}\" session={session} "
                  f"focused={focused} frames={frames} located={located} "
                  f"submitted={submitted}")
            if not session:
                why = next((ln.split("NO SESSION THIS RUN — ", 1)[1]
                            for ln in lines if "NO SESSION THIS RUN" in ln),
                           "no reason logged")
                fails.append(f"X4VR_VR=1 but no session: {why}")
            else:
                if not focused:
                    fails.append(
                        "the session never reached FOCUSED — the runtime never "
                        "gave X4 the foreground. Headset asleep, or another "
                        "application holds the session")
                # Task #38. Only judged once the layer is capable of it, which
                # the "vr copy" line is the evidence for: takes 112/113 ran a
                # deliberately zero-layer session and must keep their verdicts.
                copy = None
                copies = []   # (t, blits) per periodic line, for the rate
                for ln in lines:
                    m2 = re.search(r"vr copy \(\w+\): swapchain=(\d+) "
                                   r"blits=(\d+) released=(\d+) "
                                   r"acquire_failed=(\d+) refused=(\d+)", ln)
                    if m2:
                        copy = m2
                        mt = re.match(r"\[\s*([\d.]+)\]", ln)
                        if mt:
                            copies.append((float(mt.group(1)),
                                           int(m2.group(2))))
                if copy:
                    sc_ok, blits, released, afail, refused = (
                        int(g) for g in copy.groups())
                    print(f"    copy swapchain={sc_ok} blits={blits} "
                          f"released={released} acquire_failed={afail} "
                          f"refused={refused}")
                    # How many of the headset's frames were a REPEAT.
                    #
                    # blits is new content; the display period is the rate the
                    # headset consumes it at. Takes 303a-d were scored by
                    # dividing blits by a panel rate taken from a REPORTED
                    # headset setting, and dividing by our own submit rate
                    # instead reordered the four runs -- so the denominator
                    # was doing the arguing. This one comes from the runtime.
                    #
                    # Silent when the line is absent: every log before take
                    # 304 predates the instrument, and a missing measurement
                    # must not read as a fast display. That is the same trap
                    # as reporting 0.00 disparity for a failed search.
                    pace = None
                    for ln in lines:
                        m3 = re.search(r"vr pace \(\w+\): display period "
                                       r"([\d.]+) ms \(([\d.]+) Hz\), "
                                       r"changed (\d+) time", ln)
                        if m3:
                            pace = m3
                    if pace is None:
                        print("    pace  not measured — this log predates the "
                              "display-period instrument, so the repeat "
                              "fraction below cannot be computed and any "
                              "panel rate quoted for it was assumed")
                    else:
                        p_ms, p_hz, chg = (float(pace.group(1)),
                                           float(pace.group(2)),
                                           int(pace.group(3)))
                        if p_ms <= 0.0:
                            print("    pace  the runtime never supplied a "
                                  "display period — not an error, but the "
                                  "repeat fraction is unavailable")
                        else:
                            print(f"    pace  display {p_hz:.1f} Hz "
                                  f"({p_ms:.2f} ms)"
                                  + (f", CHANGED {chg}x mid-session — the "
                                     f"denominator moved, so any single "
                                     f"repeat fraction below is an average "
                                     f"over two regimes" if chg else ""))
                        # **Totals, and totals were WRONG on take 304a.**
                        #
                        # This printed "14.8% of headset frames showed a
                        # REPEAT" for a run whose gameplay never repeated a
                        # frame at all: the loading phase blits slowly and
                        # drags a session total down. The per-window series
                        # below says 59.8 / 59.4 / 59.8 / 82.5 / 115.6 Hz --
                        # one number cannot describe that, and the one this
                        # chose described none of it.
                        #
                        # The controls in tests/run-pace-scorer.sh did not
                        # catch it because all nine planted cases are totals;
                        # they tested the arithmetic and never the WINDOW.
                        # Kept below for the totals it is still good for, but
                        # the starved fraction is the number to read.
                        blit_win = []
                        for a, b in zip(copies, copies[1:]):
                            dt = b[0] - a[0]
                            if dt > 0:
                                blit_win.append((b[1] - a[1]) / dt)
                        blit_win = blit_win[2:]  # before the swapchain exists
                        if blit_win and p_ms > 0.0:
                            starved = [v for v in blit_win if v < p_hz]
                            s = sorted(blit_win)
                            deficit = sum(max(0.0, p_hz - v)
                                          for v in blit_win) / len(blit_win)
                            print(f"    pace  new content {s[len(s) // 2]:.1f} "
                                  f"Hz median, p10 {s[len(s) // 10]:.1f}, over "
                                  f"{len(blit_win)} window(s) — NOT a total, "
                                  f"the totals below mix loading with flight")
                            print(f"    pace  STARVED in "
                                  f"{100 * len(starved) / len(blit_win):.0f}% "
                                  f"of windows, {deficit:.1f} Hz of new "
                                  f"content missing run-wide")
                            if len(starved) > 0.5 * len(blit_win):
                                print("    pace  => the display is asking for "
                                      "frames we are not producing for most "
                                      "of the run. Measured on WiVRn this "
                                      "reads as ghosting and on SteamVR it "
                                      "mostly does not, so the deficit is "
                                      "the trigger and the runtime's handling "
                                      "of it is the severity — do not read a "
                                      "clean SteamVR run as an absent deficit")
                        if submitted > 0 and blits <= submitted:
                            rep = 1.0 - blits / submitted
                            print(f"    pace  {100 * rep:.1f}% of headset "
                                  f"frames showed a REPEAT ({blits} new of "
                                  f"{submitted} submitted)")
                            # Reprojection corrects a repeat for head
                            # ROTATION and not for scene motion, so the cost
                            # lands exactly where Patola reported it: fast
                            # relative motion, close to a station.
                            if blit_win:
                                # The window series above already judged this
                                # run and is the better statistic. Two verdicts
                                # on one page, one of them from a number that
                                # mixes loading with flight, is how take 304a
                                # got "not enough to explain a doubling
                                # complaint" printed under a 71%-starved run.
                                pass
                            elif rep >= 0.20:
                                print("    pace  => the headset is showing "
                                      "one stale frame in five or worse. "
                                      "Reprojection hides that for head "
                                      "rotation but NOT for scene motion, so "
                                      "it reads as doubling when something "
                                      "moves fast across the view. Raise the "
                                      "new-content rate (X4VR_RES) before "
                                      "reading anything else here as a "
                                      "rendering defect")
                            elif rep >= 0.05:
                                print("    pace  => a repeat rate this low "
                                      "is not enough to explain a doubling "
                                      "complaint on its own")
                            else:
                                print("    pace  => essentially every "
                                      "headset frame carried new content; "
                                      "the frame deficit is NOT available as "
                                      "an explanation for this run")
                        elif blits > submitted:
                            print(f"    pace  blits ({blits}) EXCEEDS "
                                  f"submitted ({submitted}) — X4 produced "
                                  f"more frames than the headset consumed, "
                                  f"so some of X4's work was dropped rather "
                                  f"than shown; the repeat fraction is zero "
                                  f"and the waste is on our side")
                    if not sc_ok:
                        why = next((ln.split("NO SUBMISSION THIS RUN — ", 1)[1]
                                    for ln in lines
                                    if "NO SUBMISSION THIS RUN" in ln),
                                   "no reason logged")
                        fails.append(f"no XR swapchain: {why}")
                    elif blits == 0:
                        fails.append(
                            "the swapchain exists but X4's eye image was never "
                            "copied into it — the headset saw nothing X4 drew")
                    elif submitted == 0:
                        fails.append(
                            f"{blits} eye frame(s) were copied but no XR frame "
                            f"ever carried a layer — the copy and the submit "
                            f"are not meeting")
                    if refused:
                        fails.append(
                            f"{refused} blit(s) refused on a shape mismatch — "
                            f"the swapchain and the eye image disagree, see "
                            f"the 'vr: NOT blitting' line")
                    # Not a failure: X4 presents slower than the headset runs,
                    # so blits < submitted is the normal steady state and the
                    # runtime reprojects the rest. Only worth saying when it is
                    # extreme enough to mean stutter rather than reprojection.
                    if blits and submitted and submitted / blits > 6:
                        print(f"    the headset ran {submitted / blits:.1f}x "
                              f"faster than X4 presented — every X4 frame was "
                              f"shown ~{submitted / blits:.0f} times")
                if frames == 0:
                    fails.append("the session was created but no XR frame ever "
                                 "completed — the frame loop is not running")
                elif located / frames < 0.9:
                    fails.append(
                        f"xrLocateViews returned a pose for only {located} of "
                        f"{frames} frames ({100 * located / frames:.0f}%) — "
                        f"tracking was dropping out")
                # Informational, deliberately. A run where Patola sat still is
                # a legitimate run, and failing it would teach the wrong habit
                # -- but a span of exactly zero across a long run means the
                # pose is a constant, which is the failure #33 exists to avoid.
                if located and max(span) < 1e-4:
                    print("    span is zero on every axis — either nobody "
                          "moved, or the pose is a constant. Worth a second "
                          "run with deliberate head movement before trusting "
                          "it.")
                else:
                    print(f"    head span {span[0]:.3f} x {span[1]:.3f} y "
                          f"{span[2]:.3f} z m — the pose moves")
            # This step submits nothing on purpose. If that ever changes, the
            # expectation here has to change with it rather than quietly pass.
            if submitted:
                print(f"    {submitted} layer(s) submitted — this is past the "
                      f"'submit nothing' step, so check the headset actually "
                      f"showed X4")

    # Frame time, for A/B runs that park the camera.
    #
    # Reported as one comparable number rather than a median over the session,
    # because a median over the session compares two different scenes: takes
    # 100, 104 and 105 read 6.91, 8.01 and 12.12 ms at fov 1.111, 1.5 and 1.437
    # -- not monotone in fov, because 105 was mostly a station interior. So the
    # number printed is the quietest stretch of at least 55 s, which is what the
    # parked half of an A/B produces, and the spread is printed with it so a
    # stretch that was not actually quiet is visible as such.
    perf = [(float(m.group(1)), float(m.group(2)))
            for m in re.finditer(
                r"^\[\s*([\d.]+)\]\s+layer\s+perf frame \d+: median "
                r"([\d.]+) ms", text, re.M)]
    if perf:
        meds = sorted(p[1] for p in perf)
        print(f"perf  {meds[len(meds) // 2]:.2f} ms median over {len(perf)} "
              f"window(s) — session-wide, so it compares scenes, not settings. "
              f"By phase:")
        # Phases, not a "quietest stretch". The quietest-stretch selector was
        # written for take 106 and it picked the same window in every run --
        # takes 100, 104, 105, 106 and 107 all reported ~17.3 ms, because the
        # calmest minute of an X4 session is the loading and menu phase before
        # gameplay starts, not the parked camera at the end. Five unrelated runs
        # agreeing to 0.1 ms is not a stable measurement, it is a metric that
        # does not discriminate. Segmenting instead makes the shape visible --
        # X4 runs ~2 ms at the splash, ~17-20 ms loading, ~7 ms in flight -- and
        # a parked A/B compares the LAST phase of each run, which is the one the
        # protocol actually parks in.
        phases, cur = [], [perf[0]]
        for p in perf[1:]:
            med = sorted(x[1] for x in cur)[len(cur) // 2]
            if med and abs(p[1] - med) / med > 0.25:
                phases.append(cur)
                cur = [p]
            else:
                cur.append(p)
        phases.append(cur)
        t0 = perf[0][0]
        for k, ph in enumerate(phases):
            w = sorted(x[1] for x in ph)
            end = (phases[k + 1][0][0] if k + 1 < len(phases) else ph[-1][0])
            # Only worth pointing at if it could plausibly BE a parked minute:
            # takes 100 and 105 end on a single window, and labelling that "the
            # number to compare" is how a 5.65 ms sample of nothing becomes a
            # result.
            # No "compare this one" tag any more. It used to mark the last
            # phase, and that label is what made the circularity actionable:
            # see the draws-segmented block below for why the last phase is
            # not reliably the parked one.
            print(f"      {ph[0][0] - t0:5.0f}–{end - t0:<5.0f}s "
                  f"{w[len(w) // 2]:6.2f} ms  ({len(ph)} window(s), "
                  f"{w[0]:.2f}–{w[-1]:.2f})")

        # --- the comparable number, cut on X4's own draw count -------------
        #
        # The phases above are shape. They are not the measurement, because
        # grouping samples BY frame time and then reporting frame time is
        # circular -- and that cost two misreadings in one session.
        #
        # Takes 158/159/160 all contain a 17-19 ms stretch that is X4's
        # LOADING SCREEN: ~12 draws across ~3 slots, for about 70 s. Frame
        # time *drops* to 7 ms at the instant the world appears (~97 draws,
        # ~40 slots) -- more geometry, faster frames. A frame-time cluster
        # cannot find that boundary, because the boundary is not where the
        # frame time is calm, it is where the draw count steps. Take 160's
        # loading and gameplay landed in one 17.74 ms phase and buried a
        # 20.75 ms result; the 1x/4x pair before it was compared on a window
        # nobody had checked was the same state in both runs.
        #
        # So: segment on what X4 reports it is drawing, never on the
        # quantity being measured.
        draws = [(float(m.group(1)), int(m.group(2)))
                 for m in re.finditer(
                     r"^\[\s*([\d.]+)\]\s+layer\s+frame \d+ (?:NEW|hb):[^\n]*"
                     r"draws=(\d+)", text, re.M)]
        WORLD = 50  # measured: ~12 while loading, ~97-117 in flight
        busy = [t for t, d in draws if d > WORLD]
        if busy:
            # The LAST contiguous stretch, so a flicker of geometry during
            # loading cannot open the window early.
            start = busy[0]
            for a, b in zip(busy, busy[1:]):
                if b - a > 20.0:
                    start = b
            g = sorted(v for t, v in perf if t >= start)
            lo = [d for t, d in draws if t < start]
            hi = [d for t, d in draws if t >= start]
            if g:
                span = max(t for t, _ in perf if t >= start) - start
                print(f"perf  GAMEPLAY {g[len(g) // 2]:.2f} ms "
                      f"({1000 / g[len(g) // 2]:.0f} fps) — {len(g)} window(s) "
                      f"over {span:.0f}s from t0+{start - t0:.0f}s, range "
                      f"{g[0]:.2f}–{g[-1]:.2f}. This is the A/B number.")
                if lo and hi:
                    lo.sort(); hi.sort()
                    a, b = lo[len(lo) // 2], hi[len(hi) // 2]
                    # Only claim a clean split when there is one. "before" is
                    # the whole pre-gameplay session including the splash,
                    # which itself draws geometry, so the two medians are not
                    # always far apart -- and asserting a split that the
                    # numbers do not show is the same over-claim this file
                    # already had to have removed once.
                    print(f"      segmented on draws: {a} before vs {b} after"
                          + (" — a clean step" if b > 2 * a else
                             " — NOT a clean step, check the window by hand"))
                if len(g) < 5 or span < 30.0:
                    print("warn  the parked stretch is too short to compare. "
                          "X4 needs ~70 s to load this save; park in the world "
                          "for 60 s AFTER it appears, or this number is a "
                          "sample of whatever happened to be on screen.")
            else:
                print("perf  no frame-time window fell inside the gameplay "
                      "stretch — the run ended too soon after the world "
                      "appeared to measure anything")
        elif draws:
            print(f"perf  GAMEPLAY not reached — draws never exceeded {WORLD}, "
                  f"so this run never left the loading screen or the menu")

    # --- did the shear-ui probe actually run on anything? -------------------
    #
    # Take 152 set X4VR_SHEAR_UI=1, looked identical, and would have been read
    # as "shearing the UI does not fix the HUD". It was a no-op: the pass was
    # reclassified but the nonworld modules still carried an identity matrix,
    # so the patch counts came out byte-identical to the run without it
    # (nonworld=56 stereo=294 in both, stereo == the world count exactly).
    #
    # A probe that cannot be shown to have run cannot refute anything, so this
    # refuses to let the next one be graded by eye.
    if "X4VR_SHEAR_UI=1" in (run or ""):
        patched = [ln for ln in lines if "patched vertex shader #" in ln]
        m = re.search(r"world=(\d+) nonworld=(\d+) stereo=(\d+)",
                      patched[-1]) if patched else None
        carried = any("shear-ui probe:" in ln for ln in lines)
        if not m:
            print("shear-ui  no patch counts in this log")
        else:
            w, nw, st = (int(g) for g in m.groups())
            print(f"shear-ui  world={w} nonworld={nw} stereo={st}, nonworld "
                  f"matrices {'set' if carried else 'NOT set'}")
            if not carried or st <= w:
                fails.append(
                    f"X4VR_SHEAR_UI was on but the probe did not reach the "
                    f"draws: stereo={st} against world={w}, so the {nw} "
                    f"nonworld modules got no per-eye variant"
                    f"{'' if carried else ' and the matrices were never copied'}"
                    f". This run cannot say anything about the HUD — it is a "
                    f"repeat of take 152, not a result")

    # --- the pose we declare to the runtime ---------------------------------
    #
    # A silent fallback to identity is indistinguishable from working: X4 keeps
    # re-rendering from the driven camera, so the world still changes as the
    # head turns, and only a screen-locked element -- the HUD -- reveals that
    # the image is pinned in space. That is the shape Patola reported after take
    # 149, and it took two hypotheses and no instrument to notice.
    pose = [ln for ln in lines if "vr pose:" in ln]
    if not pose:
        # Name the SUBJECT. This said "not reported — this build predates the
        # instrument", which reads as "the pose channel is absent" and is how
        # take 186b's FAIL got past a reader. A flatscreen take has no runtime
        # to declare a pose to, so silence here is expected, not a gap.
        print("pose  no pose DECLARED TO THE RUNTIME in this log — expected "
              "for a flatscreen take. Says nothing about #59's channel, "
              "which reports separately as `pose-channel`")
    else:
        # **Identity before head-look arms is correct, not a defect.** Take 150
        # reported FAIL on a run whose pose was right the whole time it mattered:
        # 15 identity samples, every one of them during loading and the menu,
        # then 9 driven ones tracking Patola's chair exactly (42-45 deg on the
        # first hold, 65.00 clamped on the second, -1.58 back at centre). The
        # predicate asked whether a HUD line existed ANYWHERE in the log rather
        # than whether THIS sample was during steering -- the third check in
        # this file to grade by a condition it did not actually test.
        #
        # What matters is identity AFTER the pose has been driven once: that is
        # the gate dropping out mid-run, which is the failure a player feels.
        driven = [ln for ln in pose if "cam_valid=1" in ln]
        first_driven = pose.index(driven[0]) if driven else len(pose)
        ident = [ln for ln in pose[first_driven:] if "cam_valid=0" in ln]
        pre = first_driven
        unlinked = [ln for ln in pose if "shared=0" in ln]

        # ...and a FOURTH time, in this same check, exactly as the paragraph
        # above warned. Takes 220 and 221 each reported one post-arming identity
        # sample and each was INSIDE an external-view window the gate had
        # announced 1.5 s earlier:
        #
        #   [197886.806] headlook: view is now external/floating — idle
        #   [197888.290] vr pose: ... cam_valid=0 ...   <- "the gate dropped out"
        #   [197888.995] headlook: view is now the cockpit — steering
        #
        # Head-look was not live; it was idle BY DESIGN, and identity is the
        # designed consequence. The verdict even pointed at `steering`, which
        # is the one thing that had behaved correctly.
        #
        # Gating on the gate's own announcement, not on a branch re-derived
        # here: what makes a sample excusable is that head-look SAID it was
        # idle, never that the pose happened to come out identity.
        #
        # Head-look announces idleness through TWO predicates, and steers only
        # when both allow it (`may_steer = cockpit && hud`): the view (`view
        # is now external/floating — idle`) and the HUD (`HUD is now down —
        # idle, mouse handed back`). Take 318 opened the menu (both went
        # idle), then quit: the view came back "cockpit" during X4's teardown
        # while the HUD stayed down, and the identity sample after that was
        # counted as head-look live. Each predicate gets its own windows; a
        # sample is excused inside EITHER.
        def _windows(marker):
            win, open_at = [], None
            for ln in lines:
                if marker not in ln:
                    continue
                m = re.search(r"\[\s*(\d+\.\d+)\]", ln)
                if not m:
                    continue
                t = float(m.group(1))
                if "idle" in ln:
                    open_at = t if open_at is None else open_at
                elif open_at is not None:
                    win.append((open_at, t))
                    open_at = None
            if open_at is not None:
                win.append((open_at, float("inf")))
            return win
        idle_win = _windows("headlook: view is now") + \
            _windows("headlook: HUD is now")
        # Since take 335: off the pilot seat the write stands down and
        # `steering` is false by design, so identity is correct there too.
        # Take 335's FAIL was exactly those frames, uncounted as idle.
        seat_open = None
        for ln in lines:
            m = re.search(r"\[\s*(\d+\.\d+)\]", ln)
            if not m:
                continue
            if "headlook: out of the pilot seat" in ln:
                seat_open = float(m.group(1)) if seat_open is None else seat_open
            elif "headlook: in the pilot seat" in ln and seat_open is not None:
                idle_win.append((seat_open, float(m.group(1))))
                seat_open = None
        if seat_open is not None:
            idle_win.append((seat_open, float("inf")))

        def _in_idle(ln):
            m = re.search(r"\[\s*(\d+\.\d+)\]", ln)
            if not m:
                return False
            t = float(m.group(1))
            return any(a <= t <= b for a, b in idle_win)

        excused = [ln for ln in ident if _in_idle(ln)]
        ident = [ln for ln in ident if not _in_idle(ln)]
        print(f"pose  {len(pose)} sample(s), {len(driven)} driven, {pre} "
              f"identity before head-look armed (expected), {len(ident)} "
              f"identity after (not expected)")
        if excused:
            print(f"pose  {len(excused)} further identity sample(s) fall inside "
                  f"{len(idle_win)} window(s) where head-look ANNOUNCED itself "
                  f"idle (external/floating view, HUD down, or off the pilot seat) "
                  f"— identity is "
                  f"correct there, and counting them was a false FAIL on takes "
                  f"220 and 221")
        if unlinked:
            fails.append(
                f"the layer could not reach the injector's shared state on "
                f"{len(unlinked)} of {len(pose)} samples — every VR frame in "
                f"those is submitted world-locked no matter what head-look "
                f"does. Check x4vr_shared_state is exported and the injector "
                f"actually loaded")
        elif ident:
            # WHERE it happened changes what to look at. Take 318's one sample
            # came after X4's HUD global went NULL at exit, when the guard then
            # answered "HUD up" and head-look resumed steering a quitting X4.
            nul = re.search(r"\[\s*(\d+\.\d+)\][^\n]*X4's HUD global is NULL",
                            text)
            t_id = [float(m.group(1)) for m in
                    (re.search(r"\[\s*(\d+\.\d+)\]", ln) for ln in ident) if m]
            # TEARDOWN means the NULL came after play. Take 335's NULL was
            # the load screen's, 50 s before the cockpit, and every
            # identity sample came after it -- so this called a mid-run
            # gap "at TEARDOWN". The NULL must follow the last driven pose.
            t_dr = [float(m.group(1)) for m in
                    (re.search(r"\[\s*(\d+\.\d+)\]", ln) for ln in driven) if m]
            nuls = [float(m.group(1)) for m in re.finditer(
                r"\[\s*(\d+\.\d+)\][^\n]*X4's HUD global is NULL", text)]
            late = [t for t in nuls if t_dr and t >= max(t_dr)]
            nul = None
            if late:
                nul = re.search(r"\[\s*(%.3f)\][^\n]*X4's HUD global is NULL"
                                % late[0], text)
            at_exit = bool(nul and t_id and min(t_id) >= float(nul.group(1)))
            where = (f"at TEARDOWN: X4's HUD global had gone NULL at "
                     f"{float(nul.group(1)):.3f}, and head-look announced "
                     f"`HUD is now up — steering` instead of idling (the NULL "
                     f"guard answered 'HUD up' — fixed after take 318)"
                     if at_exit else
                     "mid-run: the gate dropped out, so those frames were "
                     "submitted world-locked with head-look live. cam_valid "
                     "went back to 0 with no idle announcement before it, so "
                     "this is not the by-design idle: check what cleared "
                     "cam_valid, not X4's camera")
            fails.append(
                f"the pose fell back to identity {len(ident)} time(s) AFTER it "
                f"had been driven, while head-look had NOT announced itself "
                f"idle — {where}")
        elif not driven and "X4VR_HEADLOOK=1" in (run or ""):
            fails.append(
                "the pose was never driven — every frame went to the runtime "
                "world-locked. Either head-look never armed or `steering` never "
                "reached the layer")
        elif not driven:
            # Gate on intent. Takes 165a/b deliberately dropped X4VR_HEADLOOK
            # so the off-axis question would not be confounded by head-look
            # holding MOUSELOOK, and this check failed both of them for not
            # answering a question they did not ask. That is the take-103
            # mistake — the fourth time in this file — and a scorer that FAILs
            # a correctly-designed run is one people stop reading.
            hl = re.search(r"\bX4VR_HEADLOOK=(\S*)", run or "")
            said = (f"X4VR_HEADLOOK={hl.group(1)}" if hl
                    else "X4VR_HEADLOOK was not set")
            print(f"pose  world-locked all run, and {said} — this run says "
                  f"nothing about head tracking, which is not a defect in it")

    # --- #59's per-frame pose channel, which is a DIFFERENT pose ------------
    #
    # Take 186b is why this section exists. The layer diagnosed itself
    # correctly and printed `pose final: ... FAIL — a pipeline was created
    # against a layout without our set`, and this file printed PASS over it,
    # then went on to say "pose not reported — this build predates the
    # instrument". Both statements came from the block above, which greps
    # "vr pose:" -- the pose we DECLARE TO THE RUNTIME. A flatscreen take
    # legitimately never logs that, so its message was true about its own
    # subject and irrelevant to the run being scored.
    #
    # Two different objects share the word "pose", the scorer only knew one of
    # them, and the run that was handed over with this file as its acceptance
    # check came back green with a wrecked frame. An acceptance check that
    # cannot see a FAIL the layer already printed is worse than no check: it
    # converts the layer's correct diagnosis into a confirmation.
    # **`pose final:` prints at TEARDOWN, and these runs never reach it.**
    # It is absent from takes 214, 216 and 217 alike, so failing on its absence
    # is a check that has never once passed -- which is not a check, it is a
    # false alarm on every pose run, and a false alarm is how a real one comes
    # to be ignored. `pose: channel up` is the line that actually says the
    # channel came up, and it IS present. Judge on that, and treat the teardown
    # report as a bonus when the exit path happens to produce it.
    chan = [ln for ln in lines if "pose final:" in ln]
    up = [ln for ln in lines if "pose: channel up" in ln]
    want_chan = "X4VR_POSE=1" in (run or "")
    collide = [ln for ln in lines if "could not extend a pipeline layout" in ln]
    if chan:
        ln = chan[-1]
        tail = ln.split("pose final:", 1)[1].strip()
        print(f"pose-channel  {tail}")
        if "FAIL" in ln:
            fails.append(
                "the #59 pose channel FAILED — pipelines were built on a "
                "layout that does not declare our set while their modules "
                "read it. That is an unbound descriptor, so the geometry "
                "those modules drew is undefined, and every OTHER number in "
                "this run was measured through it. Nothing here is evidence "
                "about the transform")
        elif collide:
            fails.append(
                "the pose channel reported OK but a pipeline layout still "
                "refused our set — the counters and the warning disagree, so "
                "one of the two instruments is wrong")
    elif want_chan and not up:
        fails.append(
            "the run asked for the pose channel (X4VR_POSE=1) and the layer "
            "never printed `pose: channel up` — the channel did not come up, "
            "so no module was driven. Gate on intent: this run asked the "
            "question and did not answer it")
    elif want_chan:
        # Came up, but the teardown report never ran. Worth saying once, and
        # NOT worth failing: takes 214, 216 and 217 all reach `channel up` and
        # none reaches `pose final:`, so failing here failed every pose run
        # this project has done -- including take 216, which produced a clean
        # positive result while carrying this FAIL.
        print(f"pose-channel  up ({up[-1].split('pose:', 1)[-1].strip()[:90]}) "
              f"— no `pose final:` teardown report, which these runs do not "
              f"reach; the coverage counters it carries are unavailable")
    if collide:
        # Printed even when the counters pass, because this is the line that
        # names the ROOT cause; `pose final` only reports the consequence.
        print(f"pose-channel  {len(collide)} layout collision warning(s) — "
              f"X4 declares more descriptor sets than our index assumed. "
              f"{collide[0].split('pose:', 1)[-1].strip()[:120]}")

    # --- was the classification computed on X4's bytes or on ours? ----------
    #
    # An intrinsic invariant, which is why it is here and not a count of rp
    # numbers: **X4 has ZERO World-and-procedural modules.** Measured over all
    # 409 dumps under both readings, narrow and wide. "World" means positioned
    # by a per-object matrix or the camera; "procedural" means the vertex
    # shader synthesises a fullscreen triangle from gl_VertexIndex. Nothing is
    # both, and task #40's comment records the same count from the other side.
    #
    # So a module reported as both is proof that the layer classified a module
    # it wrote rather than the one X4 sent. patch_vertex_pose_rotate emits an
    # access chain into camera-block member 1 inside the vertex function, and
    # classify()'s wide reading treats an access to member 0, 1, 7 or 8 there
    # as proof of world geometry -- so the patch manufactures the evidence the
    # classifier looks for. 30 of the 409 flip that way offline; takes 195 and
    # 196 each logged 28 of them, and both were scored PASS by this file before
    # this section existed.
    #
    # Fixed in take 197 by classifying ci->pCode. This section is what says
    # whether that reached the game.
    kindlines = [ln for ln in lines if "mod#" in ln and ("pipe:" in ln or
                                                         "bind rp #" in ln)]
    wp = set(re.findall(r"mod#\d+/([0-9a-f]{8})\(World[^)]*,procedural", " ".join(kindlines)))
    proc = set(re.findall(r"mod#\d+/([0-9a-f]{8})\([^)]*procedural", " ".join(kindlines)))
    if not kindlines:
        print("classify      no module lines — needs X4VR_PIPE_INVENTORY=1, so "
              "this run says nothing about which bytes were classified")
    elif not proc:
        # A zero that means "nothing was examined" reads identically to a zero
        # that means "nothing was wrong". Say which this is.
        print("classify      no procedural module was reported at all, so the "
              "World-and-procedural check had nothing to run on")
    else:
        print(f"classify      {len(proc)} procedural module(s) seen, "
              f"{len(wp)} of them also reported World")
        if wp:
            fails.append(
                f"{len(wp)} module(s) are reported as World AND procedural, and "
                f"X4 has none — 0 of 409 dumps under either reading. That means "
                f"the layer classified a module it had written, not the one X4 "
                f"sent: the pose patch adds the camera-block access the wide "
                f"reading treats as world geometry. Those modules took K_world, "
                f"the live-sx path and the affine while being screen-space "
                f"triangles, so nothing measured in this run about geometry is "
                f"evidence")

    # The camera walk that produces `proj`, `view` and `shadow` above is gated
    # on X4VR_DUMP_MATRICES, and take 224 was spent on a run that did not set
    # it. Absence of those sections then reads as "the instrument is broken"
    # rather than "it was never switched on", so name the knob.
    #
    # There is deliberately no separate #70 verdict here. I built one --
    # `proj FIELD`, partitioning slots into shadow-configured and rendering --
    # and it was wrong at the design level: `proj` above already reports the sx
    # SET with counts, degrees and which entry honours X4VR_FOV, and it said so
    # before this task existed. Worse, my partition keyed on the SLOT, while the
    # same output states "cam#N is a UBO slot, not a camera", so all 125 slots
    # landed in both halves and the verdict printed "MISMATCH -- 179.97 deg vs
    # 179.97 deg (1.0000x)". Read the `proj` set; it is the better instrument.
    if "camera walk OFF" in text:
        print("field   the camera walk is OFF in this run — it needs "
              "X4VR_DUMP_MATRICES=1, and without it the proj/view/shadow "
              "sections above cannot exist, so this run says nothing about "
              "the camera fields, the cascades, or #70")
    # --- did the cascade come from the block X4's shaders sample? -----------
    #
    # The layer prints `proj OBJBLOCK:` once it has walked the object slots.
    # Its absence in a run that asked for matrices means the build predates the
    # capture, and then every csm number above came from camera member 9 --
    # which 370 dumped modules declare and ZERO sample.
    #
    # `spread` is the guard that matters: the object block is per-DRAW, so if
    # its slots disagree about the cascade then no single one speaks for the
    # frame and the numbers have no referent. The layer warns; this fails.
    # --- the yaw fade near vertical ----------------------------------------
    #
    # Read from the injector's own full-rate tracker, NOT from the sampled
    # headlook lines. Those are every 30th call, so adjacent ones differ by
    # tens of degrees when the player moves fast: a doctored 65 deg cliff sat
    # inside take 209's own 62.68 deg level baseline, and no statistic over
    # them could tell the defect from a quick turn. Consecutive frames can.
    # (\d+(?:\.\d+)?) not ([\d.]+): the greedy version swallowed the sentence's
    # trailing period and float() threw on "0.021." — caught by a doctored line,
    # which is the only reason this section was ever run against one.
    fd = re.findall(r"fade: worst single-frame yaw command step — level "
                    r"(\d+(?:\.\d+)?) deg, fade band \(65-82\) "
                    r"(\d+(?:\.\d+)?), deep \(>=82\) (\d+(?:\.\d+)?)", text)
    if fd:
        lv, fb, dp = (float(x) for x in fd[-1])
        print(f"fade    worst single-frame command step: level {lv:.3f} deg, "
              f"fade band {fb:.3f}, deep {dp:.3f}")
        if fb == 0.0 and dp == 0.0:
            print("fade    the head never pitched past 65 deg, so this run "
                  "does NOT exercise the fade — look further up or down")
        elif dp > 1.4:
            # 1.4 is derived, not chosen: max_step_deg is 12 and the smoothstep
            # weight at exactly 82 deg is 0.0994, so the largest step the fade
            # can permit anywhere in this bucket is 12 * 0.0994 = 1.19 deg,
            # plus rounding to whole mouse counts. Anything above that means
            # the weight was NOT applied — which is exactly what take 210
            # found, where the injector had left it at its default 1.0.
            fails.append(
                f"the yaw command moved {dp:.3f} deg in a single frame past 82 "
                f"deg of pitch, where the weight is 0.0994 and max_step_deg is "
                f"12, so the fade cannot permit more than 1.19. The weight is "
                f"not reaching the servo")
        else:
            print(f"fade    deep step {dp:.3f} deg is within the 1.19 the "
                  f"weight permits (0.0994 x max_step 12), so the fade is "
                  f"reaching the servo")
    elif "X4VR_HEADLOOK=1" in (run or ""):
        print("fade    no `fade:` line — this build predates the full-rate "
              "tracker, so the log says nothing about the yaw fade")

    # --- task #77: did the roll-turned disparity actually run? --------------
    #
    # Three states, and the two that are NOT "on" are the ones worth printing.
    # X4VR_ROLL_STEREO needs the pose channel, and X4VR_POSE is absent from
    # every run command this project has used -- so the natural way to try this
    # is to set one knob, patch nothing, and read the null as "the fix does not
    # work". The layer names the missing knob; this repeats it where the run is
    # judged.
    # The A/B's own phases, reported as INTENT. This run is judged by a person,
    # so the log's job is to say what was asked for at each moment -- the
    # verdict comes from Patola, and the timestamps let his account and the
    # knob be lined up afterwards instead of trusting either alone.
    rab = re.findall(r"\[\s*(\d+\.\d+)\]\s+layer\s+roll A/B: (COMPENSATED|control)", text)
    if rab:
        comp = sum(1 for _, w in rab if w == "COMPENSATED")
        span = float(rab[-1][0]) - float(rab[0][0])
        print(f"roll    A/B ran {len(rab)} phase(s) over {span:.0f} s: "
              f"{comp} compensated, {len(rab)-comp} control")
        if len(rab) < 4:
            fails.append(
                f"the A/B only reached {len(rab)} phase(s) — too few for "
                f"Patola to have felt the switch more than once. A subjective "
                f"verdict on one transition is an anecdote; run longer")

    # The VALUE, which take 215 did not carry and could not have. Printed
    # before the verdict, because a verdict on an unpublished number is what
    # made that take unreadable.
    rv = re.findall(r"roll: head (-?\d+\.\d+) deg, weight (\d+\.\d+) -> "
                    r"published (-?\d+\.\d+) deg as \(cos,sin\)="
                    r"\((-?\d+\.\d+),(-?\d+\.\d+)\)(.*)", text)
    if rv:
        heads = sorted(abs(float(a)) for a, b, c, d, e, f in rv)
        pubs = sorted(abs(float(c)) for a, b, c, d, e, f in rv)
        forced = "FORCED" in rv[0][5]
        n = len(rv)
        print(f"roll    {n} sample(s){'  FORCED, head ignored' if forced else ''}: "
              f"head |roll| median {heads[n//2]:.2f} max {heads[-1]:.2f} deg, "
              f"published median {pubs[n//2]:.2f} max {pubs[-1]:.2f} deg")
        # Gate on INTENT. Roll stereo is back in the standing line (take 315),
        # so a FAIL whenever the head stayed level would fire on every take
        # that was about something else -- and a FAIL on every take is read
        # past, which is how X4VR_POSE=0 hid for ~55 of them. A run asks about
        # roll by its knobs or its name.
        roll_asked = bool(re.search(r"\bX4VR_ROLL_(AB|FORCE)=", run or "") or
                          re.search(r"\bX4VR_TAKE=\S*ROLL", run or "",
                                    re.IGNORECASE))
        if not forced and heads[-1] < 5.0 and roll_asked:
            fails.append(
                f"the head never rolled more than {heads[-1]:.2f} deg, so this "
                f"run does NOT exercise #77 whatever else it shows — tilt the "
                f"head, or use X4VR_ROLL_FORCE to drive the shear directly")
        elif not forced and heads[-1] < 5.0:
            print(f"roll    not exercised (head |roll| max {heads[-1]:.2f} deg) "
                  f"— fine for a take that was not about roll; it says nothing "
                  f"about #77")
        elif pubs[-1] < 0.5 * heads[-1]:
            fails.append(
                f"the head rolled to {heads[-1]:.2f} deg but only {pubs[-1]:.2f} "
                f"was published — the value is being lost between head_angles() "
                f"and the buffer, so the shader is turning the disparity by "
                f"almost nothing. This is the value, not the arithmetic")

        # The SIGN, which every line above throws away by taking abs(). Take
        # 220 published the right magnitude with the wrong sign and scored
        # clean here; the defect lived in the one dimension the instrument had
        # already discarded, and Patola paid for it with a run.
        #
        # The shear must be the head's roll NEGATED (see roll_compensation_deg
        # -- calibrated on takes 216 and 219, not derived). Only samples with a
        # real tilt can tell the two apart, so small ones are excluded rather
        # than averaged in.
        if not forced:
            agree = disagree = 0
            for a, b, c, _d, _e, tail in rv:
                if "[A/B: control]" in tail:
                    continue
                h, w, p = float(a), float(b), float(c)
                if abs(h) * w < 5.0:
                    continue
                if abs(p - (-h * w)) < abs(p - (h * w)):
                    agree += 1
                else:
                    disagree += 1
            if agree or disagree:
                print(f"roll    sign: {agree} sample(s) match -head, "
                      f"{disagree} match +head (of {agree + disagree} tilted "
                      f"past 5 deg on the compensated arm)")
            if disagree > agree:
                fails.append(
                    f"the published shear follows the head's roll UNNEGATED "
                    f"({disagree} of {agree + disagree} tilted samples) — this "
                    f"is take 220's defect. At head roll H it leaves the "
                    f"disparity at 2H instead of 0, and past 45 deg the eyes "
                    f"also swap, so neither A/B arm can have a null. The map "
                    f"is roll_compensation_deg() and it is a negation")
            elif not agree and not disagree and heads[-1] >= 5.0:
                print("roll    sign: UNJUDGED — no compensated-arm sample was "
                      "tilted past 5 deg, so this run says nothing about the "
                      "head-to-shear map, only about the magnitude")

    # --- task #74: the head's lateral/vertical shift ------------------------
    #
    # Gated on INTENT, not outcome: a run that asked for translation and
    # published nothing is a failure even though every other line is clean,
    # and that is the case a "report what we saw" section would pass silently.
    tv = re.findall(r"head translate: raw \((-?\d+\.\d+), (-?\d+\.\d+), "
                    r"(-?\d+\.\d+)\) m in head axes -> published "
                    r"\((-?\d+\.\d+), (-?\d+\.\d+)\)([^\n]*)", text)
    asked = "X4VR_HEAD_TRANSLATE=1" in (run or "")
    pose_asked = bool(re.search(r"\bX4VR_POSE=1\b", run or ""))
    if "head translate: ASKED FOR BUT INACTIVE" in text and pose_asked:
        # Take 315's build printed this once at TEARDOWN, after its final
        # reports, on a run that had published 31 real shifts: the channel was
        # already down and the line blamed the configuration. With X4VR_POSE=1
        # asked for, it is that teardown frame, not a refused run.
        print("translate  (one 'ASKED FOR BUT INACTIVE' line from a build that "
              "printed it at teardown with X4VR_POSE=1 set — not a refusal)")
    if "head translate: ASKED FOR BUT INACTIVE" in text and not pose_asked:
        # Ranked first: every sample below would read a published 0.0000 and
        # look like a head that never leaned, when the shift was REFUSED.
        fails.append(
            "X4VR_HEAD_TRANSLATE was asked for but the pose channel is off, so "
            "the layer refused the shift (the shaders could not apply it, and "
            "declaring it anyway makes the compositor warp a lean that is not "
            "in the image). Add X4VR_POSE=1 — it is not implied. This run says "
            "nothing about #74")
    elif tv:
        txs = [abs(float(a[3])) for a in tv]
        tys = [abs(float(a[4])) for a in tv]
        raws = [max(abs(float(a[0])), abs(float(a[1]))) for a in tv]
        clamped = sum(1 for a in tv if "CLAMPED" in a[5])
        latch = re.search(r"neutral point latched at \(([^)]*)\)", text)
        print(f"translate  {len(tv)} sample(s): |published| max x {max(txs):.4f} "
              f"y {max(tys):.4f} m, raw head motion max {max(raws):.4f} m"
              f"{f', {clamped} CLAMPED' if clamped else ''}")
        if latch:
            print(f"translate  neutral point ({latch.group(1)}) — every value "
                  f"above is measured from there, not the stage origin")
        # A run where the head never actually moved says nothing, whatever the
        # code did. State it rather than reporting a clean zero as a pass.
        if max(raws) < 0.02:
            fails.append(
                f"the head never moved more than {max(raws):.4f} m, so this run "
                f"does NOT exercise #74 whatever else it shows — lean left and "
                f"right, and up and down, by a hand's width at least")
        elif clamped == len(tv):
            fails.append(
                f"every one of {len(tv)} sample(s) was CLAMPED, so the shift "
                f"was pinned at the limit the whole run and nothing varied — "
                f"raise X4VR_HEAD_TRANSLATE_MAX or lean less")
    elif asked:
        fails.append(
            "X4VR_HEAD_TRANSLATE=1 was asked for and the layer published no "
            "shift at all — the knob did not reach the pose block. Check that "
            "the VR views were located; the shift is computed from them")
    else:
        print("translate  not asked for in this run (X4VR_HEAD_TRANSLATE unset)")

    # --- task #57: which image carries the UI -------------------------------
    #
    # Read from the LATCHED `ui: TARGET` line, not from the `ui final:` report.
    # The report fires from mv_report, whose useful call is at teardown, and
    # these runs do not reach teardown — the same trap that made #76's first
    # acceptance rule unsatisfiable. The latch fires at the framebuffer, so any
    # run that renders UI has it.
    #
    # Not gated on intent: the identification is unconditional, and a run that
    # cannot name the UI target is one nothing about #57 can be built on,
    # whatever it was asked for.
    uit = re.search(r"ui: TARGET img #(\d+) — (\d+)x(\d+) fmt=(\d+), "
                    r"(DOUBLED to 2|NOT doubled)", text)
    uiwarn = re.findall(r"ui \S+ \S*\s*: (\d+) UI-only target\(s\) — EXPECTED",
                        text)
    if uit:
        print(f"ui      img #{uit.group(1)} carries the UI — "
              f"{uit.group(2)}x{uit.group(3)} fmt={uit.group(4)}, "
              f"{uit.group(5)}")
        if uit.group(5) != "DOUBLED to 2":
            # **The reason here used to be wrong and it would have failed a
            # healthy run.** It said a non-doubled target "would show the left
            # eye's UI to both", which assumed the UI pass is drawn per-eye. It
            # is not: classify_unsheared() marks it all-LDR, so the original
            # unpatched modules are bound and BOTH array layers carry identical
            # pixels. Layer B is also a quad, not the projection layer that
            # sentence was written for.
            #
            # What doubling is still needed for is narrow and real: ui_capture()
            # copies two array layers into a two-layer staging image, so a
            # target with one layer would make that copy wrong.
            fails.append(
                "the UI target is NOT doubled, so ui_capture()'s two-layer "
                "copy has no second layer to read. The quad itself only needs "
                "layer 0 — this is about the capture, not about the stereo")
    elif "ui final:" in text or "ui first present:" in text:
        fails.append(
            "no `ui: TARGET` line — the predicate never resolved to exactly "
            "one image, so which image carries the UI is NOT established and "
            "#57 has nothing to put on a layer. The `ui ...: img #N is NOT "
            "the UI target` lines name the reason each candidate was rejected")
    else:
        print("ui      no UI-target report — this build predates task #57")

    # --- task #57 take 243: was the quad's pose actually EXERCISED? ----------
    #
    # **The rule take 242 needed and did not have.** Its submission line was
    # latched once, at start-up, where nothing is driving and every angle is
    # zero — so it printed the one configuration in which its own claim could
    # not be tested, and a wrong frame survived a green log. The line is now
    # periodic; this checks that at least one sample caught a turned camera.
    #
    # Gated on intent, and on intent only: a run that asked for the UI layer
    # and never saw a non-zero declared angle has not tested anything about the
    # quad's frame, whatever the screen looked like.
    if "X4VR_UI_LAYER=1" in (run or ""):
        quads = re.findall(r"ui: quad at [\d.]+ m, [\d.]+ x [\d.]+ m, "
                           r"declared yaw=(-?[\d.]+) pitch=(-?[\d.]+) deg",
                           text)
        if not quads and "ui: SUBMITTING a quad" in text:
            # Take 242 and earlier. That build latched ONE line at start-up and
            # declared identity; it cannot report a turned camera because it
            # never printed a second sample. Not a failure of the run — a
            # failure of the instrument it shipped with, and saying so is the
            # difference between "did not happen" and "cannot be seen here".
            print("ui quad build predates the periodic line — identity-era "
                  "submission, frame NOT testable from this log")
        elif not quads:
            fails.append(
                "X4VR_UI_LAYER=1 but no `ui: quad` line — the quad never "
                "submitted, so nothing about #57's frame was exercised")
        else:
            worst = max(max(abs(float(y)), abs(float(p))) for y, p in quads)
            print(f"ui quad {len(quads)} submission sample(s), largest "
                  f"declared angle {worst:.2f} deg")
            if worst < 1.0:
                fails.append(
                    f"every one of the {len(quads)} `ui: quad` samples "
                    f"declared under 1 deg (largest {worst:.2f}). The quad and "
                    "the ship's frame coincide there, so this run cannot tell "
                    "the fix from take 242's identity — the head-look servo "
                    "was not driving, or the head never turned while the UI "
                    "was up")

    # --- task #57: the geometry the layer actually submits -------------------
    #
    # **This exists because three fixes for the HUD slip were argued from a
    # video.** The head, the scene's declared viewpoint and the quad's centre
    # are three points the layer has exactly, every frame, and none of them was
    # ever printed. The line reports them; this reports what they mean.
    #
    # The discriminator is not any single magnitude but whether the two layers
    # AGREE about where the viewer is. `d_scene` is how far the head is from
    # the point layer A claims the pixels came from; `d_quad - zm` is how far
    # the quad's true distance from the eye has drifted from its nominal one.
    # If those two track each other, both layers see the same head displacement
    # and the slip is elsewhere. If `d_quad - zm` moves while `d_scene` stays
    # flat, they do not, and that gap is the slip.
    #
    # **Pre-declaration samples must be dropped, and take 253 is why.** The
    # first frames are logged before the scene's viewpoint has ever been
    # declared, so `declared=0,0,0` and `d_scene` degenerates into the head's
    # height above the origin -- about 1.13 m, twenty times the real range.
    # Take 253 had 81 such samples out of 14523: 0.56% of the data, and they
    # dragged r(t_head, d_scene) from +0.919 down to +0.259, which reads as
    # "neither tracks the head" when in fact they track it closely. A handful
    # of high-leverage points at the wrong end of the range is all a
    # correlation needs to invert its verdict.
    geom = [m.groupdict() for m in re.finditer(
        r"geom: head=\S+ neutral=\S+ declared=(?P<decl>\S+) quad=\S+ "
        r"zm=(?P<zm>[\d.]+) t_head=(?P<th>[\d.]+) d_scene=(?P<ds>[\d.]+) "
        r"d_quad=(?P<dq>[\d.]+) hv=\d(?: qv=(?P<qv>\d))?", text)]
    predecl = sum(1 for g in geom if g["decl"] == "0.0000,0.0000,0.0000")
    geom = [g for g in geom if g["decl"] != "0.0000,0.0000,0.0000"]
    if geom:
        import statistics
        zmv = [float(g["zm"]) for g in geom]
        th = [float(g["th"]) for g in geom]
        ds = [float(g["ds"]) for g in geom]
        dq = [float(g["dq"]) for g in geom]
        # qv is ABSENT on the pre-253 line format and "0" when the UI layer
        # is off. Those are different statements and the first draft printed
        # "quad never submitted" for take 252, which had submitted it 12940
        # times on a log that simply predates the field. Same
        # didn't-happen/cannot-say confusion this file has already fixed once.
        # `re.findall` yields '' for a non-participating optional group,
        # NOT None — so an `is not None` test is always true and the
        # old-format check silently passed. Compare against ''.
        have_qv = any(g["qv"] is not None for g in geom)
        qv = sum(1 for g in geom if g["qv"] == "1")
        hv = len(geom)
        dqe = [q - z for q, z in zip(dq, zmv)]
        def stat(v):
            s = sorted(v)
            return (statistics.median(s), s[int(0.9 * (len(s) - 1))], s[-1])
        print(f"geom    {len(geom)} sample(s), head located in {hv}, "
              + (f"quad submitted in {qv}" if have_qv
                 else "quad state not recorded (log predates qv=)")
              + (f"; {predecl} pre-declaration sample(s) dropped" if predecl
                 else ""))
        for nm, v in (("t_head  head from neutral", th),
                      ("d_scene head from declared", ds),
                      ("d_quad-zm quad vs nominal", dqe)):
            m, p, x = stat([abs(a) for a in v])
            print(f"        {nm:<28} median {m:.4f}  p90 {p:.4f}  max {x:.4f} m")
        # Do the two layers see the same head displacement?
        if len(th) > 20 and statistics.pstdev(th) > 1e-4:
            def corr(a, b):
                ma, mb = statistics.fmean(a), statistics.fmean(b)
                num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
                da = sum((x - ma) ** 2 for x in a) ** 0.5
                db = sum((y - mb) ** 2 for y in b) ** 0.5
                return num / (da * db) if da > 1e-9 and db > 1e-9 else 0.0
            rs, rq = corr(th, ds), corr(th, dqe)
            print(f"        t_head vs d_scene      r={rs:+.3f}")
            print(f"        t_head vs (d_quad-zm)  r={rq:+.3f}")
            # **Conditional, because the first version was not.** It printed
            # "both r near +1 => the layers agree" unconditionally, and take
            # 250 came back with +0.986 and +0.095 under that sentence. A
            # canned line that reads like a verdict is worse than no line.
            if rs > 0.8 and rq > 0.8:
                print("        both track the head => the layers agree about "
                      "the viewer, and the slip is NOT this")
            elif rs > 0.8:
                print("        the scene's declared viewpoint does NOT follow "
                      "the head (d_scene grows with t_head) while the quad's "
                      "distance does not track it — the two layers are told "
                      "different things about where the viewer is")
            else:
                print("        neither tracks the head; read the medians "
                      "above rather than these correlations")
        if have_qv and qv == 0:
            print("        quad never submitted (UI layer off) — d_quad is not "
                  "meaningful here; t_head and d_scene are")
    elif "X4VR_UI_LAYER=1" in (run or ""):
        print("geom    no `geom` line — build predates it, or X4VR_POSELOG "
              "is unset (the line rides on it)")

    # --- task #76: X4's own UI-composition switch ---------------------------
    #
    # Gated on INTENT for the reason #74 above is: a run that asked to force the
    # switch and forced it in zero modules is a failure, and it looks exactly
    # like a run that never asked.
    #
    # **Counted from the PER-MODULE lines, not from the aggregate, and take 239a
    # is the whole reason.** The aggregate at "first present" is a snapshot: X4
    # creates the copy_gamma permutations lazily, so 239a patched two of them
    # 3 ms before the first present and the other two 9.8 s later. The line read
    # 2, this section demanded 4, and a run whose screen answered the question
    # perfectly -- every scrap of UI gone, all of it back without the knob --
    # was scored FAIL by a rule I wrote. The teardown line could have said 4 and
    # these runs never reach teardown, which this same file says about the pose
    # channel a few sections up.
    #
    # So: `hudless #N` is one line per module and N is monotonic, which makes it
    # the only count available in a run that exits the way these do.
    #
    # Four is a CEILING, not a target: exactly four of the 409 dumped modules
    # carry a uint named U_fsr3g_useuitexture at byte 512 of set 4 binding 0
    # (tools/block_offset_census.py, asserted by the offline suite). Fewer means
    # X4 did not need every permutation; MORE means the signature widened, and
    # only that direction invalidates the screen.
    #
    # 0 is the CONTROL arm and must be scored as a legitimate run, not as
    # "nothing happened": it forces the switch to its shipped value, so the
    # modules are still matched and rewritten and the screen must be unchanged.
    hud = [m.group(1) for m in
           re.finditer(r"hudless #(\d+): U_fsr3g_useuitexture forced", text)]
    hud_asked = re.search(r"X4VR_HUDLESS=([^\s]+)", run or "")
    hud_bad = "is neither 0 nor 1" in text
    if hud_bad:
        fails.append(
            "X4VR_HUDLESS was set to something that is neither 0 nor 1, so the "
            "layer ignored it and patched NOTHING. This run says nothing about "
            "#76 in either direction")
    elif hud_asked:
        n = max((int(x) for x in hud), default=0)
        arm = ("FORCED — X4 should emit the HUD-less scene, so ALL of X4's UI "
               "must be GONE (menus, load screens, map, HUD) and the scene "
               "otherwise intact"
               if hud_asked.group(1) == "1" else
               "CONTROL — the switch is forced to its SHIPPED value, so this "
               "must look identical to a run with X4VR_HUDLESS unset")
        print(f"hudless  X4VR_HUDLESS={hud_asked.group(1)}: {n} module(s) "
              f"patched (ceiling 4) — {arm}")
        if n == 0:
            fails.append(
                "X4VR_HUDLESS was asked for and NOT ONE module was patched — "
                "the signature matched nothing X4 created. Re-run "
                "tools/block_offset_census.py 4 0 --at 512 against a fresh "
                "dump; this run says nothing about #76")
        elif n > 4:
            fails.append(
                f"X4VR_HUDLESS forced the switch in {n} module(s), MORE than "
                f"the 4 the offline census names, so the signature has widened "
                f"and is hitting shaders it was not built for. Do NOT read the "
                f"screen as an answer about #76")
        elif n < 4:
            print(f"hudless  {n} of the 4 known permutations were created this "
                  f"run — X4 makes them lazily, so this is not a shortfall")
    elif hud:
        print(f"hudless  not asked for (X4VR_HUDLESS unset); "
              f"{max(int(x) for x in hud)} module(s) patched, as expected")
    else:
        print("hudless  not asked for in this run (X4VR_HUDLESS unset)")

    if "roll stereo: ON" in text:
        print("roll    ON — the eye offset turns with the head. Under tilt the "
              "disparity should stay HORIZONTAL: close one eye, then the "
              "other, and the two images must differ side-to-side, never one "
              "above the other")
    elif "roll stereo: ASKED FOR BUT INACTIVE" in text:
        fails.append(
            "X4VR_ROLL_STEREO was set but the pose channel is down, so nothing "
            "was patched and the disparity still goes vertical under head "
            "tilt. Add X4VR_POSE=1 — it is not implied. This run says nothing "
            "about #77")
    elif "roll stereo: ASKED FOR BUT REFUSED" in text:
        fails.append(
            "X4VR_ROLL_STEREO was set but refused: the off-axis affine is on "
            "and the two are untested together. Drop X4VR_OFFAXIS for this "
            "measurement, or accept that #77 is unmeasured in this run")
    elif "X4VR_ROLL_STEREO" in (run or ""):
        print("roll    the knob is in the command line but the layer never "
              "reported on it — the layer may predate #77")

    # --- task #75: is the declared pose the pose X4 rendered from? ----------
    #
    # The layer submits `sh->cam_yaw_deg` as the orientation the frame was drawn
    # with, and the compositor reprojects by it. That field is the injector's
    # BELIEF, stored after the servo step, so it contains a command X4 will not
    # receive for about two ticks. If it runs ahead of the pixels the warp is
    # applied by an angle the frame was never drawn with -- the world swims
    # exactly when the head moves.
    #
    # WHAT A HEALTHY RUN READS, before the numbers:
    #
    #   |d| small and NOT growing with the step size -> the declaration matches
    #       the pixels and #75 is not a defect.
    #   |d| ~ one servo step, growing when the lead is armed -> the declaration
    #       leads the pixels, and the lead makes it worse while making the
    #       tracking better. Those pull opposite ways on what a person sees.
    #
    # **`age` is load-bearing.** The readback is older than the frame; if it is
    # old, `d` is mostly the camera having moved and says nothing about the
    # declaration. The previous attempt at this number paired the periodic
    # `vr pose` line against `camread` across a 0.25 s window and got 6-7 deg of
    # pure sampling offset from seven samples. Anything over ~1.5 tick periods
    # is reported and excluded rather than averaged in.
    #
    # **`rot` splits this report in two, and it has to.** Under
    # X4VR_DECLARE_RENDERED_ROT the declaration stops being the servo command
    # and becomes X4's camera read on the submitting thread, so `d` stops
    # meaning "the gain error" and starts meaning "the injector's copy is
    # stale" -- a healthy run would be reported as "the declaration LEADS the
    # pixels" by a rule written for the other arm. That is this scorer failing a
    # healthy run for the third time if it is left alone, so the arm is read off
    # the log line rather than assumed, and `cmd` is carried in BOTH arms so the
    # two are compared like with like.
    pl = re.findall(
        r"\[\s*(\d+\.\d+)\]\s+layer\s+poselag: declared=(-?\d+\.\d+),(-?\d+\.\d+) "
        r"x4=(-?\d+\.\d+),(-?\d+\.\d+) d=(-?\d+\.\d+),(-?\d+\.\d+) "
        r"age=(-?\d+\.\d+) valid=(\d) rawvalid=(\d)"
        r"(?: residual=-?\d+\.\d+,-?\d+\.\d+"
        r"(?: cmd=(-?\d+\.\d+),(-?\d+\.\d+) rot=(\d)(?: foot=(\d))?)?)?", text)
    # Off the seat under X4VR_HEADLOOK_FOOT the declaration is the head part
    # and `x4` includes the body, so `d` there is the body's angle, not an
    # error. Stamped by the layer (`foot=1`) and set apart here.
    pl_foot = [g for g in pl if g[13] == "1"]
    pl = [g for g in pl if g[13] != "1"]
    if pl_foot:
        print(f"poselag  {len(pl_foot)} frame(s) ON FOOT set apart — declared "
              f"head-look's written angle; X4's camera there includes the body")
    if pl:
        # Groups, in order: ts, decl_y, decl_p, x4_y, x4_p, d_y, d_p, age,
        # valid, rawvalid, cmd_y, cmd_p, rot. The last three are absent in logs
        # written before the #75 A/B, and `None` there means "unknown arm",
        # which is deliberately not the same value as either arm.
        rows = []
        for g in pl:
            rows.append((
                float(g[0]),                                   # 0 ts
                wrap180(float(g[5])), float(g[6]),             # 1,2 d yaw/pitch
                float(g[7]),                                   # 3 age
                g[8] == "1", g[9] == "1",                      # 4,5 valid flags
                float(g[1]),                                   # 6 declared yaw
                float(g[3]),                                   # 7 x4 yaw
                float(g[10]) if g[10] else None,               # 8 cmd yaw
                # 9: rot MODE, not a bool. 0 = servo command, 1 = the
                # sample at submit (take 278), 2 = the sample at the first
                # render pass (take 280). Read as a bool this would file
                # every mode-2 frame under "control" and report the fix as
                # never having acted.
                int(g[12]) if g[12] else None,
            ))
        live = [r for r in rows if r[4] and r[5]]
        # Which arm produced this log is READ off the line, never inferred from
        # the knob I believe I passed: an env block can arrive truncated, which
        # is how take 114 died.
        rot_on = [r for r in live if r[9]]
        rot_off = [r for r in live if r[9] == 0]
        modes = sorted({r[9] for r in rot_on})
        msrc = "+".join({1: "at-submit", 2: "at-draw"}.get(m, f"mode{m}")
                        for m in modes)
        if rot_on or rot_off:
            print(f"poselag  arm: {len(rot_on)} frame(s) declared X4's OWN "
                  f"camera [{msrc}], {len(rot_off)} declared the servo "
                  f"command (control)")
        elif live:
            print("poselag  arm: log predates the #75 A/B — no cmd=/rot= "
                  "fields, so only the control reading below applies")
        print(f"poselag {len(rows)} submitted frame(s), {len(live)} with both a "
              f"declared pose and a live read-back")
        if not live:
            print("poselag  nothing to compare — cam_valid or rawvalid was 0 "
                  "throughout, so the image was world-locked and no "
                  "reprojection error can arise")
        else:
            ages = sorted(r[3] for r in live)
            amed = ages[len(ages) // 2]
            # **Relative to the run's own cadence, with only a slack margin.**
            # A fixed 50 ms floor was tried first and let a doctored case
            # through untouched: at a 77 Hz tick the typical age is 6 ms and a
            # 36 ms sample -- six ticks stale -- sat comfortably under the
            # floor. The floor exists to stop a tiny median excluding
            # everything, so it belongs as a few ms of slack, not as a ceiling
            # larger than the effect.
            cutoff = max(3 * amed, amed + 0.005)
            fresh = [r for r in live if 0 <= r[3] <= cutoff]
            stale = len(live) - len(fresh)
            print(f"poselag  read-back age: median {amed*1000:.1f} ms, "
                  f"p90 {ages[int(0.9*(len(ages)-1))]*1000:.1f} ms"
                  f"{f', {stale} sample(s) excluded as stale' if stale else ''}")
            if fresh:
                dy = sorted(abs(r[1]) for r in fresh)
                n = len(dy)
                print(f"poselag  |declared - X4| yaw: median {dy[n//2]:.3f} deg, "
                      f"p90 {dy[int(0.9*(n-1))]:.3f}, max {dy[-1]:.3f}  (n={n})")

                # **Split by the lead's A/B arm, which is the whole point.**
                #
                # The layer cannot stamp the arm -- it does not know it -- so
                # the join is on time, against the injector's own `lead
                # ARMED/IDLE` transitions. Those are statements of INTENT with
                # timestamps, not a re-derivation of the branch, and the phases
                # are 20 s apart so a millisecond join cannot land in the wrong
                # one.
                #
                # This is the trade-off #73 never measured: the lead reduces
                # the tracking error lagfit reports while enlarging every
                # command, and every command is exactly the angle the
                # declaration runs ahead by.
                ph = re.findall(
                    r"\[\s*(\d+\.\d+)\]\s+inject\s+headlook: lead (ARMED|IDLE)",
                    text)
                if ph:
                    marks = [(float(t), s == "ARMED") for t, s in ph]

                    def arm_at(t):
                        cur = None
                        for mt, a in marks:
                            if mt <= t:
                                cur = a
                            else:
                                break
                        return cur

                    by = {True: [], False: []}
                    for r in fresh:
                        a = arm_at(r[0])
                        if a is not None:
                            by[a].append(abs(r[1]))
                    if by[True] and by[False]:
                        out = {}
                        for a, name in ((False, "IDLE (control)"),
                                        (True, "ARMED (lead)  ")):
                            v = sorted(by[a])
                            m = len(v)
                            out[a] = v[int(0.9 * (m - 1))]
                            print(f"poselag  {name} n={m:5d}  |declared - X4| "
                                  f"median {v[m//2]:.3f} deg  p90 "
                                  f"{out[a]:.3f}  max {v[-1]:.3f}")
                        if out[False] > 1e-6:
                            g = out[True] / out[False]
                            print(f"poselag  the lead changes the declaration "
                                  f"error by {g:.2f}x at p90 — "
                                  f"{'it BUYS tracking with reprojection error, and #73 must be judged on both' if g > 1.3 else 'no material trade-off; the lead is not paying for its tracking gain here'}")
                # **The #75 A/B, scored on the quantity the fix acts on.**
                #
                # Offline from takes 276/277: cmd = 0.2*head + 0.8*x4, so the
                # command sits a fifth of the way from the camera that drew the
                # pixels towards the head, and the compositor undoes only 80% of
                # the error in the image. |cmd - x4| is that leftover fifth, and
                # it is the number the fix is supposed to drive to zero. It is
                # computed the same way in both arms, which is the only reason
                # the arms can be compared at all.
                gain[:] = sorted(abs(wrap180(r[8] - r[7])) for r in fresh
                              if r[8] is not None)
                if gain:
                    m = len(gain)
                    print(f"poselag  |cmd - X4| (the fifth left uncorrected): "
                          f"median {gain[m//2]:.3f} deg, p90 "
                          f"{gain[int(0.9*(m-1))]:.3f}, max {gain[-1]:.3f}")
                # **Intent, then outcome, and never outcome alone.** `rot=` says
                # what HAPPENED; the env line says what the run ASKED for. If
                # the knob was set and no frame carries rot=1 the fix was inert,
                # and reporting that as "the control arm" would quietly turn a
                # broken run into a clean-looking one — the exact shape of
                # `gate-on-intent-not-outcome`.
                # Any non-zero setting, not just "=1": the knob also takes
                # 2 and the words submit/draw, and matching only "=1" would
                # call a mode-2 run unasked-for and FAIL it.
                asked_rot = bool(run and any(
                    f"X4VR_DECLARE_RENDERED_ROT={v}" in run
                    for v in ("1", "2", "draw", "submit")))
                if asked_rot and not rot_on:
                    why = ("GetCameraRotation did NOT resolve"
                           if "declrot: GetCameraRotation NOT FOUND" in text
                           else "no declrot: line at all, so the knob never "
                                "reached the layer")
                    fails.append(
                        f"X4VR_DECLARE_RENDERED_ROT was set but every frame "
                        f"declared the servo command — {why}. This run does "
                        f"NOT test #75's fix")
                elif rot_on and not asked_rot:
                    fails.append(
                        "frames declared X4's own camera but the env line does "
                        "not carry X4VR_DECLARE_RENDERED_ROT=1 — the log and "
                        "the command line disagree about what was run")
                if rot_on:
                    # In this arm `declared` IS X4's camera, so |declared - X4|
                    # measures the injector's copy going stale, not a lead. It
                    # SHOULD be non-zero and it should be about v*age; reading
                    # it as a lead here is the failure this split exists to
                    # prevent.
                    dr = sorted(abs(wrap180(r[6] - r[7])) for r in rot_on)
                    m = len(dr)
                    print(f"poselag  fix ON: |declared - X4readback| median "
                          f"{dr[m//2]:.3f} deg p90 {dr[int(0.9*(m-1))]:.3f} — "
                          f"this is the injector's copy going stale over "
                          f"{amed*1000:.0f} ms, NOT a lead; the declaration is "
                          f"X4's camera by construction here")
                    if gain:
                        print(f"poselag  => #75 fix ACTED. The declaration no "
                              f"longer carries the {gain[len(gain)//2]:.3f} deg "
                              f"median command offset, and the compositor is "
                              f"being asked for the whole error rather than 80% "
                              f"of it. Judge the run on whether the jitter went, "
                              f"not on this number.")
                elif rot_off or not (rot_on or rot_off):
                    # The control arm, and every log written before the A/B.
                    # The scale that matters: one servo step. Below a tenth of a
                    # degree the compositor's warp is inside its own filtering
                    # and this is not a mechanism worth chasing.
                    if dy[int(0.9 * (n - 1))] < 0.1:
                        print("poselag  => the declaration TRACKS the pixels. "
                              "#75's mechanism exists but is too small to "
                              "explain anything; look elsewhere for #73's "
                              "residual.")
                    else:
                        print(f"poselag  => the declaration LEADS the pixels by "
                              f"up to {dy[-1]:.2f} deg. The compositor is "
                              f"warping each frame by an angle it was not drawn "
                              f"with, and that is a per-frame position error the "
                              f"servo's own tracking number cannot see.")

    # --- task #75: the render-to-submit gap take 278 assumed -----------------
    #
    # Take 278 declared `camera(T_submit)` on the reasoning that a fresher read
    # is a better one. But the pixels were drawn at T_render < T_submit, so a
    # fresher read leads the RENDERED pose by more, not less, and which
    # candidate is closer depends on how far the camera moves in between. That
    # quantity was never measured; the speed-matched arms crossed over at
    # ~100 deg/s because of it.
    #
    # `dyaw` is that quantity, measured. The verdict is a comparison against
    # THIS run's own gain error, not against a constant carried from an earlier
    # take -- a threshold remembered from another run is how a scorer ends up
    # answering a question that has moved.
    cs = re.findall(
        r"camstamp: atdraw=(-?\d+\.\d+),(-?\d+\.\d+) "
        r"atsubmit=(-?\d+\.\d+),(-?\d+\.\d+) dt=(-?\d+\.\d+) "
        r"dyaw=(-?\d+\.\d+) dpitch=(-?\d+\.\d+) seq=(\d+) torn=(\d) got=(\d)",
        text)
    if cs:
        # torn=0 AND got=1. Both conditions were inverted when this was first
        # written, so every clean frame was discarded and every unusable one
        # kept — and the FAIL branch fired on all four synthetic cases, which
        # is the only reason it was caught before a run.
        good = [(abs(wrap180(float(g[5]))), abs(float(g[6])), float(g[4]))
                for g in cs if g[8] == "0" and g[9] == "1"]
        torn = sum(1 for g in cs if g[8] == "1")
        nogot = sum(1 for g in cs if g[9] == "0")
        print(f"camstamp {len(cs)} frame(s), {len(good)} clean "
              f"({torn} torn — X4 stamped a new frame mid-read, "
              f"{nogot} with no camera call)")
        if not good:
            fails.append("camstamp produced no clean pair — X4VR_CAMSTAMP was "
                         "set but every frame was torn or unstamped, so this "
                         "run does NOT measure the render-to-submit gap")
        else:
            dy = sorted(g[0] for g in good)
            dts = sorted(g[2] for g in good)
            n = len(dy)
            print(f"camstamp  render->submit dt: median {dts[n//2]*1000:.1f} ms,"
                  f" p90 {dts[int(0.9*(n-1))]*1000:.1f} ms")
            print(f"camstamp  |camera moved in between| yaw: median "
                  f"{dy[n//2]:.3f} deg, p90 {dy[int(0.9*(n-1))]:.3f}, "
                  f"max {dy[-1]:.3f}")
            # The comparison that decides it, against this run's own numbers.
            # **NOT the median.** X4's camera is a 30 Hz staircase and the
            # draw->submit gap is 13-22 ms, so in ~76% of frames it has not
            # stepped at all and dyaw is exactly 0. The median therefore reads
            # 0.000 in EVERY speed bin -- including one whose p90 is 9.9 deg and
            # whose max is 15.0 -- which is a statistic that cannot express the
            # alternative it exists to rule out. Take 279 was scored "SMALL" on
            # it and the conclusion was wrong.
            nz = [v for v in dy if v > 0.001]
            print(f"camstamp  the camera stepped between draw and submit in "
                  f"{100.0*len(nz)/n:.1f}% of frames; in the rest dyaw is "
                  f"exactly 0, so the MEDIAN is 0 by construction and the tail "
                  f"is the only usable statistic")
            if nz:
                nz.sort()
                print(f"camstamp  dyaw GIVEN a step: median "
                      f"{nz[len(nz)//2]:.3f} deg, p90 "
                      f"{nz[int(0.9*(len(nz)-1))]:.3f}")
            if gain:
                gm = gain[len(gain) // 2]
                gp = gain[int(0.9 * (len(gain) - 1))]
                print(f"camstamp  this run's gain error |cmd-X4| for scale: "
                      f"median {gm:.3f} deg, p90 {gp:.3f}")
                # TWO ratios, because one cannot say this. Over all frames the
                # gap is diluted by the majority in which the camera never
                # stepped, and that number says how atsubmit compares to the
                # command on average. Given a step it says how WRONG atsubmit is
                # when it is wrong at all. A single ratio collapses "better on
                # average" and "off by a full step a quarter of the time" into
                # one figure, and they want different fixes.
                r_all = dy[int(0.9 * (n - 1))] / max(gp, 1e-9)
                r_step = ((nz[int(0.9 * (len(nz) - 1))] / max(gp, 1e-9))
                          if nz else 0.0)
                frac = 100.0 * len(nz) / n
                print(f"camstamp  => p90 over ALL frames {r_all:.2f}x the gain "
                      f"error; p90 GIVEN a step {r_step:.2f}x")
                if r_all < 0.75:
                    print(f"camstamp  => declaring camera-at-submit BEATS "
                          f"declaring the command — it is exact in "
                          f"{100.0-frac:.0f}% of frames, where the command is "
                          f"wrong in nearly all of them. Take 278's fix was "
                          f"directionally right.")
                else:
                    print(f"camstamp  => declaring camera-at-submit does NOT "
                          f"beat the command; take 278 swapped one lead for "
                          f"another.")
                if r_step > 0.75:
                    print(f"camstamp  => but it is NOT clean: in {frac:.0f}% of "
                          f"frames the camera steps in between and atsubmit is "
                          f"off by a FULL camera step — the same size as the "
                          f"gain error it replaced. That is not a coincidence: "
                          f"the servo's command is what makes the camera step, "
                          f"so step size and gain error are one quantity seen "
                          f"at two points in the loop. Declare the ATDRAW "
                          f"sample; it removes this residual by construction.")
                else:
                    print(f"camstamp  => and it is clean: even given a step the "
                          f"gap stays under the gain error, so atdraw would buy "
                          f"little over atsubmit.")
            else:
                print("camstamp  no poselag gain error in this log to scale "
                      "against — the dyaw number above stands alone and the "
                      "comparison that decides #75 cannot be made")

    if re.search(r"\bX4VR_HEADLOOK_WRITE=1\b", run or ""):
        print("inputrate  (write mode: no mouse delta is sent by design — the "
              "servo-rate lines below count ticks of a servo this run did not "
              "use; read `hwrite` instead)")
    # --- task #75: do WE send sparsely, or does X4 ignore what we send? -----
    #
    # X4's camera changes on a near-constant ~43% of RENDERED frames across
    # takes 280/281/282 (42.9%, 41.7% at 88 fps; 44.6% at 58 fps), so it tracks
    # the frame loop rather than a fixed sim clock. `inputrate` counts, at full
    # rate, how often the servo actually emits a delta against how often X4's
    # camera then moves. The two hypotheses want opposite fixes.
    #
    # Read per WINDOW and described, never selected from. Four runs once
    # reported ~17.3 ms across a 35% knob change because a "calmest window"
    # selector kept finding the loading phase, so what is printed here is the
    # median with the spread and the window count beside it -- and the number of
    # windows that disagree with the verdict, so a verdict cannot hide a split.
    ir = re.findall(
        r"inputrate: (\d+) ticks in ([\d.]+) s = ([\d.]+) Hz \| delta sent on "
        r"([\d.]+)% \(=([\d.]+) Hz\) \| camera read on ([\d.]+)% of ticks, "
        r"moved on ([\d.]+)% of READS \(=([\d.]+) Hz\) \| clamped ([\d.]+)%",
        text)
    # Current shape, with `steering` as the send's denominator. Named groups: the
    # positional form of this block has already produced two wrong readings.
    ir3 = [m.groupdict() for m in re.finditer(
        r"inputrate: (?P<ticks>\d+) ticks in (?P<secs>[\d.]+) s = "
        r"(?P<hz>[\d.]+) Hz \| steering on (?P<steer>[\d.]+)% of ticks, delta "
        r"sent on (?P<sent>[\d.]+)% of STEERING ticks \(=(?P<senthz>[\d.]+) Hz\)"
        r" \| camera read on (?P<read>[\d.]+)% of ticks, moved on "
        r"(?P<cam>[\d.]+)% of READS \(=(?P<camhz>[\d.]+) Hz\) \| clamped "
        r"(?P<clamp>[\d.]+)%", text)]
    if ir3:
        def med(xs):
            s = sorted(xs)
            return s[len(s) // 2]
        # Only the STEERING windows can speak to whether X4 acts on what we
        # send — take 284's protocol had a 20 s still segment and a 21 s map
        # segment, and scoring those together produced "delta sent on 100.0%
        # against a camera moving on 13.1%" and the verdict that X4 was ignoring
        # us. Windows with no steering are counted and set aside, not averaged in.
        on = [g for g in ir3 if float(g["steer"]) > 50.0]
        off = len(ir3) - len(on)
        ticks = sum(int(g["ticks"]) for g in ir3)
        secs = sum(float(g["secs"]) for g in ir3)
        hzs = [float(g["hz"]) for g in ir3]
        print(f"inputrate  {len(ir3)} windows, {ticks} ticks over {secs:.0f} s | "
              f"tick {med(hzs):.1f} Hz median (range {min(hzs):.1f}-"
              f"{max(hzs):.1f}) | {off} window(s) had no steering and are set "
              f"aside")
        if on:
            cams = [float(g["cam"]) for g in on]
            sents = [float(g["sent"]) for g in on]
            reads = [float(g["read"]) for g in on]
            ticks_on = med([float(g["hz"]) for g in on])
            print(f"inputrate  steering windows ({len(on)}): delta sent on "
                  f"{med(sents):.1f}% of them, camera readable on "
                  f"{med(reads):.1f}%, camera moved on {med(cams):.1f}% of reads"
                  f" — i.e. {med(cams) / 100.0 * ticks_on:.1f} Hz against a "
                  f"{ticks_on:.1f} Hz tick")
            # Bimodal by construction: a window either has the camera moving or
            # it does not. Take 284's deciles were 0,0,0,0,6.5,51.8,77.2,92.7,
            # 98.3 and its median of 13.1% named the empty valley between the two
            # modes — a value no window actually had.
            lo = sum(1 for c in cams if c < 20.0)
            hi = sum(1 for c in cams if c > 60.0)
            if lo and hi:
                print(f"inputrate  NOTE: that distribution is BIMODAL — "
                      f"{lo} window(s) below 20% and {hi} above 60%. The median "
                      f"names the gap between them and describes neither; read "
                      f"the two modes, not the middle.")
            if hi:
                act = med([c for c in cams if c > 60.0])
                print(f"inputrate  => where the camera was actually being "
                      f"driven, X4 acted on {act:.1f}% of our ticks. So X4's "
                      f"camera rate tracks the TICK rate, and raising the tick "
                      f"raises it — which is what X4VR_TICK_HZ tests.")
    # The line changed shape when the window and the denominator were fixed. A
    # regex that silently matches nothing would read an older log as "the
    # instrument was not running", so the old shape is recognised and named.
    ir_old = re.findall(r"inputrate: (\d+) ticks in ([\d.]+) s = ([\d.]+) Hz "
                        r"\|.*X4 camera moved on ([\d.]+)%", text)
    if not ir and not ir3 and ir_old:
        print(f"inputrate  {len(ir_old)} lines, but in the PRE-FIX cumulative "
              f"format: totals since the first tick, and `cam_moved` divided by "
              f"all ticks rather than by reads. Last line says "
              f"{float(ir_old[-1][2]):.1f} Hz and {float(ir_old[-1][3]):.1f}% "
              f"camera movement — do not quote either: the first is diluted by "
              f"startup and the second conflates 'did not move' with 'could not "
              f"be read'. Difference consecutive lines by hand, or re-run.")
    if ir:
        def med(xs):
            s = sorted(xs)
            return s[len(s) // 2]
        hzs = [float(g[2]) for g in ir]
        sents = [float(g[3]) for g in ir]
        reads = [float(g[5]) for g in ir]
        cams = [float(g[6]) for g in ir]
        clamps = [float(g[8]) for g in ir]
        hz, sent_p, read_p, cam_p, clamp_p = (med(hzs), med(sents), med(reads),
                                              med(cams), med(clamps))
        sent_hz, cam_hz = med([float(g[4]) for g in ir]), med(
            [float(g[7]) for g in ir])
        ticks = sum(int(g[0]) for g in ir)
        secs = sum(float(g[1]) for g in ir)
        print(f"inputrate  {len(ir)} windows, {ticks} ticks over {secs:.0f} s | "
              f"median tick {hz:.1f} Hz (range {min(hzs):.1f}-{max(hzs):.1f}); "
              f"delta sent on {sent_p:.1f}% ({sent_hz:.1f} Hz); camera readable "
              f"on {read_p:.1f}% of ticks and moved on {cam_p:.1f}% of those "
              f"({cam_hz:.1f} Hz); clamped {clamp_p:.1f}%")
        if read_p < 95.0:
            print(f"inputrate  NOTE: the camera was only readable on "
                  f"{read_p:.1f}% of ticks, so the movement figure rests on a "
                  f"subset. Below ~50% treat it as unmeasured rather than low.")
        if sent_p < 60.0:
            print(f"inputrate  => OURS. The servo emits a delta on only "
                  f"{sent_p:.1f}% of ticks, so X4 cannot update more often than "
                  f"we ask it to. A count needs 0.23 deg of error while the "
                  f"dead zone sits at 0.15, and lround DISCARDS the residue — "
                  f"carrying it instead would emit on nearly every tick at the "
                  f"same average rate. This is fixable here.")
        elif cam_p < 0.75 * sent_p:
            print(f"inputrate  => X4's. We send on {sent_p:.1f}% of ticks and "
                  f"the camera moves on only {cam_p:.1f}%, so X4 is sampling "
                  f"the mouse at its own rate and dropping the rest. Not "
                  f"fixable from this side: the staleness has to be hidden "
                  f"rather than removed, which means #86-style work on the "
                  f"warp, not on the input.")
        else:
            print(f"inputrate  => the camera follows what we send "
                  f"({cam_p:.1f}% against {sent_p:.1f}%), so the rate is set by "
                  f"the TICK rate of {hz:.1f} Hz. If that is below the display "
                  f"rate the head-pose publish is the bottleneck, and it is "
                  f"ours.")
        # The 0.75 boundary sits at ~74% for a run that sends on ~99% of ticks,
        # and take 283 measured 73% acceptance -- one point from flipping the
        # verdict. A threshold that close to the data has to say so rather than
        # present either side as settled.
        if sent_p >= 60.0:
            margin = cam_p - 0.75 * sent_p
            if abs(margin) < 5.0:
                print(f"inputrate  NOTE: that verdict is {abs(margin):.1f} "
                      f"points from its own threshold ({cam_p:.1f}% against "
                      f"0.75x{sent_p:.1f}%={0.75 * sent_p:.1f}%), so it is not "
                      f"settled by this number alone. `pumprate` decides it "
                      f"without a threshold.")
        if hz < 60.0:
            print(f"inputrate  NOTE: the tick rate itself is only {hz:.1f} Hz. "
                  f"The servo cannot act faster than it is ticked, so this "
                  f"bounds everything above — see `pumprate` for which step "
                  f"loses them.")
        # A median verdict over a run that split in half is worth little, so the
        # split is reported rather than left for the median to absorb.
        dis = sum(1 for s in sents if (s < 60.0) != (sent_p < 60.0))
        if dis:
            print(f"inputrate  NOTE: {dis} of {len(ir)} windows fall on the "
                  f"OTHER side of the 60% send threshold, so the verdict above "
                  f"describes a median, not the whole run. Read the windows.")

    # --- task #75: where the published head poses go ------------------------
    #
    # The layer publishes one pose per submitted frame (89.6 Hz over take 283's
    # 208 s) and `inputrate` counts ~55 ticks/s, so about 39% are lost between
    # two components that are BOTH ours. `inputrate` cannot say which step,
    # because it counts ticks after two early returns; `pumprate` counts each
    # step at its own site, and prints the layer's own publish count on the same
    # line as the fresh count it should equal.
    #
    # The first measurement this answers is which hook drives. X4 calls
    # SDL_PumpEvents in every take since 118, and the SDL_PeepEvents tick site
    # is gated on !g_pump_seen, so that path has been dead code throughout --
    # which makes the "~18 kHz" in headlook_tick's comment a fact about a hook
    # that does not drive anything.
    # `tick=` is optional: takes 284 and earlier predate the driver, and a regex
    # that silently matched nothing would read those logs as having no instrument
    # at all rather than as having the drain-driven one.
    # NAMED groups, and dicts rather than tuples. This one line has twelve fields
    # and I have now mis-indexed it TWICE in one sitting — once shifting
    # fresh/nopose/skipped by one while reading the log by hand, once reading
    # `tick=` as `driver=` here, which silently disabled the whole driver verdict
    # while every other line looked right. A twelve-field positional tuple is a
    # defect waiting for its next reader.
    pr = [m.groupdict() for m in re.finditer(
        r"pumprate: (?P<dt>[\d.]+) s \| pump (?P<pump>[\d.]+) Hz peep "
        r"(?P<peep>[\d.]+) Hz wait (?P<wait>[\d.]+) Hz \| tick entered "
        r"(?P<ent>[\d.]+) Hz, nopose (?P<nopose>\d+), fresh (?P<fresh>[\d.]+) Hz,"
        r" skipped (?P<skip>[\d.]+) Hz \| layer published (?P<pub>[\d.]+) Hz \| "
        # OPTIONAL: present only once X4VR_HEADLOOK_UNGATED existed. Making it
        # mandatory would silently unscore every take up to 295 — the same
        # break the rfab control suite caught when `ugarm` went in strictly.
        r"(?:acted (?P<actedhz>[\d.]+) Hz \(ungated (?P<ung>\d+)\) \| )?"
        r"acted on (?P<acted>[\d.]+)% of published \| driver=(?P<driver>\w+)"
        r"(?: tick=(?P<tick>\S+))?", text)]
    if pr:
        def med(xs):
            s = sorted(xs)
            return s[len(s) // 2]
        # RATES FROM TOTALS, not medians of the components. `entered` must equal
        # `fresh + skipped`, and it does per window -- but the median of a sum is
        # not the sum of the medians, so printing three independent medians made
        # take 284's line read 60.5 = 51.7 + 23.0 and look self-inconsistent when
        # the data was exact. A statistic that cannot express an identity the
        # data satisfies is the wrong statistic.
        secs = sum(float(g["dt"]) for g in pr)

        def rate(k):
            return sum(float(g[k]) * float(g["dt"]) for g in pr) / secs
        pump, peep, wait = rate("pump"), rate("peep"), rate("wait")
        entered, fresh = rate("ent"), rate("fresh")
        skipped, pub = rate("skip"), rate("pub")
        nopose = sum(int(g["nopose"]) for g in pr)
        acted = 100.0 * fresh / pub if pub else 0.0
        driver = pr[-1]["driver"]
        pubs = [float(g["pub"]) for g in pr]
        freshes = [float(g["fresh"]) for g in pr]
        pumps = [float(g["pump"]) for g in pr]
        print(f"pumprate   {len(pr)} windows over {secs:.0f} s | driver="
              f"{driver} | pump {pump:.1f} Hz (range {min(pumps):.1f}-"
              f"{max(pumps):.1f}), peep {peep:.1f} Hz, wait {wait:.1f} Hz")
        print(f"pumprate   layer published {pub:.1f} Hz (range "
              f"{min(pubs):.1f}-{max(pubs):.1f}) -> tick entered {entered:.1f} "
              f"Hz -> fresh {fresh:.1f} Hz (range {min(freshes):.1f}-"
              f"{max(freshes):.1f}), skipped {skipped:.1f} Hz, nopose {nopose} "
              f"total => acted on {acted:.1f}% of published")
        # The decisive test is PER WINDOW, not on the aggregate. Take 284's
        # median pump (60.7) sat below its median published (90) and the verdict
        # said THE HOOK -- while in 30 of its windows pump ran at 103-142 Hz,
        # ABOVE the publish rate, with fresh unchanged at ~53. Those windows
        # refute "too few polls" on their own, and an aggregate hid them.
        # If polls were spread evenly, `fresh` would reach min(pump, published):
        # every publish interval would contain at least one poll. How far short
        # it falls measures how CLUSTERED the polls are, per window, with no
        # group comparison — two self-selected groups of windows differ in head
        # speed and frame rate as well as in poll rate, so a fast-vs-slow split
        # cannot attribute the difference to polling.
        ratio = sorted((float(g["fresh"]) / min(float(g["pump"]), float(g["pub"])),
                        float(g["pump"]), float(g["pub"]), float(g["fresh"]))
                       for g in pr
                       if min(float(g["pump"]), float(g["pub"])) > 1.0)
        if ratio:
            mid = ratio[len(ratio) // 2][0]
            # The window that polled hardest: does over-polling ever saturate?
            top = max(ratio, key=lambda r: r[1])
            print(f"pumprate   fresh / min(pump, published) = {mid:.2f} median "
                  f"(1.00 would mean every publish interval contained a poll). "
                  f"Hardest-polling window: {top[1]:.0f} Hz of polls against "
                  f"{top[2]:.0f} Hz of publishes caught {top[3]:.1f} Hz "
                  f"({top[3] / top[2]:.2f} of them), at {top[1] / max(top[3], 1e-9):.1f} "
                  f"polls per tick.")
            if top[3] >= 0.95 * top[2]:
                print(f"pumprate   => CLUSTERED POLLS, and over-polling "
                      f"saturates. The hardest-polling window caught EVERY "
                      f"publish, so nothing caps the tick below {top[2]:.0f} Hz "
                      f"— the deficit elsewhere is phase, not capacity: the "
                      f"polls arrive in bursts, so at the usual ~1.5x over-rate "
                      f"only ~{100 * mid:.0f}% of publish intervals contain "
                      f"one. Note this run reached that rate because OUR OWN "
                      f"injected events lengthen the drain loop, which is take "
                      f"119's runaway in miniature and not a lever to pull. "
                      f"REMOVE the freshness gate (X4VR_HEADLOOK_UNGATED) — do "
                      f"NOT move the tick off the drain.")
            # That last sentence used to read "the fix is to stop deriving the
            # tick from the drain at all", which is X4VR_TICK_HZ — the knob
            # x4vr_inject.cpp documents as HARMFUL, in the comment that opens
            # "that move is the defect". headlook_tick() is called from INSIDE
            # the SDL_PeepEvents interception, so the mouse event it pushes is
            # consumed by the very drain loop it was pushed into: 0.01 ms
            # residency on the drain (take 291) against 16.79 ms on a thread of
            # our own (take 292), and every session that ever went wild had the
            # knob set. Staying on the drain is load-bearing.
            #
            # The gate and the cadence are separate questions and this line had
            # merged them. Removing `if (!fresh) return;` keeps the tick where
            # it is and only stops it skipping iterations — 117 -> 53 ms of
            # servo lag in servo_rate.cpp section 12, with the 0.01 ms
            # residency intact.
            #
            # Left as a warning about the class: this advice was written before
            # take 292 measured the damage, survived every scoring run since,
            # and was still being read as current — it changed a handover
            # recommendation before anyone checked it against the code it was
            # recommending changes to.
        drv = pump if driver == "pump" else peep
        # Which cadence the tick ran on decides what every number above means.
        # With our own driver, `pump` is no longer the tick's supply and the
        # hook branches below would be answering a question the run retired.
        tickmode = pr[-1]["tick"] or "drain"
        asked = None
        m_tick = re.match(r"ours@(\d+)Hz", tickmode)
        if m_tick:
            asked = float(m_tick.group(1))
            print(f"pumprate   tick driver is OURS at {asked:.0f} Hz — `pump` is "
                  f"X4's drain rate and no longer the tick's supply.")
            if entered < 0.85 * asked:
                print(f"pumprate   => THE DRIVER DID NOT RUN at its rate: asked "
                      f"{asked:.0f} Hz, the tick was entered {entered:.1f} Hz. "
                      f"Read `tickdrv` before anything else — nothing below "
                      f"speaks to the fix until the cadence is real.")
            elif acted >= 95.0:
                print(f"pumprate   => FIXED. {acted:.1f}% of published poses "
                      f"reached the servo at {fresh:.1f} Hz against "
                      f"{pub:.1f} Hz published, up from take 284's 51.8% on the "
                      f"drain. Judge the jitter on the headset, and check "
                      f"`inputrate` for whether X4 still acts on ~0.7-1.0 per "
                      f"tick — if it does, X4's camera rate roughly doubled.")
            else:
                print(f"pumprate   => the driver ran ({entered:.1f} Hz) but only "
                      f"{acted:.1f}% of publishes produced a tick. At a cadence "
                      f"at or above the publish rate the freshness gate should "
                      f"catch nearly all of them, so this is the gate or the "
                      f"phase, not the rate: {skipped:.1f} Hz of ticks saw no "
                      f"new pose.")
        if pub <= 0.0:
            print("pumprate   => the layer published NOTHING in the median "
                  "window. Either head-look was not armed or the layer was not "
                  "loaded; nothing below this line means anything.")
        elif asked is not None:
            pass  # the driver branch above is the verdict; the hook ones are moot
        elif acted >= 90.0:
            print(f"pumprate   => NO loss here. {acted:.1f}% of published poses "
                  f"reached the servo, so the ~55 Hz tick of take 283 was the "
                  f"PUBLISH rate being low in those windows, not poses being "
                  f"dropped. The bottleneck is upstream of this instrument — "
                  f"the layer's submit rate.")
        elif drv < 0.9 * pub:
            print(f"pumprate   => THE HOOK. X4 calls {driver} only "
                  f"{drv:.1f} times/s against {pub:.1f} poses published, so "
                  f"several publishes collapse into one poll and the loss is "
                  f"structural: the servo cannot tick faster than X4 asks for "
                  f"events. Fixable on our side only by giving the tick a "
                  f"different driver than X4's event loop — and that is the one "
                  f"lever left that is entirely ours.")
        elif nopose > 0.2 * len(pr) * pub * 2.0:
            print(f"pumprate   => POSE VALIDITY. {driver} runs fast enough "
                  f"({drv:.1f} Hz against {pub:.1f} published) but "
                  f"head_share_read refused {nopose} times, which is the "
                  f"valid==0 or torn-read path. The loss is in the layer's pose, "
                  f"not in the tick rate.")
        else:
            # RESOLVED after take 294 -- it was neither of the two suspects.
            #
            # `fresh` is `updates != last_updates`, which is an INDICATOR, not a
            # count: when several publishes land between two polls they all
            # collapse into one `fresh`. So `fresh` counts POLL INTERVALS THAT
            # CONTAINED AT LEAST ONE PUBLISH, and comparing it to the publish
            # count as a percentage compares two different quantities. Clustered
            # polls make the gap arbitrarily large while nothing whatever is
            # going wrong.
            #
            # The honest reading is that the SERVO ITERATES at `fresh` Hz.
            # Corroborated independently in take 294: mean lagsample tick 19.9 ms
            # = 50.3 Hz against fresh 52.2 Hz, measured from timestamps that
            # share none of this arithmetic.
            #
            # Nothing is lost by the collapse -- each iteration reads the LATEST
            # pose, so the servo is never acting on stale data. What it costs is
            # loop iterations, and the lag of a proportional servo is
            # (1/kp - 1) * tick_period, so halving the iteration rate doubles
            # the lag. That is the defect, and it is a real one.
            # `acted` is logged from the branch the code took, so prefer it:
            # with X4VR_HEADLOOK_UNGATED on, `fresh` is no longer the iteration
            # count and this line would be wrong by 2x in the arm being measured.
            acted_hz = None
            if any(g.get("actedhz") for g in pr):
                tot = sum(float(g["actedhz"]) * float(g["dt"])
                          for g in pr if g.get("actedhz"))
                span = sum(float(g["dt"]) for g in pr if g.get("actedhz"))
                acted_hz = tot / span if span else None
            if acted_hz and abs(acted_hz - fresh) > 0.05 * max(fresh, 1.0):
                print(f"pumprate   => the servo ITERATES at {acted_hz:.1f} Hz, "
                      f"NOT the {fresh:.1f} Hz `fresh` reports — ungating is on, "
                      f"so stale poses are being acted on too. Read `ugab`.")
            else:
                print(f"pumprate   => the servo ITERATES at {fresh:.1f} Hz. "
                      f"{driver} runs at {drv:.1f} Hz and the layer publishes "
                      f"at {pub:.1f}, but `fresh` is an indicator, not a count: "
                      f"publishes arriving between two polls collapse into one. "
                      f"So the {acted:.1f}% is NOT lost poses — every iteration "
                      f"reads the latest pose — it is lost ITERATIONS.")
            print(f"pumprate   => lag of a proportional servo is "
                  f"(1/kp - 1) * tick_period, so the iteration rate is the "
                  f"lever: at {fresh:.1f} Hz that is "
                  f"{3000.0/fresh if fresh else float('nan'):.0f} ms at kp 0.25, "
                  f"against {3000.0/pub if pub else float('nan'):.0f} ms if "
                  f"every publish drove one iteration. Take 294 measured 85 ms "
                  f"end to end.")
        if driver == "peep":
            print(f"pumprate   NOTE: driver=peep means X4 never called "
                  f"SDL_PumpEvents in this run, which has not happened since "
                  f"take 118. Check the `sdl:` line before reading anything "
                  f"above.")
        # pump == peep to the count is a structural fact about X4's event loop,
        # and it is what finally retires the 18 kHz figure: a `while
        # (PollEvent(&e))` drain calls BOTH once per iteration, so the pair rate
        # is the iteration count and 18 kHz was take 119's own 3.2-million-event
        # flood making that loop spin. Our injected events still inflate it.
        eq = sum(1 for g in pr
                 if abs(float(g["pump"]) - float(g["peep"])) < 0.01)
        if eq > 0.9 * len(pr):
            print(f"pumprate   pump == peep in {eq}/{len(pr)} windows, to the "
                  f"count — X4 calls them as a PAIR, which is what a `while "
                  f"(SDL_PollEvent(&e))` drain does. So `pump` counts drain "
                  f"ITERATIONS, not event-loop passes, and it rises when our own "
                  f"synthetic mouse events lengthen the drain: it is partly our "
                  f"own signal and must not be read as X4's poll rate. The "
                  f"~18 kHz in x4vr_headlook.hpp was this loop spinning on take "
                  f"119's 3.2 million queued key events.")

    # --- task #75: what does X4 actually RECEIVE? ---------------------------
    #
    # A census, not a hypothesis. Two mechanisms have already been proposed and
    # refuted — the servo arithmetic (tests/headlook.cpp, all 8 cells stable)
    # and a stale off-thread camera read (take 286, worst window 1.83 ms) — so
    # this counts both ends of the channel rather than testing a third guess.
    #
    # `pushed` and `got` come from their own sites: "delayed" and "DROPPED" want
    # opposite fixes and one counter cannot separate them.
    # `%+lld` always prints a sign, so the in-flight count arrives as "+0" and a
    # `-?\d+` matched only the windows where it happened to go NEGATIVE: 2 lines
    # of 94 in take 287a, from which a "CLEAN" verdict was printed. A regex that
    # silently matches a biased subset is worse than one that matches nothing,
    # because nothing is visible as nothing. The count check below exists so this
    # class cannot recur silently — it has now cost two readings in this task.
    ef = re.findall(
        r"evflow: motion pushed ([\d.]+) Hz -> X4 took ([\d.]+) Hz \(([-+]?\d+) "
        r"in flight/lost\) \| key pushed ([\d.]+) Hz -> took ([\d.]+) Hz \| "
        r"queue time mean ([\d.]+) ms max ([\d.]+) \| player's own motion "
        r"([\d.]+) Hz \| ours per drain 0\.\.8\+: ([\d/]+) \| drains "
        r"([\d.]+) Hz", text)
    ef_raw = text.count("evflow: motion pushed")
    if ef_raw and len(ef) != ef_raw:
        print(f"evflow     PARSE DEFECT: {ef_raw} lines in the log, {len(ef)} "
              f"matched by the scorer. Whatever is printed below describes that "
              f"SUBSET, which is not the run. Fix the pattern before reading it.")
    if ef:
        def med(xs):
            s = sorted(xs)
            return s[len(s) // 2]
        pm = med([float(g[0]) for g in ef])
        gm = med([float(g[1]) for g in ef])
        qmean = med([float(g[5]) for g in ef])
        qmax = max(float(g[6]) for g in ef)
        foreign = med([float(g[7]) for g in ef])
        # The histograms are per-window counts; summed they describe the run.
        hist = [0] * 9
        for g in ef:
            for i, v in enumerate(g[8].split("/")[:9]):
                hist[i] += int(v)
        tot = sum(hist) or 1
        multi = sum(hist[2:])
        print(f"evflow     {len(ef)} windows | motion pushed {pm:.1f} Hz -> X4 "
              f"took {gm:.1f} Hz | queue time mean {qmean:.2f} ms, max "
              f"{qmax:.2f} | player's own motion {foreign:.1f} Hz")
        print(f"evflow     ours per drain: " +
              ", ".join(f"{i if i < 8 else '8+'}:{100.0 * hist[i] / tot:.1f}%"
                        for i in range(9) if hist[i]))
        if gm > 0 and qmean == 0.0 and qmax == 0.0:
            print(f"evflow     => THE TIMESTAMP DID NOT SURVIVE. X4 took "
                  f"{gm:.1f} Hz of our events but every queue time read zero, "
                  f"so X4's SDL is overwriting the field this measurement rides "
                  f"in — tests/sdl_push_timestamp.cpp passes against the system "
                  f"SDL 3.4.16, and X4 ships its own. The queue time is VOID "
                  f"here; the counts and the burst histogram still stand.")
        elif pm > 0 and gm < 0.8 * pm:
            print(f"evflow     => COMMANDS ARE BEING LOST. We push {pm:.1f} Hz "
                  f"and X4 takes only {gm:.1f} Hz — {100.0 * (1 - gm / pm):.0f}% "
                  f"never arrive. The servo's error never closes because most of "
                  f"what it sends is discarded, which is a different defect from "
                  f"delay and wants a different fix: stop pushing faster than "
                  f"the queue is drained.")
        elif multi > 0.25 * tot:
            print(f"evflow     => BACKLOG. {100.0 * multi / tot:.0f}% of drains "
                  f"hand X4 two or more of our commands at once, each emitted on "
                  f"a separate tick believing it would act alone. Mean queue "
                  f"time {qmean:.2f} ms, worst {qmax:.2f} ms — that is how long "
                  f"the servo steers without its last command having landed.")
        else:
            print(f"evflow     => CLEAN. X4 takes what we push "
                  f"({gm:.1f} of {pm:.1f} Hz), {100.0 * multi / tot:.0f}% of "
                  f"drains carry more than one, and the queue time is "
                  f"{qmean:.2f} ms. The channel is not the defect either, and "
                  f"with the servo and the camera read already cleared, nothing "
                  f"proposed so far explains take 285b.")
        # Only a competitor when there was something to compete WITH: take
        # 313 ran head-look off, pushed 0.0 Hz, and was a mouse take by
        # design -- and this told it to keep its hands off the mouse.
        if foreign > 1.0 and pm > 0.5:
            print(f"evflow     NOTE: {foreign:.1f} Hz of the player's OWN mouse "
                  f"motion crossed the same channel. Those compete with ours "
                  f"for X4's free-look, and the run should have had hands off "
                  f"the mouse.")

    # --- #46/#64: did the cursor draw, and did its position ever move? ------
    #
    # Every early return in the overlay's record() used to be a bare
    # `return false`, so takes 305 and 306 -- both of which armed the overlay
    # and never drew a thing -- produced byte-identical logs. The census names
    # the branch. Silent on older logs, which is the difference between "not
    # measured" and "measured zero".
    cc = re.findall(
        r"cursor census \((\w+)\): (\d+) present\(s\) — (.*?) \| pointer "
        r"x=(-?[\d.]+)\.\.(-?[\d.]+) y=(-?[\d.]+)\.\.(-?[\d.]+) against a "
        r"(\d+)x(\d+) eye \(([^)]*)\)", text)
    if cc:
        when, total, bins, xlo, xhi, ylo, yhi, ew, eh, moved = cc[-1]
        total, ew, eh = int(total), int(ew), int(eh)
        # A final census written at teardown, after the swapchain chains were
        # gone, read "against a 0x0 eye" (take 313) -- and every percentage
        # below divides by it. Borrow the extent from the last line that had
        # one and SAY so; the counts on the final line still stand.
        if not ew or not eh:
            known = [r for r in cc if int(r[7]) and int(r[8])]
            if known:
                ew, eh = int(known[-1][7]), int(known[-1][8])
                print(f"cursor     NOTE: the {when} census had no eye extent "
                      f"(written after teardown); using {ew}x{eh} from the "
                      f"last census that had one.")
        xlo, xhi = float(xlo), float(xhi)
        ylo, yhi = float(ylo), float(yhi)
        drew = 0
        reasons = {}
        for part in bins.split(" | "):
            bits = part.rsplit(" ", 1)
            if len(bits) == 2 and bits[1].isdigit():
                reasons[bits[0]] = int(bits[1])
        drew = reasons.get("drew", 0)
        print(f"cursor     {len(cc)} census line(s), last ({when}): {total} "
              f"present(s), drew {drew} "
              f"({100.0 * drew / total if total else 0.0:.1f}%) | {bins}")
        print(f"cursor     pointer x={xlo:.0f}..{xhi:.0f} y={ylo:.0f}..{yhi:.0f} "
              f"against a {ew}x{eh} eye")
        # Ordered by what has to be true first: a position that never moves
        # makes every placement question unanswerable, so it outranks a cull.
        if xhi <= xlo and yhi <= ylo:
            print(f"cursor     => THE POSITION CHANNEL IS DEAD. The pointer "
                  f"never left ({xlo:.0f},{ylo:.0f}). publish_cursor()'s only "
                  f"caller is our SDL_GetMouseState interposer and X4 does not "
                  f"call it — check for a 'sdl: GetMouseState extent' line, "
                  f"which logs on every new extreme and so appears on the first "
                  f"call. #46's placement CANNOT be read from this run.")
        elif drew == 0:
            worst = max((v, k) for k, v in reasons.items() if k != "drew")
            print(f"cursor     => NEVER DREW, and the reason is '{worst[1]}' "
                  f"({worst[0]} of {total}). That names the branch; it does not "
                  f"name the cause.")
        else:
            # The factor-2 prediction, committed in docs before this ran: the
            # pointer measured 0..1408 in BOTH axes against a 2816 eye. Stated
            # as a ratio so it reports whatever the number is, rather than
            # announcing the prediction regardless.
            rx = (xhi - xlo) / ew if ew else 0.0
            ry = (yhi - ylo) / eh if eh else 0.0
            if not ew or not eh:
                print(f"cursor     => DREW, but no census carried an eye "
                      f"extent, so the pointer range cannot be read as a "
                      f"fraction of anything.")
            else:
                print(f"cursor     => DREW. Pointer was SEEN across "
                      f"{100.0 * rx:.0f}% of the eye in x and "
                      f"{100.0 * ry:.0f}% in y.")
            if ew and eh and (rx < 0.75 or ry < 0.75):
                # "Was not seen there" is not "cannot go there". This range is
                # where the player moved the pointer, and take 308 -- a
                # cockpit-only run -- read 0..1408 in both axes purely because
                # X4 warps the pointer to the window centre (1408 = 2816/2)
                # for mouse-look and it never went anywhere else. Saying
                # "CANNOT REACH" from that would be inventing a limit out of a
                # protocol, which is how this project has already published a
                # confident negative from an input nobody supplied.
                print(f"cursor     => NOT SEEN OVER THE WHOLE EYE "
                      f"({100.0 * rx:.0f}%x{100.0 * ry:.0f}%). This is where "
                      f"the pointer WENT, not where it CAN go — only a take "
                      f"whose protocol drove it deliberately into all four "
                      f"corners can tell a real limit from an unswept edge. "
                      f"If it was swept: near 50% in BOTH axes is a "
                      f"logical-vs-physical factor of 2, near 50% in x ALONE "
                      f"is the SBS fold. Note 1408 = half of 2816 is also "
                      f"where X4 parks the pointer for mouse-look, so a "
                      f"cockpit-only run reaches it for an uninteresting "
                      f"reason.")
        # #46's actual quantity: where the cursor was DRAWN, after the canvas
        # map. The pointer range above is a proxy that takes 307-309 showed is
        # a bad one -- it reports the protocol, not the geometry.
        cd = re.findall(
            r"cursor drawn \((\w+)\) ARM (\w) \(canvas (\w+)\): ndc "
            r"x=(-?[\d.]+)\.\.(-?[\d.]+) y=(-?[\d.]+)\.\.(-?[\d.]+) = "
            r"(\d+)%x(\d+)% of the eye \| (\d+) frame\(s\) \| channel "
            r"says the window is (\d+) wide against a (\d+) eye"
            r"(?:[^|\n]*\| reachable x=(-?[\d.]+)\.\.(-?[\d.]+) "
            r"y=(-?[\d.]+)\.\.(-?[\d.]+))?", text)
        # The A/B schedule, stamped by the code that took the branch.
        ph = re.findall(r"cursor canvas: phase (\d+) — ARM (\w) \(canvas (\w+)\)",
                        text)
        if ph:
            arms = {}
            for _, a, _s in ph:
                arms[a] = arms.get(a, 0) + 1
            print(f"cursor     A/B ran {len(ph)} phase(s): " +
                  ", ".join(f"ARM {a} x{n}" for a, n in sorted(arms.items())))
            if len(arms) < 2:
                print(f"cursor     => THE A/B DID NOT ALTERNATE. Only ARM "
                      f"{list(arms)[0]} ever ran, so there is no comparison in "
                      f"this log — check X4VR_CURSOR_CANVAS_AB was set and the "
                      f"run was longer than one phase.")
        if cd:
            # Keep the LAST line per arm; the ranges are cumulative.
            per_arm = {}
            for rec in cd:
                per_arm[rec[1]] = rec
            for a in sorted(per_arm):
                (_, _, st, dxl, dxh, dyl, dyh, fx, fy, nfr, cw_, ce_,
                 rxl, rxh, ryl, ryh) = per_arm[a]
                print(f"cursor     ARM {a} (canvas {st}): drawn over ndc "
                      f"x={dxl}..{dxh} y={dyl}..{dyh} = {fx}%x{fy}% of the "
                      f"eye, {nfr} frame(s)" +
                      (f"; the map can reach x={rxl}..{rxh} y={ryl}..{ryh}"
                       if rxl else ""))
            if len(per_arm) == 2 and per_arm["A"][12] and per_arm["B"][12]:
                # The map's own boxes decide whether the arms differed. The
                # drawn extents below depend on where the pointer went, so a
                # run that never swept ARM B would have read "did not act".
                ra = float(per_arm["A"][13]) - float(per_arm["A"][12])
                rb = float(per_arm["B"][13]) - float(per_arm["B"][12])
                if abs(ra - rb) > 0.02:
                    print(f"cursor     => THE KNOB ACTED. The map lets ARM A "
                          f"reach {50 * ra:.0f}% of the eye's width and ARM B "
                          f"{50 * rb:.0f}%. Patola's report decides which arm "
                          f"is BETTER; this line only says the two arms were "
                          f"genuinely different.")
                else:
                    print(f"cursor     => THE KNOB DID NOT ACT. Both arms' "
                          f"maps reach {50 * ra:.0f}% of the width — whatever "
                          f"was observed is NOT the canvas map.")
            elif len(per_arm) == 2:
                fa = int(per_arm["A"][7])
                fb = int(per_arm["B"][7])
                if fb > fa + 10:
                    print(f"cursor     => THE KNOB ACTED. ARM B reaches "
                          f"{fb}% of the eye against ARM A's {fa}%, so the "
                          f"canvas map really was bypassed. Patola's report "
                          f"decides which arm is BETTER; this line only says "
                          f"the two arms were genuinely different.")
                else:
                    print(f"cursor     => THE KNOB DID NOT ACT. ARM A "
                          f"{fa}% vs ARM B {fb}% — the two arms drew over the "
                          f"same region, so whatever was observed is NOT the "
                          f"canvas map and nothing here is scorable.")
            (_, _, _, dxl, dxh, dyl, dyh, fx, fy, _, cw_, ce_,
             rxl, rxh, ryl, ryh) = cd[-1]
            fx, fy, cw_, ce_ = int(fx), int(fy), int(cw_), int(ce_)
            # Drawn against REACHABLE, per axis: the share of the box the map
            # allows that the cursor actually visited. Absent on logs before
            # the layer printed the box, and then nothing is claimed about it.
            have_reach = bool(rxl)
            if have_reach:
                spx = float(rxh) - float(rxl)
                spy = float(ryh) - float(ryl)
                cov_x = (float(dxh) - float(dxl)) / spx if spx > 0 else 0.0
                cov_y = (float(dyh) - float(dyl)) / spy if spy > 0 else 0.0
                reach_fx = round(50.0 * spx)
                reach_fy = round(50.0 * spy)
            # The single-arm CONFINED reading below describes ONE geometry. In
            # an A/B there are two and `cd[-1]` is merely whichever logged
            # last: take 311 printed "can only ever occupy 100%x67% — a border
            # of about 0%" off ARM B, which is meaningless as a statement about
            # the run. The per-arm lines above are the result.
            #
            # Gated on a FLAG, not by poisoning fx/fy: the first attempt set
            # them to -1 and the `fx < 90` branch duly reported "-1%x-1% ...
            # 100% of the AREA unreachable". Interpretation belongs inside a
            # branch on the number, and a sentinel is not a branch.
            single_arm = len(per_arm) < 2
            if cw_ and cw_ != ce_:
                print(f"cursor     => THE INPUT PATH AND THE IMAGE DISAGREE. "
                      f"The channel publishes against a {cw_}-wide window "
                      f"while the eye is {ce_}. The pointer and the picture "
                      f"are in different coordinate spaces, and every "
                      f"placement number here is measured across that seam — "
                      f"settle it before reading anything else.")
            elif single_arm and have_reach and (reach_fx < 90 or
                                                 reach_fy < 90):
                # The geometric statement, from the map's own edges. This is
                # what the old verdict claimed to be and was not: it read the
                # DRAWN extent, which depends on the protocol exactly as the
                # pointer range does (take 313: 41%x43% drawn, 52.5% reachable).
                print(f"cursor     => CONFINED BY THE MAP. The map can place "
                      f"the cursor over at most {reach_fx}%x{reach_fy}% of the "
                      f"eye, whatever the player does — "
                      f"{100 - reach_fx * reach_fy / 100.0:.0f}% of the AREA "
                      f"unreachable. That is the canvas affine, by design; the "
                      f"question is whether it SHOULD apply here.")
                if cov_x < 0.95 or cov_y < 0.95:
                    print(f"cursor     => and the pointer was not swept to the "
                          f"map's edge: it visited {100 * cov_x:.0f}% x "
                          f"{100 * cov_y:.0f}% of the reachable box. The drawn "
                          f"{fx}%x{fy}% is the PROTOCOL, not the limit.")
            elif single_arm and have_reach:
                print(f"cursor     => the map reaches {reach_fx}%x{reach_fy}% "
                      f"of the eye, so nothing is confining it; drawn "
                      f"{fx}%x{fy}% is where the pointer went.")
            elif single_arm and (fx < 90 or fy < 90):
                # Older log, no reachable box: the drawn extent is all there
                # is, and it depends on where the pointer went.
                print(f"cursor     => drawn over {fx}%x{fy}% of the eye. This "
                      f"log predates the reachable box, so it cannot say how "
                      f"much of that is the map and how much the protocol — "
                      f"the drawn extent depends on where the mouse went, "
                      f"exactly as the pointer range does.")
            elif single_arm:
                print(f"cursor     => the cursor reaches {fx}%x{fy}% of the "
                      f"eye, so nothing is confining it.")

        if reasons.get("off-eye"):
            print(f"cursor     NOTE: {reasons['off-eye']} frame(s) culled "
                  f"off-eye. That cull is computed from the UNMAPPED position "
                  f"while the draw uses the mapped one, and the canvas "
                  f"contracts — so some of these would have been on-eye.")

    # --- #46: did X4 ever set a cursor? Its shape is the hover signal -------
    #
    # The vanilla control established the signal exists and is unmissable:
    # unmodified X4 shows an arrow off a clickable element and a cross-in-circle
    # over one, equally at centre and edges. So a run where X4 never SETS a
    # cursor is a run where it never saw the pointer over anything.
    cz = re.search(r"cursor sets: 0 set\(s\) — X4 built (\d+) cursor\(s\)", text)
    cs = re.findall(
        r"cursor sets: (\d+) set\(s\) — handle found (\d+), NOT found (\d+), "
        r"null (\d+) \| (\d+) were a CHANGE \((\d+) while head-look was "
        r"STEERING, (\d+) while it was NOT; (\d+) in the COCKPIT, (\d+) "
        r"elsewhere(?P<unk> — VIEW NEVER STAMPED[^)]*)?\) \| (\d+) mouse "
        r"button", text)
    if cs:
        rec = cs[-1]
        view_unknown = bool(rec[9])
        (sets, found, missed, nul, chg, chg_st, chg_free, chg_cp,
         chg_el) = (int(v) for v in rec[:9])
        btn = int(rec[10])
        print(f"cursor-set {sets} set(s): handle found {found}, NOT found "
              f"{missed}, null {nul} | {chg} change(s) — {chg_st} steering / "
              f"{chg_free} free, {chg_cp} COCKPIT / {chg_el} elsewhere | "
              f"{btn} button press(es)")
        # Ordered by what has to be true first. A stale composite makes the
        # shape question unanswerable from the screen, so it outranks any
        # reading of whether the shape "changed".
        if missed:
            print(f"cursor-set => WE COMPOSITED A STALE IMAGE {missed} time(s). "
                  f"X4 set a cursor whose handle we never captured, so the "
                  f"drawn cursor kept the previous shape while X4's had "
                  f"changed. X4 imports only SDL_CreateColorCursor and "
                  f"SDL_SetCursor and both are hooked, so this is OURS — a "
                  f"capture that missed, not an unhooked API.")
        elif chg <= 1:
            hl_on = bool(re.search(r"\bX4VR_HEADLOOK=1\b", run or ""))
            print(f"cursor-set => X4 NEVER CHANGED THE CURSOR ({chg} change). "
                  f"It set one and kept it, so it never saw the pointer over "
                  f"anything clickable. " +
                  ("Head-look was ON, and takes 312/313 showed head-look ON "
                   "suppresses cockpit hover (313, head-look off: 107 "
                   "click-free changes) — so this is the expected reading, not "
                   "a placement result." if hl_on else
                   "Head-look was OFF, where takes 313/314 saw hover work — "
                   "so this is NOT the known suppression and needs its own "
                   "explanation."))
        elif view_unknown:
            print(f"cursor-set => THE VIEW WAS NEVER STAMPED, so the "
                  f"cockpit/elsewhere split is meaningless here — it reads "
                  f"{chg_cp}/{chg_el} only because the flag sat at its "
                  f"default. headlook_tick stamps it and returns early when "
                  f"X4VR_HEADLOOK=0. The {chg} change(s) and {btn} button "
                  f"press(es) still stand; WHERE they happened does not.")
        elif chg_cp == 0:
            print(f"cursor-set => NOT ONE CHANGE IN THE COCKPIT ({chg_el} of "
                  f"{chg} were elsewhere — map or menu). X4's shape is its "
                  f"only hover signal and the vanilla control proved it fires "
                  f"there, so in the cockpit X4 never saw the pointer over "
                  f"anything clickable. Changes elsewhere cannot answer the "
                  f"cockpit question and are not evidence for it.")
        elif (re.search(r"\bX4VR_HEADLOOK_WRITE=1\b", run or "") and
              chg_st > 0):
            # Gate on intent. "Steering" meant MOUSELOOK held until take 321;
            # in write mode it means the head's angle is being WRITTEN and the
            # mouse is never captured, so a change while steering is hover
            # working with head-look on -- the opposite of the branch below,
            # which take 321 printed on its 599 changes.
            print(f"cursor-set => X4 CHANGED THE CURSOR {chg_st} time(s) WHILE "
                  f"HEAD-LOOK WROTE THE CAMERA (of {chg}). Write mode holds no "
                  f"MOUSELOOK, so this is hover working with head-look on. "
                  f"WHERE the drawn cursor sat is the player's to say.")
        elif chg_free == 0:
            print(f"cursor-set => EVERY CHANGE CAME WHILE HEAD-LOOK WAS "
                  f"STEERING ({chg_st} of {chg}), and none while it was free. "
                  f"That is #71 restated — holding MOUSELOOK is how X4 "
                  f"suspends mouse control — and NOT a placement defect. The "
                  f"two want completely different work, so this run cannot "
                  f"choose between them: it needs free-look periods with the "
                  f"pointer over a target.")
        else:
            print(f"cursor-set => X4 DID CHANGE THE CURSOR {chg} time(s), and "
                  f"we published every one. So hover registered and the shape "
                  f"changed on screen. WHERE the drawn cursor sat when it "
                  f"changed is not in this log — only the player can say "
                  f"whether it was on the object.")
            if btn and chg <= btn + 1:
                print(f"cursor-set    CAUTION: {btn} button press(es) against "
                      f"{chg} change(s). Clicking changes the shape too, so "
                      f"these changes may ALL be clicks and none of them a "
                      f"hover. Not attributable from this run.")
        # Per A/B arm. The arm changes only where WE draw the cursor, never
        # what X4 hit-tests, so X4's change rate must be the same in both:
        # take 314 read 1.97 vs 1.89 changes/s. A large difference would mean
        # the knob reached X4's picking -- something besides the drawing moved.
        ph_t = [(float(t), a) for t, a in re.findall(
            r"\[\s*([\d.]+)\] layer   cursor canvas: phase \d+ — ARM (\w)",
            text)]
        cs_t = [(float(t), float(c)) for t, c in re.findall(
            r"\[\s*([\d.]+)\] inject  cursor sets: \d+ set\(s\).*?\| "
            r"(\d+) were a CHANGE", text)]
        if len({a for _, a in ph_t}) == 2 and len(cs_t) > 2:
            acc = {"A": [0.0, 0.0], "B": [0.0, 0.0]}
            for (t0, c0), (t1, c1) in zip(cs_t, cs_t[1:]):
                arm = None
                for tp, a in ph_t:
                    if tp <= t1:
                        arm = a
                if arm in acc:
                    acc[arm][0] += c1 - c0
                    acc[arm][1] += t1 - t0
            ra = acc["A"][0] / acc["A"][1] if acc["A"][1] else 0.0
            rb = acc["B"][0] / acc["B"][1] if acc["B"][1] else 0.0
            print(f"cursor-set per arm: ARM A {acc['A'][0]:.0f} change(s) in "
                  f"{acc['A'][1]:.0f} s ({ra:.2f}/s), ARM B {acc['B'][0]:.0f} "
                  f"in {acc['B'][1]:.0f} s ({rb:.2f}/s)")
            lo, hi = min(ra, rb), max(ra, rb)
            if hi > 0 and lo < 0.5 * hi:
                print(f"cursor-set => X4's HOVER RATE DEPENDS ON THE ARM "
                      f"({ra:.2f} vs {rb:.2f}/s). The arm only moves our "
                      f"drawing, so X4's picking should not see it — either "
                      f"the player hovered far less in one arm, or the knob "
                      f"reached something it should not.")
            elif hi > 0:
                print(f"cursor-set => same rate in both arms, as it must be: "
                      f"the arm moves only our drawing, not X4's picking.")
    elif cz:
        print(f"cursor-set 0 set(s) — X4 built {cz.group(1)} cursor(s) and set "
              f"none")
        print(f"cursor-set => X4 NEVER SET A CURSOR AT ALL. Its shape is its "
              f"only hover indication, so it never saw the pointer over "
              f"anything clickable. Printed rather than inferred from silence: "
              f"this is the most informative of the three outcomes and an "
              f"early return here would have looked like an old build.")

    # --- hover binned by X4's camera direction (X4VR_HOVER_VIEW) ----------
    #
    # Take 315's premise: does hover survive a camera turned WITHOUT
    # MOUSELOOK? The log can say that hover FIRED with the view turned; only
    # Patola can say whether the cross sat ON the object when it did.
    hv = re.findall(
        r"hover view: cursor changes with the view straight (\d+) \| turned "
        r"left/right \(\|yaw\|>=\d+\) (\d+) \| turned up/down "
        r"\(\|pitch\|>=\d+\) (\d+) \| in transit (\d+) \| view unknown "
        r"(\d+)", text)
    if hv:
        st, yw, pt, tr, unk = (int(v) for v in hv[-1])
        print(f"hoverview straight {st} | turned left/right {yw} | turned "
              f"up/down {pt} | in transit {tr} | unknown {unk}")
        for m in re.findall(r"milestone: (.+)", text):
            print(f"hoverview milestone: {m}")
        if yw + pt == 0:
            print("hoverview => NO HOVER WITH THE VIEW TURNED. Either the "
                  "numpad part was not done, or X4 does not hover while its "
                  "camera is turned without MOUSELOOK — which would be the "
                  "refutation of P315.2. Ask which before reading it.")
        else:
            print(f"hoverview => X4 HOVERED WITH ITS CAMERA TURNED "
                  f"({yw + pt} change(s)) and MOUSELOOK not held: picking runs "
                  f"in a turned camera. Whether the cross sat ON the object is "
                  f"Patola's to say, not the log's.")

    # --- our extensions (X4VR_EXTENSIONS) ---------------------------------
    #
    # Gate on intent: asked for and never served is a FAIL, because the take
    # would read as "with the extension" while X4 ran without it. The clamp
    # block above says whether X4's camera then went past 65/35.
    ext_asked = bool(re.search(r"\bX4VR_EXTENSIONS=1\b", run or ""))
    served = re.findall(r"content: serving (\S+) in memory via (\w+) — (.*?); "
                        r"your", text)
    if served:
        via = sorted({v for _, v, _w in served})
        print(f"extensions content.xml served from {served[-1][0]} via "
              f"{'/'.join(via)} — {served[-1][2]}")
        if any("COULD NOT" in w for _, _v, w in served):
            fails.append("an extension of ours COULD NOT be enabled in the "
                         "content.xml fork — see the 'content: serving' line")
    elif ext_asked:
        fails.append("X4VR_EXTENSIONS=1 but X4 never read content.xml through "
                     "the fork — our extension was probably NOT loaded, and "
                     "this run is not the 'with extension' configuration")

    # --- X4VR_HEADLOOK_WRITE: head-look WITHOUT MOUSELOOK -------------------
    #
    # The head's angle written into X4's free-look matrix (camera+0x5b0), with
    # x4vrmod_headlook holding X4's follower still. Gate on intent: asked for
    # and never armed, never written, or MOUSELOOK pressed even once is a FAIL.
    # The two questions the take exists for, each from its own channel:
    #   tracking -- does X4's camera follow the head? lagsample, full rate,
    #               x4 against head in the cockpit while steering;
    #   hover    -- does X4 now SET cursors while head-look steers? the cursor
    #               census, which MOUSELOOK held at zero in takes 312-320.
    hw_asked = bool(re.search(r"\bX4VR_HEADLOOK_WRITE=1\b", run or ""))
    hw_rep = re.findall(
        r"headlook write(?: \(final\))?: (\d+) write\(s\), (\d+) failed, "
        r"(\d+) refused tick\(s\) \| last written yaw (-?[\d.]+) pitch "
        r"(-?[\d.]+) \| MOUSELOOK pressed (\d+) time\(s\)", text)
    hw_armed = re.findall(
        r"headlook write: ARMED on camera (\S+) \(mode 0\) — X4 reads maxpitch "
        r"(\S+) maxyaw (\S+) \((\w+)\), anglespeed (\S+) angleacc (\S+);", text)
    hw_session = re.search(r"headlook write: REFUSED for the session — (.*)", text)
    hw_refusals = []
    for m in re.finditer(r"headlook write: REFUSED — (.*)", text):
        if m.group(1) not in hw_refusals:
            hw_refusals.append(m.group(1))
    if hw_asked or hw_rep or hw_armed or hw_session:
        if hw_session:
            fails.append("X4VR_HEADLOOK_WRITE refused for the whole session: "
                         + hw_session.group(1))
        for a in hw_armed[-2:]:
            print(f"hwrite  ARMED on camera {a[0]} — X4 reads maxpitch {a[1]} "
                  f"maxyaw {a[2]} ({a[3]}), anglespeed {a[4]}, angleacc {a[5]}")
        for r_ in hw_refusals[:6]:
            print(f"hwrite  refused (counted, said once): {r_[:240]}")
        if hw_rep:
            w_, f_, rf_, ly, lp, ml = hw_rep[-1]
            print(f"hwrite  {w_} write(s), {f_} failed, {rf_} refused tick(s) | "
                  f"last written yaw {ly} pitch {lp} | MOUSELOOK pressed {ml} "
                  f"time(s)")
            if int(ml) > 0:
                fails.append(f"write mode pressed MOUSELOOK {ml} time(s) — the "
                             f"one thing it exists not to do")
            if hw_asked and not hw_armed:
                fails.append("X4VR_HEADLOOK_WRITE=1 never ARMED — every tick "
                             "was refused; the hwrite lines say why")
            elif hw_asked and int(w_) == 0:
                fails.append("X4VR_HEADLOOK_WRITE=1 armed but wrote nothing")
        elif hw_asked:
            fails.append("X4VR_HEADLOOK_WRITE=1 asked, but no write report — "
                         "the mode never ran (head-look off, or no pose)")
        # The G-force sway, read off the controller live (since take 322).
        # Judged against intent: with x4vrmod_nosway served the sway must stay
        # at rest; without it, a large one is just X4 being X4.
        sw = re.findall(r"\| sway max [\d.]+ deg [\d.]+ m this window, "
                        r"([\d.]+) deg ([\d.]+) m run", text)
        nosway = bool(re.search(r"x4vrmod_nosway (already enabled|ENABLED)", text))
        if sw:
            sd, sm = float(sw[-1][0]), float(sw[-1][1])
            print(f"hwrite  sway (camera controller, live): run max {sd:.2f} "
                  f"deg, {sm * 100:.1f} cm"
                  + ("; x4vrmod_nosway served" if nosway else
                     "; x4vrmod_nosway NOT served"))
            if nosway and (sd > 1.0 or sm > 0.005):
                fails.append(f"x4vrmod_nosway was served but the camera still "
                             f"swayed {sd:.2f} deg / {sm * 100:.1f} cm — the "
                             f"diff did not reach the sway physics")
            elif nosway:
                print("hwrite  => sway OFF ✓ — the view moves only with the "
                      "head")
        elif hw_rep and nosway:
            print("hwrite  sway: no readout (this build predates it, or the "
                  "controller never verified)")
        if hw_asked:
            if not re.search(r"x4vrmod_headlook (already enabled|ENABLED)", text):
                fails.append("X4VR_HEADLOOK_WRITE=1 but the content fork never "
                             "served x4vrmod_headlook ENABLED — X4's follower "
                             "was not held, so writes would be pulled home")
            ls_ = re.findall(
                r"lagsample: arm=\d view=(\w+) steer=(\d) cam=(\d) "
                r"head=(-?[\d.]+),(-?[\d.]+) x4=(-?[\d.]+),(-?[\d.]+)", text)
            use = [(float(h), float(hp), float(x), float(xp))
                   for v, st, cm, h, hp, x, xp in ls_
                   if v == "cockpit" and st == "1" and cm == "1"
                   and abs(float(hp)) < 60.0]
            if use:
                dy = sorted(abs(wrap180(x - h)) for h, _hp, x, _xp in use)
                dp = sorted(abs(xp - hp) for _h, hp, _x, xp in use)
                n_ = len(dy)
                med, p90 = dy[n_ // 2], dy[int(0.9 * (n_ - 1))]
                print(f"hwrite  tracking: |X4 - head| yaw median {med:.2f} p90 "
                      f"{p90:.2f} deg, pitch median {dp[n_ // 2]:.2f} p90 "
                      f"{dp[int(0.9 * (n_ - 1))]:.2f} deg over {n_} cockpit "
                      f"tick(s) while writing (pitch under 60)")
                # **The tail, as episodes.** Take 321 read median 0.53 but
                # p90 10.74, and with the head STILL the p90 was 8.6: not
                # latency. 76 stretches of 5-17 s where X4 sat 8-20 deg past
                # the head, always in the head's direction -- the size of
                # take 318's G-force sway (19.3 max), arriving the first time
                # mouse steering was possible with the head turned. MOUSELOOK's
                # closed loop had cancelled it; a write does not. Printed so
                # the next take can be compared on the same terms.
                ts_ = re.findall(r"\[\s*([\d.]+)\]\s+inject\s+lagsample: arm=\d "
                                 r"view=cockpit steer=1 cam=1 head=(-?[\d.]+),"
                                 r"(-?[\d.]+) x4=(-?[\d.]+)", text)
                eps, cur = [], None
                for t_, h_, hp_, x_ in ts_:
                    if abs(float(hp_)) >= 60.0:
                        continue
                    e_ = wrap180(float(x_) - float(h_))
                    t_ = float(t_)
                    if abs(e_) > 5.0:
                        if cur and t_ - cur[1] < 0.3:
                            cur[1] = t_
                            cur[2] = max(cur[2], abs(e_))
                            cur[3] += (e_ > 0) == (float(h_) > 0)
                            cur[4] += 1
                        else:
                            if cur:
                                eps.append(cur)
                            cur = [t_, t_, abs(e_), int((e_ > 0) == (float(h_) > 0)), 1]
                if cur:
                    eps.append(cur)
                long_ = [e for e in eps if e[1] - e[0] >= 1.0]
                if long_:
                    tot = sum(e[1] - e[0] for e in long_)
                    same = sum(e[3] for e in long_) / max(1, sum(e[4] for e in long_))
                    print(f"hwrite  departures: {len(long_)} stretch(es) of 1 s+ "
                          f"with X4 more than 5 deg off the head, {tot:.0f} s in "
                          f"all, longest {max(e[1] - e[0] for e in long_):.1f} s, "
                          f"worst {max(e[2] for e in long_):.1f} deg; "
                          f"{100 * same:.0f}% of their samples off in the "
                          f"head's own direction")
                if med <= 2.0:
                    print("hwrite  => X4's camera FOLLOWS the written angle — "
                          "head-look without MOUSELOOK works")
                else:
                    fails.append(f"X4's camera does NOT follow the write: |X4 - "
                                 f"head| yaw median {med:.2f} deg — the follower "
                                 f"is not held, or the base is not identity")
            else:
                print("hwrite  tracking: no cockpit tick with the camera "
                      "readable while writing — not measured")
            cs_ = re.findall(r"cursor sets: (\d+) set\(s\) — handle found \d+, "
                             r"NOT found \d+, null \d+ \| (\d+) were a CHANGE "
                             r"\((\d+) while head-look was STEERING", text)
            cz_ = re.search(r"cursor sets: 0 set\(s\) — X4 built", text)
            if cs_:
                sets_, chg_, chg_st_ = cs_[-1]
                print(f"hwrite  hover: X4 set the cursor {sets_} time(s), "
                      f"{chg_} change(s), {chg_st_} while head-look steered"
                      + (" — MOUSELOOK no longer suppresses it"
                         if int(chg_st_) > 0 else
                         " — none while steering; ask Patola whether he hovered"))
            elif cz_:
                print("hwrite  hover: X4 NEVER set a cursor — with no MOUSELOOK "
                      "held that is not the 312-320 suppression; ask Patola "
                      "whether the cursor appeared at all")

    # --- #70: X4's base field switched live (X4VR_SHADOWFOV_AB) -------------
    #
    # FIX phases widen the camera's base field (camera+0x890, X4's 73.74 deg
    # default) to the rendered one; CONTROL phases restore X4's. The verdict on
    # the shadows is Patola's -- a switch within one run, announced by
    # notification. This block says whether the switch actually happened, and
    # what each phase cost in frame time (a wider base could make X4 draw more).
    sf_asked = bool(re.search(r"\bX4VR_SHADOWFOV_AB=\S+", run or ""))
    sf_armed = re.findall(r"shadowfov: ARMED on camera (\S+) — base (\S+) "
                          r"effective (\S+) \(rendered field (\S+) rad = "
                          r"(\S+) deg\)", text)
    sf_sess = re.search(r"shadowfov: REFUSED for the session — (.*)", text)
    sf_ref = []
    for m in re.finditer(r"shadowfov: REFUSED — (.*)", text):
        if m.group(1) not in sf_ref:
            sf_ref.append(m.group(1))
    sf_ph = [(float(t), int(n), k) for t, n, k in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+shadowfov: phase (\d+) (FIX|CONTROL) —",
        text)]
    sf_rep = re.findall(r"shadowfov: phase \d+ (?:FIX|CONTROL) \| base (\S+) "
                        r"effective (\S+) \| (\d+) base write\(s\), (\d+) "
                        r"effective restore\(s\), (\d+) failed", text)
    if sf_asked or sf_armed or sf_sess:
        if sf_sess:
            fails.append("X4VR_SHADOWFOV_AB refused for the session: "
                         + sf_sess.group(1))
        for a in sf_armed[:3]:
            print(f"shadowfov ARMED on camera {a[0]} — base {a[1]} effective "
                  f"{a[2]}, rendered field {a[3]} rad ({a[4]} deg)")
        for r_ in sf_ref[:4]:
            print(f"shadowfov refused (said once): {r_[:220]}")
        if sf_asked and not sf_armed:
            fails.append("X4VR_SHADOWFOV_AB asked but never ARMED — nothing "
                         "was switched; the refusal lines say why")
        nfix = sum(1 for _t, _n, k in sf_ph if k == "FIX")
        print(f"shadowfov {len(sf_ph)} phase(s), {nfix} FIX / "
              f"{len(sf_ph) - nfix} CONTROL")
        if sf_rep:
            b_, e_, bw, rs, fl = sf_rep[-1]
            print(f"shadowfov last: base {b_} effective {e_} | {bw} base "
                  f"write(s), {rs} effective restore(s), {fl} failed")
            if int(fl):
                fails.append(f"shadowfov: {fl} write(s) into the camera failed")
            if int(rs):
                print(f"shadowfov NOTE: X4 recomputed the effective field "
                      f"{rs} time(s) (option x a widened base) and it was "
                      f"put back — a camera re-init during a FIX phase")
        # **Did each phase actually hold its own value?** Take 322 printed
        # "8 FIX / 8 CONTROL" while every CONTROL phase after the first read
        # the WIDENED base: X4 re-asserted the written value each frame and
        # the CONTROL write (counted ~60/s) never stuck. Phases that never
        # differed compared FIX with FIX, and this block called it healthy.
        per = re.findall(r"shadowfov: phase \d+ (FIX|CONTROL) \| base (\S+) "
                         r"effective", text)
        if sf_armed and per:
            want_fix = float(sf_armed[0][3])
            want_ctl = 1.28700
            bad = [(k, float(b)) for k, b in per
                   if abs(float(b) - (want_fix if k == "FIX" else want_ctl)) > 1e-4]
            if bad:
                kinds = sorted({k for k, _b in bad})
                fails.append(
                    f"shadowfov: {len(bad)} of {len(per)} phase report(s) did "
                    f"NOT read their own phase's base ({'/'.join(kinds)} read "
                    f"{bad[0][1]:.5f}) — the arms collapsed, X4 kept the other "
                    f"value, and this run did not compare what it claims to")
        if sf_asked and sf_armed and nfix == 0:
            fails.append("X4VR_SHADOWFOV_AB armed but the run ended before "
                         "the first FIX phase — nothing was compared")
        # Frame time by phase: the layer's `perf frame` windows, credited to
        # the phase their timestamp falls in.
        pf = [(float(t), float(ms)) for t, ms in re.findall(
            r"\[\s*([\d.]+)\]\s+layer\s+perf frame \d+: median ([\d.]+) ms",
            text)]
        if sf_ph and pf:
            by = {"FIX": [], "CONTROL": []}
            for t, ms in pf:
                cur = [k for t0, _n, k in sf_ph if t0 <= t]
                if cur:
                    by[cur[-1]].append(ms)
            for k in ("CONTROL", "FIX"):
                v = sorted(by[k])
                if v:
                    print(f"shadowfov perf {k:<7} median {v[len(v) // 2]:.2f} "
                          f"ms over {len(v)} window(s)")

    # --- #70: the rendered field switched live (X4VR_FIELD_AB), and camuj ----
    #
    # Phase A renders at X4VR_FOV, phase B at X4VR_FIELD_B (flatscreen). Take
    # 322's lesson: a phase is judged by what X4 DREW, not by what was written
    # -- the layer's `camuj` lines carry the main view's projection sx, which
    # must read the phase's own value, or the arms did not differ. Then the
    # question the take exists for: does the shadow lookup's inverse
    # (M_invprojection_uj) invert the projection drawn, in each phase?
    fa_asked = bool(re.search(r"\bX4VR_FIELD_AB=\S+", run or ""))
    fa_arm = re.search(r"fieldab: ARMED on camera \S+ — A (\S+) \(\S+ rad, "
                       r"(\S+) deg, sx (\S+)\), B (\S+) \(\S+ rad, (\S+) deg, "
                       r"sx (\S+)\)", text)
    fa_sess = re.search(r"fieldab: REFUSED for the session — (.*)", text)
    fa_ref = []
    for m in re.finditer(r"fieldab: REFUSED — (.*)", text):
        if m.group(1) not in fa_ref:
            fa_ref.append(m.group(1))
    fa_start = re.search(r"\[\s*([\d.]+)\]\s+inject\s+fieldab: STARTED — "
                         r"game time ([\d.]+)", text)
    fa_ph = [(float(t), int(n), k) for t, n, k in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+fieldab: phase (\d+) (A|B) —", text)]
    fa_rep = re.findall(r"fieldab: phase \d+ [AB] \| effective \S+ \| held A "
                        r"(\d+)/(\d+) B (\d+)/(\d+) tick\(s\) \| (\d+) write\(s\), "
                        r"(\d+) reverted by X4, (\d+) failed", text)
    uj = [(float(t), int(dr), float(sx), float(ux), float(e1), float(e2),
           float(e3), rest) for t, dr, sx, ux, e1, e2, e3, rest in re.findall(
        r"\[\s*([\d.]+)\]\s+layer\s+camuj: main cam@\d+ draws=(\d+) proj sx "
        r"(-?[\d.]+) sy \S+ \| proj_uj sx (-?[\d.]+) sy \S+ \| inv\*proj (\S+), "
        r"inv_uj\*proj_uj (\S+), inv_uj\*proj (\S+) \| (.*)", text)]
    if fa_asked or fa_arm or fa_sess:
        if fa_sess:
            fails.append("X4VR_FIELD_AB refused for the session: " + fa_sess.group(1))
        if fa_arm:
            print(f"fieldab ARMED — A {fa_arm.group(1)} ({fa_arm.group(2)} deg, "
                  f"sx {fa_arm.group(3)}), B {fa_arm.group(4)} "
                  f"({fa_arm.group(5)} deg, sx {fa_arm.group(6)})")
        for r_ in fa_ref[:4]:
            print(f"fieldab refused (said once): {r_[:220]}")
        gtr = re.findall(r"\[\s*([\d.]+)\]\s+inject\s+gametime: (RUNNING|stopped)",
                         text)
        print(f"fieldab game clock: {sum(1 for _t, k in gtr if k == 'RUNNING')} "
              f"start(s), {sum(1 for _t, k in gtr if k == 'stopped')} stop(s)"
              + (f"; phases STARTED at game time {fa_start.group(2)} s"
                 if fa_start else "; phases NEVER started"))
        if fa_asked and not fa_start:
            fails.append("X4VR_FIELD_AB asked, but the phases never started — "
                         "game time never ran with the cockpit camera up (see "
                         "the gametime lines)")
        nb = sum(1 for _t, _n, k in fa_ph if k == "B")
        print(f"fieldab {len(fa_ph)} phase(s), {len(fa_ph) - nb} A / {nb} B")
        if fa_rep:
            ha, ta, hb, tb, wr, rv, fl = (int(x) for x in fa_rep[-1])
            print(f"fieldab held A {ha}/{ta}, B {hb}/{tb} tick(s) | {wr} "
                  f"write(s), {rv} reverted by X4, {fl} failed")
            for nm, h, t in (("A", ha, ta), ("B", hb, tb)):
                if t >= 50 and h < 0.9 * t:
                    fails.append(f"fieldab: phase {nm} held its field on only "
                                 f"{100 * h / t:.0f}% of ticks — X4 kept "
                                 f"putting its own back")
            if fl:
                fails.append(f"fieldab: {fl} write(s) into the camera failed")
        # Per phase, from the layer: what X4 drew, and whether the shadows'
        # inverse inverts it. Judged over the camera blocks drawn, NOT the
        # "main cam" (the most-drawn block): in take 323 that block sat at X4's
        # default 73.74 deg (sx 1.33333) in both phases while ~30 blocks per
        # reading followed the switch, and the scorer called the switch failed
        # against what Patola saw. The census planted here had assumed the
        # most-drawn block was the picture.
        if fa_arm and fa_ph and uj:
            want = {"A": float(fa_arm.group(3)), "B": float(fa_arm.group(6))}
            by = {"A": [], "B": []}
            for row in uj:
                cur = [k for t0, _n, k in fa_ph if t0 <= row[0] - 1.0]
                if cur:
                    by[cur[-1]].append(row)
            steady = {}
            for k in ("A", "B"):
                rows = by[k]
                if not rows:
                    print(f"fieldab {k}: no camuj line inside a {k} phase")
                    if fa_asked:
                        fails.append(f"fieldab: no camuj reading inside any "
                                     f"{k} phase — X4VR_CAMDIFF=1 missing?")
                    continue
                o = "B" if k == "A" else "A"
                own = other = own_bad = 0
                worst = 0.0
                per_line = []
                for row in rows:
                    vals = set()
                    for a_, b_, n_, miss_, w_ in re.findall(
                            r"(-?[\d.]+)/(-?[\d.]+) x(\d+) \(inv_uj miss "
                            r"(\d+), worst ([\d.e+-]+)\)", row[7]):
                        a_, b_ = float(a_), float(b_)
                        vals.add(a_)
                        if abs(a_ - want[k]) < 2e-3:
                            own += int(n_)
                            if (abs(b_ - a_) > 0.01 * abs(a_)
                                    or int(miss_)):
                                own_bad += max(int(miss_), 1)
                            worst = max(worst, float(w_))
                        elif abs(a_ - want[o]) < 2e-3:
                            other += int(n_)
                    per_line.append(vals)
                msx = sorted(r_[2] for r_ in rows)[len(rows) // 2]
                print(f"fieldab {k}: X4 DREW with sx {want[k]:.5f} in {own} "
                      f"block reading(s), the other phase's {want[o]:.5f} in "
                      f"{other}, over {len(rows)} camuj line(s) (the most-drawn "
                      f"block read sx {msx:.5f} — context, not the picture)"
                      if own else
                      f"fieldab {k}: NO block drew the phase's sx "
                      f"{want[k]:.5f} ({other} at the other phase's "
                      f"{want[o]:.5f}) over {len(rows)} camuj line(s)")
                if not own or other > 0.05 * own:
                    fails.append(f"fieldab: in phase {k} {own} block(s) drew "
                                 f"the phase's sx {want[k]:.5f} and {other} "
                                 f"the other phase's — the rendered field did "
                                 f"not follow the switch, so the phases did "
                                 f"not differ")
                elif own_bad:
                    print(f"fieldab {k}: => MISMATCH — {own_bad} of the "
                          f"{own} block(s) drawn at sx {want[k]:.5f} carry a "
                          f"shadow-lookup inverse that does not invert their "
                          f"projection (worst {worst:.1e}): #70's matrix, "
                          f"fixable by giving the shader the drawn one")
                else:
                    print(f"fieldab {k}: => every block drawn at sx "
                          f"{want[k]:.5f} carries a shadow-lookup inverse that "
                          f"inverts it (worst {worst:.1e}) — not a "
                          f"reconstruction error in this phase. Range: each "
                          f"block is checked against ITSELF; a pass reading "
                          f"another block's inverse would not show here")
                # Values present in at least half the phase's readings,
                # clustered at 5e-4 relative (0.47213 and 0.47216 are one).
                cl = []
                for v in sorted({v for vs in per_line for v in vs}):
                    if cl and abs(v - cl[-1][0]) <= 5e-4 * max(abs(v), 1e-9):
                        cl[-1][1].add(v)
                    else:
                        cl.append((v, {v}))
                steady[k] = [c0 for c0, mem in cl
                             if sum(1 for vs in per_line if vs & mem)
                             >= 0.5 * len(per_line)]
            if "A" in steady and "B" in steady:
                def near(v, xs):
                    return any(abs(v - x) <= 5e-4 * max(abs(v), 1e-9)
                               for x in xs)
                ma = sorted((v for v in steady["A"] if not near(v, steady["B"])
                             and abs(v - want["A"]) >= 2e-3), reverse=True)
                mb = sorted((v for v in steady["B"] if not near(v, steady["A"])
                             and abs(v - want["B"]) >= 2e-3), reverse=True)
                if ma or mb:
                    print(f"fieldab other blocks that MOVED with the field: "
                          f"A {' '.join(f'{v:.5f}' for v in ma) or '-'} / B "
                          f"{' '.join(f'{v:.5f}' for v in mb) or '-'}")
                    if len(ma) == len(mb) and ma:
                        print("fieldab   as 2/sx (a width, IF orthographic): "
                              f"A {' '.join(f'{2 / v:.4g}' for v in ma)} / B "
                              f"{' '.join(f'{2 / v:.4g}' for v in mb)}; "
                              f"A/B sx "
                              f"{' '.join(f'{a_ / b_:.3f}' for a_, b_ in zip(ma, mb))}")
                else:
                    print("fieldab no other block moved with the field")
    if uj:
        # Every run with camuj (X4VR_CAMDIFF=1): the slot pairs that disagree.
        bad_pairs = set()
        for row in uj:
            for a_, b_ in re.findall(r"(-?[\d.]+)/(-?[\d.]+) x\d+", row[7]):
                if abs(float(a_) - float(b_)) > 0.01 * max(abs(float(a_)), 1e-6):
                    bad_pairs.add((a_, b_))
        print(f"camuj   {len(uj)} reading(s); most-drawn block proj sx "
              f"{sorted(r_[2] for r_ in uj)[len(uj) // 2]:.5f}, proj_uj sx "
              f"{sorted(r_[3] for r_ in uj)[len(uj) // 2]:.5f}"
              + (f"; slot pair(s) whose projection and unjittered projection "
                 f"DISAGREE: {', '.join(f'{a_}/{b_}' for a_, b_ in sorted(bad_pairs))}"
                 if bad_pairs else "; every slot's two projections agree"))

    # --- #70's fix: X4's cascade fit given its own divisors (X4VR_CSM_COVER) --
    #
    # The injector retargets the fit's four reads of X4's aspect globals at a
    # private pair and logs, every 5 s, the cascade-0 sx X4's formula predicts
    # for the arm in force. The layer's camuj lines carry what X4 DREW: cascade
    # 0 is the largest sx among the orthographic blocks, which are the ones
    # whose shadow inverse is never maintained (inv_uj miss > 0; the picture's
    # blocks read miss 0). An arm is judged by that, not by what was written.
    cf_asked = bool(re.search(r"\bX4VR_CSM_COVER=\S+", run or ""))
    cf_patched = re.search(r"csmfit: PATCHED — .*?coverage ([\d.]+)", text)
    cf_refused = re.search(r"csmfit: REFUSED — (.*?)\. X4's", text)
    cf_ab = bool(re.search(r"\bX4VR_CSM_AB=[1-9]", run or ""))
    if cf_asked or cf_patched or cf_refused:
        if cf_refused:
            fails.append("X4VR_CSM_COVER refused: " + cf_refused.group(1))
            print(f"csmfit REFUSED — {cf_refused.group(1)}")
        elif not cf_patched:
            fails.append("X4VR_CSM_COVER asked, but no PATCHED line — the "
                         "injector never touched the fit")
            print("csmfit NOT PATCHED")
        else:
            print(f"csmfit PATCHED — the fit's 4 reads retargeted, coverage "
                  f"{cf_patched.group(1)}"
                  + (", CONTROL/FIX phases" if cf_ab else ", FIX always"))
        cf_start = re.search(r"\[\s*([\d.]+)\]\s+inject\s+csmfit: STARTED", text)
        cf_ph = [(float(t), int(n), k) for t, n, k in re.findall(
            r"\[\s*([\d.]+)\]\s+inject\s+csmfit: phase (\d+) (FIX|CONTROL) —",
            text)]
        cf_rep = [(float(t), k, float(cov), float(psx)) for t, k, cov, psx in
                  re.findall(r"\[\s*([\d.]+)\]\s+inject\s+csmfit: phase -?\d+ "
                             r"(FIX|CONTROL) \| .*?\| coverage ([\d.]+) \| "
                             r"predicted cascade-0 sx ([\d.]+)", text)]
        if cf_ab:
            nf = sum(1 for _t, _n, k in cf_ph if k == "FIX")
            print(f"csmfit {len(cf_ph)} phase(s), {len(cf_ph) - nf} CONTROL / "
                  f"{nf} FIX" + ("" if cf_start else "; phases NEVER started"))
            if cf_patched and not cf_start:
                fails.append("X4VR_CSM_AB asked, but the phases never started "
                             "— game time never ran with the cockpit camera up")

        def arm_at(t):
            if not cf_ab:
                return "FIX" if cf_patched else None
            cur = [k for t0, _n, k in cf_ph if t0 <= t - 1.0]
            return cur[-1] if cur else None

        def cascade0(rest):
            c = [float(a_) for a_, _b, _n, miss in re.findall(
                r"(-?[\d.]+)/(-?[\d.]+) x(\d+) \(inv_uj miss (\d+)", rest)
                 if int(miss) > 0 and 0.0 < float(a_) < 0.6]
            return max(c) if c else None

        # Each camuj reading is judged against the injector's own report just
        # before it (within 6 s): the arm in force and the cascade 0 X4's
        # formula predicts for the field in force. A report with no field
        # (menus and loading have no player camera, so the fit runs on X4's
        # own values) predicts nothing, and readings under it are not judged.
        # In the FIX-always form those readings are X4's own boxes BY DESIGN;
        # judged against the run's median FIX prediction they would have
        # been counted as misses (take 326 is that form's first run).
        reps = sorted(cf_rep)
        rep_t = [r_[0] for r_ in reps]
        judged = {"CONTROL": [], "FIX": []}
        unjudged = absent = 0
        for row in uj:
            v = cascade0(row[7])
            if not v:
                continue
            k_ = bisect.bisect_right(rep_t, row[0]) - 1
            if k_ < 0 or row[0] - rep_t[k_] > 6.0 or reps[k_][3] <= 0:
                unjudged += 1
                continue
            _t, arm, cov, pred = reps[k_]
            if cf_ab and arm_at(row[0]) != arm:
                unjudged += 1  # a phase switch between report and reading
                continue
            if v < 0.5 * pred:
                # Cascade 0 was not drawn this frame (take 326: 12 frames held
                # only cascades 3-4, or 1-4), so the largest block listed is a
                # far cascade. A fit that ignored the divisors draws cascade 0
                # LARGER than the prediction (X4's own 0.472 against 0.230),
                # never smaller, so setting these aside cannot hide a failure.
                absent += 1
                continue
            judged[arm].append((v, pred, cov))
        for arm in ("CONTROL", "FIX"):
            rows = judged[arm]
            expected = cf_patched and (cf_ab or arm == "FIX")
            if not rows:
                if expected and (cf_start or not cf_ab):
                    print(f"csmfit {arm}: no camuj reading under a {arm} report")
                    fails.append(f"csmfit: no camuj reading under a {arm} "
                                 f"report — X4VR_CAMDIFF=1 missing?")
                continue
            ms = sorted(v for v, _p, _c in rows)
            ps = sorted(p for _v, p, _c in rows)
            cs = sorted(c for _v, _p, c in rows)
            mmed, pmed, cmed = (x[len(x) // 2] for x in (ms, ps, cs))
            hit = sum(1 for v, p, _c in rows if abs(v - p) <= 0.01 * p)
            print(f"csmfit {arm}: X4 drew cascade 0 at sx {mmed:.5f} (median "
                  f"of {len(rows)} camuj reading(s)), its arm predicts "
                  f"{pmed:.5f} at coverage {cmed:.3f}; {hit}/{len(rows)} "
                  f"within 1% of the report before them")
            if hit < 0.5 * len(rows):
                fails.append(f"csmfit: in {arm} X4 drew cascade 0 at sx "
                             f"{mmed:.5f}, not the arm's {pmed:.5f} — the fit "
                             f"did not follow the divisors"
                             + (", so the arms did not differ" if cf_ab else ""))
        if absent:
            print(f"csmfit {absent} reading(s) without cascade 0 (X4 drew only "
                  f"far cascades that frame) — set aside")
        if unjudged:
            print(f"csmfit {unjudged} reading(s) not judged — no field in "
                  f"force (menus, loading: X4's own sizing by design) or a "
                  f"phase switch between report and reading")
        # What each arm costs: bigger boxes take in more shadow casters.
        cf_pf = [(float(t), float(ms)) for t, ms in re.findall(
            r"\[\s*([\d.]+)\]\s+layer\s+perf frame \d+: median ([\d.]+) ms",
            text)]
        if cf_ab and cf_pf:
            by = {"CONTROL": [], "FIX": []}
            for t, ms in cf_pf:
                a_ = arm_at(t)
                if a_ in by:
                    by[a_].append(ms)
            if by["CONTROL"] and by["FIX"]:
                mc = sorted(by["CONTROL"])[len(by["CONTROL"]) // 2]
                mf = sorted(by["FIX"])[len(by["FIX"]) // 2]
                print(f"csmfit frame time: CONTROL {mc:.2f} ms "
                      f"({len(by['CONTROL'])} window(s)), FIX {mf:.2f} ms "
                      f"({len(by['FIX'])}) — FIX {100 * (mf - mc) / mc:+.1f}%")

    # --- X4VR_CURSOR_CANVAS=auto: the cursor through the canvas in menus ----
    #
    # Take 326's map menu closed under a cursor drawn at the raw pointer; take
    # 311 had shown full-screen menus need the canvas map. The injector polls
    # X4's IsFullscreenMenuDisplayed and logs each change; the layer logs each
    # change of the mapping it actually used. Both ends are checked: a menu the
    # injector saw that never reached the layer's choice is the switch failing.
    ca_asked = bool(re.search(r"\bX4VR_CURSOR_CANVAS=auto\b", run or ""))
    ui_up = re.findall(r"\[\s*([\d.]+)\]\s+inject\s+ui: full-screen menu "
                       r"(UP|DOWN)", text)
    # The layer's ON names its reason since take 339: a full-screen menu, an
    # on-foot menu holding the pointer, or (take 340) a menu with the HUD
    # down that head-look centred. Only the first answers `ui_up`.
    ca_all = re.findall(r"\[\s*([\d.]+)\]\s+layer\s+cursor canvas: AUTO — "
                        r"(ON|OFF)(?: \((an on-foot menu|a menu with the HUD "
                        r"down|X4's right-click menu))?", text)
    ca_sw = [(t, k) for t, k, why in ca_all if not why]
    ca_foot_on = [float(t) for t, k, why in ca_all if why == "an on-foot menu"]
    ca_cen_on = [float(t) for t, k, why in ca_all if why.startswith("a menu")]
    if ca_asked or ca_sw:
        if "IsFullscreenMenuDisplayed NOT FOUND" in text:
            fails.append("X4VR_CURSOR_CANVAS=auto: X4 does not export "
                         "IsFullscreenMenuDisplayed — the cursor never left "
                         "the raw pointer")
        n_up = sum(1 for _t, k in ui_up if k == "UP")
        n_on = sum(1 for _t, k in ca_sw if k == "ON")
        print(f"cursor-auto  X4 reported a full-screen menu {n_up} time(s); "
              f"the layer switched the cursor to the canvas {n_on} time(s), "
              f"back to the raw pointer "
              f"{sum(1 for _t, k in ca_sw if k == 'OFF')} time(s)")
        if ca_asked and n_up and not n_on:
            fails.append("X4VR_CURSOR_CANVAS=auto: X4 showed a full-screen "
                         "menu but the layer never drew the cursor through "
                         "the canvas — the switch did not reach the layer")
        elif ca_asked and not n_up:
            print("cursor-auto  no full-screen menu in this run (the map was "
                  "never opened), so the switch was not exercised")
        # Each injector UP followed by a layer ON within 1 s: the per-frame
        # path, not just the totals.
        late = 0
        sw_on = [float(t) for t, k in ca_sw if k == "ON"]
        for t, k in ui_up:
            if k != "UP":
                continue
            t = float(t)
            if not any(t - 0.05 <= x <= t + 1.0 for x in sw_on):
                late += 1
        if n_up and late:
            print(f"cursor-auto  {late} of {n_up} menu opening(s) had no layer "
                  f"switch within 1 s (a menu up for less than a frame, or "
                  f"the channel lagging)")

    # --- on-foot menus: the interact menu and NPC dialogs (take 338) --------
    #
    # Take 338 drew the cursor raw over them and it sat off the highlighted
    # option. The injector logs each opening (X4 freeing the pointer on foot)
    # with the arm it chose; arm A must reach the layer as a canvas switch.
    fm_open = re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+ui: on-foot menu (\d+) OPEN — X4 freed "
        r"the pointer( \(still free after a HUD-down menu\))?; .*?"
        r"\(arm (A|B)[^)]*\) \| head-look yaw (-?[\d.]+) pitch (-?[\d.]+) "
        r"\| pointer (-?[\d.]+),(-?[\d.]+)", text)
    fm_close = {int(n): float(t) for t, n in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+ui: on-foot menu (\d+) closed", text)}
    fm_ab = bool(re.search(r"\bX4VR_CURSOR_FOOT_AB=1\b", run or ""))
    # Take 341: a fresh opening moves the pointer to the window's centre.
    fm_rec = {int(n): (x, y) for n, x, y in re.findall(
        r"ui: on-foot menu (\d+) — pointer recentred from (?:\(unknown\) )?"
        r"(-?[\d.]+),(-?[\d.]+) to ", text)}
    fm_norec = re.findall(r"ui: on-foot menu (\d+) — X4's window is not "
                          r"known yet, pointer NOT recentred", text)
    if fm_open or (ca_asked and "foot: PATCHED" in text):
        if not fm_open:
            # Silence proves "never" only for a build that logs openings:
            # count the pointer releases off the seat from the SDL lines,
            # which every build writes.
            seat = re.findall(r"\[\s*([\d.]+)\]\s+inject\s+headlook: "
                              r"(in|out of) the pilot seat — ", text)
            freed = 0
            for t, k in re.findall(r"\[\s*([\d.]+)\]\s+inject\s+sdl: "
                                   r"relative mouse mode (ON|off)", text):
                t = float(t)
                before = [w for ts, w in seat if float(ts) <= t]
                if k == "off" and before and before[-1] == "out of":
                    freed += 1
            if freed:
                print(f"foot-menu  X4 freed the pointer {freed} time(s) off "
                      f"the seat and no on-foot menu was logged — a build "
                      f"before take 339, which drew them all raw")
            else:
                print("foot-menu  no on-foot menu opened (X4 never freed the "
                      "pointer off the seat), so the canvas there was not "
                      "exercised")
        arms = {"A": 0, "B": 0}
        late_a = 0
        for t, n, res, arm, hy, hp, px, py in fm_open:
            t = float(t)
            arms[arm] += 1
            end = fm_close.get(int(n))
            dur = f"{end - t:5.1f} s" if end is not None else "  open at exit"
            reached = any(t - 0.05 <= x <= t + 1.0 for x in ca_foot_on)
            if arm == "A" and not reached:
                late_a += 1
            print(f"foot-menu  #{int(n):<3d} t {t:10.1f}  arm {arm}  {dur}  "
                  f"head-look {float(hy):+6.1f} {float(hp):+6.1f}  "
                  f"pointer {float(px):6.0f},{float(py):6.0f}"
                  + ("  canvas ON" if reached else "")
                  + ("  (resumed after a HUD-down menu)" if res else "")
                  + ("  recentred" if int(n) in fm_rec else ""))
        if fm_open:
            print(f"foot-menu  {len(fm_open)} opening(s): arm A (canvas) "
                  f"{arms['A']}, arm B (raw) {arms['B']}"
                  + (" — alternating, X4VR_CURSOR_FOOT_AB" if fm_ab else ""))
            print("foot-menu  which arm put the cursor ON the highlighted "
                  "option is Patola's to say; this only says each arm was "
                  "drawn as asked")
            fresh = sum(1 for _t, _n, res, *_ in fm_open if not res)
            if fm_rec or fm_norec:
                kinds = re.findall(r"ui: on-foot menu \d+ — pointer recentred "
                                   r"from .*? \((a conversation|no "
                                   r"conversation|conversation unknown)", text)
                print(f"foot-menu  pointer recentred at {len(fm_rec)} of "
                      f"{fresh} fresh opening(s)"
                      + (f", NOT at {len(fm_norec)} (X4's window unknown)"
                         if fm_norec else "")
                      + (f" — to a conversation's options {kinds.count('a conversation')}, "
                         f"the centre {kinds.count('no conversation')} (no "
                         f"conversation) + {kinds.count('conversation unknown')} "
                         f"(unknown)" if kinds else ""))
        if late_a:
            fails.append(f"on-foot menus: {late_a} canvas-arm opening(s) "
                         f"never reached the layer as a canvas switch")
        if fm_ab and fm_open and not (arms["A"] and arms["B"]):
            print("foot-menu  ONLY ONE ARM opened — the A/B needs at least "
                  "one opening of each")

    # --- the volumetric fog's ray march (take 349) ---------------------------
    #
    # Its ray direction is un-projected at the near plane, so the per-eye
    # inverse projection turned it ~18 deg per eye (takes 345-348). The layer
    # keeps X4's own inverses for it; this says how many modules took that,
    # which is 0 in any run that never met volumetric fog.
    fog_kept = re.findall(r"invproj \S+: fog ray march — (\d+) module\(s\) "
                          r"kept on X4's own inverse projections(.*)", text)
    if fog_kept:
        n_, tail = fog_kept[-1]
        print(f"fog     the volumetric fog's ray march: {n_} module(s) kept on "
              f"X4's own inverse projections"
              + (" — X4VR_FOG_RAY_PER_EYE=1, so patched per eye as before"
                 if "X4VR_FOG_RAY_PER_EYE=1" in tail else ""))

    # --- X4VR_DUMP_SHADERS: the modules were written (take 346) -------------
    #
    # Take 346 asked for the dumps; the directory did not exist, the layer
    # warned and wrote nothing, and the run scored PASS -- the shader
    # identification it existed for could not be done. Gate on intent: asked
    # means the layer must say it dumped.
    sd_m = re.search(r"\bX4VR_DUMP_SHADERS=(\S+)", run or "")
    if sd_m:
        if re.search(r"modules: dumping X4's shader modules to ", text):
            print(f"shaders dumped to {sd_m.group(1)}/mod-NNNN.spv")
        else:
            fails.append(f"X4VR_DUMP_SHADERS={sd_m.group(1)} asked but no "
                         f"module was written"
                         + (" (the layer said it is not writable)"
                            if "is not writable" in text else ""))

    # --- X4's open-menu table, read (take 342) -----------------------------
    #
    # Reconnaissance for the cockpit's right-click menu: is X4's "InteractMenu"
    # in the table IsFullscreenMenuDisplayed looks names up in while that menu
    # is open? The injector logs every change of the set; this lists them with
    # where he was, and the names it ever saw.
    mn_ok = re.search(r"menus: IsConversationActive and the open-menu lookup "
                      r"match", text)
    mn_ref = re.search(r"menus: REFUSED — (.*)", text)
    mn_sets = [(float(t), v) for t, v in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+ui: X4's open menus: (.*)", text)]
    mn_fail = len(re.findall(r"ui: X4's open menus could not be read", text))
    if mn_ok or mn_ref or mn_sets:
        if mn_ref:
            print(f"menus   REFUSED — {mn_ref.group(1)}")
        seat_ev = [(float(t), k) for t, k in re.findall(
            r"\[\s*([\d.]+)\]\s+inject\s+headlook: (in|out of) the pilot "
            r"seat — ", text)]

        def _seat_at(t):
            k = "in"
            for ts, kk in seat_ev:
                if ts <= t:
                    k = kk
            return "seat" if k == "in" else "foot"
        names = {}
        for t, v in mn_sets:
            for nm in v.split(", "):
                nm = nm.replace("[full]", "").replace(" (and more)", "")
                if nm and nm != "none":
                    names.setdefault(nm, set()).add(_seat_at(t))
        print(f"menus   {len(mn_sets)} change(s) of X4's open-menu set"
              + (f", {mn_fail} unreadable walk(s) skipped" if mn_fail else ""))
        for t, v in mn_sets[:40]:
            print(f"menus     t {t:10.1f}  {_seat_at(t):4s}  {v}")
        if len(mn_sets) > 40:
            print(f"menus     ... {len(mn_sets) - 40} more")
        if names:
            print("menus   names seen: " + ", ".join(
                f"{k} ({'/'.join(sorted(v))})" for k, v in sorted(names.items())))
        print("menus   => " + (
            "InteractMenu IS in X4's table — the cockpit right-click menu can "
            "be followed from it" if "InteractMenu" in names else
            "InteractMenu never appeared — if Patola right-clicked in the "
            "cockpit, this table does not hold that menu"))

    # --- HUD-down menus off the seat: the station teleporters (take 339) ----
    #
    # Head-look centres X4's camera for them, but X4 calls none of them
    # full-screen, so take 339 drew the cursor raw and Patola found neither
    # of the on-foot arms aligned there (neither applied). The injector logs
    # each period off the seat with its arm; arm A must reach the layer.
    hm_open = re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+ui: menu with the HUD down (\d+) OPEN "
        r"off the pilot seat — .*?\(arm (A|B)", text)
    hm_close = {int(n): float(t) for t, n in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+ui: menu with the HUD down (\d+) "
        r"closed", text)}
    hm_ab = bool(re.search(r"\bX4VR_CURSOR_CENTRED_AB=1\b", run or ""))
    # The layer's canvas state over time, any reason: a full-screen menu
    # outranks the centring, so take 340's map and crafting bench (A) never
    # logged a HUD-down ON -- the canvas was already on -- and this FAILed
    # them; and its vendor list under B was the canvas all the same.
    ca_state = [(float(t), k, why) for t, k, why in ca_all]

    def _canvas_at(t):
        st = ("OFF", "")
        for tc, k, why in ca_state:
            if tc <= t:
                st = (k, why)
            else:
                break
        return st
    # --- X4's right-click menu, followed from its table (take 343) ---------
    #
    # The injector logs the table holding "InteractMenu" (OPEN) and losing it
    # (closed); the layer must be drawing through the canvas -- for this or
    # any other reason -- within 0.2 s of every opening.
    rc_ev = [(float(t), k) for t, k in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+ui: X4's right-click menu (OPEN|closed)",
        text)]
    if rc_ev:
        periods, start = [], None
        for t, k in rc_ev:
            if k == "OPEN":
                start = t
            elif start is not None:
                periods.append((start, t))
                start = None
        if start is not None:
            periods.append((start, None))
        late = 0
        for t0, t1 in periods:
            k, why = _canvas_at(t0 + 0.2)
            ok_ = k == "ON"
            if not ok_ and (t1 is None or t1 - t0 > 0.5):
                late += 1
            print(f"rclick  t {t0:10.1f}  "
                  + (f"{t1 - t0:5.1f} s" if t1 is not None else "open at exit")
                  + ("  canvas ON" + (f" ({why})" if why else " (full-screen)")
                     if ok_ else "  canvas OFF"))
        print(f"rclick  {len(periods)} right-click menu period(s) from X4's "
              f"table; Patola's eye says whether the cursor sat on the "
              f"highlighted option")
        if late:
            fails.append(f"X4's right-click menu: {late} period(s) without the "
                         f"canvas 0.2 s after X4's table showed it")

    if hm_open:
        arms = {"A": 0, "B": 0}
        late_a = 0
        moot = 0
        for t, n, arm in hm_open:
            t = float(t)
            arms[arm] += 1
            end = hm_close.get(int(n))
            dur = f"{end - t:5.1f} s" if end is not None else "  open at exit"
            reached = any(t - 0.05 <= x <= t + 1.0 for x in ca_cen_on)
            k, why = _canvas_at(t + 1.0)
            full = k == "ON" and not why
            if arm == "A" and not (reached or k == "ON"):
                late_a += 1
            if full:
                moot += 1
            print(f"hud-menu   #{int(n):<3d} t {t:10.1f}  arm {arm}  {dur}"
                  + ("  canvas ON (full-screen: either arm)" if full else
                     "  canvas ON" if reached or k == "ON" else ""))
        print(f"hud-menu   {len(hm_open)} period(s) off the seat: arm A "
              f"(canvas) {arms['A']}, arm B (raw) {arms['B']}"
              + (" — alternating, X4VR_CURSOR_CENTRED_AB" if hm_ab else ""))
        if moot:
            print(f"hud-menu   {moot} of them were full-screen, so the canvas "
                  f"held whatever the arm — only the others test it")
        if late_a:
            fails.append(f"HUD-down menus off the seat: {late_a} canvas-arm "
                         f"period(s) never reached the layer as a canvas "
                         f"switch")
        if hm_ab and not (arms["A"] and arms["B"]):
            print("hud-menu   ONLY ONE ARM — the A/B needs at least one "
                  "period of each")

    # --- the start-screen cursor stand-in, and which cursors X4 picks ------
    #
    # Takes 326-327 drew no cursor on the start screen: X4 shows SDL's default
    # system arrow until its first own SDL_SetCursor, and that is never
    # captured. The injector now shows X4's first-built cursor (#0) until then,
    # and logs which built cursor X4 sets on its first changes -- the check on
    # whether #0 is the arrow X4 itself uses in its menus.
    cd_said = re.search(r"sdl: X4 has set no cursor yet — showing its "
                        r"(?:first-built one|arrow) \(#(\d+), (\S+) "
                        r"hot=\((\d+),(\d+)\)\)", text)
    cd_sets = re.findall(r"sdl: X4 set cursor #(-?\d+)", text)
    if cd_said or cd_sets:
        from collections import Counter as _C
        cnt = _C(cd_sets)
        print("cursor-default " + (
            f"X4's cursor #{cd_said.group(1)} ({cd_said.group(2)}, hot "
            f"{cd_said.group(3)},{cd_said.group(4)}) shown until X4 set one"
            if cd_said else "no stand-in shown (X4 set a cursor first, the "
            "stand-in's hotspot did not match, or an older build)"))
        if cd_sets:
            print(f"cursor-default X4's first set was #{cd_sets[0]}; the sets "
                  f"logged ({len(cd_sets)}) by build index: "
                  + ", ".join(f"#{k} x{v}" for k, v in cnt.most_common(8)))
            if cd_said and cd_sets[0] != cd_said.group(1):
                print(f"cursor-default NOTE: X4's own first choice was "
                      f"#{cd_sets[0]}, not the stand-in #{cd_said.group(1)} "
                      f"(take 328: X4's first set is its cockpit cursor; its "
                      f"menu arrow is the one it sets at the map's edge)")

    # Where X4 switched shape inside full-screen menus: the content's edges in
    # window pixels, measured by X4 itself (take 327's map-fit question).
    me = [(int(w), float(x), float(y)) for w, x, y in re.findall(
        r"sdl: X4 set cursor #(-?\d+).*? at pointer (-?[\d.]+),(-?[\d.]+) "
        r"\[full-screen menu\]", text)]
    if me:
        xs = sorted(x for _w, x, _y in me)
        ys = sorted(y for _w, _x, y in me)
        print(f"cursor-edges {len(me)} shape change(s) inside full-screen "
              f"menus: pointer x {xs[0]:.0f}..{xs[-1]:.0f}, y {ys[0]:.0f}.."
              f"{ys[-1]:.0f} (the window is 0..W-1). X4 switches shape where "
              f"its content starts and ends, so a sweep across the map's four "
              f"edges puts them here")

    # --- X4's camera while the HUD is down: the map's unreachable menus -----
    #
    # Take 328B. X4 draws its UI on a plane fixed to the cockpit, seen through
    # the player camera at its UI field (tan 0.75), and hit-tests the pointer
    # through the same projection -- so a rotated camera keystones and shifts
    # the whole UI inside the window the pointer is confined to. X4's own look
    # follower recentred the camera when a menu opened (take 311: 0,0 every
    # time); x4vrmod_headlook's anglespeed 0 stopped that, and 328B's map sat
    # at -1.99,6.37 for 157 s with 13.7% of X4's UI out of reach. The model
    # below reproduced X4's own hit-testing from the log alone: the arrow at
    # pointer 2643 lands on UI x 2816.0, the UI's right edge.
    def _ui_out_of_reach(yaw, pitch, w=2816, n=64):
        """Share of X4's UI square whose projection leaves the window, for X4's
        camera at (yaw, pitch) in GetCameraRotation's degrees."""
        import math
        t = 0.75
        y, p_ = math.radians(-yaw), math.radians(-pitch)
        cy, sy, cp, sp = math.cos(y), math.sin(y), math.cos(p_), math.sin(p_)
        out = 0
        for i in range(n):
            for j in range(n):
                nx = 2 * (i + 0.5) / n - 1
                ny = 1 - 2 * (j + 0.5) / n
                X, Y, Z = nx * t, ny * t, 1.0
                # R = Ry(yaw) Rx(pitch); the camera sees R^T P
                x1, y1, z1 = cy * X - sy * Z, Y, sy * X + cy * Z
                x2, y2, z2 = x1, cp * y1 + sp * z1, -sp * y1 + cp * z1
                if z2 <= 0:
                    out += 1
                    continue
                sx, sy_ = x2 / z2 / t, y2 / z2 / t
                if abs(sx) > 1 or abs(sy_) > 1:
                    out += 1
        return out / (n * n)

    mc_hud = [(float(t), k) for t, k in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+headlook: HUD is now (up|down)", text)]
    # "external/floating" is the pre-329 label, which also covered PAUSED: the
    # ESC menu pauses, and take 329's ESC menu was this case.
    mc_view = [(float(t), k) for t, k in re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+headlook: view is now "
        r"(the cockpit|external/floating|external|floating|PAUSED)", text)]
    mc_cam = [(float(t), float(a), float(b)) for t, a, b in re.findall(
        r"\[\s*([\d.]+)\]\s+layer\s+poselag: .*? x4=(-?[\d.]+),(-?[\d.]+)",
        text)]
    mc_armed = re.search(r"\[\s*([\d.]+)\]\s+inject\s+headlook write: "
                         r"ARMED on camera", text)
    # " (PAUSED)" since take 330: the ESC menu's centring (take 330 counted 2
    # of its 5 until this matched it).
    # "off the pilot seat" since take 340: the same centring on foot, which
    # until then was logged as "in the cockpit".
    mc_centred = re.findall(r"\[\s*([\d.]+)\]\s+inject\s+headlook: HUD down "
                            r"(?:in the cockpit|off the pilot seat)"
                            r"(?: \(PAUSED\))? — X4's camera "
                            r"CENTRED from yaw (-?[\d.]+) pitch (-?[\d.]+)",
                            text)
    # Intent: write mode, not switched off, and a build that HAS the centring
    # (its write report carries the field) -- 328B asked for neither.
    mc_asked = bool(re.search(r"\bX4VR_HEADLOOK_WRITE=1\b", run or "")) and \
        not re.search(r"\bX4VR_HEADLOOK_MENU_CENTRE=0\b", run or "") and \
        "| menu centring " in text
    if mc_hud and mc_cam and mc_armed:
        t_armed = float(mc_armed.group(1))
        periods, start = [], None
        for t, k in mc_hud:
            # a second "down" restarts: the line prints on a transition, so
            # two in a row means the first period's "up" was never logged
            if k == "down":
                start = t
            elif k == "up" and start is not None:
                periods.append((start, t))
                start = None
        if start is not None:
            periods.append((start, mc_cam[-1][0]))

        def _view_at(t):
            v = "the cockpit"
            for tv, k in mc_view:
                if tv <= t + 0.05:
                    v = k
            return v

        # Off the pilot seat (since take 335) X4's camera is the player
        # object's -- the BODY's yaw from the mouse and arrows -- so its angle
        # is not head-look's. Take 337 FAILed an ESC menu on foot at +60.9 deg
        # with no head-look on foot at all. Off the seat: shown, and judged
        # only where head-look acts there (X4VR_HEADLOOK_FOOT), by whether it
        # logged the centring.
        seat_off = []  # (from, to, head-look acts on foot)
        so_open = None
        for m_ in re.finditer(r"\[\s*([\d.]+)\]\s+inject\s+headlook: (out of|in) "
                              r"the pilot seat — (head-look continues|the write)",
                              text):
            t_ = float(m_.group(1))
            if m_.group(2) == "out of" and so_open is None:
                so_open = (t_, m_.group(3) == "head-look continues")
            elif m_.group(2) == "in" and so_open is not None:
                seat_off.append((so_open[0], t_, so_open[1]))
                so_open = None
        if so_open is not None:
            seat_off.append((so_open[0], float("inf"), so_open[1]))

        def _off_seat(t):
            for f_, t2_, acts in seat_off:
                if f_ <= t <= t2_:
                    return True, acts
            return False, False

        rows, frozen_cockpit = [], []
        for a, b in periods:
            if b < t_armed:
                continue
            off, acts = _off_seat(a)
            if off:
                cen = any(a - 0.5 <= float(t_) <= b + 0.5
                          for t_, _y, _p in mc_centred)
                # share None: not judged (X4's own camera on foot)
                rows.append((a, b, "off the seat", float("nan"), float("nan"),
                             None if not acts else (0.0 if cen else 1.0)))
                if acts and not cen:
                    frozen_cockpit.append((a, b, float("nan"), float("nan"),
                                           1.0))
                continue
            # settled: the period's last 0.4 s (and at least 0.3 s in, past
            # the tick that switches). Take 343 sat down into the docked menu
            # and X4 eased the seat's camera 23 -> 0 deg over 1.07 s; the
            # median from 0.3 s in read +0.11 and called it FROZEN. A camera
            # left at the head's angle is still off at the END, which is
            # what this judges.
            # Readings arrive at the frame rate; a sparse log (or a short
            # period) falls back to its latest reading.
            late_ = [(t, y, p_) for t, y, p_ in mc_cam if a + 0.3 <= t <= b]
            sel = [(y, p_) for t, y, p_ in late_ if t >= b - 0.4]
            if not sel and late_:
                sel = [(late_[-1][1], late_[-1][2])]
            if not sel:
                continue
            ys = sorted(y for y, _p in sel)
            ps = sorted(p_ for _y, p_ in sel)
            my, mp = ys[len(ys) // 2], ps[len(ps) // 2]
            view = _view_at(a)
            share = _ui_out_of_reach(my, mp)
            rows.append((a, b, view, my, mp, share))
            # the ESC menu (PAUSED) is drawn through the cockpit's camera too
            if view in ("the cockpit", "PAUSED") and \
                    (abs(my) > 0.1 or abs(mp) > 0.1):
                frozen_cockpit.append((a, b, my, mp, share))
        # `t0` exists only when the run logged perf windows; a short run has
        # none, and reading it unbound crashed every block after this one.
        try:
            mc_ref = t0
        except NameError:
            _m0 = re.search(r"\[\s*([\d.]+)\]", text)
            mc_ref = float(_m0.group(1)) if _m0 else 0.0
        if rows:
            print(f"menucam {len(rows)} HUD-down period(s) after head-look "
                  f"armed; X4's camera settled at (yaw, pitch), and the share "
                  f"of X4's UI that projects outside the window the pointer is "
                  f"confined to:")
            for a, b, view, my, mp, share in rows[:12]:
                if view == "off the seat":
                    what = ("X4's own camera (no head-look on foot), not judged"
                            if share is None else
                            "head-look CENTRED it" if share == 0.0 else
                            "head-look did NOT centre it")
                    print(f"menucam   t0+{a - mc_ref:6.1f}s {b - a:6.1f}s off "
                          f"the pilot seat — {what}")
                    continue
                tag = ("CENTRED" if abs(my) <= 0.1 and abs(mp) <= 0.1
                       else "OFF-CENTRE")
                print(f"menucam   t0+{a - mc_ref:6.1f}s {b - a:6.1f}s in "
                      f"{view:17s} {my:+6.2f},{mp:+6.2f} deg  {tag:10s} "
                      f"{100 * share:5.1f}% of X4's UI out of reach")
            if len(rows) > 12:
                print(f"menucam   ... {len(rows) - 12} more")
            if mc_centred:
                print(f"menucam {len(mc_centred)} period(s) centred by "
                      f"X4VR_HEADLOOK_MENU_CENTRE, from "
                      + ", ".join(f"{float(y):+.2f},{float(p_):+.2f}"
                                  for _t, y, p_ in mc_centred[:6]))
            if frozen_cockpit:
                worst = max(frozen_cockpit, key=lambda r: r[4])
                if worst[2] != worst[2]:  # NaN: an off-seat period
                    print(f"menucam => NOT CENTRED off the pilot seat: head-look "
                          f"acts on foot (X4VR_HEADLOOK_FOOT) but logged no "
                          f"centring in {len(frozen_cockpit)} menu period(s)")
                else:
                  print(f"menucam => FROZEN: in {len(frozen_cockpit)} cockpit or "
                        f"ESC-pause period(s) X4 drew and hit-tested its menus "
                        f"through a "
                        f"camera left at the head's angle; worst "
                        f"{worst[2]:+.2f},{worst[3]:+.2f} deg puts "
                        f"{100 * worst[4]:.1f}% of X4's UI beyond the pointer's "
                        f"reach (take 328B: the map's left wedge and bottom band)")
                if mc_asked:
                    fails.append("X4VR_HEADLOOK_MENU_CENTRE: a cockpit menu "
                                 "stayed off-centre — the centring did not "
                                 "act (look for 'headlook write: REFUSED')")
            elif any(r[2] in ("the cockpit", "PAUSED") for r in rows):
                print("menucam => every cockpit and ESC-pause menu period "
                      "CENTRED: X4's UI fills the window, so the pointer "
                      "reaches all of it")
            if any(r[2] == "external/floating" for r in rows):
                print("menucam NOTE: 'external/floating' is a pre-329 label "
                      "that also covered the ESC menu's PAUSE (take 329), so "
                      "those periods may be the ESC menu")

    # --- X4VR_STEER: the mouse-steering target, A/B ----------------------
    #
    # Patola's choice after take 328: B (the line from where you look, the
    # default) and A (the target fixed to the cockpit), the apostrophe key
    # switching live. The injector hides X4's own line through the config
    # fork and says when X4 WOULD show it; the layer draws ours and counts
    # frames by the mode it actually drew (A falls back to B while the nose is
    # off the eye). Intent is the run line; each link of the chain is checked
    # separately, so a silent overlay names the link that broke.
    st_m = re.search(r"\bX4VR_STEER=(\S+)", run or "")
    st_asked = bool(st_m) and st_m.group(1) in ("a", "A", "nose", "b", "B",
                                                "gaze")
    st_ok = re.search(r"steer: all (\d+) ShouldShowMouseSteeringHelperLine "
                      r"sites match", text)
    st_ref = re.search(r"steer: REFUSED — (.*)", text)
    st_cfg = re.search(r"config: mousesteeringline: (?:'([^']*)' -> 'false'|"
                       r"already 'false')", text)
    st_rest = re.search(r"config: mousesteeringline: '([^']*)' -> '([^']*)' "
                        r"— restored", text)
    st_on = re.search(r"steer: overlay ON, starting in (\S+)", text)
    st_gate = re.findall(r"\[\s*([\d.]+)\]\s+inject\s+steer: X4's steering "
                         r"line would be (ON|OFF)", text)
    st_tog = re.findall(r"milestone: steering target (A|B)", text)
    # "nose off the eye" is take 331's wording; "ring out of sight" since the
    # fallback tests the ring against what both eyes show.
    # The last field changed meaning twice: take 331 "A asked but nose off the
    # eye -> B", take 332 "A asked but ring out of sight -> B" (fallbacks to
    # B), and since Patola rejected the fallback "A with the nose behind the
    # eye -> hidden" (frames where A drew nothing). `st_fbkind` says which.
    st_drawn = re.findall(r"steer drawn \((?:periodic|final)\): B (\d+) "
                          r"frame\(s\), A (\d+) frame\(s\) \| line in (\d+) \| "
                          r"(A asked but (?:nose off the eye|ring out of sight) "
                          r"-> B|A with the nose behind the eye -> hidden) in "
                          r"(\d+)", text)
    st_box = re.search(r"steer: both eyes show x (\S+)\.\.(\S+) y (\S+)\.\.(\S+) "
                       r"of the rendered eye", text)
    st_rec = re.findall(r"steer: mouse steering engaged — pointer recentred "
                        r"from (?:\(unknown\) )?(-?[\d.]+),(-?[\d.]+)", text)
    st_norec = "pointer NOT recentred" in text
    if st_asked or st_ok or st_ref or st_drawn:
        if st_ref:
            print(f"steer   REFUSED by the site check — {st_ref.group(1)}")
            if st_asked:
                fails.append("X4VR_STEER: X4's code differs from the analysed "
                             "build — no target drawn, X4's own line left")
        elif st_ok:
            print(f"steer   {st_ok.group(1)} code sites match; overlay "
                  + (f"ON from {st_on.group(1)}" if st_on else "never started")
                  + "; X4's own line "
                  + ("hidden through the config fork" if st_cfg else
                     "NOT hidden (no mousesteeringline line)"))
            if st_asked and not st_cfg:
                fails.append("X4VR_STEER: X4's own steering line was never "
                             "hidden — the config fork did not serve "
                             "mousesteeringline=false, so both lines show")
        ons = sum(1 for _t, k in st_gate if k == "ON")
        print(f"steer   X4 would have shown its line {ons} time(s) (mouse "
              f"steering engaged in the cockpit); apostrophe toggles: "
              f"{len(st_tog)}"
              + (f" ({' -> '.join(st_tog[:12])})" if st_tog else ""))
        if st_drawn:
            b, a, ln, fbkind, fb = st_drawn[-1]
            b, a, ln, fb = int(b), int(a), int(ln), int(fb)
            what = ("A with the nose behind the eye, nothing drawn"
                    if "hidden" in fbkind else
                    "A fell back to B (the pre-333 fallback)")
            print(f"steer   the layer drew the target in {b + a} frame(s): B "
                  f"{b}, A {a} | with a line {ln} | {what}: {fb}")
            if st_tog and a == 0 and "A" in st_tog:
                fails.append("X4VR_STEER: A was selected but the layer never "
                             "drew A — the mode did not reach the layer")
        elif st_asked and ons:
            fails.append("X4VR_STEER: X4 showed its steering line but the layer "
                         "never drew the target — the channel or the overlay "
                         "is broken")
        elif st_asked:
            print("steer   no mouse steering in this run, so the target was "
                  "never drawn — the overlay was not exercised")
        if st_box:
            print(f"steer   both eyes show x {st_box.group(1)}..{st_box.group(2)} "
                  f"y {st_box.group(3)}..{st_box.group(4)} of the rendered eye "
                  f"(take 332's fallback box, since removed)")
        if st_rec or st_norec:
            print(f"steer   mouse steering engaged {len(st_rec)} time(s) with the "
                  f"pointer recentred, from "
                  + (", ".join(f"{x},{y}" for x, y in st_rec[:6]) or "-")
                  + (" | at least once NOT recentred (X4's window unknown)"
                     if st_norec else ""))
    elif st_rest:
        print(f"steer   not asked; mousesteeringline restored to "
              f"'{st_rest.group(2)}' from the overlay's earlier run")

    # --- foot: X4VR_FOOTLOG=1, what X4's player camera does out of the seat
    #
    # Read-only census (injector/x4vr_foot.hpp). Each second: the player's
    # mouse, the head, and per angle source -- GetCameraRotation (rot), the
    # camera's local transform (+0x5a0, where head-look writes in the
    # cockpit) and its base (+0x6f0) -- the net and travel of yaw and pitch.
    # Grouped by regime (camera mode, piloting). What FOLLOWS the mouse is a
    # correlation over mouse-only windows; what follows the head, over
    # head-only windows. "Nothing we read" is an answer the block can give,
    # and it means the walking view lives in the attached object.
    ft_asked = bool(re.search(r"\bX4VR_FOOTLOG=[1-9]", run or ""))
    ft_on = "foot: census ON" in text
    ft_before = "sampled BEFORE head-look's write" in text
    ft_reg = re.findall(
        r"\[\s*([\d.]+)\]\s+inject\s+foot: regime camera (\S+) mode (-?\d+) "
        r"attached (\S+) \(vtable (\S+)\)(?: controller (\S+))? \| piloting "
        r"(\d) occupied (\d) \| "
        r"view (\S+) hud (\d) rel (\d) \| rot (\S+) (\S+) (\S+)", text)
    _ft_t = (r"(?:-|net (\S+) (\S+) travel (\S+) (\S+) at (\S+) (\S+))")
    _ft_m = (r"(?:-|net (\S+) (\S+) travel (\S+) (\S+) at (\S+) (\S+) rot (\S+) "
             r"moved (\d+) pos (\S+))")
    # Each window with its own timestamp, from the same match: a second
    # findall for the times would pair them by position, and one line that
    # failed the long pattern would shift every pairing after it.
    ft_win_t = [(float(m.group(1)), m.groups()[1:]) for m in re.finditer(
        r"\[\s*([\d.]+)\]\s+inject\s+"
        r"foot: window (\S+) s n (\d+) mode (-?\d+) piloting (\d) occupied (\d) "
        r"view (\S+) hud (\d) rel (\d) \| mouse n (\d+) dx (\S+) dy (\S+)"
        r"(?: dropped (\d+))?(?: pitchzero (\d+))? \| head " + _ft_t + r" \| rot " + _ft_t + r" \| local " + _ft_m +
        r" \| base " + _ft_m, text)]
    ft_win = [g for _t, g in ft_win_t]
    if ft_asked and not ft_on:
        fails.append("X4VR_FOOTLOG asked but the census never started")
    # X4VR_HEADLOOK_FOOT: B's patch (0x1929f70's walking branch). Intent is
    # the run line; the patch reports itself once, at load.
    fp_asked = bool(re.search(r"\bX4VR_HEADLOOK_FOOT=1", run or ""))
    # "branch" is take 336's retarget (retired: the cockpit's branch);
    # "exit" the second patch, the NULL-controller exit through a stub.
    fp_ok = re.search(r"foot: PATCHED — 0x1929f70's walking (branch|exit)", text)
    fp_ref = re.search(r"foot: patch REFUSED — (.*?)\. Walking stays", text)
    if fp_ok:
        if fp_ok.group(1) == "branch":
            print("foot    patch IN (take 336's retarget of the COCKPIT's "
                  "branch — retired): it composed the camera's own rotation "
                  "on top of the seat's, doubling the cockpit's head-look")
        else:
            print("foot    patch IN (the NULL-controller exit, through the "
                  "stub): walking composes the camera's own rotation "
                  "(head-look) after the player's")
    elif fp_ref:
        print(f"foot    patch REFUSED — {fp_ref.group(1)}")
    if fp_asked and not fp_ok:
        fails.append("X4VR_HEADLOOK_FOOT asked but 0x1929f70 was never patched"
                     + (f" — {fp_ref.group(1)}" if fp_ref else
                        " (no foot: line at all)"))
    if ft_on:
        def _f(x):
            try:
                return float(x)
            except (TypeError, ValueError):
                return None
        wins = []
        for t_w, g in ft_win_t:
            w = {"t": t_w, "secs": _f(g[0]), "mode": int(g[2]), "pilot": g[3] == "1",
                 "occ": g[4] == "1", "view": g[5], "hud": g[6] == "1",
                 "rel": g[7] == "1", "mn": int(g[8]), "dx": _f(g[9]),
                 "dy": _f(g[10]),
                 # None: a take-334 line, which summed the mouse BEFORE
                 # head-look zeroed it -- so its dx/dy are not what X4 got.
                 "drop": int(g[11]) if g[11] else None,
                 "pz": int(g[12]) if g[12] else 0}
            off = 13
            for src, n in (("head", 6), ("rot", 6), ("local", 9), ("base", 9)):
                v = g[off:off + n]
                off += n
                if v[0] == "":
                    w[src] = None
                    continue
                d = {"ny": _f(v[0]), "np": _f(v[1]), "ty": _f(v[2]),
                     "tp": _f(v[3]), "ay": _f(v[4]), "ap": _f(v[5])}
                if n == 9:
                    d.update(rot=_f(v[6]), moved=int(v[7]), pos=_f(v[8]))
                w[src] = d
            wins.append(w)
        print(f"foot    census ON: {len(ft_reg)} regime line(s), {len(wins)} "
              f"window(s)")
        for (t, cam, mode, att, vt, ctl, pil, occ, view, hud, rel, ry, rp,
             rr) in ft_reg[:16]:
            print(f"foot    t {float(t):9.1f}  mode {mode:>2}  piloting {pil} "
                  f"occupied {occ}  view {view:<8} hud {hud} rel {rel}  "
                  f"camera {cam} attached {att} vtable {vt}"
                  + (f" controller {ctl}" if ctl else "")
                  + f"  rot {ry} {rp} {rr}")
        if len(ft_reg) > 16:
            print(f"foot    ... {len(ft_reg) - 16} more regime line(s)")

        def _corr(xs, ys):
            n = len(xs)
            if n < 3:
                return None, None
            mx, my = sum(xs) / n, sum(ys) / n
            sxx = sum((x - mx) ** 2 for x in xs)
            syy = sum((y - my) ** 2 for y in ys)
            sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
            if sxx <= 0 or syy <= 0:
                return 0.0, 0.0
            return sxy / (sxx * syy) ** 0.5, sxy / sxx

        hw_armed = re.search(r"\[\s*([\d.]+)\]\s+inject\s+milestone: "
                             r"head-look write ARMED", text)
        hw_armed_t = float(hw_armed.group(1)) if hw_armed else None
        seat_t = [float(t) for t in re.findall(
            r"\[\s*([\d.]+)\]\s+inject\s+headlook: (?:in|out of) the "
            r"pilot seat — ", text)]
        groups = {}
        for w in wins:
            groups.setdefault((w["mode"], w["pilot"]), []).append(w)
        off_seat = [k for k in groups if not k[1]]
        for (mode, pilot), ws in sorted(groups.items()):
            secs = sum(w["secs"] or 0 for w in ws)
            mx = sum(abs(w["dx"] or 0) for w in ws)
            my = sum(abs(w["dy"] or 0) for w in ws)
            rel = sum(1 for w in ws if w["rel"])
            label = "PILOTING" if pilot else "OFF THE SEAT"
            print(f"foot    mode {mode} {label}: {secs:.0f} s, mouse |dx| "
                  f"{mx:.0f} |dy| {my:.0f}, relative mouse in {rel}/{len(ws)} "
                  f"window(s)")
            # The mouse column is only evidence about X4 if X4 received it.
            unknown = sum(1 for w in ws if w["drop"] is None)
            dropped = sum(w["drop"] or 0 for w in ws)
            mouse_unsure = None
            if unknown:
                mouse_unsure = (f"the mouse was counted BEFORE head-look's drop "
                                f"in {unknown} window(s) (a take-334 line), so "
                                f"what X4 received is unknown")
            elif dropped:
                mouse_unsure = (f"head-look ZEROED {dropped} mouse motion "
                                f"event(s) before X4 saw them")
            if mouse_unsure:
                print(f"foot      NOTE {mouse_unsure} — the mouse-only rows "
                      f"below are not a test of X4")
            # Take 334 sampled AFTER head-look's write in the same pump, so
            # under X4VR_HEADLOOK_WRITE its local column is our own write.
            own_write = (not ft_before and
                         bool(re.search(r"\bX4VR_HEADLOOK_WRITE=1", run or "")))
            if own_write:
                print("foot      NOTE sampled AFTER head-look's write (a "
                      "take-334 census): the local column is our own write "
                      "read back, not what X4 kept")
            for src in ("head", "rot", "local", "base"):
                ds = [w[src] for w in ws if w[src]]
                if not ds:
                    print(f"foot      {src:<5} never read")
                    continue
                ty = sum(d["ty"] or 0 for d in ds)
                tp = sum(d["tp"] or 0 for d in ds)
                extra = ""
                if "rot" in ds[0]:
                    extra = (f", any rotation {sum(d['rot'] or 0 for d in ds):.1f}"
                             f" deg, translation moved up to "
                             f"{max(d['pos'] or 0 for d in ds):.3f} m")
                print(f"foot      {src:<5} yaw travel {ty:8.1f} pitch travel "
                      f"{tp:7.1f} deg{extra}")
            # Under the write, a window is only a test of whether X4 follows
            # the head where the write was asked to act: after its ARMED
            # milestone (game time running), with the HUD up, and not
            # spanning a seat change -- take 338's seated pitch slope read
            # 0.79 from the loading windows and the sit-down's animation,
            # while per-tick tracking was 0.06 deg.
            if hw_armed_t is not None:
                kept = [w for w in ws if w["t"] >= hw_armed_t and w["hud"]
                        and not any(0.0 <= w["t"] - ts <= 2.0
                                    for ts in seat_t)]
                if len(kept) != len(ws):
                    print(f"foot      {len(ws) - len(kept)} window(s) set "
                          f"aside from the rows below: before head-look's "
                          f"ARMED milestone, HUD down, or within 2 s of a "
                          f"seat change")
                ws_fit = kept
            else:
                ws_fit = ws
            # Mouse-only: the mouse moved, the head nearly did not.
            mo = [w for w in ws_fit if w["head"] and abs(w["dx"] or 0) + abs(w["dy"] or 0) >= 30
                  and (w["head"]["ty"] or 0) + (w["head"]["tp"] or 0) < 3.0]
            ho = [w for w in ws_fit if w["head"] and abs(w["dx"] or 0) + abs(w["dy"] or 0) < 5
                  and (w["head"]["ty"] or 0) + (w["head"]["tp"] or 0) >= 5.0]
            follows_mouse, follows_head = [], []
            for src in ("rot", "local", "base"):
                for axis, mk, ak in (("yaw", "dx", "ny"), ("pitch", "dy", "np")):
                    # That axis's own motion: `mo` admits a window on dx + dy,
                    # so with the mouse's pitch zeroed on foot the dy column
                    # is nearly all zeros and a couple of windows made take
                    # 338's "local pitch FOLLOWS mouse dy" (r -0.89 of 47).
                    pts = [(w[mk], w[src][ak]) for w in mo
                           if w[src] and w[mk] is not None and w[src][ak] is not None
                           and abs(w[mk]) >= 30]
                    r, gain = _corr([a for a, _ in pts], [b for _, b in pts])
                    if r is None and src == "rot":
                        print(f"foot      mouse-only: mouse {mk} moved 30+ "
                              f"counts in {len(pts)} window(s) — too few to "
                              f"test the {axis} axis")
                    if r is not None:
                        tag = "FOLLOWS" if abs(r) >= 0.7 else "does not follow"
                        print(f"foot      mouse-only ({len(pts)} windows): "
                              f"{src} {axis} {tag} mouse {mk} (r {r:+.2f}, "
                              f"{gain:+.4f} deg/count)")
                        if abs(r) >= 0.7:
                            follows_mouse.append(f"{src} {axis}")
                    hk = "ny" if axis == "yaw" else "np"
                    pts = [(w["head"][hk], w[src][ak]) for w in ho
                           if w[src] and w["head"][hk] is not None
                           and w[src][ak] is not None]
                    r, gain = _corr([a for a, _ in pts], [b for _, b in pts])
                    if r is not None:
                        tag = "FOLLOWS" if abs(r) >= 0.7 else "does not follow"
                        print(f"foot      head-only ({len(pts)} windows): "
                              f"{src} {axis} {tag} the head (r {r:+.2f}, "
                              f"slope {gain:+.3f})")
                        if abs(r) >= 0.7:
                            follows_head.append(f"{src} {axis}")
            # **With the walking patch, X4's camera on foot is the body times
            # the head**, so the head moves in nearly every window and a
            # mouse-only window (head still) barely exists: take 338's census
            # said "the mouse turns NOTHING" while Patola turned with it. The
            # body is what the camera has beyond head-look's own part, so it
            # is read directly as rot - local and needs no head filter.
            if not pilot and fp_ok and fp_ok.group(1) == "exit":
                def _wrap(a):
                    while a > 180.0:
                        a -= 360.0
                    while a < -180.0:
                        a += 360.0
                    return a
                # Mouse-look only: with the pointer free (an on-foot menu,
                # rel 0) the mouse moves the cursor, and for 5 s after the
                # HUD comes back up X4 may set the facing itself (take 339's
                # teleports turned him ~175 deg). Those took 339's fit from
                # -0.115 to -0.090 deg/count; 338 and 339 agree without them.
                hud_up_t = [float(t) for t in re.findall(
                    r"\[\s*([\d.]+)\]\s+inject\s+headlook: HUD is now up",
                    text)]
                pts = [(w["dx"], _wrap(w["rot"]["ny"] - w["local"]["ny"]))
                       for w in ws_fit if w["rot"] and w["local"] and w["rel"]
                       and w["dx"] is not None and abs(w["dx"]) >= 30
                       and w["rot"]["ny"] is not None
                       and w["local"]["ny"] is not None
                       and not any(0.0 <= w["t"] - u <= 5.0 for u in hud_up_t)]
                r, gain = _corr([a for a, _ in pts], [b for _, b in pts])
                if r is not None:
                    tag = "FOLLOWS" if abs(r) >= 0.7 else "does not follow"
                    print(f"foot      body = rot - local ({len(pts)} mouse-look "
                          f"windows, dx 30+): body yaw {tag} mouse dx (r {r:+.2f}, "
                          f"{gain:+.4f} deg/count)")
                    if abs(r) >= 0.7:
                        follows_mouse.append("the body's yaw (rot - local)")
            if not pilot:
                print(f"foot => mode {mode} off the seat: the mouse turns "
                      + ("? (" + mouse_unsure + ")" if mouse_unsure else
                         ", ".join(follows_mouse) if follows_mouse else
                         "NOTHING we read (the walking view lives elsewhere — "
                         "the attached object)" if mo else
                         "? (no mouse-only window)")
                      + "; the head turns "
                      + (", ".join(f for f in follows_head
                                   if not (own_write and f.startswith("local")))
                         + (" (local: our own write, read back)" if own_write
                            and any(f.startswith("local") for f in follows_head)
                            else "")
                         if follows_head else
                         "nothing" if ho else "? (no head-only window)"))
        if not off_seat:
            print("foot => NOT EXERCISED: no window off the pilot seat")
        # Since take 335 the write stands down off the seat. Intent: write
        # mode + a census that saw the player off the seat must show it.
        downs = len(re.findall(r"headlook: out of the pilot seat — the write "
                               r"stands down", text))
        conts = len(re.findall(r"headlook: out of the pilot seat — head-look "
                               r"continues on foot", text))
        ups = len(re.findall(r"headlook: in the pilot seat — the write may act",
                             text))
        if downs or ups or conts:
            print(f"foot    seat: the write stood down {downs} time(s) off the "
                  f"seat, continued on foot {conts} time(s), resumed {ups} "
                  f"time(s)")
        hw = re.search(r"\bX4VR_HEADLOOK_WRITE=1", run or "")
        if off_seat and ft_before and hw and fp_ok and not conts:
            fails.append("foot: patched and off the pilot seat, but head-look "
                         "never said it continues on foot")
        elif off_seat and ft_before and hw and not fp_ok and not downs:
            fails.append("foot: off the pilot seat with X4VR_HEADLOOK_WRITE, "
                         "but the write never stood down — it writes on foot, "
                         "where take 334 found it reaches nothing")
        pz = sum(w["pz"] for w in wins if not w["pilot"])
        if pz:
            print(f"foot    the mouse's pitch zeroed in {pz} motion event(s) "
                  f"off the seat (head alone for pitch)")

    # --- the Lua reconnaissance (X4VR_LUA=1): which state, source or bytecode
    lua_rows = re.findall(r"lua\[(buf|file)\] (.*?) \((\d+) bytes, "
                          r"(source|bytecode|by path), L=(\S+)\)", text)
    if lua_rows:
        kinds = {}
        states = {}
        for _v, nm, _sz, kind, L in lua_rows:
            kinds[kind] = kinds.get(kind, 0) + 1
            states.setdefault(L, []).append(nm)
        print(f"lua     {len(lua_rows)} chunk(s): "
              + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items()))
              + f" | {len(states)} Lua state(s)")
        for key in ("crosshair", "interactmenu", "helper", "widget_fullscreen"):
            hit = [(nm, L, kind) for _v, nm, _sz, kind, L in lua_rows
                   if key in nm.lower().replace(" ", "")]
            if hit:
                nm, L, kind = hit[0]
                print(f"lua     {key}: {nm} -> state {L} ({kind}); that state "
                      f"loaded {len(states.get(L, []))} chunk(s)")
        same = [r for r in lua_rows if "crosshair" in r[1].lower()]
        inter = [r for r in lua_rows if "interactmenu" in r[1].lower()]
        if same and inter:
            print("lua     => the crosshair code and the right-click menu share "
                  "ONE Lua state" if same[0][4] == inter[0][4] else
                  "lua     => the crosshair code and the right-click menu are in "
                  "DIFFERENT Lua states: an add-on in 'menus' cannot reach the "
                  "crosshair")

    # --- who WRITES the camera orientation? (X4VR_CAMWATCH) --------------
    #
    # Read-only hardware write-watchpoints on the matrix GetCameraRotation
    # reads. Every branch below says what it can and cannot conclude; the
    # WRITER lines are for offline disassembly, not a verdict in themselves.
    if "camwatch: anchors REFUSED" in text:
        m = re.search(r"camwatch: anchors REFUSED[^(]*\(([^)]*)\)", text)
        fails.append("X4VR_CAMWATCH refused to arm: " +
                     (m.group(1) if m else "anchors did not decode") +
                     " — this X4 build is not the one the probe was written "
                     "against")
    cws = re.findall(
        r"camwatch: (\d+) event\(s\), (\d+) sampling, (\d+) arm failure\(s\) "
        r"\| (\d+) store\(s\) counted, (\d+) sampled, (\d+) LOST \| (\d+) "
        r"distinct writer ip\(s\)(?:: ([^|]*))? \| camera yaw span (\d+) deg",
        text)
    if cws:
        (ev, samp_ev, errs, stores, sampled, lost, nwr, top,
         yspan) = cws[-1]
        ev, stores, sampled, nwr, yspan = (int(ev), int(stores), int(sampled),
                                           int(nwr), int(yspan))
        aimed = re.findall(r"camwatch: aimed at (.+?) — (\d+) event", text)
        print(f"camwatch {ev} event(s), {samp_ev} sampling, {errs} arm "
              f"failure(s) | {stores} store(s) counted, {sampled} sampled, "
              f"{lost} LOST | {nwr} distinct writer ip(s) | camera yaw span "
              f"{yspan} deg")
        if aimed:
            print(f"camwatch aimed at {aimed[-1][0]}")
        for m in re.findall(r"camwatch: camera \S+, (\d+) valid buffer\(s\)"
                            r"[^;]*; the index globals name buffers (\d+) and "
                            r"(\d+)", text)[-1:]:
            print(f"camwatch {m[0]} valid buffer(s); index globals named "
                  f"{m[1]} and {m[2]} when last aimed")
        if ev == 0:
            fails.append("X4VR_CAMWATCH armed NO event — see the 'arming "
                         "thread ... failed' line for the errno")
        elif stores == 0 and yspan < 20:
            print("camwatch => NOT EXERCISED: no store counted, but the camera "
                  "barely turned either — not a null")
        elif stores == 0:
            print(f"camwatch => NO STORE to the watched rows while the camera "
                  f"turned {yspan} deg. The matrix GetCameraRotation reads is "
                  f"not written at the entries watched — a BOUNDED null about "
                  f"these addresses, not about the camera")
        elif sampled == 0:
            print(f"camwatch => {stores} store(s) counted but NONE sampled — "
                  f"the upgrade to a sampling event failed; see the arm "
                  f"failures")
        else:
            print(f"camwatch => {nwr} INSTRUCTION(S) WRITE the camera "
                  f"orientation ({stores} stores counted, {sampled} sampled). "
                  f"Disassemble each offline — the store is the instruction "
                  f"just BEFORE the ip:")
            for w in re.findall(r"camwatch: WRITER #(\d+) ip=(\w+) \(the store"
                                r"[^)]*\) thread (\d+) '([^']*)' buf (\d+)"
                                r"(?: \[([^\]]*)\])? \|.*?\| stack->X4 "
                                r"text:(.*)", text):
                lab = f" [{w[5]}]" if w[5] else ""
                print(f"camwatch   #{w[0]} ip={w[1]} thread {w[2]} "
                      f"'{w[3]}' buf {w[4]}{lab} | returns:{w[6][:160]}")
    # Hop 2: the camera controller's chain, and which stored matrix moves
    # WITH the camera (free-look), stays still, or moves on its own.
    # A refused CAMERA CLASS is transient: take 317 refused the load-screen
    # camera (another class) and verified the cockpit's 40 s later. Only a run
    # that never verified a chain failed; a CODE mismatch always fails.
    cwm = re.search(r"X4VR_CAMWATCH=(\d+)", run or "")
    cw_mode = int(cwm.group(1)) if cwm else 0
    for m in re.finditer(r"camwatch hop(?:2b?|3): REFUSED — (.*)", text):
        if "code at" in m.group(1):
            fails.append("X4VR_CAMWATCH refused: " + m.group(1))
    # Hop 3 watches the camera's own transform and does not need the
    # controller's class; there the chain is only the reference.
    if cw_mode == 4:
        h3 = re.findall(r"camwatch hop3: camera (\S+) mode (-?\d+) — "
                        r"translation \(([^)]*)\) rows \(([^)]*)\)", text)
        for cam, mode, tr, rows in h3[-3:]:
            print(f"camwatch hop3 camera {cam} mode {mode} — translation "
                  f"({tr}) rows ({rows})")
        if "co-move [camera.rot]" not in text:
            fails.append("X4VR_CAMWATCH=4 asked for hop 3, but the camera's own "
                         "transform was never censused — see the 'camwatch "
                         "hop3' lines")
    if cw_mode == 4:
        pass
    elif ("camwatch hop2: REFUSED — controller class" in text and
            "controller class verified" not in text):
        fails.append("X4VR_CAMWATCH=2 never verified the camera controller — "
                     "every camera it saw was another class")
    elif "camwatch hop2: REFUSED — controller class" in text:
        print("camwatch hop2 refused another camera class first (load/menu "
              "camera) and verified the cockpit's later — transient, expected")
    ch = re.findall(r"camwatch hop2: (controller \S+ vtable .*)", text)
    if ch:
        print(f"camwatch hop2 {ch[-1][:400]}")
    cm_all = {}
    for rec in re.findall(
            r"camwatch hop2 co-move \[([^\]]*)\] @(\S+): (\d+) tick\(s\), "
            r"camera moved in (\d+), the matrix moved WITH it in (\d+), ALONE "
            r"in (\d+) \|(.*)", text):
        cm_all.setdefault(rec[0], []).append(rec)

    # **A freed object is not a rotation.** Take 318's last census line read
    # the controller after X4 freed it at exit: all zeros, whose trace angle is
    # acos(-1/2) = exactly 120 deg, and the diagonal ranges dropped to 0.000.
    # This scorer took the LAST line and called the sway "FOLLOWS THE CAMERA
    # and spans its range (120.0 deg)". Logs since then count such reads and
    # keep them out ("not-a-transform"); older ones are recognised by that
    # exact signature and the last line before it is used instead.
    def _freed_read(rec):
        mr = re.search(r"max rotation ([\d.]+) deg", rec[6])
        rg = re.search(r"3x3 ranges (.*)", rec[6])
        vals = re.findall(r"(-?[\d.]+)\.\.(-?[\d.]+)", rg.group(1)) \
            if rg else []
        return bool(mr and abs(float(mr.group(1)) - 120.0) < 0.005 and
                    len(vals) == 9 and
                    any(float(vals[k][0]) == 0.0 for k in (0, 4, 8)))
    cm = {}
    for lab, recs in cm_all.items():
        pick = recs[-1]
        if "not-a-transform" not in pick[6] and _freed_read(pick):
            clean = [r for r in recs if not _freed_read(r)]
            if clean:
                print(f"camwatch hop2 [{lab}] its last census line read a "
                      f"FREED object (all zeros: trace angle exactly 120 deg, "
                      f"diagonal down to 0.000 — X4 tearing down at exit); "
                      f"scored on the line before it")
                pick = clean[-1]
        cm[lab] = pick
    ysp = re.findall(r"camera yaw span (\d+) deg", text)
    cam_half = int(ysp[-1]) / 2.0 if ysp else 0.0
    for lab in sorted(cm, key=lambda x: (x != "controller", x)):
        _, addr, t, c, w, a, rest = cm[lab]
        t, c, w, a = int(t), int(c), int(w), int(a)
        nbad = re.search(r"(\d+) not-a-transform read\(s\) skipped", rest)
        badtxt = (f", {nbad.group(1)} freed/not-a-transform read(s) kept out"
                  if nbad and int(nbad.group(1)) else "")
        # Hop 3's sharper question: does its angle EQUAL the camera's?
        mt = re.search(r"match: far (\d+), within 3 deg (\d+), \|dtheta\| "
                       r"median <(\d+) p90 <(\d+) deg", rest)
        match_v = None
        if mt:
            far, w3, med, p90 = (int(x) for x in mt.groups())
            if far < 300:
                match_v = (f"angle match NOT EXERCISED — only {far} tick(s) "
                           f"with the camera 10+ deg off centre (needs 300)")
            elif med <= 3 and p90 <= 10:
                match_v = (f"HAS THE CAMERA'S ANGLE — |dtheta| median <{med}, "
                           f"p90 <{p90} deg over {far} far tick(s) "
                           f"({100.0 * w3 / far:.0f}% within 3 deg): THE "
                           f"FREE-LOOK")
            elif med > 10:
                match_v = (f"does NOT have the camera's angle — |dtheta| "
                           f"median <{med} deg over {far} far tick(s)")
            else:
                match_v = (f"PARTLY has the camera's angle — |dtheta| median "
                           f"<{med}, p90 <{p90} deg over {far} far tick(s); "
                           f"read the hop3 samples before calling it")
        # HOW FAR, not only when. Under head-look the camera moves on most
        # ticks, so anything that changes often "moves with" it -- take 317's
        # sway matrix did, while never rotating past ~9 deg against 131.
        rot = re.search(r"max rotation ([\d.]+) deg", rest)
        disp = re.search(r"max displacement ([\d.]+)", rest)
        mag = float(rot.group(1)) if rot else None
        if mag is None and not disp:
            # Older logs: estimate from the logged off-diagonal ranges.
            rg = re.search(r"3x3 ranges (.*)", rest)
            vals = re.findall(r"(-?[\d.]+)\.\.(-?[\d.]+)", rg.group(1)) \
                if rg else []
            if len(vals) == 9:
                off = [abs(float(x)) for k, pair in enumerate(vals)
                       if k not in (0, 4, 8) for x in pair]
                mag = math.degrees(math.asin(min(1.0, max(off))))
        magtxt = (f", max rotation {mag:.1f} deg" if mag is not None else "") + \
                 (f", max displacement {float(disp.group(1)):.4f}" if disp else "")
        # A translation that never left its first value is static, whatever
        # the move counts say: before take 320 they included the rotation rows
        # read after it, and take 319 called a zero camera.pos "a TRANSLATION
        # that moves with the camera".
        if c < 30:
            v = "NOT EXERCISED (the camera barely moved)"
        elif disp and float(disp.group(1)) < 1e-4:
            v = (f"STATIC — the translation never left its first value"
                 + (" (the move counts are the rotation rows read after it)"
                    if w or a else ""))
        elif w == 0 and a == 0:
            v = "STATIC — never changed"
        elif (mag is not None and cam_half > 0 and mag < 0.25 * cam_half):
            v = (f"moves, but only up to {mag:.1f} deg against the camera's "
                 f"+-{cam_half:.0f} — TOO SMALL to be the free-look (sway / "
                 f"inertia / shake)")
        elif (mag is not None and cam_half > 0 and mag >= 0.5 * cam_half and
              a <= 0.02 * t):
            v = (f"FOLLOWS THE CAMERA and spans its range ({mag:.1f} deg "
                 f"against +-{cam_half:.0f}) — the free-look candidate")
        elif disp and a <= 0.02 * t:
            v = "a TRANSLATION that moves with the camera"
        elif w >= 0.8 * c and a <= 0.1 * t:
            v = "MOVES WITH THE CAMERA (magnitude unknown — older log)"
        elif a > w:
            v = "moves ON ITS OWN (shake / sway / something else)"
        else:
            v = "MIXED — moves with the camera and on its own"
        print(f"camwatch hop2 [{lab}] {t} ticks, camera moved {c}, matrix WITH "
              f"it {w}, ALONE {a}{magtxt}{badtxt} => {v}")
        if match_v:
            print(f"camwatch hop3 [{lab}] => {match_v}")
    smp = re.findall(r"camwatch hop3 sample \[camera\.rot\]: (.*)", text)
    if smp:
        print(f"camwatch hop3 {len(smp)} raw camera/matrix pair(s) logged for "
              f"the convention; the first ones:")
        for x in smp[:4]:
            print(f"camwatch hop3   {x[:220]}")

    # --- where does X4 keep its free-look angle? (X4VR_CAMSCAN) ------------
    #
    # Read-only. Silent unless the knob ran, so "not measured" never reads as
    # "measured nothing". A null only counts if the camera actually swept:
    # the scan discovers a slot only when the angle leaves +-5 degrees, and a
    # run that never turned the view had nothing to find.
    csum = re.findall(
        r"camscan: (\d+) sample\(s\), camera yaw span ([\d.]+) deg, pitch "
        r"span ([\d.]+) deg \| (\d+) region\(s\) (\d+) bytes readable \| "
        r"discoveries (\d+), alive (\d+), eliminated (\d+)(, CAPPED[^|]*)? \| "
        r"SURVIVORS (\d+)", text)
    coff = "camscan: GetCameraRotation" in text and "the scan is OFF" in text
    if coff:
        fails.append("X4VR_CAMSCAN ran but GetCameraRotation's prologue did not "
                     "decode — the scan never started (X4 build changed?)")
    elif csum:
        (n, ys, ps, nreg, nbytes, disc, alive, elim, capped,
         surv) = csum[-1]
        n, surv = int(n), int(surv)
        ys, ps = float(ys), float(ps)
        print(f"camscan  {n} sample(s) | camera swept yaw {ys:.1f} deg, pitch "
              f"{ps:.1f} deg | {nreg} region(s), {nbytes} bytes | "
              f"discoveries {disc}, alive {alive}, eliminated {elim}"
              f"{' — CAPPED' if capped else ''} | SURVIVORS {surv}")
        # Most recent first, de-duplicated: the list is re-printed as it runs.
        cands = re.findall(r"camscan: CANDIDATE (.+)", text)
        seen = []
        for c in cands[::-1]:
            if c not in seen:
                seen.append(c)
        if ys < 10.0 and ps < 10.0:
            print(f"camscan  => NOT EXERCISED. The camera never swept more "
                  f"than 10 degrees on either axis, so there was nothing to "
                  f"find — this is not a null.")
        elif surv == 0:
            print(f"camscan  => NOTHING TRACKED THE ANGLE across a real sweep "
                  f"({ys:.0f} deg yaw, {ps:.0f} deg pitch). The free-look "
                  f"state is not stored as an angle inside the scanned range "
                  f"(root, cam, and one pointer deep) — it may be a quaternion "
                  f"or matrix, or live further away. A bounded null, not "
                  f"'there is none'.")
        else:
            print(f"camscan  => {surv} SLOT(S) TRACKED X4's ANGLE across many "
                  f"distinct values. These are the candidates for a direct "
                  f"camera write; a WRITE test is the next step, not this run:")
            for c in seen[:8]:
                print(f"camscan     {c}")

    # --- #71: what controllers does X4 actually see? ------------------------
    #
    # X4 requests SteamInput006 AND imports the whole SDL3 joystick API, and
    # which path is live is not readable from the binaries. Silent unless X4
    # touched the API, so a keyboard-and-mouse run carries no line about
    # hardware nobody plugged in.
    pc = re.findall(
        r"pad census: enumerate (\d+) \(last saw (-?\d+) device\(s\)\) \| "
        r"opened joystick (\d+) gamepad (\d+) \| POLLED axis reads (\d+) \| "
        r"EVENTS joy-axis (\d+) pad-axis (\d+) buttons (\d+) added (\d+) \| "
        r"mapping files (\d+)", text)
    pad_names = re.findall(r'pad: X4 (opened \w+) id=(\d+) name="([^"]*)"', text)
    pad_axes = re.findall(r"pad axes \((\w+), device id ([\d,?]+)\): (.+)", text)
    if pc:
        en, seen, oj, op, ax, eja, epa, evb, eva, maps = (int(v) if v.isdigit()
                                                          else int(v)
                                                          for v in pc[-1])
        ev_axis = eja + epa
        print(f"pad        enumerate {en} (last saw {seen} device(s)) | opened "
              f"joystick {oj} gamepad {op} | mappings {maps}")
        print(f"pad        axis values: POLLED {ax} | EVENTS joy {eja} pad "
              f"{epa} | buttons {evb} | hotplug-added {eva}")
        for what, pid, name in pad_names:
            print(f"pad        {what} id={pid} \"{name}\"")
        # LAST line per family only. The ranges are cumulative, so every
        # periodic report restates the whole history and take 309 printed 130
        # near-identical lines — a wall that buries the one line anybody reads
        # and hides the differences between families inside it.
        last_axes = {}
        for fam, ids, axes in pad_axes:
            last_axes[fam] = (ids, axes)
        for fam, (ids, axes) in last_axes.items():
            print(f"pad        axes seen ({fam}, device {ids}): {axes}")
            # Full travel is what says an axis was actually swept to its
            # stops; a partial range means the run never exercised it, and
            # reading "this axis does not move" off that would be the same
            # error as reporting zero for an input nobody supplied.
            full, part = [], []
            for m in re.finditer(r"(\d+)(?:=\w+)?:(-?\d+)\.\.(-?\d+)", axes):
                idx, lo, hi = int(m.group(1)), int(m.group(2)), int(m.group(3))
                (full if hi - lo > 55000 else part).append(idx)
            if full:
                print(f"pad        swept to the stops ({fam}): "
                      f"{', '.join(str(i) for i in full)}")
            if part:
                print(f"pad        only partly moved ({fam}): "
                      f"{', '.join(str(i) for i in part)} — these were not "
                      f"driven to their limits, so their absence from a "
                      f"binding says nothing")
        # `seen` is -1 when X4 never called SDL_GetJoysticks at all, which take
        # 307 showed is the normal case: it learns about devices from ADDED
        # events and opens them directly. So "no device" must be judged on
        # opens and events, not on an enumeration X4 does not perform.
        if oj == 0 and op == 0 and eva == 0:
            print(f"pad        => X4 SEES NO DEVICE. No open succeeded and no "
                  f"ADDED event arrived, so nothing reached X4 through "
                  f"evdev/udev at all. No rebinding could fix that — the "
                  f"question is whether something captured the device upstream "
                  f"or it was never presented. Check the run's stated "
                  f"controller configuration.")
        elif oj == 0 and op == 0:
            print(f"pad        => ANNOUNCED BUT NEVER OPENED. {eva} ADDED "
                  f"event(s) and no successful open — a mapping/GUID question, "
                  f"not a binding one.")
        elif ax == 0 and ev_axis == 0:
            print(f"pad        => OPENED, NO AXIS TRAFFIC. {oj + op} device(s) "
                  f"opened and not one axis value arrived by either channel. "
                  f"If the sticks were moved during this take that is a real "
                  f"defect; if they were not, this line says nothing.")
        else:
            how = ("EVENTS" if ev_axis and not ax else
                   "POLLING" if ax and not ev_axis else "BOTH")
            print(f"pad        => AXIS VALUES ARE REACHING X4, by {how} "
                  f"({ax} polled, {ev_axis} events) across {oj + op} opened "
                  f"device(s). Anything not steering is a BINDING question: "
                  f"the inputmap puts steering on INPUT_JOYAXIS_RX/RY and "
                  f"strafe on X/Y. Compare those against the axis indices "
                  f"above — and note the gamepad family is SDL's "
                  f"LEFTX/LEFTY/RIGHTX/RIGHTY order, which is NOT raw joystick "
                  f"numbering, so an index means different hardware depending "
                  f"on which family carried it.")
        if eja and epa:
            print(f"pad        NOTE: BOTH families carried axis values ({eja} "
                  f"joystick, {epa} gamepad). Two devices, or one device seen "
                  f"twice — read the per-family axis lines separately and do "
                  f"not pool their indices.")
    elif pad_names:
        print(f"pad        {len(pad_names)} device line(s) but no census line — "
              f"the report is gated on the pumprate cadence, which needs X4's "
              f"drain to be running.")

    # --- task #75: did our own tick cadence actually run? -------------------
    #
    # Reported separately from `pumprate` because "the driver ran at 90 Hz" and
    # "the servo ticked 90 times a second" are different claims, and a driver
    # that reported only its SETTING could not tell a working fix from a thread
    # that never started. The re-phase count is the safety net: with absolute
    # deadlines, a tick slower than its period would otherwise busy-loop pushing
    # SDL events, which is take 119's drain livelock.
    td = re.findall(r"tickdrv: asked (\d+) Hz, ran ([\d.]+) Hz over ([\d.]+) s, "
                    r"([\d.]+)% of wakeups late by >half a period, (\d+) "
                    r"re-phase\(s\) after overrun", text)
    if td:
        def med(xs):
            s = sorted(xs)
            return s[len(s) // 2]
        ask = float(td[-1][0])
        ran = med([float(g[1]) for g in td])
        lates = med([float(g[3]) for g in td])
        resync = sum(int(g[4]) for g in td)
        rans = [float(g[1]) for g in td]
        print(f"tickdrv    {len(td)} window(s) | asked {ask:.0f} Hz, ran "
              f"{ran:.1f} Hz median (range {min(rans):.1f}-{max(rans):.1f}), "
              f"{lates:.1f}% of wakeups late, {resync} re-phase(s)")
        if ran < 0.85 * ask:
            print(f"tickdrv    => the cadence was NOT achieved ({ran:.1f} of "
                  f"{ask:.0f} Hz). With {resync} re-phase(s) the tick is "
                  f"overrunning its period, so the servo is being starved by its "
                  f"own driver and the run does not test the fix.")
        elif resync > 0:
            print(f"tickdrv    NOTE: {resync} re-phase(s) — some ticks ran long "
                  f"enough to miss a deadline by over two periods. Ticks were "
                  f"DROPPED rather than bursting, which is the intended failure, "
                  f"but a large count means the tick itself is too slow.")
        else:
            print(f"tickdrv    => cadence held at {ran:.1f} Hz with no "
                  f"re-phasing, so the rate below is the one asked for.")

    # --- task #75: is GetCameraRotation safe off X4's own thread? -----------
    #
    # The question is whether X4 survives and whether the value is sane — NOT
    # whether the two threads agree. They sample at different instants, so while
    # the camera moves they SHOULD differ; agreement would mean the probe was
    # reading a stale copy.
    cp = re.findall(r"camprobe: (\d+) call\(s\) off-thread, (\d+) non-zero, "
                    r"(\d+) ABSURD \| probe (-?[\d.]+),(-?[\d.]+) \| "
                    r"X4-thread (-?[\d.]+) \(seen (\d+)\)", text)
    if cp:
        calls, nz, absurd = (int(cp[-1][0]), int(cp[-1][1]), int(cp[-1][2]))
        print(f"camprobe   {calls} off-thread GetCameraRotation call(s), {nz} "
              f"non-zero, {absurd} outside +/-200 deg, over {len(cp)} report(s)")
        if absurd:
            print(f"camprobe   => UNSAFE: {absurd} reading(s) outside any "
                  f"possible camera angle. The getter races something when "
                  f"called off X4's thread, so the tick driver must NOT call it "
                  f"from its own thread — sample it on X4's thread and pass the "
                  f"value across.")
        elif nz == 0:
            print(f"camprobe   => INCONCLUSIVE: every reading was 0,0, so the "
                  f"camera never moved and the call was never exercised against "
                  f"live state. Free-look has to be active during the probe for "
                  f"this to mean anything.")
        else:
            print(f"camprobe   => the call SURVIVED {calls} off-thread "
                  f"invocations with {nz} live non-zero readings and nothing out "
                  f"of range. That is evidence it does not crash or tear, not a "
                  f"proof of thread-safety; it is the configuration the tick "
                  f"driver needs, exercised in the cheapest place it could fail.")

    # --- task #75: is the off-thread camera read STALE, or is the camera fast?
    #
    # Take 285b's two threads disagreed by up to 59.588 deg during the
    # oscillation, and staleness and genuine ~1000 deg/s burst motion both
    # predict exactly that — with opposite fixes. `camlag` matches each reading
    # against the series X4's OWN thread recorded, which fast motion cannot age.
    #
    # The CONTROL is the whole point: a reading taken on X4's thread, run
    # through the identical matcher, gives the lag that "current" scores. A bare
    # number of milliseconds from the probe would have nothing to be read
    # against, and tests/camring.cpp shows why that matters — an earlier version
    # of this matcher reported 500 ms of lag for a perfectly current reading on a
    # 2 Hz sweep, purely from value recurrence.
    cl = [m.groupdict() for m in re.finditer(
        r"camlag: CONTROL \(X4's own thread\) n=(?P<sn>\d+) mean (?P<sm>[\d.]+) "
        r"ms max (?P<sx>[\d.]+) \| OURS \(off-thread\) n=(?P<on>\d+) mean "
        r"(?P<om>[\d.]+) ms max (?P<ox>[\d.]+) \| match residual "
        r"(?P<res>[\d.]+) deg \| (?P<ref>\d+) refused", text)]
    if cl:
        last = cl[-1]
        sn, on = int(last["sn"]), int(last["on"])
        sx, ox = float(last["sx"]), float(last["ox"])
        ref = int(last["ref"])
        # DIFFERENCED, not read off the last line. These are cumulative counters
        # and quoting the final mean is the same dilution defect that made take
        # 283's `inputrate` report 33.4% where per-window differencing said 73%.
        # I fixed that in two instruments and then wrote it a third time here.
        # It matters: the cumulative means are 18.41/13.13 ms, pulled up by
        # matches saturated at the window edge, while the per-window medians are
        # 10.90/6.67.
        wins = []
        for a, b in zip(cl, cl[1:]):
            dsn = int(b["sn"]) - int(a["sn"])
            don = int(b["on"]) - int(a["on"])
            if dsn < 20 or don < 20:
                continue
            wins.append((
                (float(b["sm"]) * int(b["sn"]) - float(a["sm"]) * int(a["sn"]))
                / dsn,
                (float(b["om"]) * int(b["on"]) - float(a["om"]) * int(a["on"]))
                / don))

        def med(xs):
            s = sorted(xs)
            return s[len(s) // 2]
        if wins:
            sm = med([w[0] for w in wins])
            om = med([w[1] for w in wins])
            diffs = [w[1] - w[0] for w in wins]
            worst = max(diffs)
            print(f"camlag     {len(wins)} windows | CONTROL (X4's thread) "
                  f"median {sm:.2f} ms, n={sn} | OURS (off-thread) median "
                  f"{om:.2f} ms, n={on} | OURS-CONTROL median {med(diffs):+.2f} "
                  f"ms, worst window {worst:+.2f} | residual "
                  f"{float(last['res']):.3f} deg | {ref} refused")
            print(f"camlag     max matched age CONTROL {sx:.1f} ms, OURS "
                  f"{ox:.1f} ms — at/near the 300 ms window these are SATURATED, "
                  f"not measurements, and they are what inflate the cumulative "
                  f"means over the per-window medians above.")
            # A structural offset, stated rather than left to flatter the result.
            print(f"camlag     NOTE: CONTROL excludes the entry it just wrote, "
                  f"so its floor is one poll period while OURS' floor is zero. "
                  f"A perfectly current off-thread read should therefore score "
                  f"about a frame BETTER, not equal. Read the bound, not the "
                  f"sign.")
        else:
            sm, om, worst = float(last["sm"]), float(last["om"]), 0.0
            print(f"camlag     too few windows to difference; cumulative only: "
                  f"CONTROL {sm:.2f} ms OURS {om:.2f} ms — do not quote these "
                  f"as a result.")
        if sn < 100 or on < 100:
            print(f"camlag     => TOO FEW SAMPLES ({sn} control, {on} ours). "
                  f"The camera has to be MOVING for a match to mean anything — "
                  f"free-look has to be active and swinging for most of the run.")
        elif ref > 4 * on:
            print(f"camlag     => MOSTLY REFUSED ({ref} refusals against {on} "
                  f"measurements): the camera was too still for the window to "
                  f"support a match. Inconclusive, not a null.")
        elif worst > 8.0:
            print(f"camlag     => STALE. Our off-thread reading is {om:.1f} ms "
                  f"old against the {sm:.1f} ms a reading taken on X4's own "
                  f"thread scores through the same matcher — {om - sm:.1f} ms "
                  f"of real staleness. At the ~1000 deg/s the oscillation "
                  f"reached, that is {(om - sm) * 1.0:.0f} deg of error the "
                  f"servo steers on, which is the size of the jumps. The tick "
                  f"driver must sample the camera on X4's thread and pass the "
                  f"value across, NOT call the getter from its own.")
        else:
            print(f"camlag     => NOT STALE. Per window, our off-thread reading "
                  f"medians {om:.1f} ms against the control's {sm:.1f} ms, and "
                  f"in the WORST of {len(wins)} windows it is only "
                  f"{worst:+.1f} ms behind — not an average that could hide a "
                  f"bad stretch. At the ~1000 deg/s the oscillation reached, "
                  f"that bounds the error the servo steers on at a few degrees, "
                  f"where the episode needed ~60. **Staleness is refuted**: "
                  f"take 285b's 59.588 deg two-thread disagreement was genuine "
                  f"fast motion. With the servo arithmetic already cleared by "
                  f"tests/headlook.cpp, BOTH standing explanations are dead and "
                  f"the cause is somewhere not yet looked at. Do not build a "
                  f"freshness fix.")

    if "TWO THREADS are in headlook_tick" in text:
        fails.append("two threads entered headlook_tick — the servo's state is "
                     "not atomic, so the command itself is corrupt, not just "
                     "the counters. X4VR_TICK_HZ must MOVE the tick, not add a "
                     "caller: the drain's call was not suppressed. Every "
                     "head-look number in this run is void.")

    # --- task #75: which per-view member carries the SMOOTH camera ----------
    #
    # Take 281's ORIGINAL capture (the re-encode was void -- its transcode
    # manufactured 32.5% duplicate frames, and duplicates were the signal) says
    # X4 renders smooth ~90 fps motion: 79.3% of consecutive display frames move,
    # median 4.857 px, against 0.073 px held still. `GetCameraRotation` changes
    # on 41.7%. So X4 interpolates and that getter does not describe what X4
    # draws -- which retires every declaration this project has tried, since all
    # four sample it.
    #
    # `camdiff` diffs the 11 mat4s of the per-view block per frame. The member
    # changing on ~80-90% of frames is the smooth camera the declaration must
    # read. The verdict is NOT hardcoded to a member name: naming the expected
    # winner in advance is how an instrument ends up confirming the guess it was
    # built around.
    cd_ = re.findall(
        r"camdiff: cam@(\d+) draws=(\d+) (\S+)\s+changed on\s+([\d.]+)% of "
        r"(\d+) frames, max delta ([\d.]+)", text)
    if cd_:
        # Last complete report only: the counters are cumulative, so the final
        # block is the one with the most frames behind it.
        last = {}
        for cam, draws, name, pct, fr, mx in cd_:
            last[name] = (float(pct), int(fr), float(mx), cam, int(draws))
        fr = max(v[1] for v in last.values())
        cam = next(iter(last.values()))[3]
        print(f"camdiff  per-view block of camera @{cam}, {fr} frames diffed")
        for name, (pct, _f, mx, _c, _d) in sorted(
                last.items(), key=lambda kv: -kv[1][0]):
            print(f"camdiff    {name:<18} {pct:5.1f}%  max delta {mx:.6f}")
        # A member only counts as the smooth camera if it ALSO carries real
        # motion: a jitter matrix dithering in its last bit changes on every
        # frame and means nothing, which is what max delta is here to catch.
        smooth = [n for n, v in last.items()
                  if v[0] >= 70.0 and v[2] > 1e-4]
        stair = [n for n, v in last.items() if 25.0 <= v[0] <= 55.0]
        if smooth:
            print(f"camdiff  => SMOOTH channel(s): {', '.join(smooth)} — "
                  f"changing on >=70% of frames with real magnitude. This is "
                  f"what the declared orientation must be read from; "
                  f"GetCameraRotation is not it.")
        else:
            print("camdiff  => NO member changes on >=70% of frames with real "
                  "magnitude. Either X4's smooth camera is not in this block "
                  "at all (it would then be per-object, baked CPU-side under "
                  "camera-relative rendering), or the wrong camera was picked "
                  "— check the draws count above against the `proj CAMERA` "
                  "lines before concluding anything.")
        if stair:
            print(f"camdiff  => staircase-rate member(s): {', '.join(stair)} — "
                  f"these move at GetCameraRotation's ~40%, so they are another "
                  f"view of the channel already known to be wrong")
    elif run and "X4VR_CAMDIFF=1" in run:
        fails.append("X4VR_CAMDIFF=1 was set but no camdiff line was written — "
                     "no credited camera block was readable, so this run does "
                     "NOT locate the smooth camera")

    # --- task #73: open-loop identification of X4's camera ------------------
    #
    # Take 212 measured the plant THROUGH a closed loop, which picks its command
    # from the error it is trying to measure. Two explanations built that way
    # died the same day. X4VR_PLANTID replaces the servo with a fixed sequence
    # and the head drives nothing, so what comes out is the plant and only the
    # plant.
    #
    # WHAT EACH CANDIDATE READS, stated before the numbers are:
    #
    #   integrator (what tests/servo_lag.cpp assumed)
    #       delivery ~100% at EVERY size, and the camera stops dead when the
    #       commands stop (coast ~0%).
    #   per-step cap (the "anglespeed" story, already doubted -- take 212 saw
    #   541 deg/s)
    #       delivery ~100% small, collapsing at large sizes; coast ~0%.
    #   smoothed / momentum
    #       delivery below 100% everywhere, and the camera KEEPS MOVING after
    #       the commands stop -- a coast that decays over several steps.
    #
    # The pair (how delivery fades across the staircase, how much the coast
    # retains) separates all three.
    #
    # **tests/run-plantid-scorer.sh checks that THIS code does**, by emitting the
    # injector's exact log format from an integrator, a smoother and a per-step
    # cap and requiring the three verdicts below to come out right. This comment
    # used to credit tests/plantid.cpp, which checks its own statistics on its own
    # in-memory trace and never produces a line this parser can read — two
    # different pieces of arithmetic, and the untested one is the one that names
    # the plant on the day the take is spent.
    # `[-+]?`, not `-?`: the injector prints size and dx with %+d, so a
    # positive one arrives as "+16". With `-?` the pattern silently kept only
    # the negative-direction elements -- half the run, and exactly the half
    # that would have made a direction-dependent plant look symmetric.
    pid = re.findall(
        r"\[\s*(\d+\.\d+)\]\s+inject\s+plantid: pass=(\d+) elem=(\d+) "
        r"size=([-+]?\d+) phase=(\w+) k=(\d+) dx=([-+]?\d+) cam=(-?\d+\.\d+) "
        r"meas=(\d) camok=(\d)", text)
    if pid:
        m = re.search(r"X4VR_HEADLOOK_GAIN=(\d+(?:\.\d+)?)", run or "")
        gain = float(m.group(1)) if m else 0.115
        S = [dict(t=float(a), size=int(d), phase=e, k=int(f), dx=int(g),
                  cam=float(h), meas=i == "1", ok=j == "1")
             for a, b, c, d, e, f, g, h, i, j in pid]
        done = "sequence COMPLETE" in text
        elems = {(int(c), int(b)) for a, b, c, *_ in pid}
        print(f"plantid {len(S)} tick(s), {len(elems)} element-pass(es), "
              f"sequence {'COMPLETE' if done else 'INCOMPLETE — the run ended early, '
                                                 'so the largest sizes may be missing'}")

        # **Delivery is measured from the IMPULSE, over a window, not from the
        # step that issued the command.**
        #
        # X4 answers a mouse delta about two ticks later. Crediting the motion
        # to the commanding tick therefore charges each command with its
        # predecessor's motion and loses its own to the following phase -- and
        # because the sustain burst is SHORT for big commands (they eat the
        # runway fastest), the largest sizes lose the largest share. Take 213
        # read that as delivery collapsing from 94% to 33% and this scorer
        # called it a PER-STEP CAP. It was an artefact: summed over a window
        # that outlives the delay, every size delivers 100.0%.
        #
        # The impulse phase exists for exactly this: one command, then quiet.
        # Sum the whole response and the delay cannot bias it.
        deliv = {}
        lagprof = {}
        for i, s in enumerate(S):
            # `impulse` OR `coast`, to read both builds. Take 213 stamped each
            # line with the phase the sequence had moved ON to, so its single
            # impulse command arrives labelled `coast` with k=0; the build
            # after it stamps the phase that produced the command. Neither
            # spelling can collide with a sustained tick, which is labelled
            # `sustain` (or `decay` on the last one, same bug).
            if s["dx"] == 0 or s["phase"] not in ("impulse", "coast"):
                continue
            w = S[i:i + 10]
            if len(w) < 10 or not all(x["ok"] for x in w):
                continue
            if any(x["dx"] != 0 for x in w[1:]):
                continue
            if any(not (0 < w[j + 1]["t"] - w[j]["t"] < 0.5) for j in range(9)):
                continue
            sgn = -1.0 if s["dx"] > 0 else 1.0
            d = deliv.setdefault(abs(s["size"]), [0.0, 0.0, 0])
            d[0] += abs(s["dx"]) * gain
            d[1] += (w[9]["cam"] - w[0]["cam"]) * sgn
            d[2] += 1
            for j in range(8):
                lagprof.setdefault(j, []).append(
                    (w[j + 1]["cam"] - w[j]["cam"]) * sgn)
        # Where the response actually lands. This is a first-class number: it is
        # X4's input-to-camera transport delay, which nothing before take 213
        # had measured, and it is what made take 212's plant look nonlinear.
        conc, peakj = None, None
        if lagprof:
            tot = sum(sum(v) / len(v) for v in lagprof.values())
            print("plantid  impulse response, degrees per tick after the "
                  "command:")
            row = "           "
            peak, peakj = -1e9, 0
            for j in sorted(lagprof):
                m = sum(lagprof[j]) / len(lagprof[j])
                row += f"+{j}:{m:6.2f}  "
                if m > peak:
                    peak, peakj = m, j
            print(row)
            # Median tick period, not the gap between the first two samples:
            # those can straddle a phase boundary and mis-state the delay in ms
            # by 2x, which this line did on take 213 (42 ms against 26).
            gaps = sorted(b["t"] - a["t"] for a, b in zip(S, S[1:])
                          if 0 < b["t"] - a["t"] < 0.5)
            tick = gaps[len(gaps) // 2] if gaps else 0.0
            conc = peak / max(tot, 1e-9)
            # **"tick" was the wrong word and it cost a wrong reading.** Since
            # take 289 the sequence steps in the READBACK's time base, so `S` is
            # one sample per camera update and this gap is the camera update
            # period -- not X4VR_TICK_HZ, which in take 290a was 45.3 Hz against
            # the 29 Hz printed here. The milliseconds were always right; the
            # label invited comparing 290a's "2 ticks" with 290b's across two
            # different tick rates and concluding the delay was fixed in ticks.
            print(f"plantid  transport delay: the response peaks {peakj} camera "
                  f"update(s) after the command ({peakj * tick * 1000:.0f} ms at "
                  f"this {1/tick if tick else 0:.0f} Hz CAMERA UPDATE rate — not "
                  f"the servo tick), and {conc*100:.0f}% of it arrives in that "
                  f"one update")

        fracs = []
        if deliv:
            print("plantid  delivery per command size, summed over the whole "
                  "impulse response:")
            small = large = None
            for sz in sorted(deliv):
                c, mv, n = deliv[sz]
                if n < 5:
                    continue
                frac = mv / c if c else 0.0
                bar = "#" * int(round(frac * 20))
                print(f"           {sz:4d} counts ({sz*gain:5.2f} deg) n={n:4d}  "
                      f"delivered {frac*100:5.1f}%  {bar}")
                if small is None:
                    small = frac
                large = frac
                fracs.append(frac)
            fade = (large / small) if (small and small > 1e-6) else float("nan")
        else:
            small = large = fade = None
            print("plantid  no usable sustained steps — was the camera "
                  "read-back live? (camok=0 everywhere means X4 reported "
                  "nothing, not that it reported zero)")

        # The coast: with nothing commanded, how much motion survives?
        coast = []
        for i in range(1, len(S) - 1):
            a, b, c = S[i - 1], S[i], S[i + 1]
            if not (b["phase"] in ("coast", "decay") and b["dx"] == 0):
                continue
            if not (a["ok"] and b["ok"] and c["ok"] and b["meas"]):
                continue
            if not (0 < b["t"] - a["t"] < 0.5 and 0 < c["t"] - b["t"] < 0.5):
                continue
            prev = abs(b["cam"] - a["cam"])
            nxt = abs(c["cam"] - b["cam"])
            if prev > 0.05:
                coast.append(nxt / prev)
        if coast:
            coast.sort()
            cmed = coast[len(coast) // 2]
            print(f"plantid  coast: with nothing commanded the camera still "
                  f"travels {cmed*100:.0f}% of the previous step (n={len(coast)})")
        else:
            cmed = None
            print("plantid  coast UNMEASURED — no quiet step followed a moving "
                  "one")

        # Name it, or say plainly that it is none of the candidates.
        #
        # **Coast used to be checked first and alone, and that was wrong.** The
        # claim was that coast "separates a smoothing plant from a stiff one
        # whatever the delivery curve looks like". It does not separate either
        # from a plant with TRANSPORT DELAY: during COAST a delayed integrator is
        # still delivering the impulse from three ticks back, so it moves with
        # nothing commanded and reads 100% — the same as momentum, for a
        # completely different reason and with the opposite fix.
        #
        # Take 288 is that plant, and this code called it SMOOTHED / MOMENTUM.
        # It was validated against Integrator, Smoother and Capped, not one of
        # which had a delay; a control set that excludes the answer cannot catch
        # a wrong answer. tests/run-plantid-scorer.sh now carries a delayed
        # integrator and a delayed smoother as well.
        #
        # **What actually separates them is the SHAPE of the impulse response**,
        # which this function already computed and threw away: a delayed
        # integrator puts all of its response in ONE tick (100% on the synthetic
        # control, 92% on take 288), while a smoother spreads it (28% undelayed,
        # 72% delayed). Delay moves the response; smoothing widens it, and only
        # the width tells them apart.
        # **Is this run measurable at all? Take 289 was not, and I named a plant
        # from it anyway.**
        #
        # plantid's phases are counted in TICKS -- IMPULSE is one tick, COAST is
        # fourteen -- but the plant is only observable when X4's camera readback
        # CHANGES. Take 288 ran 2.5 ticks per camera update and every size
        # delivered 99.6-99.7%. Take 289 raised X4VR_TICK_HZ to 90 while the
        # camera update rate barely moved (12.1 -> 16.0 per second), giving 5.7
        # ticks per update: the one-tick impulse now usually falls entirely
        # between two readbacks, and delivery came back 49.8 / 83.0 / 124.6%.
        #
        # **124.6% is impossible.** An integrator cannot deliver more than it was
        # commanded, so that column is not a plant measurement and nothing
        # downstream of it means anything. The scorer printed "SMOOTHED /
        # MOMENTUM" from it regardless, contradicting take 288 on the same game.
        #
        # So: gate on the delivery column before naming anything. A flat column
        # is what a measurable run looks like; a scattered one says the sequence
        # out-ran the readback and the fix is the run's cadence, not the verdict.
        # **Gate on the CAUSE, not on the spread.** My first version refused any
        # delivery column spanning more than 25 points — which is precisely the
        # signature of a PER-STEP CAP, so it called two of the three plants it
        # exists to name "unmeasurable". A gate that rejects the answer is worse
        # than no gate. Caught by tests/run-plantid-scorer.sh, which is the only
        # reason it is not in this file.
        #
        # What is left is one genuine impossibility plus the direct measurement
        # of the failure: how many ticks pass per camera update. The IMPULSE is
        # ONE tick, so once the readback refreshes less often than every ~3
        # ticks, that tick usually falls between two readbacks and the impulse is
        # simply not observed.
        # **The ticks-per-update half of this gate is gone, because it measured
        # the wrong thing the moment the log changed under it.**
        #
        # It computed len(S)/changes over the logged lines. That was right while
        # the injector logged every tick. It now logs one line per SEQUENCE STEP,
        # and most of the sequence is SETTLE, COAST and DECAY — where nothing is
        # commanded and the camera is genuinely still, so the value does not
        # change because nothing happened, not because the readback is stale. On
        # take 290b it read 3.7 and refused a run whose delivery column was
        # 99.6% flat at every one of the seven sizes.
        #
        # The real ratio is measured in Calibrate, where the camera is certainly
        # moving, and the injector logs it: takes 290a/290b report 1.39 and 1.30.
        # Reported below as context, never gated on — an instrument that
        # re-derives a quantity the log already states can evaluate a different
        # one, which is what happened here.
        bad = None
        if fracs and max(fracs) > 1.10:
            bad = (f"delivery reaches {max(fracs)*100:.1f}% — nothing here can "
                   f"deliver MORE than it was commanded")
        if bad:
            print(f"plantid  UNMEASURABLE — {bad}.")
            print(f"plantid  Delivery is the validity signal: take 288 and both "
                  f"arms of 290 read 99.6% flat at every size, take 289 read "
                  f"49.8-124.6%. ")
            print(f"plantid  The phases are counted in TICKS but the plant is "
                  f"only visible when the camera readback CHANGES, and this run "
                  f"had {tpu:.1f} ticks per update. Take 288 ran 2.5 and read "
                  f"99.6% flat at every size; take 289 raised X4VR_TICK_HZ to 90, "
                  f"got 5.7, and read 49.8-124.6%. Re-run with the tick near X4's "
                  f"camera update rate, or lengthen plantid's phases so each "
                  f"covers the same number of UPDATES — do NOT read a plant off "
                  f"these numbers.")
        elif conc is None:
            print("plantid  signature UNAVAILABLE — no impulse response, so the "
                  "response WIDTH cannot be measured. Coast alone cannot name a "
                  "plant: see the note above.")
        elif conc >= 0.82:
            # One tick holds essentially all of it: the plant applies what it is
            # given, whole, after a fixed delay. `fade` then splits a faithful
            # integrator from one that saturates on large commands.
            #
            # Built up piecewise rather than as one f-string with a trailing
            # conditional: written that way, `... else ""` binds to the WHOLE
            # concatenation, so a run with no coast measurement printed a blank
            # line where its signature should be. I wrote that bug in this very
            # edit and caught it before running.
            sig = f"plantid  signature: {conc*100:.0f}% of the response in one tick"
            sig += " (concentrated)"
            if fade is not None and fade == fade:
                sig += f", fade {fade:.2f}x"
            if cmed is not None:
                sig += f", coast {cmed*100:.0f}%"
            print(sig)
            if fade is not None and fade == fade and fade < 0.75:
                print("plantid  => PER-STEP CAP. Delivery is flat for small "
                      "commands and collapses for large ones. Keep "
                      "max_step_deg under the knee above and the servo stays "
                      "in the linear region.")
            elif peakj == 0:
                print("plantid  => PURE INTEGRATOR, no delay. Every size "
                      "delivers what it asked for in the very next tick. There "
                      "is NO cap, NO momentum and NO nonlinearity, so the plant "
                      "is exactly what tests/servo_lag.cpp models.")
            else:
                print(f"plantid  => INTEGRATOR WITH TRANSPORT DELAY of {peakj} "
                      f"tick(s). Every size delivers what it asked for and the "
                      f"response arrives whole; there is NO momentum, NO cap "
                      f"and NO nonlinearity. **The coast figure above is the "
                      f"DELAY, not momentum** — during COAST the camera is "
                      f"still paying out the impulse from {peakj} ticks back. "
                      f"A proportional servo against dead time is the classic "
                      f"oscillator, and re-observing a reading it has already "
                      f"acted on multiplies its gain; that is where to look, "
                      f"not at the plant's linearity.")
        elif conc <= 0.75 and cmed is not None and cmed < 0.15:
            # **The two statistics contradict each other, so name neither.**
            # A smoother's defining property is that velocity PERSISTS: after the
            # command stops the camera keeps moving and decays. Coast measures
            # exactly that, and near zero it refutes momentum whatever the width
            # says. A wide response with no coast is the other thing that widens
            # a response -- jitter in WHEN a delayed impulse lands -- and take
            # 290b is that: 49% in one step against a coast of 0%.
            #
            # Reported as a conflict rather than resolved by moving a band. The
            # bands were set from planted plants; moving one to break a tie in
            # live data is fitting the instrument to the answer, which is how
            # this file got "SMOOTHED / MOMENTUM" on a delayed integrator in the
            # first place.
            print(f"plantid  signature CONTRADICTORY — {conc*100:.0f}% of the "
                  f"response in one step says SPREAD, but coast "
                  f"{cmed*100:.0f}% says the camera STOPS DEAD, and no smoother "
                  f"does both.")
            print("plantid  => NOT MOMENTUM (coast refutes it), but the delay is "
                  "not cleanly measured either — the width is being widened by "
                  "something other than the plant. No candidate is named.")
        elif conc <= 0.75:
            print(f"plantid  signature: only {conc*100:.0f}% of the response in "
                  f"one tick (spread over {peakj}+ ticks)"
                  + (f", coast {cmed*100:.0f}%" if cmed is not None else ""))
            print("plantid  => SMOOTHED / MOMENTUM. The response is WIDE, not "
                  "merely late: X4 filters the mouse input rather than applying "
                  "it. A servo that commands POSITION increments is the wrong "
                  "shape for this plant; the lead adding more command cannot "
                  "fix it (take 212, 0.94x).")
        else:
            print(f"plantid  signature AMBIGUOUS — {conc*100:.0f}% of the "
                  f"response in one tick falls between the concentrated (>=82%) "
                  f"and spread (<=75%) bands. No candidate is named. Naming one "
                  f"anyway is how the 200 deg/s story survived a day.")

    # --- task #75: X4's latency band, measured DURING PLAY ------------------
    #
    # An exogenous dither -- a few mouse counts the servo does not know it is
    # sending -- makes closed-loop identification valid. Correlating it against
    # the plant's total input and against the plant's output gives
    # `b = a (*) P`, and the servo's reaction to the dither appears on both sides
    # and divides out. Correlating against the output ALONE recovers P/(1+PC)
    # instead, which turned a planted 5.00 into 11.35 the first time.
    #
    # **This is a second implementation of arithmetic that already exists in
    # C++.** tests/run-dither-scorer.sh drives THIS code against planted bands
    # emitted by tests/dither_id.cpp, because the C++ one being right says
    # nothing about this one, and this one is what reads the take.
    dth = re.findall(
        r"\[\s*(\d+\.\d+)\]\s+inject\s+dither: v=([-+]?\d+) dx=([-+]?\d+) "
        r"cam=(-?\d+\.\d+)", text)
    if len(dth) > 500:
        m = re.search(r"X4VR_HEADLOOK_GAIN=(\d+(?:\.\d+)?)", run or "")
        g = float(m.group(1)) if m else 0.115
        T = [float(a) for a, _, _, _ in dth]
        V = [float(b) for _, b, _, _ in dth]
        # sign_yaw is -1: a positive dx drives the camera negative.
        U = [-float(c) * g for _, _, c, _ in dth]
        C = [float(d) for _, _, _, d in dth]
        n = len(V) - 1
        DC = [C[i + 1] - C[i] for i in range(n)]
        gaps = sorted(T[i + 1] - T[i] for i in range(n) if 0 < T[i + 1] - T[i] < 0.5)
        tick = gaps[len(gaps) // 2] if gaps else 0.0
        var = sum(v * v for v in V[:n])
        # **12 taps, not 28, and take 291 is why.** The forward substitution is
        # sequential, so P[0..12] are bit-identical at either setting -- ML only
        # decides how many taps enter `keep`. Beyond the plant's support those
        # taps are noise, and the 5% floor does not remove them because the floor
        # is relative to a peak the noise can itself set. Replaying take 291's own
        # timestamps and command sequence with a HEALTHY 40-165 ms band planted in
        # place of X4 read mass 1.43 at ML=28; the take read 1.46. The instrument
        # produced that verdict, not X4. At 43.5 Hz, 12 taps is 276 ms -- past any
        # latency this servo stays stable at.
        ML = 12
        if var < 1e-9:
            print("dither   UNMEASURABLE — the dither column is all zeros")
        else:
            A = [sum(V[i - L] * U[i] for i in range(L, n)) / var for L in range(ML + 1)]
            B = [sum(V[i - L] * DC[i] for i in range(L, n)) / var for L in range(ML + 1)]
            if abs(A[0]) < 1e-9:
                print("dither   UNMEASURABLE — the dither never reached the plant input")
            else:
                P = [0.0] * (ML + 1)
                for L in range(ML + 1):
                    acc = B[L] - sum(A[L - k] * P[k] for k in range(L))
                    P[L] = acc / A[0]
                peak = max(P)
                # Taps under 5% of the peak are noise; counting them biased every
                # planted band by +1.5 ticks of mean and +2.3 of spread, all in
                # the same direction.
                keep = [(L, p) for L, p in enumerate(P) if p > 0.05 * peak]
                tot = sum(p for _, p in keep)
                if tot < 1e-6:
                    print("dither   UNMEASURABLE — no positive impulse response")
                else:
                    mu = sum(L * p for L, p in keep) / tot
                    sd = (sum(p * (L - mu) ** 2 for L, p in keep) / tot) ** 0.5
                    print(f"dither   {n} tick(s) at {1/tick if tick else 0:.1f} Hz, "
                          f"dither amplitude {int(max(abs(v) for v in V))} count(s)")
                    print("dither   impulse response, share delivered per tick "
                          "after the command:")
                    print("           " + "  ".join(
                        f"+{L}:{P[L]:5.2f}" for L in range(min(10, ML + 1))))
                    print(f"dither   latency {mu*tick*1000:.0f} ms mean "
                          f"({mu:.2f} ticks), recovered mass {tot:.2f}")
                    # The tap spread is NOT the plant's spread. Planting bands of
                    # the same MEAN and varying WIDTH on take 291's own data moved
                    # this number less than the seed-to-seed scatter did, while
                    # varying the mean at fixed width moved it cleanly. It is a
                    # second reading of the mean wearing a spread's units, so it
                    # is printed as diagnostics and never as a measurement.
                    print(f"dither   (tap spread {sd*tick*1000:.0f} ms / "
                          f"{sd:.2f} ticks — conditioning, NOT the plant's "
                          f"jitter; see below)")
                    # **The instrument's own range statement, calibrated rather
                    # than assumed.** Replaying take 291's real timestamps and
                    # real command sequence with a known band planted in place of
                    # X4, at this run's dither amplitude:
                    #
                    #   truth ms   25    50    70    90   110   140 | 180   220   300
                    #   read ms    41    64    63    88   109   124 | 148   121   138
                    #   mass     1.14  1.07  0.87  0.81  0.76  0.53 |0.35  0.14  0.23
                    #
                    # Mass falls monotonically with the mean and separates what is
                    # reachable (0.53-1.14) from what is past the window
                    # (0.14-0.41) with no overlap. The old gate demanded mass
                    # within 0.05 of 1.0, which every one of those healthy rows
                    # fails -- it was calibrated on a quiet synthetic servo, and
                    # in ordinary play the player's own steering carries 51x the
                    # instrument's variance.
                    if tot < 0.45:
                        print(f"dither   => OUT OF REACH. Mass {tot:.2f} is below "
                              f"the 0.45 floor, which means the true latency is "
                              f"past this window ({ML} taps = "
                              f"{ML*tick*1000:.0f} ms). The mean above is a "
                              f"truncation artefact — do NOT read it.")
                    elif tot > 1.35:
                        # **Both ends, and the first draft only had one.** A mass
                        # of 3.92 sailed through a lone `< 0.45` floor and the
                        # next branch announced "the mass is fine" about a number
                        # it had never looked at. Every healthy row of the
                        # calibration above sits under 1.14.
                        print(f"dither   => ILL-CONDITIONED. Mass {tot:.2f} is far "
                              f"above the 1.14 the calibration reaches, so the "
                              f"deconvolution diverged rather than measured. Seen "
                              f"when the servo is slamming against X4's stops: "
                              f"the plant is not linear there and this instrument "
                              f"cannot see through it. Read the `clamp` and "
                              f"`poselag` blocks instead.")
                    elif mu > 6.0:
                        print(f"dither   => OUT OF RANGE. A mean of {mu:.2f} ticks "
                              f"is past the stability threshold this servo slams "
                              f"at; the mass is fine but the regime is not.")
                    else:
                        lo, hi = mu*tick*1000 - 20, mu*tick*1000 + 20
                        print(f"dither   => IN RANGE, mass {tot:.2f}. X4's "
                              f"input-to-camera latency with the servo RUNNING is "
                              f"{lo:.0f}-{hi:.0f} ms (the calibration above is "
                              f"good to about +-20 ms). Open-loop plantid got "
                              f"40-165 ms in takes 290a/290b.")
                        print(f"dither   => the WIDTH of the band is NOT measured "
                              f"here. At {int(max(abs(v) for v in V))} count(s) "
                              f"this instrument resolves the mean and nothing "
                              f"finer; a tight band and a 40-165 ms one are "
                              f"indistinguishable at this amplitude.")

    # --- task #75: the kp A/B, scored on the WORSE of overshoot and lag ------
    #
    # The servo overshoots its own target by 0.5 x dead_time x max_step, and the
    # levers that shrink that all buy it back as lag. So neither number alone is
    # the score: both are the neck and the view disagreeing, and the honest
    # figure is the larger. Modelled, paired, dead time 3:
    #
    #                  350 deg/s over/lag/WORST     500 deg/s WORST
    #   kp 0.25          22.00 / 16.70 / 22.00           27.17
    #   kp 0.15          16.59 / 16.70 / 16.70           23.81
    #
    # and identical at 120 deg/s, where nothing differs. So the prediction is a
    # change in the TAIL at speed and none in the middle -- reporting a median
    # would report a null however well it worked.
    lag = re.findall(
        r"\[\s*(\d+\.\d+)\] inject  lagsample: arm=\d+ view=(\w+) steer=(\d) "
        r"cam=(\d) head=(-?[\d.]+),(-?[\d.]+) x4=(-?[\d.]+),(-?[\d.]+) "
        r"kparm=(-?\d+) kp=([\d.]+) ylim=([\d.]+)", text)
    if lag:
        # **The timestamp travels IN the row.** The rate below differences two
        # samples and calls the result deg/s, which is only true if they are
        # adjacent ticks -- and a parallel list of times would drift out of step
        # with the filtering the way windows_t() did.
        rows = [(v, int(st), int(cv), float(hy), float(x4y), int(ka), float(kp),
                 float(yl), float(t))
                for t, v, st, cv, hy, _hp, x4y, _x4p, ka, kp, yl in lag]
        use = [r for r in rows if r[0] == "cockpit" and r[1] and r[2]]
        arms = sorted({r[5] for r in use})
        print(f"kpab   {len(lag)} lagsample(s), {len(use)} in cockpit with "
              f"steering and a live read-back; kp arm(s) {arms}")
        if -1 in arms and len(arms) == 1:
            print("kpab   no kp A/B in this run — X4VR_HEADLOOK_KP_AB unset, so "
                  "this says nothing about kp")
        elif len(arms) < 2:
            print(f"kpab   ONLY ONE ARM present ({arms}) — the run ended inside "
                  f"a single phase, or the A/B never flipped. Not scorable.")
        else:
            # Head rate from consecutive samples; the effect lives in the fast tail.
            # **Bin by the rate that CAUSED the error, not the one beside it.**
            # The error at tick i is paid out from commands issued ~3 ticks
            # earlier (take 291's measured dead time), so binning on the current
            # rate files a fast sweep's overshoot under "slow" the moment the
            # head stops — which is exactly when the overshoot arrives. On the
            # planted fixture that leaked a p90 of 19.74 deg into the under-150
            # band, where nothing should exceed 5.
            LAGT = 3
            # **Take 294: the rate must come from ADJACENT samples.**
            #
            # Differencing use[i-4] and use[i-3] and calling it deg/s is only
            # true if those two are consecutive ticks. A menu, a view change or
            # any steer=0 stretch leaves a gap in `use`, and the difference
            # across it is not a rate at all: take 294's over-350 band contained
            # "head rates" of 1738 and 2367 deg/s, against a human maximum
            # around 500-800 and a whole-run head travel of 77 deg. Those
            # samples then carried the episodes into the band the servo model
            # was supposed to own.
            #
            # The whole window i-4..i must be gap-free, because the error at i is
            # attributed to the command issued at i-3.
            GAP = 0.060   # s; the p99 tick is 56 ms, so this admits every real one
            def adjacent(i):
                for k in range(i - LAGT - 1, i):
                    dt = use[k + 1][8] - use[k][8]
                    if not (0.0 < dt <= GAP):
                        return False
                return True
            # Rate in deg/s from the ACTUAL interval, not a nominal tick. The old
            # fixed 63.0 was wrong by up to 4.4x on its own, since the tick
            # wanders 9-56 ms.
            def band(lo, hi):
                out = {}
                for i in range(LAGT + 1, len(use)):
                    if not adjacent(i):
                        continue
                    a_, b_ = use[i - LAGT - 1], use[i - LAGT]
                    rate = abs(b_[3] - a_[3]) / (b_[8] - a_[8])
                    if not (lo <= rate < hi):
                        continue
                    b_ = use[i]                        # ...but score THIS sample
                    tgt = max(-b_[7], min(b_[7], b_[3]))   # the servo's own target
                    out.setdefault(b_[5], []).append(abs(b_[4] - tgt))
                return out
            dropped = sum(1 for i in range(LAGT + 1, len(use))
                          if not adjacent(i))
            if dropped:
                print(f"kpab   {dropped} of {len(use)-LAGT-1} sample(s) sit "
                      f"across a gap wider than {1000*GAP:.0f} ms and are "
                      f"excluded — their head 'rate' would be an artifact of the "
                      f"gap, not a measurement")
            print(f"{'':7}{'head deg/s':<14}{'arm':<9}{'n':>6}{'p50':>8}"
                  f"{'p90':>8}{'WORST':>9}")
            for lo, hi, label in ((0, 150, "under 150"), (150, 350, "150-350"),
                                  (350, 9e9, "over 350")):
                g = band(lo, hi)
                for arm in sorted(g):
                    e = sorted(g[arm])
                    if len(e) < 20:
                        continue
                    name = "control" if arm == 0 else "ARM_B"
                    print(f"kpab   {label:<14}{name:<9}{len(e):>6}"
                          f"{e[len(e)//2]:>8.2f}{e[int(0.90*len(e))]:>8.2f}"
                          f"{e[-1]:>9.2f}")
            fast = band(350, 9e9)
            if 0 in fast and 1 in fast and len(fast[0]) >= 20 and len(fast[1]) >= 20:
                kpv = {r[5]: r[6] for r in use}
                # **Judge a QUANTILE, not two maxima.** Take 294 compared
                # max(140 samples) against max(67) and called the result a null.
                # An extreme-value comparison at unequal n is not a measurement:
                # the larger sample wins by having more draws. p90 exists in both
                # arms and moves with the effect the model predicts.
                e0, e1 = sorted(fast[0]), sorted(fast[1])
                w0, w1 = e0[int(0.90*len(e0))], e1[int(0.90*len(e1))]
                print(f"kpab   => over 350 deg/s the p90 neck-vs-view "
                      f"disagreement is {w0:.2f} deg at kp {kpv.get(0, 0):.3f} "
                      f"(n={len(e0)}) and {w1:.2f} at kp {kpv.get(1, 0):.3f} "
                      f"(n={len(e1)}); the maxima are {max(e0):.2f} and "
                      f"{max(e1):.2f} and are NOT the verdict")

                # **PER-PHASE CONSISTENCY, and it outranks the pooled number.**
                #
                # The arms alternate every N seconds, and an episode lasts tens
                # of seconds, so a phase is the unit of replication and there are
                # only four or five of them per arm. Take 294's pooled control
                # p90 of 25.14 against ARM_B's 3.84 was carried ENTIRELY by
                # phase 8 -- the other three control phases read 1.68, 4.25 and
                # 5.34, below two of the three ARM_B phases. Pooling hid that.
                ph = [(float(t), int(n)) for t, n in re.findall(
                    r"\[\s*(\d+\.\d+)\] inject  headlook: kp \w+ \([\d.]+, "
                    r"lead [\d.]+x, A/B phase (\d+)", text)]
                if len(ph) >= 4:
                    def phase_of(ts):
                        p = None
                        for pt, n in ph:
                            if pt <= ts:
                                p = n
                            else:
                                break
                        return p
                    per = {}
                    for i in range(LAGT + 1, len(use)):
                        if not adjacent(i):
                            continue
                        a_, b_ = use[i - LAGT - 1], use[i - LAGT]
                        if abs(b_[3] - a_[3]) / (b_[8] - a_[8]) >= 150.0:
                            continue          # the slow band: most of the mass
                        b_ = use[i]
                        k = phase_of(b_[8])
                        if k is None:
                            continue
                        tgt = max(-b_[7], min(b_[7], b_[3]))
                        per.setdefault((k, b_[5]), []).append(abs(b_[4] - tgt))
                    tbl = {}
                    for (k, arm), v in sorted(per.items()):
                        if len(v) < 50:
                            continue
                        v = sorted(v)
                        tbl.setdefault(arm, []).append((k, v[int(0.90*len(v))]))
                    if 0 in tbl and 1 in tbl and len(tbl[0]) >= 2 and len(tbl[1]) >= 2:
                        print(f"kpab   under 150 deg/s, p90 per PHASE "
                              f"(the unit of replication):")
                        for arm in (0, 1):
                            s = "  ".join(f"ph{k}:{p:.2f}" for k, p in tbl[arm])
                            print(f"kpab     {'control' if arm==0 else 'ARM_B':<8} {s}")
                        lo0 = min(p for _, p in tbl[0])
                        lo1 = min(p for _, p in tbl[1])
                        hi0 = max(p for _, p in tbl[0])
                        hi1 = max(p for _, p in tbl[1])
                        if lo0 > hi1 or lo1 > hi0:
                            print(f"kpab   => SEPARATED — every phase of one arm "
                                  f"beats every phase of the other. The arms "
                                  f"really do differ.")
                        else:
                            print(f"kpab   => the per-phase ranges OVERLAP "
                                  f"(control {lo0:.2f}-{hi0:.2f}, ARM_B "
                                  f"{lo1:.2f}-{hi1:.2f}), so any pooled "
                                  f"difference is phase assignment, not kp. "
                                  f"**This run does not answer the kp question** "
                                  f"whatever the pooled line above says.")
                if w1 < 0.85 * w0:
                    print("kpab   => pooled: ARM_B is better, as the model "
                          "predicted — but read the per-phase line first")
                elif w1 > 1.15 * w0:
                    print("kpab   => pooled: ARM_B is WORSE — read the per-phase "
                          "line before treating that as a refutation")
                else:
                    print("kpab   => pooled: no difference beyond 15%. The model "
                          "predicted 22.00 -> 16.70. A null is only a refutation "
                          "if the per-phase line above says SEPARATED.")
            else:
                print("kpab   the over-350 deg/s band has too few samples in one "
                      "arm to compare — the effect only exists there, so this run "
                      "did not exercise it. Turn the head FASTER, not more.")

    # --- task #75 take 295: the rate fix A/B, scored as a TIME ---------------
    #
    # The complaint is "the visuals take time to follow up on the movement", so
    # the score is that time, in milliseconds, and not an angle. The error of a
    # proportional servo tracking a moving head is (head_rate * lag), so dividing
    # the rms error by the rms head rate within an arm gives a lag that does not
    # depend on how fast the player happened to move during that arm -- which
    # matters, because the arms alternate and the player is not controlled.
    #
    # PREDICTED, from tests/servo_rate.cpp section 11 driven by take 294's own
    # tick sequence:  control 80 ms -> ARM_B 50 ms at nominal 100 Hz (tau 40 ms).
    # The harness reproduced take 294's measured 85 ms as 90.4 without being
    # fitted to it, so the control arm is a check on the harness as well.
    rf = re.findall(
        r"\[\s*(\d+\.\d+)\] inject  lagsample: arm=\d+ view=(\w+) "
        r"steer=(\d) cam=(\d) head=(-?[\d.]+),(-?[\d.]+) "
        # ylim is CAPTURED now, not skipped. Without it nothing downstream can
        # tell a lag sample from one taken with the camera pinned at its stop,
        # which is what made take 305 unscorable without saying so.
        r"x4=(-?[\d.]+),(-?[\d.]+) kparm=-?\d+ kp=[\d.]+ ylim=([\d.]+) "
        # **Tolerant of both spellings and of the field being absent.** Take
        # 295 logged `rfhz=` before the knob was split into tau + slew, and no
        # `ugarm=` at all; a regex that demands the newest shape silently
        # unscores every earlier run. The control suite caught this the moment
        # it was written the strict way.
        r"rfarm=(-?\d+) rf(?:hz|tau)=([\d.]+)(?: ugarm=(-?\d+))?", text)
    if rf:
        # ylim APPENDED at index 9, so every existing index keeps its meaning.
        rr = [(float(t), v, int(st), int(cv), float(hy), float(x4y), int(ar),
               float(hz), int(ug) if ug else -1, float(yl))
              for t, v, st, cv, hy, _hp, x4y, _x4p, yl, ar, hz, ug in rf]
        ck = [r for r in rr if r[1] == "cockpit" and r[2] and r[3]]
        arms = sorted({r[6] for r in ck})
        print(f"rfab   {len(rf)} lagsample(s) carry rfarm; {len(ck)} in cockpit "
              f"with steering and a live read-back; arm(s) {arms}")
        if len(arms) < 2:
            print("rfab   ONLY ONE ARM — X4VR_HEADLOOK_TAU_AB unset, or the "
                  "run ended inside one phase. Not scorable.")
        else:
            hz = max(r[7] for r in ck)
            def lag_ms(rows):
                """rms error / rms head rate, in ms. Adjacent samples only: the
                rate is a derivative and a gap makes it an artifact — the same
                defect kpab carried until take 294."""
                es, rs, n = 0.0, 0.0, 0
                for a_, b_ in zip(rows, rows[1:]):
                    dt = b_[0] - a_[0]
                    if not (0.0 < dt <= 0.060):
                        continue
                    e = b_[5] - b_[4]
                    r_ = (b_[4] - a_[4]) / dt
                    es += e * e
                    rs += r_ * r_
                    n += 1
                if n < 200 or rs <= 0.0:
                    return None, n
                return 1000.0 * math.sqrt(es / n) / math.sqrt(rs / n), n
            # Per PHASE first — the lesson take 294 taught the kp block.
            ph, cur = [], None
            for r in ck:
                if r[6] != cur:
                    ph.append([])
                    cur = r[6]
                ph[-1].append(r)
            per = {}
            for seg in ph:
                if len(seg) < 300:
                    continue
                L, n = lag_ms(seg)
                if L is not None:
                    per.setdefault(seg[0][6], []).append(L)
            print(f"{'':7}{'arm':<16}{'phases':>8}{'lag ms each':>34}")
            for arm in sorted(per):
                name = "control (off)" if arm == 0 else f"ARM_B tau{hz:.0f}ms"
                vals = "  ".join(f"{v:.0f}" for v in per[arm])
                print(f"rfab   {name:<16}{len(per[arm]):>8}{vals:>34}")
            if 0 in per and 1 in per and len(per[0]) >= 2 and len(per[1]) >= 2:
                m0 = sorted(per[0])[len(per[0]) // 2]
                m1 = sorted(per[1])[len(per[1]) // 2]
                print(f"rfab   => median lag {m0:.0f} ms control, {m1:.0f} ms "
                      f"ARM_B; predicted 80 -> 50")
                if max(per[1]) < min(per[0]):
                    print(f"rfab   => SEPARATED — every ARM_B phase beats every "
                          f"control phase. The fix works and should ship on.")
                elif min(per[1]) > max(per[0]):
                    print(f"rfab   => SEPARATED THE WRONG WAY — every ARM_B "
                          f"phase is worse. The fix is refuted; leave it off.")
                else:
                    print(f"rfab   => the per-phase ranges OVERLAP (control "
                          f"{min(per[0]):.0f}-{max(per[0]):.0f}, ARM_B "
                          f"{min(per[1]):.0f}-{max(per[1]):.0f}). Whatever the "
                          f"medians say, this run does not separate the arms — "
                          f"the same trap take 294's kp A/B fell into.")
            else:
                print("rfab   too few complete phases per arm to compare — the "
                      "run needs at least two of each, so it must outlast four "
                      "A/B periods")
            # Take 294's fault, counted rather than assumed gone.
            ref = len(re.findall(r"headlook: REFUSED read-back", text))
            rsy = len(re.findall(r"headlook: RESYNC to", text))
            print(f"rfab   read-back guard: {ref} refusal(s), {rsy} resync(s). "
                  + ("Zero refusals means either the guard never fired or the "
                     "fault did not recur — the `clamp` plateau tells them apart."
                     if not ref else
                     "Each refusal is a 100-deg excursion that did NOT happen."))

    # --- task #75: the freshness gate, scored the same way as the rate fix ---
    #
    # Same stream, same statistic, split on `ugarm` instead. PREDICTED from
    # tests/servo_rate.cpp section 12 at take 295's measured rates:
    #
    #   gated (today)   tick 23.9 ms   lag 117.0 ms   worst |cam| 72.45
    #   UNGATED         tick 11.9 ms   lag  53.2 ms   worst |cam| 78.55
    #
    # so ARM_B should roughly HALVE the lag and make the excursion somewhat
    # worse while staying inside the limit. The excursion is the thing to watch:
    # doubling the iteration rate doubles the motion in flight at a fixed
    # per-tick step, which is the term that killed X4VR_TICK_HZ.
    if rf and len({r[8] for r in rr if r[8] >= 0}) >= 2:
        ck = [r for r in rr if r[1] == "cockpit" and r[2] and r[3]]
        print(f"ugab   {len(ck)} cockpit tick(s) with steering and a live "
              f"read-back; ungate arm(s) {sorted({r[8] for r in ck})}")
        # **CLAMP SATURATION VOIDS THIS STATISTIC, and take 305 is why.**
        #
        # The lag estimate is rms(command error) / rms(head rate). When the
        # head goes past the servo's own yaw ceiling the camera stops at the
        # stop while the head keeps going, so the "error" is the head's
        # excursion and carries no timing information at all. Take 305 ran
        # 35% of its samples with the camera pinned at 65.00 and a head
        # reaching 176.7 deg; the block printed lags of 229-850 ms — against
        # take 294's 85 — and a confident OVERLAP verdict from them.
        #
        # Worse, the two arms saturated by DIFFERENT amounts (52% gated vs
        # 33% ungated), so dropping the saturated samples does not rescue it
        # either: the survivors are then two different populations, and the
        # same data yields SEPARATED-THE-WRONG-WAY instead. One dataset, four
        # answers, chosen by how the contamination is handled — which means
        # the instrument is what is being measured.
        #
        # So this refuses rather than reports. A run that wants this number
        # must keep the head inside the servo's ceiling.
        ug_scorable = True
        sat = [r for r in ck if r[9] > 0.0 and abs(r[5]) >= r[9] - 0.01]
        if ck and len(sat) > 0.10 * len(ck):
            per_arm = {}
            for r in ck:
                per_arm.setdefault(r[8], [0, 0])
                per_arm[r[8]][1] += 1
                if r[9] > 0.0 and abs(r[5]) >= r[9] - 0.01:
                    per_arm[r[8]][0] += 1
            share = ", ".join(
                f"{'gated' if a == 0 else 'ungated'} "
                f"{100 * c[0] / max(1, c[1]):.0f}%"
                for a, c in sorted(per_arm.items()))
            print(f"ugab   NOT SCORABLE — {100 * len(sat) / len(ck):.0f}% of "
                  f"samples have the camera AT its yaw stop ({share}). The "
                  f"lag estimate divides a command error by a head rate, and "
                  f"at the stop the error is the head's excursion past a "
                  f"camera that cannot follow — it carries no timing. Re-run "
                  f"with the head kept INSIDE the servo's ceiling.")
            print("ugab   (take 305 read 229-850 ms this way against take "
                  "294's 85, and the arms saturated 52% vs 33% so removing "
                  "them leaves two different populations, not a clean pair)")
            ug_scorable = False
        ph, cur = [], None
        for r in ck:
            if r[8] != cur:
                ph.append([])
                cur = r[8]
            ph[-1].append(r)
        per, rate = {}, {}
        for seg in ph if ug_scorable else []:
            if len(seg) < 300:
                continue
            es, rs, n = 0.0, 0.0, 0
            for a_, b_ in zip(seg, seg[1:]):
                dt = b_[0] - a_[0]
                if not (0.0 < dt <= 0.060):
                    continue
                es += (b_[5] - b_[4]) ** 2
                rs += ((b_[4] - a_[4]) / dt) ** 2
                n += 1
            if n < 200 or rs <= 0.0:
                continue
            per.setdefault(seg[0][8], []).append(
                1000.0 * math.sqrt(es / n) / math.sqrt(rs / n))
            rate.setdefault(seg[0][8], []).append(
                1000.0 * (seg[-1][0] - seg[0][0]) / max(1, len(seg) - 1))
        print(f"{'':7}{'arm':<18}{'tick ms':>9}{'phases':>8}{'lag ms each':>28}")
        for arm in sorted(per):
            name = "control (gated)" if arm == 0 else "ARM_B (ungated)"
            tk = sum(rate[arm]) / len(rate[arm])
            vals = "  ".join(f"{v:.0f}" for v in per[arm])
            print(f"ugab   {name:<18}{tk:>9.1f}{len(per[arm]):>8}{vals:>28}")
        if 0 in per and 1 in per and len(per[0]) >= 2 and len(per[1]) >= 2:
            if max(per[1]) < min(per[0]):
                print("ugab   => SEPARATED — every ungated phase has less lag "
                      "than every gated one, as section 12 predicted.")
            elif min(per[1]) > max(per[0]):
                print("ugab   => SEPARATED THE WRONG WAY — ungating made the lag "
                      "worse. Keep the gate.")
            else:
                print(f"ugab   => the per-phase ranges OVERLAP (gated "
                      f"{min(per[0]):.0f}-{max(per[0]):.0f}, ungated "
                      f"{min(per[1]):.0f}-{max(per[1]):.0f}), so this run does "
                      f"not separate them whatever the medians read.")
        elif ug_scorable:
            print("ugab   too few complete phases per arm — the run must "
                  "outlast four A/B periods")
        # When not scorable the refusal above already said why. Printing
        # "too few complete phases" underneath it names a second, wrong cause
        # for the same silence, and the next reader fixes the run length
        # instead of the protocol.

        # **The cost side, reported whether or not the LAG was scorable.**
        # A halved lag bought with a spinning frustum is not a win — take 295
        # is why this sits beside the lag rather than being left for the
        # player to notice. It used to live inside the comparison branch, so
        # a run that could not be scored for lag also silently lost its
        # excursion report, which is the half that does NOT depend on the
        # head having stayed inside the ceiling.
        for arm in sorted({r[8] for r in ck}):
            seg = [r for r in ck if r[8] == arm]
            if not seg:
                continue
            worst = max(abs(r[5]) for r in seg)
            off = sum(1 for r in seg if abs(r[4]) < 15.0
                      and abs(r[5] - r[4]) > 30.0)
            print(f"ugab   {'gated' if arm == 0 else 'ungated':<8} worst |X4| "
                  f"{worst:6.2f} deg, {off} sample(s) with the camera 30+ "
                  f"deg off a centred head")

    # --- task #73: the head-look lag, and whether the lead removes it -------
    #
    # WHAT A HEALTHY RUN READS, stated before the numbers are read:
    #
    #   tau IDLE  ~ 3 x the pose period      (37 ms at 90 Hz, 150 ms at 20 fps)
    #   tau ARMED < 25% of IDLE, at a MATCHED pose rate
    #   still ARMED   the camera's drift with the head held still -- the price
    #
    # tau is the slope of tracking error against head rate. A slope does not
    # need fine sampling, only a spread of speeds, which is why the same
    # measurement was possible from `camread` at one sample in 30 -- but that
    # cannot tell which side of an A/B boundary a sample fell on, so the
    # per-pose `lagsample` line exists for this.
    #
    # The arms alternate WITHIN one run on purpose. The lag is proportional to
    # the pose period, so two separate takes would differ in frame rate as well
    # as in the lead, and no comparison between them could separate a servo fix
    # from a frame-time change. The pose rate is therefore printed per arm, and
    # a mismatch is called out rather than silently averaged over.
    lag = re.findall(
        r"\[\s*(\d+\.\d+)\]\s+inject\s+lagsample: arm=(\d) view=(\w+) "
        r"steer=(\d) cam=(\d) head=(-?\d+\.\d+),(-?\d+\.\d+) "
        r"x4=(-?\d+\.\d+),(-?\d+\.\d+)", text)
    if lag:

        # Only samples where the question is even defined: the cockpit (the
        # external camera is a DIFFERENT camera -- task #72 -- and mixing them
        # is what made take 208 report a false MOVED), a live readback, and the
        # servo actually steering.
        # **And not against X4's stop.** The servo targets its own limits, which
        # default to X4's shipped 65/35; past those the error is the CLAMP, not
        # the lag, and it persists at near-zero head rate — exactly the shape
        # that drags a regression. Read from the run line so a take with the
        # clamp extension enabled measures against the limits it actually used.
        def limit_of(name, dflt):
            m = re.search(name + r"=(\d+(?:\.\d+)?)", run or "")
            return float(m.group(1)) if m else dflt

        ylim = limit_of("X4VR_HEADLOOK_YAW_LIMIT", 65.0)
        plim = limit_of("X4VR_HEADLOOK_PITCH_LIMIT", 35.0)
        usable = [(float(t), int(a), float(hy), float(hp), float(xy), float(xp))
                  for t, a, v, st, cm, hy, hp, xy, xp in lag
                  if v == "cockpit" and st == "1" and cm == "1"
                  and abs(float(hy)) < ylim - 2.0
                  and abs(float(hp)) < plim - 2.0]
        if lag and not usable:
            print(f"lagfit  every sample was outside the cockpit, unsteered, "
                  f"or past the {ylim:.0f}/{plim:.0f} deg servo limits — "
                  f"nothing to fit")
        arms = {}
        for i in range(1, len(usable)):
            t0, a0, hy0, hp0, xy0, xp0 = usable[i - 1]
            t1, a1, hy1, hp1, xy1, xp1 = usable[i]
            dt = t1 - t0
            # Both ends inside the same arm, or the pair straddles a toggle and
            # says nothing about either.
            if dt <= 0 or dt > 0.5 or a0 != a1:
                continue
            d = arms.setdefault(a1, {"yaw": [], "pitch": [], "dt": [],
                                     "still": [], "wild": 0})
            d["dt"].append(dt)
            # **Reject head rates the servo could not have tracked anyway.**
            #
            # max_step_deg is 12 per pose, so the camera's own ceiling is
            # 12 x pose_rate deg/s. Above that the error is set by the STEP
            # CLAMP, not by the lag, and the linear model does not apply --
            # so such a sample measures a different thing while carrying
            # enormous leverage in a least squares fit.
            #
            # This is not hypothetical. Six samples out of 3186, from a
            # discontinuity in a synthetic trajectory, moved tau from 140 ms to
            # 69 -- a 2x error from 0.2% of the data, because least squares
            # weights by rate SQUARED and those had 1675 deg/s. A tracker
            # hiccup or a dropped pose in a real run does the same thing.
            ceiling = 12.0 / max(dt, 1e-6)
            ry = wrap180(hy1 - hy0) / dt
            rp = (hp1 - hp0) / dt
            if abs(ry) > ceiling or abs(rp) > ceiling:
                d["wild"] += 1
                continue
            d["yaw"].append((ry, wrap180(hy1 - xy1)))
            d["pitch"].append((rp, hp1 - xp1))
            # The price of the lead: how fast the camera moves when the head is
            # NOT moving. Predicted before the run at ~5 deg/s for 0.05 deg of
            # tracker noise, so this is read against a number, not impressions.
            #
            # **Held still for a WHILE, and judged on NET displacement.** Two
            # earlier versions of this were wrong in opposite ways. Testing one
            # interval scored a head reversing direction as "still" -- an unled
            # camera is at its furthest behind exactly there -- and reported the
            # control arm as the jittery one, which is backwards. Then requiring
            # every interval to be under 0.05 deg made it unmeasurable, because
            # the tracker noise this exists to measure is itself 0.05 deg, so
            # the predicate could never fire on the very signal it is about.
            #
            # Net displacement over a window rejects noise (it averages out) but
            # not real motion. The camera is then judged on PATH length, which
            # noise does not average out -- that is the jitter a player sees.
            W = 5
            if i >= W and all(usable[j][1] == a1 for j in range(i - W, i + 1)):
                span = usable[i][0] - usable[i - W][0]
                net_y = abs(wrap180(usable[i][2] - usable[i - W][2]))
                net_p = abs(usable[i][3] - usable[i - W][3])
                if 0 < span < 1.5 and net_y < 0.5 and net_p < 0.5:
                    path = sum(abs(wrap180(usable[j][4] - usable[j - 1][4]))
                               for j in range(i - W + 1, i + 1))
                    d["still"].append(path / span)

        def slope(pairs):
            n = len(pairs)
            if n < 30:
                return None
            sw = sum(w for w, _ in pairs)
            se = sum(e for _, e in pairs)
            sww = sum(w * w for w, _ in pairs)
            swe = sum(w * e for w, e in pairs)
            den = n * sww - sw * sw
            if abs(den) < 1e-9:
                return None
            return (n * swe - sw * se) / den

        names = {0: "IDLE (control)", 1: "ARMED (lead)"}
        taus = {}
        for a in sorted(arms):
            d = arms[a]
            hz = len(d["dt"]) / max(sum(d["dt"]), 1e-6)
            ty, tp = slope(d["yaw"]), slope(d["pitch"])
            still = (sorted(d["still"])[len(d["still"]) // 2]
                     if d["still"] else float("nan"))
            taus[a] = (ty, tp, hz)
            sy = f"{ty * 1000:7.1f}" if ty is not None else "      ?"
            sp = f"{tp * 1000:7.1f}" if tp is not None else "      ?"
            wild = (f"  dropped {d['wild']} past the servo's own "
                    f"{12.0 * hz:.0f} deg/s ceiling" if d["wild"] else "")
            print(f"lagfit  {names.get(a, a):15s} n={len(d['yaw']):5d} "
                  f"pose {hz:5.1f} Hz  tau yaw {sy} ms  pitch {sp} ms  "
                  f"still {still:5.2f} deg/s{wild}")

        if 0 not in taus or 1 not in taus:
            print("lagfit  only one arm present — the A/B needs both, so this "
                  "run cannot judge the lead. Check X4VR_HEADLOOK_LEAD_AB was "
                  "set and the run outlasted one full period")
        else:
            (iy, ip, ihz), (ay, ap, ahz) = taus[0], taus[1]
            # A confounded comparison is worse than none: say so rather than
            # dividing two numbers taken at different frame rates.
            if ihz > 0 and abs(ahz - ihz) / ihz > 0.25:
                fails.append(
                    f"the two arms ran at different pose rates ({ihz:.1f} vs "
                    f"{ahz:.1f} Hz), so the lag difference between them is "
                    f"confounded with the frame rate — the whole reason the "
                    f"arms alternate inside one run. Re-run; do not read the "
                    f"ratio below")
            if iy is not None and ay is not None:
                # 4 T is what an unled proportional servo must read at the
                # instant a sample is taken: steady state on a ramp gives
                # e = e(1-kp) + rate*T, so e = rate*T/kp. If the control arm
                # does not show it, the model this run tests is wrong and the
                # ratio below means nothing.
                expect = 4.0 / max(ihz, 1e-6)
                if iy < 0.4 * expect:
                    print(f"lagfit  NOTE control arm tau {iy*1000:.1f} ms is "
                          f"well under the {expect*1000:.1f} ms a kp=0.25 servo "
                          f"must show at {ihz:.1f} Hz — the head may not have "
                          f"moved fast enough to measure a slope")
                # **This threshold depends on X4's transport delay, and the
                # first version of it did not know that.**
                #
                # 0.45 came from a delay-1 RAMP simulation. Replaying take 212's
                # OWN head trajectory through the real servo and the plant take
                # 213 identified shows the achievable ratio is a strong function
                # of the delay -- a perfectly working lead reads:
                #
                #     delay 0 -> 0.39x     delay 2 -> 0.63x
                #     delay 1 -> 0.50x     delay 3 -> 0.73x
                #
                # Take 213 measured X4's delay at 2 ticks. So on a real head the
                # best available is around 0.63x, and the old threshold could
                # not be met however well the lead worked: it failed take 212
                # for being physical. A ramp is the lead's best case and must
                # not be what an acceptance bound is derived from.
                #
                # 0.85 is what is left -- clear of the 1.00 a lead that never
                # reached the servo gives, and above everything a working one
                # produces at any plausible delay.
                ratio = ay / iy if iy > 1e-6 else float("inf")
                if abs(ay) > 0.85 * abs(iy):
                    # **Do not name a cause here.** The first version of this
                    # message asserted "the lead is not reaching the servo",
                    # and take 212 read 0.94x with the lead reaching the servo
                    # perfectly -- 99.3% of its commanded deltas matched the
                    # lead model exactly, against 35.9% for the no-lead model.
                    # The cause was the PLANT: X4's response is sub-linear in
                    # the per-step delta, so commanding more returns
                    # proportionally less. A failure message that names one
                    # cause sends the next reader to check the wrong thing.
                    fails.append(
                        f"the lead did not remove the lag: yaw tau {ay*1000:.1f} "
                        f"ms armed against {iy*1000:.1f} ms idle ({ratio:.2f}x, "
                        f"wanted <0.85x; a working lead reads 0.39-0.73x depending on "
                        f"X4's transport delay, which take 213 measured at 2 "
                        f"ticks). Two candidates, "
                        f"and they are distinguishable: reconstruct the commanded "
                        f"dx from the lagsample stream and compare it against the "
                        f"`headlook: ... delta` lines. If the ARMED deltas match "
                        f"a no-lead model, the lead is not reaching the servo. If "
                        f"they match the lead model, it is X4's response that is "
                        f"not linear — see task #73 in docs/x4-quirks.md")
                else:
                    print(f"lagfit  the lead removes the lag: yaw tau "
                          f"{iy*1000:.1f} -> {ay*1000:.1f} ms ({ratio:.2f}x) at "
                          f"a matched {ihz:.1f} Hz")
            armed_still = (sorted(arms[1]["still"])[len(arms[1]["still"]) // 2]
                           if arms[1]["still"] else None)
            idle_still = (sorted(arms[0]["still"])[len(arms[0]["still"]) // 2]
                          if arms[0]["still"] else None)
            if armed_still is not None and idle_still is not None:
                if armed_still > 8.0:
                    fails.append(
                        f"the lead's noise gain is worse than predicted: the "
                        f"camera drifts {armed_still:.2f} deg/s with the head "
                        f"still ({idle_still:.2f} idle), against ~5 predicted "
                        f"for 0.05 deg of tracker noise. The mitigation is the "
                        f"runtime's XrSpaceVelocity or a low-pass on the "
                        f"derived rate, NOT a smaller lead")
                else:
                    print(f"lagfit  noise price: {idle_still:.2f} -> "
                          f"{armed_still:.2f} deg/s of camera drift with the "
                          f"head still, inside the ~5 predicted")
    elif "X4VR_HEADLOOK_LEAD_AB" in (run or ""):
        fails.append("the run asked for the lead A/B but no `lagsample:` line "
                     "was emitted — the instrument did not run, so this take "
                     "measures nothing about #73")

    # --- task #68: did X4's free-look clamp actually move? ------------------
    #
    # X4's own readback, from `inject camread: X4 <y>,<p> ... | cmd <y>,<p>`.
    # Two things have to be true before the answer means anything, and both are
    # checked here rather than assumed:
    #
    #   * the read-back must be LIVE. 66% of the samples across 15 stored takes
    #     have raw 0.0000,0.0000 -- X4 reporting nothing, not reporting zero --
    #     and averaging those in says "the camera never moved".
    #   * the COMMAND must have exceeded the old stop. The servo clamps its own
    #     target, so if it never asked past 65 the camera could not go there
    #     and "no change" is a statement about the player's neck, not about X4.
    # ...and it must be the COCKPIT camera.
    #
    # Take 208 reported "MOVED, X4 reached 175.76 deg" and it was wrong. X4's
    # `<external>` element declares maxpitch="85" and NO maxyaw at all, so
    # external-view yaw is unbounded BY DESIGN. Segmented by the injector's own
    # `headlook: view is now ...` lines, that run reads:
    #
    #     cockpit   n=220  max |X4| yaw= 65.00 pitch=35.00   (command asked 69.95)
    #     external  n= 62  max |X4| yaw=175.76 pitch=69.09
    #
    # The cockpit clamp had not moved at all. Aggregating the two populations
    # turned "no effect" into a confident success, so the walk is segmented
    # here and external samples are reported separately, never mixed in.
    cam_pat = (r"camread: X4 (-?[\d.]+),(-?[\d.]+) deg \(raw (-?[\d.]+),"
               r"(-?[\d.]+)\) \| cmd (-?[\d.]+),(-?[\d.]+)")
    view, first_to = None, None
    by_view = {"cockpit": [], "external": [], "unknown": []}
    for ln in lines:
        if "view is now the cockpit" in ln:
            view = "cockpit"; first_to = first_to or "cockpit"; continue
        if ("view is now external" in ln or "view is now floating" in ln
                or "view is now PAUSED" in ln):
            view = "external"; first_to = first_to or "external"; continue
        m = re.search(cam_pat, ln)
        if m:
            by_view[view or "unknown"].append(m.groups())
    # The samples before the FIRST transition belong to whatever the run was
    # not transitioning to. Take 209 logged 537 camread lines and this section
    # judged on 24, because everything before the first transition sat in
    # `unknown` and was dropped once a cockpit sample existed — 95% of the
    # evidence discarded silently, on a run where the player never left the
    # cockpit except briefly.
    if by_view["unknown"] and first_to:
        was = "external" if first_to == "cockpit" else "cockpit"
        by_view[was] = by_view["unknown"] + by_view[was]
        print(f"clamp   {len(by_view['unknown'])} pre-transition sample(s) "
              f"credited to {was.upper()} — the first transition was TO "
              f"{first_to}, so the run was in {was} before it")
        by_view["unknown"] = []
    ext = [c for c in by_view["external"]
           if not (float(c[2]) == 0.0 and float(c[3]) == 0.0)]
    if ext:
        print(f"clamp   {len(ext)} EXTERNAL-view sample(s) excluded, max |X4| "
              f"yaw={max(abs(float(c[0])) for c in ext):.2f} — <external> "
              f"declares no maxyaw, so that number is unbounded by design and "
              f"says nothing about the cockpit clamp")
    cam = by_view["cockpit"] or by_view["unknown"]
    if cam and not by_view["cockpit"]:
        # Distinguish "never transitioned" from "transitioned but never INTO
        # the cockpit": the first means the pre-transition samples are as good
        # as we get, the second means the cockpit population is genuinely
        # absent. Saying "no view transitions" while also excluding external
        # samples, as this did, is self-contradictory on its face.
        print("clamp   %s — the samples read below are the ones before any "
              "transition, so treat them as cockpit only if the run started "
              "there" % ("no cockpit transition in this log, though external "
                         "ones occurred" if by_view["external"] else
                         "no view transitions in this log at all"))
    if cam:
        live = [c for c in cam
                if not (float(c[2]) == 0.0 and float(c[3]) == 0.0)]
        print(f"clamp   {len(cam)} camread sample(s), {len(live)} with a live "
              f"read-back ({len(cam) - len(live)} returned raw 0,0)")
        if not live:
            print("clamp   NOTHING to read — every sample is a dead read-back, "
                  "so this run says nothing about the clamp")
        else:
            # **A stop is a PLATEAU, not a maximum. Take 294 is why.**
            #
            # The old code read `max |X4| yaw` and announced EXTENDED PATH on
            # 119.04 deg. Four samples out of 14376 produced that number, all
            # four inside one read-back fault with the head at 18 deg; the other
            # 20304 cockpit ticks pinned at exactly 65.00-65.20. A maximum over a
            # run containing view transitions and invalid read-backs is not a
            # measurement of anything X4 does deliberately.
            #
            # What a stop looks like instead: the camera DWELLS at a value while
            # the command keeps asking past it. Same dwell-mass rule
            # tools/wildness.py settles its stop detection with, after three
            # successive rules that were each right on half the sessions.
            def plateau(seq):
                """`seq` is (t, |camera|, |target|) in order. A stop is where the
                camera DID NOT MOVE while the target was past it -- both halves
                are needed. "Target past camera" alone is true throughout
                ordinary lag, and scoring that returns wherever the camera
                happened to be while the servo was behind (53.94 deg on take
                294, against a real stop at 65.1).

                Reported as a high QUANTILE: four fault samples move a max by
                54 deg and cannot move a p95 at all."""
                held = []
                for (t0, v0, _), (t1, v1, tg1) in zip(seq, seq[1:]):
                    dt = t1 - t0
                    if not (0.0 < dt <= 0.5):
                        continue          # a gap is not a stationary camera
                    if tg1 > v1 + 2.0 and abs(v1 - v0) < 0.2:
                        held.append(v1)   # told to go further, and did not
                if len(held) < 20:
                    return None, 0
                held.sort()
                return held[int(0.95 * len(held))], len(held)

            # **Prefer lagsample: 20308 cockpit ticks against camread's 479.**
            # The clamp block predates lagsample and kept reading the sparse
            # stream out of habit. The target is the servo's own
            # clamp(head, yaw_limit), which is what it was asking X4 for.
            lag_ck = [r for r in rows if r[0] == "cockpit" and r[1] and r[2]] \
                if "rows" in dir() and rows and len(rows[0]) >= 9 else []
            if lag_ck:
                py, ny = plateau([(r[8], abs(r[4]),
                                   abs(max(-r[7], min(r[7], r[3]))))
                                  for r in lag_ck])
                pp, np_ = None, 0
                src = f"lagsample, {len(lag_ck)} cockpit tick(s)"
            else:
                # camread carries no timestamp in `live`; its cadence is ~0.4 s
                # and regular, so index stands in for time at a 1.0 spacing.
                py, ny = plateau([(float(i), abs(float(c[0])), abs(float(c[4])))
                                  for i, c in enumerate(live)])
                pp, np_ = plateau([(float(i), abs(float(c[1])), abs(float(c[5])))
                                   for i, c in enumerate(live)])
                src = f"camread, {len(live)} sample(s)"

            # **A full circle has no stop.** Since take 318 the extension sets
            # maxyaw=180 and X4 WRAPS there (179.22 -> 180.00 -> -179.66), so
            # the plateau finder -- built to find a stop -- found one at 92.77
            # out of servo lag, and the verdicts below called the 175.67 max
            # "an excursion past the plateau" and the extension "modified=1"
            # history. Count the wraps in the cockpit stream first; with one,
            # the stop question is answered: there is none.
            wraps = 0
            for a_, b_ in zip(lag_ck, lag_ck[1:]):
                if (0.0 < b_[8] - a_[8] <= 0.5 and abs(a_[4]) > 170.0 and
                        abs(b_[4]) > 170.0 and (a_[4] > 0) != (b_[4] > 0)):
                    wraps += 1
            xy_max = max(abs(float(c[0])) for c in live)
            xp_max = max(abs(float(c[1])) for c in live)
            cy = max(abs(float(c[4])) for c in live)
            cp = max(abs(float(c[5])) for c in live)
            # The verdicts below read the PLATEAU. The max is kept in view
            # because a gap between them is itself the read-back fault signal.
            #
            # **No plateau means UNMEASURED, and unmeasured must not be able to
            # fire a verdict.** Falling back to the max here would reintroduce
            # exactly the defect this replaces: take 294's pitch has no plateau
            # and its max is 66.10, which would announce EXTENDED PATH off the
            # same four fault samples. 0.0 cannot exceed any threshold below.
            xy = py if py is not None else 0.0
            xp = pp if pp is not None else 0.0
            print(f"clamp   max |X4| yaw={xy_max:.2f} pitch={xp_max:.2f} deg   "
                  f"(max |command| yaw={cy:.2f} pitch={cp:.2f})")
            # **Write mode has no X4 stop to find.** A written angle goes
            # through X4's follower path, which does not clamp; only the
            # servo's own limits bound it. Take 321 printed "DID NOT MOVE ...
            # stopped at 65.00" off a plateau of 26.60 that was G-force sway.
            if re.search(r"\bX4VR_HEADLOOK_WRITE=1\b", run or ""):
                print("clamp   write mode: the written angle bypasses X4's "
                      "look clamp (the follower path does not clamp), so "
                      "nothing here measures X4's stop; the servo's limits "
                      "bound it, and `hwrite` says whether X4 followed")
            else:
                if wraps:
                    print(f"clamp   NO STOP — X4's cockpit yaw WRAPPED through "
                          f"+-180 {wraps} time(s): a full circle, the extension's "
                          f"maxyaw=180 in force. "
                          + (f"The plateau finder's {py:.2f} is servo lag, not a "
                             f"stop — there is none to find" if py is not None
                             else "Nothing to plateau"))
                elif py is not None:
                    print(f"clamp   PLATEAU yaw={py:.2f} deg from {src}, {ny} "
                          f"sample(s) with the target 2+ deg past the camera"
                          + (f"; pitch={pp:.2f} ({np_})" if pp is not None else
                             "; pitch not measured on this stream"))
                    if xy_max > py + 5.0:
                        print(f"clamp   !! the MAX is {xy_max - py:.1f} deg past the "
                              f"plateau — that excursion is not a stop. Check the "
                              f"`headlook: REFUSED` lines; take 294's 119.04 came "
                              f"from a read-back fault, not from X4's clamp")
                else:
                    print(f"clamp   no plateau — the command never asked past where "
                          f"the camera already was, so nothing here measures a stop "
                          f"however large the max is")
                # Pitch gets the same guard as yaw, and take 209 is why it needed
                # one. There the neck ran out at 34.49 deg of command against a 75
                # deg limit, and X4 read 35.07 — barely past the stock 35.00, which
                # every fixed-clamp take pins to exactly. Without this the pitch
                # number reads as a measurement when the experiment never ran.
                if cp <= 36.0:
                    print(f"clamp   pitch INCONCLUSIVE — the command only reached "
                          f"{cp:.2f} deg, so nothing asked past the stock 35.00 "
                          f"stop. X4 read {xp:.2f}"
                          + (", which is past 35.00 and weakly suggests the limit "
                             "did move, but a neck is not a measurement"
                             if xp > 35.02 else
                             ", and 35.00 is exactly where a stock clamp sits"))
                # **X4's stop is NOT a constant, and assuming 65/35 cost take 293.**
                #
                #   285b-s2, 287b   X4 read exactly 65.00 / 35.00
                #   292             76.79 / 38.39
                #   293             78.79 / 48.87
                #
                # Same config file byte for byte (3822 bytes, 0 overrides), same
                # DECLARE_RENDERED_ROT=2, and `residual` is 0.00 at p99 and max in
                # both — so this is X4's own camera, not anything we add. Everything
                # seen is bounded by the `trackmaxyaw="85" trackmaxpitch="85"` that
                # `<internal>` declares next to maxyaw=65, and X4 lets individual
                # cockpits override the defaults, so a different ship is the likely
                # difference between the two pairs of takes.
                #
                # It has to be said loudly because a run that assumes 65 will place a
                # manipulation nowhere near the real limit: take 293 capped the servo
                # at 40 to keep the camera away from "the stop", and the stop was ~79.
                if wraps:
                    pass  # said above: no stop at all
                elif xy > 65.5 or xp > 35.5:
                    print(f"clamp   !! EXTENDED PATH — X4's stop is NOT the stock "
                          f"65/35 here; it read yaw {xy:.2f}, pitch {xp:.2f}. An "
                          f"extension or the ship's own cockpit sets it. Any knob "
                          f"set relative to \"the stop\" is against the wrong "
                          f"number.")
                elif cy <= 65.5:
                    # **Asymmetric, and saying so is the point.** Exceeding 65.5
                    # PROVES the limit is raised; pinning at 65.00 proves nothing,
                    # because the servo's own yaw_limit_deg defaults to 65 and a
                    # camera stopped by US looks exactly like one stopped by X4.
                    # Separating them needs X4VR_HEADLOOK_YAW_LIMIT above 65.
                    print(f"clamp   path UNDETERMINED — "
                          + (f"X4's stop was never measured (no plateau)"
                             if py is None else
                             f"X4's stop plateaus at {xy:.2f} yaw")
                          + f", and the servo never asked past 65 either, so a "
                          f"stock clamp and an extended one are indistinguishable "
                          f"here. Do NOT record this run as unmodified; set "
                          f"X4VR_HEADLOOK_YAW_LIMIT above 65 to tell them apart.")
                asked = cy > 66.0
                if not asked:
                    print("clamp   INCONCLUSIVE — the command never exceeded 66 "
                          "deg, so the servo never asked to go past the old stop. "
                          "Turn the head further, or raise "
                          "X4VR_HEADLOOK_YAW_LIMIT; a 65.00 here is the servo's "
                          "ceiling, not X4's")
                elif wraps:
                    print(f"clamp   MOVED ✓ — X4 turned a full circle (max |yaw| "
                          f"{xy_max:.2f} on the sparse stream, {wraps} wrap(s) "
                          f"through +-180 on the full one)")
                elif xy > 65.5:
                    print(f"clamp   MOVED ✓ — X4 reached {xy:.2f} deg of yaw, past "
                          f"the 65.00 stop every stored take pins to. The override "
                          f"reached X4 and the limit is data, confirmed live")
                else:
                    print(f"clamp   DID NOT MOVE — the command asked past 66 deg "
                          f"and X4 plateaued at {xy:.2f}, so the override did not "
                          f"reach the camera. Check `fileprobe` for whether the "
                          f"file was even read before blaming the value")

    # --- task #68: which door does X4 knock on for parameters.xml? ----------
    #
    # It records ATTEMPTS, so a path that does not exist still shows up -- that
    # is the whole question, since the file we want to serve is not on disk.
    fp = [ln.split("fileprobe: ", 1)[1].strip()
          for ln in lines if "fileprobe: " in ln]
    if fp:
        userdir = [p for p in fp if "parameters.xml" in p and
                   "/EgoSoft/" in p]
        ext = [p for p in fp if "extensions/" in p and "parameters.xml" in p]
        arch = [p for p in fp if p.endswith(".cat)") or p.endswith(".dat)")]
        cam = [p for p in fp if "camerasettings" in p]
        print(f"fileprobe     {len(fp)} distinct interesting path(s)")
        if userdir:
            print(f"fileprobe     X4 DOES probe a user-level parameters.xml — "
                  f"the existing fopen path can serve it and #68 is nearly "
                  f"free: {userdir[0]}")
        else:
            print("fileprobe     no user-level parameters.xml probe. The "
                  "cheapest door is CLOSED, so #68 needs the extension route "
                  "or archive interception")
        if ext:
            print(f"fileprobe     extension libraries/ IS probed ({len(ext)}), "
                  f"so a synthesised extension would be read — but weigh the "
                  f"save's modified=\"0\" before spending it")
        if cam:
            print(f"fileprobe     camerasettings probed at {len(cam)} path(s) "
                  f"— this is the POSITIVE CONTROL for a user-level library "
                  f"override, since the player has one on disk")
        else:
            print("fileprobe     camerasettings NOT probed — the control did "
                  "not fire, so a missing parameters.xml probe proves nothing "
                  "about X4 and only that this run did not reach that code")
        print(f"fileprobe     {len(arch)} archive open(s)")
        if any("capping" in ln for ln in lines):
            print("fileprobe     CAPPED at 400 — anything opened later is "
                  "unrecorded, so absence below the cap is not evidence")
    elif "X4VR_FILEPROBE=1" in (run or ""):
        print("fileprobe     asked for, but NOTHING matched — the probe is "
              "wired to fopen/fopen64/open/open64/openat, so this means X4 "
              "reached none of them for these names, not that it never looks")

    # --- task #67: the two camera walks, reported not acted on --------------
    #
    # Neither outcome is a FAIL. This run's camera numbers all come from the
    # accumulating walk either way; the line says how much would move if the
    # default flipped, which is the thing that could not be known offline.
    lv = [ln for ln in lines if "proj LIVESETS:" in ln]
    if lv:
        # The LAST line, not the first. Take 203 logged one report at the first
        # opportunity and read "3 slot(s)" — three, because the accumulation
        # this measures had not happened yet. The layer now reports a growth
        # series (first, then each doubling), so the final line is the closest
        # thing to the run's steady state and the earlier ones show the climb.
        if len(lv) > 1:
            print(f"livesets      {len(lv)} report(s) as the count grew; the "
                  f"last is the steady state and is what is read below")
        m = re.search(r"accumulating (\d+) slot\(s\), winner offset=(\d+) "
                      r"draws=(\d+) \| superseding (\d+) slot\(s\), winner "
                      r"offset=(\d+) draws=(\d+) \| winner (\w+)", lv[-1])
        if m:
            acc, ao, an, sup, so, sn, verdict = m.groups()
            print(f"livesets      accumulating {acc} slot(s) → superseding "
                  f"{sup}; winner {verdict.lower()}"
                  + ("" if verdict == "AGREES" else
                     f" (offset {ao} draws {an} → offset {so} draws {sn})"))
            drop = int(acc) - int(sup)
            if drop > 0:
                print(f"livesets      {drop} slot(s) were stale descriptor "
                      f"writes. #67 is the decision to flip the default; "
                      f"nothing above uses the superseding walk yet")
            if verdict != "AGREES":
                print("livesets      the WINNER moves, so flipping #67 "
                      "re-baselines camera attribution — settled camera "
                      "numbers must be re-read, not assumed to carry over")
    elif "X4VR_DUMP_MATRICES=1" in (run or ""):
        print("livesets      no `proj LIVESETS` line — this build predates "
              "#67's side-by-side, so this log says nothing about how much "
              "stale descriptor writes move the camera pick")

    ob = [ln for ln in lines if "proj OBJBLOCK:" in ln]
    if not ob:
        if "X4VR_DUMP_MATRICES=1" in (run or ""):
            print("objblock      no `proj OBJBLOCK` line — this build predates "
                  "the object-block capture, so any csm number above came from "
                  "the dead camera member and is not evidence")
    else:
        tail = ob[0].split("proj OBJBLOCK:", 1)[1].strip()
        print(f"objblock      {tail[:150]}")
        m = re.search(r"(\d+) distinct cascade scale", ob[0])
        spread = int(m.group(1)) if m else -1
        # NOT a FAIL any more, and the reason it used to be one was wrong.
        #
        # Take 202 failed here on "the CSM is light-space and should be
        # identical in every slot". X4's own GLSL refutes that: M_shadowCSM0 is
        # member 3 of the per-OBJECT block and is applied to three different
        # source frames — object-space `Pos` in 186 vertex entry points,
        # world-space `IO_world_pos` in others, view-space `view_pos` in 54
        # fragment ones. It carries the draw's own transform, so a spread is
        # the expected reading, not a defect.
        #
        # The half of take 202's spread that WAS a defect is fixed in the
        # layer: writes accumulated per descriptor set instead of superseding,
        # so the walk read regions X4 had recycled and got `inf`.
        if spread > 1:
            print(f"objblock      {spread} distinct |row0| — EXPECTED. Member 3 "
                  f"is per-object and composes the draw's own frame, so this "
                  f"is a cascade scale only for a view-space reader and the "
                  f"walk does not identify which. Not #56's evidence; see the "
                  f"#56 line above")
        elif spread == 0:
            print("objblock      spread 0 — no slot carried a nonzero matrix, "
                  "so the scales above are unmeasured rather than measured")
        if any("=inf" in ln for ln in lines if "csm" in ln):
            fails.append(
                "an object slot read `csm-scale=inf`, which means the walk "
                "read a buffer region X4 had already recycled. That was take "
                "202's defect and the layer fix should have removed it — a "
                "recurrence means descriptor writes are accumulating again")

    # --- which VARIANT the no-depth passes actually bind --------------------
    #
    # Take 196 is why this exists, and it exists here rather than as a grep
    # because a grep I picked is what cost takes 101 and 102.
    #
    # The distinction this section is about: a module can be PATCHED and still
    # not be the one BOUND. classify_unsheared() marks a colour pass with no
    # depth attachment `unsheared` (x4vr_layer.cpp:2027), needs_original() then
    # returns true for it, and pipeline creation swaps the ORIGINAL twin back
    # in. Take 195's rp #31/#32 -- the deferred lighting output, and the two
    # passes the x=791 seam is bracketed to -- bind `->original` on both
    # stages, so the per-eye M_invprojection correction and #61's R^-1 are
    # emitted perfectly and never execute there.
    #
    # An offline check over the dumps cannot see this by construction: it
    # patches a file and interprets the result, and the layer's variant
    # selection never enters. That is how the "uncorrected reconstruction"
    # hypothesis was retired by an instrument answering a different question.
    #
    # X4VR_SHEAR_NODEPTH=1 flips the rule. The check is GATE ON INTENT: if the
    # run asked for it, those passes must stop binding `->original`. Whether
    # the resulting frame looks better is a different question and not one this
    # file answers -- the knob also restores the shear, which take 70 measured
    # at 85% per-eye disagreement on #57, so a WORSE-looking take is the knob
    # working, not a regression.
    # Take 196 corrected this predicate. The first version keyed on "no-depth"
    # alone and FAILed a valid take: rp #0/#33/#46 stayed `->original` under
    # the knob, and they are RIGHT to. They carry formats 44L/50L/50L -- every
    # colour attachment LDR -- so they are held by the all-LDR rule at
    # x4vr_layer.cpp:2060, a DIFFERENT rule that X4VR_SHEAR_NODEPTH does not
    # gate. A pass can be no-depth and all-LDR at once; the knob only reaches
    # the ones that are no-depth and NOT all-LDR.
    #
    # This is the wrong-granularity error again: no predicate over "no-depth
    # passes" can split the two, because the classified object was wrong. The
    # eligible object is "no-depth AND at least one HDR colour attachment",
    # which the inventory's own format column already states -- `[97H]` versus
    # `[44L]` -- so it is read here rather than assumed.
    binds = [ln for ln in lines if "bind rp #" in ln]
    nodepth_rps = set()   # eligible: the knob should flip these
    ldr_held = set()      # no-depth but all-LDR: held by the other rule
    for ln in lines:
        m = re.search(r"rp #(\d+)\.\d+: .*?\[([0-9LH,]+)\].*?\bno-depth\b", ln)
        if not m:
            continue
        tags = m.group(2)
        if "H" in tags:
            nodepth_rps.add(m.group(1))
        else:
            ldr_held.add(m.group(1))
    want_nodepth = "X4VR_SHEAR_NODEPTH=1" in (run or "")
    if not binds:
        if want_nodepth:
            fails.append(
                "the run asked for X4VR_SHEAR_NODEPTH=1 and there are no "
                "`bind rp #` lines to say whether it changed anything — that "
                "record needs X4VR_PIPE_INVENTORY=1. The run cannot answer "
                "the question it was launched to ask")
    elif not nodepth_rps:
        print("shear-nodepth no pass was reported as no-depth — the render "
              "pass inventory needs X4VR_MV_INVENTORY=1, so which passes the "
              "rule applies to is unknown here")
    else:
        orig = set()
        patched = set()
        for ln in binds:
            m = re.search(r"bind rp #(\d+)\b", ln)
            if not m or m.group(1) not in nodepth_rps:
                continue
            if "->original" in ln:
                orig.add(m.group(1))
            if "->patched" in ln:
                patched.add(m.group(1))
        seen = sorted(orig | patched, key=int)
        print(f"shear-nodepth {len(nodepth_rps)} no-depth HDR pass(es) are "
              f"eligible; of those that bind, {len(orig)} bind ->original "
              f"{sorted(orig, key=int)}, {len(patched)} bind ->patched "
              f"{sorted(patched, key=int)}")
        if ldr_held:
            print(f"shear-nodepth {len(ldr_held)} further no-depth pass(es) "
                  f"are all-LDR {sorted(ldr_held, key=int)} — held by the "
                  f"rule at x4vr_layer.cpp:2060, which this knob does not "
                  f"gate. Their ->original is correct, not a miss")
        if want_nodepth and orig:
            fails.append(
                f"the run asked for X4VR_SHEAR_NODEPTH=1, which exists to stop "
                f"no-depth passes binding the original twin, and "
                f"{len(orig)} of them still bind ->original "
                f"{sorted(orig, key=int)}. The knob did not reach the "
                f"decision it names, so this take is not a test of it — "
                f"whatever the frame looks like")
        elif want_nodepth and not seen:
            fails.append(
                "the run asked for X4VR_SHEAR_NODEPTH=1 but no no-depth pass "
                "bound any tracked module, so nothing was observed either "
                "way. Gate on intent: the question was asked and not answered")
        elif not want_nodepth and orig:
            # The baseline, printed so the next take has something to diff
            # against rather than a remembered number.
            print(f"shear-nodepth baseline: X4VR_SHEAR_NODEPTH is off, so "
                  f"->original at {sorted(orig, key=int)} is the rule working "
                  f"as written, not a defect")

    # --- the map gate ------------------------------------------------------
    #
    # Take 146 is why this lives in the scorer instead of in a grep. The check
    # I handed over was `grep -c "view is now" >= 2`; it returned exactly 2 and
    # would have been read as a pass. Both transitions belonged to the ESC menu,
    # which pauses; the map produced neither, and we drove mouse deltas into it
    # for 9.2 seconds. A count cannot tell you which event it counted.
    #
    # **The M key says the map was exercised. It does not say when it closed.**
    # Take 148 worked perfectly in the headset and this check failed it. The
    # episodes were built by pairing M presses, on the assumption that M toggles
    # the map -- but the log holds THREE M presses, an odd number, because that
    # map was closed with ESC or a click. So the last episode ran to infinity
    # and swallowed the perfectly correct resume 220 ms after the map closed.
    #
    # X4's own state was in the log the whole time. The HUD down->up interval is
    # when X4 says the HUD was gone; that is the period we must not be steering,
    # measured by the game rather than inferred from what the player pressed.
    # The M key keeps exactly one job it is good at -- proving the map was
    # opened at all, independently of our own code, so a build whose gate never
    # fires cannot pass by staying silent.
    def ts(ln):
        m = re.match(r"\[\s*([0-9.]+)\]", ln)
        return float(m.group(1)) if m else None

    def stamps(pred):
        return [t for t in (ts(ln) for ln in lines if pred(ln)) if t is not None]

    m_down = stamps(lambda ln: "key[peep]" in ln and "type=0x300" in ln
                    and "scancode=16 " in ln)
    hud_down = stamps(lambda ln: "HUD is now down" in ln)
    hud_up = stamps(lambda ln: "HUD is now up" in ln)
    rel_on = stamps(lambda ln: "relative mouse mode ON" in ln)
    end_of_log = max(stamps(lambda ln: True) or [float("inf")])

    if not m_down:
        print("map   NOT EXERCISED — no M press in this log, so this run says "
              "nothing about whether the map gate holds")
    elif not hud_down:
        print(f"map   {len(m_down)} M press(es), 0 HUD gate releases")
        fails.append(
            f"the map was opened {len(m_down)} time(s) and the HUD gate never "
            f"released (first M at t={m_down[0]:.3f}) — we kept steering into "
            f"it. Check that 'camread: IsHUDActive' resolved, and read the "
            f"IsInPanelMode value logged beside it")
    else:
        # Each HUD-down opens an interval that its next HUD-up closes; one still
        # open at the end of the log is a real interval too (X4 was quitting).
        intervals = [(d, next((u for u in hud_up if u > d), end_of_log))
                     for d in hud_down]
        # **What a healthy run reads, stated before trusting the number:** X4
        # enters relative mouse mode ZERO times strictly inside a HUD-down
        # interval. Mouselook is not a proxy for the defect, it IS the defect --
        # it is what makes the mouse rotate the view instead of driving the
        # cursor, which is why the map could not be dragged in take 147.
        # **Except the hand-back.** On foot X4 resumes walking mouse-look a
        # few ms BEFORE it reports the HUD up: take 343 stood up out of the
        # docked menu (11 ms) and left a station menu (23 ms), and this
        # FAILed both. Take 147's defect was mouselook in the middle of the
        # map; the last 0.1 s of an interval is X4 handing control back.
        relapsed = [(a, [t for t in rel_on if a < t < b - 0.1])
                    for a, b in intervals]
        relapsed = [(a, hits) for a, hits in relapsed if hits]
        print(f"map   {len(m_down)} M press(es), {len(intervals)} HUD-down "
              f"interval(s), {len(intervals) - len(relapsed)} of them free of "
              f"mouselook ({len(hud_up)} re-acquire(s) after)")
        if relapsed:
            a, hits = relapsed[0]
            fails.append(
                f"X4 re-entered mouselook inside {len(relapsed)} HUD-down "
                f"interval(s) — first HUD down t={a:.3f}, back in mouselook "
                f"t={hits[0]:.3f} ({hits[0] - a:.3f}s later, {len(hits)} "
                f"time(s) in that interval). The mouse rotates the view instead "
                f"of driving the cursor, so the map cannot be dragged. Either "
                f"something presses the free-look key while may_steer is false, "
                f"or X4 itself uses relative mode for map dragging")
        elif not hud_up:
            fails.append(
                "the HUD gate released and never re-acquired — head tracking "
                "would stay dead after the first map, which is worse than the "
                "bug it fixes")

    print()

    # ------------------------------------------------ pan channel (#33)
    #
    # Characterisation, not a pass/fail on X4's behaviour. The run is gated on
    # INTENT -- did we ask for the measurement and did the instrument run --
    # never on whether the channel turned out to be usable. A channel that
    # springs back on release is a perfectly good measurement and a dead end
    # for the feature; those are different verdicts and only the first belongs
    # here.
    if "X4VR_PANPROBE=1" in (run or ""):
        # Everything is keyed on the LOG timestamp, not on the probe's own t=,
        # so samples and key edges share one clock and an episode can be cut
        # out by time. Selecting samples by matching the held= field instead
        # would merge repeat presses of the same key into one episode, and
        # "UP" is a substring of "UP_LEFT" and "UP_RIGHT" -- a first-match on
        # an aliased name is a mistake this project keeps making.
        samp, eps = [], []
        for ln in lines:
            t = ts(ln)
            if t is None:
                continue
            m = re.search(r"panprobe: t=[0-9.]+ yaw=(-?[0-9.]+) "
                          r"pitch=(-?[0-9.]+) deg", ln)
            if m:
                samp.append((t, float(m.group(1)), float(m.group(2))))
                continue
            m = re.search(r"panprobe: key (\w+) (DOWN|UP)", ln)
            if m:
                eps.append((m.group(1), m.group(2), t))

        if not samp:
            fails.append(
                "X4VR_PANPROBE=1 but the probe logged no samples — either "
                "GetCameraRotation did not resolve (grep 'camread: "
                "GetCameraRotation') or panprobe_tick never ran. Nothing in "
                "this log says anything about the pan channel")
        elif not eps:
            print(f"pan   NOT EXERCISED — {len(samp)} camera sample(s) but no "
                  f"free-look keypad press in this log. The probe was "
                  f"listening and nothing was pressed")
        else:
            # Pair each DOWN with the next UP of the SAME key. An unmatched
            # DOWN is reported as an anomaly rather than run to end-of-log:
            # padding the open half with a sentinel is exactly how take 148's
            # scorer swallowed a correct result.
            holds, open_downs = [], []
            for name, edge, t in eps:
                if edge == "DOWN":
                    open_downs.append((name, t))
                else:
                    hit = next((i for i in range(len(open_downs) - 1, -1, -1)
                                if open_downs[i][0] == name), None)
                    if hit is not None:
                        holds.append((name, open_downs.pop(hit)[1], t))

            print(f"pan   {len(holds)} complete hold(s), {len(open_downs)} "
                  f"unclosed, {len(samp)} camera sample(s)")
            if open_downs:
                print(f"      unclosed: {', '.join(n for n, _ in open_downs)} "
                      f"— key(s) whose release never reached us; those holds "
                      f"are not measured rather than guessed at")

            for name, td, tu in holds:
                during = [s for s in samp if td <= s[0] <= tu]
                if len(during) < 3:
                    print(f"      {name:10s} held {tu - td:.2f}s — only "
                          f"{len(during)} sample(s), too few to read a rate")
                    continue
                y0, p0 = during[0][1], during[0][2]
                y1, p1 = during[-1][1], during[-1][2]
                dur = during[-1][0] - during[0][0]
                half = len(during) // 2
                r1 = ((during[half][1] - y0) /
                      max(during[half][0] - during[0][0], 1e-3))
                r2 = ((y1 - during[half][1]) /
                      max(during[-1][0] - during[half][0], 1e-3))
                # After release: the six-second tail the probe keeps at full
                # cadence. This is the number that decides the feature.
                after = [s for s in samp if tu < s[0] <= tu + 6.0]
                tail = ""
                if after:
                    ye, pe = after[-1][1], after[-1][2]
                    dt = after[-1][0] - during[-1][0]
                    back = (abs(ye) < abs(y1) * 0.5) or (abs(pe) < abs(p1) * 0.5)
                    tail = (f" | +{dt:.1f}s after release yaw={ye:.2f} "
                            f"pitch={pe:.2f} {'RECENTRED' if back else 'held'}")
                print(f"      {name:10s} {dur:.2f}s  yaw {y0:+.2f}->{y1:+.2f} "
                      f"({(y1 - y0) / max(dur, 1e-3):+.1f} deg/s)  pitch "
                      f"{p0:+.2f}->{p1:+.2f} "
                      f"({(p1 - p0) / max(dur, 1e-3):+.1f} deg/s)"
                      f"{'  PLATEAU' if abs(r2) < abs(r1) * 0.25 else ''}{tail}")

            # **What a usable channel reads, stated before reading it:** yaw or
            # pitch moves while the key is held at a rate a servo can command,
            # and STAYS PUT after release. Flat under a real press means X4
            # ignores the state and no synthesis work would have helped.
            # RECENTRED means the channel is a spring, and a spring cannot
            # carry an absolute head pose however it is driven.
            moved = [h for h in holds
                     if any(abs(s[1]) > 1.0 or abs(s[2]) > 1.0
                            for s in samp if h[1] <= s[0] <= h[2])]
            if holds and not moved:
                print("      every hold left the camera within 1 deg of centre "
                      "— under a REAL keypress on X4's factory binding, so "
                      "the directional states do not move this camera and the "
                      "channel is dead without any synthesis question arising")

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
