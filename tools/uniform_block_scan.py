#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Every member of every X4 uniform block, named, counted per shader stage.

    tools/uniform_block_scan.py                 # all blocks
    tools/uniform_block_scan.py 1 0             # just the camera block

X4's dumped modules carry their own `OpMemberName`, so this reads the block
layout out of X4 instead of reconstructing it. That matters twice:

* **Never count members by position.** `V_light_direction_view[3]` and
  `V_evsm[GFX_MAX_CSM]` are ONE member each, so counting `uniform` lines in
  `common.glsl` and calling the n-th one member n is off by however many arrays
  came before it. The names here come from the module being scanned.
* **Never scan a module whole.** Every X4 module holds two entry points, vertex
  and fragment, and the answer "which stage reads this" is different for each.
  Attribution goes through `spv_entry_scan.analyse`, which walks the call graph
  from each entry point.

The counts are per ENTRY POINT, not per module: a member read by both stages of
one module is counted once in each column, which is the number that decides
whether a vertex patch or a fragment patch has to carry a correction.

**The population is a property of the configuration, not of X4.** The injector
forces `ssao=0`, `ssr=false`, `antialiasing=none`, `distortion=false`, so the
shaders those settings would compile are absent and their members read zero.
`M_projection_uj` is the clearest case: zero readers here, two in X4's GLSL
(`ssao_v2.frag`, `ssr.frag`). A zero in this table means "nothing in the dumps
we have", never "X4 does not use it".
"""
import glob
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv_entry_scan as S

# (label, set, binding). Set 0 is the bindless arena and has no member list;
# set 2's layout is generated per shader family by MACRO_BUFFER_MATERIAL, so
# the same member index means different things in different modules -- it is
# listed for completeness and its names must not be compared across rows.
BLOCKS = [
    ('CAMERA  (per view)', 1, 0),
    ('MATERIAL (per family -- indices NOT comparable across modules)', 2, 0),
    ('WORLD   (per object)', 3, 0),
    ('DYNAMIC (per draw)', 4, 0),
]


def scan(files, wset, wbind):
    by_stage = defaultdict(lambda: defaultdict(set))
    names = defaultdict(set)
    for p in files:
        r = S.analyse(p, want_set=wset, want_binding=wbind)
        if not r:
            continue
        for st, (mem, _rd, _wr) in r.items():
            for m, n in mem:
                by_stage[st][m].add(os.path.basename(p))
                if n != '?':
                    names[m].add(n)
    return by_stage, names


def main():
    files = sorted(glob.glob('/tmp/x4vr-shaders/mod-*.spv'))
    if not files:
        sys.exit('no dumps in /tmp/x4vr-shaders -- run with X4VR_MV_DUMP=1')
    want = None
    if len(sys.argv) == 3:
        want = (int(sys.argv[1]), int(sys.argv[2]))
    print(f'{len(files)} modules in /tmp/x4vr-shaders\n')
    for label, wset, wbind in BLOCKS:
        if want and want != (wset, wbind):
            continue
        by_stage, names = scan(files, wset, wbind)
        if not by_stage:
            print(f'### {label}  set {wset} binding {wbind}: '
                  'no module declares it\n')
            continue
        allm = sorted({m for st in by_stage for m in by_stage[st]})
        stages = sorted(by_stage)
        print(f'### {label}  set {wset} binding {wbind} '
              f'-- {len(allm)} members read')
        print(f"{'mem':>4} {'name':<30} " + ''.join(f'{s:>10}' for s in stages))
        for m in allm:
            n = sorted(names.get(m, {'?'}))
            label_n = n[0] if len(n) == 1 else n[0] + f' (+{len(n) - 1} other)'
            row = ''.join(f'{len(by_stage[s].get(m, ())):>10}' for s in stages)
            print(f'{m:>4} {label_n:<30} {row}')
        print()


if __name__ == '__main__':
    main()
