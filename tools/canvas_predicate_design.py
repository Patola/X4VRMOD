#!/usr/bin/env python3
"""Design the third module category offline, over every dumped module.

Task #30 needs a category the two-way World/NonWorld split cannot express.
World geometry gets a shear whose per-eye offset scales as 1/z. Fullscreen
post passes must get nothing at all -- shearing a fullscreen triangle
displaces the quad while the buffers it samples stay put, which is what take
70 measured as an 85% per-eye disagreement in the lighting. The UI wants the
third thing: a *constant* per-eye offset, which is what a fixed virtual depth
is.

Today both of the last two live in NonWorld, so no knob can address one
without the other. This script asks whether they are separable statically.

The candidate separator is whether the *vertex stage* draws from a vertex
buffer at all:

  * a fullscreen triangle synthesises its position from gl_VertexIndex and
    declares no Location-decorated Input;
  * a UI quad has real vertex attributes.

Ground truth is the run, not a guess: which modules X4 actually bound to its
world passes, its shadow passes, its fullscreen post passes and its LDR UI
pass. The log and the dump must come from the SAME take -- module and pass
serials are per-run.

Usage:  tools/canvas_predicate_design.py [LOG] [DUMPDIR]
"""
import collections
import os
import re
import subprocess
import sys

LOG = sys.argv[1] if len(sys.argv) > 1 else '/tmp/x4vr-take80.log'
DUMP = sys.argv[2] if len(sys.argv) > 2 else '/tmp/x4vr-shaders-take80'

# --- runtime ground truth ------------------------------------------------
# Pass shape, exactly as the layer classified it, and the verdict it reached.
# Parsed rather than recomputed so a disagreement between this script and the
# layer shows up as a parse failure instead of as a quiet second opinion.
INV = re.compile(r'layer   rp #(\d+)\.\d+: (\d+) colour \[([^\]]*)\] '
                 r'(no-depth|depth \d+) final=(-?\d+) -> (\w+) \(([^)]*)\)')
BIND = re.compile(r'mv final: rp #(\d+) <- frag module #(\d+) \((mod-\d+)\.spv\)')

shape, verdict = {}, {}
pass_mods = collections.defaultdict(set)
for line in open(LOG, encoding='utf-8', errors='replace'):
    m = INV.search(line)
    if m:
        rp = int(m.group(1))
        shape[rp] = (int(m.group(2)), m.group(3), m.group(4))
        verdict[rp] = (m.group(6), m.group(7))
    m = BIND.search(line)
    if m:
        pass_mods[int(m.group(1))].add(m.group(3))

if not shape or not pass_mods:
    sys.exit(f'{LOG}: no inventory or no bindings -- was it run with '
             'X4VR_MV_INVENTORY=1?')


def passes_where(pred):
    return sorted(rp for rp in shape if pred(rp))


def is_ldr(fmts):
    return bool(fmts) and all(f.endswith('L') for f in fmts.split(',') if f)


world_rps = passes_where(lambda rp: verdict[rp][0] == 'STEREO')
shadow_rps = passes_where(lambda rp: shape[rp][0] == 0)
ldr_rps = passes_where(lambda rp: shape[rp][0] > 0 and is_ldr(shape[rp][1]))
hdr_post_rps = passes_where(lambda rp: verdict[rp][1] == 'fullscreen post'
                            and not is_ldr(shape[rp][1]))

GROUPS = [('world', world_rps), ('shadow', shadow_rps),
          ('ldr (UI + blits)', ldr_rps), ('hdr fullscreen post', hdr_post_rps)]

print(f'log  {LOG}\ndump {DUMP}\n')
print(f'{len(shape)} passes, {len(set().union(*pass_mods.values()))} '
      'modules bound\n')
for name, rps in GROUPS:
    mods = set().union(*[pass_mods[r] for r in rps]) if rps else set()
    print(f'{name:22s} {len(rps):3d} passes -> {len(mods):3d} modules  '
          f'rp {rps if len(rps) < 12 else str(rps[:11]) + "..."}')
print()

# --- per-module static analysis -----------------------------------------
# Everything here is restricted to the vertex entry point where the question
# is about the vertex stage. X4 ships combined vertex+fragment modules, so a
# whole-module scan answers a different question: 247 of 409 modules read a
# camera matrix in their *fragment* stage.
BUILTIN_VERTEX_INDEX = 42
BUILTIN_INSTANCE_INDEX = 43


