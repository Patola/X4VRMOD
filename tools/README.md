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

## block_offset_census.py

**What else lives at byte N of a uniform block — the test to run before writing
one.** `uniform_block_scan.py` aggregates by member *index*, which is right for
the camera block, where one layout covers every module. Sets 2 and 4 are
generated per shader family, so the same index — and the same byte — is a
different variable in different modules.

    tools/block_offset_census.py 1 0              # layouts: camera == 1
    tools/block_offset_census.py 4 0 --at 512     # who ELSE is at that byte
    tools/block_offset_census.py 4 0 --name fsr3g
    tools/block_offset_census.py 4 0 --layout mod-0012

A host write addresses a byte and X4 binds many shader families through one
descriptor slot, so the question a write must survive is not "do the modules I
care about agree on the offset" — four permutations of one shader always will —
but **what every other module keeps there**. Byte 512 of set 4 is
`U_fsr3g_useuitexture` in 4 modules and `U_patternstr`/`U_pattern_color`/six
more in 119; byte 1520 of set 1 is `B_csmdebugcolor` in all 370. That contrast
is the tool's own control, and the second case is the one `X4VR_CSM_DEBUG`
writes live.

Counts over `/tmp/x4vr-shaders` are **405 X4 modules plus 4 of ours** — the
cursor shim is in the dumps, and it declares `g_tCursor` at set 1 binding 0.

## name_module.py

**What a dumped module IS, not just which dump it is.** The FNV-1a hash join
(`module_map.py`) answers "which mod-NNNN"; this answers "which X4 shader".

    tools/name_module.py mod-0012 mod-0022
    tools/name_module.py                     # everything it can name
    tools/name_module.py --ambiguous         # the ones it cannot pin

The DYNAMIC block's tail is generated from the shader's own `<properties>` list
in `shadergl/ogl/*.xml`, so the member names are the property names with a type
prefix — a fingerprint from a dump back to a source file. Validated on a module
identified independently: `mod-0012` matches `copy_gamma.xml` EXACT and nothing
else, and the layer already names it in the log.

**38 of 409 named uniquely, 24 ambiguous, 308 unmatched.** The named ones are
the post-process, present and UI shaders — small property lists, no
macro-injected members — which is the class every question about the composite
and the HUD lands in. UNMATCHED means the fingerprint cannot pin it, never "not
an X4 shader". Ambiguity is reported, not hidden: `visual_transition` and
`xu_simple_hdr_out` have identical property lists, and the tie breaks on the
vertex stage.

This is the tool that settled `#103` — the "fullscreen quad bringing the scene
in" is `visual_transition`, a UI fade, so the target holds UI alone.

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

## hud_frame.py

Is a HUD element screen-space or world geometry? Read it off an SBS frame —
no run, no layer instrument. The per-eye shear displaces by inverse depth, so
a draw at `w = 1` is immune to it and disparity within one frame says which
frame an element was drawn in.

    tools/hud_frame.py /tmp/x4-162b.jpg --region reticle=672,660,64
    tools/hud_frame.py /tmp/x4-162b.jpg --anaglyph radar=1080,960,220 --out /tmp/a

**Three channels, always.** A thin cyan overlay lying on textured geometry puts
two things at two depths in one window, and a single correlation returns
whichever has more contrast, wearing the other one's name. `luma` is both,
`hud` is `(G+B)/2 − R` (X4's green/cyan HUD, minus its warm cockpit and white
stars), `grey` is the background with the overlay taken out. Where they
disagree, the disagreement is the finding — that is how #55 found take 162's
radar measurement had read the panel behind the radar.

**The range: 0 px has two causes.** Screen-space reads 0, and so does world
geometry far enough away — the starfield reads 0 in every frame. So this
separates *screen-space or infinity* from *near geometry*, and does **not**
separate screen-locked from world-at-infinity. A canted take cannot either,
because #40 makes the canvas agree with infinity at that distance; only a
finite `X4VR_CANVAS_M` can.

`PEGGED` means the peak sat on the search bound — the first version capped at
−40 px and returned −40 for every near-cockpit window. `--anaglyph` writes the
check a person can make: left eye into red, right into green+blue, so flat
content is sharp and parallax is fringed.

## guard_band.py — the shadow column (#56)

The last column prices what a widened `<fov>` does to shadow resolution, and it
is **conditional**: X4's shadow atlas is a fixed 10 × 2048×2048 D16 at every
field from 1.0 to 1.5 and at both render extents, so IF cascades are fitted to
the camera frustum a wider field spreads the same texels over a wider world.
The ratio is the tangent ratio — the same one that sets `sym px/eye` — because
a frustum-fitted cascade spans `2·z·tan(half)`. The difference is that render
pixels can be bought back and the atlas cannot, so **#35's off-axis saving does
not apply to it**. Whether the fit is frustum-based is unmeasured; the layer's
`proj SHADOW` line reports the cascade extent and #56 needs it at two fields.

## headlook_verify.py

Did X4 actually apply #33's head rotation? Measured from two screenshots — one
with the rotation off, one with it on.

    tools/headlook_verify.py --self-check
    tools/headlook_verify.py --verify /tmp/x4-186a.jpg /tmp/x4-186b.jpg \
        --sx 0.70024 --yaw 10

**`register_affine.py` cannot score this and says so itself**: "they would NOT
be sufficient for a rotation or a keystone, and this tool would report a
confident wrong answer for either". #35's map is separable; a rotation is a
homography. This is its sibling for that shape.

The useful property is that **the NDC map is depth-independent** — every pixel
moves by a rule that depends only on where it is on screen and on `sx`. So two
screenshots are sufficient, with no depth map, and the same warp must fit near
and far geometry alike. A fit that works on the cockpit and fails on the
background is evidence *against* the rotation.

`--self-check` pins the map against the closed form (2e-16), pins the NDC round
trip (4e-16), and requires the right angle to separate from a wrong one by a
wide margin. That last criterion is relative on purpose: with pitch, content
leaves the frame and cannot come back, so the image round trip is 0.77 where
pure yaw is 0.999 — and lowering a floor until both pass would be fitting the
test to the result.

It also found a real bug in itself: `warp` first built the inverse rotation by
negating the angles, which is right for one axis and wrong for a composition,
because `Ry(−y)·Rx(−p)` is not the inverse of `Ry(y)·Rx(p)`. The inverse is the
transpose, and `ndc_map` now takes a matrix so it cannot be expressed any other
way.

## xref.py / xdisp.py

Static reading of X4's binary (bytes only, nothing written).

    tools/xref.py 0x4533b00 '"csmthresholds"' 0x2094fc0   # who references / calls it
    tools/xdisp.py 0xcc8 [--mnemonic movss]                # every [reg + 0xcc8]

Byte-pattern scans whose hits are attributed to an instruction by majority
vote over 32 linear decodes started 32–64 bytes earlier. Decoding backwards
from a displacement reads `f3 0f 11 83 ...` (movss) as `11 83 ...` (adc). The
vote was validated on the known camera+0x874 sites. Found #70's cascade fit
(0x2094fc0).
