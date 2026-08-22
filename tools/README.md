# tools

Placeholder — see ../DESIGN.md for this component's role.

## spv_entry_scan.py

Which **entry point** of a dumped SPIR-V module reads which camera-block member,
and which touch a 3D image. X4's modules carry a vertex AND a fragment
`OpEntryPoint` in one module, so a whole-module scan attributes every access to
whichever entry point it happens to read — that produced three wrong conclusions
in task #49, including "no fragment shader reads `M_projection`" when ten do.
Walks the call graph from each entry point instead.

    tools/spv_entry_scan.py                 # summary over /tmp/x4vr-shaders
    tools/spv_entry_scan.py mod-0365 ...    # per-module detail

Dump indices are creation order and do **not** match a log's `module #N`. Join
by content.

## eye_component.py

Characterise something one eye has and the other has not. Within one dump the
eyes are the same instant, so at a distant object parallax is zero and the eye
difference **is** the component — no temporal change or exposure drift to argue
about.

    tools/eye_component.py --prefix=/tmp/x4vr-t183 --frames=10,16,12,17
    tools/eye_component.py <layer0.png> <layer1.png>

Answers the question that has to come first: **absent or displaced?** A mono
screen-space buffer sampled at each eye's own UV leaves the component drawn
~1027 px away; a gate leaves it nowhere. Task #49 spent several takes on a
screen-space-buffer theory that this retired in one command.

Two things it refuses to do, each of which cost a conclusion in #49: it does
not report angular structure only at the radius of peak amplitude (that called
a plainly diagonal plume "isotropic"), and it does not deliver an
absent/displaced verdict when the eyes agree — a control reads as a control,
not as a null result. **Always pass a negative control frame.**

## uniform_block_scan.py

Every member of every X4 uniform block, named from the module's own
`OpMemberName`, counted per shader stage over the dumps.

    tools/uniform_block_scan.py           # all blocks
    tools/uniform_block_scan.py 1 0       # just the camera block

Two rules it exists to enforce. **Never count members by position** —
`V_light_direction_view[3]` is one member, so counting `uniform` lines in
`common.glsl` is off by however many arrays came before. **Never scan a module
whole** — attribution goes through `spv_entry_scan.analyse`, for the reason
that tool exists.

The counts are per entry point, which is the number that decides whether a
vertex or a fragment patch has to carry a correction. **A zero means "nothing
in the dumps we have", never "X4 does not use it"**: the injector forces
`ssao=0`, `ssr=false`, `antialiasing=none`, so `M_projection_uj`, `M_jitter`
and `M_prevworldviewprojection` read zero because we emptied them.

## headlook_clip.py

The clip-space matrix for #33's head rotation, derived and self-checked
offline before it costs a take.

    tools/headlook_clip.py --self-check
    tools/headlook_clip.py --yaw 10 --sx 0.75405

Answers one question: can the rotation be **baked**, the way
`patch_vertex_clip` bakes its `k[16]`? No. `M = P·R·P⁻¹` is exact at every
depth (checked to 1e-11 over six measured cameras), but six of its sixteen
entries carry `sx` or `sy` — and they are the entries that do the rotating.
A matrix baked at the menu camera is 72 px wrong on a 1408 px eye at 10° of
yaw, and at the wide-field camera the guard band wants it drives `w` through
zero. So the rotation rides the live-sx patches, and `patch_vertex_clip` —
which carries the modules that must stay screen-locked anyway — does not get
it. Emits `k[16]` column-major, matching `make_canvas_k`.
