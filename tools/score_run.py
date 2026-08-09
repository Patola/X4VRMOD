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
    # Did this run *ask* for the probe? Take 103 scored FAIL on "the probe never
    # sampled a swapchain image" in a run that never enabled the probe -- so the
    # acceptance check reported a defect for a question the run did not pose,
    # which is the fastest way to teach someone to stop reading the scorer.
    # Gate on intent, never on outcome: the env line records what was asked for.
    m_probe = re.search(r"X4VR_MV_PROBE=(\S*)", run or "")
    probe_asked = bool(m_probe and m_probe.group(1) not in ("", "0"))
    if run is None:  # pre-take-34 build: no env line, so intent is unknowable
        probe_asked = any("mv probe:" in ln for ln in lines)

    if not samples and not probe_asked:
        # Explicitly *not* silence, and explicitly not a pass. The right eye is
        # simply unjudged in this run, and saying so is the whole point.
        print("swapchain  unjudged — this run did not enable X4VR_MV_PROBE, so "
              "nothing here speaks to the right eye's contents.")
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
                    print(f"warn  no camera in this run reads the field "
                          f"X4VR_FOV={asked:g} asks for "
                          f"({asked * FOV_BASE_DEG:.2f}°) — either X4 rejected "
                          f"the tag or the law is not linear here")

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
                        verdict = ("matches the scene camera" if abs(r - 1) < 0.02
                                   else f"separate {r:.2f}x too "
                                        f"{'much' if r > 1 else 'little'}")
                        print(f"stereo  X4VR_PROJ_SX={m_baked.group(1)} against "
                              f"a scene sx of {ssx:.5f}: the {n} that cannot "
                              f"read sx live {verdict} (task #23)")
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
            tag = ("  <- last phase, the one a parked A/B compares"
                   if k == len(phases) - 1 and len(ph) >= 3
                   and ph[-1][0] - ph[0][0] >= 20.0 else "")
            print(f"      {ph[0][0] - t0:5.0f}–{end - t0:<5.0f}s "
                  f"{w[len(w) // 2]:6.2f} ms  ({len(ph)} window(s), "
                  f"{w[0]:.2f}–{w[-1]:.2f}){tag}")

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
