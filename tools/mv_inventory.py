# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#!/usr/bin/env python3
"""Join X4VR_MV_INVENTORY's two logs and cost the multiview doubling.

Usage:  python3 tools/mv_inventory.py /tmp/x4vr.log

The layer prints render passes (what a pass is) and framebuffers (how big it
is) separately, because Vulkan puts the extent in the framebuffer and the
formats in the render pass. Neither alone answers the question that decides
the partition: what does doubling this pass actually cost?

The log accumulates across runs and the serials restart at 0 each time, so
only the last run is read -- everything after the final "instance created
(app=X4)".
"""
import re
import sys
from collections import defaultdict

# Bytes per pixel for the formats X4 uses as attachments. Anything missing is
# reported rather than guessed at, so a new format shows up as a gap in the
# accounting instead of a silently wrong total.
BPP = {
    9: 1, 13: 1, 16: 2, 37: 4, 43: 4, 44: 4, 50: 4, 64: 4,
    70: 2, 76: 2, 77: 4, 83: 4, 91: 4, 97: 8, 100: 4,
    124: 2, 125: 4, 126: 4, 129: 4, 130: 8,
}
NAME = {
    9: "R8_UNORM", 13: "R8_UINT", 16: "R8G8_UNORM", 37: "RGBA8_UNORM",
    43: "RGBA8_SRGB", 44: "BGRA8_UNORM", 50: "BGRA8_SRGB",
    64: "A2B10G10R10_PACK32", 70: "R16_UNORM", 91: "R16G16_UINT",
    100: "R32_SFLOAT",
    76: "R16_SFLOAT", 77: "R16G16_UNORM", 83: "RG16_SFLOAT",
    97: "RGBA16_SFLOAT", 124: "D16_UNORM", 126: "D32_SFLOAT",
}

RP = re.compile(r"rp #(\d+)\.(\d+): (\d+) colour \[([^\]]*)\]"
                r"(?: depth (\d+)| no-depth) -> (MONO|STEREO) \(([^)]+)\)")
FB = re.compile(r"fb  rp #(\d+): (\d+)x(\d+) layers=(\d+) attachments=(\d+)"
                r"(?: imgs=\[([^\]]*)\])?")
DEAD = re.compile(r"img #(\d+): destroyed")
IMG = re.compile(r"img #(\d+): (\d+)x(\d+)x(\d+) layers=(\d+) mips=(\d+) "
                 r"samples=(\d+) fmt=(\d+) usage=0x([0-9a-f]+)")

VK_USAGE_COLOR = 0x10
VK_USAGE_DEPTH = 0x20


def image_bytes(w, h, d, layers, mips, samples, fmt):
    """Full allocation including the mip chain -- doubling pays for all of it."""
    bpp = BPP.get(fmt, 0)
    total = 0
    for level in range(mips):
        total += max(w >> level, 1) * max(h >> level, 1) * max(d >> level, 1) * bpp
    return total * layers * samples


