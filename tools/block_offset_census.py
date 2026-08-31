#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""What ELSE lives at byte N of a uniform block -- the test before writing it.

    tools/block_offset_census.py 4 0                 # every layout, summarised
    tools/block_offset_census.py 4 0 --at 512        # who else is at byte 512
    tools/block_offset_census.py 4 0 --name fsr3g    # where a member lives
    tools/block_offset_census.py 4 0 --layout mod-0012

**This exists because `uniform_block_scan.py` answers a different question and
answering the wrong one nearly cost a global corruption.** That tool aggregates
by member INDEX, which is right for the camera block, where one layout covers
every slot. Sets 2 and 4 are generated per shader family (`MACRO_BUFFER_MATERIAL`,
`MACRO_BUFFER_DYNAMIC`), so the same index -- and the same byte -- is a
different variable in different modules.

A host write addresses a BYTE, and X4 binds many families through one
descriptor slot. So the question a write has to survive is not "do the modules
I care about agree on the offset" -- four permutations of the same shader
always will -- but **"what does every OTHER module keep there"**. For set 4
byte 512 that is 54x `U_patternstr`, 32x `U_pattern_color`, 14x
`U_diffuse_tiling` and six more, against 4x `U_fsr3g_useuitexture`: a write
would zero a float in 119 families to set a flag in four.

