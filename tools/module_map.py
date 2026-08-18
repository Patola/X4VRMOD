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
    if not lines:
        print("no 'pipe:' lines — the run needs X4VR_PIPE_INVENTORY=1")
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
