#!/usr/bin/env python3
"""Design a widened World predicate offline, against every dumped module.

Ground truth comes from the run, not from guessing: which modules X4 actually
bound to its shadow passes, its lighting passes, its G-buffer fill and its
present passes. A predicate is only acceptable if it catches the light volumes
and misses every shadow-pass module.
"""
import re, os, subprocess, sys, collections

LOG = '/tmp/x4vr-take63.log'
DUMP = '/tmp/x4vr-shaders-take61'

# --- runtime ground truth ------------------------------------------------
pass_mods = collections.defaultdict(set)
sig = {}
for L in open(LOG, encoding='utf-8', errors='replace'):
    m = re.search(r'mv final: rp #(\d+) <- frag module #(\d+)', L)
    if m:
        pass_mods[int(m.group(1))].add(int(m.group(2)))
    m = re.search(r'layer   rp #(\d+)\.\d+: (\d+) colour \[([^\]]*)\] ?(no-depth|depth \d+)', L)
    if m:
        sig[int(m.group(1))] = (int(m.group(2)), m.group(4))

# A shadow pass renders depth only: zero colour attachments.
shadow_rps = [rp for rp, (nc, _) in sig.items() if nc == 0]
light_rps = [23, 24, 25]
gbuf_rps = [16, 17]
present_rps = [0, 1, 7]

shadow_mods = set().union(*[pass_mods[r] for r in shadow_rps]) if shadow_rps else set()
light_mods = set().union(*[pass_mods[r] for r in light_rps])
gbuf_mods = set().union(*[pass_mods[r] for r in gbuf_rps])
present_mods = set().union(*[pass_mods[r] for r in present_rps])
print(f'shadow passes {sorted(shadow_rps)} -> {len(shadow_mods)} modules')
print(f'lighting passes {light_rps} -> {len(light_mods)} modules')
print(f'g-buffer passes {gbuf_rps} -> {len(gbuf_mods)} modules')
print(f'present passes {present_rps} -> {len(present_mods)} modules')

# --- per-module static analysis -----------------------------------------
def analyse(path):
    t = subprocess.run(['spirv-dis', path], capture_output=True, text=True).stdout
    if not t:
        return None
    dset, dbind = {}, {}
    for v, n in re.findall(r'OpDecorate (%\S+) DescriptorSet (\d+)', t):
        dset[v] = int(n)
    for v, n in re.findall(r'OpDecorate (%\S+) Binding (\d+)', t):
        dbind[v] = int(n)
    consts = {}
    for c, val in re.findall(r'(%\S+) = OpConstant %\S+ (-?\d+)\b', t):
        consts[c] = int(val)

    cam = {v for v in dset if dset[v] == 1 and dbind.get(v) == 0}
    world = {v for v in dset if dset[v] == 3 and dbind.get(v) == 0}

    # The vertex entry point's function only. X4 ships combined modules, so a
    # whole-module scan answers a different question -- the exact trap recorded
    # for classify() at take 60.
    ep = re.search(r'OpEntryPoint Vertex (%\S+)', t)
    if not ep:
        return dict(vertex=False, cam={}, world={}, has3=bool(world))
    fn = ep.group(1)
    body = re.search(re.escape(fn) + r' = OpFunction.*?OpFunctionEnd', t, re.S)
    body = body.group(0) if body else ''

    cam_members, world_members = set(), set()
    for r, base, idx in re.findall(r'= OpAccessChain %\S+ (%\S+) (%\S+)(?: (%\S+))?', body):
        pass
    for mm in re.finditer(r'= OpAccessChain %\S+ (%\S+)((?: %\S+)+)', body):
        base = mm.group(1)
        idxs = mm.group(2).split()
        if not idxs:
            continue
        first = consts.get(idxs[0])
        if first is None:
            continue
        if base in cam:
            cam_members.add(first)
        elif base in world:
            world_members.add(first)
    return dict(vertex=True, cam=cam_members, world=world_members, has3=bool(world))

info = {}
for f in sorted(os.listdir(DUMP)):
    if not f.endswith('.spv'):
        continue
    s = int(f[4:8])
    a = analyse(os.path.join(DUMP, f))
    if a:
        info[s] = a
print(f'analysed {len(info)} modules\n')

# --- candidate predicates ------------------------------------------------
def cur(a):   # today's rule
    return a['vertex'] and 0 in a['world']

def wide(a):  # widened: also camera view/projection driven geometry
    if cur(a):
        return True
    if not a['vertex'] or a['has3']:
        return False
    # M_view(0), M_projection(1), M_viewprojection(7), M_viewinverse(8)
    return bool(a['cam'] & {0, 1, 7, 8})

for name, pred in (('current', cur), ('widened', wide)):
    W = {s for s, a in info.items() if pred(a)}
    print(f'--- {name}: {len(W)} World of {len(info)}')
    for label, group in (('shadow', shadow_mods), ('lighting', light_mods),
                         ('g-buffer', gbuf_mods), ('present', present_mods)):
        g = {s for s in group if s in info}
        if g:
            print(f'    {label:9s} {len(g & W):3d}/{len(g):3d} World'
                  + ('   <-- MUST BE 0' if label == 'shadow' else ''))
    print(f'    light volumes 207/209: '
          + ', '.join(f'{s}={"World" if s in W else "UI"}' for s in (207, 209)))

# --- safety: what does widening newly catch, and is any of it fullscreen? ---
W0 = {s for s, a in info.items() if cur(a)}
W1 = {s for s, a in info.items() if wide(a)}
new = sorted(W1 - W0)
print(f'\nnewly World under widening: {len(new)} -> {new}')
fs = []
for s in new:
    t = subprocess.run(['spirv-dis', f'{DUMP}/mod-{s:04d}.spv'],
                       capture_output=True, text=True).stdout
    ep = re.search(r'OpEntryPoint Vertex (%\S+)([^\n]*)', t)
    iface = ep.group(2) if ep else ''
    # A fullscreen triangle builds its position from gl_VertexIndex and takes
    # no vertex attributes. Shearing one would move the UI, which is the
    # take 33 logo regression.
    if 'gl_VertexIndex' in iface and 'SPECIAL_VERTEXLOCATION_POSITION' not in t:
        fs.append(s)
print(f'  of those, fullscreen/no-attribute (UI risk): {len(fs)} -> {fs}')
print(f'  bound to shadow passes: {sorted(set(new) & shadow_mods)}')
print(f'  bound to present passes: {sorted(set(new) & present_mods)}')
print(f'  bound to lighting passes: {sorted(set(new) & light_mods)}')