def main(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    # Serials restart per process; keep only the last X4 instance.
    start = 0
    for i, ln in enumerate(lines):
        if "instance created (app=X4)" in ln:
            start = i
    lines = lines[start:]

    passes, fbs, images = {}, defaultdict(list), {}
    img_passes = defaultdict(set)   # image serial -> render passes using it
    swapchain_attached = set()      # passes with a driver-owned attachment
    destroyed = set()               # images torn down before exit
    for ln in lines:
        m = RP.search(ln)
        if m:
            serial, sub, ncol, fmts, depth, verdict, why = m.groups()
            cols = [int(f[:-1]) for f in fmts.split(",") if f]
            passes.setdefault(int(serial), []).append(
                dict(sub=int(sub), cols=cols,
                     depth=int(depth) if depth else None,
                     verdict=verdict, why=why))
            continue
        m = DEAD.search(ln)
        if m:
            destroyed.add(int(m.group(1)))
            continue
        m = IMG.search(ln)
        if m:
            s, w, h, d, layers, mips, samples, fmt, usage = m.groups()
            images[int(s)] = dict(w=int(w), h=int(h), d=int(d),
                                  layers=int(layers), mips=int(mips),
                                  samples=int(samples), fmt=int(fmt),
                                  usage=int(usage, 16))
            continue
        m = FB.search(ln)
        if m:
            serial, w, h, layers, natt = (int(x) for x in m.groups()[:5])
            fbs[serial].append((w, h, layers, natt))
            for tok in (m.group(6) or "").split(","):
                tok = tok.strip()
                if tok.startswith("#"):
                    img_passes[int(tok[1:])].add(serial)
                elif tok == "?":
                    swapchain_attached.add(serial)

    unknown = set()
    rows = []
    for serial, subs in sorted(passes.items()):
        sizes = fbs.get(serial, [])
        # A pass can be instantiated at several sizes; the largest is what
        # the doubling has to pay for.
        w, h = max(((w, h) for w, h, _, _ in sizes), default=(0, 0))
        verdict = "STEREO" if any(s["verdict"] == "STEREO" for s in subs) else "MONO"
        why = subs[0]["why"]
        fmts = []
        for s in subs:
            fmts += s["cols"] + ([s["depth"]] if s["depth"] else [])
        bpp = 0
        for f in fmts:
            if f not in BPP:
                unknown.add(f)
            bpp += BPP.get(f, 0)
        rows.append(dict(serial=serial, w=w, h=h, verdict=verdict, why=why,
                         fmts=fmts, bytes=w * h * bpp, sized=bool(sizes)))

    def fmt_bytes(n):
        return f"{n / (1 << 20):8.1f} MB"

    print(f"{'pass':>5} {'extent':>11} {'verdict':>7}  {'cost':>11}  formats")
    print("-" * 78)
    for r in rows:
        ext = f"{r['w']}x{r['h']}" if r["sized"] else "(no fb)"
        names = ",".join(NAME.get(f, str(f)) for f in r["fmts"])
        print(f"{r['serial']:>5} {ext:>11} {r['verdict']:>7}  "
              f"{fmt_bytes(r['bytes'])}  {names[:44]}")

    # A pass with no framebuffer was declared but never run this session --
    # X4 creates several variants of each and instantiates one. They cost
    # nothing, but they must still be classified the same as their twins:
    # pipelines are built against a render pass, and giving one variant a view
    # mask while its twin keeps none would split them incompatibly.
    live = [r for r in rows if r["sized"]]
    print()
    print(f"{'verdict':>7} {'reason':>18} {'declared':>9} {'live':>5} "
          f"{'doubling cost':>15}")
    print("-" * 60)
    by = defaultdict(lambda: [0, 0, 0])
    for r in rows:
        key = (r["verdict"], r["why"])
        by[key][0] += 1
        by[key][1] += 1 if r["sized"] else 0
        by[key][2] += r["bytes"]
    for (verdict, why), (n, nlive, b) in sorted(by.items()):
        print(f"{verdict:>7} {why:>18} {n:>9} {nlive:>5} {fmt_bytes(b):>15}")

    tot = sum(r["bytes"] for r in live if r["verdict"] == "STEREO")
    print(f"\nSTEREO, instantiated: {sum(1 for r in live if r['verdict'] == 'STEREO')}"
          f" passes, {fmt_bytes(tot)} if every one is doubled.")
    print("NOTE: this is an upper bound and probably a large overestimate --")
    print("      it counts passes, not images, and passes sharing one target")
    print("      are billed once each. Needs vkCreateImage to resolve.")

    # Reductions give themselves away by shape: a 1-pixel-tall target is a
    # scan, not a view of the world. Exposure is deliberately shared between
    # eyes -- per-eye exposure would let them flicker independently.
    odd = [r for r in live if r["verdict"] == "STEREO" and min(r["w"], r["h"]) <= 4]
    if odd:
        print("\nSTEREO passes shaped like reductions (candidates for MONO):")
        for r in odd:
            print(f"  pass {r['serial']:>3}  {r['w']}x{r['h']}  "
                  + ",".join(NAME.get(f, str(f)) for f in r["fmts"]))

    # ---- the real cost: per image, not per pass ----------------------------
    if not images:
        print("\n(no img lines -- layer predates the vkCreateImage hook)")
        return
    verdict_of = {r["serial"]: r["verdict"] for r in rows}
    attach = {s: i for s, i in images.items()
              if i["usage"] & (VK_USAGE_COLOR | VK_USAGE_DEPTH)}

    used, conflicts, doubled_bytes = [], [], 0
    for s, info in sorted(attach.items()):
        ps = img_passes.get(s, set())
        if not ps:
            continue  # attachment-capable but never bound to a framebuffer
        if s in destroyed:
            continue  # belonged to a scene that has since been torn down
        verdicts = {verdict_of.get(p, "?") for p in ps}
        b = image_bytes(info["w"], info["h"], info["d"], info["layers"],
                        info["mips"], info["samples"], info["fmt"])
        need = "STEREO" in verdicts
        if need:
            doubled_bytes += b
        row = dict(serial=s, info=info, passes=sorted(ps), bytes=b,
                   verdicts=verdicts, need=need)
        used.append(row)
        # An image written by a per-eye pass AND a shared pass cannot simply
        # be doubled: the shared pass writes layer 0 only, so eye 1 reads
        # whatever was there before. Every one of these needs a decision.
        if len(verdicts) > 1:
            conflicts.append(row)

    print(f"\n{'image':>6} {'extent':>12} {'mips':>4} {'passes':>6} "
          f"{'cost':>11}  {'double?':>7}  format")
    print("-" * 78)
    for r in used:
        i = r["info"]
        ext = "{}x{}".format(i["w"], i["h"])
        need = "YES" if r["need"] else "no"
        name = NAME.get(i["fmt"], i["fmt"])
        print(f"{r['serial']:>6} {ext:>12} {i['mips']:>4} {len(r['passes']):>6} "
              f"{fmt_bytes(r['bytes'])}  {need:>7}  {name}")

    print(f"\nattachment images bound to a framebuffer: {len(used)}")
    print(f"of those, touched by a per-eye pass:       "
          f"{sum(1 for r in used if r['need'])}")
    print(f"REAL extra VRAM to double them:           {fmt_bytes(doubled_bytes)}")
    print(f"  (pass-level upper bound was            "
          f"{fmt_bytes(sum(r['bytes'] for r in live if r['verdict'] == 'STEREO'))})")

    if conflicts:
        print("\nCONFLICT — image used by both per-eye and shared passes.")
        print("Doubling it leaves the shared pass writing layer 0 only, so eye")
        print("1 reads stale content. Each needs an explicit decision:")
        for r in conflicts:
            i = r["info"]
            print(f"  img #{r['serial']:<3} {i['w']}x{i['h']} "
                  f"{NAME.get(i['fmt'], i['fmt'])}  passes={r['passes']} "
                  f"verdicts={sorted(r['verdicts'])}")
    if swapchain_attached:
        print(f"\npasses attached to a driver-owned (swapchain) image: "
              f"{sorted(swapchain_attached)}")
        print("  never doubled -- the swapchain is what gets presented.")
    if unknown:
        print(f"\nformats missing from the BPP table: {sorted(unknown)}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/x4vr.log")
