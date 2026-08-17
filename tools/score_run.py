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
                         f"were found at {m_mvd.group(1)}-present-n*-layer*.ppm "
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
                        "world shader was patched, so the off-axis target fell "
                        "back to X4's symmetric field. The picture is "
                        "self-consistent — render and declaration agree — but "
                        "this run says nothing about the runtime's frusta. "
                        "Take 163 timed the margin at 0.8 s, so losing it "
                        "means the runtime came up slower than that here")
                if src:
                    print(f"offaxis  target from {src.group(1).strip()}; the "
                          f"compositor is told the same frusta the shader "
                          f"coefficients were baked from")
                # **The check that would have failed take 165b before anyone
                # put the headset on.** The affine reaches World modules only.
                # Everything else -- every NonWorld module, the unsheared twin,
                # the skybox's fullscreen shader, the loading screen -- stays in
                # X4's symmetric frame, so it sits at image centre in both eyes.
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
                    if div > 1.0:
                        fails.append(
                            f"the declared frusta are canted {c0:+.2f}° and "
                            f"{c1:+.2f}°, and the affine reaches World modules "
                            f"only — so the HUD, the skybox's fullscreen "
                            f"shader, the unsheared twin and the loading "
                            f"screen all sit at image centre in both eyes and "
                            f"are asked to DIVERGE by {div:.2f}°. Nobody fuses "
                            f"that; x4vr_view.hpp:291 already records it as "
                            f"the negative control. The world is correct and "
                            f"everything else is not")
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
        print("pose  not reported — this build predates the instrument")
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
        print(f"pose  {len(pose)} sample(s), {len(driven)} driven, {pre} "
              f"identity before head-look armed (expected), {len(ident)} "
              f"identity after (not expected)")
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
                f"had been driven — the gate dropped out mid-run, so those "
                f"frames were submitted world-locked while head-look was live. "
                f"cam_valid went back to 0: check `steering`, not X4's camera")
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
