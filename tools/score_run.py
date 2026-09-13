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
                for ln in lines:
                    m2 = re.search(r"vr copy \(\w+\): swapchain=(\d+) "
                                   r"blits=(\d+) released=(\d+) "
                                   r"acquire_failed=(\d+) refused=(\d+)", ln)
                    if m2:
                        copy = m2
                if copy:
                    sc_ok, blits, released, afail, refused = (
                        int(g) for g in copy.groups())
                    print(f"    copy swapchain={sc_ok} blits={blits} "
                          f"released={released} acquire_failed={afail} "
                          f"refused={refused}")
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
        idle_win, open_at = [], None
        for ln in lines:
            if "headlook: view is now" not in ln:
                continue
            m = re.search(r"\[\s*(\d+\.\d+)\]", ln)
            if not m:
                continue
            t = float(m.group(1))
            if "idle" in ln:
                open_at = t if open_at is None else open_at
            elif open_at is not None:
                idle_win.append((open_at, t))
                open_at = None
        if open_at is not None:
            idle_win.append((open_at, float("inf")))

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
                  f"idle (external/floating view) — identity is correct there, "
                  f"and counting them was a false FAIL on takes 220 and 221")
        if unlinked:
            fails.append(
                f"the layer could not reach the injector's shared state on "
                f"{len(unlinked)} of {len(pose)} samples — every VR frame in "
                f"those is submitted world-locked no matter what head-look "
                f"does. Check x4vr_shared_state is exported and the injector "
                f"actually loaded")
        elif ident:
            fails.append(
                f"the pose fell back to identity {len(ident)} time(s) AFTER it "
                f"had been driven, while head-look had NOT announced itself "
                f"idle — the gate dropped out mid-run, so those frames were "
                f"submitted world-locked with head-look live. cam_valid went "
                f"back to 0 with no `view is now external/floating` before it, "
                f"so this is not the by-design idle: check what cleared "
                f"cam_valid, not X4's camera")
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
            print("pose  world-locked all run, and X4VR_HEADLOOK was not set — "
                  "this run says nothing about head tracking, which is not a "
                  "defect in it")

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
        if not forced and heads[-1] < 5.0:
            fails.append(
                f"the head never rolled more than {heads[-1]:.2f} deg, so this "
                f"run does NOT exercise #77 whatever else it shows — tilt the "
                f"head, or use X4VR_ROLL_FORCE to drive the shear directly")
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
    if tv:
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
        r"(?: cmd=(-?\d+\.\d+),(-?\d+\.\d+) rot=(\d))?)?", text)
    if pl:
        # Groups, in order: ts, decl_y, decl_p, x4_y, x4_p, d_y, d_p, age,
        # valid, rawvalid, cmd_y, cmd_p, rot. The last three are absent in logs
        # written before the #75 A/B, and `None` there means "unknown arm",
        # which is deliberately not the same value as either arm.
        rows = []
        for g in pl:
            rows.append((
                float(g[0]),                                   # 0 ts
                float(g[5]), float(g[6]),                      # 1,2 d yaw/pitch
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
                gain[:] = sorted(abs(r[8] - r[7]) for r in fresh
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
                    dr = sorted(abs(r[6] - r[7]) for r in rot_on)
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
        good = [(abs(float(g[5])), abs(float(g[6])), float(g[4]))
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
                      f"119's runaway in miniature and not a lever to pull. The "
                      f"fix is to stop deriving the tick from the drain at all.")
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
            print(f"pumprate   => INCONSISTENT, and that is a defect of this "
                  f"instrument rather than a finding: {driver} runs at "
                  f"{drv:.1f} Hz, above the {pub:.1f} Hz publish rate, and "
                  f"nopose is only {nopose} — so every published pose should "
                  f"have been seen, yet only {acted:.1f}% were. Either `updates` "
                  f"advances more than once per publish, or the windows of the "
                  f"two counters are misaligned. Do not spend a take on this "
                  f"number until it is resolved.")
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
    # retains) separates all three; tests/plantid.cpp checks that it does,
    # against simulated plants of each kind.
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
            print(f"plantid  transport delay: the response peaks {peakj} tick(s) "
                  f"after the command ({peakj * tick * 1000:.0f} ms at this "
                  f"{1/tick if tick else 0:.0f} Hz tick rate), and "
                  f"{peak/max(tot,1e-9)*100:.0f}% of it arrives in that one tick")

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

        # Name it, or say plainly that it is none of the three.
        #
        # **Coast is checked first, and alone.** It is the one number that
        # separates a smoothing plant from a stiff one whatever the delivery
        # curve looks like -- and for a smoother the delivery curve is not
        # trustworthy anyway: the reset leaves residual velocity that the 8-step
        # settle does not fully bleed off, which swamps the smallest impulses
        # and made `fade` come out NaN. Requiring both numbers before naming
        # anything turned a correct diagnosis into "INCOMPLETE".
        if cmed is not None and cmed > 0.35:
            print(f"plantid  signature: coast {cmed*100:.0f}%")
            print("plantid  => SMOOTHED / MOMENTUM. The camera keeps moving "
                  "after the command stops, so X4 filters the mouse input "
                  "rather than applying it. A servo that commands POSITION "
                  "increments is the wrong shape for this plant; the lead "
                  "adding more command cannot fix it (take 212, 0.94x).")
        elif fade is not None and fade == fade and cmed is not None:
            print(f"plantid  signature: fade {fade:.2f}x across the staircase, "
                  f"coast {cmed*100:.0f}%")
            if small is not None and small > 0.9 and fade > 0.9 and cmed < 0.2:
                print("plantid  => PURE INTEGRATOR with transport delay. Every "
                      "size delivers what it asked for and the camera stops "
                      "dead when the commands do. There is NO cap, NO "
                      "momentum and NO nonlinearity, so the plant is exactly "
                      "what tests/servo_lag.cpp models -- and take 212's null "
                      "is therefore NOT the plant's doing. #73's cause is "
                      "still open; do not close it on this run.")
            elif fade < 0.75:
                print("plantid  => PER-STEP CAP. Delivery is flat for small "
                      "commands and collapses for large ones, with no coast. "
                      "Keep max_step_deg under the knee above and the servo "
                      "stays in the linear region.")
            else:
                print("plantid  => INTEGRATOR, near enough. Then the plant is "
                      "NOT what defeated the lead in take 212 and the cause is "
                      "still open — do not close #73 on this.")
        elif deliv:
            print("plantid  signature INCOMPLETE — one of the two numbers is "
                  "missing, so no candidate is named. Naming one on a single "
                  "number is how the 200 deg/s story survived a day.")

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
        def wrap180(d):
            while d > 180.0:
                d -= 360.0
            while d < -180.0:
                d += 360.0
            return d

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
        if "view is now external" in ln:
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
            xy = max(abs(float(c[0])) for c in live)
            xp = max(abs(float(c[1])) for c in live)
            cy = max(abs(float(c[4])) for c in live)
            cp = max(abs(float(c[5])) for c in live)
            print(f"clamp   max |X4| yaw={xy:.2f} pitch={xp:.2f} deg   "
                  f"(max |command| yaw={cy:.2f} pitch={cp:.2f})")
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
            asked = cy > 66.0
            if not asked:
                print("clamp   INCONCLUSIVE — the command never exceeded 66 "
                      "deg, so the servo never asked to go past the old stop. "
                      "Turn the head further, or raise "
                      "X4VR_HEADLOOK_YAW_LIMIT; a 65.00 here is the servo's "
                      "ceiling, not X4's")
            elif xy > 65.5:
                print(f"clamp   MOVED ✓ — X4 reached {xy:.2f} deg of yaw, past "
                      f"the 65.00 stop every stored take pins to. The override "
                      f"reached X4 and the limit is data, confirmed live")
            else:
                print("clamp   DID NOT MOVE — the command asked past 66 deg "
                      "and X4 still stopped at 65.00, so the override did not "
                      "reach the camera. Check `fileprobe` for whether the "
                      "file was even read before blaming the value")

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
        relapsed = [(a, [t for t in rel_on if a < t < b]) for a, b in intervals]
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
