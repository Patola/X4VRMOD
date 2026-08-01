#!/usr/bin/env python3
"""Which dumped modules sample a *depth-comparison* image, and from which
binding.

Two traps this script is written around:

  1. X4 ships combined vertex+fragment modules. A whole-module scan answers a
     different question than "what does the fragment stage do" -- the exact
     mistake recorded for classify() at take 60. So: resolve the Fragment
     entry point, follow OpFunctionCall transitively, and look only there.

  2. The descriptor a sample came from is several indirections away from the
     sample instruction: OpImageSampleDref* takes a sampled-image id, which
     comes from OpSampledImage, whose image comes from OpLoad of an
     OpAccessChain into the bindless array variable. Walk it back rather than
     guessing from proximity.
"""
import re
import subprocess
import sys
import os
import glob

SAMPLE_OPS = re.compile(r'= (OpImageSample\w*|OpImageFetch|OpImageGather\w*|'
                        r'OpImageSparseSample\w*|OpImageRead)\b')


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
    body = reachable_body(t, 'Fragment')
    if body is None:
        return None

    dset = {v: int(n) for v, n in
            re.findall(r'OpDecorate (%\S+) DescriptorSet (\d+)', t)}
    dbind = {v: int(n) for v, n in
             re.findall(r'OpDecorate (%\S+) Binding (\d+)', t)}

    # Type table: which OpTypeImage ids carry the Depth flag. Operand order is
    # OpTypeImage <sampled-type> <dim> <depth> <arrayed> <ms> <sampled> <fmt>.
    depth_img = set()
    for tid, dim, dep in re.findall(
            r'(%\S+) = OpTypeImage %\S+ (\w+) (\d+) ', t):
        if dep == '1':
            depth_img.add(tid)

    # Variable -> its pointee type id, so a sample can be attributed to a set
    # and binding. Chase through OpTypePointer and the array/struct wrappers.
    ptr = {p: (sc, ty) for p, sc, ty in
           re.findall(r'(%\S+) = OpTypePointer (\w+) (%\S+)', t)}
    arr = {a: e for a, e in
           re.findall(r'(%\S+) = OpType(?:Runtime)?Array (%\S+)', t)}
    var_ty = {}
    for v, p in re.findall(r'(%\S+) = OpVariable (%\S+) UniformConstant', t):
        ty = ptr.get(p, (None, None))[1]
        while ty in arr:
            ty = arr[ty]
        var_ty[v] = ty

    # id -> originating variable, propagated across the loads and chains that
    # sit between the bindless array and the sample instruction.
    origin = {}
    for res, base in re.findall(r'(%\S+) = OpAccessChain %\S+ (%\S+)', body):
        origin[res] = base
    for res, src in re.findall(r'(%\S+) = OpLoad %\S+ (%\S+)', body):
        origin[res] = src
    for res, img in re.findall(r'(%\S+) = OpSampledImage %\S+ (%\S+) %\S+',
                               body):
        origin[res] = img
    for res, img in re.findall(r'(%\S+) = OpImage %\S+ (%\S+)', body):
        origin[res] = img
    for res, srcs in re.findall(r'(%\S+) = OpCopyObject %\S+ (%\S+)', body):
        origin[res] = srcs

    def root(i):
        seen = set()
        while i in origin and i not in seen:
            seen.add(i)
            i = origin[i]
        return i

    hits = []
    for line in body.splitlines():
        m = SAMPLE_OPS.search(line)
        if not m:
            continue
        op = m.group(1)
        args = line.split('=', 1)[1].split()
        # <op> <result-type> <sampled-image> ...
        if len(args) < 3:
            continue
        r = root(args[2])
        hits.append(dict(op=op, var=r, set=dset.get(r), bind=dbind.get(r),
                         depth=var_ty.get(r) in depth_img,
                         dref='Dref' in op))
    return hits


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/tmp/x4vr-shaders-take74'
    want = set(sys.argv[2:])
    rows = []
    for p in sorted(glob.glob(os.path.join(d, 'mod-*.spv'))):
        n = int(re.search(r'mod-(\d+)', p).group(1))
        if want and str(n) not in want:
            continue
        h = analyse(p)
        if h is None:
            continue
        rows.append((n, h))

    ndref = 0
    for n, hits in rows:
        dr = [x for x in hits if x['dref'] or x['depth']]
        if not dr:
            continue
        ndref += 1
        where = sorted({(x['set'], x['bind'], x['op']) for x in dr})
        print('mod %-4d %s' % (n, '  '.join(
            'set%s/bind%s %s' % (s, b, o) for s, b, o in where)))
    print('--- %d fragment modules scanned, %d sample a depth image' %
          (len(rows), ndref))


if __name__ == '__main__':
    main()
