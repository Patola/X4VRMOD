#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Which ENTRY POINT of a dumped module reads which camera member / 3D image.

    tools/spv_entry_scan.py                 # summary across /tmp/x4vr-shaders
    tools/spv_entry_scan.py mod-0365 ...    # per-module detail

**THREE WRONG CONCLUSIONS IN TASK #49 CAME FROM NOT DOING THIS.**

X4's dumped modules carry TWO OpEntryPoints -- (0,'main') vertex and (4,'main')
fragment -- in one SPIR-V module. A whole-module scan therefore attributes every
access to whichever entry point it happened to read: my first scan took the
first (said Vertex), my second kept the last (said Fragment), and the two
disagreed about the same module. Neither was measuring what it claimed.

This walks the call graph from each entry point and attributes an access to a
stage only if a function reachable from that entry point contains it.
"""
import glob
import struct
import sys
from collections import defaultdict

OP_ENTRY, OP_NAME, OP_MEMBER_NAME = 15, 5, 6
OP_DECORATE = 71
OP_TYPE_IMAGE, OP_TYPE_SAMPLED_IMAGE = 25, 27
OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_POINTER, OP_TYPE_STRUCT = 28, 29, 32, 30
OP_CONSTANT, OP_VARIABLE, OP_LOAD, OP_ACCESS_CHAIN = 43, 59, 61, 65
OP_FUNCTION, OP_FUNCTION_END, OP_FUNCTION_CALL = 54, 56, 57
OP_SAMPLED_IMAGE, OP_IMAGE_WRITE, OP_IMAGE = 86, 99, 100
SAMPLE_OPS = set(range(87, 99))
DEC_BINDING, DEC_DESCSET = 33, 34
STAGE = {0: 'Vertex', 4: 'Fragment', 5: 'Compute', 1: 'TessCtl', 2: 'TessEval',
         3: 'Geom'}

def parse(path):
    d = open(path, 'rb').read()
    if len(d) < 20 or struct.unpack('<I', d[:4])[0] != 0x07230203:
        return None
    w = list(struct.unpack(f'<{len(d)//4}I', d[:len(d) // 4 * 4]))
    out, i = [], 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0:
            return None
        out.append((op, w[i:i + n]))
        i += n
    return out

def analyse(path, want_set=1, want_binding=0):
    insts = parse(path)
    if not insts:
        return None
    entries, dec, const, ptr_to, kids = [], defaultdict(dict), {}, {}, defaultdict(list)
    three_d, mnames = set(), {}
    for op, f in insts:
        if op == OP_ENTRY and len(f) >= 3:
            entries.append((f[1], f[2]))
        elif op == OP_MEMBER_NAME and len(f) >= 3:
            s = b''.join(struct.pack('<I', x) for x in f[3:]).split(b'\0')[0]
            mnames[(f[1], f[2])] = s.decode('utf-8', 'replace')
        elif op == OP_DECORATE and len(f) >= 4 and f[2] in (DEC_BINDING, DEC_DESCSET):
            dec[f[1]][f[2]] = f[3]
        elif op == OP_CONSTANT and len(f) >= 4:
            const[f[2]] = f[3]
        elif op == OP_TYPE_IMAGE and len(f) >= 9 and f[3] == 2:
            three_d.add(f[1])
        elif op == OP_TYPE_SAMPLED_IMAGE and len(f) >= 3:
            kids[f[1]].append(f[2])
        elif op in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY) and len(f) >= 3:
            kids[f[1]].append(f[2])
        elif op == OP_TYPE_POINTER and len(f) >= 4:
            ptr_to[f[1]] = f[3]
            kids[f[1]].append(f[3])
        elif op == OP_TYPE_STRUCT and len(f) >= 2:
            kids[f[1]].extend(f[2:])
    changed = True
    while changed:
        changed = False
        for t, cs in kids.items():
            if t not in three_d and any(c in three_d for c in cs):
                three_d.add(t); changed = True

    # split into functions, build the call graph
    fn_body, calls, cur = defaultdict(list), defaultdict(set), None
    for op, f in insts:
        if op == OP_FUNCTION:
            cur = f[2]
        elif op == OP_FUNCTION_END:
            cur = None
        elif cur is not None:
            fn_body[cur].append((op, f))
            if op == OP_FUNCTION_CALL and len(f) >= 4:
                calls[cur].add(f[3])

    cam_vars = {vid for vid, dd in dec.items()
                if dd.get(DEC_DESCSET) == want_set and dd.get(DEC_BINDING) == want_binding}
    struct_of = {}
    for op, f in insts:
        if op == OP_VARIABLE and len(f) >= 3 and f[2] in cam_vars:
            struct_of[f[2]] = ptr_to.get(f[1])

    res = {}
    for model, fn in entries:
        seen, stack = set(), [fn]
        while stack:                       # reachability closure
            g = stack.pop()
            if g in seen:
                continue
            seen.add(g)
            stack.extend(calls.get(g, ()))
        members, tainted, reads3d, writes3d = set(), set(), 0, 0
        for g in seen:
            for op, f in fn_body.get(g, ()):
                if op in (OP_VARIABLE, OP_LOAD, OP_ACCESS_CHAIN, OP_SAMPLED_IMAGE,
                          OP_IMAGE) and len(f) >= 3 and f[1] in three_d:
                    tainted.add(f[2])
                if op == OP_ACCESS_CHAIN and len(f) >= 5:
                    if f[3] in cam_vars and f[4] in const:
                        m = const[f[4]]
                        members.add((m, mnames.get((struct_of.get(f[3]), m), '?')))
                if op in SAMPLE_OPS and len(f) >= 4 and f[3] in tainted:
                    reads3d += 1
                if op == OP_IMAGE_WRITE and len(f) >= 2 and f[1] in tainted:
                    writes3d += 1
        res[STAGE.get(model, model)] = (sorted(members), reads3d, writes3d)
    return res

if __name__ == '__main__':
    want = sys.argv[1:] or None
    files = ([f'/tmp/x4vr-shaders/{m}.spv' for m in want] if want
             else sorted(glob.glob('/tmp/x4vr-shaders/mod-*.spv')))
    if want:
        for p in files:
            r = analyse(p)
            print(f"--- {p.split('/')[-1]} ---")
            for st, (mem, rd, wr) in (r or {}).items():
                print(f"  {st:<9} 3D reads={rd} writes={wr}")
                print(f"    members: {[f'{m}:{n}' for m, n in mem]}")
    else:
        by_stage = defaultdict(lambda: defaultdict(list))
        for p in files:
            r = analyse(p)
            if not r:
                continue
            for st, (mem, rd, wr) in r.items():
                for m, n in mem:
                    by_stage[st][m].append(p.split('/')[-1])
        for st in sorted(by_stage):
            print(f"=== {st} ===")
            for m in sorted(by_stage[st]):
                if m in (1, 2, 3, 4, 21):
                    print(f"   member {m:>2}: {len(by_stage[st][m]):>3} module(s)")