Reads the layout out of X4's own `OpMemberName`/`OpMemberDecorate Offset`, so
nothing here is reconstructed from `common.glsl`. Dumps are Egosoft's:
`/tmp/x4vr-shaders`, never committed.
"""
import argparse
import glob
import struct
import sys
from collections import Counter, defaultdict

OP_NAME, OP_MEMBER_NAME, OP_ENTRY = 5, 6, 15
OP_DECORATE, OP_MEMBER_DECORATE = 71, 72
OP_TYPE_BOOL, OP_TYPE_INT, OP_TYPE_FLOAT = 20, 21, 22
OP_TYPE_VECTOR, OP_TYPE_MATRIX = 23, 24
OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 28, 30, 32
OP_CONSTANT, OP_VARIABLE, OP_ACCESS_CHAIN = 43, 59, 65
OP_FUNCTION, OP_FUNCTION_END, OP_FUNCTION_CALL = 54, 56, 57
DEC_BINDING, DEC_DESCSET, DEC_OFFSET = 33, 34, 35
STAGE = {0: 'Vert', 4: 'Frag', 5: 'Comp'}


def parse(path):
    d = open(path, 'rb').read()
    if len(d) < 20 or struct.unpack('<I', d[:4])[0] != 0x07230203:
        return None
    w = list(struct.unpack(f'<{len(d) // 4}I', d[:len(d) // 4 * 4]))
    out, i = [], 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0:
            return None
        out.append((op, w[i:i + n]))
        i += n
    return out


def wstr(words):
    return b''.join(struct.pack('<I', x) for x in words).split(b'\0')[0] \
        .decode('utf-8', 'replace')


def analyse(path, want_set, want_binding):
    """(members, read_by) for the block, or None if the module lacks it.

    members is [(index, name, byte_offset, type)]; read_by maps stage -> set of
    member indices reached by an OpAccessChain from that entry point. The stage
    walk matters for the same reason spv_entry_scan exists: one X4 module holds
    both a vertex and a fragment entry point.
    """
    insts = parse(path)
    if not insts:
        return None
    dec = defaultdict(dict)
    moff = defaultdict(dict)
    mnames, tnames = {}, {}
    types, const, ptr_to, entries = {}, {}, {}, []
    for op, f in insts:
        if op == OP_ENTRY and len(f) >= 3:
            entries.append((f[1], f[2]))
        elif op == OP_NAME and len(f) >= 3:
            tnames[f[1]] = wstr(f[2:])
        elif op == OP_MEMBER_NAME and len(f) >= 4:
            mnames[(f[1], f[2])] = wstr(f[3:])
        elif op == OP_DECORATE and len(f) >= 4 and f[2] in (DEC_BINDING,
                                                            DEC_DESCSET):
            dec[f[1]][f[2]] = f[3]
        elif op == OP_MEMBER_DECORATE and len(f) >= 5 and f[3] == DEC_OFFSET:
            moff[f[1]][f[2]] = f[4]
        elif op == OP_CONSTANT and len(f) >= 4:
            const[f[2]] = f[3]
        elif op == OP_TYPE_POINTER and len(f) >= 4:
            ptr_to[f[1]] = f[3]
        elif op == OP_TYPE_STRUCT and len(f) >= 2:
            types[f[1]] = ('struct', list(f[2:]))
        elif op == OP_TYPE_ARRAY and len(f) >= 4:
            types[f[1]] = ('array', f[2], f[3])
        elif op == OP_TYPE_MATRIX and len(f) >= 4:
            types[f[1]] = ('mat', f[2], f[3])
        elif op == OP_TYPE_VECTOR and len(f) >= 4:
            types[f[1]] = ('vec', f[2], f[3])
        elif op == OP_TYPE_FLOAT and len(f) >= 3:
            types[f[1]] = ('float',)
        elif op == OP_TYPE_INT and len(f) >= 4:
            types[f[1]] = ('int' if f[3] else 'uint',)
        elif op == OP_TYPE_BOOL and len(f) >= 2:
            types[f[1]] = ('bool',)

    def tname(t, depth=0):
        e = types.get(t)
        if not e or depth > 4:
            return '?'
        if e[0] in ('float', 'int', 'uint', 'bool'):
            return e[0]
        if e[0] == 'vec':
            base = tname(e[1], depth + 1)
            return f'vec{e[2]}' if base == 'float' else f'{base}vec{e[2]}'
        if e[0] == 'mat':
            return f'mat{e[2]}'
        if e[0] == 'array':
            return f'{tname(e[1], depth + 1)}[{const.get(e[2], "?")}]'
        return e[0]

    # A UNIFORM BLOCK, not merely something at that (set, binding). The dumps
    # include four modules of OUR OWN -- the cursor shim declares g_tCursor at
    # set 1 binding 0 -- and without the struct test they counted as camera
    # blocks with zero members, i.e. as a second "layout" of X4's block.
    var = stype = None
    for op, f in insts:
        if op == OP_VARIABLE and len(f) >= 3:
            dd = dec.get(f[2], {})
            if dd.get(DEC_DESCSET) == want_set and \
               dd.get(DEC_BINDING) == want_binding:
                t = ptr_to.get(f[1])
                if types.get(t, ('',))[0] == 'struct':
                    var, stype = f[2], t
    if var is None or stype is None:
        return None
    members = [(i, mnames.get((stype, i), '?'), moff[stype].get(i), tname(mt))
               for i, mt in enumerate(types.get(stype, ('struct', []))[1])]

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
    read_by = {}
    for model, fn in entries:
        seen, stack = set(), [fn]
        while stack:
            g = stack.pop()
            if g in seen:
                continue
            seen.add(g)
            stack.extend(calls.get(g, ()))
        got = set()
        for g in seen:
            for op, f in fn_body.get(g, ()):
                if op == OP_ACCESS_CHAIN and len(f) >= 5 and f[3] == var \
                        and f[4] in const:
                    got.add(const[f[4]])
        read_by[STAGE.get(model, str(model))] = got
    return members, read_by, tnames.get(stype, '?')


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('set', type=int)
    ap.add_argument('binding', type=int)
    ap.add_argument('--at', type=int, help='census of one byte offset')
    ap.add_argument('--name', help='find members whose name contains this')
    ap.add_argument('--layout', nargs='+', help='full layout of these modules')
    ap.add_argument('--dumps', default='/tmp/x4vr-shaders')
    a = ap.parse_args()

    files = sorted(glob.glob(f'{a.dumps}/mod-*.spv'))
    if not files:
        sys.exit(f'no dumps in {a.dumps} -- run with X4VR_MV_DUMP=1')
    mods = [(p, r) for p, r in ((p, analyse(p, a.set, a.binding))
                                for p in files) if r]
    print(f'{len(mods)} of {len(files)} dumps declare set {a.set} '
          f'binding {a.binding}\n')

    def rd(read_by, i):
        return ','.join(s for s, g in read_by.items() if i in g) or '-'

    if a.layout:
        for p, (members, read_by, bname) in mods:
            if not any(x in p for x in a.layout):
                continue
            print(f"=== {p.split('/')[-1]}  block '{bname}' ===")
            for i, n, off, t in members:
                print(f'  m{i:<3} @{str(off):>5}  {t:<10} {n:<34} '
                      f'read:{rd(read_by, i)}')
        return

    if a.name:
        for p, (members, read_by, _b) in mods:
            for i, n, off, t in members:
                if a.name in n:
                    print(f"  {p.split('/')[-1]}  m{i:<3} @{str(off):>5} "
                          f'{t:<8} {n:<32} read:{rd(read_by, i)}')
        return

    if a.at is not None:
        here, span = Counter(), 0
        for _p, (members, _r, _b) in mods:
            offs = [o for _i, _n, o, _t in members if o is not None]
            hit = False
            for i, n, off, t in members:
                if off == a.at:
                    here[(t, n)] += 1
                    hit = True
            if not hit and offs and min(offs) < a.at < max(offs):
                span += 1
        total = sum(here.values())
        print(f'byte {a.at}: {total} module(s) start a member exactly there, '
              f'{span} more straddle it\n')
        print(f"{'modules':>8}  {'type':<10} member")
        for (t, n), c in here.most_common():
            print(f'{c:>8}  {t:<10} {n}')
        if total > 1 and len(here) > 1:
            print(f'\n*** {len(here)} DIFFERENT members share byte {a.at}. '
                  'A host write by offset hits all of them. ***')
        return

    shapes = Counter()
    for _p, (members, _r, _b) in mods:
        last = max((o for _i, _n, o, _t in members if o is not None),
                   default=None)
        shapes[(len(members), last)] += 1
    print(f'{len(shapes)} distinct layouts\n')
    print(f"{'members':>8} {'last offset':>12} {'modules':>8}")
    for (n, last), c in sorted(shapes.items()):
        print(f'{n:>8} {str(last):>12} {c:>8}')
    if len(shapes) > 1:
        print('\nMore than one layout: member indices and byte offsets are '
              'NOT comparable across modules.\nUse --at before writing any '
              'offset in this block.')


if __name__ == '__main__':
    main()
