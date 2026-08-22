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
