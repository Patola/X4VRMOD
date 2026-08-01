#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Find vertex modules that leak the *unsheared* clip position into a varying.

`patch_vertex_clip` appends `gl_Position = K * gl_Position` before every return
of the vertex entry point. It therefore shears the value that reaches the
rasterizer, and nothing else. Any output the shader computed **from that same
clip position earlier in the function** keeps the centre-camera value.

That is a defect with exactly the shape this project is chasing. A fragment
shader handed such a varying -- typically `clip.xy/clip.w` remapped to a screen
UV -- samples a screen-space buffer at where the centre camera would have put
the surface, while the fragment itself was rasterized where the *sheared* camera
puts it. The two disagree by the full disparity, and the sign is opposite in the
two eyes, so the error follows the shear sign with no dependence on which array
layer the eye is stored in. That is precisely what take 79 measured when a
negative IPD swapped the two eyes' results exactly.

The scan is a def-use reachability question:

    P = the id stored to gl_Position
    flag any Output variable (other than Position itself) whose stored value
    transitively depends on P

Two traps this is written around, both already paid for in this project:

  1. **X4 ships combined vertex+fragment modules.** A whole-module scan answers
     a different question. Resolve the *Vertex* entry point and follow
     `OpFunctionCall` transitively, exactly as shadow_scan.py does.

  2. **gl_Position is usually a member of a `gl_PerVertex` block**, reached by
     `OpAccessChain`, not a bare variable. Resolve both spellings; looking only
     for a variable decorated `BuiltIn Position` finds nothing and reports a
     reassuring zero.

Usage:

    tools/clip_varying_scan.py [dump-dir] [module-number ...]

Needs `spirv-dis` on PATH.
"""
import glob
import os
import re
import subprocess
import sys

BUILTIN_POSITION = '0'  # BuiltIn Position == 0


def disasm(path):
    r = subprocess.run(['spirv-dis', '--no-color', path],
                       capture_output=True, text=True)
    return r.stdout


def reachable_body(text, stage):
    """The stage's entry function plus everything it calls, transitively."""
    ep = re.search(r'OpEntryPoint ' + stage + r' (%\S+)', text)
    if not ep:
        return None
    bodies = {}
    for m in re.finditer(r'(%\S+) = OpFunction .*?OpFunctionEnd', text, re.S):
        bodies[m.group(1)] = m.group(0)
    seen, stack, out = set(), [ep.group(1)], []
    while stack:
        fn = stack.pop()
        if fn in seen or fn not in bodies:
            continue
        seen.add(fn)
        out.append(bodies[fn])
        for c in re.findall(r'= OpFunctionCall %\S+ (%\S+)', bodies[fn]):
            stack.append(c)
    return '\n'.join(out)


def analyse(path):
    t = disasm(path)
    if not t:
        return None
    body = reachable_body(t, 'Vertex')
    if body is None:
        return None  # not a vertex module

    # --- which pointers denote gl_Position ------------------------------
    pos_vars = set(re.findall(
        r'OpDecorate (%\S+) BuiltIn Position', t))
    # The block spelling: OpMemberDecorate %struct <n> BuiltIn Position, then a
    # variable of that struct type, reached by OpAccessChain %ptr %var %int_n.
    pos_members = {}
    for sid, idx in re.findall(
            r'OpMemberDecorate (%\S+) (\d+) BuiltIn Position', t):
        pos_members[sid] = idx
    ptr_of = {p: ty for p, ty in
              re.findall(r'(%\S+) = OpTypePointer \w+ (%\S+)', t)}
    var_struct = {}
    for v, p in re.findall(r'(%\S+) = OpVariable (%\S+) Output', t):
        var_struct[v] = ptr_of.get(p)
    const_val = {c: v for c, v in
                 re.findall(r'(%\S+) = OpConstant %\S+ (\d+)', t)}

    pos_ptrs = set(pos_vars)
    for res, base, idx in re.findall(
            r'(%\S+) = OpAccessChain %\S+ (%\S+) (%\S+)', body):
        st = var_struct.get(base)
        if st in pos_members and const_val.get(idx) == pos_members[st]:
            pos_ptrs.add(res)

    # --- the value stored to gl_Position --------------------------------
    stored_pos = [val for ptr, val in
                  re.findall(r'OpStore (%\S+) (%\S+)', body) if ptr in pos_ptrs]
    if not stored_pos:
        return dict(vertex=True, pos=False)

    # --- def-use: operands of every result-producing instruction --------
    uses = {}
    for line in body.splitlines():
        m = re.match(r'\s*(%\S+) = Op\w+(.*)$', line)
        if not m:
            continue
        uses[m.group(1)] = set(re.findall(r'%\S+', m.group(2)))

    def depends_on(root, targets, limit=20000):
        seen, stack, n = set(), [root], 0
        while stack and n < limit:
            i = stack.pop()
            n += 1
            if i in seen:
                continue
            seen.add(i)
            if i in targets:
                return True
            stack.extend(uses.get(i, ()))
        return False

    # ONLY the final stored value. X4 uses gl_Position as a scratch variable:
    #
    #     OpStore %pos %365          ; gl_Position = vec4(object_pos, 1)
    #     %370 = OpLoad %pos
    #     %371 = %M * %370           ; transform it
    #     OpStore %pos %371          ; the value that actually reaches the raster
    #
    # so intermediate values -- object, world and view position -- pass through
    # gl_Position on their way to the varyings. Treating every stored value as
    # "the clip position" flagged 280 of 382 modules, which is just "this
    # varying derives from the vertex position" and answers nothing. The patch
    # shears the *last* store, so that is the only value whose leakage matters.
    targets = {stored_pos[-1]}

    # --- outputs other than Position ------------------------------------
    out_vars = set(re.findall(r'(%\S+) = OpVariable %\S+ Output', t))
    names = dict(re.findall(r'OpName (%\S+) "([^"]*)"', t))
    leaks = []
    for ptr, val in re.findall(r'OpStore (%\S+) (%\S+)', body):
        if ptr in pos_ptrs:
            continue
        base = ptr
        for res, b, _i in re.findall(
                r'(%\S+) = OpAccessChain %\S+ (%\S+) (%\S+)', body):
            if res == ptr:
                base = b
                break
        if base not in out_vars:
            continue
        if base in pos_vars or var_struct.get(base) in pos_members:
            continue
        if depends_on(val, targets):
            leaks.append(names.get(base, base))
    return dict(vertex=True, pos=True, leaks=sorted(set(leaks)))


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/tmp/x4vr-shaders-take82'
    want = set(sys.argv[2:])
    nvert = nleak = 0
    rows = []
    for p in sorted(glob.glob(os.path.join(d, 'mod-*.spv'))):
        n = int(re.search(r'mod-(\d+)', p).group(1))
        if want and str(n) not in want:
            continue
        r = analyse(p)
        if not r or not r.get('vertex'):
            continue
        nvert += 1
        if r.get('leaks'):
            nleak += 1
            rows.append((n, r['leaks']))
    for n, leaks in rows:
        print('mod %-4d leaks clip position into: %s' % (n, ', '.join(leaks)))
    print('--- %d vertex modules scanned, %d leak the unsheared clip position'
          % (nvert, nleak))


if __name__ == '__main__':
    main()
