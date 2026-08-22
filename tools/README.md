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
