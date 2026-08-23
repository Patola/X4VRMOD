#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Name the modules in a run's pipe inventory against the dumped mod-NNNN.spv.

X4's shader modules have no names. The frame analysis identifies them as
mod-NNNN from the dumps in /tmp/x4vr-shaders, and the layer's task #42 lines
identify them by an FNV-1a hash of the unpatched SPIR-V. Hashing the dumps the
same way is the join, and it is the whole reason the hash is in the log:
without it "mod#217" means nothing outside the run that produced it.

    tools/module_map.py <log> [dumpdir]

Prints, per render pass and subpass, which dumped modules draw it, how they
were classified, and which variant the pipeline actually got -- which is the
question "what drew the thing that looks wrong in the headset" reduces to.
"""
import collections
import glob
import os
import re
import sys


def fnv1a(path):
    with open(path, 'rb') as f:
        data = f.read()
    h = 2166136261
    for i in range(0, len(data) - 3, 4):
        h ^= int.from_bytes(data[i:i + 4], 'little')
        h = (h * 16777619) & 0xffffffff
    return h


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    log = sys.argv[1]
    dumpdir = sys.argv[2] if len(sys.argv) > 2 else '/tmp/x4vr-shaders'

    by_hash = {}
    for p in sorted(glob.glob(os.path.join(dumpdir, 'mod-*.spv'))):
        by_hash[fnv1a(p)] = os.path.basename(p)[:-4]
    if not by_hash:
        print(f"no dumps in {dumpdir} — module numbers will be unnamed\n")

    text = open(log, errors='replace').read()
    lines = re.findall(r'pipe: rp #(\d+|4294967295)\.(\d+) \[([^\]]*)\](.*)',
                       text)
    # `bind rp #N ...` — the pass that actually BOUND the pipeline, which is not
    # always the one it was created against. Vulkan lets a pipeline be used with
    # any render pass COMPATIBLE with its own, and X4 uses that: take 192's
    # creation-keyed output named rp #26/#28/#30, three passes that never
    # execute, and missed #27/#29/#31, the three that do. Reading only `pipe:`
    # answers "created against" while every question asked here is "used by".
    binds = re.findall(r'bind rp #(\d+) (.*)', text)
    if not lines and not binds:
        print("no 'pipe:' or 'bind' lines — the run needs X4VR_PIPE_INVENTORY=1")
        return 1

    # A pass draws the same thing every frame, and X4 rebuilds pipelines, so
    # collapse to the distinct (module, variant) set per pass rather than
    # printing one line per pipeline creation.
    passes = collections.defaultdict(set)
    flags = {}
    unnamed = set()
    for rp, sp, fl, rest in lines:
        key = ('rp #' + rp if rp != '4294967295' else 'rp #?') + '.' + sp
        flags[key] = fl
        for m in re.finditer(
                r's(\d+)=mod#(\d+)/([0-9a-f]{8})\(([^)]*)\)->(\w+)', rest):
            _stage, serial, h, attrs, variant = m.groups()
            name = by_hash.get(int(h, 16))
            if name is None:
                unnamed.add(h)
                name = 'mod#' + serial
            passes[key].add((name, attrs, variant))

    # The bind-time view, built with the SAME parser so the two cannot drift.
    bound = collections.defaultdict(set)
    for rp, rest in binds:
        key = 'rp #' + rp
        for m in re.finditer(
                r's(\d+)=mod#(\d+)/([0-9a-f]{8})\(([^)]*)\)->(\w+)', rest):
            _stage, serial, h, attrs, variant = m.groups()
            name = by_hash.get(int(h, 16))
            if name is None:
                unnamed.add(h)
                name = 'mod#' + serial
            bound[key].add((name, attrs, variant))

    # Keyed on the LINES, not the parsed modules. Keying on `bound` reported
    # "this build predates bind-time attribution" for a log that had the lines
    # and simply carried no module detail -- absence of parseable content
    # dressed as absence of the feature, which is the exact confusion this
    # whole section exists to remove.
    if binds and not bound:
        print(f"{len(binds)} 'bind rp #' line(s), none carrying module detail "
              f"— the passes bound pipelines whose modules were never "
              f"inventoried (unpatched, or created before the knob).\n")
    if bound:
        print("=== BOUND AT DRAW (what each pass actually used) ===\n")
        for key in sorted(bound, key=lambda k: int(k.split('#')[1])):
            print(f"{key}")
            for name, attrs, variant in sorted(bound[key]):
                print(f"    {name:<10} {attrs:<34} -> {variant}")
            print()
        created = {k.split('.')[0] for k in passes}
        only_created = sorted(created - set(bound),
                              key=lambda k: int(k.split('#')[1]))
        only_bound = sorted(set(bound) - created,
                            key=lambda k: int(k.split('#')[1]))
        if only_created:
            print("passes named at CREATION but never seen binding: "
                  + ', '.join(only_created))
        if only_bound:
            print("passes that BOUND a pipeline created against another pass: "
                  + ', '.join(only_bound))
        if only_created or only_bound:
            print("  ^ that disagreement is render-pass compatibility, and it "
                  "is the whole reason\n    this section exists. Read the "
                  "bind list, not the creation list.\n")
    elif not binds:
        print("no 'bind rp #' lines — this build predates bind-time "
              "attribution, so everything below is CREATED-AGAINST and may "
              "name passes that never execute.\n")
        print("=== CREATED AGAINST (may not be the pass that used it) ===\n")

    for key in sorted(passes, key=lambda k: (len(k), k)):
        print(f"{key}  [{flags[key]}]")
        for name, attrs, variant in sorted(passes[key]):
            print(f"    {name:<10} {attrs:<34} -> {variant}")
        print()

    if unnamed:
        print(f"{len(unnamed)} module(s) had no match in {dumpdir}. That is "
              f"expected if the dumps came from a different X4 version or a "
              f"different graphics preset — re-dump before reading names.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