def analyse(path):
    text = subprocess.run(['spirv-dis', path], capture_output=True,
                          text=True).stdout
    if not text:
        return None

    ep = re.search(r'OpEntryPoint Vertex (%\S+) "[^"]*"([^\n]*)', text)
    if not ep:
        return None
    iface = set(re.findall(r'%\S+', ep.group(2)))

    dset, dbind, dloc = {}, {}, {}
    for v, n in re.findall(r'OpDecorate (%\S+) DescriptorSet (\d+)', text):
        dset[v] = int(n)
    for v, n in re.findall(r'OpDecorate (%\S+) Binding (\d+)', text):
        dbind[v] = int(n)
    for v, n in re.findall(r'OpDecorate (%\S+) Location (\d+)', text):
        dloc[v] = int(n)
    builtin = {v: int(n) for v, n in
               re.findall(r'OpDecorate (%\S+) BuiltIn (\d+)', text)}
    for v, name in re.findall(r'OpDecorate (%\S+) BuiltIn (\w+)', text):
        builtin[v] = name

    # Storage class comes from the OpVariable itself, not from a decoration.
    storage = {}
    for v, sc in re.findall(r'(%\S+) = OpVariable %\S+ (\w+)', text):
        storage[v] = sc

    # A vertex *attribute* is an Input variable in the vertex entry point's
    # interface. Restricting to the interface is what keeps the fragment
    # stage's own Location-decorated Inputs (which are the vertex stage's
    # Outputs) out of the count.
    attrs = sorted(dloc[v] for v in iface
                   if storage.get(v) == 'Input' and v in dloc)
    vidx = any(builtin.get(v) in (BUILTIN_VERTEX_INDEX, 'VertexIndex')
               for v in iface)
    iidx = any(builtin.get(v) in (BUILTIN_INSTANCE_INDEX, 'InstanceIndex')
               for v in iface)

    consts = {c: int(val) for c, val in
              re.findall(r'(%\S+) = OpConstant %\S+ (-?\d+)\b', text)}
    cam = {v for v in dset if dset[v] == 1 and dbind.get(v) == 0}
    obj = {v for v in dset if dset[v] == 3}

    # Members read from a block, inside the vertex function only.
    body = text.split(f'{ep.group(1)} = OpFunction', 1)
    body = body[1].split('OpFunctionEnd', 1)[0] if len(body) > 1 else ''
    cam_members, obj_members = set(), set()
    for var, idx in re.findall(r'OpAccessChain %\S+ (%\S+) (%\S+)', body):
        member = consts.get(idx)
        if member is None:
            continue
        if var in cam:
            cam_members.add(member)
        if var in obj:
            obj_members.add(member)

    return dict(attrs=attrs, vidx=vidx, iidx=iidx,
                cam_members=cam_members, obj_members=obj_members,
                has_obj_block=bool(obj))


mods = {}
for name in sorted(os.listdir(DUMP)):
    if not name.endswith('.spv'):
        continue
    a = analyse(os.path.join(DUMP, name))
    if a:
        mods[name[:-4]] = a
print(f'{len(mods)} of {len(os.listdir(DUMP))} dumped modules have a vertex '
      'stage\n')


# --- the candidate ------------------------------------------------------
def category(a):
    """World / Canvas / Fullscreen, the three-way split under test."""
    if 0 in a['obj_members'] or a['cam_members'] & {0, 1, 7, 8}:
        return 'World'
    return 'Canvas' if a['attrs'] else 'Fullscreen'


print('             ', ''.join(f'{c:>12s}' for c in
                                ('World', 'Canvas', 'Fullscreen', 'unknown')))
for name, rps in GROUPS:
    bound = set().union(*[pass_mods[r] for r in rps]) if rps else set()
    counts = collections.Counter(category(mods[m]) for m in bound if m in mods)
    counts['unknown'] = len(bound - set(mods))
    print(f'{name:14s}', ''.join(f'{counts[c]:12d}' for c in
                                 ('World', 'Canvas', 'Fullscreen', 'unknown')))

print('\nAll dumped modules:',
      dict(collections.Counter(category(a) for a in mods.values())))

# The two answers that decide whether the predicate is usable at all.
print('\n--- what the LDR passes actually contain ---')
for rp in ldr_rps:
    row = []
    for m in sorted(pass_mods[rp]):
        a = mods.get(m)
        row.append(f'{m}={category(a) if a else "no-vs"}'
                   + (f'{a["attrs"]}' if a and a['attrs'] else ''))
    print(f'  rp #{rp:<3d} [{shape[rp][1]}] {shape[rp][2]:9s} '
          f'{verdict[rp][1]:18s} {" ".join(row) if row else "(no draws)"}')
