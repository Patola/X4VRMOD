# Frame analysis (Phase 2, in progress)

Renderdoc findings on X4 9.0 (RADV, RX 7900 XTX). This is the empirical
basis for the double-render stereo mechanism; keep it updated as we learn
more.

---

## Where this stands (read this first)

**Current state: `stage2-stereo-shading-correct` (take 83).** Both eyes render
from a single draw, both are correctly lit, and the parallax is real. This is
the first state in the project where the stereo image is not visibly defective.

| Tag | What it holds |
|---|---|
| `stage1-multiview-verified` | Both array layers byte-identical (readback) |
| `stage1-complete` | …and a frame built entirely from layer 1 is correct end to end |
| `stage2-per-eye-k` | Per-eye `K` via `gl_ViewIndex` — one draw, two different eyes |
| `stage2-tonemap-masked` | The SRGB resolve replicates into both layers of `#103` (knob is misnamed) |
| `stage2-frag-patch` | A patched sampler reads layer N in view N — proven offline, **not usable on X4** |
| `stage2-duplicate-restored` | Both eyes have a picture at an acceptable framerate; right eye a bit-exact copy |
| **`stage2-stereo-shading-correct`** | **Both eyes correctly lit, parallax real — take 83** |

Sections below are **chronological and include the wrong turns on purpose**.
Several of them state conclusions that later sections refute. Where a claim was
overturned the refutation is written next to it rather than replacing it, so
read to the end of a thread before acting on its middle.

### The per-eye shading defect, and how it was actually fixed

Takes 56–83 chased one symptom: **one eye visibly darker than the other on
large surfaces**, structure identical, which killed the sense of depth. It is
worth stating the resolution up front because thirteen takes of this document
argue toward the wrong answer first.

**The bug.** X4's deferred lighting reconstructs surface position from
`gl_FragCoord` and the depth buffer. `mod-0180`, the sun light with cascaded
shadows, reconstructs it **twice from the same input**:

    A = camera member 2 (M_invprojection)     -> the view vector. Specular.
    B = camera member 4 (M_invprojection_uj)  -> x5 CSM matrices -> S_sampler2DShadow

The eye shear moves `gl_Position` only, so the depth buffer belongs to the
sheared eye while both matrices are the centre camera's. `patch_fragment_invproj_eye`
corrected **member 2 only** — the view vector, which is nearly invisible — and
left the shadow lookup reading the centre frame. A 3.2 cm position error at the
shadow lookup is 36 px at 0.83 m, one full disparity, so a surface was
**shadowed in one eye and lit in the other**.

**The fix** (`cd0df98`): apply the same `T(d)·M` correction to member 4.
`X4VR_PROJ_INVPROJ` defaults **on** from take 83 and covers both members.

**Why it took thirteen takes.** The knob existed from take 67 and always
returned a null, and that null was read as refuting the *mechanism*. Across 385
fragment modules, 236 load member 2 and **2** load member 4 — so every coverage
metric ("100 of 138 eligible", "no coverage hole") looked healthy while the two
modules that mattered went untouched. The tell was there: the knob moved the
measurement by **0.04%**. Exactly zero means "never ran"; large means "ran and
mattered"; a *tiny non-zero* means **ran on the wrong thing**, and that is the
most misleading of the three. The two members are now logged as separate
counters for exactly this reason — a combined total reads a healthy 236 in
precisely the broken state.

    invproj final: per-eye M_invprojection — 224 modules corrected
    invproj final: per-eye M_invprojection_uj — 2 modules corrected   <- this one

**Measured, not described** (`#57`, tile ratio and the worst blob):

| | tile p99 | flagged, confidently matched | blob median | blob p90 | crop NCC |
|---|---|---|---|---|---|
| take 82 (before) | 4.078 | 14.4% | 1.33 | 28.91 | 0.7653 |
| take 83 (after)  | **1.536** | **1.8%** | **1.00** | **1.85** | **0.9055** |

The last column is the independent check: the crop's own alignment improved with
nothing about the measurement changed, which a fix that merely rescaled one eye
could not do. Residual is 1.8% against the `IPD=0` control's 0.0%; some of that
is genuine one-eye occlusion, which is correct stereo.

**Two measurement corrections that came out of this**, both of which invalidate
earlier numbers in this document:

1. `write_ppm` applies Reinhard **and** gamma before writing, so every
   brightness ratio read off a dump — 1.686, 1.530, the tile maps — is in
   compressed units. `tools/shading_model.py` inverts it. In linear terms the
   defect was ~3.2x, not 1.7x.
2. Alignment must be locked before brightness is compared, and the tile must
   span a brightness range or gain and offset are not separable. Both rules were
   stated in this document and then broken.

### Renamed: `X4VR_CLIP_K_UI` -> `X4VR_CLIP_K_NONWORLD`

`classify()` splits vertex modules into `World` -- geometry positioned by a
per-object matrix (set 3), or by the camera block under `wide_camera` -- and
everything else. "Everything else" was called `UI`, and the matrix applied to it
`K_ui`. On X4 that set is 54 modules of 350 and includes every fullscreen
triangle and every procedural vertex shader; almost none of it is UI. The name
described its most visible member, and reading it as "the matrix for the HUD"
is what let take 82 draw a conclusion broader than the knob could support.

    K_ui                  -> K_nonworld              (and Kind::UI -> Kind::NonWorld)
    X4VR_CLIP_K_UI        -> X4VR_CLIP_K_NONWORLD
    X4VR_CLIP_K_UI_RIGHT  -> X4VR_CLIP_K_NONWORLD_RIGHT
    X4VR_CLIP_SHIFT_UI    -> X4VR_CLIP_SHIFT_NONWORLD

**The old spellings are gone**, with no alias and no warning: nothing outside
this repo consumes them, and the code carries no trace of a name we got wrong.

**So a pre-rename command line pasted into a current build is silently
ignored** -- the variable is set, nothing reads it, and the run looks valid
while doing something else. That is a real edge and this is where it is
recorded, because the code no longer says it anywhere.

**To reproduce a take from before this rename, check out that commit**, where
the old name works natively. That is the project's own rule regardless: a
known-good state is code *and* knobs, so a historical command line is only
meaningful against the build it was run on. Every command line recorded below
predates the rename and is left exactly as it was run.

The per-module summary line changed with it, so a **new** log reads
`[world=296 nonworld=54 ...]` where the takes below quote `world=296 ui=54`.

**Setting it still often does nothing, and that is by design.** Two independent
gates decide whether `K` reaches a draw: `classify()` picks *which matrix* a
module is patched with, and `needs_original()` decides whether the patched
module is bound at all. An unsheared pass binds the *unpatched* module whatever
it was patched with. So `K_nonworld` reaches only a draw whose module is
`NonWorld` **and** whose pass is sheared. See the take-82 correction near the
end of this document.

### The frame graph, as far as it is known

Serials are assigned in creation order and **held identical across runs 47 and
take nineteen**, which was an inference and is now a measurement. Images come
in a repeating block — the same target set allocated several times (`#8–#23`,
`#46–#59`, `#92–#107`, …) — and the blocks align on their one unmistakable
member, the `mips=2 fmt=13` mask at offset **+9**.

| Serial | Format | Role | Stereo? |
|---|---|---|---|
| `#92`, `#95`, `#96`, `#97` | 97 `RGBA16F` | HDR colour / G-buffer | **per-eye** |
| `#93` | 126 | depth-stencil | per-eye |
| `#98`, `#99` | 83 `R16G16_SFLOAT` | G-buffer (normals/motion) | **per-eye** |
| `#100` | 50 `B8G8R8A8_SRGB` | LDR | not seen written |
| `#101` | 13 `R8_UINT`, mips=2 | classification mask, rp #25 + rp #27 | **per-eye** |
| `#102` | 9 `R8_UNORM` | mask | **per-eye** |
| **`#103`** | **50 `B8G8R8A8_SRGB`** | **LDR composition (scene + UI), rp #40 + rp #52** | replicates, **mono content** |
| `#104`, `#105` | 97 `RGBA16F` @352² | bloom ping-pong | replicates, mono |
| `#122`, `#123`, `#124` | 76 `R16_SFLOAT` @1408² | rp #61 / rp #63; unidentified | replicates, mono |
| — | 44 `B8G8R8A8_UNORM` | rp #0/#1/#4/#7/#10/#14/#17 → eye image | **not even masked** |

The last row is the remaining frontier. Everything that "replicates but is
mono" is mono for one reason — it is *sampled* through the bindless table — so
all of it is fixed by the same index offset. The format-44 chain needs masking
first, and that is where `SbsCompositor`'s two-layer eye image finally matters.

### X4 is bindless, and that decides the mechanism

**Read "Take twenty" before planning anything about sampling.** The measured
fact: every shader that draws into `#103` samples one descriptor —
`set 0 binding 7, count 53306`. X4 keeps every texture in the game in a single
bindless table, indexed by `S_diffuse_idx`, a `uint` in
`BLOCK_BUFFER_BINDING_SLOT_DYNAMIC` (set 4 binding 0).

Consequences, all of them load-bearing:

* **Promoting a texture's type is not available.** `Arrayed 0 → 1` on that
  table promotes all 53,306 entries; ~20 images are doubled. The patch already
  refuses (the pointee is an `OpTypeArray`), and a test now asserts it.
* **`patch_fragment_view_layer` is proof-of-mechanism, not the mechanism.** It
  is tagged `stage2-frag-patch` and its offline gate is real — a sample can
  follow `gl_ViewIndex` on this driver, measured before and after. Do not wire
  it into X4.
* **`#103` is not a tonemap output.** It is the LDR composition target: a
  generic textured-quad draw brings the scene in, UI geometry is drawn on top,
  and six pipelines share rp #40. `X4VR_MASK_TONEMAP` is a misnomer kept for
  continuity with the tagged runs.

### Next step: the bindless index offset (task #13)

Because the index is an integer in a uniform, per-view sampling becomes integer
arithmetic and nothing else changes type:

    element = S_diffuse_idx + gl_ViewIndex * OFFSET

with the layer writing, at `slot + OFFSET`, an ordinary 2D view of **layer 1**
of whatever image sits in `slot`. `sampler2D` stays `sampler2D`. The
shader/view pairing problem that the array approach had — a `sampler2DArray`
requiring a matching `2D_ARRAY` view, validation error if they disagree —
disappears entirely.

Best property: **the slot holding `#95` never has to be identified.** Mirror
every descriptor write into the twin region, substituting a layer-1 view for
per-eye images and duplicating the descriptor otherwise. Undoubled textures
then read identically in both views, so the patch needs no per-shader
targeting — and a shadow map's twin *is* the same shadow map, which defuses
that hazard instead of dodging it.

Headroom is not a problem: RADV allows 8,388,606 sampled-image descriptors per
set against X4's 53,306.

**What the survey settled about this plan** (details under Q1/Q2 above):

* The twin region **needs no allocation**. X4 declares and allocates all 53,306
  with `PARTIALLY_BOUND`, uses a dense 10,980-slot prefix, and leaves 42,326
  descriptors allocated-but-unwritten. `OFFSET = 26,653` splits the array in
  half with 2.4× headroom over the observed high-water mark.
* The mirror must be **per-write, not one-shot** — but for a reason the
  prediction got wrong. Slots do not appear late; their *contents* are rewritten
  ~2× per frame.
* The mirror must cover **bindings 5 and 7 both**; they hold identical
  populations.

**The open question is now cost, not feasibility.** X4 issues ~21,900
image-descriptor writes per frame. Mirroring each one doubles that. Two shapes
to weigh before writing code, and this should be decided by measurement:

1. **Mirror every write** in `vkUpdateDescriptorSets` — simple, always current,
   and it adds ~21,900 descriptor writes per frame of CPU work.
2. **Bulk-copy the prefix** with `VkCopyDescriptorSet` (one copy of
   0..10,979 → 26,653.., `descriptorCount` in one call) — far cheaper per frame,
   but it is a snapshot, so anything X4 rewrites after the copy is a frame stale
   in the second view. Since the table is rewritten twice per frame, "stale" is
   not hypothetical and needs bounding before this is chosen.

Neither is obviously right. Given "performance is king", option 1 gets built
first because it is correct by construction, and option 2 becomes an
optimisation with a measured baseline to beat.

### Task #13 is two runs, on purpose

The mirror and the shader patch are independent, and building both before
measuring either is how a broken frame becomes un-diagnosable — the mistake this
project has already paid four runs for. So:

**Step A — mirror only, with nothing reading it.** The layer duplicates every
image-descriptor write into `slot + OFFSET`, substituting a layer-1 view where
the image is per-eye and copying the descriptor verbatim otherwise. No shader is
patched, so nothing indexes the twin region and **the frame must not change at
all**. That makes it a clean isolation: any frame-time delta is the mirror's
cost and nothing else, and any validation error is the mirror's fault and
nothing else. It also proves the twin region is writable before a shader depends
on it, which is currently an inference from `PARTIALLY_BOUND` plus a
fully-allocated pool, not a measurement.

**Step B — the index offset, on the narrowest useful set of shaders.**
`element = S_diffuse_idx + gl_ViewIndex * OFFSET`. Gate: `#103` flips from
all-IDENTICAL to all-DIFFER.

Two notes on step B that step A does not need to wait for:

* **The new patch is much simpler than the abandoned one.** No retyping, no
  coordinate extension, no following a retyped value through function bodies
  with a taint set. It declares the `ViewIndex` builtin and rewrites one index
  operand into an `OpIAdd`/`OpIMul` pair. The hard parts of
  `patch_fragment_view_layer` do not recur.
* **Patch few shaders, not all 333.** Every patched shader can index any slot, so
  the twin must be complete regardless — but the *risk* scales with how many
  modules are rewritten, and the gate only needs the fragment modules drawing
  into `#103`. The `srgb-resolve rp #40: frag module #N` join already names them.

### What the next run (step A) is trying to find out

Predictions committed **before** the run, as usual. This one is mostly a cost
measurement, which means the prediction that matters is a number.

**P1 — Is the twin region actually writable?** Currently an inference: the count
is fixed, so allocation consumes all 53,306, so the pool backs them, so writing
above the high-water mark is legal.

> Predicted: **yes**, no validation error and no `VK_ERROR_OUT_OF_POOL_MEMORY`.
> If this is wrong the whole mechanism dies and options reopen at the
> replay-the-pass level.

**P2 — Does the frame change?** Nothing reads the twin region, so nothing should.

> Predicted: probe verdicts **identical to take twenty-one** — G-buffer DIFFER,
> `#103` IDENTICAL. Any change here means the mirror is writing somewhere it
> should not, and the most likely somewhere is a slot X4 is still using, i.e.
> `OFFSET` overlapping the live prefix.

**P3 — What does mirroring cost?** ~21,900 extra descriptor writes per frame.

> Predicted: **under 1.5 ms added to the median frame time.** A descriptor write
> on RADV for an `UPDATE_AFTER_BIND` binding is close to a small memcpy, so
> 21,900 of them should be well under a millisecond of CPU; the slack is for
> building the second `VkWriteDescriptorSet` array. If it lands above ~3 ms,
> option 2 (bulk `VkCopyDescriptorSet`) stops being an optimisation and becomes
> the design.

**P4 — How many sets come from the bindless layout?** The mirror has to cover
each of them, and the survey never counted.

> Predicted: **a small number, under 8** — one per frame in flight, or one
> outright, since the table is global and `UPDATE_AFTER_BIND` exists precisely so
> a single set can be rewritten while in use.

**P5 — Does the used prefix stay clear of `OFFSET` over a long session?** 10,980
after thirteen minutes.

> Predicted: **yes, comfortably** — the prefix grew 358 slots in thirteen
> minutes and `OFFSET` is 26,653 away. A guard logs it if the prefix ever
> crosses, because silent overlap would corrupt X4's own textures rather than
> merely break stereo.

P1 and P3 are the load-bearing ones: P1 can kill the mechanism, P3 can change
it. P2 is the control that says the instrument and the mirror are not lying to
each other. P4 and P5 are cheap and prevent a class of silent failure.

### What that run found — Q1–Q4, scored

Ran 2026-07-30, `X4VR_BINDLESS_SURVEY=1`, 9,934 frames of play. **Two of four
predictions right**, and the two that were wrong were wrong in ways that matter
more than the two that were right. Predictions are left verbatim with the
verdict underneath, so the record shows what was believed and not only what
turned out to be true. Numbers and narrative: *Take twenty-one*.

**Q1 — Does X4 allocate all 53,306 descriptors, or declare the array large and
bind fewer?** This decides the size of the job. If the layout really carries
53,306, the layer only has to *mirror writes* into a twin region. If X4 uses
`VARIABLE_DESCRIPTOR_COUNT` and binds a few thousand, the layer must widen the
descriptor set layout **and** the pool — a much bigger intervention, touching
object creation rather than data.

> Predicted: **variable count**, bound well below 53,306. 53,306 is suspiciously
> exact for a compile-time bound and bindless engines normally declare the
> maximum and size the set to the level's content.

**WRONG, and wrong in the good direction.** One layout, seven bindings with
`count > 1`, and `flags = 0x7` on every large one:
`UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING | PARTIALLY_BOUND`. Bit 0x8,
`VARIABLE_DESCRIPTOR_COUNT`, is **not set anywhere in the run**, and no
`VkDescriptorSetVariableDescriptorCountAllocateInfo` was ever passed. X4
declares the full 53,306 and allocates the full 53,306.

    binding 2  INPUT_ATTACHMENT      58   partially-bound, update-after-bind
    binding 3  INPUT_ATTACHMENT      58   partially-bound, update-after-bind
    binding 4  SAMPLER               18   no flags
    binding 5  SAMPLED_IMAGE     53,306   partially-bound, update-after-bind
    binding 6  STORAGE_IMAGE     53,306   partially-bound, update-after-bind
    binding 7  SAMPLED_IMAGE     53,306   partially-bound, update-after-bind
    binding 8  STORAGE_IMAGE     53,306   partially-bound, update-after-bind

**The consequence deletes the biggest predicted job.** Because the count is
fixed and no variable count is used, `vkAllocateDescriptorSets` consumes all
53,306 from the pool — so the pool already backs every one of them, and
`PARTIALLY_BOUND` makes an unwritten descriptor legal as long as no shader
reads it. **The twin region already exists.** No widening the layout, no
resizing the pool, no intercepting object creation. Writing above X4's
high-water mark is just writing.

Independent cross-check that the layout reader is honest: binding 4 reports
`SAMPLER × 18`, and the SPIR-V survey independently found an 18-entry `sampler`
array at set 0 binding 4. Two instruments, different sources, same number.

**Q2 — How many slots does X4 actually write, and how?** Decides where a twin
region can live and whether an offset can be a compile-time constant.

> Predicted: a few thousand distinct slots, written **incrementally** as content
> streams in, not as one bulk update at load. Expect writes to keep arriving
> during play, which means the mirroring cannot be a one-shot pass at startup.

**Right about the conclusion, wrong about the reason — and the real reason is
worse.** The slot *set* is nearly static: 10,622 slots written before the first
present, 10,980 by the end, so 96.7% of it existed before a single frame was
shown and thirteen minutes of play added 358. "Streams in incrementally" is not
what happens.

What is dynamic is the *contents*:

    217,705,579 image-descriptor writes     (217,684,326 after the first present)
    /  9,934 frames   =  ~21,900 per frame
    /     10,980 used slots  ≈  two full rewrites of the table every frame

So mirroring still cannot be a one-shot pass at startup — but not because slots
appear late. It is because **every slot's contents are rewritten about twice per
frame**, which kills any plan built on identifying a slot once and mirroring it,
and sets the mirror's cost by X4's descriptor traffic rather than by table size.

Shape of the used region, which is what the offset needs:

    bindings 5 and 7:  10,980 distinct slots, range 0..10,979  — dense, no holes
    of 53,306 declared:  20.6% used, 42,326 free
    191 slots hold a per-eye (doubled) image: #101, #103, #600, #607

Bindings 5 and 7 carry the **same population** — same count, same range, same
191 per-eye slots, same image serials. X4 maintains two identical sampled-image
tables, so the mirror has to cover both.

Dense, and one-fifth full, means the offset can be a compile-time constant with
room to spare. `OFFSET = 26,653` (half of 53,306) leaves 2.4× headroom over the
observed high-water mark, and the layer can log if the prefix ever crosses it.

**Q3 — Does any shader index the table non-uniformly?** A non-uniform index
needs `NonUniformEXT` on the access chain, and the patch must preserve it.
`gl_ViewIndex` is draw-uniform, so it adds no non-uniformity of its own.

> Predicted: **no** — the indices seen so far come from uniform blocks, which is
> uniform by construction. If any shader computes an index from an interpolated
> input this flips, and the patch has to carry the decoration through.

**RIGHT, and settled without the run.** `NonUniform` appears in **0 of 409**
dumped modules. Indices are draw-uniform; the patch has no decoration to
preserve.

**Q4 — Is the same table used for storage images or subpass inputs anywhere?**
Those would need excluding: a storage image has no sampler, and a subpass input
is already view-indexed.

> Predicted: **no** — binding 7 is declared `sampled` (`Sampled=1`) in every
> module read so far, and the input attachments the layer already fixes arrive
> through `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT`, a different descriptor type.

**RIGHT, and settled without the run.** All 333 declarations at set 0 binding 7
are plain 2D sampled images. The layout confirms it from the other side: the
storage images live at separate bindings (6 and 8) and the input attachments at
2 and 3, each its own descriptor type.

The two that would have changed the plan most were Q1 and Q3. Q3 held. Q1 broke
in the direction that removes work rather than adding it, which is the first
time in this project that a wrong prediction has been good news.

**Pass condition for the eventual patch:** `#103` flips from all-IDENTICAL to
all-DIFFER. Still nothing on screen — that waits for the format-44 chain.

**The same run also answered task #5's real question, unasked.** The per-eye
difference is already large and real in the G-buffer, and dies at exactly the
boundary the mechanism predicts. See *Where the difference dies*, below.

### Where the difference dies

The survey run had the probe on, and its verdicts across 9,934 frames locate the
failure boundary exactly. Counts are DIFFER / total probes for that image:

| image | shape | verdict |
| --- | --- | --- |
| `#97` | `RGBA16F` 1408² | **21 / 23 DIFFER** |
| `#95` | `RGBA16F` 1408² | **20 / 23 DIFFER** |
| `#98` `#99` | `RG16F` 1408² | **20 / 23 DIFFER** |
| `#102` | `R8_UNORM` 1408² | **6 / 6 DIFFER** |
| `#101` | `R8_UINT` 1408², 2 mips | **10 / 23 DIFFER** (6–9% of texels) |
| `#92` | `RGBA16F` 1408² | 6 / 23 DIFFER |
| `#103` | `BGRA8_SRGB` 1408² — the composition | **0 / 22 — always IDENTICAL** |
| `#104` `#105` | `RGBA16F` 352² | 0 / 21 IDENTICAL |
| `#122` `#123` | `R16_SFLOAT` 1408² | 0 / 18, 0 / 17 IDENTICAL |

Two things follow, and both are measurements rather than arguments.

**The stereo half of the mod works.** The shear and the per-view camera
constants produce genuinely divergent geometry in the lighting inputs — not a
uniform offset, not noise: `#101` differs in 6–9% of its texels with the first
differing texel scattered across the frame between probes ((9,0), (82,0),
(1274,0), (446,244)). Everything upstream of a sampler is already stereo.

**Everything downstream of a sampler is mono, and that is the whole remaining
problem.** `#103`'s writers are masked passes — the layer *does* run them
per-view — and their output is identical every single time, because what they
read arrives through a descriptor bound to layer 0. This is the central
asymmetry, finally measured end to end instead of reasoned about: multiview
view-indexes subpass inputs automatically and never view-indexes samplers.

**A free constraint on task #8.** `#122`/`#123` and `#104`/`#105` are *masked
yet identical* — the layer renders them twice with different view state and gets
the same answer both times. So their contents cannot depend on rasterized
geometry; they are derived purely from sampled data. That narrows what they can
be without guessing at them, which is the standing rule for these three.

### Why patching is the chosen mechanism

Decided with the user after measuring, not by default. The LDR domain is mono
*by construction*: every pass is single-subpass, no masked pass outputs LDR,
and the four passes writing the presented image are single-attachment — so
their input can only arrive by descriptor. **Multiview view-indexes subpass
inputs automatically but never samplers**, and a descriptor set has no
per-view form. Alternatives considered and rejected: replaying the LDR pass
range per eye (no shader knowledge needed, but invasive command-buffer
interception), and blitting to a double-wide source so a *vertex* patch could
offset UV (cheapest, but an extra full-res copy per frame).

Bindless narrows this further, and in the right direction. Replay would now be
*worse* than it looked — the source descriptor lives in a table shared by the
whole game, so pointing it at layer 1 between replays means rewriting a
descriptor other draws depend on, and descriptor sets cannot be updated while
in use. The index offset needs no replay, no extra copy, and no descriptor
rewrite: it adds a slot, and the shader picks which one.

The module-structure rules are shared and already proven (`d623ee1`):
`patch_vertex_clip` takes an optional right-eye matrix and selects
arithmetically rather than by branch or `OpSelect`, both cases validation-clean.
The index patch selects the same way — capability with the capabilities,
extension only below SPIR-V 1.3, decoration in the annotation section, new Input
added to the entry point's interface list. One caution specific to X4: a single
module carries **both** entry points, so a vertex patch and an index patch will
meet in the same module, each wanting its own `ViewIndex` input.

### Reading the instruments (hard-won; do not re-derive)

* **`X4VR_LOG` appends.** A "single run" log usually holds dozens. Segment on
  `instance created (app=X4)` before aggregating anything. Serials restart per
  run, so a whole-file summary is confident nonsense — it once produced a table
  contradicting the run it came from.
* **Writer lists need `X4VR_MV_INVENTORY=1`.** Without it every pass hashes to
  the `UINT32_MAX` sentinel and, because the list de-duplicates *by serial*,
  all of an image's writers collapse into a single `?`. Never read writer
  counts from a run without the inventory.
* **`(uniform 0xNN)` is not "content that agrees".** The probe reports a layer
  that is one repeated texel. Two layers merely *cleared* to the same value
  agree trivially, and counting that as evidence of mono behaviour is how a
  mono target passes for a stereo one. `(all zero)` is the special case it
  always named.
* **The inventory prints two verdicts now.** `MONO`/`STEREO` is about K;
  `+MASKED` is about replication. Since the split they are independent.

### The launch command that matches the baselines

    X4VR_GAMESCOPE=1 X4VR_ONE_EYE=1 X4VR_MV=1 X4VR_STEREO=1 \
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_MASK_TONEMAP=1 \
    ./launch/x4vr-launch.sh

`X4VR_GAMESCOPE=1` is not optional for comparability: X4 ignores
`res_width`/`res_height` while borderless and sizes to the display, so without
it every render target changes dimensions and the probe table stops being
comparable to the tagged runs. `X4VR_ONE_EYE=1` gives 1408×1408 (SBS width/2),
which is what every take from sixteen on has used.

### Knobs that matter now

    X4VR_MV=1                 doubling + masking (the whole stage-1 mechanism)
    X4VR_STEREO=1             per-eye K, both eyes baked, gl_ViewIndex selects
    X4VR_MASK_TONEMAP=1       mask rp #40/#52 so #103 replicates (keyed on the
                              SRGB attachment format; UNORM is left alone).
                              MISNOMER: those passes are not a tonemap, they
                              are LDR composition and are shared with UI
                              geometry. Kept for continuity with the tags.
    X4VR_MV_PROBE=1           the per-image layer0-vs-layer1 verdict table
    X4VR_MV_INVENTORY=1       pass/framebuffer/image inventory; required for
                              any join between passes and image serials
    X4VR_MV_DUMP=<prefix>     write the two layers as PPM on a DIFFER
                              (gated on RGBA16F — cannot dump #122/#123 yet)
    X4VR_SBS_LAYERS=2         allocate the second layer on the eye image
    X4VR_SBS_RIGHT_LAYER=1    …and actually show it in the right half
    X4VR_PRESENT_MODE=0       uncap the frame rate; every perf number before
                              this one measured the monitor, not the renderer
    X4VR_TEST_OUT_SRGB=1      test binary only: make its LDR second pass SRGB,
                              which is X4's tonemap in miniature
    X4VR_TEST_ARRAY_SAMPLER=1 test binary only: bind a 2D_ARRAY view instead of
                              a 2D one — goes with a patched fragment shader
    X4VR_DUMP_SHADERS=<dir>   write every module X4 creates as <dir>/mod-NNNN.spv;
                              the srgb-resolve log names which serials matter
    X4VR_BINDLESS_SURVEY=1    measure the bindless table: layout counts, variable
                              descriptor counts, distinct slots written, and
                              which slots hold a doubled image. No behaviour
                              change. Independent of X4VR_MV, deliberately —
                              MV=0 is the control

### Deliberately still open

* **What `#122`/`#123`/`#124` are** (task #8). Full-res single-channel
  `R16_SFLOAT`, a two-pass ping-pong sitting right after the `88×88×128` froxel
  volumes (`#116`–`#121`). Shape fits AO or a screen-space shadow resolve.
  Deliberately *not* guessed at: globally-applied shadows wrecked the earlier
  X4 VR attempt, so this gets identified before it is touched. They already
  replicate, so they need only the fragment half — a cheap second customer for
  the same patch once named.
* **The doubling overshoot** — 91 images / ~566 MB against ~18–21 / ~135 MB
  needed. Untouched on purpose so a tightening regression cannot be confused
  with a stereo bug.
* **No real perf number yet.** `X4VR_PRESENT_MODE` now makes one obtainable;
  `X4VR_MV=0` remains a valid baseline, so this is not urgent. Note the
  masked tonemap adds a second full-resolution fullscreen pass, so the
  measurement should happen with the knob in both states.
* **Lighting constants are still the left eye's.** Per-eye `K` shears clip
  space; `M_invprojection`, `V_cameraposition` @736 and
  `V_light_direction_view` @864 are not yet per-view, so the right eye's
  deferred lighting reconstructs position with the left eye's matrices. Not
  yet visible, and it will be once the LDR chain carries the difference.
* **`#100`** is the other `B8G8R8A8_SRGB` target in the block and no pass has
  been seen writing it. If a second SRGB writer ever appears, the
  `X4VR_MASK_TONEMAP` carve-out would catch it too — worth a check rather than
  a surprise.

### What the method has been worth

Four guesses have been overturned by measuring before patching, each of which
would have cost a wrong-looking live run and a false conclusion about the
mechanism:

* `#122`/`#123` "are the tonemap pair" — they are `R16_SFLOAT`. Patching them
  would have changed nothing and read as the fragment patch failing.
* `#101` "is intermittently mono" — it was a constant clear the instrument
  could not name. Patching near it would have been patching a non-problem.
* **"X4 binds textures per descriptor"** — it is bindless, one 53,306-entry
  table for the whole game. The type-promotion patch was built, proven offline,
  and is unusable here. Wiring it first would have promoted every texture X4
  owns and produced a spectacular, hard-to-attribute mess.
* **"`#103` is the tonemap output"** — it is the LDR composition target, and
  the pass it is written by is shared with UI geometry.

Three instruments have also been quietly wrong for at least one run each: the
probe's zero-only test, the writer-list `?` sentinel, and the sampler lister's
blindness to `OpTypeArray`. Enough of a pattern to be a rule: **check a new
instrument's first non-trivial reading against something already known.**

And one implementation trap caught offline: dropping SRGB from
`is_ldr_format`, the obvious one-line way to mask the tonemap, also shears it.
The suite reports that as `rp=sheared fb=masked` rather than merely going red.

---

## Per-view frame-constants UBO ("camera constants")

Found on a complex scene object's constant buffer. One UBO holds all the
per-view matrices, each a `mat4` (16 floats = 64 bytes). The reported
offsets are in **floats**, so the byte offset = float-offset × 4:

| # | Name | Float off | Byte off | Role | Per-eye action |
|---|------|-----------|----------|------|----------------|
| 1 | `M_view` | 0 | 0 | world → view | **PATCH** — apply ±IPD/2 along view-space X (and, in VR, the headset pose) |
| 2 | `M_projection` | 16 | 64 | view → clip | **PATCH** — per-eye projection (re-square to the eye's 1:1 aspect; VR: OpenXR frustum) |
| 3 | `M_invprojection` | 32 | 128 | clip → view (depth→pos) | **PATCH** — `inverse(M_projection_eye)`; drives deferred lighting reconstruction |
| 4 | `M_projection_uj` | 48 | 192 | unjittered projection | **PATCH** — eye projection without TAA jitter (== #2 with AA off) |
| 5 | `M_invprojection_uj` | 64 | 256 | unjittered inv-projection | **PATCH** — `inverse(#4)` |
| 6 | `M_jitter` | 80 | 320 | TAA jitter | leave (should be identity/zero with AA off) |
| 7 | `M_prevjitter` | 96 | 384 | previous TAA jitter | leave |
| 8 | `M_viewprojection` | 112 | 448 | world → clip | **PATCH** — `M_projection_eye × M_view_eye` (primary geometry transform) |
| 9 | `M_viewinverse` | 128 | 512 | view → world | **PATCH** — `inverse(M_view_eye)` |
| 10 | `M_shadowCSM0Clip` | 144 | 576 | world → shadow cascade 0 clip | **LEAVE** — light-space, shared by both eyes |
| 11 | `M_shadowCSM1Clip` | 160 | 640 | world → shadow cascade 1 clip | **LEAVE** |

Buffer is ≥ 704 bytes (11 × 64). **7 matrices patched, 4 left** (jitter ×2,
shadow ×2).

### What this tells us

- **X4 is a deferred renderer.** `M_invprojection` / `M_viewinverse` are the
  classic depth→view-space and view→world reconstruction matrices used by
  deferred lighting and screen-space effects. Because they live in the *same*
  per-view UBO as `M_view`/`M_projection`, **patching this one UBO per eye
  makes both geometry and lighting render for that eye** — the whole reason
  we chose double-render, achieved at a single interception point.
- **Shadows are view-independent.** The CSM clip matrices are light-space and
  shared, so we do not touch them and both eyes sample the same shadow maps.
- **TAA jitter is out of the way** once AA is disabled (jitter → identity;
  `M_projection == M_projection_uj`).

### Per-eye math (to validate empirically in Phase 3/4)

Let `V` = X4's `M_view`, `P` = X4's `M_projection` (computed for the 2:1
2816×1408 frame). For a flatscreen SBS eye with interpupillary `ipd`:

```
Veye = Translate(±ipd/2, 0, 0)_viewspace · V      # sign TBD; swap if eyes reversed
Peye = perspective(fovy_from(P), aspect = 1.0)    # re-square: 1408×1408 eye viewport
       # fovy from P[1][1] = 1/tan(fovy/2), aspect-independent
Peye_uj = Peye                                     # AA off
M_invprojection      = inverse(Peye)
M_invprojection_uj   = inverse(Peye)
M_viewprojection     = Peye · Veye
M_viewinverse        = inverse(Veye)
```

In VR (Phase 5), `Veye` = `eyeFromHead · headPose` from OpenXR and `Peye` =
the runtime's per-eye asymmetric frustum, instead of the ±ipd/2 + re-square.

Note X4 renders the 2:1 frame with a 2:1 projection; per eye we replace the
projection with a 1:1 one, so no squish. Handedness / reversed-Z convention
must be read off `P` once and matched exactly.

## Full-frame analysis (capture: `x4-capture-cockpit.rdc`, cockpit, static)

Parsed with `tools/parse_capture.py` from the XML conversion
(`renderdoccmd convert -c xml`). Frame = 2816×1385 (window at capture time),
742 draws, 52 dispatches, 47 render passes, ~2 queue submits.

### THE key finding: the view-constants arena (buffer 967)

- `VkBuffer` **967**: size **229,376 = 128 × 1792**, usage UNIFORM_BUFFER,
  bound at **memory 793 + 251,887,616**.
- It is an **array of per-view constant blocks, stride 1792 bytes**. The
  11-mat4 camera block (704 bytes) documented above is the head of each
  block; the remaining ~1088 bytes are other per-view constants.
- 62 descriptor-set slots reference it with `range 1792` at different
  offsets — one block per *view* in the frame:
  - **offset 3584 (block #2) = the main camera** — credited by ~211 scene
    draws (the G-buffer pass alone has 249);
  - other offsets (105728, 111104, 16128, 53760, 75264, 17920, 5376, …) are
    shadow-cascade and auxiliary views (~40–120 draws each).
- Memory 793 (and 829) are **huge (~268 MB) host-coherent persistently
  mapped arenas**; X4 updates constants by **plain CPU memcpy into the
  mapping** (zero `vkCmdUpdateBuffer` in the whole frame; renderdoc shows
  whole-arena `Coherent Mapped Memory Write` flushes).
- Descriptor sets are **baked** (1652 allocations, no dynamic offsets, no
  in-frame UBO descriptor writes — set state appears as renderdoc
  `Initial Contents`). So per-draw constants are addressed by *static*
  (buffer, offset) pairs in per-object sets.

**Interception consequences:**
1. The per-eye patch is a **CPU write into mapped memory** (the main-view
   1792-byte block), not a command-stream edit.
2. Because both eyes' submissions would read the *same* block, double-render
   needs either (a) submit-L → sync → rewrite block → submit-R
   (simple, serializes eyes), or (b) a layer-recorded clone command buffer
   with the eye-R constants sourced from a different block/copy (parallel,
   more machinery). Start with (a), optimize to (b).
3. Offsets are likely ring-allocated and can move frame to frame; at runtime
   the layer identifies the arena by "UBO with descriptor range 1792" and
   the *main* view block as the one bound by draws in the big G-buffer pass.

### Render-pass skeleton (frame order)

| # | Passes | FB size / formats | Content |
|---|--------|-------------------|---------|
| 1–4 | 4 | 2816×1385, 6 att: D32 + 3×RGBA16F + 2×RG16F | **G-buffer** (pass 3 = main geometry, 249 draws) |
| 5–8 | 4 | 2048×2048 D16 | **Shadow cascades** (4) |
| 9 | 1 | 2816×1385, 5 att | secondary geometry (60 draws) |
| 10 | 1 | 2816×1385, D32+R8_UINT | ID/stencil-ish pass (41 draws) |
| 11–22 | 12 | 1408×692 → 1×1, R32F | **depth/luminance pyramid** (auto-exposure) |
| 23–25 | 3 | 1408×692 RGBA16F | half-res ping-pong (bloom/blur head) |
| 26–31 | 6 | 2816×1385, 6 att | **deferred lighting/composite** (31 has 53 draws — light volumes) |
| 32–39 | 8 | 704×346 RGBA16F | bloom pyramid ping-pong |
| 40–41 | 2 | 2816×1385 R16F (+52 dispatches) | AO/exposure compute block |
| 42–45 | 4 | 2816×1385 | more composite (2×RGBA16F pairs) |
| 46 | 1 | 2816×1385 **B8G8R8A8_SRGB, 232 draws** | **UI/HUD pass** |
| 47 | 1 | 2816×1385 B8G8R8A8_UNORM, 1 draw | final blit → present |

Other UBO patterns for reference: per-object slots of range 768 (×724) and
4096 (×587); big arenas buffer 7243 (8 MB) / 9805 (4 MB) in memory 829.

### Notes / caveats

- The plain-`xml` conversion **omits buffer payloads** (`MapData` bodies),
  so matrix *values* can't be read from the XML — structure only. Values can
  be inspected in the qrenderdoc UI when needed.
- All 6-attachment targets are **full-frame sized**: intermediate passes are
  not split. First SBS implementation should therefore be **sequential
  full-frame per eye + composite into halves at the end** (option (a)),
  rather than trying to halve every intermediate target.
- The UI pass (46) is a separate SRGB pass after all 3D — good news: it can
  be rendered once and composited per-eye at a chosen depth plane.

## Cross-capture comparison (cockpit vs walking vs map)

Three captures parsed (`x4-capture-cockpit.rdc`, `x4-walking-cockpit.rdc`,
`x4-map.rdc`). The architecture is **identical across scene types**:

| | Cockpit | Walking | Map open |
|---|---|---|---|
| Passes / draws | 47 / 742 | 43 / 273 | 50 / 1283 |
| View arena | buffer 967 | buffer **838** (different run) | buffer 967 (same run as cockpit) |
| Arena stride/mem | 1792, mem 793+251.9M | 1792, mem 793+251.7M | 1792, mem 793+251.9M |
| Main-view block | off 3584 (211 draws) | off 8960 (59 draws) | off 8960 (498) **+ 91392 (256), 12544 (131), …** |
| Shadow cascades | 4× 2048² D16 | 4× | 4× |
| Compute block | 52 dispatches | 52 | 52 |
| UI pass | SRGB, 232 draws | SRGB | SRGB, 255 draws |
| Final | 1-draw blit | same | same |

Conclusions:

1. **Stable architecture.** Same pipeline in every mode; the view arena is
   always "the UNIFORM_BUFFER referenced by descriptor slots of range 1792"
   in the memory-793 arena. Buffer *ID and block offsets vary by run and
   scene* → runtime discovery must be dynamic (pattern-match, don't hardcode).
2. **The map is a real 3D scene**, not a 2D overlay — 717-draw G-buffer pass
   of galaxy/sector geometry, plus *multiple* simultaneously-active view
   blocks. Menu-mode (mono, world-locked quad) remains the right first
   design, but stereo-map is genuinely possible later.
3. Consequently, **mode detection cannot come from the render graph** (map
   looks like gameplay to Vulkan) — it must come from the injector (Lua /
   game state), as designed.
4. **Main-view selection rule** for the layer: the range-1792 block bound by
   draws of the *largest* 6-attachment G-buffer pass. Unambiguous in
   cockpit/walking; with the map open there are several big views, which is
   fine because menu mode doesn't patch cameras.

## Live validation (harness run, full gameplay session, 17,430 frames)

Ran the harness on X4 directly (menu → load save → cockpit → walking → map →
menu). Results:

- **Harness is stable**: no crash across 17,430 frames, save load, loading
  screens, and all scene transitions.
- **The view arena is double-buffered**: two UBO buffers alternate as the
  winner every frame (frames-in-flight). Per-eye patching must target the
  *current frame's* buffer.
- **The main-view block offset is NOT stable** — it **ring-moves** across the
  128-block arena over a session (observed blocks #0, #1, #2, #3, #4, #6–#9,
  #39, #41, #44, #54, #59, …). So the offset **cannot be hardcoded**; the
  layer must discover it dynamically every frame. Confirmed:
- **The dynamic "most-drawn range-1792 block wins" heuristic works** — in
  cockpit gameplay it locks onto the block credited by ~202 draws (matches
  the cockpit capture's ~211 main-view weight); in menus it correctly falls
  to a 1-draw block.
- **config.xml is read via `fopen(".../EgoSoft/X4/<id>/config.xml", "r")`** —
  the Phase-1 interposition point (note `ventureconfig.xml` shares the
  substring; match the exact basename).

**Refinement noted for Phase 3/4:** scope draw-crediting to the *main
G-buffer render pass* (the largest 6-attachment pass) rather than all draws,
so the winner is unambiguous even mid-transition. The current whole-frame
heuristic is adequate for detection but we want render-pass precision before
we start writing eye matrices.

## Still to determine (updated)
- The remaining ~1088 bytes of the 1792 block (per-view params that may also
  need per-eye patching, e.g. camera world position for specular).
- Present-time details: swapchain image count, present mode, and how the
  final pass maps onto the swapchain image (single full-screen draw — easy
  overlay point). *Partly answered live:* `1408x1408 images>=4 format=44
  presentMode=2`, and the SBS composite already blits at present time.
- All three captures are 2816×1385 (config height fix hadn't taken effect);
  re-verify 2816×1408 next session. *Superseded:* the target is now 1408×1408
  per eye (tag `one_eye_baseline`); the live pass inventory in Phase 4b below
  is at that size and confirms the capture's pass map.
- Which render targets are distinct **images** rather than distinct passes —
  needed to turn the Phase 4b doubling estimate into a real number, and the
  same `vkCreateImage` hook the doubling itself needs.

## Phase 3 findings: camera-relative rendering (the blocker)

Live experiments with the layer writing into the view arena (all under
gamescope, menu 3D scene, verified by screenshots):

| Experiment | Result |
|---|---|
| Read the view block | **Works.** Values are live and correct: the projection's aspect tracked the swapchain exactly (1.778/0.889 = 2.000 at 2816×1408; 1.778/0.744 = 2.39 at 3440×1440). |
| `M_view` contents | **Always IDENTITY**, in every block of the arena (survey of 128 blocks). |
| `M_projection` | Reversed-Z, infinite far plane, near = 0.1, column-major (m[11]=±1, m[15]=0), Y-flipped (m[5] negative). |
| Write eye offset into `M_view` + rebuild `M_viewprojection`/`M_viewinverse` | **No visual effect.** |
| Zero `M_viewprojection` in the credited block | **No visual effect.** |
| Hammer-zero `M_viewprojection`+`M_projection` at ~10 kHz from a thread | **No visual effect** (rules out any write/read race). |
| Write-then-verify at present time | Block is **STILL ZERO** at present → X4 does not rewrite it; the GPU is reading its camera data from elsewhere. |
| Zero the **entire** arena buffer (229,376 B) | **Screen goes fully black** → the arena *is* consumed by the GPU. |

**Interpretation.** `M_view` being identity everywhere means X4 renders
**camera-relative**: object transforms are baked against the camera on the
CPU (normal for a space sim, to avoid float precision loss at astronomical
distances). So the per-view `M_view`/`M_viewprojection` are not what
positions the geometry — which is exactly why writing them changes nothing,
while wiping the whole arena still kills the image (the deferred lighting
and post passes *do* read this block, most likely `M_invprojection` /
`M_viewinverse` for depth→view reconstruction).

**Also fixed along the way:** draw-crediting ignored the descriptor *set
index*, so an auxiliary/UI view could win the frame. The capture shows the
main camera block is the range-1792 UBO bound at **descriptor set 1** during
the 6-attachment G-buffer pass (84% of its 249 draws). Crediting is now
scoped to set 1, though in the menu scene the winner is unchanged.

### Consequences for the stereo design

Patching one per-view matrix is **not** sufficient on this engine. The
realistic options, in order of preference:

1. **Pre-multiply the per-object transforms.** With `V = I`, an object's
   baked transform already includes the camera. An eye offset `d` is then a
   fixed pre-multiplication: `MVP_eye = P · T(−d) · P⁻¹ · MVP`. This needs
   the per-object constant layout (the range-204/256/768/4096 blocks bound
   at set 2+) — a renderdoc shader-reflection job, same method that gave us
   the 11 view matrices.
2. **Patch the projection only** for a *sheared* stereo (off-axis frustum).
   Cheap, but gives parallax without a true eye translation — needs testing
   for comfort, and depends on `M_projection` actually being consumed by
   geometry (not yet proven: zeroing it had no effect, which suggests
   geometry uses baked MVPs including projection).
3. **Intercept at the shader level** (SPIR-V patching) to inject an eye
   offset into the vertex stage uniformly — heaviest, most invasive, but
   independent of how the CPU bakes transforms.

Next diagnostic (cheap, decisive between the above): zero **only**
`M_invprojection` (float offset 32) and only `M_projection` (offset 16),
separately, in every block. If invprojection alone blackens the image, the
view block is confirmed lighting-only and option 1 becomes the main path.

## Ground truth: shader reflection (SPIR-V debug names)

X4 ships its shaders **with full debug info**, so the constant layouts can be
read directly instead of inferred. Extracted all 140 `vkCreateShaderModule`
payloads from the capture's blob archive and disassembled with `spirv-dis`
(see `tools/extract_shaders.py`). The engine binds **five** descriptor sets:

| Set | Block | Purpose |
|-----|-------|---------|
| 1 | `BLOCK_BUFFER_BINDING_SLOT_CAMERA` (64 members, 1792 B) | per-view constants |
| 2 | `BLOCK_BUFFER_BINDING_SLOT_MATERIAL` (64 members) | material params |
| 3 | `BLOCK_BUFFER_BINDING_SLOT_WORLD` (15 members) | **per-object transforms** |
| 4 | `BLOCK_BUFFER_BINDING_SLOT_DYNAMIC` (11 members) | texture/envmap matrices |

### Set 3 — `BLOCK_BUFFER_BINDING_SLOT_WORLD` (the missing piece)

| Off | Member |
|-----|--------|
| 0 | **`M_worldviewprojection`** |
| 64 | `M_world` |
| 128 | `M_prevworldviewprojection` |
| 192..448 | `M_shadowCSM0..4` |
| 512 | `V_blendcolor` |
| 528 | `F_alphascale` |
| 532+ | `B_packedtangentframe`, `B_vertexdata0..2`, `B_useskinning` |

**This explains every Phase-3 result.** Geometry is transformed by
`M_worldviewprojection` from the *per-object* set-3 block — already baked on
the CPU, camera included (hence `M_view` = identity). The set-1 camera block
we were patching is consumed by lighting/post (and by `V_cameraposition`,
`M_invprojection`, …), which is why zeroing the whole arena blackened the
screen while patching `M_view`/`M_viewprojection` did nothing at all.

### Set 1 — camera block, beyond the 11 matrices

The block is 64 members; the first 11 are the matrices already documented.
The rest matter for stereo correctness:

| Off | Member | Note |
|-----|--------|------|
| 704 | `V_viewportpixelsize` | per-eye viewport size |
| 720 | `V_screenresolution` | |
| 736 | **`V_cameraposition`** | must move with the eye (specular/parallax) |
| 752..848 | `V_ambient1`, `V_direction1..3`, `V_lightcolor1..3` | |
| 864 | `V_light_direction_view` | **view-space** light dirs — per-eye |
| 928 | `V_csmthresholds` | |

### Revised stereo plan

Per eye we must patch **two** places, not one:

1. **Set 3, offset 0** — `M_worldviewprojection` for *every object drawn*:
   `MVP_eye = P · T(−d) · P⁻¹ · MVP`, a single fixed 4×4 pre-multiplication
   (`M_view` is identity, so the eye offset is a pure view-space translate).
   `M_prevworldviewprojection` (off 128) needs the same treatment for
   motion-vector correctness.
2. **Set 1** — the camera block: `M_view`/`M_viewprojection`/`M_invprojection`
   (+`_uj` variants) *and* `V_cameraposition` and `V_light_direction_view`,
   so deferred lighting matches the eye. This is the "per-eye lighting" the
   design called for.

Shadow matrices (`M_shadowCSM*` in both blocks) stay shared — light-space.

Open question for the next step: per-object blocks are numerous (724×768 B +
587×4096 B observed). Patching them on the CPU each frame is feasible but
touches a lot of memory; the alternative is a GPU-side `vkCmdUpdateBuffer`
injected per draw, or SPIR-V patching of the vertex stage to apply the eye
transform once. Measure before choosing.

## Phase 3b: clip-space injection works (the Phase-4 mechanism)

`common/x4vr_spirv.hpp` patches scene vertex shaders to append
`gl_Position = K * gl_Position`, with `K` baked in as SPIR-V constants at
`vkCreateShaderModule` time — **zero per-frame CPU or GPU cost** beyond one
mat4×vec4 per vertex.

Validation, offline against all 140 shader modules extracted from the
capture:

- **136 patched, 0 rejected by `spirv-val`, 4 correctly skipped** (compute).
- X4's modules are **multi-entry**: 136 contain *both* a Vertex and a
  Fragment entry point in the same module. The patcher scopes its injection
  to the Vertex entry function only — verified by disassembly (the injected
  `OpMatrixTimesVector` appears exactly once, inside `%main` (Vertex), never
  in `%main_0` (Fragment)).

Live in X4 (`X4VR_CLIP_SHIFT=0.35`): shaders patch during load with no
driver rejections, and the rendered scene is visibly translated in NDC.
**This is the mechanism Phase 4 will use**, with `K = P·T(−d)·P⁻¹` per eye.

### Known follow-up: the UI shifts too

The 2D UI/HUD is drawn with vertex shaders as well, so it is displaced by
`K` along with the world. For stereo this is wrong — the UI must either be
excluded from the patch or given its own (smaller, or zero) eye offset so it
sits at a comfortable fixed depth. Options, cheapest first:

1. Skip patching modules whose vertex shader does **not** bind the set-3
   `BLOCK_BUFFER_BINDING_SLOT_WORLD` block (UI draws are unlikely to use
   per-object world transforms) — a static, zero-cost classification made at
   module creation.
2. Patch UI shaders with a separate `K_ui` (identity, or a mild depth
   offset), which also gives us the "UI at a chosen depth plane" the design
   wants for menu mode.

The frame map already shows the UI is a **separate late SRGB pass**, so a
per-pass distinction is available as a fallback.

### Classification attempt 1: set-3 presence (partial success)

`x4vr::spv::classify()` splits vertex modules by whether they declare the
set-3 per-object world block. On X4's 140 modules: **96 WORLD, 40 UI, 4
compute**. The 40 "UI" modules all have the interface signature
`%IO_uv0 %_ %gl_VertexIndex` — the classic fullscreen-triangle pattern, i.e.
**fullscreen post-process passes** (bloom, lighting composite, blits).

Keeping those at identity is important in its own right: giving a screen-space
pass a world eye offset would corrupt it, not merely misplace it. So this
classification earns its keep regardless of what happens with the HUD.

**But it does not isolate the HUD.** Live test with `K_world` shifted and
`K_ui` identity: the menu text still moved with the world (its origin went
from x≈215 to x≈500 in a 2000-px-wide capture). So X4's UI/text shaders
*also* declare set 3 — presumably they share the standard pipeline layout
even when the block's contents are irrelevant to them. Declaring a set is
not the same as being positioned by it.

Better discriminators to try, cheapest first:

1. **Vertex input attributes.** World geometry declares
   `SPECIAL_VERTEXLOCATION_POSITION/NORMAL/TANGENT`; UI/text quads are far
   more likely to be index- or uv-driven. X4 names its interface variables,
   so this is readable straight from the SPIR-V at module creation — still
   zero per-frame cost.
2. **Per-render-pass selection.** The frame map already shows the UI is a
   separate late SRGB pass. This is exact, but needs two pipeline variants of
   the same module (one per K) selected at bind time, so it costs pipeline
   memory and some bookkeeping.
3. **Whether the shader actually *reads* `M_worldviewprojection`** (member 0
   of the set-3 block) rather than merely declaring the block — a data-flow
   check on the SPIR-V. More precise than (1), more work.

Note for Phase 4: a shifted HUD is not fatal to a first SBS milestone (it
lands at *some* depth rather than the right one), so this can be solved in
parallel with the eye split rather than blocking it.

## Phase 4a: the per-eye matrix, and where it may/may not be applied

### Derivation (implemented in `x4vr::make_eye_shear`)

X4's projection gives, for a view-space point `(x, y, z, 1)`:

```
x_c = sx*x     y_c = sy*y     z_c = near (constant!)     w_c = z
```

Offsetting the camera by `dx` means translating the world by `-dx`, so
`x_c' = x_c - sx*dx`. Because `z_c` is the constant `near` for every vertex,
that constant term can be carried by `z_c`, giving an identity matrix with a
single shear term:

```
K[0][2] = -sx*dx/near        (column-major m[8])
```

In NDC this is `x_ndc' = x_ndc - (sx*dx)/z`: the shift falls off with view
depth, i.e. **true stereo parallax** — near geometry separates strongly,
distant stars not at all. (The Phase-3b proof used a plain clip-space
translation, which slides everything uniformly and carries no depth cue; it
proved the injection mechanism, not the stereo math.)

Env: `X4VR_EYE=left|right`, `X4VR_IPD=<metres>` (default 0.064),
`X4VR_PROJ_SX` / `X4VR_PROJ_NEAR` (defaults are the values measured at
2816×1408; to be derived from the live camera block later).

**Caution:** the shear scales as `dx/near` with `near = 0.1`, so it is ~10×
`sx*dx`. An "exaggerated for visibility" IPD must stay small — 0.3 m is
already a strong effect; several metres produces meaningless output.

### Where the matrix is valid — and where it is not

The derivation assumes clip `z_c` is the constant camera near plane. That
holds only for draws going through the main perspective projection. It is
**wrong for shadow passes**, which transform through `M_shadowCSM*` in
light space.

Classification was therefore tightened: a module counts as World only if its
vertex stage actually **reads member 0 (`M_worldviewprojection`)** of the
set-3 block, not merely declares the block. On X4's shaders: **94 World / 42
UI / 4 compute** (this correctly moved two shadow-related fullscreen modules
into the non-shifted bucket).

That still does not separate shadow *geometry* draws, because X4 bakes the
light-space matrix into the same `M_worldviewprojection` slot and reuses the
same shader modules. But the capture settles how to handle it:

> **Shadow and main passes share no pipelines** (shadow passes 5–8 use 9
> pipelines, main geometry passes 3/46 use 11, intersection = **0**).

So the exclusion belongs at **pipeline creation**, not module creation:

1. Hook `vkCreateRenderPass` and record which passes are depth-only
   (shadow passes are 2048×2048, D16, no colour attachments).
2. In `vkCreateShaderModule`, keep **both** variants — the original and the
   K-patched one.
3. In `vkCreateGraphicsPipelines`, pick the original variant when the
   pipeline's render pass is depth-only, the patched variant otherwise.

Static, decided once per pipeline, zero per-frame cost — and it generalises
to the two-eye case (one pipeline variant per eye) that Phase 4b needs.

## UI input is CPU-side and does not follow GPU-side transforms

Live finding (2026-07-25 cockpit session, right-eye shear at IPD 0.3): with
the UI misclassified as world geometry, every UI surface — main menu, cockpit
HUD, and the map — rendered shifted left by the same amount. Two conclusions:

1. **The menu, the HUD and the map share one reference frame.** They are all
   driven by the same per-object block and all inherit `K_world` together, so
   they can be fixed (or moved) as a single unit.
2. **Their interaction boxes did NOT move.** To click an item that appeared at
   position *x*, the mouse had to go to its original, unshifted position —
   noticeably far to the right of where the element was drawn.

Point 2 is the important one and it is a hard architectural constraint:

> **X4 hit-tests the UI on the CPU, in unshifted screen space.** Anything we
> do to UI geometry on the GPU desynchronises what the player sees from what
> the player can click.

Consequences for the design:

- `K_ui` must stay **identity**. Shearing the UI is not merely cosmetically
  wrong, it breaks input. This is now a correctness requirement, not a
  preference.
- The "world-locked menu quad" idea in menu mode **cannot** be implemented by
  transforming UI vertices in the layer alone. Whatever transform is applied
  to the UI for VR must be accompanied by the *inverse* transform on pointer
  coordinates, applied before X4 sees them — i.e. in the `LD_PRELOAD`
  injector (SDL mouse events), not in the Vulkan layer.
- This is precisely the job of the planned **cursor shim**, and it is a
  stronger requirement than "make the cursor visible in VR": the shim owns the
  mapping between VR pointing and X4's canonical 2816×1408 screen space.
- Because the map renders in the UI pass, excluding that pass from the eye
  shear also lands the map in mono — which is what menu mode wants anyway.

### UI-pass exclusion (same mechanism as the shadow exclusion)

Pass 46 is a separate SRGB pass (`B8G8R8A8_SRGB`, 232 draws) after all 3D,
and pass 47 is the final `B8G8R8A8_UNORM` blit; every world pass writes
multi-attachment float targets (RGBA16F / RG16F / D32). So a render pass
whose colour attachments are **all 8-bit UNORM/SRGB** is screen-space, and
its pipelines take the unpatched module variant — decided once, at pipeline
creation, exactly like the depth-only case.

## Phase 4a validated: the shear is real parallax

Measured 2026-07-25 with `tools/measure_parallax.py`, comparing two captures
of the same save at the same camera pose: one with `X4VR_EYE` unset (no
shader patching at all) and one with `X4VR_EYE=right X4VR_IPD=0.3`.

| Region | Shift | Corr | Implied z |
|---|---:|---:|---:|
| Starfield (top right) | **0 px** | 0.999 | ∞ |
| HUD radar (screen space) | **0 px** | 0.708 | — |
| HUD bars (screen space) | **0 px** | 0.892 | — |
| Canopy strut (mid) | **−117 px** | 0.788 | 1.60 m |
| Right hull panel (near) | **−172 px** | 0.801 | 1.09 m |

Three distinct behaviours from one constant matrix:

- **Screen-space UI: exactly 0** — the pipeline-creation exclusion works.
- **Stars: 0 at correlation 0.999** — this is simultaneously the strongest
  possible camera-alignment check (the two loads are pixel-identical at
  infinity) and the far reference. Anything other than 0 here would have
  meant the poses did not match, not that parallax vanished.
- **Cockpit: −117 and −172 px** — and crucially the two near regions differ
  *from each other*, ordered correctly: the hull panel is nearer than the
  canopy strut, so it moves more.

A uniform clip-space translation would have moved all five regions by an
identical amount. Four distinct values (0, 0, −117, −172) can only come from
a shift proportional to 1/z. **The clip-space identity holds in the engine,
not just on paper.**

The measured displacements are below the 190–375 px band predicted before the
run, but that band assumed cockpit geometry at 0.5–1 m. The implied depths are
1.09 m and 1.60 m, and `shift = sx·dx/z` reproduces the measured values there
exactly — the model is confirmed; only the guessed distances were off.

Note this validates the **geometry** half of a stereo eye. Per-eye *lighting*
still requires patching the camera block (`M_view`, `M_viewprojection`,
`M_invprojection(_uj)`, `V_cameraposition` @736, `V_light_direction_view`
@864), which the deferred passes read; that is the remaining Phase-4a work
before `sbs_lighting_done`.

## Phase 4b: multiview — the device, and the render-pass partition

The second eye arrives as **array layer 1** rather than as the right half of a
wider frame. That choice is forced, not stylistic: X4 lays its UI out from the
*window* size while rendering into the swapchain extent, so a wide window with
a narrow render desynchronises what is drawn from what can be clicked (the
"two left halves" symptom — see `docs/x4-quirks.md`). An extra array layer is
invisible to X4's sizing; an extra half-width is not. Render size stays equal
to window size, which is the arrangement verified end to end at tag
`one_eye_baseline`.

### The device supports it; X4 switches it off

Probed at `vkCreateDevice` (`X4VR_MV_INVENTORY` is not needed for this — it
always logs):

```
multiview: supported=1 maxViews=8 maxInstanceIndex=2147483647
           geomShader=1 tessShader=1
multiview: X4 requests it? ext=0 feature=0 — device api 1.4, app api 1.2
multiview: enabled in X4's existing feature struct
```

Three facts worth keeping:

1. **X4 declares Vulkan 1.2**, where multiview is core. No extension to add,
   no KHR alias to chase, `VkRenderPassMultiviewCreateInfo` under its core
   name.
2. **X4 leaves the feature disabled**, and a disabled feature makes every
   multiview render pass invalid. The layer enables it.
3. **X4 already supplies a feature struct** with `multiview = VK_FALSE`, so
   the layer takes the *flip* path, not the prepend path. Adding a second
   `VkPhysicalDeviceMultiviewFeatures` beside an existing
   `VkPhysicalDeviceVulkan11Features` is forbidden outright, which is why the
   code searches the chain before prepending.

Regression-tested by `tests/run-multiview-enable.sh`, which proves the feature
is *live* (it creates a two-view render pass and lets validation judge) rather
than merely requested, and includes two cases that must fail.

### Half of X4's render passes never run

`X4VR_MV_INVENTORY=1`, joined with the framebuffer log by
`tools/mv_inventory.py`, over a full session (cockpit → walking → map → exit):

| Verdict (seed) | Reason | Declared | Instantiated |
|---|---|---:|---:|
| MONO | all-LDR/UI | 9 | 6 |
| MONO | depth-only/shadow | 10 | 5 |
| STEREO | "world" | 47 | 23 |

**66 render passes are declared; only 34 ever get a framebuffer.** X4 builds
several variants of each pass and instantiates one — with antialiasing forced
off, the MSAA twins are dead.

*Trap:* the dead twins must still be classified **identically** to their live
siblings. Pipelines are created against a render pass, so giving one variant a
view mask while its twin keeps none splits them into incompatible passes. Do
not "optimise" by skipping passes that were never seen with a framebuffer.

### The live frame, by role

| Passes | Extent | Attachments | Role |
|---|---|---|---|
| 23 | 1408² | `RGBA16F, RGBA16F, RG16F, RG16F` + `D32` | **G-buffer** — exactly one |
| 13, 28–32, 51, 53, 60, 65 | 1408² | `RGBA16F` + `D32` | forward / transparent geometry |
| 25 | 1408² | `R8_UINT` + `D32` | ID / selection buffer |
| 38, 39 | 1408² | `RGBA16F` | fullscreen HDR |
| 62, 64 | 1408² | `R16_SFLOAT` | single-channel fullscreen |
| 34, 36 | 352² | `RGBA16F` | bloom, quarter-res |
| 21 | 256² | `RGBA16F` | small aux target |
| 27 | 704² | `R8_UINT` | half-res mask |
| 55, 57, 59 | **4096×1** | `RGBA16F` | **exposure / luminance reduction** |
| 42, 44, 46, 48, 50 | 2048² | `D16` | 5 shadow cascades |
| 0, 1, 7, 14, 40, 52 | 1408² | `BGRA8(_SRGB)` | UI / final blit |

The G-buffer's shape (4 MRT + D32, and only one instance) confirms the
capture's pass map against a live session.

### The seed's axis was wrong — "world" is not the question

Reusing the shear classification (`unsheared`) as the multiview seed exposed a
flaw that had been latent in it all along.

`is_ldr_format()` recognises only **four-channel** 8-bit formats. Every
single- and two-channel target — `R8_UNORM`, `R8_UINT`, `R8G8_UNORM`,
`R16_UNORM`, `R16_SFLOAT`, `R16G16_UNORM` — therefore fails the LDR test, is
treated as HDR, and lands in the "world geometry" bucket. Their shape gives
them away: **one colour attachment, no depth attachment**. A pass with no
depth attachment is a fullscreen quad, not world geometry.

*Why this never broke the shear:* the shear has a **second gate** — a module
counts as World only if its vertex stage actually reads `M_worldviewprojection`
(member 0 of the set-3 block). Fullscreen quads do not, so they were excluded
at the shader level regardless of what the pass classification claimed. The
pass-level over-claim was absorbed silently and invisibly.

**Multiview has no such second gate.** Doubling is decided per pass, so the
over-claim would ship.

But the fix is not to move those passes to MONO. A bloom or tonemap pass
consuming the per-eye lighting result *must* run per eye.

### Two orthogonal decisions, not one

The seed conflates two questions that have different answers, and untangling
them is the whole of the partition:

> **1. How many layers?** — does this pass contribute to the final per-eye
> image?
>
> * **2 layers:** G-buffer, lighting, all screen-space post, tonemap, **and the
>   UI**.
> * **1 layer:** shadow cascades (light space, shared by construction) and the
>   exposure reductions (a shared scalar).
>
> **2. Which `K` per view?** — this is what `unsheared` already answers.
>
> * **World:** the per-eye shear.
> * **UI:** identity for *both* views.

**The UI is the case that proves they are separate.** "The UI must be mono"
means *it must not be sheared* — not *it must be rendered once*. The UI has to
appear in **both** eyes, at the same screen position. So its passes are
two-layer like everything else, drawn twice with an identity `K`, which puts it
at zero parallax in both eyes.

That is also what keeps input working: identity `K` means the drawn position
equals the unshifted screen position X4 hit-tests against (see the CPU
hit-testing section above). Rendering the UI into only one layer would leave
the other eye without a HUD.

So the seed's *verdicts* are largely right for question 2 while its *reasons*
are wrong, and it does not answer question 1 at all. Question 1 is tracked as a
separate `per_eye` classification; `unsheared` is left alone so the
verified-good shear does not move while something else changes.

### Reductions are shared, and they announce themselves by shape

Passes 55/57/59 are **4096×1**. A one-pixel-tall target is a scan, not a view
of the world — the luminance/exposure chain.

These must be **MONO**, and for a reason that is not thrift: per-eye exposure
lets the two eyes auto-expose independently and flicker against each other,
which is far worse in a headset than on a monitor. The seed had them as
STEREO, so this is a correctness fix the seed would have shipped.

> **Measured at the start of stage 2, and the fix is not needed.** This was
> carried for three phases as a correctness item to do before `K` differs. It
> was never tested, and it is wrong.
>
> Divergence requires the read to be **view-indexed**, and nothing in the
> reduction chain is. All seven 4096×1 targets are `SAMPLED |
> COLOR_ATTACHMENT` (`usage=0x14`) with no `INPUT_ATTACHMENT` bit anywhere,
> and the reduction framebuffers carry `attachments=1` — with a single
> attachment there is no second one to read as a subpass input, so the source
> can only arrive through a descriptor. A plain `sampler2D` on a single-layer
> view reads layer 0 whatever `gl_ViewIndex` says, and the shipping path does
> not substitute sampler descriptors (only input attachments, and only
> `X4VR_MV_PRESENT_LAYER` touches the sampled ones).
>
> So both views read the same texels, compute the same value, and write it to
> their own layer; whatever samples the result reads layer 0. The chain
> already produces **one shared exposure, derived from the left eye** — which
> is the behaviour the "fix" was meant to produce.
>
> Leaving it masked costs two 4096×1 passes instead of one and ~224 KB of
> doubled targets. Not worth a change that would have been made on an
> untested premise, in the one part of the frame where a mistake shows up as
> the whole image being the wrong brightness.
>
> Note the shape of this: the claim was structural and confident, and the
> refutation came from data already on disk — the same framebuffer log, from
> the same run. Cf. the correction at the overlap check: a log from the
> *right* run is the cheapest instrument there is.

`tools/mv_inventory.py` flags any STEREO pass with a dimension ≤ 4 for exactly
this reason — found by shape rather than by guessing which passes are
reductions.

### VRAM is not the pruning criterion

Costing every instantiated STEREO pass at double gives **330 MB**. That number
is an upper bound and almost certainly a large overestimate: it bills *passes*,
not *images*, so the ten `RGBA16F + D32` geometry passes — very likely one
colour/depth pair reused ten times — are counted ten times over. Resolving it
needs `vkCreateImage`.

Either way the conclusion holds: on a 24 GB card even the inflated figure is
noise. **Memory is not a reason to exclude a pass.**

The real cost of stereo is **fragment shading** — every per-eye pass shades
twice the pixels, and that is inherent to drawing two eyes, not something
multiview avoids. What multiview saves is CPU submission and vertex work
versus double-submitting the frame.

*Consequence:* passes are excluded for **correctness** (light-space, shared
exposure, CPU-hit-tested UI), never for thrift. Everything genuinely
downstream of the camera stays doubled and costs what stereo costs.

### Known remaining work

* **Multiview does not cover compute.** A compute dispatch reading a doubled
  image sees layer 0 and silently produces a mono result for both eyes. The
  overrides already disable most of that chain (`ssao=0`, `ssr=false`,
  `glow=0`, `antialiasing=none`), so what remains is shadows → G-buffer →
  lighting → exposure → UI. Re-enabling any of those settings reopens this.
* **The deferred lighting pass samples the G-buffer.** Once the G-buffer is
  two-layer, those samplers must become `sampler2DArray` indexed by
  `gl_ViewIndex`. Bounded to one pass and a handful of modules, but it is real
  SPIR-V work and the one part of Phase 4b that is not plumbing.

  > **Wrong, and the error was expensive (takes 6–15).** X4's lighting passes
  > (rp 30/31/32/64) do not *sample* the G-buffer — they read it as **subpass
  > inputs** (`S_subpassInput_AUTOMS`). Under multiview a subpass input is
  > already view-indexed by the spec: view N reads layer N, with no shader
  > change at all. So the predicted "real SPIR-V work" was zero SPIR-V work.
  >
  > What it was instead: the *descriptor* has to name the same subresource the
  > framebuffer does. `vkCreateFramebuffer` was swapping attachments for
  > two-layer array views while X4's input-attachment descriptors still named
  > single-layer ones, so view 1 read an image that had no layer 1. Fixed by
  > substituting the array view at `vkUpdateDescriptorSets`.
  >
  > Worth keeping as written, because the wrong prediction is instructive: it
  > named the right pass and the right resource and still pointed at the wrong
  > *mechanism*, which is exactly the kind of near-miss that survives review.
  > The lesson for the sampled case is that it may yet arrive — if a later
  > phase re-enables a chain that reads the G-buffer through an ordinary
  > sampler, `sampler2DArray` becomes real again. It just was not this.

### Measured: the real doubling cost, and the sharing that hid it

`vkCreateImage` + `vkCreateImageView`, joined at `vkCreateFramebuffer`
(session: menu → flight → dock at a station → walk around):

```
attachment images bound to a framebuffer: 27
of those, touched by a per-eye pass:       21
REAL extra VRAM to double them:              135.3 MB
  (pass-level upper bound was               316.8 MB)
```

The 2.3× overcount is exactly the aliasing predicted: **one D32 depth image
(`#93`) is shared by 11 render passes**, and three G-buffer targets
(`#97/98/99`) by 9 each. Counting passes billed each of them nine or eleven
times.

1376 `vkCreateImage` calls in the session, of which 27 are framebuffer
attachments — the rest is streamed station and ship texture.

**No conflicts.** No image is written by both a per-eye and a shared pass, so
the write-side partition is clean and nothing needs an arbitration rule.

*Caveat on the number:* X4 builds a complete set of targets for the menu
(`#46/47/51-53`) and another for the game a second later (`#92-99`). Without
destroy tracking both are counted, so 135.3 MB is an upper bound on an upper
bound; the true figure is likely nearer 80 MB. `img #N: destroyed` was added
afterwards so later runs discount them. It changes no decision — every
candidate figure is far below where memory matters.

### The swapchain is the cut point

Passes 0, 1, 7 and 14 attach a **driver-owned swapchain image** (printed `?` by
the inventory, because it never passes through `vkCreateImage`). A swapchain
image cannot be given a second array layer — it is the thing being presented.

So the per-eye chain has to stop one step earlier, and the natural place is
already built: `SbsCompositor` hands X4 its own images from
`vkGetSwapchainImagesKHR`. Those become **one two-layer image** rather than
per-eye images, X4's final pass writes both layers, and the compositor blits
layer 0 and layer 1 into the real swapchain — side by side for SBS on a flat
screen, or straight into an OpenXR swapchain later.

## Phase 4b stage 1: doubling the frame — what it took to get layer 1 rendered

Stage 1 renders the frame into two array layers with the **same** eye matrix for
both, so a correct result is indistinguishable from before. Enabled with
`X4VR_MV=1` (off by default). Gate 1: `fallbacks=0`, and the game visually
unchanged.

> **Correction (take sixteen).** "The game visually unchanged" was recorded
> here as if it verified something. It did not, and could not: what reaches the
> screen is layer 0 in every configuration except an explicit redirect, so an
> unchanged screen says nothing whatever about layer 1. Every run in this
> document that "looked normal" had a broken second view. Stage 1 was actually
> verified ten takes later, by reading the two layers back and comparing bytes
> — see *take sixteen* at the end. A test whose passing condition is "nothing
> looks different" is only a test if the thing under test can change what you
> are looking at.

Measured on X4 with `X4VR_MV=1`:

```
mv final: doubled=90 masked=46 substituted=21 per_eye_images=18
          redirected=0 fallbacks=0
mv final: pipelines masked=570 unmasked=595 dynamic_rendering=0
mv: X4 uses vkCreateRenderPass (v1)
```

`substituted=21` independently reproduces the 21 per-eye images found offline
by joining framebuffers to passes — two unrelated methods agreeing.

### THE finding: multiview replicates draws, not transfers

> **X4 copies between its render targets, and every per-eye image carries
> `TRANSFER_SRC | TRANSFER_DST`. A copy region names `layerCount = 1`, so it
> moves layer 0 and leaves layer 1 holding whatever was there before. One such
> copy anywhere in the chain drains the second view.**

*Symptom it was found through:* the 3D scene entirely black while the HUD
renders perfectly — the UI never passes through a doubled image. Note the
wording: this finding was *found via* that symptom, and does not *explain* it.
Widening the transfers fixed 14554 real copies and left the screen exactly as
black. See takes three to five below.

*Why nothing reported it:* copying one layer of a two-layer image is **legal**.
Validation has nothing to say. There is no error anywhere; the data simply
stops propagating.

*Fix:* `vkCmdCopyImage`, `vkCmdBlitImage`, `vkCmdResolveImage` and
`vkCmdClearColorImage` widen a region to cover both layers — but **only** when
it starts at layer 0 and covers exactly one. Anything else is a deliberate
per-layer access. Counted as `transfers_widened`.

*Generalisation worth carrying into every later phase:* **multiview only
replicates what happens inside a render pass.** Transfers, clears outside a
pass, and compute dispatches all still act on layer 0 alone and must be widened
by hand. Compute is not a problem today only because the config overrides
disable most of that chain (`ssao=0`, `ssr=false`, `glow=0`).

### How it was found — the elimination chain

Each step killed an entire class of cause, and the order mattered because the
cheap tests came first:

| Evidence | Ruled out |
|---|---|
| Layer-0 control renders correctly | The test instrument itself |
| `dynamic_rendering=0`, v1 render passes | Mask living in a struct we never touch |
| `pipelines masked=570` | Pipelines not being multiview-aware |
| 1 of 90 doubled images has `STORAGE` | Compute writes |
| **Offline: `LAYER1_DRAWN=1`** | The multiview mechanism itself |

The decisive one is the last. `tests/run-multiview-render.sh` draws through the
layer into a doubled target and reads both layers back, proving in **one
second** that draw replication works here. That turned the question from "why
doesn't multiview work" into "what else touches these images", and the usage
flags answered immediately.

*Lesson:* three live runs were spent on a question a 200-line offline test
settled instantly. When a live symptom has several candidate causes, reproduce
the mechanism offline **before** spending runs discriminating between them.

### Two broken instruments, and what they cost

Both were mistaken for real failures. Recorded because the failure modes are
generic to this kind of work:

1. **The layer-1 redirect was applied at image-view creation**, which moved
   `baseArrayLayer` on *every* doubled image. 90 images are doubled but only 18
   are ever written by a masked pass, so the rest had their reads pointed at a
   layer nothing had rendered into. Produced a black frame with an intact HUD —
   the exact signature predicted for a genuine failure. Moved to
   **descriptor-update time**, where the per-eye set is known.
2. **Validation was never actually running.** It reports through its own
   channel, so its output went to stderr and never reached `X4VR_LOG`. "Zero
   validation errors" meant only that we were grepping a file it never writes
   to. Now `X4VR_VALIDATE=1` sends it to a file.

*Rule adopted:* an instrument gets the same "include a case that must fail"
discipline as the code under test. `X4VR_MV_PRESENT_LAYER=0` now redirects
through the **same** substitution path to layer 0, which holds known-good
content — so a black layer 1 can be told apart from a broken redirect.

> **Refined at take fifteen.** This rule is necessary and not sufficient. The
> readback probe was built with a must-fail case, passed it, and still shipped
> a blind spot that made 4759 of 5994 comparisons vacuous — because the case
> could not fail for the reason that mattered. The must-fail case has to be
> able to fail *the specific way the instrument is likely to be wrong*, not
> merely to fail at all.

### Cost, and where it overshoots

| | Images | VRAM |
|---|---:|---:|
| Doubled | 90 | 565.6 MB |
| Actually per-eye | 18–21 | ~135 MB |

The permissive rule is deliberate — at `vkCreateImage` there is no framebuffer
and no render pass, so the precise question cannot be asked, and the two errors
are not symmetric (over-doubling wastes memory; under-doubling is a hard
validation error naming the attachment). But "memory is not the constraint" was
argued on a 24 GB card and does not hold on 8 GB.

Tightening is low-risk for the same reason the rule was loose: cutting too far
is caught by validation, by name. Signals available at creation time, all from
the live inventory: `D16` is the shadow atlas while main depth is `D32_SFLOAT`;
extents unrelated to the render size or a clean downscale of it;
`TRANSIENT_ATTACHMENT`-only images. Deferred until stereo works, so a tightening
regression cannot be confused with a stereo bug.

### Stage 2 (not yet done)

* The **UI is still mono**. It belongs in both eyes, but the final blit writes
  the swapchain image, which cannot take a second array layer because it is the
  thing being presented. The handoff needs `SbsCompositor`'s images to become
  one two-layer image. Costs nothing while both eyes match.
* The **exposure reductions are still masked**, and — measured at the start of
  stage 2 — that turns out to be fine. See the correction under "Reductions
  are shared": nothing in the chain is view-indexed, so both views already
  compute one shared exposure. No change needed.
* Then per-eye `K` via `gl_ViewIndex`, and per-eye camera constants so the
  deferred lighting follows.

## Stage 1, takes three to five: the frame was black for a second reason

Take two produced a black 3D scene with a perfect HUD, and the transfer
finding above explained it. It was true, and it was not enough: take three
widened 14554 transfer regions and the scene stayed exactly as black.

What followed is recorded as much for the method as the result, because four
runs went into a question that turned out to be badly posed.

### Ruled out without a run

The gate-2 redirect points reads at layer 1 only for images a masked pass
renders into. If X4 recycled a render target between a masked and an unmasked
pass, the redirect would point at a layer the unmasked pass never wrote, and
the black would be an artifact of the instrument rather than a finding.

Answered from the framebuffer logs already on disk: **20 images attached to
masked passes, 7 to unmasked, 0 in both.** No reuse, no confound. A log that
already exists is cheaper than a run, and this one had been sitting there
since the inventory pass.

> **Correction (take fifteen): this measurement was invalid and should never
> have been believed.** The framebuffer lines came from an inventory run
> recorded *before* masking existed, and image serials restart every run — so
> the two halves of the join described different images that merely shared
> numbers. The answer happened to be right (measured properly inside a live run
> at take fifteen: 26 images tracked, 20 masked, 6 unmasked, 0 mixed), but it
> was right by luck, and for five runs it was used to close a door that was
> never actually shown to be shut. The "cheaper than a run" reasoning is the
> trap: a log that already exists is only cheaper if it is a log *of the thing
> you are asking about*.

### Take four: two candidates, measured rather than argued

`pipe_masked` / `pipe_unmasked` count where a pipeline was *built*. Nothing
counted where one was *used* — neither `vkCmdBeginRenderPass` nor
`vkCmdBindPipeline` was hooked. A pipeline compiled against an unmasked but
compatible render pass and bound inside a masked one draws to a single view,
legally and silently, because the driver settled the view count at compile
time. Engines build pipelines against template passes routinely, so this was
not exotic.

The second candidate was barriers: the render pass transitions both layers,
since its attachment is the two-layer view we substitute, but an explicit
barrier between passes names `layerCount = 1` and moves layer 0 alone.

| Measured | Verdict |
|---|---|
| `binds ok=276755 MISMATCHED=0` | Dead. Every pipeline bound in a masked pass was compiled for one. |
| `barriers narrow=322363 wide=0` | Present at scale — and harmless. Validation raises no layout error on layer 1. |
| Validation, read live for the first time | No multiview, framebuffer or render-pass VUID anywhere. |

Both hooks were deliberately measurement-only. Widening a barrier changes what
the driver may do to the image, and a behaviour change does not belong in the
run whose job is to identify a cause.

Worth recording separately: the 20 `VUID-VkGraphicsPipelineCreateInfo-
renderPass-06038` errors are **X4's own** — a fragment shader reading input
attachment 20 where the subpass declares 4 — and predate the layer. Also that
`VK_KHRONOS_VALIDATION_LOG_FILENAME` finally made the oracle readable; every
earlier "validation was clean" in this document meant "we never read it".

### Take five: testing layer 1 without the instrument that kept being wrong

Every test of "is layer 1 shaded?" had run through the gate-2 descriptor
redirect — our own code, wrong twice. `X4VR_MV_MASK=2` removes it: view 0 of a
masked pass maps to array layer **1**, so the frame renders into layer 1 alone
and X4 reads layer 0 through its own untouched views. The detector becomes the
game's own read path.

Pinned down offline first (`drawn=0/1`, the exact inverse of the ordinary
case) so the live run would have one interpretation instead of two. The render
suite now asserts `LAYER0_DRAWN` as well — without it the new case would have
passed by looking identical to the old one.

**Result: black.** So the mask really does steer draws, and layer 1 really is
being shaded. The write path was never the problem.

### The contradiction, and the cause it wrongly named

> **Correction (takes 6–15).** The contradiction below is real and is still the
> most useful thing in this section — it is what eventually located the bug.
> The cause it was resolved *into*, immediately below, is **wrong**. Every
> candidate in the list that follows was hooked and then measured at zero
> (`per-eye images written layer-0-only=0`). Pixels were not arriving outside
> draws. The actual cause was that view 1's *lighting* read a descriptor
> naming a single-layer view — see the next section. Kept unedited because the
> reasoning is sound and the conclusion still false, which is the more useful
> thing to be able to recognise later.

Put the three results side by side:

| Mask | Reads | Screen |
|---|---|---|
| `0x3` | layer 0 (normal path) | normal |
| `0x3` | layer 1 (redirect) | **black** |
| `0x2` | layer 0 (normal path) | **black** |

Row 3 proves draws reach layer 1. Row 1 proves layer 0 is complete. With one
eye matrix the two layers should hold the same picture, so row 2 should be
indistinguishable from row 1 — and it is not.

The resolution is that **not every pixel arrives by a draw.** Multiview
replicates draws; the transfer fix widened image-to-image copies where both
images were doubled, and that is a strict subset of the ways an image gets
written. Everything else still lands in layer 0 alone:

* the Vulkan 1.3 spellings — `vkCmdCopyImage2`, `BlitImage2`, `ResolveImage2`
  — which were not hooked at all, leaving a hole exactly as wide as the one
  the v1 fix closed;
* `vkCmdClearDepthStencilImage`, and depth is what the whole deferred chain
  reconstructs position from, so missing layer 1 there blacks the scene by
  itself;
* `vkCmdCopyBufferToImage`, which **cannot** be widened — the source holds one
  layer's worth of bytes and asking for two reads past its end. This one has
  to be repaired by copying layer 0 across afterwards, not by widening.

One such write into a per-eye image leaves layer 1 stale, and the staleness is
invisible until something reads layer 1 — at which point it cascades through
every pass downstream and the scene goes black.

`layer0_only` now counts these and names the first dozen by image serial,
because "something writes only layer 0" is not actionable and "image #37 does,
via `vkCmdCopyBufferToImage`" is.

**And it counted zero, in every run since.** The hooks were worth adding —
those really are holes, and a later phase that re-enables the compute chain
will need them — but none of them was the black frame. The instrument built to
confirm this hypothesis is what refuted it, which is the only reason the
hypothesis cost one run instead of five.

### The rule this cost four runs to learn

The transfer finding was *true*. It was promoted to *sufficient* without
anything checking that it was, and three runs went into the gap. A confirmed
cause is not a complete one — when a fix fires 14554 times and changes nothing
visible, the fix worked and the inventory of causes was incomplete.

The corollary is the cheaper one: a result that contradicts a property the
design guarantees (here, that identical eye matrices make the layers
identical) locates the bug faster than any number of new hypotheses. Row 2 of
that table was measured in take two and its significance was not read until
take five.

## Stage 1, takes 6 to 15: the second view rasterised but was never lit

Take five ended with the write path proven and the frame still black. What
follows took nine more runs, and the cause turned out to be one line's worth
of Vulkan: **a subpass input descriptor that no longer named the same
subresource as the framebuffer attachment it read.**

### The bug

X4's deferred lighting passes — rp 30/31/32/64 — declare six attachments: one
colour, one depth, and four more read as **subpass inputs**. That is the
`S_subpassInput_AUTOMS` named in X4's own `VUID-VkGraphicsPipelineCreateInfo-
renderPass-06038` errors, which we had seen at take four, correctly identified
as X4's own, and then filed away as irrelevant.

`vkCreateFramebuffer` replaces a masked pass's attachments with two-layer array
views. Nothing replaced the matching **descriptors**, which still named X4's
single-layer views. A subpass input is view-indexed — view N reads layer N of
the attachment — so in a view-masked pass the descriptor and the attachment
disagree: view 1 is meant to read layer 1, and the descriptor describes an
image that has only layer 0.

Rasterisation is ordinary and reaches view 1 regardless. Lighting arrives
through `subpassLoad` and did not. Hence:

![layer 0](x4vr-l0.png) ![layer 1](x4vr-l1.png)

Same station, same silhouettes, pixel-aligned, and no light. Measured rather
than eyeballed: `missing=0 changed=439272 extra=0`, with the non-empty counts
of the two layers **exactly equal**. Every texel that had content differed in
value; not one was absent.

It also explains the star. With the gate-2 redirect on, everything downstream
read this unlit layer, auto-exposure chased a near-black frame, and the only
thing bright enough to survive was the sun — which is precisely what was
visible in all that black.

**The substitution was reachable only when `X4VR_MV_PRESENT_LAYER` was set.**
So every run without the redirect — all the ones that "looked normal" — had the
mismatch live. A clean screen was never evidence about layer 1, because the
screen only ever showed layer 0.

### What was eliminated, and how

| Candidate | Killed by |
|---|---|
| Transfers not replicating | Fixed, `transfers_widened=14554`, still black. True but not sufficient. |
| Pipelines compiled against unmasked passes | `binds ok=276755 MISMATCHED=0` |
| Narrow image barriers | `narrow=322363` but validation reports no layout error on layer 1 |
| Draws not reaching layer 1 | `X4VR_MV_MASK=2`: render into layer 1 alone, screen goes black |
| Unwidened writes to per-eye images | `layer0_only=0` |
| Compute writes | 42 images carry `STORAGE`; exactly one is doubled, none per-eye |
| Stale redirect cache | `redirect_stale=0` (the bug was real; it was not this one) |
| Render-target reuse across masked/unmasked passes | `writers tracked for 26 images`, 20 masked-only, 6 unmasked-only, 0 mixed |
| The redirect mechanism itself | Offline: `MASK=2 PRESENT_LAYER=1` → `drawn=0/1 sampled=1` |

### Three instruments, and what each got wrong

The diagnosis cost far more than the fix, and every overrun traces to an
instrument that was trusted before it was tested.

* **The gate-2 redirect** reported black frames for eight runs. It was sound in
  miniature and irrelevant in practice: it could only ever say "something is
  wrong downstream", never what.
* **The readback probe** hashed a fixed 64×64 patch at the origin. In X4 that
  corner is blank most frames, so **4759 of 5994 captures compared two empty
  regions** and agreed. It passed its own must-fail case because the offline
  image was exactly 64×64, making a corner copy and a full copy the same bytes.
  The suite now renders 128×128 and asserts the probe's reported extent.
* **The masked/unmasked overlap check** at take four returned "0 overlap" and
  closed a door for five runs. It read framebuffer lines from an inventory run
  recorded *before* masking existed, and image serials restart every run — so
  it answered for the wrong run with the wrong numbering. Recomputed inside a
  live run, it was still 0, but that was luck, not method.

### Rules taken from this

1. **A confirmed cause is not a complete one.** The transfer finding was true.
   Promoting it to sufficient, with nothing checking that it was, cost three
   runs.
2. **A measurement against the wrong baseline is worse than none**, because it
   closes a door. Serials restart per run; anything joining two runs by serial
   is invalid.
3. **An instrument needs a must-fail case that can actually fail for the reason
   you care about.** The probe had one and still shipped a blind spot, because
   the case could not distinguish the failure it was written to catch.
4. **When a result contradicts a property the design guarantees, the property
   is the thing to doubt.** "Identical eye matrices mean identical layers" was
   used to rule out every content-side explanation. It was false, and the run
   that showed it false was take two.
5. **Look at the image.** Twelve runs went into inferring what layer 1 held
   from aggregate numbers. One dump answered it in a glance, and the dumping
   code was smaller than most of the counters that preceded it.

## Stage 1, take sixteen: both views identical, stage 1 done

The prediction above, unchanged: no `DIFFER` on `#95`, `input attachments
fixed` non-zero. Run was `X4VR_MV=1 X4VR_MV_PROBE=1 X4VR_ONE_EYE=1
X4VR_GAMESCOPE=1`, redirect deliberately off — the screen shows layer 0 either
way, so it is not evidence; the probe is.

    43 probe captures, 0 DIFFER, 43 IDENTICAL
    24 of the 43 non-empty on both sides
    input attachments fixed = 43730

`#95` — the image that had reported `DIFFER` in every previous run — came back
identical six times, twice with genuinely non-zero content
(`7ffe7b2d2d907af0`, `0a5b0fe54fc6d9f2`).

Two non-zero captures is a small sample, so the reason it settles the question
is the correlation in the run before it, where the same image was probed under
the same conditions with the fix absent:

| | non-zero `#95` captures | `DIFFER` | `IDENTICAL` |
|---|---|---|---|
| before the fix | 5 | 5 | 0 |
| after the fix | 2 | 0 | 2 |

Before, *every* capture with content in it diverged and every all-zero one
agreed — agreement was purely a statement about emptiness. After, the all-zero
captures still agree and the ones with content agree too. The variable that
predicted the verdict stopped predicting it.

That closes stage 1: one frame, two array layers, same eye matrix, byte-identical.

### A sentinel that read like data

The same run printed `img #95 writers — masked rp [4294967295]`, where the run
before it printed `[31,30,32,38,39,64]`. Not a regression: pass serials are
only assigned when `X4VR_MV_INVENTORY` is on, and this run did not set it, so
every entry was `UINT32_MAX`.

Fixed anyway, to print `?`. Rule 2 above is about baselines, but the same edge
applies to a single number: `4294967295` reads like a render pass, `?` reads
like "not measured", and only one of those can be mistaken for a finding by
someone reading the log six weeks from now — including me.

## Stage 2: the LDR domain is mono by construction, and that is the real work

Measured before writing any of it, from the pass inventory:

* Every render pass X4 creates is **single-subpass** — `rp #N.0` and never
  `.1`, across the whole inventory.
* Every **STEREO** (masked) pass outputs only HDR formats: `[9H] [13H] [16H]
  [70H] [76H] [77H] [97H]`. Not one masked pass writes an LDR attachment.
* The passes that write the image X4 presents are `rp #0, #1, #7, #14` — the
  ones whose framebuffer attachment logs as `?` because it came from
  `vkGetSwapchainImagesKHR` rather than `vkCreateImage`. All four are
  `1 colour [44L] no-depth -> MONO (all-LDR/UI)`, and all four carry
  **`attachments=1`**.

Those three facts together settle the shape of stage 2, and it is not the shape
the plan assumed.

The chain is per-eye up to the point it stops being HDR. G-buffer and lighting
are masked and correct — stage 1 proved it. Then tonemap and UI write LDR, and
by the classification rule an LDR-output pass is MONO, so the entire LDR domain
is mono *by construction*. The eye image is LDR. There is therefore no way to
get a per-eye image on screen without changing how LDR passes work.

And they cannot be fixed the way the lighting pass was. A single-attachment
pass has no second attachment to read as a subpass input, so its source arrives
through a **descriptor sampler** — and this is the asymmetry that matters:

> **Multiview view-indexes subpass inputs automatically. It does not
> view-index samplers.** `subpassLoad` in view N reads layer N with no shader
> change. `texture(sampler2D, uv)` in view N reads layer 0, in every view,
> forever. A descriptor cannot fix it either, because a descriptor set is
> bound once for the whole pass and has no per-view form.

So stage 1's fix — point the descriptor at the array view and let multiview do
the indexing — has no analogue here. Masking the four LDR passes would give
them two layers and then write *the same left-eye image into both*.

This is the "real SPIR-V work" predicted three phases ago for the lighting
pass. The prediction was right that the work existed and wrong about where: the
lighting pass needed none, and the post/UI chain needs it all.

### The mechanism, proven offline before any run

`patch_vertex_clip` now takes an optional second matrix. With it the module
reads `gl_ViewIndex` and uses `K_left` for view 0 and `K_right` for view 1 —
one module, one draw, two eyes. Selection is arithmetic rather than a branch:

    col = colL + float(view) * (colR - colL)

`float(view)` is exact for 0 and 1, so this selects rather than blends. A
branch would mean splitting the basic block these instructions append to, and
`OpSelect`'s rules for a scalar condition with a vector result only relaxed in
SPIR-V 1.4; every op used here is core 1.0.

Two offline cases, both validation-clean:

    ok  stereo patch, same K both eyes    layers=2 drawn=1/1 identical=1
    ok  stereo patch, per-eye K differs   probe=DIFFER own=0

The second is the one that earns its place. This mechanism's most likely
silent failure is `gl_ViewIndex` always reading 0 — the module would still
compile, still draw, still fill both layers, and be quietly mono. That case
fails if it does. The first proves the patch does not corrupt a module that
has nothing to select between.

The suite's own command pool was fixed in the same commit: it had been
emitting a validation error while passing, and a suite that is not
validation-clean cannot be used to clear a patch of validation errors.

### Take eighteen: per-eye K, where DIFFER becomes the pass

Written before the run.

    X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_PROBE=1 X4VR_ONE_EYE=1 X4VR_GAMESCOPE=1

*Prediction:* `#95` reports **`DIFFER`**, and that is the **success**
condition — inverted from take sixteen, where `IDENTICAL` was the pass. The
same probe, the same image, the opposite verdict, because the frame is now
supposed to differ between the eyes.

*The screen should look unchanged.* The presented image still comes from
layer 0 alone: the whole LDR chain is mono until the tonemap patch lands. A
changed screen would mean the shear reached something it should not have.

Readings decided in advance:

| Reading | Meaning |
|---|---|
| `DIFFER`, screen unchanged | Stage 2's payload works. Proceed to the tonemap patch. |
| `IDENTICAL` | The per-view matrices are not reaching the shaders. Check `stereo=N` on the "patched vertex shader" lines. |
| `stereo=0` in the log | Classification put X4's world shaders in the UI bucket, so nothing got a right eye. |
| Screen changed | The shear leaked into a pass that should not have it — the UI, or a fullscreen post pass given `K_world`. |

Note what makes this readable at all: `#95` has been `IDENTICAL` under exactly
this instrument for two runs, with non-zero content. There is a measured
baseline to invert, which is the thing take sixteen's numbers bought.

**Result: the inversion is exact.**

    stereo: ipd=0.0640 sx=0.8890 near=0.100 -> shear m8 L=0.28448 R=-0.28448
    patched vertex shader #400 (world, per-view) [world=336 ui=64 stereo=336]
    mv probe: img #95 ... DIFFER 1975540/1982464 (99.65%)

`#95` differed on **every** non-empty capture (4 of 4) and agreed only when
both layers were empty (3 of 3) — the precise mirror of take sixteen, where
every non-empty capture agreed. 336 world shaders took the per-view patch; the
64 UI modules stayed mono, as intended. The screen was unchanged, and the
session covered cockpit, flight, leaving the seat, walking the ship interior
and the map.

### The probe also drew the map of where stereo stops

Not something the run was designed to answer, and the more useful half of it.
Counting only captures with content in them:

| Image | Extent | Differ / non-empty |
|---|---|---:|
| #92, #95, #97, #98, #99 | 1408×1408 | **all** |
| #101 | 1408×1408 | 3 / 7 |
| #122, #123 | 1408×1408 | 0 |
| #104, #105 | 352×352 | 0 |

Every one of these is a masked pass's colour attachment — the probe covers
nothing else. So the bottom four rows are passes that render into **both**
layers and put the **same picture** in each. That is the sampler boundary,
observed directly: the lit HDR chain is genuinely per-eye, and everything
downstream of the first sampled read is masked-but-mono, exactly as the
single-attachment analysis predicted.

This hands the tonemap patch its target list without a further run. `#122`,
`#123`, `#104` and `#105` are the passes whose input arrives through a
`sampler2D` reading layer 0; they are what task 4 has to fix. `#101` differing
on 3 of 7 is the one genuinely open question — worth understanding before
patching it, not after.

Recorded because the general point keeps recurring in this document: an
instrument built to answer one question answered a harder one for free, and
only because it reports *per image* rather than a single verdict per frame.
The blank-corner version of this probe could not have produced this table.

### Take seventeen: the redirect run, and why the probe does not replace it

Written before the run.

The probe compares layer 0 against layer 1 for the images it happens to
capture. That is a strong result and a narrow one. Three things it cannot say:

* **Coverage.** Take sixteen probed 13 distinct images. 21 are per-eye. The
  probe round-robins, so a short run simply does not reach all of them, and an
  image it never captured is an image it never checked.
* **Presentability.** Identical bytes in the attachment is not the same claim
  as "layer 1 survives the whole downstream chain" — post, exposure,
  `SbsCompositor`, the final blit. Every one of those runs after the point the
  probe reads.
* **The configuration stage 2 actually uses.** Stage 2 makes the layers differ
  on purpose; it runs with the reads pointed at layer 1. If that path is broken
  for some reason unrelated to shading, it should be found now, while the two
  layers are still known to be identical and any difference on screen is
  therefore a bug in the read path and nothing else.

The redirect run is also the only test here with a **demonstrated failure
mode**: before the fix it produced a black scene, ten times. A test that has
actually failed before is worth more than one that has only ever passed.

    X4VR_MV=1 X4VR_MV_PRESENT_LAYER=1 X4VR_ONE_EYE=1 X4VR_GAMESCOPE=1

*Prediction:* the scene renders normally and is indistinguishable from a
`X4VR_MV=0` run — because both layers hold the same bytes, so reading the
second one should change nothing visible. Specifically, no black scene, and the
sun no longer the only visible object.

*If it is instead black or dim:* the shading is right and something downstream
still only handles layer 0. First suspects, in order: the `SbsCompositor` blit,
the exposure reductions (which are masked and should not be — a per-eye
exposure chain fed from layer 1 could plausibly settle somewhere dark), and any
per-eye image the probe never reached. That last one is checkable without a new
run by comparing the probe's image list against the 21.

**Result: as predicted.** Everything visible, lighting correct, no black and no
dimming. Exercised well past the cockpit — the map, several menus, landing at a
station and walking around inside it.

    viewMask=0x3 doubled=91 masked=49 substituted=22 per_eye_images=19
                 redirected=857470 fallbacks=0
    binds ok=1824971 MISMATCHED=0 | layer-0-only=0 | input attachments fixed=467878

`redirected=857470` is the line that makes the run mean anything. This test
passes by *nothing looking different*, which is the same shape as the gate-1
claim corrected earlier in this document — so the first thing to check is that
the instrument was actually on. It was: 857,470 descriptor reads were pointed
at layer 1, against `redirected=0` in take sixteen. The frame on screen was
built by reading the second view, and this exact configuration produced a black
scene ten times before the fix.

Coverage is also much wider than the probe's: a station interior and the map
pull in per-eye images the cockpit never touches (`doubled` 89 → 91,
`substituted` 21 → 22), and `fallbacks=0` held across all of it.

*Not measured:* performance was reported as good, not instrumented. Stage 1
doubles fragment shading by construction, so "good" here means "no visible
regression at one-eye resolution", not a number.

### Why there is still no stage-1 perf number

The layer has had a frame-time histogram all along (`perf frame N: median …`),
so the first attempt at costing stage 1 was just to read it back. It does not
work, and the reason is worth recording because it invalidates every perf line
in the log to date:

    perf frame 2401: median 16.91 ms (59.1 fps) … worst 18.47
    perf frame 3001: median 16.83 ms (59.4 fps) … worst 17.63

That is the monitor, not the renderer. X4 asks for `FIFO`, so the frame rate is
pinned to the display refresh and a stage-1 run and an `X4VR_MV=0` run both
report ~59.4 fps whatever the GPU is actually doing. The histogram was never
wrong; it was answering a different question than the one being asked of it.

Added `X4VR_PRESENT_MODE=<n>` for measurement runs, checked against the
surface's supported modes and logged either way — a run that silently stayed
capped would produce a confident number about the refresh rate.

Correcting an overstatement made when this gap was first raised: the argument
for measuring *before* stage 2 was that the clean `X4VR_MV=0` baseline was
about to disappear. It is not — `X4VR_MV=0` stays a supported configuration
through stage 2 and beyond, so the A/B remains available later. The real
blocker was never timing, it was the vsync cap, and that is now removable.

### State

Stage 1 is complete, verified two independent ways: the two layers hold
identical bytes (readback, take sixteen), and the frame built entirely from
layer 1 is correct end to end through post, exposure, the compositor and the
present blit (take seventeen, `redirected=857470`, `fallbacks=0`).

Tagged `stage1-complete`. This is the last point at which both eyes are
supposed to match — every stage-2 change makes them differ on purpose, so a
regression after this can be bisected against a state known good on the screen
and in the bytes.

Still open from stage 1: the doubling overshoot (90 images, 565.6 MB, against
~18–21 and ~135 MB needed). Untouched deliberately, so a tightening regression
cannot be confused with a stereo bug.

Stage 2 is under way. Per-eye `K` via `gl_ViewIndex` is **done and verified
live** (take eighteen): the lit HDR chain renders two genuinely different eyes
from one draw. What remains is carrying that difference through the sampled
part of the frame, which is where it currently stops.

One piece of groundwork, not two. The exposure un-masking was checked first and
is not needed (above). What remains is the UI: `SbsCompositor`'s eye image has
to become one two-layer image, because it is what X4 renders into believing it
is the swapchain, and the composite currently copies layer 0 into *both* halves
of the real swapchain image. Until that changes there is nowhere for a second,
different eye to land.

---

## The investigation before the tonemap patch — and why the target was wrong

Two questions were left open at take eighteen: which pass writes `#122`/`#123`,
and why `#101` differs on only 3 of 7 non-empty captures. Both were answerable
from logs already on disk. Neither needed another run, and it is as well they
were asked, because the first answer invalidates the patch target the previous
section committed to.

### The join was already on disk

`X4VR_LOG` appends, so the file kept as "take eighteen" actually holds **51
runs**. Aggregating it whole produces confident nonsense: serials restart every
run, so `#101` in one segment and `#101` in another are different images. The
first pass at this analysis did exactly that and produced a table in which
`#92`/`#97`/`#98`/`#99` differed on 3–4 of hundreds of captures, contradicting
take eighteen's own finding. Segmenting by `instance created (app=X4)` and
recomputing per run reproduces the published table exactly.

**Run 47** (`t=375273`) turned out to carry *both* instruments at once — 40
probe captures and 53 `fb rp` lines. The join Q1 needed had been sitting in the
log since before the question was asked.

> **Method note.** The writers report is only meaningful with
> `X4VR_MV_INVENTORY=1`. Without it every render pass hashes to the
> `UINT32_MAX` sentinel, and because the writer list de-duplicates *by serial*,
> all of an image's writers collapse into a single `?` entry. Run 51 reports
> `img #95 writers — masked rp [?]`; run 47 reports `[31,30,32,38,39,64]` for
> the same image. The sentinel does not merely print badly, it destroys the
> multiplicity. Do not read writer counts from a run without the inventory.

### Q1: `#122` ← rp #61, `#123` ← rp #63 — and they are not the tonemap

    fb  rp #61: 1408x1408 layers=1 attachments=1 imgs=[#122] MASKED
    fb  rp #63: 1408x1408 layers=1 attachments=1 imgs=[#123] MASKED

The pass numbers were the easy half. The formats are the finding:

    img #122: 1408x1408 mips=1 fmt=76 usage=0x97 DOUBLED
    img #123: 1408x1408 mips=1 fmt=76 usage=0x97 DOUBLED

Format 76 is `VK_FORMAT_R16_SFLOAT` — **single channel, half float**. That is
not a tonemapped image. The previous section's "`#122`/`#123` are almost
certainly the tonemap/post pair, and they are the patch target" was a guess
from size and adjacency, and it is wrong.

> The format decode does not rest on recalling a Vulkan enum. The probe's
> all-zero hash for `#122` is `15bf3eca82f30383`, which is exactly FNV-1a over
> `1408×1408×2` zero bytes — confirming 2 bytes per texel independently of what
> the enum table says.

### Where the tonemap actually is

The image inventory repeats: the same render-target set is allocated several
times over (`#8–#23`, `#46–#59`, `#92–#107`, …). Aligning the blocks by their
one unmistakable member — the `mips=2`, `fmt=13` mask at offset +9 — names
every slot in the probed block:

| Serial | Format | What it is |
|---|---|---|
| `#92`, `#95`, `#96`, `#97` | 97 `RGBA16F` | HDR colour / G-buffer |
| `#93` | 126 (depth) | depth-stencil |
| `#98`, `#99` | 83 `R16G16_SFLOAT` | two-channel G-buffer (normals/motion) |
| `#100` | 50 `B8G8R8A8_SRGB` | **LDR** |
| `#101` | 13 `R8_UINT`, mips=2 | classification mask |
| `#102` | 9 `R8_UNORM` | single-channel mask |
| **`#103`** | **50 `B8G8R8A8_SRGB`** | **the tonemapped LDR image** |
| `#104`, `#105` | 97 `RGBA16F` @352² | bloom ping-pong |

And its writers:

    fb  rp #40: 1408x1408 layers=1 attachments=1 imgs=[#103]      <- no MASKED
    fb  rp #52: 1408x1408 layers=1 attachments=1 imgs=[#103]      <- no MASKED

**The tonemap output is `#103`, and its passes are unmasked.** Not by accident
— by our own rule. `classify_subpasses` marks a subpass MONO when every colour
attachment is an LDR format, and `is_ldr_format` returns true for exactly
`B8G8R8A8_SRGB`. So rp #40 and rp #52 are deliberately excluded from masking.

This changes the size of the job. `#122`/`#123` are masked: they already
replicate into both layers, and merely put the same picture in each, so for
them a fragment patch alone would have been enough. `#103` is **not masked at
all** — there is no second layer being drawn. Reaching stereo there takes two
changes, not one:

1. the pass must be masked, so the draw replicates into both layers of `#103`;
2. its fragment shader must sample `#95` per eye via `gl_ViewIndex`.

Patching only the shader on rp #61/#63, as planned, would have produced no
visible change whatsoever — and, worse, it would have *looked* like a failure
of the fragment-patch mechanism rather than a wrong target.

### Q2: `#101` is not intermittent — the instrument is too narrow

`#101` is `R8_UINT`, `mips=2`, written by two masked passes:

    fb  rp #25: 1408x1408 attachments=2 imgs=[#93,#101] MASKED
    fb  rp #27:   704x704 attachments=1 imgs=[#101]     MASKED

So hypothesis 1, "written by more than one pass", is **true as a fact and
irrelevant as an explanation**. 704 is half of 1408 and the image has exactly
two mips, so rp #27 writes mip 1 — and `probe_emit` hardcodes
`mipLevel = 0`. The second writer's output is never in the comparison.

Hypothesis 2 is the answer. Three of `#101`'s four IDENTICAL captures carry the
same hash, `b1fa160a7e480383`, which recurs across unrelated runs. Recomputing
FNV-1a over 1,982,464 bytes of a constant identifies it exactly:

    byte 0x10 -> b1fa160a7e480383   MATCH

`#101` is **cleared to 0x10** in those frames. It is a uniform buffer, not
content that happens to agree. The probe reports `(all zero)` only when every
byte is zero, so a buffer cleared to any *non-zero* constant is silently
promoted to "non-empty" and its trivial agreement is counted as evidence of
mono behaviour.

Corrected, run 51's seven `#101` captures are: **3 uniform-fill** (trivially
identical), **3 DIFFER**, and **1** genuinely identical with content — the
first capture of the run. And where it differs it differs the way a per-eye
mask should: ~97% of the image is non-empty (1,919,783 of 1,982,464) and ~8.7%
reclassifies, mostly `changed` rather than `missing`. The first-difference
column is 78, 80 and 1371 across the three — no consistent edge story, so
hypothesis 3 (edge-only parallax visibility) is not supported either.

**`#101` is not anomalous. It is per-eye like the rest of the G-buffer, and the
"3 of 7" was an artifact of the zero test.** It needs no special handling, and
nothing near it should be patched on the strength of the old reading.

> **Instrument fix this earns.** The probe should report *uniform* — all texels
> equal — not merely all-zero. Without it, every constant-cleared target reads
> as content that agrees, which is precisely the shape of evidence that would
> make a mono buffer look correctly stereo. `#122`/`#123` survive this check
> (their hashes change frame to frame, so their agreement is real), but that
> was luck, not design.

### What still is not known

Not what `#122`/`#123` *are*. Full-res single-channel `R16_SFLOAT` written as a
two-pass ping-pong, immediately after the `88×88×128` froxel volumes
(`#116`–`#121`) — consistent with an ambient-occlusion or screen-space shadow
resolve, and there is a third sibling `#124` that never appeared in a probe.
Given that globally-applied shadows wrecked the previous attempt at this mod,
the guess is not worth acting on: read the shaders bound to rp #61/#63, or dump
the images. The probe's PPM dump is currently gated on
`VK_FORMAT_R16G16B16A16_SFLOAT` and so cannot dump them today.

### The fix is a predicate split, not a format edit

`classify_unsheared` answers one question and is consumed as if it answered
two. `needs_original` uses it to decide **"does K apply to this pass's vertex
shaders?"**; `pass_is_per_eye` uses its inverse to decide **"does this pass
render into both layers?"**. Stage 1 made them the same predicate on purpose —
the comment above `pass_is_per_eye` says so — and stage 2 is where that stops
being true.

The tonemap needs opposite answers to the two questions. It is a fullscreen
post pass, so K must **not** be applied to it (shearing a fullscreen triangle
is meaningless). But it must **be masked**, so the draw replicates into both
layers of `#103`. Dropping `B8G8R8A8_SRGB` from `is_ldr_format` would get the
masking right and silently start shearing the tonemap's fullscreen triangle.

So: split the predicate. `unsheared` keeps its current definition. `per_eye`
gains the SRGB single-attachment case.

The discriminator is available at render-pass creation, where no framebuffer
and no extents exist yet. Run 47's complete MONO set:

| Count | Shape | What |
|---|---|---|
| 10 | `0 colour, depth 124` | shadow cascades — stay mono, light space |
| 7 | `1 colour [44L]` | `B8G8R8A8_UNORM` — blit/UI chain into the eye image |
| **2** | **`1 colour [50L]`** | **`B8G8R8A8_SRGB` — rp #40, rp #52 → `#103`** |

Format 50 appears nowhere else in the MONO set, so the exception is one format
wide and structurally cannot catch rp #0/#1/#7/#14.

### Why this splits into two testable halves

Masking and patching are separable, with a measured gate between them:

* **Mask only.** `#103` starts appearing in the probe table at all — today it
  cannot, because `probe_emit` only records attachments of masked passes — and
  reports **IDENTICAL**. The fullscreen triangle replicates into both layers
  and samples `#95` layer 0 in each, so identical is the *correct* result here.
  This proves the masking works with zero SPIR-V risk.
* **Then patch.** `#103` flips to **DIFFER**. Only now is the fragment patch
  under test, and it is under test alone.

Nothing changes on screen at either gate: the eye image is still written by the
mono format-44 chain, so the right eye has nothing new to show. That is
expected, and it is why the pass condition is the probe table rather than a
look at the monitor.

### The split, implemented

`X4VR_MASK_TONEMAP=1`, off by default. `classify_unsheared` is untouched and
still drives `needs_original`; a new `classify_per_eye` drives the masking, and
the two are no longer inverses. The carve-out is `subpass_is_srgb_resolve` —
single colour attachment, `B8G8R8A8_SRGB` or `R8G8B8A8_SRGB`.

The inventory log now distinguishes the two verdicts, because after the split
"MONO" no longer implies "single layer":

    rp #1.0: 1 colour [50L] no-depth -> MONO (all-LDR/UI) +MASKED

MONO is the shear verdict, `+MASKED` the replication verdict. A pass showing
both is exactly what the tonemap should look like.

Preconditions checked before writing any of it, not after: `#103` is `DOUBLED`
(so `CreateFramebuffer` can build the two-layer array view rather than taking
the `FALLBACK` path) and carries `TRANSFER_SRC` in `usage=0x97` (so the probe
can read it back at all). Both hold.

Four offline cases, over the test's own LDR second pass — which the file
already described as "exactly X4's shape, where the per-eye chain is consumed
by passes that are not themselves per-eye", i.e. the tonemap in miniature.
`X4VR_TEST_OUT_SRGB=1` switches its format, so a case can flip one thing:

    ok   tonemap masks when SRGB       rp=masked fb=masked
    ok   ...but not without the knob   rp=mono   fb=mono
    ok   ...and not for UNORM LDR      rp=mono   fb=mono
    ok   LDR pass unmasked by default  rp=mono   fb=mono

The third is load-bearing: it proves the carve-out keys on the format and not
on "LDR" generally, which is the whole difference between masking the tonemap
and masking the final blit into the presented image.

Each case asserts the classification **and** the framebuffer, because a pass
classified masked whose framebuffer is not is a fallback in disguise; and each
fails on any `FALLBACK` line.

> **Verified by mutation, including the wrong fix.** Dropping SRGB from
> `is_ldr_format` — the tempting one-liner — produces `rp=sheared fb=masked`:
> the masking looks right while K is silently applied to a fullscreen triangle.
> That is the failure mode the split exists to prevent, and the suite names it
> rather than merely going red. Deleting the uniformity flag updates likewise
> fails only "real content is not called uniform".

### Next: the live gate

Run with `X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_PROBE=1 X4VR_MASK_TONEMAP=1`.

**Pass condition:** `#103` appears in the probe table — it cannot today, since
`probe_emit` only records attachments of masked passes — and reads
**IDENTICAL**, with *no* `(uniform …)` or `(all zero)` annotation on the
captures that matter. Identical is the correct answer here: the fullscreen
triangle replicates into both layers and samples `#95` layer 0 in each. The
annotation is what distinguishes "drew the same picture twice" from "drew
nothing twice", and until this run there was no way to tell those apart.

**Also check:** `fallbacks=0` in the `mv final` line, and no new validation
errors. Nothing changes on screen; that is expected, not a failure.

---

## Take nineteen: the tonemap replicates, and the instrument earns its keep

Run: `X4VR_GAMESCOPE=1 X4VR_ONE_EYE=1 X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_PROBE=1
X4VR_MASK_TONEMAP=1 X4VR_MV_INVENTORY=1`. Cockpit, left the seat, map,
dialogue with the relief pilot to exercise the HUD. **Screen unchanged**, which
is the prediction: the chain reading `#103` is still mono, so there is nowhere
for a second eye to appear yet.

**The join needed no inference this time.** The inventory was on, so the run
named its own passes:

    rp #40.0: 1 colour [50L] no-depth -> MONO (all-LDR/UI) +MASKED
    rp #52.0: 1 colour [50L] no-depth -> MONO (all-LDR/UI) +MASKED
    fb  rp #40: 1408x1408 attachments=1 imgs=[#103] MASKED
    fb  rp #52: 1408x1408 attachments=1 imgs=[#103] MASKED
    mv final: img #103 writers — masked rp [40,52] unmasked rp []

Exactly the passes predicted from run 47, and the serials held across runs —
which was an inference then and is a measurement now. Both passes read MONO
(no K) and `+MASKED` (replicates): the split doing precisely what it was for.

**Pass condition met.** `#103` appears in the probe table for the first time —
it could not before, since `probe_emit` only records attachments of masked
passes — with **7 captures, all IDENTICAL, none uniform and none all-zero**.
Every capture is real, frame-varying content. So the pass genuinely drew the
same picture into both layers, rather than drawing nothing into both.

`fallbacks=0`, `MISMATCHED=0`, `per-eye images written layer-0-only=0`,
`image barriers wide=0`, `stale redirect entries=0`. Shader counters
`[world=336 ui=64 stereo=336]`, identical to take eighteen.

### The uniformity fix reproduced the hand analysis, live

`#101` came back annotated:

    layer0=db085906001d2323                  IDENTICAL   <- real content, agrees
    layer0=b1fa160a7e480383 (uniform 0x10)   IDENTICAL   } the clear
    layer0=b1fa160a7e480383 (uniform 0x10)   IDENTICAL   } trivially
    layer0=b1fa160a7e480383 (uniform 0x10)   IDENTICAL   } identical
    DIFFER 128914/1982464 (6.50%)
    DIFFER 150976/1982464 (7.62%)
    DIFFER 188901/1982464 (9.53%)

3 uniform, 3 differ, 1 genuine agreement — the *same structure* the FNV
arithmetic derived by hand from take eighteen, now produced by the instrument
without anyone recomputing a hash. `0x10` is even printed, matching the
constant the offline search found. That is independent confirmation the fix is
right, not merely that it compiles.

Under the old probe every one of those four IDENTICAL lines would have read as
content that agrees, and `#103`'s seven would have been indistinguishable from
a pass that never ran.

### The rest of the table, unchanged

| Image | real captures | differ | reading |
|---|---|---|---|
| `#92`, `#95`, `#97`, `#98`, `#99` | 4–5 | **all** | the per-eye HDR chain |
| `#101` | 4 | 3 | per-eye mask (1 agreement, as at take eighteen) |
| `#102` | 1 | 1 | newly captured; `R8_UNORM` mask, per-eye |
| **`#103`** | **7** | **0** | **newly captured; replicates, mono content** |
| `#104`, `#105` | 5 | 0 | bloom, mono — samples a mono source |
| `#122`, `#123` | 3 | 0 | mono, unchanged from take eighteen |

`#103` has joined the group that renders into both layers and writes the same
picture into each — the group whose members are all fixed by a fragment patch,
not by a masking change. That is the whole point of this step: it moved `#103`
from "not even replicating" to "replicating but mono", which is the state the
shader patch knows how to finish.

Tagged `stage2-tonemap-masked`.

---

## The fragment patch, built and proven offline

`patch_fragment_view_layer` in `common/x4vr_spirv.hpp`, beside
`patch_vertex_clip`. What it does to one texture:

    uniform sampler2D src       ->  uniform sampler2DArray src
    texture(src, uv)            ->  texture(src, vec3(uv, gl_ViewIndex))
    texelFetch(src, xy, 0)      ->  texelFetch(src, ivec3(xy, gl_ViewIndex), 0)

Both read forms are handled, because both appear: `texture` becomes
`OpImageSampleImplicitLod` with float coordinates, `texelFetch` becomes
`OpImage` + `OpImageFetch` with integer ones. A patch that handled one and
walked past the other would have looked correct on half of X4's shaders.

### Two decisions where the cheap version is wrong

**The image type is rebuilt, not edited.** Flipping `Arrayed` on the existing
`OpTypeImage` is a single word — and glslc gives every `sampler2D` in a module
the *same type id*, so that word promotes every other texture in the shader to
an array as well. None of those were doubled. The patch builds a private type
reachable only from the variable it was asked about.

**The variable is relocated, not retyped in place.** This was found by reading
the disassembly rather than by reasoning: in `tests/sample.frag.spv`, glslc
emits

    %src = OpVariable %_ptr_UniformConstant_11 UniformConstant
    %int = OpTypeInt 32 1
    %v2int = OpTypeVector %int 2

— the variable comes **before** the integer types the patch needs for an
`ivec3` coordinate. Declarations parked in front of it would reference types
that do not exist yet. So the variable moves to the end of the globals section
with its new type, which is also where `patch_vertex_clip` already puts things.

### What the offline gate measures

`tests/run-multiview-render.sh` grew a GPU pair. Same frame, same source, only
the shader and the bound view type differ:

| Case | `OUT1_NONZERO` | `OUT_DIFFER` |
|---|---|---|
| patched shader + 2D_ARRAY view | **1** | **1** |
| unpatched + 2D view | 0 | 0 |

The source's two layers are separated with `X4VR_MV_MASK=2` — only layer 1 is
ever drawn — rather than with the vertex patch. That matters: a sheared vertex
shader would move the *second* pass's own triangle and make the output layers
differ for a reason that has nothing to do with sampling, and the control would
stop controlling anything.

The second row is the asymmetry stated as a measurement instead of a claim. The
draw replicated (`LAYER1_DRAWN=1`) and the sample did not follow it.

A third case runs the patch with both source layers identical and requires
`OUT_DIFFER=0`, so a patch that offset the coordinate or returned the view
index itself cannot pass the first case for the wrong reason.

### The shader and the view are a pair

`sampler2DArray` bound to a 2D view is a validation error, and the patch on its
own changes nothing because a 2D view has no layer 1 to reach. Neither half is
useful alone. This is the coupling the live wiring still has to solve, and the
offline test forced it into the open by needing an explicit `aview`.

### What mutation testing said, which is not what it looked like

Four mutations, each reverted after measuring:

* **Third coordinate is a constant 0** — a perfectly valid module that samples
  a fixed layer, i.e. `gl_ViewIndex` never reaching the sample, which is the
  failure this mechanism actually risks. The must-pass case fails with
  *exactly the unpatched control's answer*. This is the one carrying the
  weight.
* **Flip `Arrayed` on the shared type** — caught only by the two-texture
  shader. Every GPU case still passes, because `sample.frag` has one texture.
  Without that shader the bug ships.
* **Accept depth samplers** — caught nothing. The shadow module is refused
  independently by the type guard, the unknown-use rule *and* the never-read
  rule; deleting all three still refuses it, because GLSL cannot express a
  shadow-sampler read that is not a `Dref` op. The case asserts the outcome and
  justifies no single guard — recorded so it is not mistaken for coverage.
* **Wave through unknown uses** — caught nothing *until the shader was fixed*.
  `sample_size.frag` originally only called `textureSize`, so it was refused as
  "declared but never read" and the case passed with the rule deleted. Giving
  it a real sample as well is what made it depend on the rule it is named
  after.

Two of four assertions were weaker than they read. Both were strengthened; the
third was documented as over-determined rather than dressed up.

### Next: which slot does the tonemap sample?

The patch takes a (set, binding). Nobody has read X4's tonemap shader, and
inventing that number is how the last two wrong turns started. So the layer now
measures it: `record_render_pass` remembers the passes masked *because* of the
SRGB carve-out — rp #40 and #52, and nothing else — and pipelines built against
them print their fragment module's sampled slots.

Committed prediction, before the run:

* **Two** `tonemap rp #…` lines, one for rp #40 and one for rp #52, naming the
  **same** module serial. They are the same pass created twice.
* The module samples **more than one** texture. A tonemap normally wants the
  HDR colour plus at least a bloom target and often an exposure/lookup buffer,
  so a single-texture answer would mean the pass is not what it is assumed to
  be.
* **None** flagged `(already array)` or `(DEPTH)`.
* The HDR source is *not* identifiable from the log alone — the slot list gives
  no image identity, so the dump plus a descriptor-side join will be needed to
  say which binding is `#95`.

If the third point is wrong the patch will refuse the shader outright, and that
is worth knowing before any wiring is written rather than after.

---

## Take twenty: X4 is bindless, and the prediction was wrong on every point

The measurement run (`X4VR_MV_INVENTORY=1` + `X4VR_DUMP_SHADERS`), 409 modules
dumped. Predicted versus measured:

| Predicted | Measured |
|---|---|
| Two lines, rp #40 and rp #52 | **Nine** lines, all rp #40; rp #52 got none |
| The same module serial in both | **Six different** modules |
| More than one sampled texture | **One** — and "samples nothing" was printed |
| None flagged `(already array)`/`(DEPTH)` | Correct, but for the wrong reason |

Four for four wrong, and each wrong answer was worth more than the predicted
one would have been.

### X4 is bindless

Every shader drawing into `#103` samples exactly one thing:

    TEX set=0 binding=7 count=53306

One array of **53,306** textures at set 0 binding 7, a separate array of 18
samplers at set 0 binding 4, and the element index arrives as a plain `uint`
from a uniform block. X4 ships debug names, so it is not a guess:

    %326 = OpAccessChain %_ptr_UniformConstant_193 %SRGB_sampler2D %325
    %327 = OpLoad %193 %326
    %330 = OpSampledImage %280 %327 %329
    %332 = OpImageSampleImplicitLod %v4float %330 %331

where `%325` is **`S_diffuse_idx`** — member 11 of
`BLOCK_BUFFER_BINDING_SLOT_DYNAMIC`, set 4 binding 0. Every material in the
game shares that one table.

Also: **one module carries both entry points**, both called `main`:

    OpEntryPoint Vertex %main "main" %IO_uv0 %_ %gl_VertexIndex
    OpEntryPoint Fragment %main_0 "main" %OUT_RT0 %IO_uv0_0

So `classify()` and `patch_vertex_clip` have been operating on modules that
also contain a fragment stage all along. Harmless — they key on the Vertex
entry point — but it means a fragment patch and a vertex patch would land in
the *same* module, each adding its own `ViewIndex` input.

### This kills the type-promotion patch for X4

`patch_fragment_view_layer` promotes a texture's `OpTypeImage` to `Arrayed 1`.
Applied here that promotes **all 53,306 entries** to `sampler2DArray`, and only
about twenty images are doubled. Every other texture in the game would then be
read at an array layer that does not exist.

It already refuses — the pointee is an `OpTypeArray`, not an image, so it bails.
That was luck as much as design, so `tests/sample_bindless.frag` now asserts it.

The offline work is not wasted: it proved *that* a sample can follow
`gl_ViewIndex`, on this driver, with a measured before/after. But the
type-promotion route is not the mechanism for X4 and should not be wired in.

### `#103` is not a tonemap output

The label came from shape — an SRGB LDR target sitting after the HDR chain —
and the shaders contradict it. rp #40's framebuffer covers `#103` and *six*
pipelines draw through it. Three carry full vertex attributes
(`SPECIAL_VERTEXLOCATION_POSITION`, `IO_uv0`, `IO_uv1`, `IO_color`) — ordinary
geometry, i.e. UI. The one that is fullscreen-shaped, `mod-0022`, has a vertex
stage using only `gl_VertexIndex` and a fragment stage doing **one** sample:

    colour = table[S_diffuse_idx] * S_diffusestr * S_diffuse_color
             * U_useralphascale * F_alphascale

That is X4's generic material shader drawing a textured quad. No exposure
curve, no bloom combine, one texture. So `#103` is the LDR **composition**
target — something copies the post-processed scene into it and UI is drawn on
top — and the actual tonemapping happens elsewhere (`#95`'s writers are rp
#31/#30/#32/#38/#39/#64).

`X4VR_MASK_TONEMAP` therefore masks more than a tonemap: it masks every
single-SRGB-attachment pass, UI geometry included. Empirically harmless — the
screen was unchanged and `MISMATCHED=0`, `layer-0-only=0`, `wide=0` — because
those UI pipelines take the unpatched modules via the existing unsheared-pass
exclusion. The knob keeps its name for continuity with the tagged runs; the
name is wrong.

**One prediction this corrects.** Task #5 expected "no HUD in the right eye if
the UI chain stays mono". The opposite should happen: the HUD is drawn *into*
`#103`, which replicates, so it will appear in both eyes at the same screen
position — which is what is wanted.

### rp #52 got no pipelines, and that is not a bug

Vulkan lets a pipeline be used with any *compatible* render pass, so X4 creates
pipelines against rp #40 and uses them with rp #52. A join from render pass to
pipeline is therefore one-way: every pipeline names a pass, but a pass does not
enumerate its pipelines. Anything built on "the pipelines of pass X" has to
tolerate that.

### The instrument lied, and the fix is regression-tested

"samples nothing" about a shader that samples, because the lister looked for
`OpTypeImage` behind the pointer and found `OpTypeArray`. Fixed, with `count`
reported. Reverting the fix makes `sample_bindless.frag` report nothing again —
the same failure, reproduced offline in a second.

That is the third instrument to have been quietly wrong here, after the probe's
zero-only test and the writer-list `?` sentinel. The pattern is consistent
enough to state as a rule: **a new instrument's first non-trivial reading should
be checked against something already known**, because all three were believed
for at least one run.

### What bindless makes possible instead

The index is a plain integer in a uniform. So per-view sampling becomes an
*integer* selection, with no type change anywhere:

    element = S_diffuse_idx + gl_ViewIndex * OFFSET

with the layer writing, at `slot + OFFSET`, an ordinary 2D view of **layer 1**
of whatever image sits in `slot`. `sampler2D` stays `sampler2D`; a 2D view of
layer 1 is an entirely ordinary view. The whole shader/view pairing problem —
`sampler2DArray` needing a matching `VK_IMAGE_VIEW_TYPE_2D_ARRAY`, and a
validation error if they disagree — simply disappears.

The property that makes it attractive: **we never need to know which slot holds
`#95`.** The layer mirrors every descriptor write into the twin region,
substituting a layer-1 view when the image is per-eye and duplicating the
descriptor otherwise. A shader reading an undoubled texture in view 1 gets the
identical texture, so the patch can be applied broadly without per-shader
targeting — and a shadow map's twin slot is the same shadow map, which defuses
the hazard that killed the earlier attempt rather than dodging it.

Feasibility, measured on this device (RADV, RX 7900 XTX):

    maxPerStageDescriptorSampledImages       = 8388606
    maxDescriptorSetSampledImages            = 8388606
    X4's table                               =   53306

About 150× headroom, so a twin region at a fixed offset fits with room to
spare. Descriptor memory roughly doubles for that binding — on the order of a
megabyte or two, against the ~566 MB the doubling already costs. Per-frame cost
is one integer multiply-add in the fragment shader, and *zero* extra work per
frame on the CPU.

Open questions before any of this is built, in order:

1. Does X4 **allocate** 53,306 descriptors, or declare the array large and bind
   fewer via `VARIABLE_DESCRIPTOR_COUNT`? This decides whether the layer must
   widen the descriptor set layout and pool, which is a far bigger intervention
   than mirroring writes.
2. How many slots does X4 actually write, and how — one bulk update or
   incremental? Determines where the twin region can live.
3. Do any shaders index the table with something that is *not* uniform across a
   draw? That would need `NonUniformEXT`, which the patch would have to
   preserve. `gl_ViewIndex` itself is draw-uniform, so it adds no
   non-uniformity of its own.
4. Is the same table used for storage images or subpass inputs anywhere? Those
   would need excluding.

None of these needs a guess: all four are answerable with counters in the
descriptor hooks the layer already has.

---

## The survey, and a fourth instrument caught being wrong

Two of the four questions never needed a run. They were answered from the 409
dumped modules, offline, in a minute:

* **Q3 — non-uniform indexing?** `NonUniform` appears in **0 of 409** modules.
  Every index comes from a uniform block, so it is draw-uniform by
  construction and the patch has no decoration to preserve. Prediction correct.
* **Q4 — storage images or subpass inputs in the same table?** No. All **333**
  declarations at set 0 binding 7 are plain 2D sampled images. Prediction
  correct.

The same scan found something not predicted at all: **there is more than one
table.**

|  n  | set | binding | dim | kind |
|---:|---|---|---|---|
| 474 | 0 | 5 | 2D | sampled |
| **333** | **0** | **7** | **2D** | **sampled** |
| 148 | 0 | 5 | Cube | sampled |
| 26 | 0 | 2 | SubpassData | subpass-input |
| 10 | 0 | 5 | 3D | sampled |
| 5 | 0 | 6 | 3D | storage |
| 4 | 0 | 0 | 2D | storage |
| 2 | 0 | 6 | Cube | storage |

Binding 5 has *more* 2D declarations than binding 7, and appears as 2D, Cube and
3D across different modules — which means different pipelines use different
descriptor set layouts for the same set and binding. So "the bindless table" is
the wrong mental model; there are several, split by dimensionality, and which
one a given shader uses is per-pipeline.

That makes the live question sharper than Q2 was: not *how many slots does X4
write* but **which binding, and which slots, receive a view of a doubled
image**. Those are the only descriptors the twin region has to mirror, and it is
knowable only at run time — an image is classified per-eye at framebuffer time,
long after creation, and the slot is X4's choice.

`X4VR_BINDLESS_SURVEY=1` reports exactly that, plus the layout counts for Q1.

### The instrument was verified against known ground truth, and failed

The offline test's sampled image *is* a doubled per-eye target, so the survey's
first reading has a right answer: one slot, one per-eye image, `img #0`. It said
so. Then the negative case — doubling off, same descriptor, same slot, image not
doubled — **passed vacuously**. The report was gated on `g_mv`, so with
`X4VR_MV=0` nothing printed at all, and the test mapped "no output" to "found
none".

Two real defects behind one weak assertion:

* `X4VR_BINDLESS_SURVEY=1` printed **nothing** unless `X4VR_MV=1` happened to be
  set too. A knob that silently does nothing is a wasted live run — and this one
  would have been wasted on the control.
* The suite could not tell "ran and found nothing" from "never ran".

Fixed by reporting independently of `X4VR_MV` and by asserting the binding line
exists before believing its zero. Mutation check: removing the per-eye
membership test now fails the negative case with `per-eye=1` where 0 is
required. Before the fix, that mutation passed.

**Four instruments, four times wrong** — the probe's zero-only test, the
writer-list `?` sentinel, the sampler lister's blindness to `OpTypeArray`, and
this. The rule earns another clause: a new instrument needs a **negative** case
whose zero is provably a measurement, not an absence of output.

## Take twenty-one: the twin region already existed

The run that was supposed to size a job discovered the job was already done by
X4, and then a second instrument — one that was only along for the ride —
answered the question three tasks ahead of it.

`X4VR_BINDLESS_SURVEY=1`, 9,934 frames, ~13 minutes of play. Scores against the
committed predictions are inline under *What that run found*; this section is
what the numbers mean.

### The prediction that was wrong was the expensive one, and it collapsed

Q1 asked whether X4 declares 53,306 descriptors and binds fewer. Predicted yes,
which would have meant widening the descriptor set layout **and** the pool —
intercepting object creation, rewriting `VkDescriptorSetLayoutCreateInfo`,
tracking pools, and hoping nothing else in X4 depended on the sizes. That was
the largest single piece of work on the roadmap.

It does not exist. `flags = 0x7`, never `0xF`; `VARIABLE_DESCRIPTOR_COUNT` never
appears; no variable-count allocation info is ever passed. X4 declares 53,306
and allocates 53,306, so the pool already backs every descriptor, and
`PARTIALLY_BOUND` makes the 42,326 it never writes legal-but-unwritten rather
than absent. The twin region is not something to build. It is free space that
has been sitting there the whole time.

This is the first wrong prediction in the project that made the work smaller.
The four before it all made it bigger.

### The prediction that was "right" was right for the wrong reason

Q2 predicted slots streaming in during play, concluding that mirroring cannot be
a startup pass. The conclusion holds. The reason is wrong, and the true reason is
harsher.

96.7% of the table is written before the first frame is ever presented — 10,622
slots — and thirteen minutes of play adds 358. The slot set is essentially
static. What moves is the contents: **217.7 million image-descriptor writes**,
~21,900 per frame, about two complete rewrites of the 10,980-slot table every
frame.

That distinction is not pedantry. A plan built on "slots appear late" would
mirror on first sight of a slot and cache it. A plan that survives ~21,900
rewrites per frame has to mirror on **every write**, forever, and its cost scales
with X4's descriptor traffic rather than with the size of the table. Had the
prediction been scored on its conclusion alone it would have counted as a hit and
the wrong design would have been built.

The measured shape, which is what the offset actually needed:

    bindings 5 and 7:  10,980 distinct slots, range 0..10,979 — dense, no holes
    of 53,306 declared: 20.6% used, 42,326 free
    191 slots hold a per-eye image: #101, #103, #600, #607

Dense and one-fifth full means `OFFSET` can be a compile-time constant. 26,653
— half the array — clears the high-water mark by 2.4×.

An unpredicted detail with real consequences: **bindings 5 and 7 hold the same
population.** Same count, same range, same 191 per-eye slots, same image
serials. X4 keeps two identical sampled-image tables. Anything that mirrors one
must mirror the other.

### The finding nobody asked for: where the difference dies

The probe was on only because it is part of the tagged baseline. Its verdicts
across the run are tabulated under *Where the difference dies*, and they settle
task #5's question without a single line of new code.

The G-buffer is already stereo. `#95`, `#97`, `#98`, `#99` differ between the
eyes in 20–21 of 23 probes; `#102` in 6 of 6; `#101` in 6–9% of its texels with
the first differing texel landing in a different place each time. The shear and
the per-view camera constants are not merely plumbed — they are producing real,
spatially-structured parallax in the lighting inputs.

And every image downstream of a sampler is identical, every time. `#103` — the
composition — is written by *masked* passes, so the layer genuinely runs them
twice with different view state, and both runs produce the same bytes. The
difference does not fade or degrade across the chain. It stops dead at the first
descriptor read.

That is the central asymmetry, measured end to end for the first time rather than
inferred from the specification. Everything before a sampler is stereo;
everything after it is mono; the index offset exists precisely to move that
boundary.

It also hands task #8 a constraint for free. `#122`/`#123` and `#104`/`#105` are
masked yet identical — rendered twice with different view state, same answer
both times — so their contents cannot depend on rasterized geometry. They are
derived purely from sampled data. That is a real narrowing of what they can be,
obtained without guessing at them, which is the standing rule for those three.

### Two more instruments wrong, and both were in the new one

The rule about verifying a new instrument's first reading paid off in one place
and was not applied in another. Binding 4 reported `SAMPLER × 18`, and the SPIR-V
survey had independently found an 18-entry sampler array at set 0 binding 4 —
two sources, two methods, same number, so the layout numbers are trustworthy.

The slot bookkeeping had no such check, and it has two defects:

* **`g_desc_slots` and `g_desc_pe` are keyed by binding alone, not by
  `(set, binding)`.** `VkWriteDescriptorSet` carries `dstSet` as a handle, not a
  set index, and the survey never recorded the mapping — so writes to binding 7
  of *any* set land in the same bucket. The stray `binding 0 — 1 distinct slots`
  in the log is the proof: nothing in the bindless tables lives at binding 0.
  The offset has to be applied to one specific table, so this must be fixed
  before the mechanism is built on these counts.
* **The per-eye slot list silently truncates.** It is capped at 360 characters
  and printed **26 of 191** entries with no minimum, maximum, or indication that
  it had stopped. All 26 fall in 10,863..10,955, which suggests the per-eye slots
  cluster at the top of the prefix — and if they do, a cheap partial mirror
  becomes possible. But 191 distinct slots cannot fit in a 93-wide window, so the
  real population is wider than anything the log shows. The clustering is
  **inferred, not measured**, and inferring it is exactly the mistake this
  project keeps paying for.

Both are cases of a reading that looks like data and is actually a sample of
unknown provenance. **Six instruments, six times wrong.** The rule gains its
third clause: an instrument that summarises a set must report the set's
**extent** — count, min, max — and must say so when it truncates. A truncated
list with no marker is indistinguishable from a complete one.

### The road the survey was not watching

Take twenty-one's slot counts are complete only if X4 writes image descriptors
exclusively through `vkUpdateDescriptorSets`. Nothing established that.
`vkUpdateDescriptorSetWithTemplate` is **core Vulkan 1.1** and X4 declares API
1.2, so using it requires no extension string — the log contains no mention of
templates, and that proves precisely nothing. The layer did not hook the call at
all.

The consequence is worse than an undercount. A mirror that hooks
`vkUpdateDescriptorSets` alone would silently miss every template write, and
view 1 would read a twin descriptor **nobody ever wrote** — an invalid
descriptor read, not a wrong colour.

Now counted rather than assumed, and the zero is printed unconditionally so "no
template line" and "templates never used" stay different readings. The template's
entries are recorded at creation because the update call carries no type or
binding information of its own; if the path turns out to be live, that record is
also exactly what mirroring it needs.

This is the seventh instrument-shaped hole, and the first one caught **before**
a run rather than after. The pattern is stable enough to state plainly: every
count this project makes is a count of what one hook saw, and the question
"which other road could carry this?" has never once been safe to skip.

## Step A built: two bugs the offline gate caught before the run

The mirror is written, and building it found two defects that a live run would
have reported as success.

### The knob that silently did nothing, again

`X4VR_BINDLESS_MIRROR=1` mirrored nothing at all unless
`X4VR_BINDLESS_SURVEY=1` happened to be set too, because the layout bookkeeping
the mirror depends on — attributing a write to a table, which is how it knows a
twin region exists — was gated on the survey flag. Identical in shape to the
defect fixed one commit earlier, where the survey printed nothing without
`X4VR_MV=1`.

It was diagnosed in a single run because the mirror's counter line prints its
zero unconditionally. `0 twin writes` in a report that exists is a measurement;
no report at all would have sent me looking at the GPU.

### The twin that read layer 0

Worse, and the reason step A exists as its own run. The layer-1 view helper was
extracted from the redirect, which builds its view at `g_mv_present_layer` —
the layer *being presented*, named by the knob that enables the redirect. The
mirror wants the layer view index 1 renders to, which is 1 by definition. Two
different concepts, and they coincide only because `g_mv_present_layer` defaults
to 0 and nothing but the redirect ever sets it.

So the mirror was writing twins that viewed **layer 0**: a perfect no-op that
would have passed every gate step A has. The frame would not have changed (P2
green), the cost would have measured correctly (P3 green), the twin region would
have proven writable (P1 green) — and step B would then have produced a
stereo-identical frame with nothing in the log to explain it, two steps and one
shader patch away from the cause.

`view_of_layer(d, device, view, layer)` now takes the layer as an argument
because the callers genuinely disagree, and the cache records which layer each
entry views so an entry for the wrong one is rebuilt like a stale one rather
than quietly handed to the other caller.

**What caught it was the GPU differential, and only that.** The mutation that
reintroduces it leaves the accounting line reading `4 of them layer-1` —
correct, and useless, because the counter cannot see *which* layer. The pair of
shaders reading slot 4 and slot 0 of the same table is what pins it: one must
come back with content and the other empty, and a mirror that retargets X4's own
slot instead of adding a twin fails the second while passing the first.

The general form, which is worth keeping: **a counter can only confirm that code
ran, never that it was right.** Every instrument in this project that was wrong
for a run was a counter trusted to mean correctness. Where the answer is a
pixel, the gate has to be a pixel.

## Take twenty-two: the mirror runs, and costs nothing measurable

Ran 2026-07-30 with `X4VR_BINDLESS_MIRROR=1` on the take-twenty-one baseline,
9,000+ frames, ~12.5 minutes. **All five predictions held** — the first clean
sweep in this project, and the reason is that the two bugs that would have
broken it were caught offline the day before.

### P1 — the twin region is writable

209,361,616 twin descriptors written. No validation error, no
`VK_ERROR_OUT_OF_POOL_MEMORY`, no skip for want of room, and the mirror never
disabled itself. The inference from `PARTIALLY_BOUND` plus a fully-allocated pool
is now a measurement.

The accounting closes exactly, which is a stronger check than the absence of
errors:

    image-descriptor writes seen : 209,361,617
    twin descriptors written     : 209,361,616
    difference                   : 1

The one unmirrored write is the set reported as `layout ? binding 0` — a set not
allocated from a table layout, which is precisely what must *not* be mirrored.
The mirror covered everything it should and nothing it should not, and the
off-by-one is the proof rather than a worry. That single stray write is also why
the `(set, binding)` attribution fix mattered: keyed by binding alone it was
invisible inside binding 0's bucket.

### P2 — the frame did not change

No image changed category. The G-buffer still differs between the eyes, and
everything downstream of a sampler is still identical:

| image | take 21 | take 22 |
| --- | --- | --- |
| `#95` `#97` | 20/23, 21/23 DIFFER | 19/21, 19/22 DIFFER |
| `#98` `#99` | 20/23 DIFFER | 20/22 DIFFER |
| `#101` | 10/23 DIFFER | 14/21 DIFFER |
| **`#103`** | **0/22** | **0/21** |
| `#104` `#105` `#122` `#123` | 0 | 0 |

Counts move with what was on screen — `#92` and `#101` were already known to be
scene-dependent — but nothing crossed the line. `#103` staying at zero is the
control working: nothing reads the twin region yet, so nothing may become
stereo, and a mirror writing where it should not would have shown up here first.

### P3 — no measurable cost

Median of the per-window medians, steady state, menu and load windows dropped:

    take 21, no mirror : 11.42 ms
    take 22, mirror    : 11.32 ms

209 million extra descriptor writes for no measurable frame time. Predicted
"under 1.5 ms"; measured nothing distinguishable from noise. That settles the
design: **per-write mirroring stays**, and the bulk `VkCopyDescriptorSet` with
its staleness problem is not needed.

**The measurement has limited power and the conclusion should be read with
that.** It compares two play sessions rather than an A/B within one — take
twenty-two contains two windows at 31 and 39 ms that take twenty-one has no
counterpart for, which is scene content, not the mirror. And this build is
GPU-bound at ~12 ms with the probe doing full-image readbacks, so CPU headroom
could hide CPU cost entirely. The claim that survives is the one the design
needs: mirroring is not *expensive enough to change the plan*. If frame time
ever becomes tight, this deserves a within-session A/B without the probe.

### P4, P5, and the blind spot

**P4:** 2 sets allocated from the table layout. Predicted under 8.

**P5:** the used prefix reached 10,984, against `OFFSET` at 26,653 — four slots
more than take twenty-one's 10,980 after a comparable session. The prefix is
remarkably stable and the margin is not in danger.

**The template path measures zero.** `0 template updates` — X4 writes image
descriptors exclusively through `vkUpdateDescriptorSets`. Take twenty-one's
counts were complete after all, which is now a measurement instead of a hope,
and the mirror is not missing a road.

### The inference that was wrong, caught by the fix

Take twenty-one showed 26 of 191 per-eye slots, all between 10,863 and 10,955,
and I wrote that they appear to cluster at the top of the prefix — noting it
would make a cheap partial mirror possible. The extent line now reports the
whole set:

    186 in 1..10983, showing 23

They span essentially the entire used prefix. There is no compact high band and
no partial mirror to be had.

Worse than wrong: it was **already contradicted by data in the same log**. Take
twenty-one's first-present line read `1=img#46` — a per-eye slot at index 1,
nowhere near the top. I had the counterexample and inferred the pattern anyway,
from a sample I knew was truncated.

That is the exact failure the extent fix was written to prevent, and it caught
its author one run later. The rule stands and gains a sharper edge: **a
truncated sample cannot support a claim about a distribution**, and the fact
that the visible entries agree with each other is not evidence — it is what
truncation looks like.

Incidentally `#95` now appears in the table at slot 10954. The design note said
the slot holding `#95` never has to be identified; it still does not, but it is
good to see it flow through the mechanism it was written about.

### Why five for five is not a sign the gates were soft

P3 could have come back at 10 ms and forced a redesign. P2 could have caught an
off-by-one in the offset corrupting X4's own textures. Those were live risks.
What made the run clean was that the two genuine bugs — the knob that did
nothing, and the twin that viewed layer 0 — were caught by the offline gate
before the game ever started. The layer-0 bug in particular would have passed
every one of P1 through P5 and surfaced two steps later with nothing in the log
to explain it.

That is the method paying for itself: the run was boring because the work was
done first.

## Step B built: the corpus found four defects that reasoning had not

The index-offset transform is written, wired, and tagged. Nothing has been run
against X4 yet — this section is what it took to get there, and it is mostly a
record of being wrong in ways only 409 real modules could show.

The transform itself is as small as predicted: declare the `ViewIndex` builtin,
rewrite one index operand into an `OpIAdd`/`OpIMul` pair. No retyping, no
coordinate extension, no following a retyped value through function bodies. The
hard parts of `patch_fragment_view_layer` genuinely do not recur, because the
index is an integer and `sampler2D` stays `sampler2D`.

Measured against the dumps rather than argued about:

    366 of 409 modules patch and validate (--target-env vulkan1.2), 0 invalid
    228 of those declare two tables and get both patched in sequence
     43 refused — all of them vertex-only, or fragment with no table to index
      0 refused that were fragment shaders with a bindless table

### Every patched module was invalid, and so was the old patch

An integer `Input` in a fragment shader must carry `Flat`
(`VUID-StandaloneSpirv-Flat-04744`). `gl_ViewIndex` is no exception — being a
builtin does not excuse it. All 352 first-attempt modules failed on this.

`patch_fragment_view_layer` — tagged `stage2-frag-patch`, "proven on the GPU" —
had the **identical defect**, and the suite had been reporting `val=OK` for it
the whole time. The reason is that the suite ran `spirv-val` with the *default*
universal target env, which does not enforce Vulkan's standalone VUIDs. The
module was invalid Vulkan and passed the check anyway; RADV happened to accept
it, which is why the GPU case went green.

That is the eighth instrument-shaped hole, and the most quietly dangerous so
far, because the instrument was a well-known external tool being called with
the wrong argument. **A validator is only as strict as its target environment**,
and "spirv-val passed" is not a claim about Vulkan unless it was told which
Vulkan.

### Three more, each found by the corpus and none by thought

* **One index type per module was too strict.** X4 indexes the same table with
  a `uint` loaded from the dynamic block in one place and a literal `%int_21` in
  another. Requiring agreement refused twelve otherwise perfect shaders. Each
  chain now carries its own type.
* **The extension check compared only the first word.** `"SPV_"` matched
  `SPV_EXT_descriptor_indexing`, which X4's bindless modules all declare, so the
  patch concluded multiview was already enabled and emitted `OpCapability
  MultiView` without the extension that permits it. The two older patches
  compare all five words; this one had been written fresh and worse.
* **Two tables meant two `ViewIndex` builtins.** 228 modules declare both
  binding 5 and binding 7, and patching them in sequence produced two variables
  decorated with the same builtin — invalid. The second call now reuses the
  first call's input.

### The gate could not see the bug that matters most

The end-to-end case — mirror on, patched shader, `OUT_DIFFER=1` — passed
immediately, and it was wrong to believe it.

`X4VR_MV_MASK` is global. It masks the *second* pass as well as the first, so
under `mask=2` output layer 0 is never rendered at all. `OUT_DIFFER=1` therefore
means "layer 0 is blank", not "the two views sampled different slots". A patch
that added `OFFSET` **unconditionally** — ignoring `gl_ViewIndex` entirely,
sending view 0 to the wrong texture and breaking the left eye — would have
produced exactly the same reading.

The fix needs no new machinery, only a second run of the same shader under
`mask=1`, where only view 0 renders and only source layer 0 has content. A
correct patch leaves view 0 on its own slot and draws; an unconditional offset
sends view 0 to a slot that was never drawn, and the frame comes back blank.

Mutation confirms the split cleanly: the unconditional-offset mutation is
**invisible** to `view 1 reads the twin` and caught **only** by `view 0 still
reads its own`.

The pattern is now familiar enough to name. Every confounded gate in this
project has had the same shape: a signal that moves for the reason you want and
also for a reason you forgot, checked in a configuration where only one of them
can happen. The cure has been the same every time — **find the second
configuration where the two explanations disagree**, and run that too.

## Take twenty-three: the mechanism works, and `#103` was the one place it did not

First run with `X4VR_BINDLESS_PATCH=1`. ~3 minutes, cockpit plus the map and
leaving the seat. No validation errors, no rejected modules, no mirror disable,
400+ fragment shaders patched. The screen looked completely normal, which is
what a correct patch should look like from view 0.

### The difference now survives a sampler

Four images that had been identical in every probe of the last two runs flipped,
and flipped completely:

| image | take 21 | take 22 | take 23 |
| --- | --- | --- | --- |
| `#104` `#105` | 0 / 21 DIFFER | 0 / 19 | **5 / 5** |
| `#122` `#123` | 0 / 18, 0 / 17 | 0 / 18 | **4 / 4** |
| **`#103`** | 0 / 22 | 0 / 21 | **0 / 8** |

The mechanism works. `#104`/`#105` and `#122`/`#123` are the images that were
*masked yet identical* — rendered twice with different view state and producing
the same bytes because everything they read came through a sampler bound to
layer 0. They now differ in every single probe. This is the first time in the
project that a **sampled** read has been per-eye, and it is what the whole
bindless index offset was built for.

It also retires the free constraint those four gave task #8: their content does
depend on the view after all, once the sampler can see it.

### Why `#103` alone did not move

Not a subtle failure, and the log names it directly.

`#103` is written by rp #40 and rp #52. The layer prints `srgb-resolve rp #40`
for exactly those subpasses that are **masked and unsheared** — masked because
they render to both layers, unsheared because they draw a fullscreen triangle
and must not get the per-eye clip matrix `K`. That is the same predicate
`needs_original()` uses, and for those pipelines the layer swaps in the twin
module kept in `g_variants.original`.

That twin was the **pristine bytes**. It existed so an unsheared pass could keep
the original geometry path — and it threw away the fragment patch along with the
shear. So every pipeline drawing into `#103` sampled view 0's slots in both
eyes, while the rest of the frame did not.

"Unsheared" is a statement about vertices. An unsheared pass still has to sample
per view. The twin now carries the fragment edit and loses only the geometry
edit.

### The gate could not have caught this, and that is the interesting part

Every offline case passes a **separate module per stage**. X4 ships **one module
carrying both entry points** — `OpEntryPoint Vertex %main "main"` and
`OpEntryPoint Fragment %main_0 "main"` — so in the game the same bytes are both
vertex-patched and fragment-patched, and the twin registered for the vertex edit
governs the fragment stage too. With separate modules that interaction cannot
occur: the fragment module is never vertex-patched, so no twin is ever
registered for it, so nothing is ever swapped.

The harness was not wrong about anything it modelled. It modelled the wrong
**shape**.

Fixed with `spirv-link`, which produces exactly X4's structure from the two test
shaders. `sample_combined.spv` is now passed as both stages, with the vertex
patch enabled so a twin is registered and the layer doing the fragment patch
itself. With a pristine twin the case reads `0/0` — the precise `#103` symptom —
and `1/1` with the fix.

Two things worth keeping from this:

**A test harness that reproduces a system's behaviour can still miss its
packaging.** Nine instrument-shaped holes so far, and this is the first where
every instrument was correct and the *model* was wrong. The question to ask of a
harness is not only "does it do what the real thing does" but "is it *built* the
way the real thing is built".

**The hardcoded bindings went too.** The layer used to patch set 0 bindings 5
and 7 by name; it now patches every declared table whose count exceeds the
mirror offset — the same bound the mirror applies before writing a twin, so the
two rules cannot drift apart. That is also what let the test use a table at
binding 0 without the layer needing to know about it.

## Take twenty-four: the fix landed, `#103` did not move, and the diagnosis was wrong

The unsheared twin now keeps the fragment patch. The binary confirms it (`.so`
built 09:15:54 against a source last touched 09:15:48), 300+ shaders were
patched, and the four images that flipped in take twenty-three flipped again:

| image | take 22 | take 23 | take 24 |
| --- | --- | --- | --- |
| `#104` `#105` | 0/19 | 5/5 DIFFER | **6/6 DIFFER** |
| `#122` `#123` | 0/18 | 4/4 DIFFER | **4/4 DIFFER** |
| `#103` | 0/21 | 0/8 | **0/7** |

`#103` did not move at all. So the take-twenty-three diagnosis — "rp #40 takes
the pristine twin, which discarded the fragment patch" — was **not** what was
holding `#103` mono, or was not the only thing.

This is worth stating plainly because the diagnosis was persuasive: the log
line `srgb-resolve rp #40` *is* printed for exactly the masked-and-unsheared
predicate that `needs_original()` uses, the twin *was* pristine, and the fix
*was* correct on its own terms — the offline case that reproduces it went from
`0/0` to `1/1`. All of that was true. It just was not the cause.

A correct fix for a real defect, validated by a test that genuinely reproduces
the symptom, is still not evidence that the defect explained the observation.
The offline case reproduced *a* bug with `#103`'s shape; it never established
that bug was `#103`'s.

### What the log could not be asked

Three questions had no answer anywhere in the run:

1. **Was `#103`'s own fragment shader patched?** The log printed
   `patched fragment shader #300` — a running count. A count is not an
   identity. `patch_fragment_index_offset` has five distinct refusal
   conditions and none of them were ever logged, so "300 patched" was
   compatible with "and the six that draw `#103` were not".
2. **Did the twin swap ever fire?** `g_variants.swapped` was incremented and
   never printed. So "the twin was wrong" and "no pipeline ever took a twin"
   were indistinguishable — and take twenty-three's whole argument rested on
   the first.
3. **What is `#103`?** The probe dumps PPMs only for images that **DIFFER**,
   and only for `R16G16B16A16_SFLOAT`. `#103` is IDENTICAL and 8-bit. The one
   image most in need of a look was the one the dumper structurally could not
   write.

Number 3 is the **tenth instrument-shaped hole**, and the sharpest of them: a
dumper gated on DIFFER can only ever show you what you already know. Every
question of the form "why is this one *not* different?" was out of its reach
by construction. It was built to confirm, not to investigate.

All three are now closed:

- `srgb-resolve` names the module **and** says `[index-offset APPLIED]` or
  `[NOT APPLIED]`.
- The summary prints modules edited **and** modules that declared a mirrorable
  table and refused, plus the twin-swap count.
- The dumper writes 8-bit BGRA/RGBA as well as half-float, dumps IDENTICAL
  images too, and takes `X4VR_MV_DUMP_IMG=N` to name one.

### The competing explanation, recorded before the run that tests it

`rp #40` and `rp #52` are the **only** two format-50 (`B8G8R8A8_SRGB`) passes
in the frame. Both write `#103`, both are `all-LDR/UI`, and six distinct
fragment modules draw into them over a run. That is the shape of a **UI pass**,
not a scene composition.

If `#103` is the HUD, then **`#103` being IDENTICAL is correct behaviour, not a
bug.** The HUD is mono; its shaders sample font atlases and icons, which are
not doubled images, so their twin slots hold verbatim duplicates and produce
identical output — exactly what the mirror is specified to do. The gate
inherited from the plan, "`#103` flips from all-IDENTICAL to all-DIFFER", would
then have been **wrong from the moment it was written**, and two runs have been
spent trying to satisfy it.

Predictions, committed before the measurement:

- **P6** — `srgb-resolve rp #40` reports `index-offset APPLIED` for every
  module it names. *(If NOT APPLIED: the patch is refusing X4's UI shaders and
  the refusal count says how widely.)*
- **P7** — the twin-swap count is greater than zero. *(If zero,
  `needs_original()` never fires for rp #40 and take twenty-three's mechanism
  was never in play at all.)*
- **P8** — the refusal count is small relative to the edited count.
- **P9** — the `#103` dump shows **HUD elements over black or over a
  non-scene background**, not the rendered world. This is the one that decides
  whether `#103` is a target at all.

P9 is the load-bearing one. P6–P8 only matter if P9 says `#103` was supposed
to differ.

### Take twenty-five: P6, P7 and P8 confirmed; P9 could not be asked

| prediction | result |
| --- | --- |
| **P6** every `srgb-resolve` module reports `APPLIED` | **held** — modules #16, #263, #265, #267, #279, #440, all APPLIED |
| **P7** twin-swap count > 0 | **held** — 1309 pipeline stages |
| **P8** refusals small relative to edits | **held, and stronger** — 420 edited, **0 refused** |
| **P9** the `#103` dump shows HUD, not the world | **undecidable from this run** |

P6–P8 together close the mechanism question completely. The index-offset patch
reaches **every** module that declares a mirrorable table — zero refusals in a
whole run — the twin swap fires 1309 times, and the specific shaders drawing
`#103` are all patched. There is no longer any version of "the patch did not
reach `#103`" left standing.

Which forces the conclusion by elimination: **`#103`'s shaders sample nothing
that is doubled.** With the patch applied, view 1 reads `index + 26653`, and
that slot holds a verbatim duplicate — so the two views produce the same bytes
because their *sources* are the same bytes. The mirror behaved exactly as
specified.

### The dump landed in the start menu

`X4VR_MV_DUMP_IMG=103` wrote its pair on the **first** probe of `#103`, which
happens before the save is loaded. The picture is the Start Menu: white and
blue text on black.

That is consistent with `#103` being the UI layer. It is equally consistent
with `#103` being the final composite — **in the start menu those are the same
picture.** The dump answered nothing, and the reason is the `static bool
dumped` one-shot: "first" was standing in for "representative", and for an
image whose first appearance is a menu, first is the one frame that cannot
discriminate.

Now sequence-numbered, capped at six pairs, so a run yields menu *and*
gameplay.

### The possibility the elimination opened

The probe copies immediately after `vkCmdEndRenderPass` of the masked pass. For
`#103` that is the end of rp #40 or rp #52 — **not** present time. So the probe
reads `#103` at an intermediate point in the frame, and "`#103` holds no
per-eye content" is a statement about that instant, not about the finished
frame.

`#103`'s usage is `0x97`, which includes `TRANSFER_DST` (0x2) and
`INPUT_ATTACHMENT` (0x80). Both are routes by which per-eye content could
arrive **without passing through a sampler**, and therefore without the
index-offset patch being involved at all. Multiview view-indexes input
attachments automatically; transfers are widened separately
(`transfers_widened=10020`). So the scene may well reach `#103` — just not at
the moment the probe looks, and not by the path the patch governs.

`#103` has two writers, rp #40 and rp #52, and the probe has been reporting
both as one number for four runs. The dump now names the pass it followed.

- **P10** — the gameplay dumps show `#103` containing HUD elements over the
  rendered world, i.e. `#103` is the final composite. *(If HUD over black, it
  is the UI layer and the take-23 gate was wrong from the start.)*
- **P11** — if P10 holds, the world in `#103` is identical between layers,
  because it arrived by a route the sampler patch does not govern.

## Take twenty-six: `#103` is the HUD, and the gate was never satisfiable

The sequence-numbered dump answered it in one picture. Six pairs, all captured
after **rp #52** (never rp #40):

| dump | contents |
| --- | --- |
| n0 | start menu, text on black |
| n1 | loading screen — full-bleed art, tip text, spinner |
| n3, n4 | **in-game HUD on pure black**: reticle, radar, shield arcs, weapon list, "Steering Mode Activated", message boxes |

**P10 refuted. P9 confirmed.** `#103` is the HUD layer, rendered over black and
composited later. There is no world in it at any point during flight.

So `#103` being IDENTICAL between layers is **correct behaviour**. The HUD is
mono by construction: its shaders sample font atlases, icon sheets and UI
textures, none of which are doubled, so their twin slots hold verbatim
duplicates and both views produce the same bytes. The mirror and the patch both
did exactly what they are specified to do.

The gate inherited from the take-19 plan — "`#103` flips from all-IDENTICAL to
all-DIFFER" — was **unsatisfiable from the moment it was written**. Three runs
(twenty-three, twenty-four, twenty-five) were spent trying to satisfy it, and
one commit (`35db59f`, the unsheared-twin fix) exists solely because of it. That
commit is still correct and still wanted — an unsheared pass does have to sample
per view — it just never had anything to do with `#103`.

The lesson is about the gate, not the runs: **a pass condition asserted before
anything is known about the target is a guess wearing the costume of a
measurement.** `#103` was chosen as the gate because it was the image the probe
kept reporting IDENTICAL — which is exactly what a correctly-mono HUD looks
like. The evidence that named it was the evidence that should have exonerated
it.

Task #13 is **done**. The mechanism is proved by `#104`, `#105`, `#122`, `#123`
flipping to DIFFER and staying there across four runs.

## The eleventh instrument-shaped hole, and it was one commit old

Take twenty-five reported `420 modules edited, 0 declared a mirrorable table and
REFUSED`, and I read that as "the patch reaches every module that needs it".

It does not. The counter was built on `list_sampled_textures`, which is
**fragment-only and 2D-only** — by design, and by its own name. X4's skybox is
a **compute** shader sampling the same 53306-entry heap as a **cube** array:

```
mod-0267:  OpEntryPoint GLCompute %main "main"
           %_arr_154_uint_53306 = OpTypeArray %154 %uint_53306
           OpName %S_samplerCube / %ray_dir / %U_output_rt_decl
```

`list` reports `TEXTURES=0` for it. So the module was never a candidate, never
counted, and the zero that looked like complete coverage was the instrument
failing to see. **This counter was added one commit earlier, specifically to
close a blind spot, and it inherited the blindness of the helper it was built
on.** Reusing a function is reusing its filters — and the filters were correct
for its own purpose, which is what made this invisible.

Fixed with `survey_image_tables`, which is stage-agnostic and dim-agnostic
because the *mirror* is: the heap holds mixed image types and the twin region
covers every image descriptor at those bindings regardless. The summary now
reports compute refusals separately, since those are refusals **by
construction** — there is no `gl_ViewIndex` in a compute shader.

`tests/skybox_cube.comp` reproduces the shape (compute + cube + large array)
rather than committing X4's module, and the suite asserts both halves: that
`survey` sees it *and* that `list` does not. 49 cases.

### What this does not yet mean

The skybox being mono is very likely **correct**. Parallax at infinity is zero:
both eyes share a rotation and differ only by a translation, so an infinitely
distant sky is genuinely identical between them. This is a case where the
missing per-view mechanism costs nothing.

That is a prediction about X4's skybox, not a proof — recorded here as one:

- **P12** — forcing the two eyes to differ everywhere *except* the skybox
  produces no visible seam or misregistration at the sky.

## Where the frame actually merges — the open question for task #5

`#103` is the HUD. The scene is `#95`/`#97`/`#98`/`#99`/`#101`, all DIFFER. So
something combines them, and the inventory says it is **not a render pass**:

- `#100` — 1408×1408, `fmt=50` (`B8G8R8A8_SRGB`), `usage=0x97`, DOUBLED — is
  created, never appears in any writer list, and is destroyed. It is the twin
  of `#103` in every declared respect and nothing renders into it.
- The final writers list contains no unmasked pass writing a full-resolution
  LDR target; the unmasked passes (42–50) are the shadow cascades.

`#103`'s usage `0x97` includes `TRANSFER_DST` (0x2) and `INPUT_ATTACHMENT`
(0x80). Both deliver content without a sampler, so the index-offset patch has
no say over either. Multiview view-indexes input attachments automatically;
transfers are widened separately.

And the layer hooks **no compute entry point at all** — not
`vkCreateComputePipelines`, not `vkCmdDispatch`. Ten of the 409 dumped modules
are compute. Whatever merges the frame, if it is a dispatch, is currently
invisible to every instrument in this project.

- **P13** — the scene/HUD merge is a compute dispatch or a blit, not a draw.

## The two paths nothing was watching

Task #14 begins by admitting a structural gap. Every instrument in this layer
is built around render passes — the writer lists, the masked/unmasked split,
the pipeline provenance, the probe. So a frame stage performed some other way
is not merely unhandled, it is **invisible**: it leaves no line anywhere.

"No render pass writes `#100`, therefore the merge is not a draw" is a sound
inference only if the non-draw calls are on record. They were not. Two hooks
now put them there:

**Transfers.** `vkCmdCopyImage` / `BlitImage` / `ResolveImage` (and the `2`
variants, and buffer→image) were already hooked to widen `layerCount`, and
already counted — `transfers_widened=10020` — but the count says how many
happened and never *which images they joined*. The inventory now records the
edge: `xfer #N -> #M via vkCmdBlitImage — R region(s), W widened`.

**Compute.** The layer hooked **no compute entry point at all**:
`vkCreateComputePipelines`, `vkCmdDispatch`, `CmdDispatchIndirect`,
`CmdDispatchBase` all went straight through the loader untouched. Now each
compute pipeline is joined to its module serial at creation, the bound pipeline
is tracked per command buffer, and dispatches are counted per shader. Nothing
about behaviour changes — this records, it does not intervene.

Both are gated on `X4VR_MV_INVENTORY=1`, and **that gate is reported**: with it
off the summary says `not measured (needs X4VR_MV_INVENTORY=1)` rather than
printing `0`. A zero that means "never looked" is the exact shape of the fourth
hole, and repeating it here — in the instruments built to close the eleventh —
would have been hard to defend.

### The harness caught a harness bug

Adding the two test cases broke eleven existing ones. The cause was in the test
program, not the layer: the edit that inserted the blit dropped the
`vkEndCommandBuffer` / `vkQueueSubmit` / `vkQueueWaitIdle` that followed it, so
the second pass was recorded and never submitted. Every sampling assertion went
to `out1=0 differ=0` at once.

That is the suite working. A silent version of this — a new instrument that
perturbs the thing it measures — is precisely what the last five sections have
been about, and here it announced itself in one run with no game involved.

`tests/noop.comp` has no resources on purpose: what is under test is the
bookkeeping chain (`CreateComputePipelines` → module serial → `CmdBindPipeline`
→ `CmdDispatch` → counter), and bindings would only add setup the assertion
does not rest on. The blit goes into a throwaway image in its own submission,
after every readback, so it cannot perturb what the suite asserts. 54 cases.

## Take twenty-seven: P13 refuted, and the sentinel that made it plausible

| prediction | result |
| --- | --- |
| **P13** the merge is a dispatch or a blit, not a draw | **refuted** |

The transfer inventory found exactly **one** image→image transfer in the entire
frame: `#95 -> #96 via vkCmdCopyImage — 1344 region(s), 1344 widened`. Nothing
blits a finished frame anywhere. Everything else was texture streaming.

The merge is a **draw**. `rp #0` and `rp #1` — `1 colour [44L] no-depth ->
MONO (all-LDR/UI)`, the only two format-44 passes — render straight into the
**swapchain images**.

### Why elimination pointed the wrong way

Those images have no serial. They arrive through `vkGetSwapchainImagesKHR`, not
`vkCreateImage`, so they never entered `g_images`, and every instrument keyed
by serial printed `?` for them: `fb rp #0: 1408x1408 layers=1 attachments=1
imgs=[?]`, twenty-four times a run, since the beginning.

The writer list is keyed by serial. So "**no render pass writes `#100`,
therefore the merge is not a draw**" was reasoning over a list that
**structurally could not contain the pass that matters**. The sentinel did not
merely omit information — it made a false conclusion look supported, and it
sent a whole run after compute and blits.

This is the `?` sentinel from the running tally — hole #2. It was found early,
described accurately, and *left*, because nothing at the time depended on
naming those images. That is the twelfth hole and the first repeat: **a known
gap, documented and unfixed, is a loaded gun.** The cost was not the run; the
run also bought the compute and transfer inventories, which are worth having.
The cost is that the wrong hypothesis looked confirmed by evidence.

Swapchain images now get a serial at `vkGetSwapchainImagesKHR`, with `usage`
set to `COLOR_ATTACHMENT` only — what the swapchain guarantees, rather than
inventing a create-info that was never called.

**This fix is not covered by the offline suite.** The headless tests have no
surface and therefore no swapchain images to register; the next live run is its
first test. Recorded rather than glossed, because "54 cases green" would
otherwise read as though this were among them.

### What compute turned out to be

Not the merge, but not nothing: 18 pipelines, 6 shaders dispatched, and the
load is real — module #362 alone ran **35,488** dispatches, #363 2,216, #364
1,107, #187 1,314.

More to the point, the honest refusal counter earned its keep on its first
outing: **8 modules declared a mirrorable table and refused the patch, 6 of
them compute.** Under the old fragment-only check that number was 0. Six
compute shaders sample the 53306-entry heap and cannot be made per-view by this
mechanism, because `gl_ViewIndex` does not exist in a dispatch. Two
*non*-compute refusals also showed up, which the old counter would have hidden
too and which are worth chasing separately.

The upload/edge split has been fixed as well: buffer→image destinations are
hundreds of distinct assets and they consumed all 256 slots of a budget shared
with the edges that mattered. They are now aggregated, and only image→image
keeps per-edge detail — of which there are single digits.

### Task #5 is now a specific question

`rp #0` and `rp #1` draw into single-layer swapchain images and are classified
`MONO`. That is where stereo dies, and it is exactly what task #5 has always
been: carry the difference through the format-44 chain to the screen. The
difference now exists upstream — `#104`, `#105`, `#122`, `#123` are genuinely
per-eye — and the last pass throws it away by construction.

- **P14** — with swapchain images registered, `fb rp #0` and `fb rp #1` name a
  real image, and that image appears in the writer list as written by an
  unmasked pass.

## Take twenty-eight: P14 half-confirmed, half-unsatisfiable, and the census that was a cap

`X4VR_GAMESCOPE=1 X4VR_ONE_EYE=1 X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_PROBE=1
X4VR_MV_INVENTORY=1 X4VR_MASK_TONEMAP=1 X4VR_BINDLESS_SURVEY=1
X4VR_BINDLESS_MIRROR=1 X4VR_BINDLESS_PATCH=1`. Cockpit run, ~4.5 minutes.

### P14, first clause: confirmed

```
img #1: 1408x1408 fmt=44 SWAPCHAIN (image 0 of 4)
...
fb  rp #0: 1408x1408 layers=1 attachments=1 imgs=[#50]
fb  rp #1: 1408x1408 layers=1 attachments=1 imgs=[#51]
```

Zero `imgs=[?]` lines in the whole run, down from twenty-four. And the count of
passes drawing into the swapchain is not two but **four**: `rp #0`, `rp #1`,
`rp #7`, `rp #14` — exactly the four sentinel lines take twenty-seven printed
and could not read. All four are `1 colour [44L] no-depth -> MONO (all-LDR/UI)`.

Take twenty-seven said "`rp #0`/`rp #1`, the only two format-44 passes". There
are seven format-44 passes; four of them reach the screen. Stating two was a
guess phrased as a count.

### P14, second clause: unsatisfiable, and I wrote it that way

> …and that image appears in the writer list as written by an unmasked pass.

It does not, and it never could. The writer list is `writers tracked for N
**doubled** images`. A swapchain image is single-layer, owned by the
presentation engine — it is never doubled, so it can never be in that list.

This is the take-19 `#103` gate again, in form if not in subject: **a gate
whose second clause was impossible from the moment it was written, because I
named an instrument without checking what it is scoped to.** Take nineteen cost
three runs to that mistake. This one cost nothing, because the first clause
carried the finding on its own — but the error was identical and I did not
catch it while writing.

### The census that was a cap — hole #13

Take twenty-seven's central claim was "exactly one image→image transfer exists
in the frame." Here is the line it rested on:

```
mv final: image transfers — 256 distinct edge(s)
mv final: xfer #95 -> #96 via vkCmdCopyImage — 1344 region(s), 1344 widened
```

`256` is not a measurement. It is `if (g_xfer_edges.size() >= 256 …) return;`
— the cap, printed as a total. Every distinct edge after the 256th was dropped
in silence, and buffer→image uploads were racing to fill those slots first.

With uploads aggregated, take twenty-eight sees **seven** image→image edges:

```
xfer #100 -> #133  —   12 region(s),    0 widened
xfer #100 -> #522  — 1106 region(s), 1106 widened
xfer #100 -> #834  —  295 region(s),  295 widened
xfer #103 -> #104  — 2118 region(s), 2118 widened   <- take 27's lone survivor
xfer #103 -> #142  —    6 region(s),    0 widened
xfer #103 -> #524  —    6 region(s),    0 widened
xfer #103 -> #987  —    6 region(s),    0 widened
```

Six of the seven were invisible. **I reported one truncated survivor as a
census, and used it as the premise of an elimination argument.**

The conclusion survives — none of the seven has a swapchain image as its
destination, so nothing copies a finished frame to the screen, and the merge is
still a draw. But it survives on evidence gathered *after* the claim, not on
the claim's own reasoning.

That is hole #13, and it is the third instance of one meta-pattern: **a bounded
instrument reporting its bound as if it were a reading.** The bound was printed
in the log, it was a suspiciously round power of two, it exactly equalled a
constant in my own source, and I read past it.

The rule this earns: *when a count could be a limit, check whether it is the
limit before it becomes a premise.*

### Every image serial in this document before this section is stale

Registering swapchain images consumed serials, so everything downstream
renumbered. Verified by anchor — the five images written by `rp #13` are
`{46,47,51,52,53}` in take twenty-seven and `{54,55,59,60,61}` in take
twenty-eight:

| take ≤27 | take ≥28 | what it is |
|---|---|---|
| `#95 -> #96` | `#103 -> #104` | the 2118-region copy |
| `#103` | `#111` | the HUD (correctly mono) |
| `#104`, `#105` | `#112`, `#113` | per-eye, 352×352 |
| `#122`, `#123` | `#130`, `#131` | per-eye |

Shift is `+4` between the two swapchains and `+8` after the second. **Do not
compare a serial across the take-27/28 boundary without applying this.**

### What the probe says now

Per-eye is holding, and broadly: `#103`, `#105`, `#106`, `#107`, `#109` all
DIFFER during gameplay (12–32%), plus `#112`/`#113` at 13–19%. `#111` — the HUD
— is IDENTICAL every cycle, which is correct and is what take twenty-six
established. Compute grew with the longer run (module #362 at 60,448
dispatches) but the shape is unchanged: 18 pipelines, 6 shaders, 8 refusals, 6
of them compute.

### Task #5: the question is now answerable, and the instrument exists

Four passes draw into a single-layer, `MONO`-classified, format-44 swapchain
image. That is where stereo dies. What has never been measured is *what those
four passes sample* — `srgb-resolve` reports shaders only for the tonemap pass,
so the four passes that actually reach the screen have never had their
fragment shaders named.

That join now exists: framebuffer creation records which passes hold a
swapchain attachment, the summary prints their fragment modules, and a pass
with no pipeline on record says so rather than printing nothing. The join is
deliberately read back in the summary rather than at pipeline-creation time, so
it does not depend on X4 creating framebuffers before pipelines — an ordering
never verified and not worth betting an instrument on.

**Offline coverage, stated plainly:** the suite (55 cases) tests only that the
join does *not* misfire — it creates render passes and framebuffers with no
surface and asserts `0 pass(es)`. The positive side cannot be reached headless.
This is the second consecutive instrument whose real test is the live run, and
it is worth saying twice rather than letting a green suite imply otherwise.

- **P15** — the four present passes name at least one fragment module, and what
  those modules sample identifies the source image: either the bindless heap
  (set 0 binding 5/7, count 53306) or a small per-pass sampler set. If it is
  the heap, the index-offset mechanism already built applies and the report
  will say `NOT APPLIED` for these modules, because they draw into a `MONO`
  pass and take the unsheared twin.

## Take twenty-nine: P15 confirmed, my reasoning for it refuted, and a bug in yesterday's fix

Same command as take twenty-eight.

### The four present passes, named

```
present rp #0  <- frag module #12  samples set 0 binding 5[53306], set 0 binding 5[53306] [index-offset APPLIED]
present rp #0  <- frag module #190 samples set 0 binding 5[53306], set 0 binding 5[53306] [index-offset APPLIED]
present rp #1  <- frag module #1..#4 samples set 0 binding 0 [index-offset NOT APPLIED]
present rp #7  <- frag module #1..#4 samples set 0 binding 0 [index-offset NOT APPLIED]
present rp #14 <- frag module #1..#4 samples set 0 binding 0 [index-offset NOT APPLIED]
```

They are two different things wearing the same classification:

* **`rp #0` is the composite.** Two fragment modules, each sampling the
  53,306-entry bindless heap **twice**, and both carrying the index-offset
  patch.
* **`rp #1`, `rp #7`, `rp #14` are plain blits** — one ordinary sampler at
  binding 0, count 1, not the heap at all. `rp #1` only ever got framebuffers on
  the pre-load swapchain.

The count also rose from 3 at first present to 4 at exit, which is what a
lazily-created composite pipeline should look like and is only visible because
the summary prints at both points.

### P15's prediction was wrong, against a fact I had already tested

> …the report will say `NOT APPLIED` for these modules, because they draw into a
> `MONO` pass and take the unsheared twin.

It says **APPLIED**, and it is right to. The unsheared twin was built in take
twenty-three to strip the *vertex* shear and **keep** the fragment edit — the
source says so at the substitution site, and `tests/run-multiview-render.sh`
has asserted `unsheared twin keeps the frag patch` ever since.

So I predicted from a half-remembered summary of my own mechanism while a
committed test asserted the opposite. Not an instrument hole — the instrument
was right and I argued past it.

### What that actually means, and it is the best possible news for task #5

`rp #0`'s fragment shaders already compute `index + gl_ViewIndex * 26653`.
`rp #0` is a non-multiview pass, so `gl_ViewIndex` is 0 there, and the
expression resolves to `index` every time.

**The composite already contains the per-view machinery. It just always
evaluates view 0.** Nothing needs to be taught to sample per-eye; the term is
compiled in and pinned to one value. That reframes task #5 from "build a
per-eye read at the end of the chain" to "give the existing term a second
value".

### The serial shift was not renumbering — it was a stale-handle bug

Take twenty-eight registered swapchain images `#50..#53`, "image 0..3 of 4".
Take twenty-nine registered `#50..#52`, "image **1**..3 of 4". Image 0 of the
second swapchain got no serial, and every later serial shifted by one.

The cause: the driver recycles `VkImage` handles across swapchains,
registration skips a handle already in `g_images`, and
`vkDestroySwapchainKHR` never removed the old swapchain's entries. So the new
swapchain's image 0 silently inherited the *dead* swapchain's serial, extent and
format. `fb rp #7`/`fb rp #14` naming `#4` — a first-swapchain image — in
second-swapchain framebuffers is the same bug showing its work.

Fixed: `SwapchainInfo` keeps its image list and `vkDestroySwapchainKHR` forgets
them, so a recycled handle registers afresh. Only entries this layer created for
a swapchain are dropped.

**Correction to the take-twenty-eight section above:** the old→new serial table
there is a one-off, not a rule. Image serials are **not stable across runs** and
never were. Re-anchor per run — the five images `rp #13` writes are a good
anchor — and never carry a serial across a run boundary.

### Task #5: the experiment needs no new code, and take seventeen already built it

`X4VR_MV_PRESENT_LAYER=1` points every image-descriptor read of a doubled image
at layer 1. Take seventeen ran it and the frame on screen was built entirely
from layer 1 — correct through post, exposure, the compositor and the present
blit, `redirected=857470`, after that exact configuration had produced a black
scene ten times before the fix.

That run passed by *nothing looking different*, because stage 1 made both
layers hold identical bytes. Stage 2 makes them differ on purpose, so the same
knob now inverts its own gate: the same configuration that had to look
unchanged must now look **changed**.

- **P16** — with `X4VR_MV=1 X4VR_STEREO=1 X4VR_MV_PRESENT_LAYER=1` and an IPD
  large enough to be unmistakable, the scene on screen is visibly displaced
  against the same run without the redirect, and `redirected` is a large
  non-zero number. If it is *not* displaced while `redirected` is large,
  the composite's heap read does not resolve to a doubled image and the
  index-offset term at `rp #0` has nothing per-eye to select.

Note that the mirror and the redirect are mutually exclusive by design and the
layer refuses the pair, so this run has `X4VR_BINDLESS_MIRROR` and
`X4VR_BINDLESS_PATCH` off. That is not a regression: it isolates the question
to the read path, which is exactly what take seventeen's failure mode covers.

## Take thirty: P16 confirmed, and the interaction hitbox that came with it

    X4VR_MV=1 X4VR_STEREO=1 X4VR_IPD=1.0 X4VR_MV_PRESENT_LAYER=1
    X4VR_ONE_EYE=1 X4VR_GAMESCOPE=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1

    stereo: ipd=1.0000 -> shear m8 L=4.44500 R=-4.44500
    mv final: redirected=125952 fallbacks=0
    mv final: binds ok=256830 MISMATCHED=0 | stale redirect entries=0

A one-metre IPD makes the two views almost disjoint — the probe reports `#107`
and `#110` at **DIFFER 100.00%**, `#109` at 96.81%, `#111` at 99.95%.

**Result, from the screen:** in the cockpit the world is noticeably shifted to
the right. P16 holds. The composite reads a doubled image, and a per-eye
difference upstream arrives on screen intact.

*(Corrected after a third run. This first said "HUD included", which was wrong:
the HUD is perfectly centred and only the world moves. The whole-frame shift was
an illusion from the one-metre IPD. Left visible rather than silently edited,
because the corrected version is the stronger result — see below.)*

That closes the last open question in the read path. Every link is now
individually verified: the views differ (probe), the composite samples the heap
(`present rp #0`), the heap read resolves to a doubled image (this run), and
layer 1 survives post, exposure, the compositor and the present blit (take
seventeen).

### The UI exclusion is exactly right, and a third run proved it

Unasked-for, and it turned out to be a cleaner result than the gate. With the
world displaced half a metre, the frame splits into two populations that behave
*differently and correctly*:

| | drawn | picks |
|---|---|---|
| cockpit HUD, notification window, map icons | centred | **aligned** |
| cockpit target markers over ships/stations | centred | **aligned** |
| on-foot aim reticle against world geometry | reticle centred, geometry displaced | offset — must click right of the chair to sit |

The split is not cockpit-versus-walking, and reading it that way would send the
next run after the wrong thing. It is **HUD-projected versus GPU-rendered**:

* Anything X4 projects on the CPU with its own camera — HUD, markers over
  ships, map icons, the notification window — is *self-consistent*. It is drawn
  where it is picked, because both ends use the same unsheared matrix. That is
  why clicking a distant station in the cockpit works: the bracket, not the
  station, is what is being clicked.
* World geometry goes through the sheared per-view matrix, so it lands
  somewhere else on screen. On foot there is no bracket to click — the reticle
  targets raw geometry — so the divergence has nothing to hide behind.

*(Mechanism inferred, not measured. What is observed is the table; that markers
are CPU-projected while geometry is not is the reading that fits it.)*

The UI half is a direct confirmation of the shear/mask predicate from task #4.
Those vertex shaders are classified `ui` and left unsheared — `patched vertex
shader #1 (ui) [world=0 ui=1 stereo=0]` — so under a one-metre IPD, the setting
most likely to expose a misclassification, **every UI element stayed exactly
where it belonged and every UI hitbox stayed with it.** The map was tested too.
That is a much stronger statement about the exclusion than any counter in the
log, and it is only visible under an IPD absurd enough to make a mistake
obvious.

The world half is X4's CPU-side picking running off its own unsheared camera,
which this layer never touches — it patches the GPU vertex shader and nothing
else. Not a bug: with a normal IPD and both eyes presented, the fused image is
centred on exactly the camera picking already uses. The offset is an artifact
of showing *one* eye displaced half a metre off-centre.

Worth keeping because it names the invariant the cursor shim has to hold:
**picking is mono and lives at the centre eye.** Any future change that shears
X4's own camera rather than the per-view copies would break input, silently,
in a way no probe or inventory would report.

### What is left of task #5 is one pass

The side-by-side path is already built end to end:

* `X4VR_SBS_LAYERS=2` gives the eye image a second array layer.
* `SbsCompositor` blits layer 0 into the left half and `right_layer` into the
  right half, and logs `<-- STEREO composite` versus `(both halves are layer 0)`
  so the two cannot be confused.
* `X4VR_SBS_RIGHT_LAYER=1` flips the right half to layer 1.
* `rp #0`'s fragment modules already carry `index + gl_ViewIndex * 26653`.

The gap is that **`rp #0` is `MONO`, so it only ever writes layer 0.** Flipping
`right_layer` today would blit a layer nothing wrote — which is why those two
knobs were split in the first place, and the split was right.

The composite cannot be singled out at `vkCreateRenderPass`: seven passes share
its exact signature (`1 colour [44L] no-depth`), and which one draws to the
screen is knowable only once a framebuffer names an image. So this has to be a
substitution at `vkCmdBeginRenderPass`, keyed on the framebuffer — the same
machinery that already substitutes 22 passes a run — with the attachment
rebuilt as a two-layer array view.

- **P17** — with the composite substituted for a `viewMask=0x3` variant and its
  attachment rebuilt as a two-layer view, `rp #0` writes both layers of the eye
  image, the compositor reports `<-- STEREO composite`, and the two halves of
  the screen differ. If the halves are identical, the substitution did not take
  and `substituted` will not have risen; if the right half is black, the pass
  was substituted but view 1 never rendered.

## Task #16: masking the composite

Two changes, and one of them had to come first.

### The eye images were never tracked

With `X4VR_SBS=1` the layer hands X4 its *own* images from
`vkGetSwapchainImagesKHR` and X4 renders the entire frame into them. Those
images were never registered: no serial, no entry in `g_images`, `?` in every
serial-keyed instrument. **The same sentinel that hid the composite for
twenty-seven takes, in a second place** — found this time by needing it rather
than by losing a run to it.

It is also a hard prerequisite. The framebuffer path refuses to build an array
view over an image it does not know is doubled, so masking the composite was
impossible while its render target was invisible. Eye images now register with
their real layer count, `doubled` set from that count rather than assumed, and
a `usage` taken from the create-info we actually wrote.

### The composite is identified by finalLayout, not by format

Seven passes share `1 colour [44L] no-depth`, so nothing about the format
singles one out. But a pass whose colour attachment ends in
`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` is *by definition* one whose output goes to
the presentation engine. That is a definition, not a heuristic, and — critically
— it is available at `vkCreateRenderPass`.

That timing is forced, not chosen. The first design here was to substitute a
two-view render pass at `vkCmdBeginRenderPass`, keyed on the framebuffer that
finally names a swapchain image. **It cannot work: a pipeline is only compatible
with render passes carrying the same viewMask**, and X4's pipelines are built
against the unmasked pass long before any framebuffer exists. A pass has to be
masked at creation or not at all. The framebuffer-time join built in take
twenty-eight remains the right instrument for *reporting*, and is the wrong one
for *deciding*.

`X4VR_MASK_PRESENT=1`, off by default, and it is the third of three knobs that
must all be on for a stereo frame:

| knob | claim |
|---|---|
| `X4VR_SBS_LAYERS=2` | the second layer exists |
| `X4VR_MASK_PRESENT=1` | something writes it |
| `X4VR_SBS_RIGHT_LAYER=1` | it reaches the right half of the screen |

They stay separate for the reason the first two always were: a run that
conflates them reports a stereo frame it has not earned.

**Offline coverage is the negative side only, and that is the side that matters
for this predicate.** Getting the `finalLayout` test backwards would mask every
pass in the game, so the suite asserts that `X4VR_MASK_PRESENT=1` classifies
*nothing* here and leaves `masked=` unchanged. The positive side needs a real
swapchain. Third consecutive change whose real test is the live run — said
plainly each time rather than letting a green suite carry an implication it
has not earned.

- **P17** — with `X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_MASK_PRESENT=1` the
  composite logs `-> STEREO (PRESENT composite)`, the eye images log `EYE …
  doubled`, `fallbacks=0` holds, and the compositor still reports `(both halves
  are layer 0)` because `X4VR_SBS_RIGHT_LAYER` is not set. Adding
  `X4VR_SBS_RIGHT_LAYER=1` then flips it to `<-- STEREO composite` and the two
  halves of the screen differ.
- **P17 is deliberately split in two.** If the right half is black or garbage
  with `RIGHT_LAYER=1`, the first run says whether the cause is upstream (the
  pass never masked, `fallbacks` non-zero) or downstream (masked and written,
  but the blit is wrong). One run cannot distinguish those and two can.

## Take thirty-one: two failures, neither of them the one under test

Run A produced two identical left halves. Two independent causes, and the run
tested nothing it was designed to test.

### 1. Wayland — and the layer had already said so

    sbs: surface reports no preferred extent (currentExtent=0xFFFFFFFF) — this is
    the Wayland WSI. … presenting the full-width image resizes the window, X4
    follows, and the split collapses to duplicating the left half. Force the X11
    driver — X4 links SDL3, so the variable is SDL_VIDEO_DRIVER=x11
    sbs: composite armed … (X4 renders full width, left half duplicated)

That message was written before this run, by this project, for exactly this
situation, and it names the fix. I composed the run command without reading the
knob list I had documented — `X4VR_X11=1` has been in `x4vr-launch.sh` since
before any of this. **The instrument was right, present, and unread.**

Nothing about the composite was exercised: without virtualization X4 renders
full width into the real swapchain and there are no eye images at all
(`EYE (image` appears zero times).

### 2. The predicate matched nothing, and the reason is the previous section

    rp #0.0: 1 colour [44L] no-depth -> MONO (all-LDR/UI)

Not `PRESENT composite`. Zero occurrences in the run.

The commit before this one said `finalLayout == PRESENT_SRC_KHR` is "a
definition, not a heuristic". **It is a definition of *one* way to present.**
The other way — leave the attachment in some working layout and transition it
with an explicit pipeline barrier — is equally legal and is what X4 does. I
wrote "definition" to mark the claim as *not* needing a check, which is exactly
the move that makes a wrong claim expensive.

Worse, the information was one line away: `g_rp_final` has recorded final
layouts all along, but only for passes already masked — so the passes I needed
it for were precisely the ones it skipped. The inventory line now prints
`final=` for every pass, so the next claim of this shape can be checked rather
than asserted.

The predicate now also accepts a single LDR colour attachment, no depth, in the
swapchain's own format, and **is labelled a heuristic in the source**. That
matches seven passes rather than one — every fullscreen LDR pass in the frame,
the composite among them. Masking all seven is a bring-up tool and is not a
shipping configuration; `fallbacks` will report any whose attachment cannot be
doubled.

### The pattern in both failures

The tally has been counting *instrument-shaped holes* — places where something
was not measured. These are the other kind: **the measurement existed and was
not read.** The Wayland message was printed and skipped; the finalLayout was
recorded and filtered out before it could contradict me.

Twelve of the thirteen holes so far were fixed by building a better instrument.
Neither of these two would have been.

- **P18** — with `X4VR_X11=1` the composite arms *virtualized* (`each eye
  1408x1408` without "X4 renders full width"), `EYE … doubled` appears four
  times, and at least one pass logs `PRESENT composite` with `+MASKED`.
  `fallbacks=0`. The screen still looks normal, because `X4VR_SBS_RIGHT_LAYER`
  is not set.

### The X11 knob broke the thing it was meant to fix

`X4VR_X11=1` fails to launch at all: **"Failed to connect to wayland socket: ."**

Two bugs, and my run command was the third.

1. The knob did `export WAYLAND_DISPLAY=""`. Empty-but-set is worse than unset:
   a client calls `wl_display_connect("")` and fails, where an unset variable
   would have fallen back to X11. The trailing `.` in the error is the empty
   socket name. Now `unset`.
2. It exported that **before gamescope launches**, and gamescope is itself a
   Wayland client of the host compositor. So it cleared the display for
   *gamescope*, not for the game, and gamescope is what died. Under gamescope
   the knob is now ignored with a message saying why — the child is already put
   on gamescope's XWayland at the `exec`, which is the right place and the only
   place.

The same empty-not-unset mistake was in the gamescope `exec` line
(`env WAYLAND_DISPLAY=`); it was harmless there only because `SDL_VIDEODRIVER=x11`
stops SDL trying Wayland at all. Changed to `env -u` so it does not depend on
that.

And the third: **`X4VR_X11=1` was never needed under gamescope.** The launcher
has forced the child onto XWayland since long before this, with a comment
saying so. I read the take-thirty-one log line, matched it to the knob list, and
did not read the twenty lines of the launcher that already handled it.

That is the second time in two runs that I acted on a documented instrument
without reading its context, and the failure mode is the same both times.

### Which surface reported no preferred extent is still unknown

`sbs: surface caps currentExtent=0xFFFFFFFF` was read as X4's. It may be
gamescope's: the layer loads into both processes, `X4VR_LOG` is one append-only
file shared by both, and that line carried nothing to tell them apart. If it was
gamescope's own host surface then nothing was wrong with the backend at all and
the split failed for a different reason entirely.

The line now carries a pid. **Not yet resolved, and recorded as unresolved**
rather than folded into either explanation.

- **P19** — the corrected Run A logs `sbs: surface caps … (pid N)` with N equal
  to the pid the injector reports for the X4 process. If the extent is
  `0xFFFFFFFF` for *that* pid, the backend really is Wayland and the SDL forcing
  is not taking; if it is a real extent for that pid and `0xFFFFFFFF` only for
  gamescope's, the split failed for some other reason and take thirty-one's
  first diagnosis was wrong.

## Take thirty-two: the predicate was dead on arrival, for a reason I had already written down

The launch works. The screen is still two identical left halves. Two findings,
one of them mine again.

### `final=2`, and the ordering that killed the fallback

```
rp #0.0: 1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI)   (494853.418)
swapchain created: 2816x1408 images>=4 format=44 -> ok           (494853.418)
```

`final=2` is `COLOR_ATTACHMENT_OPTIMAL`. X4 never leaves an attachment in
`PRESENT_SRC_KHR`, which confirms take thirty-one.

But look at the order. **`rp #0` is created before the swapchain** — same
millisecond, adjacent lines, render pass first. So the format fallback added in
take thirty-one read `g_present_format` while it was still `UNDEFINED`. It could
never have fired. `PRESENT composite` appears zero times not because the
heuristic was wrong but because it was **structurally unreachable**.

This is the third ordering assumption in this stretch, and the worst of them,
because the lesson was already recorded: the present-pass *report* was
deliberately deferred to the end-of-run summary specifically so it would not
depend on X4's creation order. I wrote that reasoning down, committed it, and
then had the *decision* depend on a creation order I never checked.

The predicate now uses no external state: single LDR colour attachment, no
depth. That cannot be beaten by ordering because it reads only the create-info
in hand.

### Candidacy is not identity, and the test said so immediately

Broadening it to all LDR passes made five offline cases fail on the first run —
including the negative-side guard written one commit earlier, which caught the
over-fire exactly as intended. That guard has now paid for itself.

The failure was in the *label*: calling every matching pass `PRESENT composite`
claims an identity the predicate cannot establish. It matches every fullscreen
LDR composition pass; one of them is the composite. The verdict strings are
restored and the flag is now `+PRESENT-CAND`, gated on the knob, with the offline
suite asserting both that the knob gates it and that a world/HDR pass is never
flagged — the half that still guards a backwards predicate.

**Unresolved, and recorded as such:** narrowing candidates to the one real
composite is not solved. Everything that would identify it — the framebuffer's
image, the swapchain format, the presented handle — is known only after the
masking decision must be made, because a pipeline is only compatible with render
passes of its own viewMask.

### The split still does not virtualize, and the pid did not settle it

```
sbs: surface caps currentExtent=4294967295x4294967295 … (pid 3341031)
sbs: composite armed … (X4 renders full width, left half duplicated)
```

One surface-caps line, one pid, and the injector only ever named pid 3341060
(bash, xkbcomp). So P19 is **still unanswered**: 3341031 is probably X4, since
arming the SBS composite happens on X4's own swapchain, but "probably" is what
the pid was added to eliminate.

The mechanism is at least clear: no preferred extent means there is nothing to
halve, so `surface_was_halved` is false, so `want_split` is false, so
`make_eye_images` is never called and there are no eye images at all. Everything
downstream of virtualization — the two-layer eye image, `EYE … doubled`, the
masked composite writing layer 1 — is unreachable until that is fixed.

- **P20** — the injector's own pid line and the surface-caps pid are compared
  directly. If they match, the surface X4 renders to genuinely reports no
  preferred extent under gamescope, and the SBS split needs a different way to
  size X4 than halving the reported extent — the launcher already claims
  `res_width`/`res_height` does this on Wayland, and that claim is now itself
  in doubt.

## The split-render sizing, fixed at the cause

Three runs failed the same way with the same log line, so this was traced end to
end rather than adjusted and retried.

### The layer was never the problem

`x4vr_CreateSwapchainKHR` decides the split by asking whether X4 requested
exactly one eye:

```c
const bool split = … && ci->imageExtent.width == X4VR_SBS_WIDTH / 2 &&
                        ci->imageExtent.height == X4VR_SBS_HEIGHT;
```

That is already lever-agnostic, and its comment already says so: X4 can be
brought to the eye size by the halved surface extent (X11) or by
`res_width`/`res_height` (Wayland). Nothing here needed changing. X4 simply
asked for 2816×1408 and the test correctly said no.

### The injector contradicted itself three lines apart

```c
// X4 only honours them when borderless is off … With borderless on it
// ignores them and sizes to the display instead.
{"res_width",  split ? eye_w : full},
{"res_height", …},
{"borderless", "true"},          // <- unconditional
```

The comment states the precondition and the next statement breaks it. So
`res_width=1408` was written into the config and then ignored, every run.

The `borderless=true` justification — *"under a correctly sized gamescope 'the
display' is already what we asked for"* — is true in one-eye mode, where
gamescope **is** the eye size, so "size to the display" and "size to one eye"
are the same request. It is false for the split render, where gamescope is
deliberately the *full* SBS width. Both modes were served by one unconditional
setting, and only one of them was ever tested.

`borderless` is now `split ? "false" : "true"`. On X11 the halved capabilities
would have carried the split without this; it is the Wayland path — this
machine's path — that depends on the config lever being honoured.

The launcher now also sets `X4VR_RES` to the eye size in SBS mode instead of
leaving the injector to infer it, so the window size and the render size are
chosen in the same place and can be read together.

### The line that stops this recurring

The split test is an exact equality, and failing it degraded silently into
"duplicate the left half" — `composite armed … (X4 renders full width)` says
*what* happened but never *why*, and three runs went past it. The layer now says
it once, with both numbers and both levers:

```
sbs: SPLIT OFF — X4 asked for 2816x1408 but one eye is 1408x1408. Nothing below
this is stereo … Two levers bring X4 to the eye size: the halved surface extent
(X11 only …) and res_width/res_height, which X4 honours ONLY when borderless is
false.
```

Any future recurrence is one line instead of a run.

### What this does not fix

`X4VR_ONE_EYE` mode is untouched and still correct — it needs `borderless=true`
and gets it. But the two modes now differ in a setting that changes how X4 sizes
itself, and only the split path has been reasoned through here; one-eye mode's
correctness rests on it being the configuration every prior run used.

**Not measured:** whether `borderless=false` costs height to a titlebar inside
gamescope. The injector's own note records `1408 -> 1385` from an observation
made outside gamescope, and gamescope does not decorate its clients — but that
is an argument, not a measurement. If it does happen, the request becomes
1408×1385, the equality fails, and the new `SPLIT OFF` line will say so with
both numbers. That is the check.

- **P21** — X4 requests 1408×1408, `SPLIT OFF` does not appear, the composite
  arms *without* "X4 renders full width", `EYE … doubled` appears four times,
  `fallbacks=0`, and at least one pass carries `+PRESENT-CAND`. The screen is
  still two identical halves, because `X4VR_SBS_RIGHT_LAYER` is unset — that is
  the run's success condition, not its failure.

## Take thirty-three: true side-by-side, and the input space that did not come with it

```
composite armed for 2816x1408 (4 images), each eye 1408x1408
    (X4 renders one eye, we own its images), eye layers=2
img #1: 1408x1408 fmt=44 layers=2 EYE (image 0 of 4) doubled
mv final: … substituted=26 per_eye_images=25 fallbacks=0
```

No `SPLIT OFF`. Every clause of P21 held. **Reported from the screen: true SBS,
acceptable framerate, lighting good.** The split render is real — X4 is drawing
one eye natively and the compositor is presenting two halves.

### The three cursor quirks are one bug

Observed:

1. **Cockpit** — the cursor traverses the full 2:1 width instead of living in one
   half, and yet brackets remain clickable "with a trail".
2. **Map** — icons are drawn correctly in *both* halves, but light up when the
   cursor is at the **centre-top of the whole screen**, not over either icon.
3. **Walking** — correct. The aim highlights the chair, in both halves.

One mapping explains all three. X4's window and input space are 1408×1408,
because that is what it was finally made to render. The display is 2816×1408 and
gamescope scales input into X4's window, so a screen point `x` reaches X4 as
`x/2`. The presented image is two copies of X4's frame side by side.

So an element X4 draws at its `x = 704` (frame centre) appears on screen at 704
*and* 2112, and activates when the physical cursor is at screen `x = 1408`.
That is exactly quirk 2, and quirk 1 is the same arithmetic with a
gamescope-drawn cursor that lives in display space and therefore is not
duplicated into either eye.

Quirk 3 is not an exception — it is the case with **no cursor at all**. X4 hides
the pointer on foot and picks through the frame centre with mouse-look, so there
is no display coordinate to mistranslate. It confirms the diagnosis rather than
complicating it: everything that picks through the frame centre is right, and
everything that picks through a screen position is off by the scale factor.

This is the invariant recorded in take thirty as *picking is mono and lives at
the centre eye*, now with its second half visible: **picking is also in X4's
eye-sized coordinate space, and nothing maps the display into it.** That is the
cursor shim's entire job, and it is the third component of this project, so far
unwritten.

Two things it has to do, and they are separate:

* **Map input.** A display coordinate must reach X4 as the corresponding point
  in one eye, so hovering an element in *either* half activates it.
* **Draw the cursor per eye.** A cursor composited in display space cannot be
  correct in a stereo image — it belongs in the eye image, before the
  duplication, so it lands in both halves at the matching place.

*(Mechanism inferred from three observations and the known geometry, not
measured. The prediction it makes is specific and cheap to falsify: the
activation point for any HUD element should be at exactly half its on-screen x
in the left copy.)*

- **P22** — an element drawn at screen `x` in the **left** half activates when
  the cursor is at screen `x/2`, and the same element in the right half (at
  `x + 1408`) activates at that same `x/2`. If instead the right half activates
  somewhere else, input is not a simple scale and the shim needs the real
  mapping measured before it is written.

### P22 confirmed, and it simplifies the shim

Tested on the map's row of top-centre icons. In SBS they appear at **1/4 and
3/4** of the screen, with nothing at the top centre — and putting the cursor at
the top centre lights up **both** copies at once.

That is `x_x4 = x_screen / 2` exactly: X4 draws the row at its own centre
(x ≈ 704), duplication puts it at 704 and 2112 of 2816, and screen 1408 maps
back to 704. Vertically nothing is off, so gamescope is scaling the axes
independently — 2816→1408 in x, 1408→1408 in y — rather than letterboxing.

**Both copies lighting together is the load-bearing detail.** It means the two
halves are not two UIs to be hit-tested separately; there is exactly **one**
element in X4's frame, drawn twice by the compositor. So the shim never has to
decide "which eye was clicked" — it only has to undo a scale, and any answer it
gives is automatically consistent across both halves.

That turns the mapping from a scale into a fold:

| | today (gamescope) | wanted |
|---|---|---|
| x | `x_screen / 2` | `x_screen mod 1408` |
| y | `y_screen` | unchanged |

With the fold, hovering the icon *where it is drawn* works in either half — 704
maps to 704, 2112 maps to 704 — and the centre-top dead spot stops being a hot
spot. The scale is not wrong so much as it is the mapping for a *squeezed* image
rather than a *duplicated* one.

Two caveats before writing it:

* This is the flatscreen-SBS ergonomic. In an HMD there is no 2D pointer over a
  side-by-side image, so the fold is a bring-up and mirror-window concern, not
  the eventual VR input path. Worth building because the whole project is being
  driven from this view, and worth not over-fitting to.
* `cursor/` is a three-line README. The shim is unwritten, and the drawing half
  of the job — compositing the pointer into the eye image *before* duplication,
  so it lands in both halves — is separate from the mapping half and does not
  follow from it.

### Correction: P22 was not confirmed. The centre is a fixed point of both models.

A retest of the map reports that **the cursor does not traverse the full 2816**
— it stays inside a centred 1408×1408 region — while the top-centre hover still
lights both icon copies.

That refutes the scale model, and it exposes the error in accepting it. Two
mappings fit the original observation:

| model | screen → X4 | cursor reaches |
|---|---|---|
| scale | `x / 2` | the full 2816 |
| translate | `x − 704` (X4's 1408 window centred in the display) | only 704…2112 |

Both send screen 1408 to X4 704. **The only point tested was the centre, which
is a fixed point of both.** I took a single measurement that could not
distinguish the candidates and wrote "P22 confirmed" over it — the same move as
the take-27 census, in a smaller frame: an observation consistent with a
hypothesis is not an observation that selects it.

The cursor confinement selects. A scaled pointer would reach the display edges;
this one is bounded by X4's own 1408-wide window, centred by gamescope. So the
mapping is a **translation**, and the "fold" proposed above is built on the
wrong model — it is retracted, not amended.

Cockpit differs because it is a different regime, not a different mapping: X4
hides the pointer and takes mouse-look there, `--force-grab-cursor` puts
gamescope in relative mode, and what spans the full width is gamescope's own
cursor rather than X4's.

The geometry this implies is worse than a wrong scale. The input region is the
centred box `704…2112`; the two drawn copies occupy `0…1408` and `1408…2816`.
**Neither copy aligns with the input region** — it straddles the middle, covering
the right half of the left copy and the left half of the right copy. Whatever the
shim does, it cannot be a coordinate tweak alone; the window, the input box and
the two copies have to be brought into agreement.

- **P23** — the discriminator, and it must use an element X4 draws **away from
  its own centre**, because every centred element agrees under both models. For
  an element at X4 `x = 352` (quarter width), the copies appear at screen 352 and
  1760. Translation predicts activation at screen **1056**; scale predicts
  **704**. One measurement, and it decides.

### P23 resolved: translation, offset 704

Measured from an annotated capture (`2812x1414`, map mode, an off-centre station
label marked in both copies and the cursor marked in red):

| | screen x |
|---|---|
| illuminated element, left copy | ≈ 614 |
| illuminated element, right copy | ≈ 2023 |
| cursor | ≈ 1319 |

The copies are `2023 − 614 = 1409` apart — the eye width, 1408, to within the
error of reading a circle off a JPEG. So the element sits at `x_x4 ≈ 614`.

| model | predicted activation | measured |
|---|---|---|
| scale (`2 × 614`) | 1228 | |
| **translate (`614 + 704`)** | **1318** | **≈ 1319** |

Translation, to a pixel. Scale is out by ninety, far outside any reading error.
`y` is unscaled (element ≈ 734, cursor ≈ 741, the difference being arrow tip
versus circle centre).

**The mapping is `x_x4 = x_screen − 704`, `y_x4 = y_screen`** — X4's 1408-wide
window centred by gamescope in the 2816-wide display, with input in window
coordinates and no scaling anywhere.

Also corrected: the confinement is **not** mode-specific. A retest finds the
cursor bounded to the centred box in the start menu and the cockpit as well.
The "cockpit is a different regime" explanation offered above was wrong — it
was built to reconcile an observation with the scale model, and once the model
went, the exception it needed went with it. What spans the full width in the
cockpit is a separate question about gamescope's own pointer under
`--force-grab-cursor`, not about this mapping.

### The geometry, now that it is known rather than assumed

```
display   0 ────────────── 1408 ────────────── 2816
copy A    |═══════════════|                          X4's frame, drawn
copy B                    |═══════════════|          X4's frame, drawn again
input                 |═══════════════|              X4's window: 704 … 2112
```

The input box straddles the seam: the right half of copy A and the left half of
copy B. **No drawn copy aligns with it**, which is why the top-centre dead zone
is the hot spot and why both copies light at once — one element, hit-tested once,
in a box that overlaps both.

That rules out fixing this in the shim by arithmetic alone. Three extents have
to agree — X4's window, its render, and the composite — and only two of them are
currently chosen together. Deferred to a fresh session rather than designed at
the end of this one; the measurement is what mattered here and it is now on
record.

## Task #18: which display server is X4 actually on

The deferral above named three extents and asked which of them to move. Reading
take thirty-three's log to answer that turned up something that changes the
question, and it was sitting in a line we had already printed:

```
[498556.958] inject  injector loaded into pid 3355800 (/usr/bin/env)
[498556.967] inject  injector loaded into pid 3355800 (…/X4 Foundations/X4)
[498558.751] layer   sbs: surface caps currentExtent=4294967295x4294967295 … (pid 3355800)
```

`env` exec'd X4, so pid 3355800 **is** X4, and X4's surface reports no preferred
extent. The launcher had passed `env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11
SDL_VIDEO_DRIVER=x11`, and the comment explaining why says, in the same file,
that clearing `WAYLAND_DISPLAY` is not enough because SDL's Wayland backend
connects to the default socket anyway. That is a failure mode we predicted in
writing and then assumed away.

Three consequences, each load-bearing:

1. **The layer's halving lever never fires.** `x4vr_GetPhysicalDeviceSurface-
   CapabilitiesKHR` returns early on `0xFFFFFFFF`. Only `res_width` +
   `borderless=false` sizes X4. Every comment reading "force the X11 driver"
   describes a fix that did not take.
2. **The buffer is the surface.** We present 2816-wide images on a surface X4
   believes is 1408 wide, so the surface *is* 2816 and X4 was never told. That
   is the disagreement, stated exactly — not three vague extents but one lie
   with a known size.
3. **gamescope may be contributing nothing.** It has no `--expose-wayland`, so
   it accepts X11 clients only. A Wayland X4 is a client of the host session,
   outside gamescope entirely, and `--force-grab-cursor` never applied to it.
   Hypothesis, with a one-run falsifier: drop gamescope and see if anything
   changes.

### The hole this exposes

`currentExtent == 0xFFFFFFFF` is a **sentinel, not a measurement**. It says the
surface declines to state a size; the Wayland WSI is merely the one we *know*
does that. The log printed "this is the Wayland WSI" as a conclusion, and for
three takes it was read as one. The same line could not distinguish *the
environment did not arrive*, *X4 overrode it*, and *SDL fell back*.

This is the fourth of its family, and the first where the instrument was not
bounded but **inferential**: not a cap reported as a census (holes #11–#13), and
not a measurement that existed and went unread (take thirty-one), but a
deduction printed in the voice of an observation. The tell is the same each
time — a line that states a fact the code was never in a position to know.

### What was built

*Layer.* Hooks on `vkCreateWaylandSurfaceKHR` / `vkCreateXcbSurfaceKHR` /
`vkCreateXlibSurfaceKHR`, recording the entry point that built each surface;
`vkDestroySurfaceKHR` forgets it, because surface handles are recycled exactly
as swapchain image handles were in take thirty. The caps line now carries
`wsi=…`, and `unknown` is a real answer it will print — a surface can come from
a platform we do not hook, and silence there would read as Wayland by
elimination. `vkCreateInstance` lists the `*_surface` extensions the process
enabled.

*The hooks are gated.* An app may choose its backend by asking for
`vkCreateWaylandSurfaceKHR` and reading the null, and X4 is on exactly that
path. Returning our own pointer for a WSI the loader does not offer would make
the game take a different branch **because we were watching**. So these live
outside `kHooks` and are returned only when the chain below resolves the name.
The differential test — same binary, layer on and off, same answers — showed the
hazard is real rather than theoretical: with only `VK_KHR_surface` enabled the
loader answers null for all three constructors and non-null for
`vkDestroySurfaceKHR`.

*Layer, read-only.* `SDL_GetCurrentVideoDriver()` and `SDL_GetHint("SDL_VIDEO_-
DRIVER")` via `dlsym`, called at surface creation (video is initialised by
then). Deliberately not an `LD_PRELOAD` interposition: gamescope is SDL2 and X4
is SDL3, they disagree about `SDL_CreateWindow`'s signature, and defining an SDL
symbol here could call one through the other's prototype. Calling an
already-resolved symbol cannot.

*Injector.* The watched environment as X4 received it, plus `/proc/self/cmdline`
for launch options outside the launcher's view; and `setenv`/`putenv`/`unsetenv`
interposed, because nothing else can see a write that happens after our
constructor ran. X4's binary contains the string `x11,wayland` next to
`SDL_VIDEO_DRIVER` and `SDL_GetCurrentVideoDriver`, so it plausibly sets its own
preference list — and in SDL the environment beats a normal-priority
`SDL_SetHint` but not an override, which is the difference between "the launcher
can win by exporting a value" and "it cannot".

### Predictions, before the run

- **P24** — X4's surface is created by `vkCreateWaylandSurfaceKHR`. If it is
  xcb or xlib instead, then `0xFFFFFFFF` is not the Wayland signature the code
  has assumed throughout, and the sizing comments are wrong in a second and
  larger way.
- **P25** — `SDL_GetCurrentVideoDriver()` returns `wayland` in X4's pid, and
  the `SDL_VIDEO_DRIVER` hint reads `x11,wayland` rather than the `x11` the
  launcher exported. If the hint reads `x11` while the driver is `wayland`,
  then x11 was *tried and failed*, which is a different repair — fix the X
  display X4 is given, not the preference.
- **P26** — the startup dump shows `SDL_VIDEO_DRIVER=x11` and
  `WAYLAND_DISPLAY=(unset)`, i.e. the environment we composed did arrive, and
  no `setenv` line follows. If instead `WAYLAND_DISPLAY` is set, the launcher's
  `env -u` is not reaching X4 and the whole diagnosis is a launcher bug.

P24 and P25 are independent: the first says what X4 ended up on, the second says
why. Confirming one does not license the other — the mistake made with P22,
where a single measurement sat at the fixed point of two models and was read as
confirming the one already believed.

### What this does not settle

X4's *believed* window size still has no direct instrument. It is inferable —
the swapchain extent X4 requests is 1408, the surface we present is 2816 — and
that inference is exactly the kind this section is about, so it is written down
as an inference. A direct reading would mean interposing `SDL_GetWindowSize`,
which is the SDL2/SDL3 signature hazard again; deferred unless P24–P26 leave the
question open.

## Take thirty-four: P25 refuted, and the instrument reproduced its own bug

Run: `X4VR_TAKE=34-wsi X4VR_LOG=/tmp/x4vr-take34.log X4VR_GAMESCOPE=1
X4VR_SBS=1 ./launch/x4vr-launch.sh` — recorded by the run itself, which is the
point of the `env: run =` line added for it.

| | predicted | measured | |
|---|---|---|---|
| P24 | surface built by `vkCreateWaylandSurfaceKHR` | **both**: Wayland *and* xcb | unscoreable as stated |
| P25 | driver `wayland`, hint `x11,wayland` | driver **`x11`**, hint **`x11`** | **refuted** |
| P26 | environment arrived intact | `SDL_VIDEO_DRIVER=x11`, `WAYLAND_DISPLAY` unset, `DISPLAY=:2` | **confirmed** |

```
wsi: instance enables VK_KHR_surface / xlib_surface / xcb_surface /
     wayland_surface / get_surface_capabilities2   (pid 3411750 = X4)
wsi: surface created via vkCreateWaylandSurfaceKHR (pid 3411750)
wsi: SDL_GetCurrentVideoDriver()=x11 hint SDL_VIDEO_DRIVER=x11 (pid 3411750)
wsi: surface created via vkCreateXcbSurfaceKHR, window 0x40002e (pid 3411750)
```

**X4 is on X11, through gamescope's XWayland, exactly as the launcher intends.**
The forcing took. The conclusion built on the previous two takes — that
`SDL_VIDEO_DRIVER=x11` had failed and X4 had fallen back to Wayland — is wrong,
and with it the claim that gamescope might be contributing nothing: `DISPLAY=:2`
is gamescope's nested X server and window `0x40002e` lives on it. gamescope
centres that 1408-wide window in its 2816-wide nested display, which is the
`704` of take thirty-three, arrived at from the other end.

The `setenv` trace also shows gamescope scrubbing `SDL_VIDEODRIVER` /
`SDL_VIDEO_DRIVER` and setting `DISPLAY=:2` for its children, and the launcher's
`env` re-adding them afterwards. The ordering the launcher depends on is real
and now visible rather than argued.

### How the instrument built to stop this did it again

The whole task existed because the layer inferred a platform from a sentinel.
The replacement logged the surface platform properly — and then printed it
**once per process**, from a `static bool first` that predates this work and was
carried through unexamined. X4 creates two surfaces a millisecond apart. The
first is Wayland; the one that presents is xcb. So the new line read
`wsi=wayland`, which is true of the surface it described and false of the game,
and I read it as the answer.

Inference from a sentinel, one level down: **a first sample is a sentinel for
the set whenever the set has more than one member.** The earlier fault was
trusting `0xFFFFFFFF` to mean Wayland; this one was trusting *surface #1* to
mean *the surface*. Both are the same move — reporting the part that was easy to
observe in the voice of the whole.

Fixed: caps are logged once per **surface**, and `swapchain created:` now names
the WSI of the surface that actually presents, because that is the only surface
any sizing argument is about.

### The lever that may never have fired

X4 enables `VK_KHR_get_surface_capabilities2`, and
`vkGetPhysicalDeviceSurfaceCapabilities2KHR` **was not hooked**. The only caps
query the layer saw in this run was the 1-variant on the Wayland probe surface;
no query on the xcb surface reached us at all, and `reporting surface width …`
— which fires whenever halving happens — appears nowhere in the log.

If X4 asks through the 2-variant, then the halving lever has never once fired,
and the "two levers bring X4 to the eye size" story repeated throughout this
code and these notes describes something that has never been tested. Every
split render to date would be the config lever alone.

Hooked now, **observation only**. Halving there would change how X4 sizes itself
on the very run meant to establish what it currently does, and the split render
took four takes to stabilise. The line says so in the log rather than leaving
the restraint implicit.

### What this does to task #19

Option A is back, and looks better than B:

* X4's window is 1408 because `res_width` says so; gamescope centres it in a
  2816 nested display; the offset is 704. Window and render agree, and the
  composite does not.
* `gamescope --force-windows-fullscreen` makes a client the size of the nested
  display. X4's window would then be 2816 — equal to the composite and to the
  display, with nothing centred and no offset.
* X4 would also *render* 2816 unless something halves it. On xcb `currentExtent`
  is the real window size, so the halving lever applies — if X4 reads it, which
  is the open question the 2-variant hook was just added to answer.

That is window = composite = display = 2816, render = 1408, and input in display
space. The remaining unknown is whether X4 lays its UI out from the window size
or the swapchain extent; if the window, the UI comes out laid for 2816 and
squashed into 1408, and A dies there instead.

- **P27** — X4 queries the xcb surface through
  `vkGetPhysicalDeviceSurfaceCapabilities2KHR`, and the reported
  `currentExtent` is 1408×1408 (the real window). If instead no caps2 line
  appears for the xcb surface, X4 never asks about the surface it presents on,
  the halving lever is unreachable by any route, and A is dead on the same
  evidence that killed the "two levers" story.

## Take thirty-five: P27 refuted, and X4 presents on a surface it should not have

Run: `X4VR_TAKE=35-caps2 X4VR_LOG=/tmp/x4vr-take35.log X4VR_GAMESCOPE=1
X4VR_SBS=1 ./launch/x4vr-launch.sh`

**P27 refuted, and more completely than predicted.** `surface caps2` appears
zero times. X4 enables `VK_KHR_get_surface_capabilities2` and never calls it.
So the halving lever is not being bypassed through the 2-variant — but it still
never fires, because the only caps query in X4's process is the 1-variant on the
**Wayland** surface, which returns `0xFFFFFFFF`. The xcb surface is never asked
about at all.

The line that changes the problem:

```
swapchain created: 1408x1408 images>=4 format=44 presentMode=1 wsi=wayland
```

X4's presenting swapchain is on the surface built by
`vkCreateWaylandSurfaceKHR`, while `SDL_GetCurrentVideoDriver()` is `x11` and an
xcb surface exists for window `0x40002e` on gamescope's `DISPLAY=:2`.

### The model this suggests

`WAYLAND_DISPLAY` is unset for X4, but `wl_display_connect(NULL)` falls back to
`wayland-0` in `XDG_RUNTIME_DIR` — the **host Plasma compositor**. gamescope was
not started with `--expose-wayland`, so its own socket is not that one. A
Wayland surface inside X4 therefore means a connection that goes around
gamescope entirely.

If that is what is happening, X4 is split across two display servers:

* **output** — a Plasma toplevel, 2816 wide because our composite made the
  buffer 2816 wide, borderless, sitting on top of gamescope's own window;
* **input** — SDL's X11 window on gamescope's nested display, 1408 wide,
  centred by gamescope in 2816, giving coordinates offset by 704 and a pointer
  confined to a centred square.

Which is exactly what has been reported from the screen since take thirty-three,
and exactly the geometry measured from `/tmp/top.jpg`. The 704 is not a
compositor placing a window inside a larger one for cosmetic reasons; it is two
different windows, in two different display servers, being mistaken for one.

It also means the split render has been "working" for the wrong reason. X4 sizes
itself from `res_width` because the surface it asks is Wayland and declines to
state an extent — and the code has been calling that "the Wayland path" while
believing X4 was on X11 for everything else.

**This is a model, not a measurement.** It is written down because it makes
sharp predictions, and the run that tests them costs one minute.

- **P28** — the Wayland and xcb surface handles are different values, and the
  swapchain's surface equals the Wayland one. If they are the *same* value, the
  driver recycled a handle, `wsi=` has been labelling the wrong object, and
  everything above collapses into an instrumentation bug.
- **P29** — X4 either never calls `vkGetPhysicalDeviceSurfaceSupportKHR` for the
  xcb surface, or calls it and is told `NO`. A `YES` it then ignored would mean
  X4 chose Wayland deliberately with a working X11 path available, which is a
  different repair.
- **P30** — there are two overlapping windows on the host: gamescope's, drawing
  nothing, and X4's on top of it. Answerable by looking at the screen, so it
  will be asked rather than inferred.

If the model holds, the repair is not arithmetic anywhere. It is to stop X4
having two windows: either deny it the host Wayland socket so the xcb surface is
the only one it can present on, or give gamescope `--expose-wayland` so its
Wayland connection lands on gamescope instead of Plasma. Both are one-line
changes to the launcher and both are testable in a single run.

## Take thirty-six: P28 and P29 confirmed, P30 refuted

Run: `X4VR_TAKE=36-handles X4VR_LOG=/tmp/x4vr-take36.log X4VR_GAMESCOPE=1
X4VR_SBS=1 ./launch/x4vr-launch.sh`

```
wsi: surface 0x3edd2040 created via vkCreateWaylandSurfaceKHR (pid 3418032)
wsi: SDL_GetCurrentVideoDriver()=x11 hint SDL_VIDEO_DRIVER=x11 (pid 3418032)
wsi: surface 0x3befb820 created via vkCreateXcbSurfaceKHR, window 0x40002e
wsi: present support? surface 0x3edd2040 (wayland) family 0..4 -> YES YES NO NO NO
swapchain created: 1408x1408 … surface 0x3edd2040 wsi=wayland -> ok
wsi: surface 0x3befb820 destroyed (was xcb)   ← at shutdown, never used
```

- **P28 confirmed.** The handles are distinct — `0x3edd2040` Wayland,
  `0x3befb820` xcb — and both are still correctly labelled when destroyed at
  shutdown. No recycling, no mislabelling. X4 genuinely presents on Wayland.
- **P29 confirmed, first branch.** X4 asks `vkGetPhysicalDeviceSurfaceSupportKHR`
  for the Wayland surface across all five queue families and **never asks about
  the xcb surface at all**. The X11 path was not tested and rejected; it was
  created and abandoned.
- **P30 refuted.** Reported from the screen: one window. Alt-tab and exposé show
  nothing else. So "two toplevels in two display servers" is wrong, and the
  model built on it in take thirty-five does not survive.

That is three takes in a row where a coherent story was promoted ahead of the
measurement and did not survive it. The pattern is worth naming: each story was
built to explain the *previous* measurement, and each one reached past it to
predict something it had no standing to predict.

### The thing that was never measured

Every remaining explanation turns on **which socket** the Wayland connection
reached, and the candidates sit side by side in `$XDG_RUNTIME_DIR`:

```
wayland-0      the host Plasma session
gamescope-0    gamescope's own compositor
```

`wl_display_connect(NULL)` uses `$WAYLAND_DISPLAY`, falling back to `wayland-0`.
The launcher runs the game under `env -u WAYLAND_DISPLAY` — which strips exactly
the variable that would have named `gamescope-0`. So the fallback is Plasma, by
construction, and the flag written to force X11 is what routes output away from
gamescope.

That is an argument, and arguments have lost three takes running. `connect(2)`
is now interposed and logs the socket path for any Wayland, gamescope or X11
socket; `XDG_RUNTIME_DIR` joins the watched environment.

- **P31** — X4 connects to `$XDG_RUNTIME_DIR/wayland-0`. If it connects to
  `gamescope-0` instead, then X4 is a Wayland client *of gamescope*, there is
  genuinely one window, and the 704 is gamescope compositing two surfaces from
  the same client — its Wayland output and its X11 input window — at different
  sizes.
- **P32** — X4 also connects to an X11 socket (`@/tmp/.X11-unix/X2`), since it
  has an xcb surface for window `0x40002e` on `DISPLAY=:2`. If it does not, that
  window came from somewhere else and the xcb surface is stranger than it looks.

P31 is the one that decides the repair. `wayland-0` means the launcher's own
`env -u WAYLAND_DISPLAY` is the cause and the fix is in the launcher.
`gamescope-0` means X4 presents one surface and takes input through another,
both inside gamescope, and the fix is to stop it having two.

## Take thirty-seven: P31 refuted, P32 confirmed, and the geometry finally closes

Run: `X4VR_TAKE=37-socket X4VR_LOG=/tmp/x4vr-take37.log X4VR_GAMESCOPE=1
X4VR_SBS=1 ./launch/x4vr-launch.sh`

```
(pid 3423574, gamescope)  net: connect(/run/user/1000/wayland-0)
(pid 3423616, X4)         net: connect(@/tmp/.X11-unix/X2)
(pid 3423616, X4)         net: connect(/run/user/1000/gamescope-0)
(pid 3423616, X4)         wsi: surface 0x25ca3720 created via vkCreateWaylandSurfaceKHR
(pid 3423616, X4)         wsi: surface 0x22dccd80 created via vkCreateXcbSurfaceKHR, window 0x40002e
(pid 3423616, X4)         swapchain created: 1408x1408 … surface 0x25ca3720 wsi=wayland
```

**P31 refuted, second branch. P32 confirmed.** X4 connects to `gamescope-0`, not
to `wayland-0`. It is a Wayland client *of gamescope*, and gamescope is the only
Plasma toplevel — which is why there is one window, exactly as reported.

The mechanism was sitting in the implicit layer directory the whole time:

```
/usr/share/vulkan/implicit_layer.d/VkLayer_FROG_gamescope_wsi.x86_64.json
```

gamescope's own WSI layer. It converts a client's X11 presentation into a native
Wayland swapchain on gamescope's compositor — which is its entire purpose, and
which is why `WAYLAND_DISPLAY` being unset never mattered: the layer knows the
socket without being told.

### The picture, all of it measured

| | server | extent | set by |
|---|---|---|---|
| **input** | gamescope's XWayland `:2`, window `0x40002e` | 1408×1408, centred → 704 | `res_width`, then gamescope's placement |
| **output** | gamescope's Wayland compositor, surface `0x25ca3720` | 2816×1408 | our composite buffer |
| display | gamescope's nested display | 2816×1408 | `-w`/`-W` |

Nothing here is a lie the layer tells, a driver quirk, or a fallback that went
wrong. **X4 has two surfaces inside gamescope, and only one of them was ever
sized deliberately.** The 704 is the gap between them, and it is the same 704
measured off the screen in take thirty-three by an entirely independent route.

Every earlier explanation of that number — gamescope scaling input, X4 falling
back to Wayland, two toplevels in two display servers — was an attempt to
account for one measurement without the other. The measurement that closed it
was `connect(2)`, which is four lines of code and was never taken because the
question had always been phrased as "which display server is X4 on", and the
answer is *both, for different purposes*.

### The repair, and why it is one flag

`gamescope --force-windows-fullscreen` makes a client's window the size of the
nested display. X4's X11 window becomes 2816 wide, input space becomes display
space, and the 704 disappears at its source rather than being cancelled
downstream. Added as `X4VR_WINDOWS_FULLSCREEN=1`, opt-in until measured.

- **P33** — X4 still renders one eye: `composite armed … each eye 1408x1408`
  and no `SPLIT OFF`. It sizes from the surface, which reports `0xFFFFFFFF`, so
  `res_width` still decides and the X11 window's size is not consulted. If
  `SPLIT OFF` appears with `X4 asked for 2816x1408`, then X4 does size from the
  window and this flag costs the split render.
- **P34** — the pointer is no longer confined to a centred square; it reaches
  both edges of the display.
- **P35** — an element in the **left** copy activates when hovered *where it is
  drawn*, 1:1, and the right copy does not respond. That is the whole point:
  the left copy and the input space would then be the same 1408 pixels.

If P33–P35 hold, task #17 stops being "map a display coordinate into eye space"
and becomes `x mod 1408` — one line, to make the right copy work too.

## Take thirty-eight: P34 confirmed, P33 refuted — and the refutation is the finding

Run: `X4VR_TAKE=38-fullscreen X4VR_LOG=/tmp/x4vr-take38.log X4VR_GAMESCOPE=1
X4VR_SBS=1 X4VR_WINDOWS_FULLSCREEN=1 ./launch/x4vr-launch.sh`

**P34 confirmed.** Reported from the screen: the cursor spans the whole window,
in the main menu, the cockpit and the map. The centred square is gone. The 704
offset is fixed at its source.

**P33 refuted.**

```
sbs: surface 0x1b791a80 caps currentExtent=4294967295x4294967295 wsi=wayland
sbs: SPLIT OFF — X4 asked for 2816x1408 but one eye is 1408x1408.
sbs: composite armed … (X4 renders full width, left half duplicated)
```

Two left halves, as reported. And the reason is the useful part: `res_width` is
still 1408 and `currentExtent` is still `0xFFFFFFFF`, yet X4 asked for 2816 —
**the width of the window gamescope had just forced.**

### What actually sizes X4

The claim written at the top of `x4vr_GetPhysicalDeviceSurfaceCapabilitiesKHR`
since it was first added — *"X4 sizes its whole pipeline from the surface's
currentExtent"* — has never been true on this path, and could not have been
tested there, because the surface has always answered `0xFFFFFFFF` and the
halving branch has never once executed. It survived as a live claim through
every take because nothing ever contradicted it out loud.

Take thirty-eight contradicted it by accident. **X4 sizes its render from its
window.** `res_width` matters only because it is what X4 asks the window to be;
anything else that resizes the window overrides it — gamescope's flag here, and
by the same mechanism the titlebar that the injector's own note recorded as
`1408 -> 1385` long ago. That note was evidence for this all along, filed as a
curiosity.

### The bind

The window is now one knob for two things that must differ:

| wanted | needs the window to be |
|---|---|
| input space = display space | 2816 |
| render = one eye | 1408 |

Take thirty-seven's flag buys the first and take thirty-three's `res_width` buys
the second, and they are the same setting. So the render size has to come from
somewhere that is not the window.

The Vulkan idiom is the obvious candidate, and X4 has never been shown it:
almost every engine reads `currentExtent` and falls back to its own size *only*
when the answer is `0xFFFFFFFF`. X4 has only ever seen the fallback branch. If
it honours a real answer, then reporting the eye extent sizes the render while
leaving the window at display width — and the three extents are settled without
compensating for anything downstream.

`X4VR_FAKE_EXTENT=1` does that: where the surface declines to state a size, the
layer states one. Nothing is halved — there is no number to halve — which is why
it is a separate knob from the original halving path rather than a widening of
it.

- **P36** — with `X4VR_FAKE_EXTENT=1` and `X4VR_WINDOWS_FULLSCREEN=1`, X4 asks
  for 1408×1408, `SPLIT OFF` does not appear, and the composite says "X4 renders
  one eye". If X4 still asks for 2816, it does not consult the surface at all,
  and the comment quoted above was fiction in both its versions — leaving the
  window as the only lever on render size, which input needs for itself.
- **P37** — the cursor still spans the full width, unchanged from take
  thirty-eight, since the window is untouched by this.
- **P38** — an element in the **left** copy activates when hovered where it is
  drawn, 1:1; the right copy does not respond. Both copies responding would mean
  input is still not in the space the left copy occupies.

## Take thirty-nine: P36 refuted — X4 does not consult the surface at all

Run: `X4VR_TAKE=39-fakeextent … X4VR_WINDOWS_FULLSCREEN=1 X4VR_FAKE_EXTENT=1`

```
sbs: surface 0xc31db70 had no preferred extent — reporting 1408x1408 (the eye)
     so the render size stops following the window.
sbs: SPLIT OFF — X4 asked for 2816x1408 but one eye is 1408x1408.
```

The layer stated the eye extent and X4 asked for the window width regardless.
**X4 does not read `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` for its size.**
The comment that hook carried from the day it was written was fiction in both
its versions, and the surface is not a lever on the render size by any route.

Two takes, two refutations, and together they close the question:

> X4's render size is its window size. Nothing else influences it. And X4's
> input space is the same window. **One number, serving two purposes that have
> to differ.**

Every remaining approach follows from that sentence:

| | window | consequence |
|---|---|---|
| take 33 | 1408 | render correct, input 1408 wide and centred → the 704 |
| take 38–39 | 2816 | input correct, render full width → two left halves |

There is no third setting. What is left is to change what X4 **believes** the
window is while leaving the real one at display width — real window 2816 for
input, believed window 1408 for the render.

### Why SDL, and why only these two functions

SDL interposition was deliberately avoided in task #18: SDL2 and SDL3 disagree
about `SDL_CreateWindow`'s signature, gamescope is SDL2, X4 is SDL3, and they
share a process tree, so defining an SDL symbol risked calling one through the
other's prototype. That reasoning does not extend to `SDL_GetWindowSize` and
`SDL_GetWindowSizeInPixels`: both SDL generations declare identical argument
lists, and the return value is forwarded opaquely instead of being interpreted.
The hazard was specific, and so is the exemption.

Logging is unconditional and the halving is behind `X4VR_HALVE_WINDOW=1`, so a
single run answers both questions — whether X4 asks these at all, and whether
answering differently moves the render.

- **P39** — an `sdl: SDL_GetWindowSize -> 2816x1408` line appears in X4's pid.
  If neither function is ever called, X4 takes its size from X11 directly or
  from cached configure events, and this lever does not exist either — leaving
  only the X11 geometry path, or giving up on the window and having the shim
  own input outright.
- **P40** — with `X4VR_HALVE_WINDOW=1`, X4 asks for 1408×1408, `SPLIT OFF` does
  not appear, and the composite reports "X4 renders one eye" — while the real
  window stays 2816 and the cursor still spans the display.
- **P41** — an element in the left copy activates where it is drawn. X4 would
  then be hit-testing a 1408-wide UI against pointer coordinates that run to
  2816, so the left copy should be exact and the right copy dead. That is the
  state in which the shim's remaining job is `x mod 1408`.

## Take forty: P39 confirmed, P40 refuted — and the two purposes come apart

Run: `X4VR_TAKE=40-halvewindow … X4VR_WINDOWS_FULLSCREEN=1 X4VR_HALVE_WINDOW=1`

```
517543.465  layer   swapchain created: 2816x1408 … (pid 3429993)
517543.498  inject  sdl: SDL_GetWindowSize -> 704x1408 (HALVED …) (pid 3429993)
517543.465  layer   sbs: SPLIT OFF — X4 asked for 2816x1408
```

**P39 confirmed:** X4 does call `SDL_GetWindowSize`. **P40 refuted:** halving it
did not move the render. And the timestamps say why — X4's first call lands
**33 ms after the swapchain already exists**. The render size was decided
before X4 ever asked.

Reported from the screen, and this is the load-bearing half: menu clicks stopped
working, and the map's top row would not highlight on hover though it still
reacted to a click. So halving that number *did* change something, and what it
changed was **interaction**.

### The finding

| | source | evidence |
|---|---|---|
| render size | not the surface (take 39), not `SDL_GetWindowSize` (take 40) — decided before either is consulted | ordering, and two refutations |
| input / hit-test space | `SDL_GetWindowSize` | halving it broke menus and hover, nothing else |

**They are different channels.** Takes 38 and 39 established that the window
cannot serve both purposes; take 40 establishes that X4 does not actually read
them from the same place. That is the difference between a dead end and a plan:
input can be taken over without touching the render at all.

It also explains take 38 and 39 in retrospect. Forcing the window to 2816 moved
*both*, because the window is upstream of both channels — but reaching into one
channel below the window moves only that one.

### Which settles the shim's purpose

The proposal to sidestep the conflict in the input layer is not a workaround for
a sizing problem that could not be solved; it is the only place the two
requirements can be separated, and the measurements now say so rather than the
argument. The plan:

* **Render** — the take-33 configuration, unchanged: window 1408, `res_width`,
  no `--force-windows-fullscreen`, no `X4VR_HALVE_WINDOW`. This is the
  configuration tagged `stage2-sbs-working` and it produces true SBS.
* **Input** — the shim owns the SDL event stream. gamescope runs with
  `--force-grab-cursor`, so the pointer is in *relative* mode and X4 integrates
  deltas into a cursor position it maintains itself. There is no OS-level
  confinement to defeat: the box the cursor "cannot leave" is X4's own clamp
  against its own window size, and both sides of that are things the shim can
  reach.
* **Drawing** — the cursor still has to be drawn into the eye image before
  duplication, or it cannot be correct in both halves. Unchanged, and
  independent of the above.

`SDL_PollEvent` is now interposed, observation only, logging the first few
mouse motion and button events with `x`, `y`, `xrel`, `yrel`. Arguments are
identical in SDL2 and SDL3; only the event layout differs, and it is read
solely when the process is X4.

- **P42** — X4 receives mouse motion through `SDL_PollEvent`, and the events
  carry non-zero `xrel`/`yrel` with `x`/`y` inside 0…1408. If `x`/`y` span
  0…2816, X4 is being handed display coordinates and clamps them later, which
  changes where the shim has to act.
- **P43** — button events carry the same coordinate space as motion. If they
  differ, the map's click-works-but-hover-does-not behaviour from this take has
  a second cause and the shim needs both paths.

## Take forty-one: the working state did not come back, and the command is the suspect

Run: `X4VR_TAKE=41-input X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1
X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MASK_TONEMAP=1 X4VR_MV=1
X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_BINDLESS_MIRROR=1 ./launch/x4vr-launch.sh`

Reported: splash and loading screens correct; in the menu and in flight the
**right eye shows HUD and interface but the 3D is black**, and the left eye's X4
logo is clipped at the top right where it used to be whole. The map works, with
the centred square back (expected — no `--force-windows-fullscreen` here).

That command was **my reconstruction of take thirty-three, not take
thirty-three**. Take thirty-three was never recorded: the `env: run =` line
exists only from take thirty-four onward, and `/tmp/x4vr.log` contains no
segment with `right half from layer 1` at all, so the run is not in that file
either. This is the same failure the run-recording line was added to prevent,
one take too late to prevent it.

What can be said without another run:

* **The layer has not changed in any way that touches rendering.** The whole
  diff from `stage2-sbs-working` to here is added observation — surface hooks,
  capability logging, socket and SDL watching — plus two knobs that default off
  and were off in this run.
* **The per-view machinery is fully engaged.** `370 modules edited`,
  `1062 pipeline stage(s)`, `505124 layer-1 twin descriptors`,
  `fallbacks=0` — the same order as the healthy reference run. Whatever is
  wrong, the bindless index-offset path is not idle.
* `substituted=25 per_eye_images=24` against take thirty-three's recorded
  `substituted=26 per_eye_images=25`. One image short, which is small enough to
  be scene-dependent and not small enough to ignore.

So the question is exactly: **my code, or my command?** Guessing at it from the
symptom is what produced the last four takes' worth of retractions.

`stage2-sbs-working` is built in a worktree at `/tmp/x4vr-tag/build`, and the
launcher already honours `X4VR_BUILD`. Running the *same* command against both
trees isolates the variable in one step.

- **P44** — the tagged build, given this command, shows the same broken right
  eye. That means the command is wrong, take thirty-three used a different knob
  set, and nothing has regressed.
- **P45** — the tagged build shows correct SBS. That means something in the
  observation-only changes is not observation-only, and the diff above is wrong
  about itself; the first suspects are the two SDL interposers, which are live
  in every run regardless of knobs.

## Take forty-two: P44 confirmed — the code is exonerated, the command is not

Same command, `X4VR_BUILD=/tmp/x4vr-tag/build`, i.e. the `stage2-sbs-working`
tree. **Identical failure.** So nothing regressed between the tag and here, and
every "observation-only" claim about the intervening diff holds. What is wrong
is the knob set.

Take thirty-three's knobs cannot be recovered. The record above quotes its log
and never its command, `/tmp/x4vr.log` holds no segment with `right half from
layer 1`, and P21 — the prediction take thirty-three was said to satisfy —
explicitly describes a run with `X4VR_SBS_RIGHT_LAYER` **unset**, so even the
doc cannot distinguish "the run that armed the container" from "the run that
turned the second eye on".

### Stop guessing, use the probe

The symptom is specific: in the right eye the HUD and interface draw but the 3D
is black; in the left eye the logo is clipped. A knob sweep would take one run
per hypothesis and each run is a person's time.

`X4VR_MV_PROBE=1` already answers the underlying question directly. It hashes
layer 0 and layer 1 of one per-eye colour attachment per frame, cycling through
them, and logs both — so "which image stops carrying a second eye" becomes an
image number rather than an inference from what appeared on screen. It was built
in task #7 for this exact class of question and has not been switched on since
the SBS work began.

`X4VR_MV_INVENTORY=1` comes with it, for the pass table that says which passes
were masked.

- **P46** — the probe reports `DIFFER` for the early world/G-buffer images and
  `IDENTICAL` (or an all-zero layer 1) from some specific image onward. The
  first image where layer 1 stops differing is where the second eye is lost, and
  the pass that writes it is the thing to look at.
- **P47** — the HUD images differ, since the HUD reaches the right eye on
  screen. If the HUD images are *identical* too, then the right eye's HUD is
  arriving by some route other than layer 1 and the composite is not doing what
  its log line claims.

This is deliberately not a knob sweep. If the probe localises the break, the
knob follows from it; if the probe shows layer 1 healthy all the way to the
composite, the fault is in the composite or the present path and no knob would
have found it.

## Take forty-three: the probe localised it, and the fix is a predicate

P46 and P47 both hold, and the numbers name the break.

| image | layer 0 non-empty | layer 1 non-empty |
|---|---|---|
| #97, #98 (world) | — | `DIFFER` 6–9% |
| #50 | 1,982,464 | 457,600 |
| #51 | 1,982,464 | 64,897 |
| #52 | 1,982,464 | 11,183 |
| #53 | 1,982,464 | 594,096 |

The world images carry a real second eye — the scene *is* drawn twice. The
present targets have a full layer 0 against a layer 1 holding between 0.5% and
30% of it. That is the black right eye, and the spread across the four images
matches the report from the screen exactly: *"briefly seems to fade in with the
left screen, only to become all black"*. Layer 1 receives content in some
frames and not others.

The inventory says why:

```
20 MONO (depth-only/shadow)
 8 MONO (all-LDR/UI)                        ← masked by no rule
 6 MONO (all-LDR/UI) +MASKED +PRESENT-CAND
```

The HUD reaches the screen through the six. The scene reaches it through some of
the eight, which render into layer 0 only.

### The real defect is that the rule depends on the scene

Take thirty-three logged **3** `+PRESENT-CAND`; take forty-three logged **6**,
on the same knobs. `subpass_is_present()` matches on the shape of whatever
passes the current scene builds — one colour attachment, no depth, LDR format —
so the set it catches changes with the content. **A configuration that depends
on the scene is not a configuration**, and that, not a lost environment
variable, is why the working state could not be restored. Recovering take
thirty-three's exact command would most likely not have recovered take
thirty-three.

This was on the record as an open item — *"narrowing `+PRESENT-CAND` from seven
passes to the one true composite"* — filed as tidying. It was the bug.

### X4VR_MASK_LDR

Masks on the property that actually matters: every colour attachment is LDR, so
the pass is somewhere on the post/UI path to the screen. It deliberately says
nothing about attachment count or depth — those are what `subpass_is_present()`
adds to guess at the composite, and they are exactly what excluded the eight.
`classify_unsheared` has already decided the pass is not a world pass before
this rule ever runs.

The inventory now prints **which** rule masked each pass —
`+MASKED(tonemap|present|ldr|?)`. `+MASKED(?)` means something set `per_eye`
that no named predicate claims, which is how a fourth rule would otherwise
arrive unnoticed; a case asserts it never appears.

Three offline cases added (63 total): the knob fires on an LDR pass and only
with the knob; it never masks an HDR pass, the failure that would silently cost
the world its second eye; and every masked pass names its rule.

- **P48** — with `X4VR_MASK_LDR=1`, the eight unmasked all-LDR passes become
  `+MASKED(ldr)`, and the present images' layer 1 fills to roughly layer 0's
  non-empty count instead of 0.5–30% of it. On screen: a right eye with 3D in
  it.
- **P49** — no `+MASKED(?)` and no HDR pass masked, live, on real content. The
  offline cases build one LDR pass; the game builds thirty-four.
- **P50** — the masked count stops moving between runs. If the tally still
  differs run to run, something else in the classification is scene-dependent
  and the configuration is still not restorable.

## Take forty-four: P48 refuted, and a retraction that matters more

Run (recorded, from the log): `X4VR_MASK_LDR=1 X4VR_TAKE=44-maskldr
X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1
X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MASK_TONEMAP=1 X4VR_MV=1
X4VR_SBS=1 X4VR_MV_PROBE=1 X4VR_MASK_PRESENT=1 X4VR_BINDLESS_MIRROR=1
X4VR_MV_INVENTORY=1`

`+MASKED(ldr)` fired on four passes. The present targets' layer 1 still holds
**3.9%–5.1%** of layer 0. Masking every all-LDR pass does not fill the right
eye, so the break is not a missing mask.

### The retraction

The claim that motivated `X4VR_MASK_LDR` — *"3 `+PRESENT-CAND` in take
thirty-three, 6 in take forty-three, on the same knobs, therefore the predicate
is scene-dependent"* — was **one log file holding two X4 sessions**. `X4VR_LOG`
appends; take forty-three's file carries two `mv: X4 uses vkCreateRenderPass`
lines. Per session the count is 3, identical to take thirty-three. Distinct
render-pass serials across the two runs: 59 and 58.

The pass population is stable. The predicate was never unstable. **The working
configuration was always restorable**, and the conclusion drawn from that
miscount — reported to Patola as "recovering take thirty-three's command
probably would not have recovered take thirty-three" — was false and
discouraging in a way the evidence never supported.

This is the fourth counting instrument in this project read without checking
what it counted over (holes #11–#13 were the others), and the first to change
the code rather than only a conclusion. The distinguishing feature is not
subtlety: `grep -c` over an appending log is a sum across runs, and nothing in
the pipeline said so.

### The gate, and why it comes first

`tools/score_run.py` refuses to score a log containing more than one session
**before** it evaluates anything else, then reports:

* the split, since nothing below it is stereo;
* every mask with the rule that set it, and a failure on `+MASKED(?)`;
* `layer1/layer0` non-empty ratio for the present targets — the black right eye
  as a number, taking the best sample per image so a frame caught mid-fade is
  not read as a broken chain.

The point is not convenience. Eleven takes were scored by a person describing a
screen to someone who then chose which theory the description supported. A
number from the log removes that step.

### Where the search actually stands

Established and not in doubt:

* the code is exonerated — the `stage2-sbs-working` tree fails identically on
  the same command (take forty-two);
* the render-pass population is stable run to run (59/58 serials);
* the world images carry a real second eye (`#97`/`#98` DIFFER 6–9%);
* the present targets do not (layer 1 at 4–5% — the HUD);
* so the loss is between the world images and the present target, and it is not
  the masking of all-LDR passes.

What is missing is a knob subset, and that is a finite search over a stable
system, scored mechanically. See `docs/bisection-plan.md`.

## Bisection run B1: the prediction, and the argument against running it

Committed before the run, as the rule requires.

B1 drops `X4VR_MASK_TONEMAP` and `X4VR_MASK_LDR` from the take-forty-four
command and changes nothing else.

**The code argues B1 will fail.** "Masking" adds a pass to the multiview set —
`classify_per_eye()` ORs the three mask predicates into `per_eye`, and a pass
that is not per-eye gets `viewMask` 0 and writes layer 0 only. The comment
written for task #4 states the consequence directly: the tonemap is a
fullscreen triangle, so K must not be applied to it, "but it must be masked, or
there is no second layer of #103 for a per-eye tonemap to write." If that model
of the chain is right, dropping `MASK_TONEMAP` cannot fill layer 1 of the
present target — it removes the only thing writing layer 1 upstream of it, and
the ratio should fall *below* take forty-four's 4–5% rather than rise.

It is run first anyway, for two reasons. The claim above is reasoning, and in
this project reasoning has lost to measurement four times running, each loss
costing more than the run would have. And B1 is the only run in the set that
tests the plan's actual premise — that take thirty-three predates both knobs —
rather than testing a mechanism.

- **P51** — B1 fails, with layer 1 at or below take forty-four's 4.8% on `#50`.
  Confirmed only if it also *falls*; a flat 4–5% means the tonemap mask was
  never contributing to layer 1 either, and the chain model above is wrong in a
  way that matters more than B1's verdict.
- **P52** — the masked tally is `present=1` and nothing else. Any `tonemap=` or
  `ldr=` line means a knob is set that the command did not set, and the run is
  void rather than informative.

If P51 is confirmed the interesting run is **B3** (drop `BINDLESS_PATCH`), not
B2: B2 restores a knob whose removal B1 will have just shown to be harmful,
which is a return to the take-forty-four configuration minus `MASK_LDR` — a
configuration take forty-three already scored.

## Take forty-five (B1): P51 confirmed, P52 refuted, and the scorer lied

Run: `X4VR_TAKE=45-B1 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408
X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1
X4VR_SBS=1 X4VR_MV_PROBE=1 X4VR_MASK_PRESENT=1 X4VR_BINDLESS_MIRROR=1
X4VR_MV_INVENTORY=1`

Patola: identical to take forty-four in every respect, right eye black.

**P51 confirmed.** Settled samples 4.4% and 4.6%, against take forty-four's
4.8% and 5.1%. It fell, as the chain model said it would.

### Hole #16: the scorer manufactured a false positive, in the run it was built for

Its first output said `img #51 … 80.5% ok` and `img #53 … 24.1%`. Both were
false, and the instrument was mine.

`#50`–`#53` are `image 0 of 4` … `image 3 of 4` — **the four swapchain images**,
one target sampled at four moments. X4 created its last world render pass at
`521798.721`; `#53` was sampled 3.7 s later and `#51` 7.8 s later, both inside
the savegame load. The two sampled before it read 4.4% and 4.6%, matching take
forty-four exactly. The 80.5% is a loading frame.

The rule that produced it was written in this file with the rationale *"take
the best sample per image, because a frame captured mid-fade is not evidence of
a broken chain."* That is precisely inverted: taking the best sample means a
frame captured mid-load **is** read as evidence of a working chain. A rule
written to prevent a false negative manufactured a false positive, in the
instrument built to stop exactly this. Two further consequences of the same
misreading: four swapchain images were being scored as four independent checks,
so every previous "four FAIL lines" was one finding printed four times.

`score_run.py` now skips any sample taken less than 10 s after a
`CreateRenderPass` (X4 stops creating them once the scene is up), prints what it
skipped and why, and reports the swapchain as one target.

### P52 refuted: `MASK_TONEMAP` is subsumed by `MASK_PRESENT`

Take forty-four: `present=1, tonemap=2`. Take forty-five: `present=3`. The same
three passes — an srgb-resolve pass with one colour attachment and no depth
also satisfies `subpass_is_present`, and `classify_per_eye` ORs the rules, so
dropping `MASK_TONEMAP` unmasked nothing. It only changed the name printed.

**B2 is therefore a provable no-op and is struck from the plan**, on the log
rather than on the argument given above it.

### `rp #0` and `rp #7` print identically and classify differently

```
rp #0.0: 1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI) +MASKED(present) +PRESENT-CAND
rp #7.0: 1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI)
```

Every field the inventory prints is identical, yet `subpass_is_present` matches
one and not the other. By elimination the differing input is the one the
inventory does not print faithfully: it resolves `pDepthStencilAttachment` to an
index and calls `VK_ATTACHMENT_UNUSED` "no-depth", while the predicate rejects
on the **pointer** being non-null (`x4vr_layer.cpp:1627`). A subpass with a
non-null pointer naming `UNUSED` reads as "no-depth" and fails the predicate.
Offline-testable; no run needed.

### What the chain read shows, and it is not what the plan expected

> **CORRECTED during task #20 — see "Hole #18" below.** The table that stood
> here reported `non-empty layer1/layer0`, which is a **fill** measure, and read
> it as "carries a second eye". Fill says whether layer 1 has content; only the
> probe's `DIFFER` percentage says whether that content is a *different* eye.
> The images listed at "101.9%" were never shown by this table to be stereo.
> The corrected reading is below; the conclusion it supported — that the loss
> is a cliff at the swapchain — survives for take forty-five but is not the
> whole picture.

Reading the same samples by `DIFFER` percentage, which is the measure that
answers the question:

| image | fmt | take 45 (patch on) | take 46 (patch off) |
|---|---|---|---|
| `#57`, `#59` | 97 `RGBA16F` | DIFFER 26.5–30.1% | DIFFER 30.9–31.9% |
| `#60`, `#61` | 83 `RG16F` | DIFFER 30.1% | DIFFER 31.8–31.9% |
| `#63` | 13 `R8_UINT` | DIFFER 17.6% | DIFFER 8.7% |
| `#66`, `#67` | 97, 352² | DIFFER 13.1–19.0% | **IDENTICAL** |
| `#65` | **50 `BGRA8_SRGB`** | IDENTICAL | **IDENTICAL** |
| `#97`, `#98` | 76 `R16_SFLOAT` | (unsettled) | **IDENTICAL** |
| `#50`–`#53` | 44, swapchain | DIFFER, layer 1 **4.4% full** | IDENTICAL |

The HDR chain is **genuinely stereo in both runs** — ~31% of pixels differ
between the eyes, which is the per-eye camera constants (task #3) working. It
was equally stereo in take forty-five, so `BINDLESS_PATCH` is not what creates
the second eye upstream.

Also corrected: `#97`/`#98` are **not** "the world images". In these runs they
are `R16_SFLOAT` single-channel 1408² buffers (one of three such triples:
`#42`–`#44`, `#84`–`#86`, `#97`–`#99`). The table at the top of this file
assigning `#97` = `RGBA16F` is from an earlier session — **the layer's image
serials are per-run and the older tables must not be read into newer logs.**
Takes forty-five and forty-six happen to agree with each other.

### Where the stereo actually dies: the sRGB resolve

`#65` is format 50, `BGRA8_SRGB` — the tonemapped LDR image. It is
**IDENTICAL** in both runs, while its inputs differ by ~31%. Its writers:

```
img #65 writers — masked rp [33,45] unmasked rp []
rp #33.0: 1 colour [50L] no-depth final=2 -> MONO (all-LDR/UI) +MASKED(present)
rp #45.0: 1 colour [50L] no-depth final=2 -> MONO (all-LDR/UI) +MASKED(present)
```

Both writers are **masked**. So the pass is replicated to both layers, runs
twice, and writes the same pixels twice — because the draw is per-view but the
**sampling is not**. View 1 reads view 0's source.

That is the entire remaining defect, and it is one step, not a region:

* patch **off** — no offset, view 1 samples view 0's source, layer 1 gets the
  left eye. The duplicate state, `stage2-duplicate-restored`;
* patch **on** — view 1 samples `index + 26653` and gets **black**, so the
  swapchain's layer 1 keeps only what was drawn by unpatched passes: the HUD,
  4.4%.

### Hole #18: fill read as stereo

`non-empty l1/l0` answers "does layer 1 have content", which is the right
question for a black right eye and the wrong one for stereo. Two images that are
bit-identical score 100% on it. The take-forty-five write-up used it to claim
the upstream chain "carries a real second eye" and to place the loss at the
swapchain; by `DIFFER`, `#66`/`#67`/`#97`/`#98` were already flat in take
forty-six. `score_run.py` reports both, and grades `DUPLICATE` separately from
`STEREO`, for exactly this reason — but the prose did not follow the tool.

### The fact that reframes the whole search

Take forty-four masked `rp #7` (as `+MASKED(ldr)`) and scored 5.1%. Take
forty-five left it unmasked and scored 4.6%. **Masking the writer changes
nothing.** So the defect is not an unmasked pass overwriting layer 1 — a masked
pass writing layer 1 would put *something* there, and if it sampled a mono
source it would put the left eye there, which is the "two left halves" state
this project already had working.

Layer 1 receives the HUD and a black hole where the scene should be. A pass
that runs, writes, and produces black is one sampling a descriptor that holds
nothing. The composite reaches its source through X4's bindless array, and
`X4VR_BINDLESS_PATCH` makes view 1 read `index + 26653`. If that slot is
populated for the world passes (which is why `#97`/`#98` differ) but empty for
the composite's source, view 1 samples nothing and writes black — exactly the
observed 4.4%, exactly the HUD and nothing else.

- **P53** — dropping `X4VR_BINDLESS_PATCH` (B3) lifts the settled swapchain
  ratio to ≈100% and puts the **left** eye's 3D in the right eye: two left
  halves, the state that worked before. Refuted if the ratio stays near 4.4%.
- **P54** — if P53 confirms, the defect is the offset applied to the
  composite's source index, not the masking, and no masking knob can fix it.

## Take forty-six (B3): P53 confirmed — the duplicate state is back

Run: recorded in `docs/known-good-runs.md`, tag `stage2-duplicate-restored`.

Patola: "two complete side-by-side screens again … the 3D scenes appear
duplicated with no black screen". Scorer: exit 0, **19 of 19 settled swapchain
samples bit-identical**.

Dropping `X4VR_BINDLESS_PATCH` was the whole difference. P53 said it would put
the left eye's 3D into the right eye at ≈100%; it did, exactly.

### P54 confirmed with it: the defect is the offset, not the masking

The probe shows where the second eye now dies, and it is a different place:

| image | take 45 (patch on) | take 46 (patch off) |
|---|---|---|
| `#61` | 101.9% | **DIFFER 31.8%** |
| `#63` | 100.1% | **DIFFER 8.7%** |
| `#97`, `#98` | 100.8%, 99.9% | **IDENTICAL** |
| `#50`–`#53` | 4.4% (black) | IDENTICAL (duplicate) |

`#61` and `#63` carry a **real per-eye difference** with the patch off — that is
the per-eye camera constants (task #3) working. The difference is then flattened
at `#97`/`#98`, which become bit-identical.

So the two states fail in opposite directions at the same step:

* patch **off** — the pass reading `#61`/`#63` samples view 0's source for both
  views, and writes the left eye twice. Genuine stereo is destroyed here;
* patch **on** — the same pass reads `index + 26653` for view 1, that slot holds
  nothing, and it writes black.

Neither is right, and no masking knob can reach it. The remaining question is
what the correct source is for view 1 at that one step, which is task #13's
mechanism applied to the wrong descriptor. That is the next piece of real work,
and it is now a single, named step rather than "somewhere between `#97` and
`#50`".

### Two more scorer holes, both the same family as #16

* **IDENTICAL parses as no data.** Those lines carry no `non-empty` field, so
  the best result the probe can report read as an absence of evidence: take
  forty-six first scored `no settled probe samples … FAIL`. The parser decided
  what counted as evidence and could not see the good shape.
* **Best-sample scoring survived in the ratio check.** Takes forty-four and
  forty-five reach 100% on their *splash* frames — X4 draws the same pixels to
  both layers there — while their cockpit frames sit at 0.4%. Scored on the
  best sample they read as partial successes. Now scored on the **worst settled
  frame**: 44/45 → 0.4% FAIL, 46 → 100% PASS.

`MIXED WRITERS` is demoted to a warning by the same run: it fires on take
forty-six, whose two layers are bit-identical, so the pass it names cannot be
contributing anything. It is built from framebuffers *created*, and a
framebuffer is not a draw.

### The clipped logo is a regression, not the aspect ratio

Ruled out from this log, without a run:

* **Not a sheared UI pass.** Every pass classified `STEREO` has HDR colour
  attachments (`97H`, `83H`, `13H`); no LDR/UI pass is sheared.
* **Not a window/render disagreement.** X4 (pid 3454557) reports
  `SDL_GetWindowSize -> 1408x1408` and creates a 1408×1408 swapchain. Its
  believed window matches its render exactly. The 2816×1408 in the log belongs
  to gamescope's own process.
* **Not the square aspect as such.** Take thirty-three rendered the same
  1408×1408 and the logo was whole — Patola noticed the clipping appear at take
  forty-one and it has persisted since.

So something between take thirty-three and take forty-one shifted UI geometry
and is still in the take-forty-six configuration. The surviving candidates are
`X4VR_STEREO` (K reaching a pipeline that draws the logo, even though no *pass*
is misclassified) and `X4VR_BINDLESS_MIRROR` (the unsheared twin not being
swapped into a pipeline that needs it). Task #20.

## Task #20: why view 1 samples black — the template road is not mirrored

The offset mechanism has two halves. `X4VR_BINDLESS_PATCH` edits fragment
shaders so view 1 computes `index + 26653`; `X4VR_BINDLESS_MIRROR` writes the
descriptors that live at those twin slots. If the patch fires on a slot the
mirror never wrote, `PARTIALLY_BOUND` makes the read undefined and the driver
returns zeros. Black.

Take forty-five confirms the patch fires on exactly the passes that matter:

```
srgb-resolve rp #33: frag module #16 samples set 0 binding 7[53306] [index-offset APPLIED]
mv final: present rp #0 <- frag module #12 samples set 0 binding 5[53306] [index-offset APPLIED]
```

So the question is whether the mirror covered those slots. Eliminating from the
code, the twin slot can be unwritten for only three reasons:

1. `g_mirror_no_room` — logged, **0**;
2. the collision guard fired — it logs `bindless mirror: DISABLED`, **absent**;
3. **the write never reached `bindless_mirror_writes()` at all.**

Binding 5 and binding 7 are both `SAMPLED_IMAGE` with `count=53306`, so the
type and size filters pass, and a write that arrives before an image is known to
be doubled is copied *verbatim* — which yields a duplicate, never black. That
leaves (3), and there is exactly one road into the descriptor table that the
mirror does not watch:

```cpp
VKAPI_ATTR void VKAPI_CALL x4vr_UpdateDescriptorSetWithTemplate(...) {
    if (g_bindless_survey) { ...count... }
    d->UpdateDescriptorSetWithTemplate(device, set, tmpl, data);   // straight through
}
```

`vkUpdateDescriptorSets` is mirrored. `vkUpdateDescriptorSetWithTemplate` is
**counted and passed through**. Any image descriptor X4 writes through a
template has no twin, and view 1 reading its twin slot gets zeros.

The layer predicted this in its own survey output, and no run ever read it:

> `%llu template updates, %llu of them via a template carrying image
> descriptors — THE COUNTS ABOVE ARE INCOMPLETE, and a mirror hooking
> vkUpdateDescriptorSets alone would miss these`

That line is gated behind `X4VR_BINDLESS_SURVEY=1`, which was never set in any
of takes forty-one through forty-six. The instrument that would have named this
existed the whole time and was switched off.

- **P55** — with `X4VR_BINDLESS_SURVEY=1` on the known-good command, the log
  reports a non-zero count of template updates carrying image descriptors, and
  prints the `bindless: update template carries image descriptors at
  binding(s) …` line naming binding 5 and/or 7. Refuted if both counts are
  zero, in which case the template road is dead and reason (3) is wrong — the
  elimination above would then have no surviving candidate and the mirror's
  coverage would need direct instrumentation instead.

The run is observation-only and rides on `stage2-duplicate-restored`, so the
screen stays good while it answers.

## Take forty-seven: P55 refuted — X4 never uses a descriptor update template

```
bindless final: 0 template updates, 0 of them via a template carrying image descriptors
```

Zero, both counters, at first present and at exit. The road the mirror does not
watch is a road X4 never drives. The elimination in the section above therefore
has **no surviving candidate**, and the mirror's coverage is not the explanation.

Recorded rather than quietly dropped, because the reasoning was sound and the
conclusion was still wrong: three mechanisms were excluded correctly and the
fourth was assumed to be live without checking, in a project whose recurring
failure is exactly that.

### What the survey did establish

```
layout #0 binding 5 — 10976 distinct slots, range 0..10975, 155 holding a per-eye image
layout #0 binding 7 — 10976 distinct slots, range 0..10975, 155 holding a per-eye image
bindless final: 61570259 image-descriptor writes, 61549006 after the first present
per-eye slots: 10946=img#65 10947=img#65 10945=img#54 10944=img#57 …
```

* X4 occupies slots **0..10975** of a 53306-element table. The offset 26653
  lands twins at 26653..37628 — inside the table, clear of X4's range. The
  constant is sound.
* **155** of those slots hold a per-eye image, and they include the exact
  sources in question: `img#65` (the sRGB tonemap output), `img#57` and
  `img#54` (HDR colour).
* X4 rewrites descriptors continuously — 61.5 million writes, 99.97% of them
  after the first present. Nothing here is a one-shot setup that could be
  missed by arriving late.

### Every mechanism that could produce black, and why each is excluded

| mechanism | verdict |
|---|---|
| twin slot out of range | `no_room` counter is 0; 10975 + 26653 < 53306 |
| twin region overlaps X4's own | collision guard logs `DISABLED`; absent |
| written via a template, unmirrored | **P55: zero template updates** |
| patch offsets a *small* array out of bounds | patch uses `count > offset`, the same bound as the mirror; the 58/18/16/2-element bindings are excluded |
| layer 1 never rendered for that image | `view_of_layer` guards on `g_per_eye_images`; a miss returns NULL and the twin is a **verbatim** copy — which yields a duplicate, not black |
| descriptor written before its framebuffer | same guard, same verbatim fallback — duplicate, not black |

Reading the code has run out of candidates. Every remaining path produces a
*duplicate* when it fails, and the observed failure is *empty*.

- **P56** — with the patch on and a settled frame, `#65`'s layer 1 is **empty**
  (fill near 0), placing the loss at `rp #33`/`rp #45` where the offset is
  applied. Refuted two distinguishable ways: layer 1 **full but identical**
  means the patch never took effect on that pass, and layer 1 **full and
  DIFFER** means the offset works at `#65` and the black is introduced further
  down, at `rp #0`.

One image, three outcomes, one run. Take forty-five could not answer it — its
only `#65` sample landed mid-load and the scorer discarded it.

## Take forty-eight: P56 refuted, and reading the shader found the bug

`#65` came back **IDENTICAL and full** with the patch on — branch two of P56.
So the patch changes nothing at `rp #33`'s output, and the loss is not there.

What the run did show is that the offset mechanism **works**:

| image | patch off (46/47) | patch on (48) |
|---|---|---|
| `#57`, `#59`, `#60`, `#61` | DIFFER 31–32% | DIFFER 31–32% |
| `#66`, `#67` | IDENTICAL | **DIFFER 13–18%** |
| `#97`, `#98` | IDENTICAL | **DIFFER 6.0 / 9.3%, full fill** |
| swapchain | IDENTICAL (duplicate) | **0.4% — empty** |

The patch converts `#66`/`#67`/`#97`/`#98` from flat to genuinely stereo, at
full fill. Mirror, twin region and offset constant are all correct. The failure
is isolated to **one pass**: `rp #0`, the composite into the swapchain.

No probed image has content in layer 0 and nothing in layer 1, so `rp #0` is not
reading an empty source. At that point the code had no candidates left, and the
answer was in the shader — which task #20 said to read and which I had put off
for three runs of log archaeology.

### The bug

`rp #0`'s fragment shader is module #12. It declares **two variables on set 0,
binding 5**:

```
OpDecorate %S_sampler2D_AUTOMS Binding 5      indexed by the literal %int_1_0
OpDecorate %S_sampler2D        Binding 5      indexed from the dynamic block
```

Aliasing one binding with two variables is legal, and X4 does it in 228 of 409
dumped modules. The layer's caller iterates *tables* and calls the patch once
per entry, so for module #12 it called `patch_fragment_index_offset(code, 0, 5,
26653)` **twice with identical arguments** — and the patch stopped at the first
matching variable:

```cpp
target_var = id;
break;              // <- one variable, whichever came first
```

So both calls hit `S_sampler2D_AUTOMS`, and:

* it was offset **twice** — `1 + 26653 + 26653 = 53307`, in a **53306**-element
  array. Out of bounds by one. With `PARTIALLY_BOUND` that read is undefined and
  comes back zeros. **Black.**
* `S_sampler2D` was **never patched**, so its view 1 stayed on view 0's slot.

Demonstrated offline against the real module, no run needed:

```
%428 = OpIAdd %int %int_1_0 %427     first patch:  1 + V*26653
%435 = OpIAdd %int %428     %434     second patch: (1 + V*26653) + V*26653
```

And it explains the run exactly. `rp #33` samples binding **7**, where module #18
declares a single variable — patched once, correctly — which is why `#97`/`#98`
were visibly stereo in the very same frame in which the present pass was black.
The failing and working passes were never different mechanisms; they were
different *variable counts on a binding*.

### The fix

`patch_fragment_index_offset` now collects **every** variable at (set, binding)
and offsets each of its access chains once. The caller dedupes by (set, binding)
so the transform is invoked once per binding — necessary because the transform
is deliberately **not** idempotent, and a test now pins that so the guard cannot
be quietly removed.

`tests/sample_alias_binding.frag` reproduces X4's shape: two aliased variables,
one literal index and one loaded from a block. Three cases — the patch applies,
each variable is offset exactly once (`OpIAdd` count 2), and patching twice still
doubles (count 4), which records why the caller's guard is load-bearing.

- **P57** — with the fix, the swapchain's settled samples `DIFFER` with layer 1
  at roughly layer 0's fill, and `score_run.py` grades **STEREO** rather than
  DUPLICATE. On screen: a right eye with 3D in it that is not a copy of the
  left. Refuted if layer 1 returns to ~4% (something else also reads a twin that
  was never written) or stays bit-identical (the offset is reaching the wrong
  variable).

## Take fifty (control): the per-view sampling is exact

`X4VR_STEREO` omitted with `X4VR_BINDLESS_PATCH=1` — the shear absent while
view 1 still samples twin slots. Any difference between the eyes here cannot be
parallax; it can only be the sampling.

Result: **every probed image IDENTICAL.** `#57`, `#60`, `#61`, `#63`, `#65`,
`#66`, `#67`, `#97`, `#98`, and 19 of 19 settled swapchain samples bit-exact.

So every twin descriptor resolves to content identical to what view 0 reads.
The mirror, the offset, and the aliased-variable fix are correct — not
"apparently working", but bit-exact over a full scene. This is the control that
take forty-nine's parallax could not provide, and it is worth having spent a run
on: it converts "the stereo looks right" into "the sampling is provably not the
variable".

### What that leaves for the cockpit lighting

The difference in take forty-nine is therefore caused by the eye offset. The
mechanism is worth stating precisely, because it bounds what can and cannot
differ:

`K` is applied to `gl_Position` at the **end of the vertex shader**. Nothing
else moves — world positions, normals and view vectors reaching the fragment
shader are still the ones X4 computed for its single camera. So **direct
lighting cannot differ between the eyes.** What can:

* **visibility** — a strut occludes different geometry in each eye, which is
  what stereo is;
* **screen-space effects** — anything sampling a screen-space buffer lands
  somewhere else once the geometry has shifted. X4 has several: `#63`
  (`R8_UINT` full-res, 2 mips — a tile/cluster light classification buffer,
  `DIFFER` 18% in take forty-nine) and `#66`/`#67` (352², `DIFFER` 13–18%).

Near-field cockpit geometry is where both are largest, and it is exactly where
Patola saw it. A screen-space term evaluated per eye is *legitimately* per-eye
and can still look wrong, which is the usual reason stereo screen-space effects
are a known problem rather than a solved one.

### The parameters that scale it, and are currently assumed

```
stereo: ipd=0.0640 sx=0.8890 near=0.100 -> shear m8 L=0.28448 R=-0.28448
```

`ipd` is a human 64 mm. `sx` and `near` are **defaults**, and the shear is
`d·sx/near` — so if X4's real near plane is not 0.1, the parallax is wrong
everywhere, in proportion, and most visibly on the closest geometry. That has
never been checked against X4's actual projection matrix.

- **P58** — the cockpit lighting difference scales with `X4VR_IPD`. At
  `X4VR_IPD=0.016` (a quarter) it is roughly a quarter as pronounced and the
  rest of the scene keeps its depth. Confirms the difference is geometric —
  visibility plus screen-space reprojection — and that the remaining question is
  calibration, not correctness. Refuted if the strut still flips dark-to-bright
  at a quarter of the separation, which would mean something view-dependent that
  is not the offset.

## Take fifty-one: P58 confirmed — the difference is geometric

`X4VR_IPD=0.016`, a quarter of the default. Patola: "the shadows are only
slightly different, as I would expect from stereo view."

| image | ipd 0.064 | ipd 0.016 |
|---|---|---|
| `#57`, `#60`, `#61` | 31.8–31.9% | 22.7–26.9% |
| `#63` (tile/light classification) | 18.0% | 9.6% |
| `#66`, `#67` (screen-space, 352²) | 13.1 / 18.3% | 3.6 / 7.1% |
| `#97`, `#98` | 6.0 / 9.3% | 0.16 / 1.2% |

Every image drops, and the screen-space buffers drop hardest — `#97` by a factor
of 37. `DIFFER` counts how many pixels differ, not by how much, so it should not
scale linearly with separation; monotonic is the prediction and monotonic is
what happened.

Together with take fifty this closes task #22. The per-view sampling is
bit-exact; the eye offset is the only source of difference; and the magnitude
tracks the separation. Nothing here is a defect. **What is left is calibration.**

### The calibration gap, stated plainly

```
stereo: ipd=0.0640 sx=0.8890 near=0.100 -> shear m8 = d*sx/near = ±0.28448
```

`ipd` is a chosen physical quantity. `sx` and `near` are **defaults nobody has
checked against X4's projection matrix**, and they are not cosmetic:

* the shear is `d·sx/near`, so an error in either scales the parallax at *every*
  depth by a constant factor;
* on a flat screen that reads as "the stereo is a bit strong or a bit weak". In
  an HMD it is the difference between a cockpit that feels the right size and
  one that feels like a doll's house or a cathedral, and wrong convergence is a
  direct cause of discomfort rather than a matter of taste;
* the artifact Patola noticed at 0.064 was strong enough to be worth reporting,
  which is weak evidence that the current constant is too large.

X4 uploads its projection matrix in a constant block the layer already reads for
other purposes. Measuring `sx` and the near plane from it, rather than assuming
them, converts the eye offset from a tuned number into a derived one. That is
task #23, and it should land before any HMD work — an HMD run calibrated against
assumed constants would produce comfort complaints that look like mod bugs.

## Task #23, before the measurement: what `sx` should be

The sentence above — "weak evidence that the current constant is too large" —
is the first thing to go. It was reasoning from a screen artifact to a
constant, and reading the record instead points the other way.

**`sx` and `near` were never assumed.** They were *measured*, in Phase 3:

> Read the view block | **Works.** Values are live and correct: the
> projection's aspect tracked the swapchain exactly (1.778/0.889 = 2.000 at
> 2816×1408; 1.778/0.744 = 2.39 at 3440×1440).

That is the whole answer sitting in the table. `sy` is fixed by the vertical
FOV and `sx = sy/aspect` — 1.778/2.000 = 0.889 at 2816×1408, 1.778/2.389 =
0.744 at 3440×1440. The number tracks the aspect, and it was measured when X4
rendered a **2:1** frame.

X4 no longer renders a 2:1 frame. It renders one eye:

```
take51:100  layer  swapchain created: 1408x1408 ... (pid 3492131)
take51:173  inject sdl: SDL_GetWindowSize -> 1408x1408 (pid 3492131)
```

Both numbers X4 could derive an aspect from say **1408×1408**. (The
2816×1408 at pid 3492088 is gamescope's window, not X4's.) The injector
overrides `res_width`/`res_height` and sets no FOV or aspect key, so nothing
else is in play.

### P59 — the prediction

Run the tagged `stage2-stereo-verified` command with `X4VR_DUMP_MATRICES=1`
added and nothing else changed. The dump is read-only: `patch_view_before_submit`
returns before every write when the test offsets are zero.

1. `M_projectionUJ` reads **sx ≈ 1.778**, sy ≈ −1.778, near ≈ 0.1,
   m[11] = ±1, m[15] = 0, column-major. Aspect 1.0, so `sx = sy`.
2. Therefore the shear we bake is **half** the correct magnitude: `proj SHEAR`
   reports `baked is 0.500x`. Every stereo run so far, including
   `stage2-stereo-verified`, has been rendering an effective IPD of half its
   configured value — take 51's `X4VR_IPD=0.016` was geometrically 8 mm.
3. `near` is unchanged at 0.1. It does not track the aspect and nothing in the
   config touches it.
4. `M_projection` and `M_projectionUJ` are **equal**, because the injector sets
   `antialiasing: none` and TAA jitter is what distinguishes them. If they
   differ, jitter is live in m[8]/m[9] — the shear's own slots — and that is a
   separate problem worth knowing about before it becomes a mystery.

**What would refute it.** `sx ≈ 0.889` in the live block means X4 computes its
projection aspect from something that is still 2:1 — most likely the surface
rather than the swapchain — and the current constant is right for the wrong
reason. That is a *better* outcome to discover now than after an HMD run, and
it would immediately implicate the same window/render disagreement that task
#21's clipped logo is suspected of.

### Why this is not "the artifact means the shear is too strong"

Patola's cockpit-lighting report was screen-space effects disagreeing between
eyes, which take 50 and take 51 established is the *correct* behaviour of true
stereo in a deferred renderer, not an over-strong offset. A geometrically exact
64 mm separation produces exactly that. Reasoning from "it looked like a lot"
to "the constant is too big" skipped the step where the constant is checked
against the matrix it was derived from, and the check is cheap.

### Offline first

`tests/view_math.cpp` (new, 22 cases) asserts the arithmetic before any of it
is claimed about X4:

* `read_proj_terms` extracts sx/sy/near in **both** storage orders — a
  transposed read returns 1.0 for `near`, which is plausible enough to pass
  unnoticed, so it is tested rather than trusted;
* it refuses on `Major::Unknown` and on a zeroed block instead of guessing;
* `make_eye_shear`'s single-term shortcut equals `P·T(−d)·P⁻¹` computed through
  the general `mul`/`invert` helpers, at both aspects and both IPDs. The
  shortcut is only valid for X4's projection shape, so it is checked against
  the long way rather than assumed;
* the wide-eye shear reproduces the logged `0.28448` exactly, and the
  square-eye shear is exactly 2× it. That last case is the arithmetic behind
  P59, asserted so the claim cannot quietly stop being true.

If the measurement confirms P59, the confirmation run needs **no code change**:
`X4VR_PROJ_SX=1.778` is an existing override. Only once the number is proven on
screen does the layer start reading it live.

## Take fifty-two: P59 refuted — sx is 1.333, and sy is not a constant

```
M_projection    [   1.333   -0.000    0.000    0.000 | ... |    0.000   -0.000    0.100    0.000]
M_projectionUJ  [   1.333   -0.000    0.000    0.000 | ... |    0.000   -0.000    0.100    0.000]
storage order detected: column-major (draws=216)
proj MEASURED: sx=1.33333 sy=-1.33333 near=0.10000 (jittered sx=1.33333 near=0.10000)
proj ASSUMED : sx=0.88900 near=0.10000
proj SHEAR   : measured |m8|=0.10667 vs baked |m8|=0.07112 -> baked is 0.667x
```

Scored honestly:

| P59 claim | verdict |
|---|---|
| `sx ≈ 1.778` | **REFUTED** — 1.33333 |
| baked is `0.500x` | **REFUTED** — 0.667x |
| `near` unchanged at 0.1 | confirmed |
| `M_projection` == `M_projectionUJ` (no TAA jitter) | confirmed, bit-for-bit |
| aspect 1.0, so `sx == abs(sy)` | confirmed — 1.33333 both |
| baked shear too *small*, not too large | confirmed; the retraction stands |

The run itself scores PASS/MIXED and tracks take fifty-one image for image, so
the read-only dump disturbed nothing.

**What I got wrong.** The reasoning — `sx = sy/aspect`, aspect is now 1.0 — was
right, and the measurement confirms it. The error was one step behind: I treated
`sy = 1.778` as a constant of the engine. It is not. Three data points now
exist, and only the first two share an `sy`:

| aspect | sx | sy |
|---|---|---|
| 2.000 (2816×1408) | 0.889 | 1.778 |
| 2.389 (3440×1440) | 0.744 | 1.778 |
| 1.000 (1408×1408) | **1.333** | **1.333** |

Had I "confirmed" P59 by setting `X4VR_PROJ_SX=1.778` — which is exactly what
the previous section proposed as the cheap next step — the parallax would have
been 33% too *large*, having started 33% too small. The wrong fix was one
command away and would have looked like progress.

### A model for X4's FOV rule, fitted after the fact

All three rows fit `aspect_eff = max(aspect, 4/3)`, `sx = 1.778/aspect_eff`,
`sy = sx·aspect`:

* 2.000 → sx = 1.778/2.000 = 0.889, sy = 0.889·2.000 = 1.778 ✓
* 2.389 → sx = 1.778/2.389 = 0.744, sy = 0.744·2.389 = 1.778 ✓
* 1.000 → sx = 1.778/1.333 = 1.333, sy = 1.333·1.000 = 1.333 ✓

In words: **X4 never computes a horizontal FOV narrower than a 4:3 monitor
would give** (73.74°), and derives the vertical from the true aspect. `1.778`
is 16/9, so the underlying setting is a 90° horizontal FOV at 16:9.

This is three points and one free parameter, fitted *after* seeing the answer.
It is a hypothesis, not a finding, and it is recorded as one. The test that
would settle it: render at an aspect between 1.0 and 1.333 — 1408×1200 gives
1.173 — where the model says sx stays clamped at 1.333 while an unclamped
`sy/aspect` says 1.516. Those are far enough apart that one dump decides it.

**Why it matters beyond curiosity.** An HMD's per-eye aspect is near 1:1, which
is inside the clamp. If the model holds, X4 will hand us 73.7° horizontal no
matter what eye size we ask for, and a headset wants half again as much. That
is a Stage-3 problem, filed as task #24, not something to solve here.

### The real conclusion: the constant cannot be baked from a guess

The shear is baked into shader modules at `vkCreateShaderModule`. In take
fifty-two that happened at t=564227.5; the projection first became readable at
t=564253.7. **Twenty-six seconds earlier, and X4 had no camera yet.** So there
is no startup-time read that could feed the bake, and the layer cannot compute
`sx` itself without a model of X4's FOV rule — a model that, on this evidence,
is exactly the kind of thing I get wrong.

Two options remain, and the second depends on a fact not yet in evidence:

1. **Read `sx` in the shader.** The camera block is `BLOCK_BUFFER_BINDING_SLOT_CAMERA`
   at **set 1, binding 0**, member 1 = `M_projection` (member 3 =
   `M_projection_uj`), declared with debug names in **366 of 409** dumped
   modules. X4 ships combined vertex+fragment modules — 394 of 409 carry a
   Vertex entry point — so the block a fragment stage uses is in the same
   module the vertex patch edits. Uniforms need no entry-point interface
   change, so the patch would add one load and a multiply. Self-calibrating,
   and correct even if the FOV moves.
2. **Keep baking, from the measured number.** Cheaper and already possible via
   `X4VR_PROJ_SX`, but only correct if `sx` never changes during a session.

X4 has a zoom. Whether that zoom moves the projection is the question that
picks the option, and it is a measurement, not an argument.

### P60 — take fifty-three

Same command as take fifty-two plus `X4VR_PROJ_SX=1.3333`, and a dump that now
reports the terms **on every change** rather than once. IPD stays at 0.016: the
number under test is `sx`, and changing the separation at the same time would
confound the only two things this run can show.

1. `proj SHEAR` reports `baked is 1.000x` — the baked and measured shears agree.
   This is arithmetic, not a discovery; if it fails, the override does not reach
   the bake and nothing else in the run means anything.
2. Every image's `DIFFER` rises against take fifty-two, none falls. The shear
   goes up 1.5×, and take fifty-one established the relationship is monotonic
   but far from linear, so a rise well under 1.5× is expected. The screen-space
   buffers (`#66`/`#67`, `#97`/`#98`) should rise proportionally hardest, the
   mirror of how they fell hardest when the IPD was quartered.
3. The shear-off control is not re-run here; take fifty proved the
   sampling path, and this run changes only a scalar the shear is built from.
4. **The open one: does `sx` move?** I expect at least one `proj CHANGED` line
   during a session that includes zooming, opening the map, and entering an
   external view — X4 exposes a zoom, and a zoom that does not touch the
   projection would be unusual. If forty changes appear, option 2 is dead and
   the in-shader read is the only correct answer. If `sx` holds at 1.33333 for a
   whole session including zoom, baking the measured value is legitimate and far
   cheaper, and the in-shader patch can be deferred behind more valuable work.

I am deliberately not predicting which. The previous prediction failed on an
assumption I had no measurement for, and this is the same shape of assumption.

## Take fifty-three: P60 confirmed — sx moves by 33×, so it cannot be baked

Patola flew a full session: cockpit, zoom in and out, map, external view on a
passing ship with zoom and rotation, then external view on his own ship.

**The log holds two X4 sessions** (`env: run =` at lines 52 and 2374 — the game
was launched twice into the same file). `X4VR_LOG` appends, so it was split
before anything was read from it. Both halves score PASS/MIXED independently.
The numbers below are session 1 only.

| P60 claim | verdict |
|---|---|
| `proj SHEAR` reports `baked is 1.000x` | confirmed — measured 0.10667 vs baked 0.10666 |
| every image's `DIFFER` rises, none falls | confirmed |
| screen-space buffers rise proportionally hardest | confirmed |
| does `sx` move? | **it moves by a factor of 33** |

DIFFER, take fifty-two → take fifty-three, worst settled sample per image:

| image | t52 | t53 | change |
|---|---|---|---|
| `#57`, `#59`, `#60`, `#61` (G-buffer) | 26.1–28.5% | 27.1–29.1% | +2% |
| `#63` | 9.70% | 11.73% | +21% |
| `#66` / `#67` | 3.65 / 8.23% | 5.41 / 10.61% | +48 / +29% |
| `#97` | 1.59% | 2.37% | +49% |

The 1.5× shear produced a rise well under 1.5×, screen-space hardest, exactly
the mirror of take fifty-one. Caveat worth stating: the scene content differed
between the two runs (this one was full of zooming), so this is weaker evidence
than take fifty-one's, which held content roughly fixed. It agrees with the
prediction; it does not carry it alone.

### The answer: sx is not a constant

```
proj CHANGED  #5: sx 1.33333 ->  1.15174   (correct |m8| 0.09214, baked 0.10666)
proj CHANGED  #9: sx 1.33333 ->  3.78085   (correct |m8| 0.30247, baked 0.10666)
proj CHANGED #10: sx 3.78085 -> 37.75372   (correct |m8| 3.02030, baked 0.10666)
```

Observed values: 1.15174, 1.33333, 3.78085, and a cluster from 31.98 to 37.75.
That is a **33× range in ordinary play** — 73.7° of horizontal FOV down to
about 3°. `near` held at 0.1 throughout for every one of them.

At full zoom the correct shear is 3.02 against a baked 0.10666: **the baked
constant is 28× too small**. Option 2 from take fifty-two — bake the measured
value — is dead. Not marginal, not a tuning question. Dead.

Both sessions hit the 40-change cap, so 33× is a floor on the range, not a
measured maximum.

### A second reason, which is the stronger one

Two changes in session 1 report `near` of 0.622 with `sx` near zero
(0.00019, 0.00123). Those are not the main perspective camera — 37 of the 39
samples read `near = 0.1`, these two do not.

Session 1 was cockpit and zoom with an F2 external view and **no map**;
session 2 was the full sequence including the map. So the map view is not the
source of these blocks — they appear in the run that never opened it.

So the layer's "most-drawn range-1792 block wins" heuristic sometimes credits a
block that is not the camera the geometry is drawing through. For a *diagnostic*
that is a curiosity. For a mechanism that feeds the shear it would be a bug, and
one that fires intermittently and looks like a rendering glitch.

**A shader does not have this problem.** Each draw reads the camera block that
is actually bound to it, which is by construction the right one. The in-shader
read is not merely self-calibrating — it removes a guess the layer cannot make
reliably from the outside.

### The patch is smaller and cheaper than the one it replaces

The measured matrix has `m[10] = 0`, so clip z is the constant near plane for
every vertex, and the shear's single term collapses:

```
x_c' = x_c + (-sx·d/near)·z_c = x_c - sx·d          (z_c ≡ near)
```

**`near` cancels.** The shader needs one scalar — `M_projection[0][0]` — and
the eye offset `d`, which is our own choice and stays baked. So the appended
code is one load, one multiply, one subtract, replacing today's full `mat4`
multiply of 16 muls and 12 adds. The correct version is *cheaper* than the
wrong one, which for once makes "performance is king" and correctness the same
choice.

Asserted offline in `tests/view_math.cpp`, against `K·p` for points that came
through P — an arbitrary vector is not a clip position and the two forms
legitimately disagree on one, so testing arbitrary vectors would assert the
wrong property.

The camera block is `BLOCK_BUFFER_BINDING_SLOT_CAMERA`, **set 1, binding 0**,
member 1 = `M_projection`, and its member order matches `x4vr_view.hpp`'s
offsets exactly (member 0 `M_view`, 2 `M_invprojection`, 3 `M_projection_uj`,
7 `M_viewprojection`, 8 `M_viewinverse`, 9/10 the shadow cascades). Of the 341
modules that declare `M_worldviewprojection`, **323 also declare the camera
block**; the remaining 18 keep the baked constant as a fallback.

### P61 — take fifty-four, the live sx on screen

`X4VR_PROJ_LIVE=1` added to take fifty-three's command and nothing else
changed. `X4VR_PROJ_SX=1.3333` stays: it no longer reaches world geometry, but
it is still the best constant for the handful of modules that have no camera
block, so leaving it in changes one variable rather than two.

1. The log reports `live-sx=N baked-sx=M` with **M a small minority** — 18 of
   341 world modules in the dump lack a camera block, so roughly 5%. `N = 0`
   would mean the patch never fired and everything silently fell back to the
   bake; the counters exist so that cannot pass as success.
2. **No `driver rejected patched module` lines.** 366 of X4's real modules were
   patched and validated offline under `--target-env vulkan1.2`, 290 of them
   with the bindless fragment patch applied first, so a rejection here means
   the driver disagrees with spirv-val and the offline evidence was worth less
   than it looked.
3. **At rest, take fifty-four should look like take fifty-three.** This is the
   strong oracle and the reason take fifty-three was worth running: it baked
   `sx = 1.3333`, which *is* the live value at the default FOV. So in the
   cockpit, not zooming, the two runs should produce closely matching `DIFFER`
   figures. A large difference at rest means the shader is reading the wrong
   scalar — right magnitude, wrong provenance is exactly the failure this step
   could introduce.
4. **Under zoom they must diverge.** Take fifty-three's parallax is frozen at
   the unzoomed value; take fifty-four's scales with `sx`. Zooming in should
   keep the stereo depth looking right where before it went flat.
5. The shear-off control (omit `X4VR_STEREO`) stays bit-identical. `have_k` is
   false there, so the live path is skipped entirely — it is gated on the same
   flag.

**A warning that is not a bug.** At full zoom `sx = 37.75` and the correct
shear is 3.02 against 0.107 unzoomed — roughly 28× the separation. That is
geometrically right: magnify the image 12× and you magnify its parallax too,
which is exactly what binoculars do and why they are uncomfortable. If deep
zoom looks or feels excessive in this run, the correct reading is "correctness
achieved, comfort is a separate question", and clamping the offset under zoom
is a decision to take deliberately later rather than a number to quietly tune
now. Recording that *before* the run so a strong effect does not get mistaken
for a regression.

## Take fifty-four: the live sx runs — and the diagnostic that justified it is unreliable

```
patched vertex shader #350 (world, per-view) [... live-sx=372 baked-sx=16]
driver rejected patched module: 0
score: PASS, grade MIXED, 152 settled samples, 8 bit-identical
```

| P61 claim | verdict |
|---|---|
| `baked-sx` a small minority (~5%) | confirmed — 16 of 388, 4.1% |
| no driver rejections | confirmed — the offline validation held on real hardware |
| at rest, take 54 matches take 53 | **untestable as set up** (below) |
| under zoom the two diverge | **not demonstrated** (below) |
| shear-off control stays bit-identical | not re-run |

Patola: *"I did the zoom twice. I was unable to notice any parallax. If there
was, it was subtle. Probably because the objects were very distant."*

That reading is correct, and the arithmetic agrees with it — see below. But
reconstructing the session to check it turned up something more important.

### `sx = 1.33333` was the menu, not the camera

The session timeline, from the change log:

```
   +0s   1.33333          <- first credited frame, 21s after first present
  +96s   1.15174 -> 3.78085   <- savegame loaded
 +96..+1578s   3.78085     <- the whole of gameplay, ~25 minutes
+1583s   17.7 -> 25.2 ramp <- the zoom, about one second
+1587s   37.75372
+1590s   CAP HIT
```

**`1.33333` holds only for the first ninety-six seconds** — the menu. Every
value measured in takes fifty-two and fifty-three was taken from that same
early window (take 52's single sample, take 53's first). So `X4VR_PROJ_SX=1.3333`,
tagged as `stage2-sx-measured` and described as "the value read out of X4's own
projection", is **the menu's projection**. During gameplay the credited block
reads 3.78085 — 2.8× larger.

This also undercuts P61's third claim: take 53 baked the menu's `sx`, so "at
rest they should match" was comparing take 54's gameplay against a constant
that was only ever right for the menu. There were two at-rest samples to test
it with, which is not enough to conclude anything either way.

### And the credited block is not reliably one camera

Interleaved with those values: 0.00123, 0.00841, 0.06984, and 1.00000 exactly.
Those are not one camera zooming. X4 renders more than one view per frame — the
cockpit target monitor is the obvious candidate — and the layer's "most-drawn
range-1792 block wins" heuristic credits whichever won that frame.

So the 33× range recorded for take fifty-three is real, but **"sx moves 33×
under zoom" overstates what was measured**. What was measured is that the
credited block's `sx` moves 33×, and some of that is the heuristic changing its
mind about which camera to report rather than any camera changing.

**The conclusion survives, and gets stronger.** A single baked constant cannot
serve several cameras that differ *at the same instant* — the main view, the
target monitor and the shadow cascades have different projections in the same
frame. No constant is right for all of them, and no smarter heuristic fixes
that, because the problem is that there is more than one right answer at once.
Only a per-draw read can be correct, which is what the shader now does. The
weak argument (zoom) has been replaced by a stronger one, and the fix was right
for a reason better than the one it was built on.

### Why no parallax was visible, in numbers

The shear is a clip-x shift of `sx·d`, and the NDC shift at view depth z is
`sx·d/z`. With `X4VR_IPD=0.016`, d = 0.008:

| configuration | sx | clip shift `sx·d` |
|---|---|---|
| take 51 (baked 0.889, ipd 0.064) | 0.889 | 0.0284 |
| take 54 (live, gameplay, ipd 0.016) | 3.78 | 0.0302 |

**Take fifty-four produced almost exactly take fifty-one's separation.** The
live `sx` is 4.25× larger and the IPD is 4× smaller, and the two nearly
cancelled. Nothing was going to look different, and the run cannot distinguish
"the live path works" from "the live path does nothing" by eye.

Distance compounds it: parallax falls as 1/z, and zoom is used to look at
things that are far away, so the deep-zoom moment had both a large coefficient
and a large z. Patola's own explanation was right.

### `DIFFER` cannot measure this, and should stop being asked to

`DIFFER` counts pixels that differ, not by how much. In a detailed scene a tiny
offset changes nearly every pixel; against distant stars a large one changes
few. At deep zoom several images went *down* (`#61` 28.70% → 7.69%, `#59`
26.71% → 9.17%) while `#63`, `#66`, `#67`, `#97` and `#98` did not move at all.
That is content, not parallax.

It was the right instrument for take fifty-one, which held the scene roughly
fixed and changed only the IPD. It is the wrong one for comparing across
sessions and across zoom levels, and reading it that way would have manufactured
a conclusion. Recorded so the next comparison does not reach for it by reflex.

### Fixed here

* The change cap was 40 and fired ten seconds into the only deep zoom of the
  session, so every probe sample during that zoom had no `sx` to attribute it
  to. Raised to 400, plus a `proj STEADY` heartbeat every 30 s — without one,
  "unchanged since t" and "logging stopped at t" look identical in a log.
* Task #24's 4:3 clamp model was fitted to `sx = 1.333` at aspect 1.0. If that
  is the menu's projection, the model may describe the menu and not the game.
  The task is amended rather than deleted: the clamp test it proposes is still
  the right experiment, it just has to be run against a gameplay camera.

### P62 — take fifty-five, the first run that can actually be seen

Take fifty-four could not show parallax because the live `sx` and the quartered
IPD nearly cancelled. So the next run restores the IPD to a human one and
changes nothing else:

```
X4VR_IPD=0.064   (was 0.016)
```

Expected clip-x shift at the gameplay `sx` of 3.78: **0.121**, against take
fifty-four's 0.030 and take fifty-one's 0.028. Four times more separation than
anything rendered so far.

1. Parallax is **clearly visible on near geometry** — cockpit frame, console,
   a station or ship at close range. Not on distant objects: the shift is
   `sx·d/z`, and at 10 km there is nothing to see at any IPD.
2. `live-sx` stays a large majority with no driver rejections, as in take 54.
3. The run still scores PASS.
4. `proj STEADY` lines now appear during quiet stretches, and the 400-change
   cap does not fire, so every probe sample has an `sx` to attribute it to.

**What would refute it:** no visible parallax on near geometry at 64 mm. At
that separation the shift is 12% of clip x at one metre, which is not subtle.
If it is still invisible, the shear is not reaching the geometry that matters
and the `live-sx=372` counter is measuring modules patched rather than modules
*used* — a distinction this run would expose and the counters currently cannot.

**Judgement, not measurement, is what this run is for.** `DIFFER` will not
settle it (take 54 established it measures pixels changed, not magnitude), and
there is no offline oracle for "does this look like the right depth". Patola
looking at near geometry is the instrument. The question to answer is not "is
there parallax" but "does the cockpit sit at a plausible distance" — and if it
looks too strong, that is the calibration signal the project has been missing
since the IPD was first set, not a fault.

## Take fifty-five: P62 half-confirmed, and task #22 was closed wrongly

`X4VR_IPD=0.064` with the live `sx`. Run ran as configured — one session,
`live-sx=284 baked-sx=12`, no driver rejections, gameplay `sx = 3.78085`, so
the clip shift was 0.121 as predicted.

Patola, looking inside the cockpit with the numpad views: *"I couldn't feel any
depth, only tried to gauge by eyes the subtle differences in angle of the
elements. Saw a couple of differences. But then I have noticed one issue —
recall that I complained about the cockpit lighting that was very different
between the two frames? We have that again. In some corners of the cockpit, the
shadows are very different… Most of the shadows match, but there are some very
clear ones that don't."*

### The argument that should have closed #22, pointing the other way

**Shadows are view-independent.** A shadow edge lies on a surface. In correct
stereo it stays on the same *surface point* in both eyes and appears at a
different screen position along with the geometry it sits on. Two eyes
disagreeing about where a shadow falls on a wall is not parallax. It is a
defect, and it always was.

Task #22 was closed after takes fifty and fifty-one on the grounds that the
difference "scales with the IPD, therefore geometric". That inference does not
hold: the bug below scales with the IPD too. The evidence never discriminated
between the two explanations, and it was treated as though it had.

### The mechanism, from X4's own shaders

The eye shear moves `gl_Position` and nothing else, so geometry rasterizes
where the offset eye would see it — correct. The deferred pass then:

1. reads the depth buffer at a screen pixel;
2. reconstructs view position with camera-block member 2, `M_invprojection`,
   which is **centre-frame**. Reconstructing a *sheared* pixel with the centre
   inverse recovers the position in **that eye's** frame;
3. looks up the shadow cascades with `M_shadowCSM*Clip` — **centre-frame**.

Surface position and shadow transform therefore disagree by the eye offset:
±32 mm at IPD 0.064, **64 mm between the two eyes**. On cockpit geometry a
metre away that is a large, obvious shadow displacement, which is exactly what
is on screen.

Counting access chains into the camera block across all 409 dumped modules:

| member | name | modules using |
|---|---|---|
| 2 | `M_invprojection` | **19** |
| 13 | `V_cameraposition` | 296 |
| 0 | `M_view` | 268 |

**All 19 of the `M_invprojection` users also reference `shadowCSM`.** They are
the deferred lighting-and-shadow passes, and they are exactly the passes the
mechanism predicts.

> **These counts are wrong — corrected in take fifty-six below.** The scan that
> produced them examined only the *first* camera-block variable per module, and
> X4's combined modules declare that block once per stage. The real figure for
> `M_invprojection` is **247 modules (244 fragment, 3 compute)**, not 19. The
> diagnosis is unaffected — if anything the mechanism is far more pervasive
> than this table claimed — but the numbers are not to be quoted.

### The discriminator was already in the data

| take | sx | d | shear = sx·d | artifact |
|---|---|---|---|---|
| pre-51 (ipd .064, sx .889) | 0.889 | 0.032 | 0.0284 | **yes** |
| 51 (ipd .016, sx .889) | 0.889 | 0.008 | 0.0071 | no |
| 54 (ipd .016, sx 3.78) | 3.78 | 0.008 | 0.0302 | not seen |
| 55 (ipd .064, sx 3.78) | 3.78 | 0.032 | 0.1210 | **yes** |

Take fifty-four's shear is within 6% of pre-fifty-one's, yet one shows the
artifact and the other does not. What differs between them is `d`, by 4×.

**Correct stereo geometry scales with the shear `sx·d`. A reconstruction offset
scales with `d` alone.** The artifact tracks `d`. That is the signature of the
bug and not of parallax.

Weak point, stated rather than buried: take fifty-four's "no artifact" is
Patola not mentioning one while looking at distant objects through a zoom, not
a deliberate inspection of cockpit shadows. It is suggestive, not conclusive.
The shader evidence is what carries this.

### What the fix has to do

`clip_sheared = K·clip_centre` with `K = P·T(−d)·P⁻¹`, so recovering the centre
frame from a sheared pixel needs `T(d)·M_invprojection` in place of
`M_invprojection`. In column-major terms that is one row-combine:

```
result[0][c] = M[0][c] + d · M[3][c]        for c = 0..3
```

selected per view by `gl_ViewIndex`, the same way the vertex patch already
selects `d`. Nineteen modules, all fragment or fullscreen, and the correction
is applied at the point the matrix is loaded rather than at the point the
position is used — locating "the reconstructed position" in arbitrary shader
code is not tractable, but locating a load of member 2 is.

**Not to be conflated with it:** `V_cameraposition` (member 13, 296 modules) is
also centre-frame, so per-eye specular is currently missing. That makes the two
eyes *more* alike, not less, so it cannot be what Patola is seeing. It is an
inaccuracy to fix later, and mixing it into this diagnosis would be the same
mistake as before — reaching for a nearby explanation because it is nearby.

### P62 scored

| claim | verdict |
|---|---|
| `live-sx` a large majority, no rejections | confirmed — 284 vs 12, zero |
| run scores PASS | confirmed |
| parallax clearly visible on near geometry | **no** — "couldn't feel any depth… a couple of differences" |
| `proj STEADY` present, cap does not fire | confirmed |

The third is unresolved rather than refuted: judging stereo depth on a
flatscreen SBS pair by eye is close to impossible, which is a limitation of the
instrument and not a result about the mod. Patola said so plainly — *"as I am
not in VR, I couldn't feel any depth"*. The honest position is that **no run so
far has been able to confirm the depth is right**, and none will until either
an HMD or a proper A/B of the same frame at two separations exists.

## The invprojection patch — and the aliased-binding bug, for the second time

`patch_fragment_invproj_eye` rewrites every load of `M_invprojection` in a
fragment stage to `T(d)·M_invprojection`, per view. SSA is preserved by giving
the load a fresh id and letting the final `OpCompositeInsert` take over the
original result id, so every existing use downstream picks up the corrected
matrix and not one use site is rewritten.

### The bug I wrote, which I had already fixed once

The first version matched the camera block by (set, binding) and **took the
first match**. It patched 106 modules. The correct number is 244.

X4's combined vertex+fragment modules declare the camera block **once per
stage**, as two variables aliased onto the same (set, binding) — `mod-0100` has
`%__1` and `%__3`, both `BLOCK_BUFFER_BINDING_SLOT_CAMERA`, both set 1 binding
0. They address the same buffer, so a patch that only *reads* the block can use
either handle. This patch rewrites existing **loads**, and those name whichever
variable their own stage declared. First-match silently skipped every module
whose fragment stage happened to use the other one.

**This is take forty-eight's bug, in code I wrote after documenting take
forty-eight's bug.** Same shape: first-match on an aliased binding, legal,
which X4 does, and which reads as correct until something downstream comes back
wrong. It is now recorded in the function's own comment, because a comment in
another function two hundred lines away did not stop me writing it again.

### The counts in the take-55 section are wrong

The scan behind "19 modules use `M_invprojection`" had the same defect — it
took the first camera variable per module — plus it matched index constants by
*name*, and X4 emits duplicates (`%int_2` and `%int_2_0` both hold 2).
Recounted across every camera variable, resolving constants by id:

| member | name | modules | by stage |
|---|---|---|---|
| 0 | `M_view` | 306 | 262 vertex, 160 fragment |
| 1 | `M_projection` | 32 | 22 vertex, 8 fragment, 2 compute |
| 2 | **`M_invprojection`** | **247** | **244 fragment**, 3 compute |
| 8 | `M_viewinverse` | 16 | 16 fragment |
| 11 | `V_viewportpixelsize` | 254 | 244 fragment, 10 vertex |
| 13 | `V_cameraposition` | 296 | 296 vertex, 86 fragment |

**247, not 19.** The diagnosis is unchanged and the mechanism is far more
pervasive than the first count suggested: nearly two-thirds of X4's modules
reconstruct position from depth in a fragment stage.

A cross-check worth having: the corrected Python analysis says 244 fragment
users, and the C++ patch — written independently, matching by shape rather than
by debug name — fires on exactly 244. Two different methods, same number.

### Offline evidence

* **244 patched, 165 refused, 0 invalid** under `spirv-val --target-env
  vulkan1.2` against X4's real modules.
* The 3 compute modules refuse, correctly: a dispatch has no `gl_ViewIndex`, so
  there is no `d` to select and picking one eye would be wrong in the other.
  They fall under the project's existing compute gap.
* **All three patches stacked** — bindless index offset, then invprojection,
  then the vertex eye offset — applied to all 409 modules: **0 invalid**.
* Emitted code checked by hand on `mod-0151`: fresh load id, `d = −0.032 +
  view·0.064`, four extract/multiply/add/insert chains, final insert taking the
  original result id.
* 7 new suite cases, including refusals on wrong set, wrong binding,
  out-of-range member, a member that exists but is never loaded, and a compute
  module.

### P63 — take fifty-six

Take fifty-five's command plus `X4VR_PROJ_INVPROJ=1`, nothing else changed.

1. The log reports `invproj … modules corrected` in the low hundreds. Far below
   that means the deferred passes are still lighting the wrong frame wherever
   the run did not reach.
2. No `driver rejected patched module` lines.
3. The run still scores PASS, and geometry parallax is **unchanged** — the
   shear is untouched, so anything that moves in the geometry would mean this
   patch reached something it should not have.
4. **The observable: the cockpit shadows agree between the two eyes.** Same
   corners Patola called out — "in some corners of the cockpit, the shadows are
   very different". Those should now match, while the geometry stays offset.

**What would refute it.** Shadows still differing says one of: the mechanism is
wrong; the 3 compute modules carry the visible part; or light positions need
the same treatment as the reconstruction and correcting one of the two is not
enough. Those are distinguishable, and the first thing to check would be
whether the *residual* is smaller — a partial fix and a wrong theory look
different.

This is the one prediction in the project so far that a flatscreen can settle
cleanly: it is not a question about depth, which take fifty-five showed cannot
be judged without an HMD, but about whether two images agree — and shadows that
disagree are visible precisely because they should not be.

## Take fifty-six: P63 refuted — the mechanism was wrong, on evidence that was an artifact

`X4VR_PROJ_INVPROJ=1`. The patch fired (`invproj final: 238 modules
corrected`), no driver rejections, run scores PASS. Patola: *"Unfortunately the
shadows still clearly disagree."*

The plumbing worked. The diagnosis was wrong.

### The evidence that carried it does not exist

Take fifty-five claimed: *"All 19 of the `M_invprojection` users also reference
`shadowCSM`. They are the deferred lighting-and-shadow passes."* That was the
sentence the whole mechanism rested on, and it is false.

The only occurrence of `shadowCSM` in `mod-0151` is:

```
OpMemberName %BLOCK_BUFFER_BINDING_SLOT_CAMERA  9 "M_shadowCSM0Clip"
OpMemberName %BLOCK_BUFFER_BINDING_SLOT_CAMERA 10 "M_shadowCSM1Clip"
```

Those are **debug names for struct members**, emitted in every module that
declares the camera block, whether or not anything reads them. Across all 409
modules, **no module accesses camera member 9 or 10 at all**. I grepped for a
string and reported it as a reference.

`mod-0151` also has zero `IO_texshadow*` inputs, so it receives no shadow data
by any route.

### Where shadows actually come from

The geometry vertex stage computes `IO_texshadowCSM0..4` and passes them as
interpolated outputs. They are derived from unsheared inputs — the shear
touches `gl_Position` only, at the end of main — so for any given surface point
they are **identical in both eyes**, and they travel with the geometry.

By that reasoning shadows should already agree, and they do not. So the cause
is somewhere I have not looked, and the last two explanations were both
constructed from indirect evidence rather than from the artifact itself.

### What this run does and does not settle

* The invprojection correction is **unvalidated**, not disproven. 244 fragment
  modules really do reconstruct position from depth, and reconstructing a
  sheared pixel with the centre inverse really does yield the eye's frame. That
  error is real; it is simply not what is on screen. The knob stays off by
  default and the patch stays in, labelled as what it is.
* `DIFFER` cannot arbitrate: takes 55 and 56 were different play sessions with
  different content, so the per-image figures are not comparable, and take 54
  already established `DIFFER` measures pixels changed rather than magnitude.

### Method note

Two consecutive wrong diagnoses of one symptom — "it is correct stereo
behaviour", then "it is the invprojection" — both reasoned from shader
archaeology without once looking at the artifact. Take forty-eight cost eleven
takes to the same habit: log archaeology in place of reading the thing itself.
The next step is a screenshot, which is the only instrument that has not been
tried and the one that settled the take-50 question in a single message.

## The screenshots: it is not a shadow, and both diagnoses are dead

Patola supplied two SBS captures. Measured per region, luminance mean, right
eye over left:

| region | shadow1 | shadow2 |
|---|---|---|
| sky / nebula background | **1.002** | **1.001** |
| near hull panel / wing | **1.889** | **1.688** |
| strut | 1.351 | — |
| cockpit floor | 0.956 | 0.851 |
| whole eye | 1.099 | 1.036 |

Cropping the *same* box from both eyes and stacking them settles what kind of
difference it is: the wing's flat panels are dark grey in the left eye and near
white in the right. Same geometry, near enough the same position, radically
different shading. It is **not a shadow edge and not a shift** — it is the
surface's own brightness.

**The background matches to 0.2%.** So this is not exposure, not tonemapping and
not the composite: those move everything, and the sky does not move at all. What
differs is confined to surfaces that shade, and it is not a constant offset
either — the panels are brighter in the right eye while the floor is *darker*.

### Both mechanisms proposed for this are now excluded

* *"Correct stereo behaviour"* (takes 50/51): shadows are view-independent, and
  a 1.9× brightness difference on a flat panel is not parallax.
* *"The deferred passes reconstruct in the eye's frame"* (takes 55/56): the
  evidence for it was `shadowCSM` appearing in `OpMemberName` debug names, and
  nothing in 409 modules reads camera members 9 or 10.

A third guess died before it was proposed: the 2048×2048 D16 shadow cascades
(`#28`–`#32`, `#70`–`#74`) *are* doubled by the layer, which looked promising —
a depth-only pass is unsheared and unmasked, so layer 1 would never be written.
But they are not in the per-eye set. The masked passes' attachments are `#1`–`#4`,
`#50`–`#53`, `#11`, `#54`, `#55`, `#59`, `#60`, `#61`, `#63` — no shadow map
among them, so both views sample the same one and the lookup coordinates come
from the vertex stage identically. Recorded because it was checked, not because
it led anywhere.

### The instrument, instead of a fourth guess

`DIFFER` counts texels that differ. It cannot say *how* they differ, and every
wrong turn on this symptom came from asking it a question it does not answer.
The probe now also reports the mean level of each layer:

```
mv probe: img #61 ... DIFFER 247671/1982464 (12.49%) ... level 0.1832/0.1841 (l1/l0 1.005)
```

Printed on `IDENTICAL` lines too, because an identical image has equal means by
construction and seeing that stated is what makes a ratio elsewhere mean
something. Half floats are decoded inline; the first component is summed, which
for this chain is red — crude on purpose, since the output wanted is *the name
of the first buffer where the eyes diverge in level*, and a photometrically
correct luminance would not make it more of a name.

### P64 — take fifty-seven

Same command as take fifty-six, fresh log, same cockpit view as the
screenshots. The question is only: **at which image does `l1/l0` first depart
from 1.000?**

* The whole-eye ratios were 1.099 and 1.036, so a mean over a full image will
  show a few percent, not 1.9× — the strong difference is local to panels. A
  departure of 2% or more is well clear of noise between two renders of one
  frame.
* If the ratio is 1.000 through the G-buffer and departs at a later composite,
  the cause is downstream of shading.
* If it departs at the G-buffer itself (`#54`/`#57`/`#59`/`#60`/`#61`), the two
  eyes are shading the same surfaces differently and the cause is in the
  geometry pass.
* If **no** image shows a departure, then the level difference is local enough
  to vanish in a full-image mean, and the next step is `X4VR_MV_DUMP_IMG` on the
  G-buffer to look at the two layers directly.

I am deliberately not naming a culprit this time. Three have been proposed for
this symptom and three have been wrong, all from indirect evidence; the point of
this run is to make the next statement about it a measurement.

## Take fifty-seven: the level probe names #57, and the parallax is measurably correct

Per-image `l1/l0` level ratio, cockpit views:

| image | what it is | ratio |
|---|---|---|
| `#59` | albedo, RGBA16F | 1.006 – 1.022 |
| `#61` | normals, RG16F | **1.005** |
| `#60` | normals, RG16F | signed, noisy |
| **`#57`** | **RGBA16F, G-buffer attachment 1** | **1.682, 1.649, 1.848** |
| `#63`, `#65`, `#66`, `#67`, `#97`, `#98` | downstream | 1.000 – 1.015 |
| `#50`–`#53` | swapchain | 1.02 |

`#61` differs in 33–46% of texels — real parallax — while its *level* holds at
1.005. `#57` differs in the same proportion of texels but its level runs up to
**1.848**, matching the 1.889 measured on the hull panel in the screenshot.

**So the G-buffer inputs agree and the lighting term does not.** Same surfaces,
different lighting.

`#57` is `1408×1408 RGBA16F`, attachment 1 of the six-attachment G-buffer pass
(`[#55 depth, #57, #59, #59, #60, #61]`) and the sole target of `rp #31/#32`.

### Nothing is failing to write

```
img #57 writers — masked rp [24,23,25,31,32,53] unmasked rp []
non-empty 541455/538833   missing=120075 changed=419816 extra=117453
```

Every writer is masked, so both layers are rendered. Coverage is near-equal and
`missing` ≈ `extra`, which is what parallax looks like — content leaving one
side and entering the other. The bulk is `changed`: texels present in both and
different. This is not a missing-write bug.

### The parallax itself is measurably correct

Cross-correlating the two halves of the screenshot, best horizontal shift and
how well it matches:

| region | shift | residual |
|---|---|---|
| starfield / nebula (far) | **0 px** | 100 |
| distant ship | −34 px | 213 |
| cockpit strut | −66 px | **17.6** |
| wing panel | −243 px | 1442 (5384 at zero) |
| holo dial | −80 px | 7047 (7680 at zero) |

Two things fall out. The far background sits at **exactly zero** — the shear
falls off with depth correctly, which is the property the whole derivation
exists to produce. And the cockpit strut shifts 66 px with a residual of 17.6,
an almost perfect match: **near geometry parallaxes cleanly and keeps its
appearance.**

The wing panel and the holo dial cannot be aligned by *any* shift. That rules
out "it is just large parallax and I misread it": a shifted copy would align at
some offset, and these do not.

The pattern across both: **bright, emissive or specular surfaces change
appearance; dark matte ones parallax cleanly.**

### P65 — take fifty-eight, a bisection rather than a fourth theory

Three theories have been proposed for this symptom and three were wrong, each
built from indirect evidence. The remaining space splits on one variable, so it
gets split instead of theorised about.

`X4VR_BINDLESS_PATCH=0`, everything else exactly as take fifty-seven. That
removes the per-view texture index offset, so both views sample identical
descriptors, while multiview and the vertex shear still render two different
layers.

* **`#57`'s ratio → 1.000** means the per-view *sampling* is what makes the
  lighting differ, and the cause is in the bindless mirror — a twin descriptor
  resolving to something other than what view 0 reads.
* **`#57`'s ratio stays ≈ 1.7** means the per-view *geometry* is responsible and
  the sampling is innocent.

Either way the answer is one bit, obtained without a theory about which
descriptor or which pass.

Expect the screen to look **mono** — take forty-six established that without the
index offset the composite reads layer 0 for both eyes. That is not a
regression; the probe reads `#57` directly and is unaffected by what the
composite does with it.

## Take fifty-eight: the bisection answers, with a third outcome

`X4VR_BINDLESS_PATCH` removed. P65 offered two outcomes; neither happened.

| image | take 57 (patch on) | take 58 (patch off) |
|---|---|---|
| `#57` | 1.68 – 1.85 | **58.7, 89.7** |
| `#59` albedo | 1.006 – 1.022 | 1.017 |
| `#61` normals | 1.005 | 1.003 |
| `#63`, `#65`–`#67`, `#97`, `#98`, `#50`–`#53` | 1.00 – 1.02 | **exactly 1.0000** |

```
level 0.006706/0.3937 (l1/l0 58.712)
level 0.00585 /0.5245 (l1/l0 89.663)
```

**Removing the per-view sampling made it fifty times worse.** So the bindless
index offset is not the cause of the lighting divergence — it is load-bearing,
and taking it away breaks something badly. The prediction that it would either
fix `#57` or leave it unchanged was too narrow: a knob can be neither the
culprit nor irrelevant, and this one is neither.

The downstream images reading **exactly** 1.0000 is the mono composite, as
expected — take forty-six's behaviour, confirmed here by measurement rather
than by looking at the screen.

### What that plus the framebuffer list says

```
fb  rp #31: 1408x1408 attachments=2 imgs=[#57,#57] MASKED
fb  rp #32: 1408x1408 attachments=2 imgs=[#57,#57] MASKED
```

Both attachments of `rp #31/#32` are `#57`: a pass that **reads `#57` and writes
`#57`**, i.e. an accumulation buffer with a feedback loop.

That explains the 50× explosion mechanically. With the offset on, view 1
accumulates from its own layer and settles 1.7× high. With it off, view 1
accumulates from **view 0's** layer while writing its own — a feedback loop
fed by the wrong input, which diverges rather than settling.

And it reframes the 1.7× residual: an accumulation buffer turns a small
per-frame error into a large steady-state one, so 1.7× need not have a
1.7×-sized cause in any single frame.

### Patola's observation, which is the best clue so far

> "There were some bright surfaces where the black albedo would change its area
> according to the angle I was looking at them. It would be bigger when I was
> looking from the left of the structure and smaller when I looked from centre
> or more to the right."

A dark region on a bright surface whose **area** varies with view angle. That is
a view-dependent shading term — parallax/relief mapping, a reflection, or a
specular lobe — and it is the kind of thing an accumulation buffer would
integrate. It is also strongly nonlinear in view angle, which is what a
steady-state factor of 1.7 between two viewpoints 64 mm apart would need.

Worth stating precisely because it cuts against an earlier claim: the shear
moves `gl_Position` only, so shading inputs are identical between eyes and a
view-dependent term computed from `IO_VertexToEye` should be identical too.
Either the term is *screen-space* (and the shear moves it), or something else
feeds it. That distinction is the next thing to establish, and it is a question
about one pass rather than about the whole frame.

### P66 — look at `#57` instead of reasoning about it

`X4VR_MV_DUMP_IMG=57` writes both layers of `#57` to disk. It is the instrument
that has existed since take twenty-five and has not been pointed at this.

Take fifty-seven's configuration (`X4VR_BINDLESS_PATCH` back on — take
fifty-eight established it is load-bearing), plus the dump.

What each outcome would mean:

* Layer 1 showing the *same* content brighter → a gain or accumulation
  difference, and the pass's own arithmetic is the place to look.
* Layer 1 showing *different content* — a reflection or dark region in a
  different place — → the view-dependent term really is resolving differently
  per eye, and the question becomes what feeds it.
* Layer 1 showing smearing or ghosting → temporal accumulation reprojected with
  the wrong matrices, which would make it a per-eye history problem.

Three outcomes, three different next steps, and no need to guess between them
in advance.

## Take sixty: `#57` dumped — an additive lift in the dark, not a shadow

`X4VR_MV_DUMP=/tmp/x4dump X4VR_MV_DUMP_IMG=57`. (Take fifty-nine set only
`X4VR_MV_DUMP_IMG` and wrote nothing: `X4VR_MV_DUMP` is what enables the dump
and gives it a path. The launcher documents both; the command was written from
memory instead of from the interface, which is the same error as reading
`shadowCSM` out of a debug name.)

Four capture pairs, `n0`/`n1` empty, `n2` and `n3` on the artifact
(probe ratios 1.576 and 1.846).

### What `#57` contains

**Ship geometry only — the background is pure black in both layers.** That
settles why the nebula matched to 0.2% in the screenshots: it never passes
through this buffer, so "the background is identical" was never evidence about
exposure or tonemapping. It was evidence that the artifact lives in a
geometry-only buffer, and it was misread as the opposite.

### After removing parallax, the difference survives

Aligning the layers by best horizontal shift and comparing only texels lit in
both:

| capture | shift | raw ratio | **aligned, lit** | coverage l0 / l1 |
|---|---|---|---|---|
| n2 | −40 px | 1.184 | **1.120** | 34.56% / 35.12% |
| n3 | −27 px | 1.223 | **1.145** | 23.87% / 23.81% |

Coverage is equal to a fraction of a percent, so no geometry is missing from
either eye. The residual 12–15% is a real shading difference.

### It is additive, and it lives in the dark

| layer-0 band | n | ratio |
|---|---|---|
| 8–25 (dark) | 89426 | **1.297** |
| 25–50 | 38568 | 1.141 |
| 50–80 | 19112 | 1.023 |
| 80–120 | 3058 | 1.050 |
| 120–180 (bright) | 9139 | **0.971** |

`B = A + 4.20` fits better (residual 405) than `B = 1.035·A` (residual 420).

**Layer 1 carries a low-level additive term that layer 0 does not**, strongest
in shadow, gone in the highlights, marginally negative in the brightest band.
That is why it reads as "the shadows disagree": the dark regions are lifted in
one eye, so shadow interiors differ while lit surfaces and the background do
not. Patola's "the black albedo changes its area" is the same observation from
the other side — a lifted floor makes a dark region look smaller.

### Where this leaves it

Established by measurement, not inference:

* the shear is depth-correct (far background aligns at 0 px, take 57);
* near geometry parallaxes cleanly and keeps its appearance (strut, residual 17.6);
* `#57` is a geometry-only accumulation buffer, read and written by `rp #31/#32`;
* per-view sampling is load-bearing — removing it drives `#57` to 90× (take 58);
* the residual is an **additive lift in dark regions of layer 1**, ~4 units on
  an 8-bit tone-mapped view, uniform coverage.

**Next step is to read the shader for `rp #31/#32`**, not to propose a fifth
mechanism. Four have been offered for this symptom and four were wrong, every
one of them reasoned from something other than the code that does the work.
Take forty-eight was the same story and cost eleven runs; the fix arrived in one
message once the shader was read. The pass is now identified, which is the part
that was missing then.

### Offline: two structural suspicions, both excluded

`rp #31.0: 1 colour [97H] no-depth final=1 -> STEREO (world)` looked damning:
a self-referencing HDR post pass classified as world geometry. `classify_unsheared`
marks a subpass unsheared only when **all** its colour attachments are LDR, and
`#57` is RGBA16F, so these passes are not marked unsheared.

Two ways that could have produced the lift, both tested against the 409 dumps
and both **excluded**:

1. **A fullscreen triangle taking the eye shear.** If the post pass's vertex
   stage were sheared, its fullscreen quad would be displaced in view 1 and the
   in-place accumulation would smear — which is exactly the shape of a
   low-level additive lift. But the shear is gated per *module* by `classify()`,
   not by the pass, and **all 40 fullscreen-vertex modules classify UI**, so
   they receive identity. Ruled out.

2. **`classify()` scanning the whole module instead of the vertex stage.** Its
   documentation says a module is World only if its *vertex stage* reads member
   0 of the set-3 block, while the loop walks every instruction — and X4 ships
   combined vertex+fragment modules, so a fullscreen vertex stage paired with a
   fragment stage that reads set-3 would be misclassified. Measured: **0 of 409
   modules** are World by the whole-module scan but not by the vertex stage.
   The discrepancy is real in the code and inert in this game's shaders.

Worth writing down because both were checked rather than assumed, and because
the second is a latent trap: it costs nothing today and would misclassify the
first X4 build that pairs a fullscreen vertex stage with a set-3 fragment read.

### Where the next session should start

The pass is identified and its inputs are characterised. What is missing is the
**module bound to `rp #31/#32`** — the log records framebuffers and passes but
does not link a pipeline to the shader modules it was built from, so the shader
that produces the lift has not been read.

That link is the next thing to add: one line at `vkCreateGraphicsPipelines`
naming the render pass and the module serials. It is read-only, it is small,
and it converts "read the shader for `rp #31/#32`" from a search into a lookup.

Five mechanisms have now been proposed or tested for this symptom and five have
failed. The two in this section failed *cheaply*, offline, before costing a run
— which is the first time that has happened, and is the pattern the remaining
work should follow.

## The join was already being collected — only the present subset was printed

The previous section said the missing link was a pipeline→module line at
`vkCreateGraphicsPipelines`. That was half right. The hook already records
`g_rp_frag[rp_serial][module_serial]` for **every** pass carrying a render-pass
serial, gated on `X4VR_MV_INVENTORY` or `X4VR_DUMP_SHADERS`. What it never did
was *print* it for anything but the passes that draw into a swapchain image.

So the data existed in take 60 and died with the process. The change is one
loop over the map, not a new hook. Read alongside the existing `fb rp #N: …
imgs=[…]` lines — framebuffer line gives a pass its images, join line gives it
its shaders — an image serial becomes a module to disassemble by lookup.

### What the dumps already settle, offline

`#57` has `usage=0x97`, and bit 7 (`0x80`) is `INPUT_ATTACHMENT`. Together with
`fb rp #31: … attachments=2 imgs=[#57,#57]` and `rp #31.0: 1 colour [97H]
no-depth`, that fixes the pass's shape without a run: **`rp #31/#32` read `#57`
as a subpass input and write `#57`** — an in-place, self-referencing pass.

Measured across all 409 modules, not assumed:

- **26 modules declare a subpass input.** Every one declares exactly **one**,
  named `S_subpassInput_AUTOMS`, at set 0, binding 2, `InputAttachmentIndex 0`.
- There is no module anywhere in the dump that reads two or more.

This **corrects a comment** in `apply_input_attachment_fix`, which claimed X4's
deferred passes read the G-buffer through "the other four" subpass inputs and
named the passes by serial (`rp 30/31/32/64`). X4 reads one. And pass serials
are per-run, so they should never have been written into a comment at all.

### P67 — committed before the run that tests it

The fragment module bound to `rp #31/#32` will be one of the 26, and will
declare **exactly one colour output at Location 0** alongside its single subpass
input — i.e. one of the twelve minimal-I/O members of that set, not one of the
fourteen that carry 9–18 interpolated locations from a geometry vertex stage.

Stated structurally on purpose. Module serials are assigned in creation order
and are **per-run**, so naming a `mod-NNNN.spv` from the July-30 dump would be
a prediction about a file that a new run renumbers. The `/tmp/x4vr-shaders`
dump is from July 30; take 60's log is from July 31. Those two must not be
joined, and P67 is written so that it cannot be.

P67 is falsified if the bound module carries interpolated inputs, declares more
than one colour output, or declares no subpass input at all — the last of which
would mean the second attachment is preserve-only and the self-reference is not
a read at all.

**No mechanism is proposed here.** Five have been proposed for this symptom and
five have failed. The next step is to read the shader, not to guess what it does.

## Take 61 — the shader behind the `#57` lift, read at last

`X4VR_TAKE=61-P67 X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_BINDLESS_PATCH=1
X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
X4VR_PROJ_INVPROJ=1 X4VR_DUMP_SHADERS=/tmp/x4vr-shaders-take61
X4VR_LOG=/tmp/x4vr-take61.log ./launch/x4vr-launch.sh` — **PASS**, 397 modules
dumped, rendering knobs identical to take 60.

### X4 creates render passes in pairs, and pipelines bind to the twin

The new join printed 33 passes and **`rp #31/#32` were not among them**. They have
framebuffers and no pipeline. Mapping both families across all 58 passes shows
why: X4 creates render-pass objects in **pairs with identical signatures** —
one object receives the `vkCreateGraphicsPipelines` calls, its twin receives the
framebuffer and the `vkCmdBeginRenderPass`. Vulkan allows this; a pipeline only
needs render-pass *compatibility*, not identity.

    rp  signature                              fb?  pipelines
    30  1 colour [97H] no-depth final=1        --   4
    31  1 colour [97H] no-depth final=1        FB   0
    32  1 colour [97H] no-depth final=1        FB   0

The pairing is visible right across the run (14/15, 16/17, 18/19, 20/21, 26/27,
28/29, 34/35 … 42/43, 46/47, 48/49, 51/52, 54/55, 56/57). **`final=1` is unique
in the whole run to 30/31/32**, so the match is unambiguous.

This matters beyond this bug: any future "which shader draws into image X" join
keyed on the `VkRenderPass` handle will silently return nothing for exactly the
passes that have framebuffers. The join must be read through the twin.

### P67 — CONFIRMED

All four modules bound to `rp #30` declare exactly one subpass input and exactly
one colour output `OUT_RT0` at Location 0. Their vertex stages build a fullscreen
triangle from `gl_VertexIndex` with no vertex buffer. Serials **from take 61's own
dump**: `#182` (samples nothing), `#368`, `#370`, `#372` (all index-offset APPLIED).

### What the pass is: volumetric fog, composited additively

Decoded from `mod-0368.spv` (take 61 dump), fragment entry `main_0`:

    scene    = subpassLoad(S_subpassInput_AUTOMS[U_index])      // #57, this pixel
    viewPos  = M_invprojection · vec4(ndc.xy, depth, 1); /= w   // camera member 2
    depth      via bindless S_sampler2D_AUTOMS[I_maindepth]     // camera member 58
    phase    = dot(normalize(viewPos), normalize(V_light_direction_view))
    clip2    = M_projection · vec4(viewPos, 1)                  // camera member 1
    froxel.xy= clip2.xy/clip2.w · scale + offset
    froxel.z = sqrt((clip2.z/clip2.w − V_volume_off) / V_volume_scale)
    fog      = textureLod(S_sampler3D[39], froxel, 0)
    OUT_RT0.rgb = scene · fog.a + fog.rgb                       // T and in-scatter

**`scene · transmittance + in-scattering`.** The second term is literally an
addition. That is the measured signature of the residual — `B = A + 4.2` beating
`B = 1.035·A`, and 1.297 dark against 0.971 bright — arriving from the code
rather than from a curve fit. Where the scene is dark, `scene·T` is small and the
added in-scatter dominates the pixel; where it is bright, it barely registers.
This is also why the residual survived parallax alignment: aligning the geometry
does not align a term that was never a function of that geometry's position.

### The volume is built by compute, so it cannot be per-view

The fog volume is a **88×88×128** 3D image (`1408/16 = 88`: a 16-pixel froxel
grid, 128 depth slices) with `usage=0x9f` — bit 3 is `STORAGE`, i.e. written by
compute. Take 61 dispatched 6 compute shaders, one of them 55,616 times. The
layer's own log states the consequence:

    bindless mirror final: index-offset patch — 354 modules edited, 8 declared a
    mirrorable table and REFUSED (6 of those are compute: no gl_ViewIndex exists there)

This is the already-documented rule — *multiview does not cover compute* — landing
on a pass that matters. There is **one** volume, built once per frame for one
camera, and both views composite from it.

### Two corrections to this document

1. **The camera block has 72 members, not 14.** Members 0–13 were recorded
   correctly, but the block continues through `V_ambient1`, three directional
   lights, `V_light_direction_view` (21), the whole `V_volume_*` group (24–32),
   `F_exposure` (40), the CSM texture factors (46–50), `I_maindepth` (58),
   `I_global_envmap` (59), `I_tonemap_clut` (60), `B_shadow` (64) and more. Several
   are read by this shader. The earlier list was a prefix mistaken for the whole.
2. **`S_subpassInput_AUTOMS` is an array**, indexed by `U_index` (member 10 of
   `BLOCK_BUFFER_BINDING_SLOT_DYNAMIC`), not a single input attachment.

### P68 — and what the timeline already excludes

The shader reads **member 2 (`M_invprojection`) and member 1 (`M_projection`) in
one round trip**. We patch member 2 per eye in the fragment stage; we patch member
1 per eye only in the *vertex* stage. So half of the round trip is eye-corrected
and half is not — an asymmetry this project introduced.

**That asymmetry is not the original cause, and the git history proves it.** #22
was reopened at take 55 (`3033a7b`); `patch_fragment_invproj_eye` landed *after*
it (`a523194`). The lift was measured before the patch that could have caused it
existed. It may still be making things worse — that is a separate question.

**P68:** running with `X4VR_PROJ_INVPROJ=0` will leave the `#57` lift materially
unchanged, because the round-trip asymmetry post-dates the symptom. If the lift
*does* drop sharply, P68 is refuted and our own patch is contributing.

This is a bisect of the knob space that already exists — no new knob, no new code,
and it is the cheapest way to separate our contribution from X4's own behaviour.
Six mechanisms have now been excluded for this symptom. The seventh candidate —
a single compute-built fog volume shared by two eyes — is **stated here as a
reading of the code, not as a diagnosis**, and is not to be patched before P68
has separated it from the asymmetry we introduced.

## Take 62 — P68 CONFIRMED: our invprojection patch is not the cause

Take 61's command with `X4VR_PROJ_INVPROJ=0` and a fresh log. **PASS.**

    invproj final: per-eye M_invprojection — 0 modules corrected

    probe             take 61 (INVPROJ=1)      take 62 (INVPROJ=0)
    l1/l0             1.846, 1.846, 1.848      1.860, 1.861
    changed           420054                   420055
    missing / extra   120049 / 117437          120049 / 117437

The patch is fully off and the lift is unchanged — `missing` and `extra` are
*identical*, `changed` differs by one texel. The scene reproduced that closely
across two runs, which makes this a clean control rather than a lucky match.

**P68 confirmed. The round-trip asymmetry we introduced is not the cause**, as
the git timeline already implied. Excluded mechanism number seven, and the first
one that was ours.

It also raises a question about a knob we are carrying. `patch_fragment_invproj_eye`
corrects 236 modules per run and has **no measurable effect on `#57`**. It was
added at take 55/56 for "the deferred passes light the wrong frame". Either it
fixes something these probes do not watch, or it is dead weight. A known-good
state is code *and* knobs, so this should be resolved before it is tagged into one.

### The descriptor is not the asymmetry either

`S_sampler3D[39]` is reached through the bindless heap, so view 1 reads the
twin at `39 + 26653`. If that twin were unwritten, view 1 would sample undefined
data and the two eyes would diverge for a trivial reason. It is written:

    bindless mirror final: offset 26653, 380290 twin writes, 49253400 twin
    descriptors, 111306 of them layer-1, 0 skipped for no room

`0 skipped for no room`, and a 3D volume has no array layers for the mirror to
redirect, so descriptor 39 and its twin resolve to the *same* image. **Both eyes
sample the same volume data.** The asymmetry is in *where each eye looks up*, not
in what it finds there.

### What this leaves, and what it does not license

Established: both views run the same fog shader, sample the same single volume,
and composite `scene·T + inscatter`; the volume is compute-built and cannot be
per-view. The froxel coordinate is derived from each view's own depth buffer, so
the two eyes land on different froxels of a volume built for neither of them.

Not established: why the difference is *systematically* one-directional
(layer 1 brighter by 1.86×) rather than a symmetric error about the mono camera.
A symmetric ±d shear producing an asymmetric result is not yet explained, and
saying "the volume is mono" does not by itself predict a sign. **That gap is the
reason nothing is patched yet.**

## The fog passthrough — built offline, validated before it costs a run

Patola proposed elimination: disable the candidates one at a time and see whether
the eyes still disagree. The `patch_fragment_invproj_eye` arm was already done —
that is exactly what take 62 was, and it came back negative. The fog arm is new,
and it is a better instrument than the "probe `#57` before the fog pass" plan it
replaces: it needs no new probe plumbing, only the removal of one term.

`patch_fragment_disable_fog` forces the froxel-volume sample to `vec4(0,0,0,1)`,
which leaves `OUT = scene·1 + 0`. The pass still runs, still reads its subpass
input, still writes its attachment; the frame graph is untouched and only the
term under test disappears.

**Identified by structure, never by serial.** The transform refuses unless the
module declares a `SubpassData` image *and* samples a 3D one. Module serials are
per-run, so a hardcoded list would have rotted the first time X4 reordered its
shader creation.

**Chased down the sampler chain rather than matched by opcode.** `OpTypeImage(3D)`
→ `OpLoad` → `OpSampledImage` → `OpImageSample*`. `mod-0368` contains *two*
`OpImageSampleExplicitLod` and only one is the fog lookup; rewriting both would
have disabled something unrelated and made the measurement unreadable. Verified
on the patched output: `%1369` became the constant, and the other sample survived.

Offline result over all 397 modules of take 61's own dump:

    patched=8  refused=389  invalid=0

The 8 are exactly the fog family (`mod-0181/0182/0367/0368/0369/0370/0371/0372`),
which includes every module bound to the fog pass. Every patched module passes
`spirv-val --target-env vulkan1.2`, and the 389 refusals were each checked to
leave the module byte-identical — a property asserted 389 times against real
bytes, which is worth more than the synthetic test case this transform does not
yet have. **That test gap is real and recorded rather than skipped.**

### P69 — committed before the run

Take 63 = take 61's command plus `X4VR_DISABLE_FOG=1`. **P69: the `#57` lift
survives with the fog term gone** — `l1/l0` stays materially above 1.0 rather
than collapsing toward it — because take 62 showed the froxel coordinate is not
the differentiator, which points at the fog carrying a difference rather than
making one.

If P69 holds, the fog is exonerated as a *source* and the search moves upstream
to the other five passes that write `#57`. If P69 is refuted and the lift
collapses, the fog composite is the source and the excluded-mechanism count
finally stops growing.

Either outcome is informative, which is the property this run was designed for.
This is a **diagnostic build**: take 63 removes an effect the flatscreen game
has, so it is not a candidate for `docs/known-good-runs.md` whatever it scores.

## Take 63 — P69 CONFIRMED: the fog carries the difference, it does not make it

Take 61's command plus `X4VR_DISABLE_FOG=1`. All 8 modules were patched
(`fog final: … in 8 module(s)`), so the test genuinely ran.

    probe     take 61        take 62        take 63 (fog term removed)
    l1/l0     1.846          1.860          1.846
    changed   420054         420055         420057
    level     .004788/.00884 .004764/.008864 .004791/.008844

Three texels out of 420,000 separate take 63 from take 61. **`#57`'s layer
difference is bit-stable with the fog composite gone.** P69 confirmed: the fog
pass is a carrier. The source is upstream, among the other five passes that
write `#57`.

That is the eighth mechanism excluded, and the first excluded by removing a term
rather than by argument.

### The wrong turn in the same run: the signature was too coarse

The screen went black — every 3D scene, HUD only. Take 63's own pass→shader join
says why:

    rp #13, #16, #23, #30, #34, #36, #38, #40, #42  <-  frag module #182

`mod-0182` is bound to **nine** passes: the main geometry passes and all five
depth-only shadow passes. It is not a fog shader. It merely satisfies "declares
a SubpassData image and samples a 3D one", and zeroing that sample removed
something the whole scene depends on.

The error was reading "bound to the fog pass" as "bound only to the fog pass".
The join that revealed the fog pass in the first place contained the refutation
already — `mod-0182`'s nine bindings were in take 61's log, unread, because
the four modules on `rp #30` were checked and not the converse.

**The measurement survived the mistake, which is luck and should not be filed as
method.** `#57` came back bit-stable *even though* `mod-0182` was broken inside
`rp #23`, one of the passes that writes `#57` — so the contamination had no
effect on the quantity under test. Had it landed differently, take 63 would have
produced a confident number from a broken frame.

The transform is left in place, knob-gated and off by default, with the flaw
recorded in its own docstring and the tightening spelled out: require the 3D
sample to feed the composite's shape (component 3 into `OpVectorTimesScalar`,
components 0..2 into the consuming `OpFAdd`) rather than merely to exist.

### Where this leaves task #22

Excluded, in order: correct-stereo/shadows; deferred invprojection (artifact);
doubled shadow cascades; bindless per-view sampling; the fullscreen shear;
`classify()`'s scan width; our own `patch_fragment_invproj_eye` (take 62); and
now the fog composite itself (take 63).

`#57` is written by six passes. The fog is cleared. The next candidates are the
G-buffer passes that write it as attachment 1 — `rp #23/#24/#25` and `rp #57` in
take 60's numbering — and the honest next question is no longer "which mechanism"
but **what `#57` actually holds**, since a buffer that is simultaneously a
G-buffer attachment, a fog target, and the source of a 1541-region copy is not
yet identified. The unexplained sign of the difference is still unexplained.

## What `#57` actually is — and a correction to this document

Answered offline from take 63's log, no run needed. **`#57` is X4's main-view HDR
scene-colour buffer**: the target the scene is lit into, fogged in, and then
tonemapped from.

    rp #17  4 colour [97H,97H,83H,83H] + depth 126, imgs=[#55,#54,#59,#60,#61]
            -> the G-buffer FILL. #55 depth, #54/#59/#60/#61 the four targets.

    rp #23/#24/#25   imgs=[#55,#57,#59,#59,#60,#61]   colour target #57
    rp #13/#22       imgs=[#55,#54,#59,#59,#60,#61]   colour target #54
            -> both read the G-buffer as subpass inputs and write one HDR colour.

    rp #31/#32       imgs=[#57,#57]                   fog, in place on #57

Downstream, from the transfer graph:

    #57 -> #58   806 region(s), 806 widened     1408x1408 RGBA16F  (full res, both eyes)
    #54 -> #100   12 region(s),   0 widened      512x512 layers=6 mips=9  (CUBEMAP)
    #54 -> #486  211 region(s), 211 widened      768x512
    #65                                          1408x1408 fmt=50 B8G8R8A8_SRGB

`#54` feeds a six-layer mipped cubemap — the environment probe X4 samples through
`I_global_envmap` (camera member 59) — and a 768×512 view. `#57` is copied at
full render resolution, every frame, widened to both eyes, into a chain that ends
in an SRGB image. Full-res, per-frame, doubled: that is the main view.

### The correction

This document has repeatedly called `#57` "G-buffer attachment 1". **It is not
part of the G-buffer at all.** The G-buffer is `#54/#59/#60/#61`, written by the
fill pass `rp #17`. `#57` is the *output* of lighting, not an input to it. The
claim came from reading a six-attachment framebuffer list positionally and never
checking which index the subpass actually used as its colour attachment.

That mattered: "the difference is in a G-buffer channel" and "the difference is
in the lit scene colour" are different problems, and every mechanism proposed for
this symptom was reasoned against the wrong one.

### X4 renders the scene many times per frame

`rp #13` (→`#54`) and `rp #23` (→`#57`) share **128 of their 133/140 fragment
modules**. The same materials, the same geometry, two targets. The layer's own
frame log has been saying so all along:

    frame 33 NEW: main view slot buffer=0x3ada51e0 offset=1792 (block #1) draws=175 slots=4

`slots` reaches **54–61 per frame**. X4 maintains dozens of camera blocks and
renders from many of them: six cubemap faces for the environment probe, smaller
views, and the main view. This is the same fact that surfaced at take 53 as
"several cameras differ in one frame" and made `X4VR_PROJ_SX` unfittable by any
one constant — recorded then as a nuisance, now as structure.

**One consequence is worth flagging and not yet tested:** `#54 -> #100` copies
**0 widened**, i.e. layer 0 only. The environment cubemap both eyes sample for
image-based lighting is built from the left eye alone. That does not by itself
produce a difference *between* the eyes — a shared cubemap is shared — so it is
noted here as structure, **not proposed as mechanism nine**.

### Still open

The `#58 -> #65` step has not been traced module-by-module; the chain is asserted
from formats and sizes, which is strong but is not the same as reading the
shader. And the sign of the difference — a symmetric ±d shear yielding a
one-directional 1.86× — remains unexplained by everything established so far.

## The lighting passes, read with `#57` correctly identified

`rp #23/#24/#25` are **deferred light accumulation**. `mod-0207`'s own names say
so: `IO_center`, `IO_radius`, `IO_lightcolor`, `IO_Intensity`,
`IO_SpecularIntensity`, `SPECIAL_VERTEXLOCATION_INSTANCE0..5`, a single
`OUT_RT0`, and a subpass input. These are **instanced light volumes** — a sphere
drawn per light, reading the G-buffer through the subpass input and accumulating
its contribution into `#57`.

For contrast, `mod-0172` has `OUT_RT0..RT3`, the material and world blocks, and
tangent/binormal/UV sets: that is a G-buffer fill shader. The two are different
jobs, and until `#57` was correctly identified they were being read as one.

### The defect: the lights are not sheared, the geometry they light is

`classify()` calls a module World only if its **vertex stage reads member 0 of
the set-3 block** (`M_worldviewprojection`). `mod-0207` and `mod-0209` declare
**only sets 0 and 1 — no set 3 at all**. They classify UI and receive the
identity shear.

Measured over every module bound to `rp #23/#24/#25`:

    140 modules bound (2 absent from the dump)
      125  set-3 WORLD  -> sheared per eye
       13  no set-3     -> UI, UNSHEARED
           [18, 154, 156, 170, 176, 182, 207, 209, 223, 225, 227, 255, 267]

**So in view 1 the scene geometry is displaced by the eye shear and the light
volumes are not.** A light sphere positioned by instance data covers a set of
pixels; the G-buffer under those pixels has moved; the light is applied to the
wrong ones. Where a volume fails to cover a pixel it should have lit, that pixel
loses that light's contribution *entirely*.

This is the first candidate that is a genuine defect rather than a
reinterpretation of correct behaviour, and it predicts the measured shape without
being fitted to it: differences that are **additive**, **concentrated in dark
regions** (a missing light contribution is proportionally largest where little
other light arrives), and **survive parallax alignment** (aligning geometry
cannot align a term applied in the unsheared frame). Coverage gained on one side
and lost on the other is also the first thing that offers a route to the
**one-directional sign**, which nothing else has.

`classify()`'s rule is not wrong so much as too narrow, and its docstring explains
why it is narrow: shearing a light-space shadow pass would be actively incorrect,
because there clip z is not the constant near plane the derivation assumes.
Geometry positioned by *instance data* rather than by a set-3 matrix simply falls
through the gap. Whatever fix follows must widen the rule without catching the
shadow passes or the UI — the 13 modules above are not all light volumes, and
`mod-0182` is in the list, which take 63 showed is bound to nine passes including
all five shadow passes.

### P70 — committed before any code is written

Shearing the light volumes per eye will **materially reduce** the `#57` lift from
its stable `l1/l0 = 1.846`. If it does not move, light-volume coverage is not the
source either and the count of excluded mechanisms reaches nine.

P70 is **not yet tested**, and no fix is implemented here. The measurement above
establishes that the light volumes are unsheared; it does not establish that this
is what produces the difference. Eight mechanisms have died from that exact step
being skipped.

## Designing the widened predicate offline (`tools/predicate_design.py`)

Run against all 397 modules of take 61's dump, with **runtime ground truth** for
which modules X4 actually bound to which passes — not guesses about what a
shader "looks like". The analyser reads the **vertex entry point's function
only**, since X4 ships combined modules and a whole-module scan answers a
different question.

### The stated fear was wrong: widening cannot break shadows

    current rule: 320 World of 397
        shadow    48/56 World

**48 of 56 shadow-pass modules are already World under today's rule.** If module
classification were the only gate, shadows would already be broken — and they are
not. The protection is at the **pass** level:

    rp #34.0: 0 colour [] depth 124 final=-1 -> MONO (depth-only/shadow)
    bindless mirror final: unsheared twin swapped into 1042 pipeline stage(s)

A depth-only pass has zero colour attachments, so `classify_unsheared`'s "all
colour attachments are LDR" is vacuously true and the pass is marked MONO; the
layer then substitutes the unsheared twin into every pipeline built for it.

So the previous section's warning — *"a careless widening breaks shadows, which
is the exact failure that killed the earlier VR attempt"* — was **wrong about
this codebase**. The two gates are independent and the pass gate is the one that
protects shadows. Recorded because it was stated confidently and in the docs.

### The candidate

World if today's rule holds, **or** the module has no set-3 block at all and its
vertex stage reads a camera-block view/projection matrix — members 0 `M_view`,
1 `M_projection`, 7 `M_viewprojection`, 8 `M_viewinverse`. That is geometry
positioned by the camera rather than by a per-object matrix, which is exactly
what an instanced light volume is.

    widened rule: 338 World of 397   (+18)

    newly World: 202 203 206 207 208 209 216 217 222
                 223 224 225 226 227 254 255 365 366

    fullscreen / no vertex attributes (UI risk) :  0
    bound to present passes                     :  0
    bound to lighting passes                    :  6  (207 209 223 225 227 255)
    bound to shadow passes                      :  2  (203 225)

**Zero fullscreen modules and zero present-pass modules** are caught, so the
take-33 logo regression is not reachable this way. The six light-volume modules
are.

### The one dependency worth naming

Modules 203 and 225 are bound to shadow passes. They are safe **only because the
pass-level MONO gate neutralises them**, not because the predicate excludes them.
That is a real coupling: if `classify_unsheared` ever stops marking depth-only
passes MONO, these two begin shearing in a light-space pass where clip z is not
the constant near plane the derivation assumes. Written down rather than left as
a property someone rediscovers by breaking it.

P70 stands as recorded. Nothing is implemented yet; this section is the design
and its safety argument, done before any code.

## State entering take 64 — read this first after a context reset

### The run to make

    X4VR_TAKE=64-P70 X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_BINDLESS_PATCH=1
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
    X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_PROJ_INVPROJ=1 X4VR_SHEAR_LIGHTS=1 X4VR_LOG=/tmp/x4vr-take64.log
    ./launch/x4vr-launch.sh

Take 61's command plus `X4VR_SHEAR_LIGHTS=1`, nothing else. Cockpit scene.

### How to score it

`python3 tools/score_run.py /tmp/x4vr-take64.log`, then the probe line:

    grep "mv probe: img #57" /tmp/x4vr-take64.log

**The number that matters is `l1/l0`.** It has been `1.846` in take 61,
`1.860` in take 62 and `1.846` in take 63, with `changed` inside three texels of
420,054 every time. That stability is the instrument's strongest property: a
real move is unmistakable against it.

Confirm the knob actually fired before reading anything into the result — the
count of newly-sheared modules should be non-zero, and 6 of the 18 are in the
lighting passes.

### P70, as committed

Shearing the light volumes **materially reduces** the lift from 1.846. If
`l1/l0` does not move, light-volume coverage is excluded too — that would be the
ninth mechanism — and the next step is **not** a tenth guess.

Also worth having Patola watch the picture, not only the number: if the lighting
visibly shifts in either eye, that is information the probe cannot supply.

### What is established, so it is not re-derived

- **`#57` is X4's main-view HDR scene colour.** Not a G-buffer attachment; the
  G-buffer is `#54/#59/#60/#61` filled by `rp #17`. This document said otherwise
  for many sessions and every early mechanism was reasoned against the wrong
  kind of buffer.
- **`rp #23/#24/#25` are deferred light accumulation** — instanced light volumes
  reading the G-buffer through a subpass input. `rp #31/#32` is volumetric fog
  composited in place, and it **carries** the difference rather than making it.
- **X4 creates render passes in pairs.** One twin takes the pipelines, the other
  takes the framebuffer. A pass→shader join keyed on the `VkRenderPass` handle
  returns nothing for exactly the passes that draw.
- **Shadows are protected at the pass level, not the module level.** Depth-only
  passes classify `MONO` and get the unsheared twin. 48 of 56 shadow modules are
  already World under the narrow rule and shadows are fine.
- **X4 renders the scene from many cameras per frame** (`slots=54..61`): six
  cubemap faces into `#54`, smaller views, and the main view into `#57`. This is
  why no single `X4VR_PROJ_SX` fits.
- Nine things excluded for the `#57` lift, listed in task #22.
- **Unexplained throughout:** a symmetric ±d shear producing a one-directional
  1.86× difference. No candidate has predicted the sign, including this one.

### Two traps left armed

- `patch_fragment_disable_fog` is **too coarse** — its signature matches
  `mod-0182`, bound to nine passes including all five shadow passes, and zeroing
  its 3D sample turns every 3D scene black. Knob-gated and off by default.
- Modules 203 and 225 are newly World under the widened predicate **and** bound
  to shadow passes. They are safe only because the pass-level MONO gate
  substitutes the unsheared twin. If that gate ever changes, they shear in a
  light-space pass where clip z is not the constant near plane.

## Take 64 — P70 refuted, and a reading of the probe I had been skipping

    X4VR_TAKE=64-P70 X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_BINDLESS_PATCH=1
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
    X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_PROJ_INVPROJ=1 X4VR_SHEAR_LIGHTS=1 X4VR_LOG=/tmp/x4vr-take64.log
    ./launch/x4vr-launch.sh

The knob fired: `world=312 ui=38` against `world=296 ui=54` in takes 61 and 63,
so sixteen more modules were sheared, and the fog knob was off (`fog final: ...
0 module(s)`). The result:

    level 0.004787/0.008839 (l1/l0 1.847)
    level 0.004786/0.008838 (l1/l0 1.846)

Against 1.846 / 1.860 / 1.846. **P70 is refuted.** Shearing the instanced
deferred light volumes does not touch the lift, so light-volume coverage joins
the exclusion list. Patola's picture agreed: same darkness on the left frame.

### The reading I had been skipping

`l1/l0` is not a constant. Laid out with the disparity — `missing` and `extra`
count texels that are lit in one layer and empty in the other, which is what a
horizontal shift produces:

| take | non-empty (l0/l1) | missing | extra | `l1/l0` |
|------|-------------------|---------|-------|---------|
| 64   | 433028 / 433212   | 2215    | 2399  | **0.997** |
| 62   | 1054548 / 1068086 | 43689   | 57227 | **1.288** |
| 64   | 541729 / 539117   | 120049  | 117437| **1.846** |

**The brightness gap tracks the disparity.** Near-zero shift reads 1.0; a
partial shift reads 1.29; the full shift reads 1.85. I had been quoting only the
third row for four takes because it is the one that repeats, and reporting its
stability as the instrument's strength — when the other rows were in the same
logs saying the number is scene-dependent.

Why the third row repeats exactly (`541729/539117`, `missing=120049`,
`extra=117437` in takes 61, 62 and 64 alike): it is the frame just after the
save loads, before the camera moves. Deterministic, which makes it a good
fixture — but it is one scene, not the measurement.

Two points from two different scenes do not establish proportionality, and this
is not claimed. What they do is split the hypothesis space, which is the next
run's job.

### P71 — committed before the run

At `X4VR_IPD=0` the shear is the identity: `d = 0`, so `K` is the identity
matrix and **every pass writes identical content to both layers**. Nothing else
changes — the bindless mirror still redirects view 1's descriptors, the invproj
correction still runs (with a zero offset), the passes still classify the same
way.

So `#57` layer 0 and layer 1 should come back `IDENTICAL`, `l1/l0 = 1.000`, and
Patola should see no left/right difference at all.

**If it does not** — if `l1/l0` stays anywhere near 1.85 with zero disparity —
then the difference was never caused by the shear, and every mechanism tried so
far has been looking at the wrong half of the layer. What is left is per-view
*plumbing*: a resource written for one view and applied to both, a mirrored
descriptor pointing somewhere wrong, or a pass writing a single layer. The six
compute modules that declare a mirrorable table and are refused (`no
gl_ViewIndex exists there`, 20960 dispatches for module #362 alone) are the
first place to look, and the 3D volumes at `88x88x128` they write are the first
suspects.

Either answer is worth the run, which is why this one is `IPD=0` and not a
tenth guess at a mechanism.

### The run

    X4VR_TAKE=65-IPD0 X4VR_STEREO=1 X4VR_IPD=0 X4VR_BINDLESS_PATCH=1
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
    X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_PROJ_INVPROJ=1 X4VR_MV_DUMP=/tmp/x4vr-t65 X4VR_MV_DUMP_IMG=57
    X4VR_LOG=/tmp/x4vr-take65.log ./launch/x4vr-launch.sh

`X4VR_SHEAR_LIGHTS` is dropped — refuted, so the known-good default stands.
`X4VR_MV_DUMP_IMG=57` writes up to six pairs of PPMs of the HDR scene colour,
which is the instrument that answers "what is dark" rather than "how much".
It costs nothing extra to arm and it already exists.

## Take 65 — P71 confirmed: the shear causes it

    X4VR_TAKE=65-IPD0 ... X4VR_IPD=0 ... X4VR_MV_DUMP=/tmp/x4vr-t65
    X4VR_MV_DUMP_IMG=57 X4VR_LOG=/tmp/x4vr-take65.log ./launch/x4vr-launch.sh

Every `#57` sample came back bit-identical:

    layer0=35ae9ccff2a0f481  layer1=35ae9ccff2a0f481  IDENTICAL  level 0.02306/0.02306 (l1/l0 1.000)
    layer0=fe32af4bafd88e24  layer1=fe32af4bafd88e24  IDENTICAL  level 0.009923/0.009923 (l1/l0 1.000)
    layer0=ff23eba0a973b8d8  layer1=ff23eba0a973b8d8  IDENTICAL  level 0.01029/0.01029 (l1/l0 1.000)

`score_run.py` graded the swapchain `DUPLICATE — layer 1 is a bit-exact copy of
layer 0`, and Patola saw the two frames lit the same with no shift. **P71 holds.**

Three things follow, and the third is the one that matters:

- **The lift is caused by the shear.** Set `d = 0` and it is gone, to the bit.
- **Per-view plumbing is excluded.** The six refused compute modules, the
  `88x88x128` volumes, the mirrored descriptors — none of them can produce a
  difference that vanishes when only the geometry changes. That whole branch,
  which P71 named as the alternative, is dead without a run of its own.
- **The probe is validated.** It reads both layers correctly and symmetrically;
  a 1.000 on content this varied is not something a broken readback produces.
  Four takes of scalar output rested on that assumption and it had never been
  tested.

The PPM dump also works — `fmt 97, after rp #24`, four pairs written, up to
845152 non-zero texels. Worth noting **where** it samples: after `rp #24`, which
is the middle of the three deferred light-accumulation passes. So the probe has
never seen `#57` finished; it sees it between light passes, before fog.

Arming the dump on the elimination run was a small planning error — the run that
proves the layers identical is the run whose pictures show nothing. It cost
nothing here because the run was needed anyway, but the instrument and the
elimination wanted different takes.

### What the shear actually is, worked through

The layer logs `stereo: ipd=0.0640 sx=1.3333 near=0.100 -> shear m8 L=0.42666`,
and `1.3333 * 0.032 / 0.1 = 0.42666`, so `m8 = sx*d/near`. Column-major index 8
is row 0, column 2 — the term that feeds clip z into clip x.

X4's projection is reverse-Z infinite-far: `m[10] = 0`, so `z_c = near_real*w_v`
and `w_c = -z_v`. Then

    x_c' = x_c + m8*z_c = sx*x_v - (sx*d/near_assumed)*near_real

against the correct `sx*(x_v - d)`. **`near` does not cancel.** The applied
shear is the intended one scaled by `near_real / near_assumed`, so a wrong
`near` is a direct multiplier on the effective IPD. `X4VR_PROJ_LIVE` reads `sx`
from X4's camera block at runtime; nothing reads `near`.

If the form is right, screen disparity is `704 * sx*d / z_v` pixels — about
30/z_v, so a metre away is 30 px and distant stars are 0. Measured disparity was
~84 px on the loaded-save frame, which is the right order for a cockpit.

### P72 and P73 — committed before the run

**P72.** In the dumps at normal IPD, the difference between the layers is a
*displacement*, not an attenuation: layer 0 will match layer 1 shifted
horizontally, and the residual after a best-fit per-region horizontal alignment
will be far smaller than the raw 33% `DIFFER`. P71 showed the difference is
purely geometric, and geometry cannot remove light from a surface — only move
where it lands. **If the residual stays large after alignment**, the eyes differ
in *shading* rather than position, and the mechanism is inside the lighting.

**P73.** `X4VR_DUMP_MATRICES=1` arms `read_proj_terms()`, which reports
`proj MEASURED` / `proj ASSUMED` / `proj SHEAR : baked is N.NNNx the correct
magnitude` from X4's own un-jittered `M_projectionUJ`. This has never been run
alongside the probe. Predict it reports a ratio materially different from
1.000 — the `near = 0.1` default is a Phase 4a assumption and enters as a
divisor. If it reports 1.000, the shear magnitude is right and task #23's worry
about `near` is closed.

### The run

    X4VR_TAKE=66-PIC X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_BINDLESS_PATCH=1
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
    X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_PROJ_INVPROJ=1 X4VR_DUMP_MATRICES=1 X4VR_MV_DUMP=/tmp/x4vr-t66
    X4VR_MV_DUMP_IMG=57 X4VR_LOG=/tmp/x4vr-take66.log ./launch/x4vr-launch.sh

Take 64's command with `X4VR_SHEAR_LIGHTS` dropped (refuted), `X4VR_IPD` back to
0.064, and the two instruments added. No code changes; both are read-only.

## Take 66 — P72 confirmed, P73 refuted, and `l1/l0` was never a defect

    X4VR_TAKE=66-PIC ... X4VR_IPD=0.064 ... X4VR_DUMP_MATRICES=1
    X4VR_MV_DUMP=/tmp/x4vr-t66 X4VR_MV_DUMP_IMG=57
    X4VR_LOG=/tmp/x4vr-take66.log ./launch/x4vr-launch.sh

### P73 refuted — the shear magnitude is right

    proj MEASURED: sx=1.33333 sy=-1.33333 near=0.10000 (jittered sx=1.33333 near=0.10000)
    proj ASSUMED : sx=1.33330 near=0.10000
    proj SHEAR   : measured |m8|=0.42667 vs baked |m8|=0.42666 -> baked is 1.000x the correct magnitude

`near` is exactly 0.1. The Phase 4a assumption was right and the algebra above,
which made `near` a multiplier on the effective IPD, is correct but harmless.
**Task #23's `near` worry is closed.**

The same instrument answered the *other* half of task #23, which was the real
one: `sx` is **not** constant. Over one session it moved

    #3: sx 1.33333 -> 3.78085   (correct |m8| 1.20987, baked 0.42666)
    #4: sx 3.78085 -> 1.15174   (correct |m8| 0.36856, baked 0.42666)

a 3.3x range — X4's zoom, sustained for 75 seconds at 3.78. `X4VR_PROJ_LIVE`
handles this for 284 of 296 world modules; the remaining **12 baked-sx modules
carry a shear up to 2.8x wrong whenever the player zooms**. Small, real, and now
the only live part of task #23.

### P72 confirmed — and it dismantles the metric

Per-tile horizontal disparity search on the dumped layer pairs, then brightness
compared *after* compensating for the disparity:

| frame | whole-frame `l1/l0` | disparity p5/p50/p95 | residual explained | **aligned `l1/l0`** |
|-------|--------------------|----------------------|--------------------|--------------------|
| n0    | 1.001              | 0 / 0 / 0 px         | —                  | **1.0002** |
| n2    | 1.219              | −247 / −28 / +19 px  | 75.8%              | **1.0101** |
| n3    | 1.215              | −379 / −28 / 0 px    | 74.6%              | **1.0064** |

(Whole-frame ratios are on the tonemapped 8-bit dumps; the probe's HDR figure
for n2 is 1.846. Same frames, same conclusion.)

**Where both eyes see the same surface, they light it identically — to within
0.1%.** The leftover residual traces occlusion edges and nothing else: run
`tools/stereo_residual.py --png out` and the map is thin outlines along the
strut and wing silhouettes, with black interiors. That is parallax. One eye sees
behind a silhouette the other does not, and no shift can align that.

So `#57`'s lift is **displacement, not attenuation**, and the sign that "no
candidate has predicted" for six takes has a dull explanation: this cockpit is
bright on the right and dark on the left, the shift moves bright structure in at
one edge and dark space out at the other, and the whole-frame mean follows. Take
62's 1.288 and n0's 1.000 were the same statistic on scenes with less to move.

### The correction

**`l1/l0` cannot tell correct stereo from broken stereo, and I scored takes 56
through 66 on it.** Two correctly offset views of an asymmetric scene have
different whole-frame means; that is what the ratio measures. Every "mechanism"
excluded for the lift — the invprojection correction, the fog composite, the
light volumes, the descriptor path, and the rest — was excluded for a symptom
that was never a defect. Those exclusions are still *true*; they were just
answers to the wrong question.

The tell was there from take 61: `missing=120049 extra=117437`, a near-symmetric
pair of edge bands, is the signature of a shift. I read those fields for four
takes as scene bookkeeping next to the number I cared about.

`tools/stereo_residual.py` is the instrument that does discriminate. It takes a
probe dump pair and reports aligned brightness with a verdict line, so this
question is one command from now on instead of a session of reasoning.

### What is actually left on `#57`

Nothing, as a rendering defect. What remains is **comfort**: the p5 disparity is
−247 px, which at 1408 px across a 73.7° frame is about 13° — geometry roughly
12 cm from the eye. That is the correct disparity for a 64 mm IPD at that
distance, and it is also far beyond what eyes fuse comfortably. Whether X4's
cockpit belongs that close to the viewer is a tuning question (IPD scale, or
pushing the cockpit back), not a bug, and it should be judged **in stereo** —
cross-eyed or in the headset. Every judgement so far, mine and Patola's, has
been made on two flat frames or on a scalar, and neither can answer it.

## Take 67 — the same answer at the surface Patola actually sees

    X4VR_TAKE=67-EYE ... X4VR_MV_DUMP=/tmp/x4vr-t67 X4VR_MV_DUMP_IMG=52
    X4VR_LOG=/tmp/x4vr-take67.log ./launch/x4vr-launch.sh

Take 66 measured `#57`, which the probe samples **after `rp #24`** — the middle
of light accumulation, before fog, before the copies, before tonemapping. So
"the eyes light the same surface identically" was established for an
intermediate buffer, not for the image on screen. Patola asked to isolate the
dark patches from the depth question and fix them first, which is the right
instinct and exposed that gap. This take dumps `#52`, one of the four presented
eye images (`fmt 44`, `after rp #0`).

    tiles          195, search +-400px
    whole-frame    l1/l0 = 1.0762
    disparity      p5/p50/p95 = -133/-26/164 px, 0 at the search bound
    residual       24.15 unaligned -> 8.98 aligned (62.8% explained)
    gain test      per-tile gain p10/p50/p90 = 0.734/0.990/1.124
                   fitting a gain changes the residual by -2.1%

Three independent readings, and the third is the one that carries the verdict:

1. **Aligned brightness** — 0.9968 / 0.9988 / 1.0031 over the well-aligned
   tiles. But see the correction below: on its own this is nearly circular.
2. **The signed difference image** is textbook parallax. Every feature appears
   as an *adjacent red/green pair* — green where it arrived in the right eye,
   red where it left in the left — struts, console edges, holo display alike.
   The distant starfield shows almost nothing, which is what zero disparity at
   infinity looks like. There is no large single-colour region anywhere, which
   is what a one-eye-only overlay would have produced.
3. **The gain test.** Fit a per-tile gain on top of the alignment and ask what
   it buys. If one eye were genuinely darker, scaling would collapse the
   residual. It changes it by **−2.1%** — it makes the fit *worse* — and the
   median gain is 0.990. The residual is structural, not photometric.

**So the dark patches are parallax on near cockpit geometry, and there is
nothing to fix before the headset.** The left eye shows dark background where
the right shows lit cockpit, because the cockpit is close enough to occlude
differently in each eye. That is also why they "shift in size with the angle of
vision": different near geometry enters the frame as the camera turns.

### A correction to the tool, one commit old

`stereo_residual.py` shipped with a verdict that averaged brightness over tiles
*selected for low residual*. That is close to a tautology — a tile that differs
photometrically keeps a high residual and is dropped from the very average meant
to detect it, so the tool would have reported agreement almost regardless. The
gain test replaces it as the basis for the verdict, and the aligned-brightness
rows are kept as description rather than evidence.

Worth noting the shape of the mistake: it is the same one that produced the
`l1/l0` error a day earlier. Build a number, do not ask what it reads when the
answer is "healthy", and it will agree with you.

### `rp #7` — the candidate that was not it

The score output has warned in every run that `rp #7` writes a presented eye
image unmasked while `rp #0` writes it masked, so "layer 1 misses whatever
`rp #7` draws". It does draw: `rp #7 <- frag modules #1..#4`, sampling
`set 0 binding 0`, a plain texture rather than the bindless heap. That is a real
per-view asymmetry on the left eye, unexplained, and it was the first suspect
here. The signed-difference image rules it out for *this* symptom — no
unpaired region exists — but the warning is still true and still unexplained,
and it is now the only known per-view asymmetry in the presented image. Task #26.

## Take 67, second reading — there IS a shading defect, and both metrics hid it

Patola rejected the "this is correct stereo" conclusion: *"in real life, if you
close one eye at a time, you don't see a completely lit surface vs an almost
completely dark surface, it's absurd."* He was right, and the record needs to
say so plainly.

### The measurement that finds it

Lock onto a distinctive high-contrast feature to get the wing's *true*
disparity — the vent on its top, matching at **−43 px, NCC 0.756** — then
compare surfaces at that disparity:

| surface | L0 | L1 | ratio |
|---------|-----|-----|-------|
| window slots (side-facing, diffuse) | 46.5 | 47.3 | **1.016** |
| wing top (upward-facing), y=520 | 45.2 | 109.1 | **2.41** |
| wing top (upward-facing), y=545 | 45.4 | 113.0 | **2.49** |

Same object, correctly registered, one surface 2.4x brighter in the right eye
and the surface next to it matching to 1.6%. The band crop shows it directly:
the wing sits in the same place in both eyes and its top face is dark blue-grey
in the left, near-white in the right.

Across the frame, **9.9% of judged tiles differ by more than 1.25x while
matching confidently in structure** — and occlusion cannot correlate at 0.7
with something that is not there, so that set is same-surface by construction.
The take 65 `IPD=0` dumps read **0.0%** on the same test, which is the negative
control this needed all along.

### Why two metrics in a row missed it

- `l1/l0` is a whole-frame mean, already retired.
- The first version of `stereo_residual.py` aligned tiles and reported a
  **median**. The effect covers a few percent of the frame; a median is exactly
  the statistic that discards it. It also selected tiles by *absolute residual*
  — dropping a differently-lit tile from the average meant to detect it — and
  it excluded everything within `maxshift` of either edge, leaving only the
  middle 39% of the width examined.

Both failures share a shape: **an aggregate chosen without asking what it does
to a localized effect.** That now has its own entry in the recurring-error list.
The human eye found this in one look at a side-by-side crop, twice, after two
tools said there was nothing there.

### P74 — committed before the run

The pattern is specific: **diffuse, side-facing surfaces match; upward-facing,
reflective ones do not.** That is the signature of a wrong *view-dependent*
shading term — specular and environment reflection — while albedo and diffuse
stay correct. Those terms are computed from a position reconstructed out of
depth, and the layer patches that reconstruction per eye:
`patch_fragment_invproj_eye`, applied to **230–238 modules**.

That patch was built to fix the `#57` lift. The lift was not a defect. So the
first hypothesis is that a correction written for a phantom problem is the
source of a real one.

Predict: with `X4VR_PROJ_INVPROJ` dropped, the flagged fraction falls
materially from 9.9% and the wing-top ratio moves off 2.4x. If it does not, the
patch is exonerated and the next knobs are `X4VR_BINDLESS_PATCH` /
`X4VR_BINDLESS_MIRROR` (view 1 samples slot `index + 26653`; the mirror is
sound for undoubled images, `l1 == VK_NULL_HANDLE -> verbatim copy`, but
doubled intermediates are another matter) and then `X4VR_PROJ_LIVE`.

### The run

    X4VR_TAKE=68-NOINVPROJ X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_BINDLESS_PATCH=1
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
    X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_MV_DUMP=/tmp/x4vr-t68 X4VR_MV_DUMP_IMG=52
    X4VR_LOG=/tmp/x4vr-take68.log ./launch/x4vr-launch.sh

Take 67 with `X4VR_PROJ_INVPROJ=1` dropped, nothing else changed. Scored with
`tools/stereo_residual.py` on the `#52` dumps, against 9.9% and 2.4x.

## State entering take 68 — read this first after a context reset

### The one thing not to get wrong again

There are **two separate questions** about the eye images, and this document
answered the first one correctly and then wrongly generalised it:

1. **Is the whole-frame brightness ratio `l1/l0` a defect?** No. Retired. Two
   correctly offset views of an asymmetric scene have different whole-frame
   means. Takes 56–66 were scored on it for nothing.
2. **Do the two eyes light the same surface the same way?** **No — and this is
   a real defect.** Confirmed on take 67's presented eye image.

Do not let (1) talk you out of (2). It already happened once: I concluded "this
is correct stereo, nothing to fix", and Patola rejected it — *"in real life, if
you close one eye at a time, you don't see a completely lit surface vs an almost
completely dark surface, it's absurd"* — and he was right. He has also seen this
artifact in earlier VR attempts and reports it is uncanny and destroys the depth
sense, which is why the headset trial is blocked behind fixing it.

### The defect, measured

Lock the wing's true disparity on a distinctive feature (the vent on its top:
**−43 px, NCC 0.756**), then compare surfaces at that disparity:

| surface | L0 | L1 | ratio |
|---------|-----|-----|-------|
| window slots (side-facing, diffuse) | 46.5 | 47.3 | **1.016** |
| wing top (upward-facing), y=520 | 45.2 | 109.1 | **2.41** |
| wing top (upward-facing), y=545 | 45.4 | 113.0 | **2.49** |

Whole frame: **9.9% of judged tiles differ by more than 1.25x while matching
confidently in structure** (NCC ≥ 0.7). Occlusion cannot correlate at 0.7 with
something that is not there, so that set is same-surface by construction.
Overall flagged including possible occlusion: 24.6%. Whole-frame `l1/l0` for the
same frame: 1.0762 — which is why the old metric saw nothing.

**Negative control: the take 65 `IPD=0` dumps read 0.0% on the identical test.**
Any future instrument that cannot reproduce 9.9% / 0.0% on these two pairs is
not measuring the defect.

### How to score a run

    python3 tools/stereo_residual.py <layer0.ppm> <layer1.ppm> [--png out]

Dumps come from `X4VR_MV_DUMP=<prefix> X4VR_MV_DUMP_IMG=52` — `#52` is one of
the four **presented** eye images (`fmt 44`, dumped `after rp #0`). Dump `#57`
only when the mid-pipeline HDR buffer is specifically wanted: the probe samples
it after `rp #24`, in the middle of light accumulation, so it cannot speak for
anything fog or tonemapping does.

Baseline pairs are preserved outside the repo at **`~/x4vr-baselines/`** —
`x4vr-t67-img52-n2-layer{0,1}.ppm` (the defect, 9.9%) and
`x4vr-t65-img57-n2-layer{0,1}.ppm` (the control, 0.0%). They are X4's rendered
output and must never be committed.

Read the numbers, and also **crop the two eyes side by side at the same
coordinates and look.** That found this in one glance, twice, after two tools
said there was nothing there.

### P74 and the run it is waiting on

Diffuse side-facing surfaces match; upward-facing reflective ones do not. That
is a wrong **view-dependent** term — specular and environment reflection — with
albedo and diffuse intact. Those terms use a position reconstructed from depth,
and `patch_fragment_invproj_eye` rewrites that reconstruction per eye in
**230–238 modules**. It was built to fix the `#57` lift, and the lift was never
a defect, so the first suspect is a correction written for a phantom problem.

    X4VR_TAKE=68-NOINVPROJ X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_BINDLESS_PATCH=1
    X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1
    X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_MASK_PRESENT=1
    X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_MV_DUMP=/tmp/x4vr-t68 X4VR_MV_DUMP_IMG=52
    X4VR_LOG=/tmp/x4vr-take68.log ./launch/x4vr-launch.sh

Take 67's command with `X4VR_PROJ_INVPROJ=1` **dropped**, nothing else changed.
Confirm from the log that it did not fire — `invproj final: ... 0 modules
corrected` — before reading anything into the result.

P74 predicts the flagged fraction falls materially from 9.9% and the wing-top
ratio moves off 2.4x. **If it does not, the patch is exonerated**; the next
knobs are `X4VR_BINDLESS_PATCH` / `X4VR_BINDLESS_MIRROR`, then `X4VR_PROJ_LIVE`.
Do not add a knob to fix a failed run — bisect the existing space.

### Established, so it is not re-derived

- `#57` is X4's main-view HDR scene colour, sampled by the probe **after
  `rp #24`**. `#50–#53` are the presented eye images. `#65` reads IDENTICAL
  between layers on every sample.
- `near` is exactly 0.100 (measured, take 66) — the Phase 4a assumption was
  right. But `sx` moves over a **3.3x range** with zoom (1.15 / 1.33 / 3.78),
  and 12 baked-sx modules are up to 2.8x wrong when it does. That is task #23.
- The bindless mirror handles undoubled images correctly:
  `l1 == VK_NULL_HANDLE -> continue`, the verbatim copy. Only 83,396 of
  38.7M twin descriptors are layer-1 substitutions.
- `rp #7` writes a presented eye image **unmasked** (`frag modules #1..#4`,
  `set 0 binding 0`), so layer 1 misses it. Real, unexplained, ruled out for
  *this* symptom by the signed-difference image. Task #26.
- Refuted and off: `X4VR_SHEAR_LIGHTS` (take 64). Too coarse and knob-gated off:
  `patch_fragment_disable_fog` (matches `mod-0182`, bound to nine passes
  including all five shadow passes; blacks out every 3D scene).

### The error class to check before trusting new analysis

Three metrics on this project have now failed by **aggregating a localized
effect**: `l1/l0` (whole-frame mean), and two versions of `stereo_residual.py`
(a median, plus a tile selection that dropped differently-lit tiles from the
average meant to find them, plus an edge exclusion that examined only the middle
39% of the width). Report **tail and affected area**, never a bare median, and
keep a negative control.

## Take 68 — P74 refuted, and the defect was never in the buffer I was measuring

The run: take 67's command with `X4VR_PROJ_INVPROJ` dropped. The knob is
confirmed off from the log — `invproj final: ... 0 modules corrected`, against
take 67's `236 modules corrected`.

| | take 67 (patch on) | take 68 (patch off) |
|---|---|---|
| lit differently **and** confidently matched | 9.9% | 9.4% |
| flagged >1.25x | 24.6% | 24.5% |
| whole-frame `l1/l0` | 1.0762 | 1.0925 |

Removing `patch_fragment_invproj_eye` did nothing. **P74 is refuted and the
patch is exonerated**, per the falsification condition committed before the run.

### The detector was matching surfaces that cannot correspond

The offender lists carried shifts of `+188px`, `+250px`, `+294px`. Those are
geometrically impossible. X4's projection is reverse-Z infinite-far, so screen
disparity is `W/2 * sx * d / z_v` — monotonic in depth, bounded by the near
plane, and **the same sign for every tile in the frame**. At the logged
`ipd=0.064 sx=1.3333 near=0.1` and 1408px that is `30.04/z_v` px: zero at
infinity, 300px at the near plane, never both signs. The empirical median is
−44px, so every real match lies in `[−300, 0]`.

A starship hull is a repeating texture and will correlate at 0.8 against a
different panel 200px away. `stereo_residual.py` now derives the search window
from the geometry (`plausible_window`) instead of searching ±400px, and prints
what the unconstrained search would have said. The negative control still reads
**0.0%**, which is the check that this is a geometric constraint and not a thumb
on the scale. Constrained: take 67 **7.8%**, take 68 **8.5%** — P74 refuted
either way.

This is the fourth time a number here came from ranking a list and reading the
top of it without checking what the entries meant.

### P75 — a per-eye exposure difference — refuted offline, no run spent

`l1/l0 = 1.0925` says the left eye is 9% darker, and Patola's report was
"darker on the left frame". If one eye were tonemapped at a different exposure,
then `right = f(left)` for a single curve across the whole frame. Measured over
1009 confidently-matched tiles, binned by level:

    left level    n     ratio          left level     n     ratio
       0-  2    112     1.017            32- 64     216     1.037
       2-  4     80     1.001            64- 96     119     1.011
       4-  8    103     0.999            96-128      58     0.969
       8- 16    137     0.990           128-160      45     1.000
      16- 32    123     1.065           160-200      16     1.016

Flat. Best single global gain **1.0025**. There is no exposure difference; the
9% whole-frame lift is content moving in at the frame edge — the retired `l1/l0`
story, confirmed quantitatively on the presented image this time.

### On image #52 the stereo is clean, and that is the clue

Residual after alignment, against how fast disparity varies inside the tile:

    disparity spread     n     median eye disagreement
        0- 4 px        220              0.3%
        4- 8 px        147              1.0%
        8-16 px         68              1.5%
       16-32 px         77              1.5%
       32+  px        497              4.8%

The disagreement is a pure function of the disparity, not of the surface. Where
the two eyes see the same surface at the same depth they agree to **0.3–0.4%**.
That is correct stereo plus tile misalignment, and it is the same "no defect"
answer two tools gave before — but this time it is a clue, because the screen
disagrees with it. Something happens to this image after the probe reads it.

### The log had been saying it since take 43

    mv final: img #52 writers — masked rp [0] unmasked rp [7]
    mv final: img #52 MIXED WRITERS — layer 1 misses the unmasked ones

All eight presented eye images (`#1–#4`, `#50–#53`) have mixed writers, and they
are the only images that do. `rp #0` is masked and writes both layers — that is
the clean stereo measured above. `rp #7` is unmasked with `layers=1`, so it
writes layer 0 and nothing else. **Everything it draws lands in the left eye and
is absent from the right.** Left eye, extra content, view-dependent, patchy:
Patola's description, exactly.

And the probe line says where it was read: `wrote ...-layer{0,1}.ppm (fmt 44,
**after rp #0**, DIFFER)`. The dumps have never contained rp #7's output.

**The wrong turn:** this file already said `rp #7` was "ruled out for *this*
symptom by the signed-difference image". That ruling-out was invalid — it looked
for rp #7's footprint in a buffer sampled before rp #7 runs. Recorded rather
than edited away.

### Root cause: one field, two readings

Sorting the inventory for the passes that write these images gives five
byte-for-byte identical lines, one masked and four not:

    rp #0.0:  1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI) +MASKED(present) +PRESENT-CAND
    rp #1.0:  1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI)
    rp #4.0:  1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI)
    rp #7.0:  1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI)
    rp #10.0: 1 colour [44L] no-depth final=2 -> MONO (all-LDR/UI)

Identical inputs cannot produce different verdicts, so the predicate reads a
field the inventory does not print. It does:

* the inventory decides "no-depth" from `pDepthStencilAttachment->attachment`;
* `subpass_is_present()` decided it from `pDepthStencilAttachment != nullptr`.

A subpass may carry a valid depth pointer whose attachment is
`VK_ATTACHMENT_UNUSED`. X4's `rp #1/#4/#7/#10` do; `rp #0` does not. Given
`colorAttachmentCount == 1` and format 44 LDR, that is the only remaining way
for the log to say "no-depth" while the predicate returns false — proven from
the log with no run.

This also explains the instability recorded at take 43: "the same command
produced 3 candidates in take 33 and 6 in take 43". Candidacy depended on
whether X4 happened to hand a null or an UNUSED depth pointer.

The fix tests the index, like the inventory does.

### P76 — the exclusive dark patches are rp #7, and masking it removes them

Falsifiable, and scoreable from the log before Patola judges anything:

1. `rp #1.0/#4.0/#7.0/#10.0` gain `+MASKED(present) +PRESENT-CAND`;
2. `mv final: img #52 writers — masked rp [0,7] unmasked rp []`;
3. **no `MIXED WRITERS` line anywhere in the log**;
4. then, and only then, Patola's eye: the dark areas that appear in one frame
   only are gone.

    X4VR_TAKE=69-MASKPRESENT-FIX X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1 X4VR_MV_DUMP=/tmp/x4vr-t69
    X4VR_MV_DUMP_IMG=52 X4VR_LOG=/tmp/x4vr-take69.log ./launch/x4vr-launch.sh

Take 68's command unchanged — the only difference is the rebuilt layer. No new
knob; `X4VR_MASK_PRESENT=1` was already on and was already meant to do this.

**If checks 1–3 pass and Patola still sees the patches**, rp #7 is genuinely
exonerated this time and the next suspect is the SbsCompositor's own copy path,
not another knob. **If 1–3 fail**, the depth pointer was not the discriminator
and the predicate needs the actual differing field printed before anything else
is tried.

Watch for two side effects: four extra masked fullscreen passes cost frame time
(performance is king), and these are the UI passes, so the clipped logo of
task #21 may move.

### What the probe still cannot see

The dump point is "after rp #0", which is not the end of the frame. Any pass
writing a presented eye image after that is invisible to every number in this
file. That is a property of the instrument, not of the frame, and it is why the
verdict on take 69 leans on the log's writer lines rather than on a PPM.

## Take 69 — P76 refuted, the artifact identified, and the instrument was blind to it

Checks 1–3, scored from the log before anything else, all pass:

    rp #0.0/#1.0/#4.0/#7.0/#10.0: ... +MASKED(present) +PRESENT-CAND   (all five)
    mv final: img #52 writers — masked rp [7,0] unmasked rp []
    MIXED WRITERS: 0 occurrences

Check 4 fails: Patola still sees the dark patches. **P76 is refuted and `rp #7`
is exonerated**, per the condition committed before the run. The mixed-writers
bug was real and the fix is kept — layer 1 was genuinely missing every UI pass —
but it was not causing the artifact.

### What the artifact actually is

Patola circled it (`/tmp/x4-patch.jpg`): the cockpit dashboard rim, upper left.
Cropping the same region out of the take 69 dumps at native resolution shows it
without ambiguity. The rim is one flat panel, and on it:

* **left eye** — dark from x≈0 to a hard boundary at **x≈290**, then lit;
* **right eye** — the same panel lit from **x≈110** onward.

The panel's own bolts and seams give its disparity as a few tens of px. The
boundary between lit and dark moves by far more than that. **A shadow lies on a
surface and must move by that surface's disparity**; this one does not, so the
two eyes disagree about where the shadow falls. This is not a reflective-surface
or specular effect, which is what P74 assumed. It is a shadow/lighting
terminator placed from a reconstruction that is wrong for at least one eye —
and it is the failure class Patola's earlier X4 VR attempt died on.

### The instrument could not see it, by construction

`stereo_residual.py`'s verdict is `sure = bad & rel`, and `rel` means NCC ≥ 0.7.
The shadowed half of the panel is flat and textureless, so it correlates with
nothing, fails the gate, and is dropped **before** its brightness is compared.
The tool was built to find a surface lit 2.4x too brightly. This is a surface
half *missing*, and the gate that made the first measurable made the second
invisible.

That is the fourth aggregate in this project to hide this defect: `l1/l0`, then
two versions of the tile verdict, now the confidence gate.

**Therefore P74's refutation is void.** Take 68 read 9.9% → 9.4% with
`patch_fragment_invproj_eye` off and the patch was declared exonerated. That
number is structurally incapable of counting this defect, so the experiment was
**uninformative, not exonerating**. P74 is reopened.

`unmatched_dark()` is the repair: warp by the propagated disparity — the
shadowed patch has lit, textured neighbours that did match, and it lies on their
surface — then compare absolute levels with no confidence gate. Validated: it
reads **0.00%** on the IPD=0 control and fires on the live frame.

    take 67 (invproj ON, 236 modules)     1.71% left-dark / 1.08% right-dark   asym 1.6x
    take 68 (invproj OFF)                 2.15% / 0.89%                        asym 2.4x
    take 69 (invproj OFF + mask fix)      1.65% / 0.96%                        asym 1.7x

### Why this table settles nothing, and what does

Take 67 was a **different view** — asteroids and nebula, not the cockpit. Its
1.6x cannot be compared with take 69's 1.7x, and reading it as "invproj does not
help" would repeat the error of comparing takes 67 and 68 tile-by-tile when the
camera had moved. The A/B has to be run on one scene.

### P77 — the terminator mismatch is the shared depth→world reconstruction

The layer's own comment at the `patch_fragment_invproj_eye` call site already
describes this defect and names the takes it was seen in:

    // The deferred passes reconstruct view position from the depth buffer,
    // which was rendered through the sheared clip position, so they get the
    // position in *that eye's* frame -- and then light it with shadow
    // matrices and light positions that are still centre-frame. The two
    // eyes end up disagreeing about where a shadow falls on a surface by
    // the full IPD.

That patch has been **off** since take 68, because I turned it off to test P74
and then kept it off on a refutation that was not valid.

    X4VR_TAKE=70-INVPROJ-ON X4VR_STEREO=1 X4VR_IPD=0.064 X4VR_PROJ_INVPROJ=1
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1 X4VR_MV_DUMP=/tmp/x4vr-t70
    X4VR_MV_DUMP_IMG=52 X4VR_LOG=/tmp/x4vr-take70.log ./launch/x4vr-launch.sh

Take 69's command plus `X4VR_PROJ_INVPROJ=1`. **Same save, same view, camera
still** — the comparison is a crop at fixed coordinates, so a moved camera
voids it exactly as it voided the take 67 comparison. Confirm the knob fired
(`invproj final: ... ~236 modules corrected`) before reading anything.

P77 predicts the left-dark excess falls materially below 1.65% and the asymmetry
moves toward 1.0, **and** that the rim crop at `x=0..620, y=440..720` shows the
two boundaries at the same place. If the numbers move but the crop does not, the
numbers are measuring something else again — the crop is the verdict.

**If it does not improve**, invproj is genuinely refuted, this time on a
measurement that can see the defect, and the next suspect is named below. Do not
add a knob.

### The next suspect, and another degenerate control

Six compute modules refuse per-eye patching outright:

    bindless mirror final: index-offset patch — 354 modules edited, 8 declared a
    mirrorable table and REFUSED (6 of those are compute: no gl_ViewIndex exists
    there)
    mv final: compute — 14 pipeline(s), 6 shader(s) dispatched
      #362: 27752 dispatches   #363: 1736   #191: 1044   #364: 867

Multiview does not cover compute, so anything those shaders compute is computed
once and used by both eyes. Clustered/froxel lighting lives there.

P71 excluded compute from causing the `#57` lift by running at **IPD=0** and
observing bit-identical layers. That control is degenerate for *this* question:
at IPD=0 both eyes are the same, so a shared compute result is trivially correct
and cannot show a difference. The exclusion is true for what it tested and says
nothing about compute-computed lighting at IPD≠0. Recorded rather than relied
on — this is the second time an IPD=0 control has been mistaken for a general
one.

### The view is not repeatable, so a fixed-coordinate crop cannot be a verdict

Take 70 was to be scored by cropping the rim at fixed coordinates, on the
assumption that loading the same save the same way gives the same view. It does
not. Takes 68 and 69 were loaded identically:

    take 68 vs take 69 (left eye, whole frame)  NCC = +0.2834
    the rim crop specifically, t68 vs t69       NCC = -0.1186

The ship drifts and the probe fires several seconds after load, so no two runs
share a frame. That invalidates more than the plan: the take 68 -> 69 comparison
(2.15% -> 1.65% left-dark) was also across different scenes, which is the exact
flaw called out one paragraph earlier for takes 67 vs 68 and then repeated.

**Anything compared at fixed image coordinates across runs is void.** What
survives is (a) within-frame, view-independent measurements — a shadow's
disparity against the disparity of the surface it lies on; (b) frame-wide
statistics read across the three dumps of a run, as a trend and not a value;
and (c) Patola's eye, which has located this defect twice in one glance after
three tools reported nothing.

P77's verdict is therefore Patola's report — gone / unchanged / better but
visible — with the numbers as cross-check. The run command is unchanged; the
instruction to hold the view is withdrawn as unachievable.

## Take 70 — P77 refuted, and the artifact is a per-eye shadow test

`invproj final: ... 236 modules corrected` — the knob fired. Patola: the patches
are still there, with a screenshot circling the same wing in both frames.

**P77 is refuted.** Together with takes 68/69 (patch off, artifact present),
`patch_fragment_invproj_eye` is now eliminated from **both** directions: it
neither causes nor cures this. Two-sided exclusions are worth more than one, and
this one closes P74 properly as well — the earlier "refutation" was scored on a
blind metric, but the conclusion happens to hold.

### It is not a moved shadow edge, it is a whole face

Measured on the wing in the take 70 n2 dump, at the region Patola circled:

    wing panel  x=845..1408 y=845..1130
      left eye mean 46.25    right eye mean 70.75    -> 1.53x
      p90:  left 77.0        right 189.0             -> 2.45x at the bright end
      NCC +0.033 -- the two eyes barely correlate structurally at all

A whole flat face going from dark grey to blown-out white is not a specular
lobe and not a 64mm parallax effect: at a few metres, a 32mm eye offset moves
the reflection vector by under a degree, which cannot flip an entire surface.
What flips a whole surface is a **binary test** — in shadow, or not.

And the pipeline agrees. At the cockpit frame:

    #57  non-empty 541543/538916   changed=419991   level 0.004788/0.008838  (l1/l0 1.846)
    #52  non-empty 1982464/1982464 changed=1062954  level 47.76/50.4         (l1/l0 1.055)

`#57` is the lighting output and layer 1 is **1.85x brighter at equal coverage**.
A horizontal shift moves content; it cannot make the same number of lit texels
1.85x brighter. The right eye is missing shadowing.

**This reopens the `#57` lift.** Task #22 closed it as "correct stereo, not a
defect", on the strength of the IPD=0 control plus a tile comparison made with
the first version of `stereo_residual.py` — the version that selected tiles by
absolute residual and so dropped differently-lit tiles from the average meant to
find them. The exclusions P71 produced remain true; the verdict does not.

### What the IPD=0 control can and cannot exclude

At IPD=0 the two layers of every doubled image hold identical content, so
sampling either one gives the same answer. That control therefore cannot detect
a mirror descriptor that points at the wrong *layer* of the right image — only
one that points at nothing. Its clean result at take 65 is consistent with both
a correct mirror and a mirror whose error is invisible when the layers agree.
Third time an IPD=0 result has been read as more general than it is.

### P78 — the bindless index offset is what makes view 1 shade differently

Two mechanisms now remain that can make view 1 differ from view 0:

1. the shear K on `gl_Position` — geometry, and required for stereo at all;
2. `X4VR_BINDLESS_PATCH` — `element = index + gl_ViewIndex * 26653`, which
   sends view 1 to a different descriptor for **every sampled texture**,
   including whatever the lighting pass reads to decide "in shadow".

Turning (2) off isolates (1). Existing knob, no new code.

    X4VR_TAKE=71-NOBINDLESSPATCH X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=0 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1 X4VR_MV_DUMP=/tmp/x4vr-t71
    X4VR_MV_DUMP_IMG=52 X4VR_LOG=/tmp/x4vr-take71.log ./launch/x4vr-launch.sh

Take 70's command with `X4VR_BINDLESS_PATCH=0` and `X4VR_PROJ_INVPROJ` dropped
(eliminated, so it stays out). Confirm from the log that the index-offset patch
edited **0 modules** before reading anything.

**Expect other things to get worse.** With no offset, view 1 samples view 0's
copy of every per-eye render target — SSAO, reflections, anything deferred. That
is fine; this run asks one question only: **does the wing still flip between
lit and dark?**

* still flips -> the bindless path is exonerated, and what remains is the shear
  itself: the geometry is displaced per eye while something the lighting reads
  is not. `#57`'s 1.846 becomes the thing to dump and measure directly
  (`X4VR_MV_DUMP_IMG=57`).
* does not flip -> the offset is sending view 1 to a wrong descriptor, and the
  next question is which table: the mirror is only a correct twin for images
  that were actually doubled, and the shadow maps are MONO depth-only passes
  that never were.

Do not add a knob to fix a failed run.

## Take 71 — the run was degenerate, but it handed over two decisive facts

`X4VR_BINDLESS_PATCH=0`, log-confirmed (`index-offset patch — 0 modules
edited`). Patola: the missing shadows came back in the right eye, **and the two
frames no longer differ by any IPD at all**.

He is right, and it makes the run uninformative for the question it was asked.
`#52` reads `IDENTICAL` on every one of six samples, `l1/l0 1.000`. The
composite samples the scene through the same bindless heap, so with no offset it
reads view 0's copy for both eyes. **The artifact vanished because the stereo
vanished** — the same degeneracy as IPD=0, and the same shape of mistake as take
68. P78 is unanswered, not confirmed. Recorded rather than salvaged.

### Fact 1 — the scene IS reproducible, so cross-run comparison is back

    take 71 n2 vs take 70 n2 (different runs)   NCC = +0.9986
                 n3 / n4 / n5                   +0.9892 / +0.9950 / +0.9871

Load the save, touch nothing for three minutes, and the frame repeats to three
decimals. The earlier +0.2834 between takes 68 and 69 was runs where the view
had been moved, not an inherent property. The section above withdrawing
fixed-coordinate verdicts is itself withdrawn: it is valid whenever the controls
are untouched, and that is now the protocol.

### Fact 2 — the difference is born in the fullscreen passes

|  image                | offset ON (take 70) | offset OFF (take 71) |
|---|---|---|
| `#61` G-buffer        | l1/l0 **1.005**     | 1.005, still DIFFERs |
| `#57` lighting output | l1/l0 **1.846**     | 31.1 / 67.4 (garbage, never sampled) |

The G-buffer's two eyes agree to 0.5%; the lighting output's disagree by 85%.
`#57` is written by six passes, of which **`rp #31` and `rp #32` write nothing
else**:

    rp #31.0: 1 colour [97H] no-depth final=1 -> STEREO (world)
    rp #32.0: 1 colour [97H] no-depth final=1 -> STEREO (world)
    rp #23.0/#24.0/#25.0/#53.0: 1 colour [97H] depth 126 final=2 -> STEREO (world)   <- the G-buffer

**No depth attachment.** Depth-tested world geometry cannot be rasterised
without one, so these are fullscreen triangles — deferred lighting — and they
were classified `STEREO`, meaning K was being applied to them.

Counting the whole frame:

    no-depth final=1 -> STEREO      3
    no-depth final=2 -> STEREO     26      <- 29 fullscreen passes, all sheared
    depth 126 final=2 -> STEREO    12      <- the actual world geometry

`rp #52` is in that list. It is the 4096x1 exposure reduction, which is as
clearly not world geometry as a pass can be, and it was being sheared.

### P79 — shearing a fullscreen triangle is the defect

This file already contained the argument, written for the tonemap: *"it is a
fullscreen triangle, so K must NOT be applied to it -- shearing a fullscreen
triangle is meaningless."* `classify_unsheared()` exempted depth-only passes and
all-LDR passes. **An HDR fullscreen pass falls through both**, and 29 of them
did.

Shearing one displaces the quad sideways per eye while the buffers it samples
stay put, so every fragment reads the wrong texel. On the hull that decides
lit-or-shadowed, which is why a whole face flips, and why the ratio explodes
between a pass that carries depth and one that does not.

The fix adds one clause to `classify_unsheared()` — no depth attachment and at
least one colour attachment means fullscreen, do not shear — using the *index*
and not the pointer, which is the lesson `subpass_is_present()` cost 37 takes.
`classify_per_eye()` gains the same clause so these passes stay masked; they
were masked before by falling through `!unsheared`, and unmasking them would
leave layer 1 with no lighting at all. **The set of doubled passes is therefore
unchanged, and so is the frame cost.** Only K's reach shrinks, from 41 passes
to 12.

`X4VR_SHEAR_NODEPTH=1` restores the old behaviour, so the previous state is
reachable from a knob and not only from git.

    X4VR_TAKE=72-NOSHEAR-FULLSCREEN X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1 X4VR_MV_DUMP=/tmp/x4vr-t72
    X4VR_MV_DUMP_IMG=52 X4VR_LOG=/tmp/x4vr-take72.log ./launch/x4vr-launch.sh

Take 70's configuration exactly, with the rebuilt layer. Load, touch nothing for
three minutes.

Score from the log first:

1. the 29 no-depth passes now read `MONO (fullscreen post) +MASKED(fullscreen)`;
2. `#52` still **DIFFERs** — if it reads IDENTICAL the stereo has been destroyed
   again and the run is degenerate like take 71;
3. `#57`'s `l1/l0` falls from **1.846** toward `#61`'s 1.005;
4. then the eyes: is the hull shaded the same in both?

P79 predicts all four. **If `#52` goes IDENTICAL**, masking was lost somewhere
and the per_eye clause is wrong. **If `#57` stays at 1.846**, the fullscreen
shear was not the mechanism and the next question is what else rp #31/#32 read
differently per eye — the bindless offset applied to a table that is not a
per-eye render target. Do not add a knob to fix a failed run.

Because the view now repeats, take 70's dumps are a true before-image:
`~/x4vr-baselines/x4vr-t70-img52-n2-layer{0,1}.ppm`, wing at
`x=845..1408 y=845..1130`, left mean 46.25 against right 70.75.

## Take 72 — P79 refuted, and the error is a function of distance

The classification changed as intended (36 no-depth passes now read
`MONO (fullscreen post) +MASKED(fullscreen)`), `#52` still DIFFERs so the stereo
survived, and `#57` held at **1.863** against take 70's 1.846. The view repeated
(NCC +0.9892 against take 70 n2), so the wing is directly comparable:

                                    left    right   ratio    p90
    take 70 (fullscreen sheared)    46.25   70.75   1.53x    77.0 / 189.0
    take 72 (fix)                   46.22   70.73   1.53x    77.3 / 189.0

Identical to two decimals. **P79 is refuted.** In hindsight the mechanism was
never sound: a fullscreen triangle is oversized and its shaders take UVs from
`gl_FragCoord`, so sliding its clip position changes no fragment's input. The
change is kept — K has no business on those passes and it provably costs
nothing — but labelled as a no-op, not a fix.

Take 72 also ran without `X4VR_PROJ_INVPROJ` while take 70 ran with it. Two
different patches, both bit-exact no-ops on the same pixels.

### A fifth aggregate, this one written while fixing the fourth

    disparity  p5/p50/p95 = 0/0/0 px      shifts +0..+0px

`plausible_window()` took the sign of the disparity from the **median** of
confidently-matched tiles. This is a space game: most of the frame is stars and
nebula at infinity where the disparity genuinely is zero, so the median is zero,
the window collapses to {0}, and the search switches itself off. Take 70 read
the same way, so the flagged percentages quoted from it were meaningless.

Fixed to read the 2nd/98th percentile — the information is in the tail, where
the near geometry is. Control still 0.0%; take 72 now reports a window of
`-320..0` and p5 = -64px.

### The measurement that matters

With a working disparity field, binned by depth (disparity is 30.04/z_v px):

    |disparity|      depth        n    median    p90     p99    %tiles >1.25x
       0-   2 px  15.02- 999.0 m  310   1.000   1.020   1.314        1.3%
      16-  32 px   0.94-   1.9 m  110   1.009   1.109   3.929        2.7%
      32-  64 px   0.47-   0.9 m  187   1.010   1.253   2.300       10.2%
      64- 320 px   0.09-   0.5 m   47   1.014   2.708   4.799       31.9%

**The error scales with nearness.** Beyond 15m the eyes agree at baseline;
inside half a metre a third of tiles differ by more than 1.25x and the tail
reaches 2.7x. Note the medians: 1.000 to 1.014 across every bin. A median would
have reported this frame as clean, again.

A constant world-space position error produces exactly this curve — 32mm is
nothing at 15m and everything at 0.3m.

### A correction to the record

An earlier reading here claimed take 71's `#52` layer 0 was a mono ground truth
proving the **left** eye correct. It is not. Take 71 ran with the offset off, so
its layer 0 *is* the view-0 path, the same path as take 72's layer 0; agreement
to 0.06% shows that path is stable across runs and nothing more. Both eyes are
sheared (`L=+0.42666 R=-0.42666`), so neither is a neutral reference. What is
established: the eyes differ, and the difference concentrates on near geometry.

### P80 — refuted offline, no run spent

`patch_fragment_invproj_eye` corrects `kCameraInvProjMember = 2` and only
member 2. X4 uses TAA, and the block carries `3 M_projection_uj,
4 M_invprojection_uj`, so a pass reconstructing world position had every reason
to read the un-jittered inverse and be missed entirely. Scanning all 409 dumped
modules for fragment-stage loads of the camera block at set 1 binding 0:

    member  name                  modules loading it
        2   M_invprojection       244
        4   M_invprojection_uj      2   (both of which also load 2)

Zero modules load member 4 alone, and the 244 matches the log's "244 of 409
eligible" exactly — which also confirms the stale dump is representative of this
run's shader set even though its serials are not. The patch's eligibility is
correct and it does reach the position-reconstructing shaders. P80 refuted.

### The blocker, and what take 73 is for

Every remaining hypothesis needs to know what modules **#364/#366/#368** — the
deferred lighting bound to `rp #30`, executing inside `rp #31/#32` — actually
do. That cannot be read from `/tmp/x4vr-shaders/`: module serials are per-run,
and take 69 logged `#364` as a *compute* module while take 72 logs it as a
fragment module. Joining the two would be the same class of error as joining
pipeline render passes to framebuffer render passes, which is how `rp #31/#32`
appeared to have no shaders at all — pipelines are created against one
`VkRenderPass` and executed inside a compatible one, so the two sets are nearly
disjoint:

    passes with fragment modules:  #0 #1 #2 ... #26 #28 #30 #33 #34 ...
    passes that own framebuffers:  #0 #1 #7 ... #27 #29 #31 #32 #33 #35 ...

Take 73 is pure observation — no behaviour change, nothing to report by eye:

    X4VR_TAKE=73-DUMPSHADERS X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1 X4VR_DUMP_SHADERS=/tmp/x4vr-shaders-take73
    X4VR_MV_DUMP=/tmp/x4vr-t73 X4VR_MV_DUMP_IMG=57
    X4VR_LOG=/tmp/x4vr-take73.log ./launch/x4vr-launch.sh

Take 72's configuration plus `X4VR_DUMP_SHADERS`, and the dump retargeted from
`#52` to `#57` so the lighting output itself can be seen per eye rather than
inferred from a ratio. It answers, offline and permanently:

* what #364/#366/#368 read, and whether the invproj correction lands in them;
* where in the frame `#57`'s 1.86x actually lives.

The shader dump must not be committed — X4's modules are copyright, and they
live in `/tmp` for that reason.

## Take 73 — the right eye has been sampling an empty shadow map

`X4VR_DUMP_SHADERS` wrote nothing: the layer does not create the directory and
warns instead — `WARNING: X4VR_DUMP_SHADERS=/tmp/x4vr-shaders-take73 is not
writable`. My error for passing a path that did not exist. The directory is
created now.

The `#57` dump paid for the run anyway, and it moved the defect two stages
upstream.

    mv probe: wrote /tmp/x4vr-t73-img57-n2-layer{0,1}.ppm (fmt 97, after rp #24, DIFFER)

    wing         left 27.99   right 47.18   1.69x
    upper strut  left  6.35   right  7.88   1.24x
    whole frame  left  7.99   right  9.79   1.23x

**After `rp #24`** — a forward geometry pass, with depth, before the deferred
lighting of `rp #31/#32`. So the difference is not made in deferred lighting at
all; it is already there when the geometry pass writes. And the two eyes' wings
keep the same panel lines, seams and vents — only the brightness differs, so it
is not a wrong albedo either. It is a shading *input*, sampled during a forward
pass.

### The mechanism

    img #70: 2048x2048x1 layers=1 mips=1 samples=1 fmt=124 usage=0xa6 DOUBLED
    img #71..#74: identical
    mv final: img #70 writers — masked rp [] unmasked rp [35]
    mv final: img #71..#74 writers — masked rp [] unmasked rp [37/39/41/43]

The five 2048x2048 shadow cascades are **doubled** — they go through the same
allocation path as everything else — but every one is written **only by
unmasked passes**. That is correct: a shadow map is light space and genuinely
shared between the eyes, and `classify_unsheared()` has always said so.

**So layer 1 of every shadow map is allocated and never written.**

And the bindless mirror substitutes layer 1 for *any* doubled image, without
ever asking whether anything wrote it:

    const VkImageView l1 = view_of_layer(d, device, infos[j].imageView, 1);
    if (l1 == VK_NULL_HANDLE)
        continue; // undoubled: the verbatim copy is the right answer
    infos[j].imageView = l1;

So view 1 — the right eye, reached through `index + 26653` — did every shadow
lookup against an unwritten depth image. Under reverse-Z an empty depth reads as
the **far plane**: nothing occludes anything, and every surface comes out fully
lit.

This accounts for the whole history:

* the right eye blown white and unshadowed, the left correct — Patola from take
  64 onward, and "the shadows which didn't appear in the right side";
* worst on the ship's own hull (nearest, self-shadowing) and absent on nebula
  and stars, which receive no shadows — the "distance dependence" measured at
  take 72 is really *which geometry receives shadows*;
* present already in `#57` after a forward pass, since that is where materials
  do their shadow lookup;
* untouched by invproj, by `rp #7`, by the fullscreen shear — none of them go
  near a descriptor;
* **and take 71 curing it by accident.** `X4VR_BINDLESS_PATCH=0` stopped view 1
  consulting the mirror at all, so it read the real shadow map. That run lost
  its stereo for the same reason, which is exactly why it looked like a cure and
  had to be discarded as degenerate. It was half a result, and the half that was
  real is this.

### The fix

`layer1_is_written()` asks `g_img_writers` — already tracked, already printed in
the inventory — whether any *masked* pass writes the image. An image with an
unmasked writer and no masked one is shared by construction, so leaving view 1
pointed at layer 0 is not a fallback for it, it is the correct answer. Unknown
writers keep the old behaviour rather than silently making a genuinely per-eye
image mono on missing information.

`X4VR_MIRROR_ALL_LAYER1=1` restores the old mirror. The log now separates the
two cases: `%llu of them layer-1, %llu kept at layer 0 as shared (unmasked
writers only)`.

### P81 — take 74

    X4VR_TAKE=74-MIRROR-SHARED X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_DUMP_SHADERS=/tmp/x4vr-shaders-take74 X4VR_MV_DUMP=/tmp/x4vr-t74
    X4VR_MV_DUMP_IMG=52 X4VR_LOG=/tmp/x4vr-take74.log ./launch/x4vr-launch.sh

Score from the log first:

1. `bindless mirror final: ... kept at layer 0 as shared` is **non-zero**;
2. `#52` still **DIFFERs** — IDENTICAL means the stereo is gone and the run is
   degenerate like take 71;
3. the wing at `x=845..1408 y=845..1130` in `/tmp/x4vr-t74-img52-n*`: left and
   right should converge from take 72's 46.22 / 70.73;
4. then Patola's eyes.

**If the shadows match but the stereo is gone**, the predicate is too broad and
is sharing genuinely per-eye images. **If nothing changes**, the shadow maps are
not reached through this descriptor path and the shader dump — which will
finally exist — says what modules #368/#370/#372 actually sample.

The shader dump must never be committed; X4's modules are copyright, which is
why they live in `/tmp`.

## Take 74 — P81 refuted, and the shader dump finally exists

The fix fired and the stereo survived:

    bindless mirror final: ... 167958 of them layer-1, 24350 kept at layer 0 as
    shared (unmasked writers only), 0 skipped for no room
    mv probe: img #52 ... DIFFER ... level 47.75/50.45 (l1/l0 1.057)

and the artifact did not move. Same view as take 72 (NCC +0.9929):

    take 72 wing   left 46.22  right 70.73   1.53x
    take 74 wing   left 46.14  right 70.65   1.53x

**P81 is refuted.** Either the shadow cascades are not reached through that
descriptor path, or an empty layer 1 is not what the right eye reads. The change
is kept — an image written only by unmasked passes is shared by construction and
pointing view 1 at its layer 0 is correct regardless — but it is not the fix.

Worth noting what "unknown writers keep the old behaviour" implies: if X4 writes
a shadow map's descriptor once at startup, before any framebuffer names it, the
predicate returns true and the substitution happens anyway. That is a live
possibility this run cannot distinguish from "shadow maps are not sampled
through binding 5 at all", and the shader dump can now settle it.

### The durable asset

`/tmp/x4vr-shaders-take74` holds **397 modules**, and `/tmp/x4vr-take74.log` is
its matching join. Module, image and render-pass serials are per-run, so these
two files are only meaningful together — and together they are the first time in
this project that a named module can actually be read. Every earlier attempt
joined a dump to a different run's log, which is how `#364` appeared as a
compute module in take 69 and a fragment module in take 72.

Never commit the dump: X4's modules are copyright, which is why they live in
`/tmp`.

### What is established about the defect

* It is in the **right eye** (layer 1, the `index + 26653` mirror path); the
  left eye's path is stable across every run.
* It is present already in `#57` **after `rp #24`**, a forward geometry pass, so
  it is not made in deferred lighting.
* The wing keeps its panel lines, seams and vents in both eyes and only its
  brightness changes, so it is not a wrong albedo — it is a shading input.
* It is proportional to nothing about the *screen* and everything about the
  geometry: shadow-receiving hull is hit hard, nebula and stars not at all.
* Eliminated with runs: `patch_fragment_invproj_eye` (both directions),
  `rp #7`/present masking, the fullscreen shear, `X4VR_SHEAR_LIGHTS`, the
  empty-shadow-map mirror. Eliminated offline: a per-eye exposure difference,
  `M_invprojection_uj`, and any descriptor write path the mirror could miss
  (both `vkUpdateDescriptorSets` and `vkUpdateDescriptorSetWithTemplate` are
  intercepted, and there are no push descriptors).

### The next step is reading, not running

The remaining question is what the forward-geometry fragment modules
(`rp #23/#24/#25` <- modules #100, #102, #104, #106, ...) actually sample and
how they index the heap — in particular whether any index reaches the mirror at
a slot X4 never wrote. That is now answerable offline against
`/tmp/x4vr-shaders-take74`, and it should be answered before another run is
spent.

## The shaders answered it: the predicate was right and never ran

Read offline against `/tmp/x4vr-shaders-take74` and its matching
`/tmp/x4vr-take74.log`. No run was spent.

### Shadow maps are sampled through binding 5

`tools/shadow_scan.py` resolves the **Fragment** entry point of each dumped
module, follows `OpFunctionCall` transitively, and walks each sample
instruction back through `OpSampledImage` / `OpLoad` / `OpAccessChain` to the
descriptor it came from. Scanning the fragment stage only matters: X4 ships
combined vertex+fragment modules, and a whole-module scan answers a different
question — the trap recorded for `classify()` at take 60.

    385 fragment modules scanned, 56 sample a depth image
    every one of them: set 0 binding 5, OpImageSampleDrefImplicitLod

Binding 5 is exactly where the mirror applies `index + 26653` for view 1. So
the right eye's shadow lookups do go through the mirror, and the question P81
asked was the right question.

### The five images, and the 19.5-second window

The cascades are unmistakable in take 74's log:

    646088.047  img #70..#74: 2048x2048x1 layers=1 mips=1 samples=1
                              fmt=124 usage=0xa6 DOUBLED

`fmt=124` is `D32_SFLOAT`; `usage=0xa6` is
`INPUT_ATTACHMENT|DEPTH_STENCIL_ATTACHMENT|SAMPLED|TRANSFER_DST`. Five of them,
all doubled, and the writer inventory says they are written by mono passes
only:

    mv final: img #74 writers — masked rp [] unmasked rp [43]
    mv final: img #73 writers — masked rp [] unmasked rp [41]
    mv final: img #72 writers — masked rp [] unmasked rp [39]
    mv final: img #71 writers — masked rp [] unmasked rp [37]
    mv final: img #70 writers — masked rp [] unmasked rp [35]

(Read that with the anchor `writers — masked rp []`. Grepping `masked rp \[\]`
matches `unmasked rp []` as a substring and reports all 42 images — the aliased
first-match error, for the fifth time.)

`layer1_is_written()` returns false for exactly this shape, which is why take 74
shipped it. The timestamps say why it did not help:

    646088.047  images created, DOUBLED
    646088.204  bindless mirror first present: ... 0 kept at layer 0 as shared
    646107.581  rp #35.0: 0 colour [] depth 124 final=-1 -> MONO (depth-only/shadow)
    646107.581  fb  rp #35: 2048x2048 layers=1 attachments=1 imgs=[#70]

X4 creates the cascades and puts them in the bindless heap **19.5 seconds
before** it builds a framebuffer naming them, and `g_img_writers` learns
nothing until framebuffer time. Every shadow slot written in that window took
the unknown-writers branch, got layer 1, and was never revisited — the heap is
written once and left. The predicate was correct and simply never ran on the
descriptors it was written for. This is the same class as take 68's
`subpass_is_present`: not a wrong rule, a rule consulted against information
that was not there yet.

It also explains the sign. Under reverse-Z an unwritten depth image reads as the
far plane, so nothing occludes anything and the surface comes out fully lit —
and the right eye is the *brighter* one, 70.7 against 46.2.

### P82 — the repair

Committed before the run that tests it.

`layer1_is_written()` becomes a tri-state `layer1_state()`; a slot filled while
the writers are unknown is **recorded**, and `repair_mirror_for_image()` puts
the verbatim view back the moment a framebuffer shows the image to be
unmasked-only. `X4VR_MIRROR_REPAIR=0` disables the repair for an A/B in one
build.

**P82: the wing's right/left ratio falls from 1.53x to about 1.0, and the log
reports a non-zero repaired count for images #70-#74.**

What refutes it, and how each reads differently in the log:

* `slot(s) filled on unknown writers` is **0** — the race does not exist and
  this whole account is wrong.
* filled is large but `repaired` is **0** — the race exists, the repair never
  fired; look at the framebuffer path, not the predicate.
* both non-zero and the wing still reads 1.53x — the empty shadow map is real
  but is not what Patola is seeing, and the shadow story is finally spent.

## Take 75 — P82 refuted, and the shadow-map account was false from the start

    X4VR_TAKE=75-MIRROR-REPAIR X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1 X4VR_MV_DUMP=/tmp/x4vr-t75
    X4VR_MV_DUMP_IMG=52 X4VR_LOG=/tmp/x4vr-take75.log ./launch/x4vr-launch.sh

The wing at the same view, `x=845..1408 y=845..1130`:

    take 72  left 46.22  right 70.73  1.530x   p90 77.3/189.0
    take 74  left 46.27  right 70.77  1.530x   p90 77.3/189.0
    take 75  left 46.27  right 70.77  1.530x   p90 77.3/189.0

Take 75 is take 74 to the last printed digit. P82 refuted.

### The instrument was blind, again

The log said `0 slot(s) filled on unknown writers`, which is the branch P82
named as "the race does not exist and this whole account is wrong". The
conclusion was right; the reasoning was not, and the number proved nothing.

The counter was placed *after* this:

    const VkImageView l1 = view_of_layer(d, device, infos[j].imageView, 1);
    if (l1 == VK_NULL_HANDLE)
        continue;                      // <-- counter is below this line

and `view_of_layer()` returns null unless the image is in `g_per_eye_images`.
So the gate removed exactly the population the counter existed to count. This
is the third instrument in this project blind to its own target, after
`sure = bad & rel` and the `rp #7` signed-difference image. The counter now
sits above the gate and the log line distinguishes "undecided" from
"substituted and recorded".

### The premise, not just the prediction

`g_per_eye_images` gains an image in exactly one place: `CreateFramebuffer`'s
`masked && g_active` branch. A shadow map is never attached to a view-masked
pass — the inventory says so, `masked rp [] unmasked rp [35]` — so it is never
in the set, so `view_of_layer()` has **always** returned null for it, so the
mirror has **always** written the verbatim view.

**The right eye has been sampling the real, correct shadow map in every take.**
P81 and P82 were both built on a mechanism that cannot occur. Take 74's
`layer1_is_written()` and take 75's repair are no-ops by construction: the
34,250 descriptors that take the early exit would reach the identical verbatim
path one line later. Both are kept as guards, with the comments corrected to
say so, because a wrong comment is worse than the dead branch it describes.

Only **24 images** in the whole frame can ever receive a layer-1 substitution —
the ones with a masked writer. They are all scene targets. No texture, no
shadow map, no lookup table is ever mirrored to layer 1.

### invproj, settled properly this time

`tools/shadow_scan.py` and a member-2 scan over the take-74 dump:

    fragment modules sampling Dref (shadow):                    56
    fragment modules loading camera member 2 (M_invprojection): 236
    BOTH -- shadow lookup on a reconstructed position:          56
    of those 56, member-2 chain in the fragment entry function: 56

So every shadow lookup in the game does run on a position reconstructed through
`M_invprojection`, and the patch reaches all of them — no helper-function gap.
The patch's arithmetic is correct, verified offline against the logged shear:

    P = [[sx,0,0,0],[0,sx,0,0],[0,0,0,n],[0,0,-1,0]],  sx=1.3333 n=0.1
    K = P.T(-d).P^-1  ->  K[0][2] = +0.42666 / -0.42666   (matches the log)
    row0 += d*row3    ==  inv(P.T(-d))                    exactly, both eyes

And it still changes nothing. Take 70 ran with the patch firing
(`invproj final: 236 modules corrected`); take 75 ran with it off
(`0 modules corrected`). Same view, NCC +0.9991:

    take 70  invproj ON   wing L 46.25  R 70.75  = 1.530x
    take 75  invproj off  wing L 46.27  R 70.77  = 1.530x

That is not a null result to shrug at — it is explained. The error the patch
removes is a world-space offset of `ipd/2` = **3.2 cm**, and the cascades are
2048x2048 over a spaceship exterior. The correction is far below one shadow
texel, so it cannot move a shadow boundary. invproj is refuted for this
artifact, and refuted for a reason.

It also rules out the shape of the defect: 3.2 cm of positional error cannot
produce `p90 77 vs 189`. Whatever the right eye is doing, it is not a slightly
displaced version of the left eye's shading.

### What is left

Every mechanism proposed so far has been eliminated, and the eliminations are
sound. What has never been done is the direct one: **find the first image in
the frame where the two layers disagree by more than disparity explains.** The
mirror's layer-1 path touches 24 images; `#57` is known to carry the defect
already; `#55`, `#59`, `#60`, `#61` are written by the same forward passes and
are earlier. Dump those per layer and bisect. That names a pass instead of
proposing a tenth mechanism, and takes 68 through 75 are what proposing
mechanisms has been worth.

### P83 — bisect the frame instead of proposing a tenth mechanism

Committed before the run that tests it.

`X4VR_MV_DUMP_IMG` now takes a **comma-separated list** and the six-dump cap is
**per image**. This matters more than it sounds: serials are per-run, so `#57`
from one take and `#59` from the next cannot be compared at all, and one image
per run would need five runs of a moving scene to collect what one run collects
at a single view.

`write_ppm_rg()` adds `R16G16_SFLOAT`, which is what `#60` and `#61` are. The
probe could always read them — `format_bpp()` has covered the format all along —
so the only reason the G-buffer normals had never been looked at was the
dumper, the same gap that once hid `#103`. They carry negative values, so they
are written through the signed analogue of the existing tone map,
`0.5 + 0.5*v/(1+|v|)`: monotonic over the whole real line, zero at mid-grey,
nothing clipped.

The chain to dump, earliest to latest, with each image's masked writers:

    #61  fmt 83  rg16f   rp [13,17,22,24,23,25,44,50,53]   G-buffer
    #60  fmt 83  rg16f   rp [13,17,22,24,23,25,44,50,53]   G-buffer
    #59  fmt 97  hdr     rp [13,17,22,24,23,25,44,50,53]
    #57  fmt 97  hdr     rp [24,23,25,31,32,53]            known to carry it
    #65  fmt 50  bgra8   rp [33,45]
    #52  fmt 44  bgra8   rp [7,0]                          the presented eye

`#55` is the scene depth (fmt 126) and cannot be dumped as a PPM; that is a
known hole, not an oversight.

**P83: the defect is absent from `#61`/`#60` and present in `#57`, and `#59` is
the discriminator.** The earlier reading of `#61` at `l1/l0` 1.005 against
`#57` at 1.846 says the divergence is made *after* the G-buffer, so:

* If `#59` is clean and `#57` is not, the culprit is a pass that writes `#57`
  and not `#59` — that is **`rp #31` or `rp #32`**, and nothing else.
* If `#59` is already dirty, it is one of `rp #13/17/22/44/50`, and the
  G-buffer normals being clean says it is a lighting pass rather than geometry.
* If `#61`/`#60` are dirty, everything above is wrong and the defect is in the
  G-buffer itself, which would make it a geometry or normal problem and would
  contradict "the wing keeps its panel lines".

Measure the wing crop `x=845..1408 y=845..1130` and report the tail and the
affected area, never a bare median — the failure recorded twice already. The
`IPD=0` negative control belongs on any reading that looks like a finding.

## Take 76 — P83 confirmed, and the defect is born in `#57`

    X4VR_TAKE=76-BISECT X4VR_STEREO=1 X4VR_IPD=0.064
    X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1
    X4VR_MV=1 X4VR_MASK_PRESENT=1 X4VR_MV_PROBE=1 X4VR_MV_INVENTORY=1
    X4VR_PROJ_SX=1.3333 X4VR_PROJ_LIVE=1
    X4VR_MV_DUMP=/tmp/x4vr-t76 X4VR_MV_DUMP_IMG=61,60,59,57,65,52
    X4VR_LOG=/tmp/x4vr-take76.log ./launch/x4vr-launch.sh

The wing crop `x=845..1408 y=845..1130`, settled frames (n2-n4, all three
sequences agreeing to the printed digit):

    img  what          ratio R/L   p90 L / R
    #61  G-buffer        1.010     118.3 / 118.7
    #60  G-buffer        0.990      85.3 /  85.3
    #59  HDR             1.056      87.3 /  87.3
    #57  HDR             1.686      64.7 / 148.3
    #52  presented       1.530      77.3 / 189.0

`#52` reproduces takes 72, 74 and 75 exactly, so the chain is anchored at the
end that was already measured. **P83 confirmed**: clean through `#61`, `#60`
and `#59`, wrong at `#57`.

### The constraint this puts on the answer

The framebuffer inventory kills the obvious reading:

    fb rp #23: 1408x1408 attachments=6 imgs=[#55,#57,#59,#59,#60,#61] MASKED
    fb rp #24: 1408x1408 attachments=6 imgs=[#55,#57,#59,#59,#60,#61] MASKED
    fb rp #25: 1408x1408 attachments=6 imgs=[#55,#57,#59,#59,#60,#61] MASKED

`#57`, `#59`, `#60` and `#61` are written by the **same draws in the same MRT
pass**. It is therefore not "a pass is wrong": one shader, one invocation,
writes all four, and three come out clean while the fourth does not. Whatever
is wrong is specific to *that one output*, not to the geometry, not to the
pass, and not to the eye's sampling of the heap in general.

The only structural difference between `#57` and its co-attachments is that two
further passes write `#57` and nothing else:

    rp #31.0: 1 colour [97H] no-depth final=1 -> MONO (fullscreen post) +MASKED(fullscreen)
    rp #32.0: 1 colour [97H] no-depth final=1 -> MONO (fullscreen post) +MASKED(fullscreen)
    fb rp #31: attachments=2 imgs=[#57,#57]
    fb rp #32: attachments=2 imgs=[#57,#57]

### "masked" means three different things, and it mattered here

`mark_masked()` is called from exactly one place, `if (per_eye)`. So
`g_masked_passes` is the set of passes that **do** render per-eye, into both
layers, and the framebuffer log's `MASKED` means the same. The inventory's
`+MASKED(fullscreen)` means something else entirely: masked out of the *shear*.
Two opposite senses of one word, both printed in the same log, and the
resolution decides whether `rp #31/#32` write layer 1 at all. They do.

### One fact short, and it was never recorded

The probe reads `#57` after `rp #24` and finds it already wrong, which looks
like it exonerates `rp #31/#32` — they run later. It does not, because images
persist between frames. Two readings fit, and `loadOp` decides:

* `CLEAR`/`DONT_CARE` — the pass starts from nothing, so the divergence is made
  inside `rp #23/#24/#25`, by one output of one shader, and `#31/#32` are out.
* `LOAD` — the pass inherits the previous frame's `#57`, so what the probe sees
  includes `rp #31/#32` from frame N-1 and they remain suspects.

`loadOp` was not tracked anywhere in the layer. Ruling a suspect out with an
instrument that cannot see it is the take-68 `rp #7` mistake, so the inventory
now prints `rp #N attachments — loadOp 0:CLEAR 1:LOAD ...` and the next run
answers it without any behavioural change.

**P84: `rp #23`'s `#57` attachment is `CLEAR` or `DONT_CARE`, which puts the
defect inside the MRT pass and rules `rp #31/#32` out.** If it is `LOAD`, the
next step is `#31/#32` and not the G-buffer shader.

## The polarity was backwards for thirteen takes

Patola ran the **vanilla game**, unmodified, and loaded the same save at the
same view. The frame that matches vanilla is the **bright** one — the right SBS
frame, which is **layer 1**.

So every reading in this document from take 64 onward has the sign wrong:

* **Layer 1 (right eye) is correct.** It is not too bright; it is right.
* **Layer 0 (left eye) is the defect.** It is too dark, and the "dark patches"
  are spurious occlusion that vanilla does not have.

The bisection numbers stand, only their interpretation flips. `#57` is still
where the two layers part company; what happens there is that layer 0 *loses*
brightness, by 1/1.686 = 0.59, not that layer 1 gains it.

This also kills the shadow-map story a second time and more cleanly than the
structural argument did. That account required the right eye to be sampling an
empty shadow map and coming out wrongly lit. Vanilla says the right eye is the
one that is correct, so there was never anything to explain on that side.

### What it costs, and what it does not

The eliminations survive. Every one of them was a symmetric test — a knob on
against a knob off, scored on a ratio between the layers — and a ratio does not
care which layer is named the defect. `patch_fragment_invproj_eye`, the present
masking, the fullscreen shear, `X4VR_SHEAR_LIGHTS`, the mirror's shared-image
handling: all still eliminated.

What it costs is direction. The suspect list has been built around the mirror
and the `index + 26653` path for thirteen takes, because that is the machinery
unique to the right eye. **Layer 0 uses X4's own descriptors, unmodified —
index + 0 — so the broken eye is the one running closest to stock.** Anything
that explains the defect has to explain it on the path with the least
interference, which rules out the entire family of "the mirror substituted the
wrong thing" hypotheses by construction rather than one at a time.

### The lesson, recorded rather than edited away

Thirteen takes established *that* the layers differ and spent all of that effort
deciding *why the brighter one was wrong*, without ever asking which one was
right. The control that settled it costs one launch of an unmodified game and
was available from the first day. `IPD=0` was kept as a negative control
throughout and it was the wrong control: it proves the two layers can be made
identical, not which of them matches the game.

P84 is unaffected — `loadOp` decides between the MRT pass and `rp #31/#32`
regardless of which layer is named the defect. But it is now a question about
what layer 0 loses, not about what layer 1 gains.

## Take 77 — P84 confirmed, and a misreading of the framebuffer corrected

Launched, save loaded, quit after the fade-in. The inventory prints at pass
creation, so the run needed nothing else.

    rp #23 attachments — loadOp 0:LOAD  1:LOAD  2:LOAD 3:LOAD 4:LOAD 5:LOAD
    rp #24 attachments — loadOp 0:LOAD  1:CLEAR 2:LOAD 3:LOAD 4:LOAD 5:LOAD
    rp #25 attachments — loadOp 0:LOAD  1:LOAD  2:LOAD 3:LOAD 4:LOAD 5:LOAD
    rp #31 attachments — loadOp 0:LOAD  1:LOAD
    rp #32 attachments — loadOp 0:LOAD  1:LOAD

The pass serials, framebuffer shapes and classifications are identical to take
76, so the identification carries across the two runs — checked, because
serials are per-run and assuming otherwise is a mistake this project has
already made.

Attachment 1 is `#57` (`imgs=[#55,#57,#59,#59,#60,#61]`, and the framebuffer
list *is* positional). `rp #24` **clears** it. The probe samples `#57` after
`rp #24`, so everything it sees was written by `rp #24` itself: nothing
survives from `rp #31`/`#32` of the previous frame, and nothing from `rp #23`.

**P84 confirmed. `rp #31` and `rp #32` are ruled out**, and on a fact rather
than on the "later in the frame" argument that would not have been sound.

### The MRT reading was wrong

Take 76 said "`rp #23/#24/#25` write `#57` alongside `#59`, `#60` and `#61`, so
the same draws produce one wrong image and three clean ones, and the defect must
be one output of one shader". That is not what the log says. The subpass line
has always said:

    rp #24.0: 1 colour [97H] depth 126 final=2 -> STEREO (world)

**One** colour attachment. The other five entries are attached to the
framebuffer and untouched by the subpass. The framebuffer line lists
attachments; it does not list outputs, and reading it as outputs is the same
class of error as reading a list positionally — recorded twice before in this
document.

`g_img_writers` has the same flaw and it is worth naming because its output has
been quoted repeatedly here: it iterates `ci->attachmentCount` at framebuffer
creation, so its "writers" means **attached to a framebuffer of that pass**, not
**written by that pass**. Every "written by rp [...]" list in this document is
really "attached in rp [...]".

So the constraint from take 76 is weaker than claimed. What survives is the
measurement, which is untouched: `#61`, `#60` and `#59` are clean, `#57` is not,
and `#57` is cleared and rewritten by `rp #24`. The defect is created by
`rp #24`'s draws.

What is not yet known is which image `rp #24` actually writes. Two of the six
attachments share format 97, so the format in the subpass line cannot identify
it, and the index was never printed. The inventory now prints

    rp #N.M writes — colour [1] depth 0 input []

which resolves it directly. **P85: `rp #24.0`'s colour attachment is index 1,
`#57`.** If it is index 2 or 3 then `rp #24` writes `#59`, the clear on
attachment 1 is a clear of an image the pass does not draw into, and the
localisation has to start from the probe's sampling point instead.

### P85 confirmed — and the pass is a lighting pass, not the G-buffer

    rp #23.0 writes — colour [1] depth 0 input [2,3,4,5]
    rp #24.0 writes — colour [1] depth 0 input [2,3,4,5]
    rp #25.0 writes — colour [1] depth 0 input [2,3,4,5]
    rp #31.0 writes — colour [0] depth none input [1]

With `imgs=[#55,#57,#59,#59,#60,#61]`: attachment 1 is `#57`, so `rp #24`
**writes `#57`** while reading depth (`#55`) and the G-buffer (`#59`, `#59`,
`#60`, `#61`) as input attachments. These are lighting/composite passes. `#57`
is the lit result, and `#59`/`#60`/`#61` measuring clean is exactly what a
correct G-buffer looks like — the defect is introduced when that G-buffer is
*lit*, not when it is filled.

Module serials are per-run and the joins confirm it: take 74 lists 138 fragment
modules for `rp #23`, take 78 lists 132, and the serials differ. Take 74's dump
cannot be read against take 78's log.

### P86 — does the defect follow the layer, or the eye offset?

Two families are left, and one existing knob separates them. `configured_ipd()`
is a bare `strtof` with no clamp, so **`X4VR_IPD=-0.064`** flips the shear sign:
layer 0 gets the right eye's viewpoint and layer 1 gets the left's, while every
index, descriptor and code path stays exactly where it was.

* **Follows the eye offset** — something in the lighting is correct for only one
  sign of the shear. The dark patches move to the **right** SBS frame and the
  wing ratio inverts from 1.69 to about 0.59.
* **Follows the layer index** — the defect is in the multiview machinery for
  layer 0 specifically. The patches **stay on the left** and the ratio stays
  near 1.69.

**P86: it follows the layer index — the patches stay on the left frame.** The
reasoning is that layer 0 runs on X4's own unmodified descriptors and the
defect survived every symmetric knob, but the prediction is the weaker half of
this: either answer halves the search, which is the point of running it.

Scored from the dumps, not from the screen: `#57` and `#52` at the wing crop,
with NCC against take 76 to confirm the view before reading the ratio.

## Take 79 — P86 refuted: the defect follows the eye offset, not the layer

    X4VR_TAKE=79-NEGIPD X4VR_STEREO=1 X4VR_IPD=-0.064 [rest as take 76]
    stereo: ipd=-0.0640 sx=1.3333 near=0.100 -> shear m8 L=-0.42666 R=0.42666

    img #57, wing crop, settled frames
      take 76  ipd +0.064   L 27.99  R 47.18   ratio 1.686   p90  64.7/148.3
      take 79  ipd -0.064   L 47.18  R 27.99   ratio 0.593   p90 148.3/ 64.7

    img #52 (presented)
      take 76  L 46.22  R 70.72
      take 79  L 70.77  R 46.28

Not merely reversed — the same numbers, swapped, to the printed digit. And the
correspondence is structural, not just statistical:

    NCC img#57: t79.layer0 vs t76.layer0  +0.4973
                t79.layer0 vs t76.layer1  +0.9988

Negating the IPD made layer 0 render what layer 1 had rendered, defect and all.
**P86 is refuted. The defect follows the eye offset, and has no dependence on
the layer index whatsoever.**

That is worth stating precisely, because the whole investigation has been aimed
the other way: the rendered result is a pure function of the shear sign. Layer
0 and layer 1 are not different code paths as far as this defect is concerned.

### Which eye is the bad one, in terms that survive a sign flip

In both runs the eye with shear **m8 = -0.42666** is the bright, correct one and
the eye with **m8 = +0.42666** is dark. By `K[0][2] = -m0*d/n`, m8 = +0.42666
means `d = -0.032`: the camera displaced toward **-x**. Both displacements are
conventionally correct for their eye — this is not a sign error in the shear.

**The eye whose camera moves to -x comes out under-lit, whichever layer it
lands in.**

### What this kills

Everything that is specific to layer 1 or to the mirror. The `index + 26653`
path, `view_of_layer`, the array-view substitution, the input-attachment fix,
`layer1_is_written` and its repair: none of them can produce a defect that
tracks the shear sign and ignores the layer. Thirteen takes of suspects, closed
by one knob and no new code.

It also closes the polarity question properly. Patola's vanilla control said
"the bright one is correct"; take 79 says brightness follows the offset
direction, so the statement to carry forward is not "layer 0 is broken" but
**"the -x eye is broken"**, which is the form that survives.

### What is left

Something in the lighting is correct for only one sign of the eye displacement.
`rp #24` writes `#57` from depth `#55` and the G-buffer `#59/#60/#61`, so the
candidates are the reconstruction from depth, whatever screen-space term the
lighting applies, and any module in that pass whose shear is baked rather than
per-view. The last of those is already an open task (#23, the 12 baked-sx
modules) and is the only one of the three that is asymmetric by construction:
a baked shear is a single constant, so it is right for one eye and wrong for
the other. That is the next thing to check, and it needs a shader dump taken in
the same run as the log, since serials are per-run.

### Correction: baked sx is not asymmetric

The section above named task #23 — the 12 modules with a baked `sx` — as the
next suspect, on the grounds that a baked shear is one constant and therefore
"right for one eye and wrong for the other". That is wrong, and take 79's own
log refutes it without a run:

    patched vertex shader #350 (ui) [world=296 ui=54 stereo=296 live-sx=284 baked-sx=12]

`world == stereo == 296`: every world module is per-view. The baked path is
`patch_vertex_clip(code, K, KR)` and it takes the right eye's `KR` alongside
`K`, exactly as the live path takes `dl` and `dr`. A baked `sx` makes the shear
wrong in *magnitude*, equally in both eyes. It cannot produce a defect that
depends on the sign of `d`.

Recorded rather than edited away, because the reasoning that produced it is the
recurring one: "this is the only candidate with the right shape" is a claim
about code that was never read.

### What asymmetry actually requires

Both eyes get correct, symmetric vertex shears. So the asymmetry cannot come
from the shear itself — it has to come from something **fixed** interacting
with something **signed**. The fixed thing combines with `+d` one way and `-d`
the other, and any threshold in between turns an antisymmetric error into a
one-sided defect.

The candidate that fits is the one already on the list: the fragment stage
reconstructs position through `M_invprojection`, which is *not* patched per eye
by default, while the vertex stage's projection *is*. That mismatch is
antisymmetric in `d` by construction. It was called refuted because take 70 ran
with `X4VR_PROJ_INVPROJ=1` (236 modules corrected) and the wing did not move —
but nobody has ever checked whether the modules `rp #24` actually executes are
among the 236. Two of the six attachments share a format and the join was never
read for this pass; the same gap that hid `rp #24`'s output image until take 78.

That is what the dump is for. **P87: the fragment modules executing in
`rp #23/#24/#25` are among the ones `patch_fragment_invproj_eye` corrects.** If
they are, invproj is genuinely refuted for this defect and the reconstruction
story dies with it. If they are not, take 70 tested a patch that never touched
the pass where the defect is made, and the refutation was never valid.

## Take 80 — P87 confirmed, invproj properly dead, and an elimination that was void

The dump and its log are a matched pair, and the run reproduced the defect
before anything was read from it: `#57` L 27.99 / R 47.18 = 1.686, `#52` 1.529.

### P87 confirmed

`rp #23` carries 138 fragment modules; `rp #24` and `#25` carry none, because
pipelines are created against `#23` and executed in the compatible passes. Of
those 138, checked against the patch's own eligibility rule — a fragment entry
point, camera vars at (set 1, binding 0) taking *all* aliases, an
`OpAccessChain` naming member 2 with exactly one index, loaded in the entry
function:

    modules executing in rp #23/#24/#25 : 138
      eligible for the invproj patch    : 100
      NOT eligible                      : 38

Take 70 corrected 236 of 244 eligible modules overall, so the patch did reach
this pass — about 100 of its modules — and the wing did not move by 0.02.
**invproj is refuted, and this time the refutation is valid**: the instrument
reached the target and reported nothing.

### The light-volume elimination was scored on the discredited metric

`X4VR_SHEAR_LIGHTS` shears "geometry positioned by the camera rather than by a
per-object matrix — X4's instanced deferred light volumes", and it is **off by
default**. So by default the light volumes are *not* sheared while the geometry
*is*.

That is the shape the defect now demands. Geometry moves by ±disparity between
the eyes; the light volumes stay where the centre camera puts them; so a volume
sits offset one way in the `-x` eye and the other way in the `+x` eye. Where a
volume stops covering a surface, that surface loses that light's contribution
and goes dark. Antisymmetric in `d`, made in the lit output and not in the
G-buffer, and tracking which geometry is lit — every established fact.

It was excluded at take 64, and the exclusion does not hold:

    Against 1.846 / 1.860 / 1.846. **P70 is refuted.**
    level 0.004787/0.008839 (l1/l0 1.847)

`l1/l0` is the whole-frame mean — the metric this document later recorded as
unable to tell correct stereo from broken, and the one the "aggregates hide
local defects" finding was written about. The wing crop that reads 1.686 did
not exist until take 72. A localized change over a few percent of the frame is
invisible to that number by construction, so take 64 measured nothing about
this defect either way.

The knob itself worked, which is worth stating so the re-test is not re-running
a no-op:

    take 80 (off) patched vertex shader #350 [world=296 ui=54 stereo=296 ...]
    take 64 (on)  patched vertex shader #350 [world=312 ui=38 stereo=312 ...]

Sixteen modules move from UI to World when it is set — the camera-positioned
light volumes, reclassified and therefore sheared.

**P88: `X4VR_SHEAR_LIGHTS=1` materially reduces the wing ratio from 1.686.**
Confirm `world=312 ui=38` in the log before reading the wing, and score the
crop and its p90 — never `l1/l0`, which is what voided this question the first
time.

## Take 81 — P88 refuted, and a look at the actual pixels

    X4VR_TAKE=81-SHEARLIGHTS ... X4VR_SHEAR_LIGHTS=1 ...

The knob fired — `[world=312 ui=38 stereo=312 live-sx=300 baked-sx=12]`, the
sixteen camera-positioned light-volume modules reclassified and sheared — and
the view matches take 80 at NCC +0.9978. The wing is **bit-identical**:

    take 80 (off)  #57  L 27.99  R 47.18  1.686   p90 64.7/148.3
    take 81 (on)   #57  L 27.99  R 47.18  1.686   p90 64.7/148.3

**P88 refuted.** Take 64's conclusion was right after all, even though the
measurement it rested on could not have justified it. Both readings are now on
the record: the exclusion was void as an argument and correct as a result.

### invproj has no coverage hole either

Of `rp #23`'s 138 modules, the 38 outside the patch break down as 24 that
declare the camera block and never chain it, 10 that use it but never member 2,
and 4 with no camera block. **None** reaches member 2 through a shape the patch
misses. The patch covers every module that could benefit, so take 70's null is
a real null.

### Correcting the magnitude argument

Take 75's section dismissed the reconstruction error as "3.2 cm, below one
shadow texel". That is true of the shadow *map* and irrelevant to screen-space
alignment:

    at z= 0.83 m: a 3.2 cm lateral error =  36.2 px
    at z= 2.00 m:                           15.0 px
    at z= 5.00 m:                            6.0 px

36 px at 0.83 m is exactly the disparity the residual tool measures for this
scene — an uncorrected `M_invprojection` displaces the lighting by one full
disparity, not by a negligible amount. The dismissal was wrong; invproj is
nonetheless refuted, on the empirical null and now on coverage as well.

### What the pixels show

`tools/stereo_residual.py` on `#57`: 14.4% of judged tiles differ by more than
1.25x *while matching confidently* — same surface, same place, different
brightness. The tail is two-sided, `p1/p50/p99 = 0.661/1.017/4.077`.

Cropping the wing from both layers and stacking them is the first time this
defect has been looked at rather than measured. The structure is identical in
both eyes — every panel line, vent and seam in the same place — and the broad
flat top face of the beam is uniformly darker in layer 0. It is a shading term
missing from a large surface, not a displaced edge.

An attempt to decide multiplicative-versus-additive on that face is **not**
reported here: the patch aligned at NCC +0.3775, which means it was comparing
different surfaces, and the ratio and difference statistics it produced are
worthless. Locking disparity on a distinctive feature before comparing
brightness is a rule this document already records, and it was broken one
paragraph after being relied on. Redo it on a patch that aligns above 0.9.

## Every brightness number on `#57` and `#52` was measured through a tone map

Before any of the above can be re-read, a correction that invalidates the
*units* of most of this chapter. `write_ppm` does not dump the render:

    v = v / (1 + v);                       // Reinhard
    byte = powf(v, 1.0f / 2.2f) * 255.0f;  // gamma

so the 1.686, the 1.530, the p90s, the tile map in `stereo_residual.py` — every
one of them is a ratio of *compressed* values. `tools/stereo_residual.py` reads
the encoded bytes and never inverts this.

For alignment that is harmless: NCC is affine-invariant, so the disparities it
found are still right. For brightness it is not, and for the
multiplicative-versus-additive question it is fatal. Reinhard is not affine. A
constant multiplicative term in the render produces a ratio in the file that
*varies* with brightness, and a constant offset produces one that varies the
other way. Asking "is the ratio constant?" of these bytes measures the tone
curve, not the defect.

Both steps invert wherever nothing clipped:

    t = (byte / 255) ** 2.2        # == v/(1+v)
    linear = t / (1 - t)

but only over a middle band. At byte 250 one code step is 15% of the
reconstructed value and 255 means infinity; at the bottom, byte 1 to 2 is a
factor of 4.5. `tools/shading_model.py` therefore fits only bytes 10..235 and
drops the rest instead of reconstructing noise.

Converting the wing means through the inverse: the eye is not 1.7x dark, it is
roughly **3.2x** dark. Every magnitude claim in this document that was read off
a PPM is compressed by that curve.

### `#57` is mostly black, which is why the guards kept firing

76% of `#57` is byte 0 in both layers, and the median is 0. It is a light
accumulation buffer in a space scene, so that is correct — but it means a
whole-frame statistic is dominated by pixels carrying no information, and it
means the worst blob (`x=1024-1151, y=1088-1151`, ratio 3.5-4.1) has only 27%
of its pixels inside the trustworthy band.

`shading_model.py` reports *why* each tile was rejected rather than a bare
count, because "0 tiles" has been read as a verdict three times in this project
and has never once been one.

### The defect is localised, which kills two families at once

On tiles that align above NCC 0.90 **and** span a factor of 3 in their own
brightness, median `L1/L0` is **1.015**, and the outliers go both ways (0.77 and
1.20). The two eyes agree to about 1.5% on ordinary textured surfaces.

That is a negative result with teeth:

* **A global per-eye exposure difference is dead.** An exposure multiplier
  applies to the whole frame; it cannot leave 57 well-aligned tiles agreeing to
  1.5% while moving others by 4x.
* **A mono screen-space resource is dead**, and separately so on an inventory:
  every storage-usage image in take 80 is a 3D volume (the 88x88x128 froxel
  volumes, 64/128-cube noise) or a cubemap probe. There is **no compute-written
  screen-space 2D image at all**, so the "compute cannot be per-view" gap has
  nothing screen-space to leak through here.

The five images written only by unmasked passes are `#70`-`#74`, all 2048^2
`D16_UNORM` depth-stencil — the cascaded shadow maps. Those are *supposed* to be
mono: both eyes must sample the same light-space map. The 28600 slots "kept at
layer 0 as shared" are exactly those, and are correct.

`#55`, the depth attachment the lighting passes test against, has writers
`masked rp [13,17,19,22,24,23,25,44,50,57] unmasked rp []` — fully per-eye.

### The multiplicative-versus-additive test, redone properly: **additive**

The patch that aligned at NCC 0.3775 is replaced by the worst blob at its own
measured disparity (`dx=-33`, NCC 0.765 including background; the surfaces
themselves align far better). Stacking those crops shows the answer before any
statistic: structure identical to the pixel — strut, grille, panel — and a
broad, soft-edged bright region lying across the strut and tube in layer 1 that
is simply **absent** in layer 0. A soft falloff, not a hard geometric edge.

A smoothness comparison between the ratio and difference fields is **not**
reported: the ratio field is clipped and its spread runs 0.82 to 29, so
normalising each field by its own spread compares incomparable scales. It cannot
answer the question and is discarded, not quoted.

The fair test gives each model **one smooth spatial parameter** — a box-filtered
gain field against a box-filtered offset field, same degrees of freedom — and
compares residuals against `rms(L1)`:

    tube+strut  w=9    multiplicative 32.7%   additive 26.1%   -> ADDITIVE
    tube+strut  w=21   multiplicative 44.2%   additive 38.5%   -> ADDITIVE
    panel       w=9    multiplicative 39.2%   additive 25.3%   -> ADDITIVE
    panel       w=21   multiplicative 57.4%   additive 42.9%   -> ADDITIVE

Consistent on both regions and both scales. **The missing term is additive** — a
light or reflection contribution present in one eye and absent in the other —
not an occlusion or ambient-occlusion factor scaling what is already there.

This overturns the earlier "pure-gain fits better on 42 of 57 tiles". Those 57
tiles are the ones that *pass* the guards, i.e. the ones with median ratio
1.015 — the tiles with no defect in them. That comparison was modelling noise.
Neither model is good (26-43% residual), so the term is not a purely smooth
additive field either; the direction is what is established, not the magnitude.

### invproj, checked a third time — the null is real

Because an additive missing light points straight back at position
reconstruction, `patch_fragment_invproj_eye` was read rather than re-run:

* the derivation is right: `view_centre = P^-1 K^-1 clip = T(d) P^-1 clip`, so
  `T(d)·M_invprojection` is the correct correction;
* the implementation is right. The doc comment writes it as
  `result[0][c] = M[0][c] + d·M[3][c]`, which in SPIR-V's column-major indexing
  would be a *column* combine and wrong — but the emitted code does
  `result[c][0] = M[c][0] + d·M[c][3]` for every column `c`, which is the row
  operation `T(d)·M`. Only the comment's index order is inverted;
* it is genuinely per-view: `d = d_left + float(gl_ViewIndex)·(d_right - d_left)`.

So the 0.04% it moved the wing (46.25 -> 46.27) is a real null and not an
artifact of a patch applied identically to both eyes. invproj stays dead.

## P89 — is a module that lights `#57` being drawn unsheared?

Everything that survives is on the geometry side. The vertex classifier splits
modules into `world` (gets the eye shear) and `ui` (gets `K_ui`, identity by
default), and take 80 reports `world=296 ui=54`. `X4VR_SHEAR_LIGHTS` moves 16 of
those 54 and take 81 was bit-identical — which shows those 16 did not draw in
this frame, **not** that the remaining 38 are innocent.

A module drawing into the light-accumulation passes while classified `ui` is
rasterised at the centre-camera position while the G-buffer it lights is sheared
per eye. Where its geometry then fails to cover a surface, that surface loses
that contribution entirely — an **additive** term, missing on one side, with the
light's own soft falloff at the boundary. That is what the crops show.

**P89: forcing `K_ui = K_world` will materially change `#57`'s per-eye
difference.**

The test needs no new knob. `X4VR_STEREO` writes only `K_world`/`K_world_r`, and
`X4VR_CLIP_K_UI`/`X4VR_CLIP_K_UI_RIGHT` are parsed afterwards, so the UI
matrices are independently settable to the same shear (`m8` is flat index 8):

    X4VR_CLIP_K_UI="1,0,0,0,0,1,0,0,0.42666,0,1,0,0,0,0,1"
    X4VR_CLIP_K_UI_RIGHT="1,0,0,0,0,1,0,0,-0.42666,0,1,0,0,0,0,1"

Scored symmetrically — the ratio between the two layers on the same crops, plus
`stereo_residual.py`'s tail and area — so the verdict survives a polarity flip,
which is the one discipline that saved the earlier eliminations.

**Expect the presented screen to look wrong.** Every UI/HUD module now carries a
world shear. `#57` is written long before any of that, so the measurement is
unaffected, but the frame Patola sees is not the thing being scored.

* **If the blob ratio moves materially** — unsheared geometry in the lighting
  passes is the source, and the next job is to find which modules and fix their
  classification rather than shear everything.
* **If `#57` is bit-identical again** — every module drawing into the lighting
  passes is already sheared, and the whole "unsheared geometry" family dies at
  once: the 38 remaining `ui` modules and the light volumes with them. That
  would leave the additive term with no geometric explanation, and the next
  place to look is the shader source of the modules bound to `rp #23`.

## Take 82 — P89 refuted; unsheared geometry is dead as a family

    X4VR_TAKE=82-KUI X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2
    X4VR_MV_DUMP=/tmp/x4vr-t82 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take82.log X4VR_MV_PROBE=1
    X4VR_MASK_PRESENT=1 X4VR_DUMP_SHADERS=/tmp/x4vr-shaders-take82
    X4VR_MV_DUMP_IMG=57,52 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1
    X4VR_CLIP_K_UI="1,0,0,0,0,1,0,0,0.42666,0,1,0,0,0,0,1"
    X4VR_CLIP_K_UI_RIGHT="1,0,0,0,0,1,0,0,-0.42666,0,1,0,0,0,0,1"
    ./launch/x4vr-launch.sh

The knob fired, and the log says so without ambiguity: take 80 reported
`world=296 ui=54 stereo=296`, take 82 reports `world=296 ui=54 stereo=350`.
296+54 — every UI-classified module now carries a per-view matrix pair.

    take 80 n3   tile ratio p1/p50/p99 = 0.661/1.017/4.077   blob 0.82/1.33/28.99
    take 82 n2   tile ratio p1/p50/p99 = 0.662/1.016/4.078   blob 0.82/1.33/28.91

Different frames (the PPMs differ by hash), same defect to three significant
figures, same disparity (`dx=-33`), same alignment (`NCC 0.7653`).

**P89 refuted.** Every module that draws into the light-accumulation passes was
already sheared. The whole "unsheared geometry" family dies here — the 38
remaining `ui` modules and the light volumes with them — and it dies on a
symmetric measurement, so the verdict survives a polarity flip.

Note for reproduction: `X4VR_DUMP_SHADERS=/tmp/x4vr-shaders-take82` logged
`WARNING: ... is not writable` and wrote nothing, because the layer does not
create the directory. The offline scans below therefore run on take 80's 397
modules, which are the same shaders — neither the game nor the layer changed.

## The leaked-varying hypothesis, raised and killed in one sitting

`patch_vertex_clip` appends `gl_Position = K * gl_Position` before every return.
It shears the value that reaches the rasterizer **and nothing else**. So any
varying the shader computed from the clip position earlier would keep the
centre-camera value while the fragment rasterizes at the sheared position — a
full-disparity disagreement, opposite per eye, with no dependence on the array
layer. That is exactly take 79's signature, so it was worth a scan.

`tools/clip_varying_scan.py` reported **280 of 382 vertex modules leaking**,
into `IO_texshadowCSM0..4`, `IO_world_pos`, `IO_viewz_nr` and `IO_VertexToEye`.
Shadow coordinates leaking the eye offset would have explained everything.

It was wrong, and disassembling one module rather than believing the tool is
what caught it. `mod-0350`:

    %365 = OpCompositeConstruct %v4float ...        ; vec4(object_pos, 1)
           OpStore %366 %365                        ; gl_Position = that
    %370 = OpLoad %v4float %366
    %371 = OpMatrixTimesVector %v4float %736 %370   ; transform
           OpStore %366 %371                        ; the value the raster sees

**X4 uses `gl_Position` as a scratch variable.** Object, world and view position
all pass through it on their way to the varyings, so "depends on a value stored
to gl_Position" means "depends on the vertex position" and flags almost
everything. Pointed at the *final* store — the only value the patch shears — the
same scan reports **0 of 382**. In `mod-0350` the final clip value `%371` is
consumed by exactly one instruction: the store to `gl_Position`.

The 280 is retained in the tool's comments as the positive control: the
dependence walk and the output detection both work, which is why the 0 can be
trusted. And the varyings it named should *not* be sheared anyway — world
position, view z and light-space shadow coordinates are all view-independent by
construction. Only `IO_VertexToEye` is genuinely wrong under the shear approach,
and it is wrong *identically in both eyes*, so it cannot produce an asymmetry.

## Where the search actually stands

Confirmed about the defect itself:

* it is **additive** — a light or reflection present in one eye, absent in the
  other, with a soft falloff at its boundary;
* it is **localised** to particular surfaces; ordinary textured geometry agrees
  between the eyes to 1.5%;
* it is a pure function of the **shear sign**, with no layer dependence;
* it is born in the light-accumulation passes. `#57` is attached in `rp #23`,
  `#24`, `#25`, `#57` (lighting) and `rp #31`/`#32` (fog, excluded at take 63),
  so "born in #57" does narrow to the lighting passes — checked, not assumed.

Eliminated, each on a fact rather than a plausibility argument:

| mechanism | how it died |
|---|---|
| `M_invprojection` reconstruction | empirical null; derivation, column indexing and per-view `d` all re-read and correct |
| light-volume coverage / unsheared geometry | take 82, `K_ui = K_world`, no change |
| mono screen-space compute resource | no compute-written screen-space 2D image exists in the inventory |
| froxel volume sampled by the lighting pass | 0 of `rp #23`'s 138 fragment modules sample a 3D image |
| fog composite | take 63, bit-stable with the term removed |
| global per-eye exposure | the defect is localised; 57 tiles agree to 1.5% |
| bindless mirror / layer-1 descriptors | take 79, negative IPD swapped the result exactly |
| shadow maps as a mono resource | mono by design and correct; both eyes must share a light-space map |
| unsheared clip position leaked into a varying | 0 of 382 modules, with a working positive control |

What has *not* been done: nobody has read the lighting fragment shader. 100 of
`rp #23`'s 138 fragment modules reference `gl_FragCoord`. Task #10 read the
shaders drawing into `#103` and that is what identified the composite; the same
move on `rp #23` is the obvious next one, and it costs no run.

## Reading the lighting shader: `M_invprojection_uj` is the matrix that mattered

Nobody had read the deferred lighting shader. `rp #23` carries 138 fragment
modules, but most write `OUT_RT0..RT3` — they are G-buffer shaders whose
pipelines were merely *created* against `rp #23`. The lighting shaders are the
**8 that declare `SubpassData`**, matching `rp #24.0 writes — colour [1] depth 0
input [2,3,4,5]`:

    mod 199/201/205/219/223/229/257   639-1084 lines   IO_lightcolor, IO_SpecIntensity
    mod 180                           3797 lines       S_sampler2DShadow, S_samplerShadow

The seven small ones are instanced light volumes carrying a per-instance colour.
`mod-0180` is the sun with cascaded shadows, and its fragment stage has **no
varyings at all** — its inputs are `gl_FragCoord` and nothing else, and its
vertex stage indexes `gl_VertexIndex`, so it is a procedurally generated
fullscreen triangle. Everything it knows about a surface it reconstructs.

And it reconstructs position **twice, from the same input**:

    %3607 = gl_FragCoord.xy / camera[11].xy          ; screen UV
    %3621 = OpImageFetch  <depth at camera[58]>      ; I_maindepth
    ...ndc = uv * scale - bias

    %3642 = camera[2]   ; M_invprojection
    %3649 = %3643 * vec4(ndc.xy, depth, 1.0)
    %3655 = %3649.xyz / %3649.w                      ; reconstruction A

    %3727 = camera[4]   ; M_invprojection_uj
    %3734 = %3728 * vec4(ndc.xy, depth, 1.0)
    %3740 = %3734.xyz / %3734.w                      ; reconstruction B

Their consumers are completely different:

    A (%3655) -> %3815 = -A, Normalize, VectorTimesMatrix, Reflect
                 the view vector. Specular and reflection.

    B (%3740) -> %3909 = Length(B)                   cascade selection by range
                 %3926 = vec4(B, 1.0)
                 %3927 = shadow[3] * %3926           cascade 0
                 %3936 = shadow[4] * %3926           cascade 1
                 %3945 = shadow[5] * %3926           cascade 2
                 %3954 = shadow[6] * %3926           cascade 3
                 %3963 = shadow[7] * %3926           cascade 4
                 -> array -> loop -> S_sampler2DShadow

Five matrices, five cascades, and five 2048^2 `D16_UNORM` shadow maps
(`#70`-`#74`) in the inventory. **B is the shadow lookup position.**

`patch_fragment_invproj_eye` is called with `kCameraInvProjMember = 2` and
nothing else. So the patch has been correcting **A**, the view vector, and
leaving **B**, the shadow lookup, reading the centre camera's frame while
`gl_FragCoord` and the depth buffer belong to the sheared eye.

Scanned across all 385 dumped fragment modules on the real camera block
(`set 1, binding 0` — a first attempt queried set 2 and returned a meaningless
240):

    load camera member 2 (M_invprojection)     : 236
    load camera member 4 (M_invprojection_uj)  :   2   -> mod-0179, mod-0180

236 against 2, which is why this never showed up in a coverage count. It also
reproduces the member table already recorded in this document, which is what
gave the corrected scan its credibility.

### This reconciles every surviving fact

* **Additive.** A misplaced shadow *removes* a light contribution from a
  surface. That is a subtraction of a light, i.e. an additive difference — which
  is what the fair equal-degrees-of-freedom fit measured on two separate
  regions at two scales.
* **Soft-edged.** The cascade loop applies a filter kernel; a shadow boundary is
  not a hard geometric edge.
* **Localised.** Only surfaces near a shadow boundary can change. Ordinary lit
  geometry agrees to 1.5%.
* **Follows the shear sign, not the layer.** The reconstruction error is `d`,
  the eye offset, with opposite sign per eye and no reference to which array
  layer the eye is stored in. Take 79's exact result.
* **Immune to `X4VR_PROJ_INVPROJ`.** The knob corrects member 2. It moved the
  wing from 46.25 to 46.27 — it fixed the view vector, which is nearly
  invisible, and never touched the shadow.
* **The magnitude is right.** 3.2 cm at 0.83 m is 36 px, one full disparity —
  the number this document corrected itself on two sections ago.

It also lands exactly where this project was warned it would: globally-applied
shadows are what wrecked the earlier attempt at an X4 VR mod.

## P90 — correcting `M_invprojection_uj` per eye removes the defect

The fix is the correction already derived and already verified, applied to the
member that feeds the shadows. `T(d)·M` is right for member 4 for the same
reason it is right for member 2: both consume the identical
`vec4(ndc.xy, depth, 1.0)`, both divide by `w`, and `Length(B)` being used as a
camera range shows B is camera-relative like A.

The two members are counted and logged **separately**, never as a total: 236 and
0 sums to a healthy-looking 236, and that sum is precisely the broken state this
whole chapter has been chasing.

**P90: with member 4 corrected, `#57`'s per-eye difference collapses.**

* Scored symmetrically as always — the blob ratio and `stereo_residual.py`'s
  tail and area, both of which survive a polarity flip.
* Take 82 baseline to beat: tile ratio `p1/p50/p99 = 0.662/1.016/4.078`,
  14-15% of judged tiles flagged, blob `L1/L0 p10/p50/p90 = 0.82/1.33/28.91`.
* **Confirmation looks like** the p99 falling toward ~1.1 and the flagged
  fraction collapsing toward the IPD=0 negative control's 0.0%.
* **If it does not move**, the log must be read before anything else: a
  `M_invprojection_uj — 0 modules corrected` line means the patch never
  matched, which is a different failure from the mechanism being wrong, and the
  two must not be confused. That is the whole reason the counter is separate.

## Take 83 — P90 CONFIRMED. The defect is fixed.

    X4VR_TAKE=83-UJ X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2
    X4VR_MV_DUMP=/tmp/x4vr-t83 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take83.log X4VR_MV_PROBE=1
    X4VR_MASK_PRESENT=1 X4VR_MV_DUMP_IMG=57,52 X4VR_IPD=0.064
    X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1 X4VR_PROJ_INVPROJ=1
    ./launch/x4vr-launch.sh

The separate counter earned its keep on the first line of the log:

    invproj final: per-eye M_invprojection — 224 modules corrected
    invproj final: per-eye M_invprojection_uj — 2 modules corrected

Two, which is exactly the offline count for `mod-0179` and `mod-0180`.

|          | tile ratio p1/p50/p99 | flagged, confidently matched | blob p10/p50/p90 | crop NCC |
|----------|----------------------|------------------------------|------------------|----------|
| take 80  | 0.661/1.017/**4.077** | 14.4%                       | 0.82/1.33/**28.99** | 0.7653 |
| take 82  | 0.662/1.016/**4.078** | 14.4%                       | 0.82/1.33/**28.91** | 0.7653 |
| take 83  | 0.573/0.993/**1.536** | **1.8%**                    | 0.27/**1.00**/**1.85** | **0.9055** |

The blob's median ratio is **1.00** — the surface that was 1.33 with a p90 of 29
now agrees between the eyes. The whole-frame p99 fell from 4.08 to 1.54, the
confidently-matched flagged area by a factor of 8, and `stereo_residual.py`
flipped to *no meaningful area is lit differently*.

The alignment number is worth its own line: the crop's own NCC rose from
**0.7653 to 0.9055** without anything about the measurement changing. That is
not a brightness statistic — the two eyes now contain the same thing to
correlate, which is the independent check that the fix is real and not a
rescaling.

Patola, on the screen: both frames bright, and the IPD shift visible — stereo.

**Residual.** 1.8% of judged tiles still differ, against the `IPD=0` negative
control's 0.0%. Some of that is genuine: a surface visible to one eye and
occluded from the other is correct stereo, not a defect. It is small enough to
stop here and it is not zero, so it is recorded rather than rounded away.

### Thirteen takes of context, in one line

The knob that was supposed to test this hypothesis existed from take 67. It
corrected member 2 — the view vector — and every run that used it measured a
term nobody can see, then reported a null that was read as "position
reconstruction is not the cause". The mechanism was right the whole time; the
patch reached the wrong matrix. What finally distinguished them was reading the
shader instead of measuring around it.

### `X4VR_PROJ_INVPROJ` now defaults ON

It stopped being an experiment when member 4 joined it. Leaving it off would
make the default build the broken one. It remains gated on `have_k`, so a run
without the shear is unaffected.

**This breaks reproduction of every take before 83 in this document.** Those ran
with the correction off because they *omitted* the variable, and omitting it now
gets the correction. Add `X4VR_PROJ_INVPROJ=0` to reproduce a pre-83 take. This
is the identical trap the take-50 control fell into with `X4VR_STEREO`, already
recorded in this file: omitting a variable and setting it to `0` stopped being
the same thing the moment the default changed.

## Correction to take 82: what `X4VR_CLIP_K_UI` can and cannot reach

Fixing the render-test failures turned up a fact that scopes take 82's
conclusion, and it is the same trap as the invproj knob.

Two independent classifications decide whether `K` reaches a draw:

* the **module** — `classify()` says `world` or `ui`, choosing `K_world` or
  `K_ui`;
* the **pass** — `needs_original()` returns `classify_unsheared()[subpass]`, and
  an unsheared pass binds the **unpatched** module regardless of what the
  patched one contains.

So setting `X4VR_CLIP_K_UI` patches modules that a pass may then decline to use.
In the offline harness this is total: every pass it renders is colour-with-no-
depth, unsheared since take 71, so the log reads

    patched vertex shader #1 (ui)
    unsheared pipeline: using unpatched modules (shadow + UI exclusion active)

and the knob moves nothing at all.

**What this does to take 82.** The lighting passes are
`rp #23/#24/#25.0: 1 colour [97H] depth 126 -> STEREO (world)` — *not*
unsheared, so `needs_original()` is false and the patched modules were bound.
P89's refutation therefore **stands for the passes it was about**: a `ui`
classified module drawing into the light accumulation did receive the world
shear, and `#57` did not move.

**What does not stand** is the sentence "the whole unsheared-geometry family
dies here — the 38 remaining `ui` modules and the light volumes with them."
Modules drawn in passes that are *themselves* unsheared — fullscreen post, UI,
depth-only shadow — use unpatched modules whatever `K_ui` says, so take 82 never
tested them. They are untested, not eliminated.

This does not affect the fix. Take 83 found and corrected the real cause and the
defect is gone. But if a residual per-eye difference is ever chased again,
**unsheared passes are not on the eliminated list**, and this paragraph is why.

The general form is already recorded above and in `x4-quirks.md`: a knob's null
refutes the knob. Here the knob fired, logged a count, and was discarded one
layer further down than the counter could see — a *third* instrument in this
project blind to its own target.

## Take 84 — the rename and the default flip are behaviour-neutral

Take 83's command line with `X4VR_PROJ_INVPROJ` **omitted**, since it now
defaults on, and no `CLIP_K` of any spelling — pure defaults on a build that
had since had a knob default flipped and three knobs renamed.

    invproj final: per-eye M_invprojection — 224 modules corrected
    invproj final: per-eye M_invprojection_uj — 2 modules corrected

    take 83   dx=-35  ncc=0.9055  blob 0.27/1.00/1.85  frame 0.573/0.993/1.536  1.8%
    take 84   dx=-35  ncc=0.9055  blob 0.27/1.00/1.85  frame 0.573/0.993/1.536  1.8%

Identical on every figure. The refactors are measured neutral, not assumed
neutral, and `stage2-stereo-shading-correct` reproduces from defaults alone.

### Take 85 — the first time the mod was played rather than screenshotted

Patola flew, walked a station, used the map, and changed cameras. No new
defects. Everything the fix was supposed to hold held under motion and scene
changes, which nothing before take 85 had tested — every prior take was one
static camera on one savegame.

One observation to resolve: **the external and cinematic cameras look mono.**
Almost certainly distance rather than a defect. Per-eye offset from centre is
`704 · sx · (ipd/2) / z` px, which at `ipd=0.064 sx=1.3333` on 1408px is

    ~30/z px      1 m -> 30 px    5 m -> 6 px    10 m -> 3 px
                 30 m ->  1 px   100 m -> 0.3 px

X4's external camera sits tens of metres out and the cinematic camera further,
so the shift is sub-pixel to about one pixel — indistinguishable from mono by
eye, and physically correct: human stereopsis is useful to roughly 10 m and
gone by 30 m.

That is arithmetic, not a measurement. The test is one run at an absurd IPD —
`X4VR_IPD=5.0` turns a 0.5 px shift at 60 m into ~39 px. Parallax appearing
means the path is stereo and this is pure scale, and the question moves to
comfort tuning (task #25). Parallax still absent means those cameras bypass the
shear, which is a real gap and a new task.

## Immersive UI mode as a goal (task #30)

Recorded here because it constrains work that is otherwise tempting to do
casually. Menu-heavy modes may stay a mono projection in front of the viewer
for now; the aim is that some of them eventually render the menu/HUD onto a
**separate transparent floating canvas** at a chosen virtual distance, with the
3D scene behind it in true stereo, so the player can move their head.

The part worth writing down now, because it is counter-intuitive:

**The UI is mono by construction, and that is exactly why it currently looks
right.** UI/HUD passes classify as unsheared, so `needs_original()` binds the
*unpatched* module and both array layers receive identical pixels. Nothing
offsets them, and nothing is meant to.

Putting the canvas at a finite distance means giving the UI a **constant**
per-eye horizontal offset — unlike world geometry, whose offset scales as
`30/z`. A constant screen shift *is* a fixed virtual depth.
`X4VR_CLIP_SHIFT_NONWORLD` is the right shape for that knob and is inert today
for the same reason `K_nonworld` is: an unsheared pass binds the unpatched
module. So this needs a **third category** — not "world" (offset scales with
depth) and not "excluded" (no offset) but "constant offset". That is a change to
the predicate, not to a matrix value, and the predicate is the thing this
document has had to correct most often.

The blocker is input, not rendering. X4 hit-tests the cursor CPU-side in window
coordinates (tasks #19, #21), so a canvas at a virtual depth needs the cursor
projected onto it — which is what task #19 exists to make possible. A
*non-interactive* floating HUD could be demonstrated much sooner than an
interactive one, and that is the honest split to plan around.

## Take 86 — the distant cameras are stereo, and zoom is clean

`X4VR_IPD=5.0`, everything else as take 85. Patola: parallax is visible in both
the external and the cinematic camera, and zoom behaves normally.

**The mono appearance at default IPD was distance, as predicted.** ~30/z px per
eye puts a 60 m third-person view at about half a pixel, which is
indistinguishable from mono and physically correct — human stereopsis is gone by
about 30 m. Nothing to fix; if a third-person view ever needs to *feel* deep,
that is hyperstereo as a comfort choice (task #25), not a defect.

Zoom exercised without visible error. Task #23's twelve baked-`sx` modules stay
open on the structural argument — they are still only correct at the default
FOV — but nothing conspicuous draws through them at the FOVs tried.

## Starting the cursor shim (#17): two findings that reorder it

### `SDL_PollEvent` is interposed and never fires

The injector has interposed `SDL_PollEvent` since take 40 to answer P42/P43 —
what coordinate space X4 is handed, and whether motion is absolute or relative.
Across takes 84, 85 and 86, including take 85's map, menu and station use:

    $ grep 'sdl: mouse' /tmp/x4vr-take8{4,5,6}.log
    (nothing)

Not a sampling cap — the counters allow 8 motions and 6 buttons and logged
zero. And interposition itself works in that same file: `SDL_GetWindowSize` is
intercepted and logged twice in the same run.

So **X4 does not read mouse input through `SDL_PollEvent`**, or the
`this_is_the_game()` gate is false in whichever process polls. Either way
**P42 and P43 are unanswered**, and they are the foundation of task #19: a shim
cannot own an event stream it has not found. The candidates are
`SDL_PumpEvents`/`SDL_PeepEvents`, `SDL_WaitEvent(Timeout)`, a statically linked
SDL whose internal calls no `LD_PRELOAD` can reach, or X4 reading evdev or the
Wayland protocol directly. That is a measurement, not a guess to be made now.

### #17's premise may already be satisfied by X4

Task #17 is "draw the cursor into the eye image before duplication, so it lands
in both halves at the matching place". But `x4-quirks.md` already records, from
the `--force-grab-cursor` work, that **X4 draws and moves its own cursor** in the
menus and the map — which is why forcing relative mode did not break them.

Anything X4 draws into its own frame is duplicated into both halves by the
compositor, correctly, for free. If that is what happens today, the drawing half
of #17 is already done and what remains is only the modes where the visible
pointer is *gamescope's*, which lives in display space and is not duplicated.

### Why this cannot be settled from here: task #29

The probe emits after `vkCmdEndRenderPass` for masked passes, so the eye image
is captured after the **first** present pass — and P76 established there are
several (`rp #0/#1/#4/#7/#10`). The UI, and therefore any cursor, is drawn
after that. `X4VR_MV_DUMP_IMG` cannot show the finished frame at all, which is
exactly what task #29 says.

So the order is: **#29 first** (dump the eye image at present, when the frame is
final), because it is the instrument #17, #21 and #30 all need, and without it
any cursor work is written blind against a premise that may not hold.

## The cursor moved, and P23's model no longer predicts it

Patola, reviewing the cursor deliberately in a repeat of take 85: it is still
bounded on the left at **1/4** of the display, but now runs **past the right
edge** — his estimate, 5/4. Previously it was bounded 1/4 to 3/4, which is
P23's measured box `704…2112`.

What the log settles:

    instance created (app=gamescope) — not the game, layer inert   pid 2877919
      swapchain created: 2816x1408 ... (pid 2877919)
    instance created (app=X4)                                      pid 2877964
      swapchain created: 1408x1408 ... (pid 2877964)
      sdl: SDL_GetWindowSize -> 1408x1408 (pid 2877964)

**X4 still believes its window is 1408x1408**, exactly as when P23 was measured.
So P23's model — `x_x4 = x_screen - 704`, the box being X4's window width
positioned by gamescope — predicts `704…2112` and is contradicted by the
observation. The model is incomplete, not merely mis-parameterised.

*(Process note: that log holds two sessions, because the run was repeated into
the same `X4VR_LOG`. Both agree, so nothing here rests on it, but
`tools/score_run.py` refuses multi-session logs and the fresh-log-per-run rule
exists for exactly this.)*

### P91 — two models fit both bounds, and only an off-centre element separates them

| model | mechanism | box | element at `x_x4` activates at pointer |
|---|---|---|---|
| **B** | translation, X4's *input* extent is now 2816 while its window reports 1408 | 704…3520 | `x_x4 + 704` |
| **C** | a scale of 2 has been reintroduced somewhere | 704…3520 | `2·x_x4 + 704` |

Both put the left stop at 704 and the right stop at 3520. **Both fit every
number observed so far**, which is the same shape as the take-30 census and the
false "P22 confirmed": an observation consistent with a hypothesis is not an
observation that selects it. The endpoints cannot decide this; only the middle
can.

**P91: for a UI element X4 draws at its own `x_x4 = 352`, the pointer must be at
screen 1056 under model B and at screen 1408 under model C.** One annotated
capture decides it, which is exactly how P23 was resolved — an off-centre
element marked in both copies, with the cursor placed where it actually
highlights that element.

Recorded before the measurement, and before any shim design rests on it. Note
that neither model is yet known to be *the* mechanism; they are the two the
current evidence admits, and a third may survive the capture that these two do
not.

### P91 resolved: model B. The mapping never changed — the confinement did.

Annotated capture, map mode: a station marked with a red circle in **both**
copies, the cursor marked purple, cursor placed where it highlights that
station. The two red circles fix the scale, being one eye-width apart by
construction.

| feature | image x | display x |
|---|---|---|
| station, left copy | 178 | 251 |
| station, right copy | 1177 | 1657 |
| separation | 999 | **1407** (= 1408 to reading error) |
| cursor, arrow tip | 683 | **962** |

The station is drawn at `x_x4 = 251`. Predictions:

| model | pointer | error |
|---|---|---|
| **B — translation `x_x4 + 704`** | 955 | **7 px** |
| C — scale `2·x_x4 + 704` | 1205 | 243 px |

**Model B.** `x_x4 = x_screen - 704`, exactly P23's mapping, re-measured on a
different element four takes later.

So the earlier framing was wrong in an instructive way: **the mapping did not
change.** What changed is the **right-hand confinement**. P23's box was
`704…2112`, bounded by X4's 1408-wide window; the pointer now runs past 2112,
off the display, consistent with an input extent of 2816 (`704…3520`) while
`SDL_GetWindowSize` still reports 1408. The offset is untouched; only the clamp
moved.

### Correction: X4 does not draw its own cursor

`x4-quirks.md` records, from the `--force-grab-cursor` work, that "X4 draws and
moves its own cursor". The capture refutes it. Patola sees **one** pointer, not
duplicated — and anything X4 drew into its own frame would appear in both copies,
because the compositor duplicates that frame.

What is true is narrower: **X4 owns the cursor's shape and position; the
compositor draws it, once, in display space.** The shape changed from reticle to
arrow on hovering the station, so X4 is setting it; the single instance shows it
is not in X4's frame.

That was load-bearing in the wrong direction. It was the reason to suspect
task #17's drawing half might already be done. It is not: a display-space
pointer cannot be correct in a side-by-side stereo image, so **#17 has real
work**, and its original statement — draw the cursor into the eye image *before*
duplication — was right all along.

### The ergonomic defect, stated precisely

X4's whole frame is reachable: `x_x4 ∈ 0…1408` maps to pointer `704…2112`,
which is on screen. Nothing is unclickable. The defect is that **you must point
where the element is not**:

```
display   0 ────────────── 1408 ────────────── 2816
copy A    |═══════════════|                          station drawn at 251
copy B                    |═══════════════|          and again at 1659
pointer               |═══════════════|              must sit at 955
```

The pointer box straddles the seam, so hovering the station you can see at 251
means putting the pointer at 955 — over the middle of copy A, nowhere near it.
Past 2112 the pointer is at `x_x4 > 1408`, outside X4's frame entirely: dead
space that the widened clamp now lets it wander into, and off the display.

Both halves of the shim follow from this and remain separate:

* **Draw** (#17) — composite the pointer into the eye image before duplication,
  so it lands at the matching place in both copies. Needs a cursor position,
  which X4 has and we currently do not.
* **Map** (#19) — make a display coordinate reach X4 as the point in one eye
  that is *drawn* there, so hovering an element where it appears activates it.
  Still blocked on locating the event stream: `SDL_PollEvent` is interposed and
  never fires.

## The input hook was on a symbol X4 never imports

`ldd` and `nm` settle in one command what three takes of silence did not:

    $ ldd "X4 Foundations/X4" | grep SDL
        libSDL3_ttf.so.0 => /usr/lib/libSDL3_ttf.so.0
        libSDL3.so.0     => /usr/lib/libSDL3.so.0
    $ nm -D --undefined-only X4 | grep -c SDL_        # imported
    82
    $ nm -D --defined-only  X4 | grep -c SDL_         # statically linked in
    0
    $ nm -D --undefined-only X4 | grep -c SDL_PollEvent
    0

SDL3 is linked **dynamically**, so `LD_PRELOAD` interposition works — and always
did, which `SDL_GetWindowSize` proved every run. **`SDL_PollEvent` is simply not
one of the 82 symbols X4 imports.** The hook could never fire. P42 and P43 were
unanswerable from take 40 onward, and the silence was read as "no events seen"
rather than "no such call".

That is the **fourth** instrument in this project blind to its own target, and
the cheapest to have caught: one `nm` on a binary that has been sitting on disk
the whole time.

### What X4 actually imports, and what each one means for the shim

    event loop      SDL_WaitEvent  SDL_PeepEvents  SDL_PumpEvents  SDL_PushEvent
    pointer state   SDL_GetMouseState  SDL_WarpMouseInWindow
    pointer mode    SDL_SetWindowRelativeMouseMode  SDL_SetWindowMouseGrab
    pointer image   SDL_CreateColorCursor  SDL_SetCursor
    window          SDL_GetWindowSize  SDL_SetWindowSize  SDL_CreateWindow

Three of these change the design:

* **`SDL_GetMouseState`** — X4 *polls* the pointer, it does not only read motion
  events. Whatever space this returns is the space X4 hit-tests in, so this is
  where task #19 acts. Rewriting one return value is a far smaller change than
  owning an event stream, and it cannot desynchronise from the events because it
  *is* the position X4 uses.
* **`SDL_CreateColorCursor` + `SDL_SetCursor`** — X4 builds a cursor **bitmap**
  and hands it to SDL, which is why the compositor draws one pointer in display
  space and why it changes shape over a station. It also means task #17 can
  composite **X4's own cursor image**, captured from that call, rather than
  inventing one that would not match the game's.
* **`SDL_WarpMouseInWindow`** — X4 recentres the pointer for mouse-look, so any
  shim that rewrites coordinates has to stay consistent with warps it did not
  issue.

The dead `SDL_PollEvent` hook is removed and replaced by `SDL_WaitEvent`,
`SDL_PeepEvents` and `SDL_GetMouseState`, still observation only.
`SDL_GetMouseState` samples on **change** rather than call count, because X4
polls it every frame and a plain counter would spend its whole budget on one
stationary position and report a range of zero — the same shape of mistake as
the counter that sat below the gate it was measuring.

- **P92** — `SDL_GetMouseState` returns coordinates in X4's 1408-wide window
  space (`0…1408`), matching `x_x4 = x_screen - 704`. If instead it returns
  `0…2816`, X4 is being handed display coordinates and clamping later, and the
  widened right-hand box is explained by that rather than by a changed extent.
- **P93** — motion events arrive through `SDL_WaitEvent` or `SDL_PeepEvents`
  and carry non-zero `xrel`/`yrel`. If neither fires either, X4 is not reading
  the mouse through SDL at all and the search moves to evdev or Wayland.

## Take 88 — P93 confirmed, P92 supported but not proven, and the sampler was wrong

The hooks fire. Motion arrives through `SDL_WaitEvent`/`SDL_PeepEvents`:

    sdl: mouse motion x=704.0 y=704.0 xrel=0.0    yrel=0.0     win=4
    sdl: mouse motion x=0.0   y=0.0   xrel=-704.0 yrel=-704.0  win=4
    sdl: mouse motion x=3.0   y=4.0   xrel=3.0    yrel=4.0     win=4
    sdl: mouse motion x=13.0  y=12.0  xrel=9.0    yrel=8.0     win=4

**P93 confirmed.** And the shape of the numbers says more than the fact that
they exist:

* `x` is an **absolute window coordinate** — it accumulates while `xrel` carries
  the delta, so this is not a relative-only stream;
* it starts at **704**, which is the centre of a 1408-wide window;
* the second event is a warp to the origin (`xrel=-704`), consistent with
  `SDL_WarpMouseInWindow`, which X4 imports;
* `SDL_GetMouseState` returns the same values at the same timestamps, so the
  polled channel and the event channel agree. That matters: the shim only has to
  be consistent with one position, not reconcile two.

**P92 is supported and not proven.** Window space is what these look like, but
the largest value observed is 17. Nothing here reaches an edge, so 0…1408 and
0…2816 are both still consistent with the data — the same "two models, one
untested region" shape as P91, and it must not be written up as confirmed.

### The sampler measured the wrong thing

Every sample lands in a 0.2-second burst 97 seconds into a 123-second run. Both
budgets — twelve changed positions, eight motion events — were spent on the
first mouse twitch, long before the deliberate sweep to the screen edge.

The question was "what is the **range**", and the instrument sampled a
*beginning*. Replaced with `note_extent()`, which logs only on a **new
extreme**: self-limiting, because a range only ever widens, and it captures the
edges, which are the only part that discriminates 0…1408 from 0…2816.

It is called **above** the sample caps in `note_mouse_event`, deliberately.
Below them, the eight-motion budget would switch off the instrument measuring
the range — the same shape as the bindless counter that sat under the gate
removing its own population, and the direct cause of take 88 reporting a maximum
of 17. That bug was written and then caught before the run, not after it.

- **P92 (restated for take 89)** — with the pointer swept to both extremes,
  `GetMouseState extent` reports `x=0..1408`. If it reports `x=0..2816`, X4 is
  handed display coordinates and clamps them itself, and the widened right-hand
  box follows from that rather than from a changed window extent.

## Take 89 — P92 confirmed. X4's input space is its own window, and the fold is back.

Pointer swept hard into all four edges:

    sdl: GetMouseState extent x=0..1407 y=0..1406
    sdl: motion        extent x=0..1407 y=0..1368

`0…1407` is X4's 1408×1408 window space exactly, on both channels. **X4 is handed
window coordinates, not display coordinates**, and hit-tests in them.

### That refutes the second half of model B, and reassigns the widened box

P91's model B was two claims: a translation of 704 (**confirmed**, twice, at P23
and P91) *and* an input extent that had grown to 2816 (**refuted here**). X4's
pointer cannot leave `0…1407`, so its input box in display space is `704…2111` —
the same box P23 measured, unchanged.

So the pointer Patola sees running past the right edge and vanishing is **not
X4's pointer**. It is gamescope's, drawn in display space, and its range says
nothing about what X4 can be made to hit. The two had been conflated because
only one of them is visible.

That also closes the loop on the earlier observations: one pointer, not
duplicated, because the compositor draws it; shape changing over a station,
because X4 calls `SDL_SetCursor`; left stop at 1/4, because gamescope confines
the pointer to X4's 1408-wide surface, which starts at display 704.

### The fold, retracted at take 30, is now correct — for a different reason

The old "fold" (`x_x4 = x_screen mod 1408`) was retracted because it rested on
the scale model, which P23 killed. It returns on solid ground, because P92
establishes the two facts it actually needs: X4's frame **is** exactly one copy,
and the pointer is confined to `704…2111`, i.e. `x_sdl ∈ 0…1407`.

The transform the shim needs is therefore one line:

    x_x4 = (x_sdl + 704) mod 1408

Check it against the P91 capture. The station is drawn at `x_x4 = 251`, so its
copies are at display 251 and 1659.

| pointer | `x_sdl` | folded `x_x4` | hits |
|---|---|---|---|
| 1659 (over the station in copy B) | 955 | `1659 mod 1408` = **251** | the station ✓ |
| 955 (where it must point *today*) | 251 | 955 | the wrong element |

And the coverage is complete rather than merely better. The reachable pointer
range `704…2111` gives `x_sdl ∈ 0…1407`, and folding that yields
`704…1407 ∪ 0…703` — **X4's whole frame**. Every element is reachable, each by
pointing at wherever one of its two copies is drawn: copy A's right half and
copy B's left half between them cover the frame exactly once.

That is the entire ergonomic fix, and it needs no window resize, no render
change and no event-stream ownership — only a rewrite of the position X4 reads,
in the one place both channels agree on.

- **P94** — rewriting the pointer position with the fold makes hovering an
  element activate it *where it is drawn*, in whichever copy the pointer is
  over, with no change to rendering. Failure modes to watch, each of which
  would show up as a different symptom: `SDL_WarpMouseInWindow` (X4 recentres
  for mouse-look, and a warp the shim did not issue must not be folded twice);
  relative mode in the cockpit, where there is no pointer to fold at all; and
  drag operations, which must fold consistently across press, move and release
  or a drag will jump at the seam.

## Task #19 implemented: the input fold

`X4VR_INPUT_FOLD=1`, **off by default** — it changes where every click lands, so
it gets proven against a run with it off before it becomes the default.

    x_x4 = (x_sdl + W/2) mod W          W = X4's own window width

`W` is read from the `SDL_GetWindowSize` calls X4 makes, gated on the caller
being the game, rather than hardcoded — gamescope asks the same question about
its own 2816-wide surface.

Applied in three places, which is the whole of it:

* `SDL_GetMouseState` — the polled position, which take 88 showed X4 reads every
  frame and which agrees with the event stream;
* the motion and button events returned by `SDL_WaitEvent` and `SDL_PeepEvents`
  — position only, never `xrel`/`yrel`, which are deltas and are what X4 steers
  the camera by;
* `SDL_WarpMouseInWindow`, routed through the *same* function.

Three traps, each of which would have made it fail quietly:

1. **`SDL_PeepEvents` peeks as well as gets.** A peek leaves the event in the
   queue, so folding on peek and again on the eventual fetch applies the
   transform twice — and because the fold is its own inverse, that lands exactly
   back on the unfolded value. The fix would have silently done nothing. Folding
   is restricted to `SDL_GETEVENT`.
2. **The warp needs no separate inverse.** The fold is an involution, so X4 asks
   for a logical position, the real pointer goes where it folds back from, and
   the value X4 reads next is what it asked for. A separately-derived inverse
   would be a second expression to keep in step with this one.
3. **Relative mode.** X4 switches the pointer to relative for mouse-look and
   steers by `xrel`, so `SDL_SetWindowRelativeMouseMode` is tracked and the fold
   stands down while it is on.

Arithmetic checked offline before spending a run, against the P91 capture:

    involution over 0..1407  : OK
    coverage of 0..1407      : COMPLETE (1408 distinct)
    pointer at display 1659 -> x_sdl 955 -> folds to 251   <- the station
    pointer at display  955 -> x_sdl 251 -> folds to 955   <- today's wrong spot

Complete coverage is the part worth stating: the fold is a bijection on
`0…1407`, so every element in X4's frame stays reachable. It moves *where* you
point, it does not trade one unreachable region for another.

## Take 90 — P94 confirmed for picking, and the fold breaks steering

`X4VR_INPUT_FOLD=1`, confirmed engaged: `sdl: input fold ON — x_x4 = (x_sdl +
704) mod 1408`. One session. Extent `x=0..1407`, unchanged.

Since gamescope hands X4 window coordinates, `x_sdl = d - 704`, and the fold
collapses to

    x_x4 = d mod 1408          d = display x

Every observation follows from that one line.

**Picking works — P94 confirmed.** Patola: clicks between 1/4 and 3/4 select the
correct element in both copies, even across the seam. Pointing at display `d`
selects what X4 drew at `d mod 1408`, which is exactly the element visible
there, in whichever copy the pointer is over.

**The line to the cursor is correctly placed**, for the same reason: X4 draws it
to `x_x4 = d mod 1408`, duplicated at that x and at `x + 1408`, so one of the two
lands precisely under the pointer.

**Steering inverts at the seam.** X4 steers from the cursor's offset relative to
its *frame centre*, `x_x4 = 704`. Under the fold that centre is reached at
`d = 704` and `d = 2112` — the two **edges** of the reachable range — while the
seam `d = 1408` maps to `x_x4 = 0`/`1408`, the frame **edges**. So sweeping left
to right takes `x_x4` from centre → right edge → *wrap* → left edge → centre.
Hence a few pixels left of the seam steers hard right and a few pixels right of
it rotates left, exactly as reported.

**Past 3/4 the steering stops following** while the pointer keeps moving: beyond
`d = 2112` the pointer leaves X4's surface, SDL clamps `x_sdl` at 1407, and
`x_x4` freezes. What continues moving is gamescope's own pointer, which is not
X4's and never was.

### These two requirements are incompatible, and no formula reconciles them

* **Picking** wants *which element is under the pointer* — `d mod 1408`, which
  must jump at the seam, because what is visible there jumps.
* **Steering** wants a *continuous signed offset from the frame centre*, which
  requires no jump anywhere in the reachable range.

The pointer's box, `704…2112`, straddles the seam: half of copy A and half of
copy B. Any mapping satisfying "point at what you see" is discontinuous there,
so this is structural and not a matter of a better transform.

Which is the conclusion this document already reached at take 30 and then lost:
*"three extents have to agree — X4's window, its render and the composite — and
only two of them are currently chosen together."* The fold is the best available
answer while they disagree, not a substitute for making them agree.

**The real fix is to align the pointer's box with one copy.** With the box at
`0…1408` instead of `704…2112`, `x_x4 = d` is *both* continuous and
point-at-what-you-see, and the seam stops being a special place. That is a
window/composite change, recorded as its own task rather than bolted onto this
one.

`X4VR_INPUT_FOLD` **stays off by default**: a genuine improvement for map and
menu work, a genuine regression for cockpit steering, and the A/B is exactly why
it was not defaulted on before being run.

Worth restating, because it bounds how much this deserves: **this is a
flatscreen bring-up ergonomic.** In an HMD there is no 2D pointer over a
side-by-side image at all, so none of this is the eventual VR input path. It is
worth having because the whole project is driven from this view, and worth not
over-fitting to — a warning this document issued at take 30 and should keep.

## P95 — is the pointer confined to X4's surface, or merely unheard outside it?

Task #31 rests on a claim this document has asserted twice and never tested:
that gamescope confines the pointer to X4's 1408-wide surface, and that the
`704…2112` box is that confinement. It is consistent with the left stop at 1/4,
but so is a completely different mechanism, and the two lead to different fixes.

| | what happens at the edge | what #31 becomes |
|---|---|---|
| **confined** | something pins the pointer to the surface | defeat or relocate a confinement |
| **unheard** | the pointer leaves freely; Wayland stops delivering events to a client the pointer is no longer over, so `x` freezes because nothing updates it | move the surface; there is nothing to defeat |

They are distinguishable at the wall:

* **confined** — pushing past the edge keeps delivering motion events with
  non-zero `xrel` while `x` sits at 0 or 1407. You are pushing against something.
* **unheard** — motion events stop entirely for as long as the pointer is
  outside, and resume when it returns.

`note_mouse_event` now logs the first eight *wall pushes* — a motion event whose
`x` is at either wall while `xrel` is non-zero — on raw SDL values, before the
fold is applied. It also logs once when motion arrives away from the walls, so
that an absence of wall pushes is evidence rather than an untested probe: this
project has read four silent instruments as measurements and will not read a
fifth.

**P95: pushing the pointer hard into the left and right edges produces `wall
push` lines.** If they appear, the pointer is confined and #31 must relocate or
defeat that confinement. If none appear while the "away from the walls" line
does, the pointer is free and simply unheard off-surface, and #31 is purely a
question of where X4's surface sits — which is a considerably easier fix, and
would also explain why gamescope's own pointer sails off to the right while X4's
freezes.

## Take 91 — the left wall confines; the right wall was never measured

    sdl: motion away from the walls (x=704) — the probe is live
    sdl: wall push #1 — x=0 pinned, xrel=-704.0        <- the startup warp, not a push
    sdl: wall push #2..#8 — x=0 pinned, xrel=-6, -4, -16, -7, -6, -2
    sdl: GetMouseState extent x=0..1407 y=0..1406 (now 1407,654)

**The left wall confines.** Seven genuine pushes at `x=0` with motion still
arriving and non-zero `xrel` while the position stays pinned. Events do not
cease, so something clamps rather than delivery stopping.

**The right wall is unmeasured, and the silence is my instrument's fault.** The
eight-push budget was spent at `588783.8`; the extent line showing the pointer
at 1407 is stamped `588785.7`. The right wall was reached *after* the counter was
full, so its zero is a budget artifact.

That is the same fixed-budget failure as take 88's sampler — written into the
probe built two messages after diagnosing it, and shipped despite the docs for
that very probe arguing that an absence had to be made meaningful. The guard was
against the probe being **dead**; the failure was the probe being **spent**. A
budget shared between two things being compared cannot compare them.

Fixed: per-wall counters, six each, so neither wall can consume the other's
evidence.

### What the left wall already tells us

Continued relative motion with a clamped position is the signature of
`--force-grab-cursor` putting the pointer in **relative mode**: the compositor
sends deltas unconditionally and SDL accumulates them into a position it clamps
to the window. That mechanism is symmetric by construction — it knows nothing
about which edge — and it involves no surface-boundary confinement at all.

If that is what is happening, it also resolves the asymmetry Patola sees on
screen without needing an asymmetric mechanism: X4's position is clamped at both
ends, while **gamescope's own pointer is a separate, unclamped thing** that
sails off to the right. Two pointers, one bounded and one not, conflated because
only one of them is visible.

- **P96** — the right wall produces `wall push RIGHT` lines just as the left
  does. Symmetric confinement means the bound is SDL clamping an accumulated
  relative position, not a surface boundary, and task #31 is then purely about
  where X4's surface sits — nothing to defeat. Asymmetry would mean a genuine
  surface-boundary effect on one side and would need explaining before #31 is
  designed.

## Take 92 — P96 confirmed. There is no surface confinement, and #17 subsumes #31.

Per-wall budgets, right edge pushed first:

    wall push RIGHT #1 — x=1407 pinned, xrel=+6   still arriving
    wall push RIGHT #2 — x=1407 pinned, xrel=+47
    wall push RIGHT #3 — x=1407 pinned, xrel=+23
    wall push LEFT  #2 — x=0    pinned, xrel=-58
    wall push LEFT  #3..#6 — x=0 pinned, xrel=-1, -33, -15, -10

**Symmetric.** Take 91's one-sided result was entirely the shared budget, as
suspected, and the asymmetry needed no explanation because it never existed.

### The mechanism, and what it is not

Motion continues at both walls while the position stays pinned. That is
`--force-grab-cursor` putting the pointer in **relative mode**: the compositor
sends deltas unconditionally, and SDL accumulates them into a position it clamps
to X4's window. There is **no surface-boundary confinement anywhere**, and
nothing for a shim to defeat.

Three things follow, and the third is the useful one:

1. The `0…1407` bound is SDL's clamp against X4's window, not a compositor
   boundary. It is also exactly one copy's worth of X4's frame.
2. X4's pointer position is therefore a **fiction SDL maintains from deltas** —
   there is no privileged desktop location it corresponds to. gamescope's
   visible pointer is a *second, independent* fiction, which is why it sails off
   to the right while X4's freezes. Two pointers, one clamped, conflated
   throughout because only one of them is drawn.
3. **Task #31 is not needed to fix pointing. Task #17 already does it.**

### Why drawing the cursor removes the problem instead of working around it

X4's pointer lives in `0…1407`, exactly one copy. Draw a cursor into the **eye
image** at `x_x4`, and the compositor duplicates it to display `x_x4` and
`x_x4 + 1408` — the same two places it duplicates everything else.

Now the cursor and the element are the *same kind of object*: both drawn by the
frame, both duplicated identically. The cursor sprite overlaps the element
sprite exactly when `x_x4 == x_element`, which is exactly when X4 hit-tests it.

    point at what you see   -> automatic, because both are in X4's frame
    continuity              -> total, no modulus anywhere
    steering                -> unbroken, the frame centre is one place again
    the fold                -> unnecessary; stays off
    extent alignment (#31)  -> not required for this

The seam stops mattering because nothing is being mapped across it. The two
visible cursors are not a defect: in stereo that is what one cursor looks like,
the same as every other object in the frame.

This is a better outcome than #31's surface-moving, and it was reachable only
after P92 (X4's space is one copy), P94/take 90 (the fold's discontinuity), and
P96 (no confinement to defeat). Each measurement narrowed it.

**#31 is not closed** — the three extents still disagree, and that is still the
reason a display-space pointer cannot be right. It is demoted: no longer the
prerequisite for pointing, and worth revisiting only if #17 turns out not to
cover a case.

### What #17 now needs, concretely

* **Position** — the injector has it (`SDL_GetMouseState`, which take 88 showed
  agrees with the event stream). The layer needs it. DESIGN.md's shared-memory
  struct between injector and layer is the intended channel and is unwritten.
* **The bitmap** — X4 builds its own cursor via `SDL_CreateColorCursor` and
  hands it over with `SDL_SetCursor`, so the shim can composite **X4's own
  cursor image**, including the reticle-versus-arrow change, rather than
  inventing one that would not match.
* **Suppressing gamescope's pointer**, or there will be two.
* **Seeing the result** — task #29. The probe captures the eye image after the
  first present pass, and the cursor is drawn after that, so today a correct
  implementation and a broken one would look identical from here.

## Task #29 implemented: the finished eye image

`X4VR_MV_DUMP_PRESENT=N` writes the eye image every N presents, to
`<X4VR_MV_DUMP>-present-n<seq>-layer{0,1}.ppm`.

**Why the existing probe could not do this.** `X4VR_MV_DUMP_IMG` fires from
`vkCmdEndRenderPass`, after the *first* pass that writes a named image. The eye
image is written by several present passes — P76 named `rp #0/#1/#4/#7/#10` —
so what it captured was the frame *before* the UI. The cursor, the HUD and the
clipped logo are all drawn after that, which is why every question about them
has been unanswerable from a dump and had to be asked of Patola's screen.

At `vkQueuePresentKHR` the frame is complete: X4 has submitted everything and is
asking for it to be shown.

**Where the copy is recorded, and why not somewhere simpler.** In
`SbsCompositor::composite()`, immediately after its `CmdCopyImage`. The eye image
sits in `PRESENT_SRC_KHR` because X4 believes it is the swapchain, and composite
is the one place that already transitions it to `TRANSFER_SRC_OPTIMAL` with a
range covering every layer. A private command buffer would have had to reproduce
that transition and would be a second thing to keep correct as these layouts
change — and this file has a record of exactly that kind of duplicate drifting
out of step.

The request is one-shot: `request_dump()` sets a buffer, `composite()` consumes
and clears it under the same mutex, so asking costs one frame and not asking
costs nothing. The readback happens after the present is chained down, so the
`QueueWaitIdle` stalls a frame that was already going to the screen.

Both array layers are written, so the dump shows the two eyes as the compositor
will duplicate them — which is what makes it the right instrument for #17, where
the question is whether a cursor drawn into the eye image lands correctly in
both copies.

This unblocks:

* **#17** — whether a composited cursor appears at the matching place in both
  halves. Today a correct implementation and a broken one look identical from
  here.
* **#21** — the clipped logo, drawn by the UI passes and therefore invisible to
  the old probe.
* **#30** — anything about where the UI sits once it is on its own canvas.

## Take 93 — #29 works, and it is not yet enough to answer #17

`X4VR_MV_DUMP_PRESENT=600` produced 19 frame pairs at `1408x1408, 2 layer(s),
bgra=1`. The frames contain the **whole UI** — the map, the top bar, the side
icon columns, station labels, the bottom bar. That is the thing the
end-of-render-pass probe could never show, and #29 is done: this is genuinely
the frame X4 asked to have presented.

Layer statistics behave as they should: the layers differ by ~8.5% of pixels in
map frames, which is the 3D content moving between the eyes while the mono UI
stays identical.

**It does not answer whether the cursor is in the eye image**, and the reason is
worth recording rather than retrying:

* a **temporal diff** cannot isolate it — 12.1% of pixels change between
  consecutive frames of the map, from ships, trade lines and animation, so a
  jiggled cursor is far inside the noise;
* a **colour search** cannot isolate it — Patola describes the cursor as a small
  blue hollow cross, and the map is blue everywhere;
* a **layer diff** cannot isolate it — if X4 drew it, it would be mono UI and
  therefore identical in both layers, which is indistinguishable from absent.

All three failures share one cause: **the dump records what the frame contained
but not where the pointer was when it was taken.** With a position, this is a
lookup at one coordinate and settled forever; without one, it is a search for a
small shape in a busy blue picture, which is the kind of measurement this project
has repeatedly got wrong.

Also observed, and relevant to #17: **the cursor stops being drawn after a few
seconds without movement.** Whatever draws it has an idle-hide policy, so a shim
that composites its own cursor has to reproduce or deliberately override that,
and a dump taken during an idle period contains no cursor *by design* — a second
way to read a real absence as evidence.

### What this makes next

The pointer position lives in the injector (`SDL_GetMouseState`) and the dump
lives in the layer. Pairing them is the **injector-to-layer shared-memory
channel** that DESIGN.md has specified since the start and that has never been
written — and it is required by #17 anyway, since drawing a cursor into the eye
image needs that same position on that same side.

So the channel is not a detour to answer this question; it is #17's first step,
and answering this question is a free consequence of taking it.

### Settled: the cursor is not in the eye image

Patola inspected all six map frames from take 93 at full resolution and found no
cursor in any of them. That is the direct form of the question I had been trying
to reach with statistics, and it agrees with the independent evidence already on
record.

**The stronger argument needed no dump at all.** The display shows X4's frame
duplicated side by side, so anything in the eye image appears *twice*. The P91
capture shows **one** cursor. That alone establishes it, and it was already
established when that capture was taken — the take-93 analysis above was
re-litigating a settled point, which is why it kept failing to find a cleaner
way to prove it.

So **#17's premise is confirmed**: the cursor is composited in display space,
outside X4's frame, and drawing it into the eye image is real work rather than a
duplicate of something X4 already does.

The idle-hide caveat is recorded and does *not* weaken this: a dump taken while
the cursor was hidden would contain no cursor for an uninteresting reason, but
the duplication argument does not rest on the dumps.

## Task #17 step 1: capturing X4's own cursor

The channel now carries the cursor **image**, not just its position.

`SDL_CreateColorCursor` is where X4 builds it. The injector captures the surface
there and `SDL_SetCursor` publishes whichever one X4 selects — which is also the
*shape* signal, since X4 swapping cursor is how the reticle becomes an arrow
over a station. A shim drawing a fixed image would be wrong in exactly the
moments the game is trying to tell the player something.

**Read positionally, and validated rather than trusted.** The injector has no
SDL headers and wants none, so `SDL_Surface` is read at fixed offsets the same
way `SDL_Event` already is. Every field is then checked — `w`/`h` in range,
`pitch >= w*4`, `pixels` non-null — and a capture that fails any of them is
refused with a log line naming the values. If SDL moves a field, the numbers
stop being plausible and nothing is captured, which beats compositing whatever
happened to be at offset 24.

**The pixel format is passed through unconverted.** A conversion table written
from memory would mangle the colours silently; instead the format id travels
with the pixels and the layer logs it once, along with a non-zero count, an
alpha-byte count and the first two pixels. One run says what the format actually
is, and *then* the unpacking gets written.

The image has its own seqlock, separate from the position's: the image changes
only when X4 switches cursor, while the position changes every frame, so one
counter would make every reader of the image retry constantly for a payload that
had not moved.

- **P97** — X4 calls `SDL_CreateColorCursor` at least once, and the captured
  surface passes the sanity checks. The log then names the size, the format id
  and the hot spot. A `cursor surface refused` line instead means the positional
  layout is wrong and step 2 cannot proceed on it; silence from both means X4
  sets its cursor by some route that is not `SDL_SetCursor`, and the shim needs
  that route found before it can match the game's cursor.

## Take 94 — P97 confirmed. The capture works, and the format is ARGB8888.

```
inject  sdl: captured cursor 0x14a51970 — 32x32 fmt=0x16362004 pitch=128 hot=(7,8)
inject  sdl: captured cursor 0x1810a860 — 32x32 fmt=0x16362004 pitch=128 hot=(0,0)
inject  sdl: captured cursor 0x1824e080 — 32x32 fmt=0x16362004 pitch=128 hot=(12,19)
inject  sdl: captured cursor 0x18100660 — 32x32 fmt=0x16362004 pitch=128 hot=(11,6)
layer   share: injector channel v1 connected
layer   share: cursor image 32x32 fmt=0x16362004 hot=(15,15) — 396/1024 px non-zero, 60 with byte3=0xff
layer   share: first row bytes 00 00 00 00 | 00 00 00 00
=== cursor positions logged: 16
```

Every part of the chain reported: the injector captured, the layer resolved the
symbol, and the image came back intact on the other side.

**`0x16362004` is `SDL_PIXELFORMAT_ARGB8888`** — printed from SDL3's own headers
rather than recalled, and it also reconstructs from `SDL_DEFINE_PIXELFORMAT`
(PACKED32, order ARGB, layout 8888, 32 bits, 4 bytes). SDL names packed formats
most-significant-byte-first, so the word is `0xAARRGGBB` and its **memory** order
on this machine is B, G, R, A — which is exactly what `VK_FORMAT_B8G8R8A8_*`
reads. The mapping is a re-labelling, not a conversion; no bytes move.

The logged `first row bytes 00 00 00 00` is a transparent corner and on its own
disambiguates nothing, which is why the unpacking was written against the format
id and then checked on a non-transparent pixel instead.

**396 of 1024 pixels non-zero, 60 fully opaque.** A sparse glyph with a small
solid core — Patola's hollow blue cross. That settles the compositing method:
`vkCmdCopyBufferToImage` and `vkCmdBlitImage` cannot blend, so either would
stamp a 32x32 opaque block onto the frame. It has to be a draw with alpha
blending, and step 2 is therefore a graphics pipeline rather than a copy.

**Four cursors, four different hot spots** — (7,8), (0,0), (12,19), (11,6), and
the selected one at (15,15). So the hot spot genuinely varies by context and
must be honoured: ignore it and a 32x32 glyph points at something up to 31 px
from what X4 hit-tests, which is close enough to look approximately right and
far enough to click the wrong thing.

## Task #17 steps 2 and 3: the pointer, drawn and de-duplicated

Step 2 draws it; step 3 removes the one gamescope draws. Both are off by
default, behind separate knobs, because seeing them one at a time is more
informative than seeing the end state.

### Where the draw goes, and why it is a draw

Into the **eye image, before the duplication**, in `SbsCompositor::composite()`.
One draw per layer then reaches both halves at the same in-eye position, and the
existing copy carries it to the screen. Drawing after the copy would mean two
draws at two offsets in *display* space — which is precisely the coordinate
system this whole task exists to get the cursor out of.

`layer/x4vr_cursor_draw.hpp` is a four-vertex textured quad with
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA` blending: push constants place it, no vertex
buffer, no index buffer. None of it is throwaway — a textured quad alpha-blended
into the eye image at an arbitrary rectangle is exactly what task #30's floating
UI canvas needs.

Shaders are compiled ahead of time and linked in (`tools/spv2hpp.py` embeds
them). A layer is loaded into someone else's process from a path the loader
chose, so "where are my shaders" has no good answer at run time.

**Everything is per swapchain image** — staging buffer, cursor texture,
descriptor set, framebuffers. That is not caution: `composite()` has already
waited on this image's fence before calling in, so this image's previous
submission has retired and its resources are free to rewrite. One shared texture
would need a stall or a ring to say the same thing.

### The channel now publishes the position X4 receives

Step 1 published the *unfolded* SDL position, reasoning that the fold (#19) is a
flatscreen ergonomic the channel should not carry. That was backwards. The field
means "where in X4's frame does X4 believe the pointer is", and with the fold on
that **is** the folded value — X4 never sees the other one. Publishing the raw
position would have put the drawn cursor 704 px from the button it activates,
with both features working exactly as designed. It now publishes after the fold,
so the invariant is structural: whatever number leaves for X4 is the number the
layer draws at.

### It always draws

`cursor_visible` is not consulted. X4 imports neither `SDL_ShowCursor` nor
`SDL_HideCursor` — checked with `nm -D`, not assumed — so that flag is the
injector's *inference* from relative-mouse mode, not something X4 said. Acting
on an inference would make the pointer vanish for reasons no measurement has
pinned down. If a stray cursor turns out to sit in the cockpit during mouse-look,
gating on it is a one-line change, and it should follow a take that shows it.

### Step 3 has exactly one lever

`nm -D` on X4 lists six mouse entry points:

```
SDL_CreateColorCursor  SDL_GetMouseState  SDL_SetCursor
SDL_SetWindowMouseGrab SDL_SetWindowRelativeMouseMode SDL_WarpMouseInWindow
```

No `SDL_ShowCursor`, no `SDL_HideCursor`. So hooking either would have been the
fifth instrument in this project bound to a symbol X4 never calls — the check
cost one command and would have cost a run. Instead the injector resolves
`SDL_HideCursor` itself (exported by the libSDL3 X4 ships, 3.2.28, whether or
not X4 references it) and calls it once. Nothing can undo it, because X4 cannot
re-show a cursor through a function it never calls.

This also explains Patola's "it stops being drawn if I hold still": that is
gamescope's own `--hide-cursor-delay`, more evidence that the pointer on screen
belongs to gamescope and not to X4's frame.

### Verified offline, before spending a run

`tests/cursor_render.cpp` drives the overlay on a real GPU under the validation
layer, with no X4 and no layer: a two-layer image left in `PRESENT_SRC_KHR`
exactly as X4 leaves it, a synthetic channel, and a readback. It checks position
in eye coordinates, that **both** layers receive it, that a half-alpha texel
comes back at 128 rather than 255 (blending happened) or 64 (alpha not applied
twice), that a fully transparent texel leaves the frame alone, that the draw
does not bleed one pixel past the quad, and that the two layers are
byte-identical. `tests/cursor_place.cpp` locks the format mapping and the
hot-spot arithmetic, and checks the generated shader header still matches the
`.spv` it came from.

All of it passed on the first run. The only failures were two validation errors
belonging to the test's own stand-in — `PRESENT_SRC_KHR` is only a legal layout
when `VK_KHR_swapchain` is enabled, which X4 does and the harness had not.

- **P98** — with `X4VR_CURSOR=1` and the injector present, the layer logs
  `cursor: overlay pipeline built` and one `cursor: drawing 32x32 hot=(h,v) into
  2 layer(s)` line, and **two** pointers are visible: X4's, drawn identically in
  both halves and landing on whatever it selects, and gamescope's, in one place
  on the composited screen. The two agreeing about *which object* is under them
  is the measurement; a drawn cursor that selects something 704 px away would
  mean the published position is still the wrong one of the two.
- **P99** — adding `X4VR_HIDE_CURSOR=1` leaves exactly one pointer, the drawn
  one, and `sdl: SDL_HideCursor() -> 1` in the log. If two remain, gamescope
  draws a pointer of its own independent of the client's, and the next lever is
  `X4VR_GRAB_CURSOR=0` or a gamescope flag rather than anything in SDL.

## Takes 95 and 96 — P98 and P99 confirmed. The pointer is in the frame.

    X4VR_TAKE=95-CURSOR ... X4VR_CURSOR=1 X4VR_LOG=/tmp/x4vr-take95.log
    X4VR_TAKE=96-CURSOR ... X4VR_CURSOR=1 X4VR_HIDE_CURSOR=1 X4VR_LOG=/tmp/x4vr-take96.log

Take 95:

    layer   cursor: overlay armed (X4VR_CURSOR=1)
    layer   cursor: overlay pipeline built for a B8G8R8A8_UNORM eye
    layer   cursor: drawing 32x32 hot=(15,15) into 2 layer(s) of the 1408x1408 eye
            — first at x=719.0 y=703.0 (channel says visible),
              texture B8G8R8A8_UNORM into an eye of B8G8R8A8_UNORM

Take 96 adds the one line that separates it:

    inject  sdl: SDL_HideCursor() -> 1 — the compositor's pointer is suppressed

No `cursor surface refused`, no `not one this knows how to consume`, no
`creation failed`. The stereo state is unchanged and still correct:
`STEREO composite`, `right half from layer 1`, `shear m8 L=0.42666 R=-0.42666`.

**Both runs used the known-good knob set with nothing added but the cursor.**
`score_run.py` reports `no settled probe samples` and `masked nothing` for both,
and neither is a defect: `X4VR_MV_PROBE` and `X4VR_MV_INVENTORY` were not passed,
so the scorer had no material. These two takes are scored on the cursor lines.

### Three cursors, and why that is the right number

Patola, on take 95: *three* cursors — the two the mod draws, and the original
between them.

**Two drawn cursors is correct, not a defect.** The composite duplicates the eye
image side by side, so one cursor drawn into the eye image appears once in each
half — one per eye. In a headset each eye sees exactly one.

The third one's *position* is a free re-confirmation of P23/P91. Our cursor sits
at eye-x in the left half and eye-x+1408 in the right; gamescope draws its own
at screen x+704, because X4's 1408-wide surface is centred in the 2816-wide
composite. Exactly midway between the two. The 704 translation has now been
measured three times, and the third time it was visible on screen.

**And the idle-hide belonged to gamescope.** Patola: the drawn cursors do not
disappear, the central one does. That closes the question take 93 opened, and
retroactively explains why that take's dumps contained no cursor — the thing
that was vanishing was never in X4's frame.

### The hot spot was the part that could have looked right and been wrong

X4 built more than twenty distinct cursors in these runs, all 32x32 ARGB8888,
with hot spots ranging from (0,0) to (15,26). Take 96 was an exhaustive
exercise — icons, pull-downs, text, collapsible sections, and the map rotated in
3D — and every hitbox landed. That is the check that matters: a shim that
ignored the hot spot would still have drawn a pointer that *looked* placed, and
would have missed by up to 31 px depending on which cursor X4 had selected.

- **P98 CONFIRMED.** Two drawn pointers, identical in both halves, over the same
  object as gamescope's, changing shape with context, selecting on their exact
  location.
- **P99 CONFIRMED.** `SDL_HideCursor()` returned 1 and the compositor's pointer
  is gone. Two remain, which is one per eye.

### This supersedes #19 rather than completing it

Neither run passed `X4VR_INPUT_FOLD`. The fold was the take-90 compromise that
fixed picking by rewriting the position X4 reads, at the cost of breaking cockpit
steering — two requirements no formula reconciled. Drawing the pointer into the
frame dissolves the conflict instead of trading between them: nothing is
rewritten, so steering is untouched, and picking is exact because cursor and
target are now the same kind of object in the same coordinate system.

The fold stays in the tree, off, as the record of a measurement. It has no
remaining job.

## Task #30 groundwork — the UI is `World`, and the module is the wrong granularity

Measured offline with `tools/canvas_predicate_design.py` over three independent
dump/log pairs (takes 61, 74 and 80; 397 dumped modules each, 382 with a vertex
stage). Log and dump must come from the *same* take — module and pass serials
are per-run — and the tool takes both as arguments so that pairing is explicit
rather than assumed.

### What the UI pass actually is

In all three takes the UI is `rp #33`: `1 colour [50L] no-depth`, i.e.
`B8G8R8A8_SRGB` with no depth attachment, which `classify_unsheared()` calls
unsheared (a colour pass with no depth is a fullscreen post pass) and
`classify_per_eye()` masks anyway. It binds exactly five modules, and they split
two ways, identically in every take:

| | vertex stage | positions through |
|---|---|---|
| one module | `gl_VertexIndex`, no vertex attributes | nothing — procedural fullscreen |
| four modules | `SPECIAL_VERTEXLOCATION_POSITION`, `IO_uv0`, `IO_uv1`, `IO_color`, one render target | `M_worldviewprojection` (set 3, member 0) |

Every other LDR pass — `rp #0`, `#1`, `#4`, `#7`, `#10`, `#45` — binds only
procedural or fragment-only modules. They are blits, as take 32 onward has said.

### Three corrections to the note written above under "Immersive UI mode as a goal"

**1. `X4VR_CLIP_SHIFT_NONWORLD` is the wrong knob.** That note said it "is the
right shape for that knob". It is not: the four UI modules read
`M_worldviewprojection`, so `classify()` returns `Kind::World` for them and they
are patched with `K_world`. `K_nonworld` can never reach a menu quad. The note
reasoned from the *pass* verdict (`MONO (all-LDR/UI)`) to a *module* class, and
those are different questions — the same conflation `split_note` in
`pass_is_per_eye()` was written to prevent.

**2. The module is the wrong granularity, so a third category cannot fix this.**
Every one of the four UI modules is *also* bound to `rp #13` and/or `rp #23`, the
main world passes:

    take 61   mod-0261 -> rp [13, 23, 33]   mod-0263 -> [13, 23, 33]
              mod-0265 -> rp [23, 33]       mod-0351 -> [13, 23, 33]
    take 74   mod-0263 -> rp [13, 23, 33]   mod-0265 -> [23, 33]
              mod-0267 -> rp [13, 23, 33]   mod-0293 -> [13, 23, 33]
    take 80   mod-0261 -> rp [13, 23, 33]   mod-0263 -> [13, 23, 33]
              mod-0265 -> rp [23, 33]       mod-0277 -> [13, 23, 33]

Only the procedural module is exclusive to the UI pass. **The same shader draws
a ship hull in `rp #13` and a menu quad in `rp #33`**, so no per-module
predicate — however many categories it has — can give those two draws different
transforms. `vkCreateShaderModule` sees one module and patches it once.

What is needed is a third **variant**, selected at `vkCreateGraphicsPipelines`
where the render pass is known, exactly as `needs_original()` already selects
the unpatched twin. The two-way `World`/`NonWorld` split stays as it is; what
gains a third member is the variant table, not the classification.

**3. The input blocker named in that note is gone.** It said "a canvas at a
virtual depth needs the cursor projected onto it — which is what task #19 exists
to make possible." #17 did it instead: the cursor is now drawn into the eye
image, per array layer, at an arbitrary rectangle. Giving it the same offset the
canvas gets is a per-layer term in `cursor_rect()`, not a projection problem.

### The canvas transform is the world shear with `z` pinned

`patch_vertex_clip` applies `gl_Position = K · gl_Position`. For `K` = identity
with `K[12] = s`, that is `(x + s·w, y, z, w)`, whose NDC x is `x/w + s` — a
constant NDC shift **for any `w`**. It does not matter whether X4's UI matrix is
an orthographic screen transform (`w = 1`) or the map's perspective one; the
shift is the same. That is what makes it a canvas rather than geometry.

Equating that to the world offset already derived above,
`704·sx·(ipd/2)/z` px on a 1408-wide eye, and using NDC 1 = 704 px:

    s = sx · (ipd/2) / z          z = sx · (ipd/2) / s

At `sx=1.3333`, `ipd=0.064` that is `s = 0.042666/z`, i.e. **30/z px per eye** —
numerically identical to the world formula, because *putting the UI at z metres*
and *world geometry at z metres* are the same statement. So the knob is a
distance in metres, not an NDC number, and it stays correct when IPD or `sx`
change:

    z = 1 m -> 30 px    2 m -> 15 px    5 m -> 6 px    10 m -> 3 px    inf -> 0

`s_left = +s`, `s_right = −s`: the left eye sees a near object displaced toward
the right, which is the sign the world shear already uses (`m8 L=+0.42666
R=-0.42666`). `s = 0` is infinity and is exactly today's behaviour, which is the
useful negative control — the knob unset must reproduce the current frame.

### The design

* At `vkCreateShaderModule`, when the canvas is asked for, build a **third**
  module from the same bytes patched with the constant-shift `K` instead of the
  world `K`, and remember whether the module classified `World`.
* At `vkCreateGraphicsPipelines`, bind that third module when the pass is a
  **canvas pass** — unsheared because all-LDR/no-depth, and masked — *and* the
  module classified `World`. Both halves of the join do real work: the pass half
  keeps the world passes out, the module half keeps the procedural fullscreen
  module in `rp #33` out.
* The cursor overlay draws at `x ± s` per array layer, so pointer and canvas
  move together.

Gated on intent: with `X4VR_CANVAS_M` unset nothing is built and nothing
changes. If a canvas variant fails to build, the module falls back to the
unpatched twin — today's behaviour — but the refusal is logged by name and
counted, because a torn UI is not escapable (you cannot read the menu to quit)
while a silently mono UI at least still plays. Scoring is from the log, as
always, not from the screen.

### Predictions, before any of it is written

* **P100** — with `X4VR_CANVAS_M=2`, the UI moves as a rigid whole by 15 px per
  eye in opposite directions, and nothing outside `rp #33` moves. The log will
  name a canvas-variant count and the resolved `s`; the eye-image dumps will show
  the HUD displaced and the starfield unchanged.
* **P101** — hit-testing stays exact. X4's CPU-side test is untouched, and the
  drawn cursor takes the same `±s`, so pointer and target keep their
  relationship in both eyes. This is the claim take 96 makes checkable.
* **P102** — `X4VR_CANVAS_M` unset reproduces take 96's frame exactly. If it does
  not, the variant is being bound when it was not asked for, and nothing measured
  after that point is trustworthy.

Predicted here, before the code, so the run that tests them cannot be reasoned
about backwards.

### Validated offline before asking for a take

**The patch.** `tests/spirv_patch vert-clip` applies the canvas matrix, and
sweeping it over all 397 of take 80's dumps:

    modules: World=320 other=77 | patched=382 refused=15 | spirv-val invalid=0

Every World module accepts it and every patched module is valid SPIR-V. The 15
refusals are exactly the 15 modules with no vertex stage — the fragment-only
blit shaders — so nothing that could carry the UI was turned away. That also
fixes the number to look for in the log: **`canvas final:` must report on the
order of 320 variants built, and never 0.**

**The arithmetic.** `tests/view_math` proves `canvas_shift(z)` equals the world
shear's displacement at depth z across three near planes an order of magnitude
apart, so `near` provably cancels rather than approximately cancelling.

**The selection.** `tests/run-multiview-render.sh` creates the discriminating
pair on a real device: `rp #2` (LDR, no depth) reports `+CANVAS` and `rp #6`
(HDR, no depth) must not. Those two are indistinguishable to every other
predicate in the layer.

**The pointer.** `tests/run-cursor.sh` runs the overlay on a GPU under
validation twice — canvas off, where both layers must stay byte-identical, and
canvas on at 8 px, where they must differ *by a translation*: each eye's quad
clear in the other. Zero validation errors either way.

**The measurement.** `tools/canvas_shift_map.py` was written before the take
and validated on synthetic pairs built from take 94's eye dump: a translated
band reads CANVAS, an unmodified copy reads NO CANVAS rather than passing
vacuously.

### The two takes

A control first, because the last four commits touched the present path and a
canvas defect must not be confused with a regression. Both dump the eye image
so the control doubles as a negative control for the shift map on real pixels.

    X4VR_TAKE=97-CONTROL X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408
    X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2
    X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1
    X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_MV_PROBE=1
    X4VR_MV_DUMP=/tmp/x4vr-t97 X4VR_MV_DUMP_PRESENT=1
    X4VR_LOG=/tmp/x4vr-take97.log
    ./launch/x4vr-launch.sh

    X4VR_TAKE=98-CANVAS ...everything above, with...
    X4VR_CANVAS_M=2
    X4VR_MV_DUMP=/tmp/x4vr-t98 X4VR_MV_DUMP_PRESENT=1
    X4VR_LOG=/tmp/x4vr-take98.log

`X4VR_MV_INVENTORY=1 X4VR_MV_PROBE=1` are new against take 96, which is why
that run scored `masked nothing` and `no settled probe samples` — the scorer had
no material, not a defect.

Scored with:

    python3 tools/score_run.py /tmp/x4vr-take97.log
    python3 tools/score_run.py /tmp/x4vr-take98.log
    python3 tools/canvas_shift_map.py /tmp/x4vr-t97-present-nN-layer0.ppm \
                                      /tmp/x4vr-t97-present-nN-layer1.ppm -30
    python3 tools/canvas_shift_map.py /tmp/x4vr-t98-present-nN-layer0.ppm \
                                      /tmp/x4vr-t98-present-nN-layer1.ppm -30

`-30` is twice the per-eye 15 px, because layer 0 was given `+s` and layer 1
`-s`. Take 97 must read **NO CANVAS** and take 98 **CANVAS**; the pair is the
measurement, and either one alone is not.

## Takes 97 and 98 — P100 and P102 confirmed; P101 untested, and two wrong turns

The canvas works. `tools/canvas_shift_map.py` over the eye dumps, take 98
against take 97 as the control, at the predicted `-30` px:

    CANVAS run  (t98)          CONTROL run  (t97)
      frame  blocks at 0 at -30      frame  blocks at 0 at -30
        150     249    0    248        150     111   89      1
        180     250    0    248        180     284  280      0
        210      67    8     26        210     286  282      0
        220      56    7     18        240      64   31      2

    peak at -30 px: canvas 99.6%, control 3.1%   -> VERDICT: CANVAS

- **P100 CONFIRMED.** The UI translated as a rigid whole by exactly the
  predicted 30 px. Frames 150 and 180 are the map, and there **248 of 249
  blocks moved together** — which is not a whole-frame shift but confirmation
  of something this file recorded long ago and had not connected to #30: *the
  map is drawn by the UI pass*. When the map is up, almost every pixel is
  canvas, so almost every pixel moves. Frames 210 and 220 are the mixed view,
  where a third of the blocks sit at exactly `-30` and the rest carry the
  world's own depth-dependent parallax. That mixture is the real check: a
  sharp spike at the canvas distance, and the world unchanged beside it.
- **P102 CONFIRMED.** The control put 280 of 284 blocks at zero and none at
  `-30`. With `X4VR_CANVAS_M` unset, the frame is the pre-canvas frame.
- **P101 NOT TESTED.** See below — the run was unusable at ~1 fps, so
  hit-testing was never exercised. It is not confirmed and is not being
  claimed.

The log agrees: `canvas: 2.000 m -> s=0.02133 NDC`, `348 variant(s) built,
0 REFUSED, swapped into 18 pipeline stage(s)`. 348 against the 320 the offline
sweep predicted, and 18 stages is about 9 pipelines — X4 ships combined modules,
so a swapped module counts twice per pipeline.

### Wrong turn 1: `X4VR_MV_DUMP_PRESENT=1` means every frame, not "on"

It is a **cadence**, not a boolean, and the recipe in this file asked for `=1`.
That is a ~12 MB readback and a full pipeline stall on every present: the dump
timestamps are 1.00, 1.03 and 1.01 s apart, so the game ran at about **1 fps**.
Patola could barely reach the map, and anything more involved — docking,
talking to an NPC — would not have been possible at all.

Nothing about the canvas caused it and no measurement above is affected; the
dumps are real frames. What it did cost is P101, which needed interaction.

The knob now says so on the first dump, in the units that hurt. Copying `=1`
out of take 94's env line without asking what the number meant is the same
mistake as reading a list positionally: the value looked like a flag.

### Wrong turn 2: the shift map's verdict rule, refuted by its own control

`canvas_shift_map.py` shipped with the rule "more blocks at intermediate shifts
than at the expected one means the UI was sheared rather than translated", and
the prediction that a healthy frame shows "nothing in between". Run on take 97
it returned **SHEARED — the UI took K_world** for a run that had no canvas at
all and therefore nothing that could have been sheared.

The intermediate population is the world's own parallax. Near geometry
legitimately occupies every shift between 0 and the canvas distance, and in a
cockpit frame it outnumbers the UI — 29 of 64 blocks in the control. The rule
was written from a picture of the frame nobody had measured, and it would have
reported a defect in a working canvas run just as readily.

The control caught it, which is the entire reason the pair was run rather than
take 98 alone. The tool now **refuses to give a verdict without a control** and
judges on the contrast between the two runs. Both directions are self-checked:
scoring the control against itself reads NO CANVAS, and scoring the two the
wrong way round reads NOT ATTRIBUTABLE.

### The scorer's FAIL was a missing instrument, not a defect

Both takes failed on `no settled probe samples for the swapchain`, and the
advice attached to it — *sit still longer* — was wrong. The probe walks the
frame's images one at a time, about one every 30 s; in a 277 s run it reached
eight, none of them the swapchain. Sitting still cannot help. The message now
says which it is and points at the present dumps, which answer the same
question directly and better. It is still a FAIL: no evidence is no evidence.

Stereo was healthy where the probe did land — `img #63 DIFFER 20.36%`,
`img #57 DIFFER 33.16%`, both with `l1/l0 ~ 1.00`.

## Take 99 — PASS, and the stutter is the instruments draining the queue

    swapchain  20 settled sample(s), layer1/layer0 99.7%..101.7%, 0 bit-identical
    grade  STEREO — every settled sample carries a real per-eye difference
    canvas 2.000 m -> s=0.02133 NDC, 15.0 px per eye on a 1408-wide eye
    canvas final: 346 variant(s) built, 0 REFUSED, swapped into 18 pipeline stage(s)
    PASS

**The probe reached the swapchain this time**, sampling `#50`–`#53` twenty
times — which settles the takes 97/98 FAIL as diagnosed: the probe walks the
frame one image at a time and a 277 s run did not get there. 326 s did. Nothing
was wrong with those runs.

**The cursor and the canvas provably took the same shift.** The overlay logged
`canvas shift 0.02133 NDC (15.0 px per eye)` against the canvas's own
`s=0.02133`. That is the half of P101 a log can answer, and it is the half that
could have been silently wrong: the two are set from one variable, so a
disagreement would have meant the cursor read it before it was published. It
did not. What is still untested is only whether a click lands where it looks
like it should, which is a human check.

### The instruments stall the frame, and the numbers say by how much

Patola, on take 99: smooth for about half a second, then unresponsive for three
to five, repeatedly — and that this interference makes precisely-aimed tests
impractical. He is right, and the log names the culprit:

    67 mv probe samples over 326 s  ->  one every 4.87 s

which is exactly the period he described. `probe_collect()` calls
`vkQueueWaitIdle` — a full GPU drain — and then walks both layers on the CPU,
about 16 MB of hashing and per-texel comparison, per sample. The present dump
does the same drain. At `X4VR_MV_DUMP_PRESENT=300` the dumps were 33 s apart
and are *not* the cause; the probe is.

The overall rate was about 9 fps (10 dumps × 300 presents over 326 s), against
take 96's playable rate with none of these three knobs set.

**So the split is now explicit.** A *measurement* take may stall freely — it is
being read from dumps and logs, and nobody has to aim at anything. An
*interaction* take must leave `X4VR_MV_PROBE`, `X4VR_MV_DUMP` and
`X4VR_MV_DUMP_PRESENT` unset; everything the canvas needs to be judged
(`canvas final:`, the cursor's shift) is logged once and costs nothing per
frame. Both stalling instruments now say so in the log the first time they run,
in the terms that matter — `expect visible stalls`, and for the dump the fact
that it is a cadence rather than a flag.

This is the third time in three takes that an instrument, not the mod, was what
made a run hard to read.

## Take 100 — P101 confirmed. The canvas is on, and nothing about aiming changed

    canvas 2.000 m -> s=0.02133 NDC, 15.0 px per eye on a 1408-wide eye
    canvas final: 340 variant(s) built, 0 REFUSED, swapped into 18 pipeline stage(s)
    cursor: ... canvas shift 0.02133 NDC (15.0 px per eye)

No `X4VR_MV_PROBE`, no dumps: zero stalling-instrument announcements in the log,
and Patola reports the framerate was finally smooth. He waited in the cockpit,
opened the map, clicked elements, changed the 3D perspective, and everything was
clickable — the cursor highlighting elements *exactly* in the right spot, with no
offset and no leeway.

**P101 CONFIRMED.**

### Why "it looked exactly like take 96" is the result and not a null

Take 96 had no canvas at all, so a run that quietly failed to engage one would
have looked *identical* — and this file has been caught by that shape before.
The log is what separates them: **18 pipeline stages took a canvas variant.**
The UI in this take was displaced 15 px per eye in opposite directions, and the
reason it was indistinguishable from take 96 in use is that the pointer was
displaced with it. That is the entire design working, not the absence of one.

The check that could have failed silently was already answered in the log —
`canvas shift 0.02133` on the cursor line against `s=0.02133` on the canvas line
— and take 100 is the human confirmation that the two agreeing on paper means a
hitbox lands in practice.

### A clarification, because the question was badly asked

"Whether the pointer sits on the button" was unclear phrasing and Patola said so.
What it meant: the drawn cursor is a bitmap **we** composite at a position **we**
choose, while X4 decides what is highlighted with its own CPU-side hit test at a
position we do not control. If the two disagreed, the highlight would land on a
neighbouring element while the arrow pointed somewhere else — both mechanisms
working exactly as written, and the result unusable. "Highlights elements exactly
in the right spot" is precisely the observation that rules it out.

## Task #21 — the logo measured from take 97's dumps, and the shear is not it

No run needed: takes 97–99 dumped the eye image from frame 0, so the start menu
is already on disk. Frame `n20` of take 97 (control, no canvas) has the Start
Menu over the planet, with the X4 logo top right and cut off at the frame edge.

Measured on the two array layers of the same frame, stable across `n20`, `n22`
and `n26`:

| region | eye-to-eye shift | ncc | implied z |
|---|---|---|---|
| the logo's X glyph | **−3 px** | 0.992 | **~20 m** |
| the Start Menu text | 0 px | 1.000 | inf |
| the station ring | 0 px | 0.997 | inf |
| the planet's limb | 0 px | 0.998 | inf |

using `disparity_px · z = 704 · sx · ipd = 60.07` at `sx=1.3333, ipd=0.064`.

**The logo is 3D geometry in the menu's background scene, at about 20 m.** It is
not a UI element: the menu text beside it is flat at 0 px, exactly as an
unsheared UI pass should be, while the logo carries real parallax.

### This refutes the leading hypothesis in this file

The surviving candidates recorded at take 46 were `X4VR_STEREO` — "K reaching a
pipeline that draws the logo" — and `X4VR_BINDLESS_MIRROR`. The first is real
and is **1.5 px per eye**. The clipped part of the "4" is of order a hundred
pixels: the bright ring runs to column 1407 in both eyes, with 28 lit rows in
the final column of the left eye and 26 in the right. The shear is present,
correctly signed (the left eye is displaced outward and is clipped ~3 px more
than the right), and about fifty times too small to be the cause.

So **both eyes are clipped, and the shear only modulates it.** Whatever removes
the right side of the logo does so before either eye is considered.

### What is left, and why #21 is probably #24 wearing a different hat

X4 believes its window is 1408×1408 and renders 1408×1408 — the SBS split asks
it for one eye's worth, and `SDL_GetWindowSize` confirms it agrees. A square
frame has a much narrower *horizontal* field than the 16:9 the menu was laid
out for: `sx = sy/aspect`, so dropping the aspect from 1.778 to 1.0 magnifies x
by 1.778 and pushes scene content on the right out of frame. The logo sits on
the right of that scene.

If that is the mechanism, #21 is not an independent bug and not a regression
from any commit. It is the same constraint as **task #24** — the frame is
narrower than the content assumes — and fixing #24 fixes this for free. It also
explains why nobody could find the take-33-to-41 code change: take 33 was never
recorded (the `env: run =` line starts at take 34), so "the same resolution with
a whole logo" was never actually established.

* **P103** — rendering each eye at 16:9 instead of 1:1 brings the logo back
  whole, with no code change at all. If it does not, the aspect is exonerated
  and the cause is something the layer does to that geometry, which the 1.5 px
  measurement says is not the shear.

The test changes one thing. `X4VR_W`/`X4VR_H` override the composite size, so a
2816×792 composite is 1408×792 per eye — 16:9 — with everything else exactly as
take 97 and the dumps still working:

    X4VR_TAKE=101-ASPECT X4VR_W=2816 X4VR_H=792 X4VR_STEREO=1
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_MV_DUMP=/tmp/x4vr-t101
    X4VR_MV_DUMP_PRESENT=60 X4VR_LOG=/tmp/x4vr-take101.log
    ./launch/x4vr-launch.sh

`X4VR_RES` is deliberately *not* set: the launcher derives it from the composite
size, and setting both is how the two would come to disagree. A measurement
take, so the dump cadence is affordable — but 60, not 1, and it only has to
reach the menu.

### The menu is a free ruler for #24 and #25

Worth keeping whatever the answer is. The start menu is reproducible, identical
every launch, and contains objects at two known-ish depths: everything at
infinity, and one object at ~20 m. That makes it a calibration scene for the
questions Patola raised — how to pick distance, perspective and IPD across
modes — without needing a savegame or a steady hand. `disparity_px · z = 60.07`
at the current settings is the whole conversion.

## Take 101 — the aspect never changed. Task #31 arriving through the launcher

Patola, on the result: not 16:9 — the square scene pushed right in the left eye
and left in the right, in an outstretched horizontal window. Exactly right, and
the log says why:

    env: run = ... X4VR_H=792 X4VR_W=2816 X4VR_RES=1408x1408 ...
    sdl: SDL_GetWindowSize -> 2816x792
    swapchain created: 2816x792
    WARNING swapchain is 2816x792, expected 2816x1408

`X4VR_RES=1408x1408` — **the launcher set it**, from the compiled-in SBS
constants, ignoring the `X4VR_W`/`X4VR_H` it had just honoured for gamescope.
Three extents, three values: the window 2816×792, X4's render 1408×1408, the
composite's idea of the frame 2816×1408. That is task #31 word for word,
reached through the launcher instead of the layer.

So take 101 measured nothing about aspect. X4 rendered a square, as it always
had; only the window it was poured into changed shape. P103 is **untested**, not
refuted.

**The bug is real and predates the take.** `W` and `H` correctly pick up the
overrides on the line above, and then both branches call `sbs_dim` again rather
than using them, so `X4VR_W`/`X4VR_H` — documented as "force the SBS size" —
could only ever move gamescope. Fixed by deriving `X4VR_RES` from `$W`/`$H`.
The default path is unchanged by construction: with neither variable set, `W`
and `H` *are* the constants, and `2816×1408 -> 1408x1408` as before.

    X4VR_W=2816 X4VR_H=792   -> X4VR_RES=1408x792   eye aspect 1.778
    X4VR_W=2816 X4VR_H=1408  -> X4VR_RES=1408x1408  eye aspect 1.000  (default)

The layer's warning was misleading too, and in the way that matters: it tested
`want` (1408×1408, from `X4VR_RES`) and printed `expected 2816x1408` (the
compiled constant). Two numbers with nothing to do with the comparison, on the
one line that was trying to report a size disagreement. It now prints what it
compared against and where that came from.

Re-run of P103, unchanged except that the launcher now propagates the size:

    X4VR_TAKE=102-ASPECT X4VR_W=2816 X4VR_H=792 X4VR_STEREO=1
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_MV_DUMP=/tmp/x4vr-t102
    X4VR_MV_DUMP_PRESENT=60 X4VR_LOG=/tmp/x4vr-take102.log
    ./launch/x4vr-launch.sh

Check the log before judging anything on screen: `env: run =` must now show
`X4VR_RES=1408x792`, and there must be **no** swapchain-size warning. If either
is wrong the run is another take 101 and the picture means nothing.

## Take 102 — the render followed, the layer did not. The third extent

`X4VR_RES=1408x792` this time, so the launcher fix worked and X4 rendered 16:9.
X4's own swapchain confirms it: `1408x792 format=44 (pid 3105232)`. And then:

    sbs: SPLIT OFF — X4 asked for 1408x792 but one eye is 1408x1408
    sbs: composite armed for 1408x792, each eye 704x792
         (X4 renders full width, left half duplicated)

**The layer's idea of an eye was still the compiled constant.** So the split
refused, the frame degraded to duplicating the left half, and the screen showed
two copies of one eye 704 apart — which is what Patola saw and described for
take 101, arrived at by a different route.

That is the whole of task #31 in one line: X4's window, X4's render and the
layer's expectation are three numbers, and two of them moved. Every component
behaved exactly as written.

Fixed with one function, `expected_eye()`, reading `X4VR_RES` — the same value
the launcher derives from the `W`/`H` it gives gamescope — so all three follow
one number. The default is unchanged by construction: `X4VR_RES=1408x1408` is
what the launcher already sets, and it equals the old constant.

### I told Patola to check the wrong things, twice

For take 102 I named two log lines: `X4VR_RES` (which was right) and the absence
of a swapchain warning (which the run could not pass — the layer is loaded in
gamescope's process too, and gamescope's swapchain is legitimately the composite
size, not an eye). The warning is now gated on `g_active`.

Neither line was the one that mattered. **`SPLIT OFF` was, and `score_run.py`
prints it first**, as `split OFF (FAIL)`, before anything else is worth reading.
The instruction should have been *run the scorer* — which is the project's own
rule, restated in this file more than once, and set aside in favour of hand-
picked greps on the two occasions it would have paid off immediately.

P103 remains untested after two attempts, neither of which reached the question.
The re-run is take 103, identical to 102:

    X4VR_TAKE=103-ASPECT X4VR_W=2816 X4VR_H=792 X4VR_STEREO=1
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_MV_DUMP=/tmp/x4vr-t103
    X4VR_MV_DUMP_PRESENT=60 X4VR_LOG=/tmp/x4vr-take103.log
    ./launch/x4vr-launch.sh

Judged by `python3 tools/score_run.py /tmp/x4vr-take103.log`, and by nothing
else until that line says `split on`.

Note for #24: `X4VR_PROJ_SX=1.3333` was measured at a 1:1 eye and is passed
unchanged here, so at 16:9 the *shear* will be derived from a stale `sx`. That
does not affect P103 — the question is where the logo's right edge falls, which
is X4's projection and not ours — but it means take 103 is not a candidate for a
known-good state, and any per-eye offset measured in it is the wrong size.

## Take 103 — P103 CONFIRMED. The clipped logo was the aspect ratio

`split on`, `X4VR_RES=1408x792`, and the eye dumps are 1408×792. Measured on
`n10`, not taken from a description:

    take 97  (1:1)   bright logo content reaches column 1407 of 1407
                     28 lit rows in the final column        -> cut by the frame
    take 103 (16:9)  bright logo content reaches column 1265 of 1407
                     0 lit rows in the final column         -> whole, with margin

**Task #21 is resolved, and it was never a regression.** No code change was
involved in fixing it — only the aspect X4 was asked to render. The take-33
baseline it was framed against was never recorded, so "the same resolution with
a whole logo" had no evidence behind it; what changed between those takes was
the arrival of the SBS split, which asks X4 for a square eye. A square frame has
a much narrower horizontal field than the 16:9 the menu was laid out for, and the
logo sits at the right of that scene.

So **#21 was #24 wearing a different hat**, exactly as predicted. It closes as a
consequence of the eye aspect, and anything that widens the field — which is
#24's job — removes it. What was ruled out along the way is worth keeping: the
shear does reach the logo, at **1.5 px per eye**, correctly signed, and roughly
fifty times too small to be the cause.

### What I could not measure, and the instrument that will

I tried to extract X4's FOV policy — whether it holds the vertical field and
narrows the horizontal (`vert-`), or the reverse — by matching the logo between
the two takes. It did not converge. Three estimates that must all be the same
number if the difference is a scale about the centre:

    glyph ndc width ratio    0.869
    centroid ndc_x ratio     0.753
    segmented area ratio     0.520   (should be the product of the two scales)

They disagree, so the segmentation is not isolating the same object in both
frames — the planet shares the logo's hue, and a brightness threshold that
separates them at one exposure does not at the other. **No number from this is
quoted anywhere**, because a wrong `sx` would propagate straight into the shear.

The right instrument already exists and needs no photogrammetry:
`X4VR_DUMP_MATRICES=1` makes the layer read X4's own camera block and log

    proj MEASURED: sx=... sy=... near=... (jittered sx=... near=...)

which is X4's projection exactly, not an inference from pixels. Two short runs
with it — one at `X4VR_H=1408`, one at `X4VR_H=792`, nothing else changed —
answer the FOV-policy question outright and give #24 its starting numbers.
Neither take 97 nor take 103 set it, which is why the four `proj` lines in those
logs are all about invproj.

That is #24's opening move, and it should be taken before any code: the current
`X4VR_PROJ_SX=1.3333` is a value measured at a 1:1 eye and passed as a constant,
so at any other aspect the shear is derived from a stale number.

## Next: task #24, from a clean slate

Everything below is the whole handoff. It needs no context from the session that
wrote it.

**Where things stand.** `stage4-ui-canvas` is the current known-good state: correct
stereo, a pointer in the frame, and the UI on a canvas at `X4VR_CANVAS_M` metres
(off by default). #21 closed as a consequence of the eye aspect and #30's first
stage is done. Open: #23, #24, #25, #31, #32, #33.

**The one thing that must be measured before any code is written for #24.**
`X4VR_PROJ_SX=1.3333` is a constant, measured once at a 1:1 eye and passed on
every run since. It is the `sx` the eye shear is derived from. At any other
aspect, and at any zoom, it is stale — which is #24 and #23 respectively, and
they are the same defect seen through two different knobs.

`X4VR_DUMP_MATRICES=1` already makes the layer read X4's own camera block and log

    proj MEASURED: sx=... sy=... near=... (jittered sx=... near=...)

with a fresh line whenever `sx` or `near` moves, and `proj STEADY` when they do
not. That is X4's projection exactly. **Do not infer it from screenshots** — the
attempt recorded above gave three mutually inconsistent estimates because the
planet shares the logo's hue.

### Run A — the 1:1 eye, and the zoom range for #23

No `X4VR_W`/`X4VR_H`, so the launcher's defaults give a 1408×1408 eye.

    X4VR_TAKE=104-PROJ-1x1 X4VR_DUMP_MATRICES=1 X4VR_STEREO=1
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take104.log
    ./launch/x4vr-launch.sh

Load a save, then **zoom through the full range in the cockpit** and back out.
Each distinct `sx` prints its own line, so this run answers #23 as well: it says
whether a constant `sx` can ever be right, and if not, over what range it moves.

### Run B — the 16:9 eye, everything else identical

    X4VR_TAKE=105-PROJ-16x9 X4VR_W=2816 X4VR_H=792 X4VR_DUMP_MATRICES=1
    ...exactly as run A...  X4VR_LOG=/tmp/x4vr-take105.log

Only the eye aspect differs. Reaching the start menu is enough; no zoom needed.

### Reading them

    python3 tools/score_run.py /tmp/x4vr-take104.log
    python3 tools/score_run.py /tmp/x4vr-take105.log
    grep -E "proj (MEASURED|STEADY|ASSUMED)" /tmp/x4vr-take104.log
    grep -E "proj (MEASURED|STEADY|ASSUMED)" /tmp/x4vr-take105.log

**`score_run.py` first, and if its first line is not `split on` nothing else in
that run means anything.** Takes 101 and 102 were both spent because the runs
were handed over with hand-picked greps to check instead of the scorer, and both
times the line that mattered was one the greps did not include. The scorer prints
it first, by design.

### The question the two runs answer

With `sx₁` from run A and `sx₂` from run B, and the eye aspect going from 1.000
to 1.778:

* `sx₂ ≈ sx₁` — X4 holds the horizontal field and grows the vertical. Then a
  wider HMD field means asking X4 for a *wider frame*, and #21's clipping was
  the vertical field being cropped rather than the horizontal narrowed.
* `sx₂ ≈ sx₁/1.778` — X4 holds the vertical field and narrows the horizontal,
  the ordinary `vert-` policy. Then a wider field is a matter of overriding the
  projection, not the window.
* Neither — X4 clamps somewhere, which is what task #24's title has always
  assumed and what nothing has yet verified.

Whichever it is, `sy` from the same line gives the vertical field for free, and
`near` is the third term `make_eye_shear` needs. Predict which before reading the
logs, and write the prediction down first.

## Task #24 — the handoff above was wrong twice, and X4 has a `<fov>` tag

Written before take 104, which is the run this section specifies.

### Retracting run A

The section above asks for a run at a 1:1 eye with `X4VR_DUMP_MATRICES=1`,
"load a save then zoom through the full range", to establish `sx₁` and to answer
#23. **Both halves of that were already on disk when it was written.** Ten logs
in `/tmp` carry `proj MEASURED`, and every one of them reads

    proj MEASURED: sx=1.33333 sy=-1.33333 near=0.10000

at `X4VR_RES=1408x1408` — takes 52, 53, 54, 55, 56, 57, 58, 59, 60 and 66. And
take 54 already swept the zoom: `score_run.py` now reports its range as
`1.00000..37.75372 (37.8x)`. Asking for run A would have spent a take
re-measuring a number ten takes agree on.

This is the failure mode the project keeps hitting from the other side. The rule
has been "do not spend a run on what a dump can answer offline"; the same rule
says do not spend a run on what a **previous run already answered**, and the
check costs one `grep` across `/tmp`. It is now the first step of every run
recipe here.

### Retracting the reading key

The three outcomes listed above have their consequences inverted. With
`aspect = W/H` and the standard perspective matrix, `sy = sx·aspect`, so at a
*wider* frame:

* `sx₂ ≈ sx₁` means the horizontal field is **held** and the vertical is
  **cropped** — the ordinary `Vert-`. The section above calls this "grows the
  vertical", which is the opposite.
* `sx₂ ≈ sx₁/1.778` means the vertical is held and the horizontal **widens** —
  `Hor+`. The section above calls this "narrows the horizontal", also the
  opposite, and then labels it `vert-`.

Recorded rather than edited away, because the error is instructive: it was
written while reasoning about fields of view in degrees, where "wider frame,
same vertical" feels like it must narrow something.

### What #24 actually turns on, which is neither of those runs

X4's `config.xml` has a **`<fov>` tag**. Patola's holds `1.1111`. The injector
already owns every read of that file, so the field of view is a config
override — the non-intrusive lever the project's design mandates — and not a
projection patch. That matters beyond convenience: the shear reads
`M_projection[0][0]` live under `X4VR_PROJ_LIVE`, so a wider field X4 computes
*itself* flows into the stereo for free, while a projection patched behind X4's
back would desynchronise its culling, HUD placement and depth range from what
is drawn.

Added this session, unset by default so every take from 52 to 103 stays
reproducible:

    X4VR_FOV=<decimal>      serve this as <fov> in the profile

Driven end-to-end against the real `libx4vr_inject.so` before any run: valid
decimals pass through verbatim, and `0`, `-1`, `abc`, `1.5x`, `1e0`, `1.2.3`,
`.` and ` 1.5` all fall back to the player's own value. `1e0` is refused on
purpose — `strtod` accepts it and X4's XML reader may not, and a value X4
rejects silently leaves the engine on its default while looking like a
measurement. That is the POM trap, already recorded in `default_overrides()`.

Every log now also carries, override or no override:

    config: effective fov=1.1111 res=1408x1408 (fov from the profile, not this run)

because X4 writes its own settings into the profile as it plays, so a value one
run sets can be served to the next run that does not set it. A run whose log
does not state its own field of view cannot be reproduced from its log — the
same trap `X4VR_PROJ_INVPROJ` sprang on every take before 83.

### The model being tested

From takes 52–66, fitted and recorded earlier in this file as a hypothesis:

    sx = C / max(aspect, 4/3),  sy = sx·aspect,  C = 1.7778 at fov = 1.1111

At a 1:1 eye the clamp binds, so `sx = 0.75·C = 1.3333` — which is what the nine
takes read. The open question is what `C` does when `<fov>` moves, and the
candidates differ in *what* the tag scales:

| law | what `<fov>` scales | `C` at fov=1.5 | `sx` at a 1:1 eye | horizontal field |
|---|---|---|---|---|
| A | the tangent (`C ∝ 1/fov`) | 1.317 | **0.988** | 90.7° |
| B | the tangent, inverted | 2.400 | **1.800** | 58.1° |
| C | the angle (81°·fov at 16:9) | 0.996 | **0.747** | 106.5° |
| — | a floor on horizontal FOV | 1.778 | **1.333** | 73.7° (unchanged) |

### P104 — committed before take 104

1. **`sx` falls below 1.33333.** Raising `<fov>` from 1.1111 to 1.5 at a 1:1 eye
   widens X4's horizontal field. Anything in **0.70…1.05** confirms; that spans
   laws A and C, and distinguishing which is secondary to the direction.
2. **`|sy| = |sx|` still holds** at a 1:1 eye, to within 0.001. This is the
   `sy = sx·aspect` leg of the model tested independently of `C`, and it is the
   check that says the model is the right shape even if the law is wrong.
3. **`near` stays 0.10000.** `<fov>` is a field of view, not a frustum depth.

**If instead `sx` reads 1.33333 unchanged**, the 4:3 clamp is a floor on the
horizontal field itself rather than on the aspect, and #24 cannot be solved from
config alone — the next move is a projection override, and the task gets much
larger. **If `sx` reads above 1.33333**, the tag runs the other way and no
further fov run should be launched until the sign is re-derived from that value.

One ambiguity is known in advance and is not a get-out: X4 may clamp the config
value itself. An `sx` of exactly 1.33333 is consistent both with "the horizontal
field is floored" and with "X4 refused 1.5". They are separated by a second run
at `X4VR_FOV=1.2` — a change small enough to be inside any plausible slider
range. If `sx` moves at 1.2 but not at 1.5, the setting was clamped, not the
field. Patola can also read the FOV slider in X4's own options menu, and the
profile after the run records what X4 kept.

### Take 104 — the run

    X4VR_TAKE=104-FOV X4VR_FOV=1.5 X4VR_DUMP_MATRICES=1 X4VR_STEREO=1
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take104.log
    ./launch/x4vr-launch.sh

No `X4VR_W`/`X4VR_H`, so the eye is 1408×1408 and the clamp is in force — the
aspect an HMD actually has, and the one the ten control takes were measured at.

**No stalling instruments.** `X4VR_MV_PROBE`, `X4VR_MV_DUMP` and
`X4VR_MV_DUMP_PRESENT` are all unset: `X4VR_DUMP_MATRICES` neither waits on the
queue nor reads anything back, so this run should be as smooth as take 100.

Sequence, chosen to match take 57's so the two are comparable line for line:
reach the main menu, load a save, **sit still in the cockpit without touching
zoom** for a few seconds, then one slow zoom all the way in and back out.

The ten takes above are the control; no control run is needed. What X4 computes
from a given `(fov, aspect)` is a property of X4, and nothing in this build
touches its matrices — `X4VR_PROJ_LIVE` only reads them.

**This take is not a known-good candidate.** `X4VR_PROJ_SX=1.3333` is stale by
construction here, so any per-eye offset in it is the wrong size. It is a
measurement, and the stereo in it is expected to be wrong.

### Reading it

    python3 tools/score_run.py /tmp/x4vr-take104.log

and nothing else until that says `split on`. The scorer now reports the proj
block itself, so no hand-picked grep is needed:

    proj  sx=... sy=... near=...
    proj  aspect |sy/sx| = ...  (candidates: X4VR_RES render extent 1.000)
    proj  N change(s), sx range ...  over the near=0.100 camera

Two scorer defects were fixed to make that line trustworthy, both found by
re-scoring logs already on disk:

* It **failed take 103 for not sampling the probe in a run that never enabled
  the probe** — gating on outcome instead of intent, in the one tool whose job
  is to be believed. It now says `swapchain unjudged` and names why.
* The `sx` range blended in samples from a block with a different `near`, which
  take 54 had already shown is not the main camera. Take 54 read as a 30694×
  range; it is 37.8× over the real camera, and the excluded samples are now
  counted and their `near` values printed rather than dropped.

## Take 104 — X4's `<fov>` scales the horizontal field linearly. P104 mostly confirmed

`split on`, `config: fov: '1.1111' -> '1.5'`, `config: effective fov=1.5
res=1408x1408 (fov from X4VR_FOV)`. The knob reached X4.

### Scoring P104 as written

| prediction | outcome |
|---|---|
| `sx` falls below 1.33333 | **confirmed** — 0.69231 |
| specifically in 0.70…1.05 | **missed**, by 1% — 0.69231 is just under the floor I set |
| `near` stays 0.10000 | confirmed — every fov-responsive sample reads 0.10000 |
| `\|sy\| = \|sx\|` at a 1:1 eye | confirmed **only for the camera that ignores fov**; see below |
| which law | **law C, the angle** — but anchored differently than the table assumed |

### The law, exact on two independent pairs

Take 104's `sx` values, compared against takes 57–60 at `fov=1.1111`:

| fov=1.1111 | fov=1.5 | angle at 1:1 | ratio of angles |
|---|---|---|---|
| 1.15174 | 0.69231 | 81.9324° → 110.6095° | **1.35001** |
| 37.75372 | 27.96006 | 3.0345° → 4.0967° | **1.35001** |

`1.5 / 1.1111 = 1.35001`. **X4's `<fov>` multiplies the horizontal field of
view as an angle**, and it does so identically at the wide end and at 33× zoom.
Solving each pair for the fov=1.0 base gives 73.7399° and 73.7397° — the same
number twice, from measurements an order of magnitude apart, and it is exactly
`2·atan(0.75)`, i.e. `sx = 1.33333`.

So, at a 1:1 eye:

    horizontal FOV = fov × 73.7399°        =>    fov = target° / 73.7399

Law C was the right family. The table's arithmetic for it was wrong because it
anchored on "81° horizontal at 16:9" and propagated through the clamp; the
actual anchor is the fov=1.0 base at the aspect being rendered.

### The finding nobody predicted: the 4:3 clamp was fitted to the wrong camera

Three `sx` values appear **bit-identical in both runs**, unmoved by a 35% FOV
change: `1.33333`, `1.00000` (exactly 90°) and `3.78085`. Three others exist
only at their own fov. So X4 runs several cameras at once, and **only some of
them honour the player's field-of-view setting**.

`1.33333` is one of the ones that does not. It is the fov=1.0 base — which means
the clamp model recorded earlier in this file, `sx = 1.7778/max(aspect, 4/3)`,
was fitted to a camera that ignores the FOV setting entirely. The doubt raised
after take 54 — "if that is the menu's projection, the model may describe the
menu and not the game" — was right, and this is the evidence.

It follows that **`X4VR_PROJ_SX=1.3333`, carried on every take since 52, is not
the scene camera's `sx`.** At the player's own `fov=1.1111` the scene camera is
`1.15174`, 16% narrower than the baked constant. This has been harmless only
because `X4VR_PROJ_LIVE=1` makes the shader read `M_projection[0][0]` per draw;
the baked value is a fallback for modules that cannot, and it is wrong for all
of them. Note also `proj SHEAR: baked is 1.000x` in this very log — that check
compares the baked value against the *first* reading, which is the fov=1.0
camera, so it agrees with itself and says nothing about the scene.

The alternation in the log is not a zoom sweep. `1.33333 ↔ 0.69231` flips within
one or two milliseconds, repeatedly:

    623421.318 #25 sx=0.69231   623421.318 #26 sx=1.33333
    623421.338 #27 sx=0.69231   623421.339 #28 sx=1.33333

That is the layer's "most-drawn block wins" heuristic choosing a different
winner between submits of the same frame — the same mis-credit take 54 recorded,
now visible as a steady oscillation because two cameras are busy at once. It is
another argument for the in-shader read, and it means **the number of `proj
CHANGED` lines is not a count of zoom steps.**

### What this settles for #24

The task title says "wider FOV than X4's 4:3 clamp allows". The clamp is real but
it is not the obstacle: it sets the fov=1.0 base, and the setting multiplies it.
A wider field costs one config value and no projection patch.

    fov = 1.356  ->  99.99°  sx=0.83923     fov = 1.437  ->  105.96°  sx=0.75404
    fov = 1.492  ->  110.02°  sx=0.69995     fov = 1.500  ->  110.61°  sx=0.69231  (measured)

1.5 is accepted and produces a real 110.6°, so there is no clamp on the setting
below that. Where the ceiling is, if any, is unmeasured.

### The gap this run could not close, and the fix

Every `proj CHANGED` line reported `sx` alone, so **whether `sy` scales with it
is unmeasurable from take 104's log.** That is not a detail: if the vertical does
not track the horizontal, a widened field is *stretched* rather than wider, and
that is a distortion the eye tolerates for a while and then does not. The
`|sy| = |sx|` confirmation above is from the `proj MEASURED` line, which is the
fov=1.0 camera — the one that did not move.

Fixed in the layer rather than reasoned about: `proj CHANGED` now carries
`sy from -> to`, and `sy` joins the change test, so a run in which only the
vertical moves is no longer invisible. `score_run.py` reports `|sy/sx|` across
every sample and warns when it leaves the eye aspect. Consequence to remember
when comparing takes: change counts from take 105 onward are not comparable with
earlier ones, because the test is now more sensitive.

### P105 — committed before the next run

1. `|sy/sx|` equals the eye aspect (1.000 at a 1:1 eye) on **every** sample,
   including the fov-responsive camera — the field scales without stretching.
2. At `X4VR_FOV=1.437`, the fov-responsive camera reads `sx = 0.75404`
   (105.96°), and the fov-independent values `1.33333`, `1.00000` and `3.78085`
   are unchanged.
3. Prediction 2 is the one that would falsify the linear law: it is an
   interpolation between the two measured points, not an extrapolation, so a
   miss means the law is not linear in between.

## Before take 105 — the scorer reports the `sx` set, and take 104 re-read

Take 104's finding survived by luck. The scorer's line for it was

    proj  83 change(s), sx range 0.69231..27.96006 (40.4x) over the near=0.100 camera

which is a min/max — and the finding was that three values were **bit-identical**
across a 35% FOV change while others moved by exactly the knob's ratio. A range
reads that as "the range shifted" and loses it entirely. I nearly stopped there.

So, before the run rather than after it, `score_run.py` now prints the distinct
`sx` values as a set, each with the field angle it implies and the `<fov>` tag
that angle corresponds to under the law above. On take 104's log, unchanged:

    proj  30 distinct sx (compare this SET against the previous take's, not the range):
          sx=1.33333    73.740° = fov 1.000   x27
          sx=0.69231   110.610° = fov 1.500   x15  <- honours X4VR_FOV
          sx=3.78085    29.630° = fov 0.402   x2
          sx=1.00000    90.000° = fov 1.221   x1
          sx=25.17394    4.550° = fov 0.062   x1
          sx=25.57296    4.479° = fov 0.061   x1
          sx=25.96385    4.411° = fov 0.060   x1
          sx=26.28173    4.358° = fov 0.059   x1
          ... 22 rarer value(s), 22 sample(s), not shown

The `<- honours X4VR_FOV` marker is the acceptance check for #24 in one line: a
camera that obeys the setting lands on the tag the run asked for.

`FOV_BASE_DEG = 73.7399` in the scorer is measured, not assumed, and the "fov"
column is only a label — it is not used to judge anything else.

It does check itself once, though, on a run that never set the knob. Take 54:

    sx=1.15174    81.932° = fov 1.111   x1

`config.xml` reads `<fov>1.1111</fov>`. The column recovers the player's own
setting from a matrix, in a log written 50 takes before anyone knew the tag
existed.

### Two corrections to the take 104 write-up above

**Provenance.** The table cites "takes 57–60" for both rows. That is right for
`1.15174` (it is in takes 53–60 and 66) but wrong for `37.75372`, which appears
in **takes 53 and 54 only** — no other session ever reached the zoom camera.

That value is better than a sample: it is a **stop**, and both runs show it as
one in different ways.

In take 53 it does not ramp at all — it alternates, `37.75372 ↔ 3.78085`,
sixteen times, changes #10 through #24. That is the mis-credit oscillation
again, in a second value pair: the camera **held** `37.75372` exactly while
another interleaved with it. Only from #25 does it drift (`37.72107`). Take 54
does the same from #10.

In take 104 the same camera *eases onto* its stop. The steps shrink
monotonically — 0.209, 0.226, 0.207, 0.183, 0.150, 0.132, 0.115, 0.092, 0.076,
0.049, 0.026, **0.004** — arrive at `27.96006`, and then retreat symmetrically:

    #54 3.78085 -> 26.49146   ...   #66 27.95579 -> 27.96006
    #67 27.96006 -> 27.94545  ...   #79 25.57296 -> 25.17394

An easing curve converging on a limit, not a peak somebody happened to sample.
So pairing `37.75372` with `27.96006` compares two stops, and `27.96006` is
within 7e-5 of the `27.96013` the low-family law predicts for it.

(An earlier draft of this section called take 53's sixteen samples a plateau of
consecutive changes. They are sixteen *returns* to the value, alternating with
`3.78085`. The conclusion is the same and the mechanism is not: reading a count
without looking at what it counted over is the mistake this file exists to stop
repeating.)

**"The number of `proj CHANGED` lines is not a count of zoom steps"** — half
right, and the set above says which half. The 83 changes are two distinct
families:

* `1.33333 ↔ 0.69231`, 27 and 15 samples, alternating within a millisecond.
  Mis-credit, exactly as recorded. Not zoom.
* `25.17394 … 27.96006`, 26 samples, nearly all distinct, easing up to a stop
  and back down. That **is** a real field sweep, on a camera that honours
  `<fov>`.

So X4 does zoom during these runs, the log does track it, and the mis-credit
oscillation is layered on top of it. The claim as written would have had someone
later discard genuine zoom evidence.

### P105, sharpened

`config.xml` still reads `<fov>1.1111</fov>`, so the takes-53/54 baseline is
intact and both predictions below are interpolations from measured endpoints.
Adding to P105 as already committed:

4. The high-family camera's stop reads `sx = 29.18689` (3.9246°). If the run's
   ramp turns around below that, the stop was never reached — untested, not
   refuted. Stated because it is a second interpolation on a **different
   camera**, so it can fail independently of prediction 2. It does not need
   deliberate zooming: in takes 53 and 54 the value was there by change #10.

Both predicted values are reproducible to the five decimals the log prints;
`37.75372` came back bit-identical in two separate sessions, so "close" is not
the standard here.

### Take 105 — the run

    X4VR_TAKE=105-FOV-SY X4VR_FOV=1.437 X4VR_DUMP_MATRICES=1 X4VR_STEREO=1 \
    X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 \
    X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 X4VR_PROJ_LIVE=1 \
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1 \
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take105.log \
    ./launch/x4vr-launch.sh

Identical to take 104 except `X4VR_FOV`, so the two logs' `sx` sets are directly
comparable. An **interaction** take: `X4VR_MV_PROBE`, `X4VR_MV_DUMP` and
`X4VR_MV_DUMP_PRESENT` are all unset, because the probe's `vkQueueWaitIdle`
would make flying and zooming miserable and nothing here needs it.

Reach a real scene — the block needs 50+ draws before anything is measured, so
the splash screen produces nothing — and fly for a few minutes. Predictions 1–3
need the fov-responsive camera to be credited at all, which only happens in
flight; prediction 4's camera showed up on its own in every prior run, so
nothing special is required for it. Take 104 was enough play for all of this.

Judged by `python3 tools/score_run.py /tmp/x4vr-take105.log`. The two lines that
carry the result are the `sy tracks sx` / `warn |sy/sx| ranges …` line and the
distinct-`sx` set.

## Take 105 — the law holds at an interpolated point, and the instrument had two defects

Played across a full session: boost, zoom, the map, landing on a station,
walking around inside it, an NPC conversation, a station elevator, and shooting
a small ship. All of it smooth and in correct SBS at a 106° field. That is the
qualitative result and it is worth stating, but the score comes from the log.

### P105 scored

**1. `|sy/sx|` is the eye aspect on every sample — refuted as written, confirmed
as meant.** The camera that honours the setting reads `|sy/sx| = 1.0000` against
an eye aspect of 1.0000. The field is widened, not stretched. But the run also
credited a camera at `sx = 3.78085` whose `sy` is `-5.67128`, i.e. `|sy/sx|`
exactly 1.5000 — a 3:2 camera, not a stretched eye.

The prediction said "every sample" about a set of samples that spans several
cameras, four paragraphs after establishing that samples span several cameras.
The scorer said what the prediction asked it to say:

    warn  |sy/sx| ranges 1.000..1.500 against 1.000 at first read — the vertical
          does not track the horizontal, so the field is being stretched

That is a false alarm produced by aggregating across cameras — the exact defect
fixed for `sx` in the commit immediately before, still live for `sy`, in a line
added *for this run*. Recorded rather than quietly repaired: knowing the failure
mode is not the same as having removed it, and the gap between the two was one
commit.

**2. Confirmed, to the resolution the log prints.** Predicted `sx = 0.75404` at
`X4VR_FOV=1.437`; measured **0.75405**, 105.964°, and the scorer recovers
`fov 1.437` from it. Of the three fov-independent values, `1.33333` and
`3.78085` came back bit-identical; `1.00000` never appeared — untested, not
refuted.

**3. The linear law survives its interpolation.** Two measured endpoints at
1.1111 and 1.5 and now a confirmed interior point. `fov = target° / 73.7399` can
be used to pick a field.

**4. Untested, and the reason is worse than the miss.** The zoom-stop camera
never appeared. But it could not have: the layer's 400-change budget was
exhausted **182 s into a 382 s session**, so the last 201 s produced no `proj`
samples at all. 199 of those 400 changes came from blocks the scorer then
excludes — one of them a degenerate block whose `near` drifted by 1e-3 a sample,
burning a slot each time. Nothing in the score said the instrument had gone
dark; "the camera did not appear" and "nothing was looked at" read identically.

### Both defects have the same shape as the finding they were measuring

Take 104's finding was *X4 runs several cameras and one number cannot describe
them*. The instrument that found it still kept **one** `last_sx/last_sy/near`
for all cameras. So:

* every flip between two motionless cameras counted as a change — take 104 spent
  42 of its budget on `1.33333 ↔ 0.69231` alone;
* one chatty block could, and did, starve every other camera of budget;
* the aspect check averaged a 1:1 camera and a 3:2 camera and called the result
  a distortion.

Fixed in the layer, keyed on the block the draws actually went through
(`ViewSlot{buffer, offset}`):

* per-camera `sx/sy/near` and change counter, so an alternation between two
  steady cameras emits **nothing**;
* per-camera budget of 120 plus a global backstop of 2000, and both announce
  themselves when they bite;
* cameras are numbered on sight and every line carries `cam#N`; a camera's first
  sighting logs `proj CAMERA cam#N: sx= sy= near= (draws=, |sy/sx|=)`, which for
  a steady camera is the only line it will ever produce — and steady cameras are
  precisely the ones that reveal which cameras ignore `<fov>`;
* `cam#0` keeps the original `proj MEASURED`/`ASSUMED`/`SHEAR` trio, because 71
  logs on disk are `score_run.py`'s regression suite. The `SHEAR` line now says
  in the line itself that it compares against whichever camera drew first.

And in the scorer: the aspect is judged on **one** camera, the one that honours
the setting; `sy` is carried per `sx` rather than folded into the key (pre-105
logs have no `sy`, and keying on the pair split one camera into two rows); and
the layer's cap is reported with the blind window it created.

Consequence, for the second take running: change counts from 106 onward are not
comparable with earlier takes, and this time the reason is large — the
mis-credit oscillation, which was most of the count, no longer appears at all.

### What the log cannot say about the cost

Median frame time at 1408×1408, from `perf frame`:

| take | fov | median of medians |
|---|---|---|
| 100 | 1.1111 | 6.91 ms |
| 104 | 1.5 | 8.01 ms |
| 105 | 1.437 | **12.12 ms** |

Take 105 is the slowest at the *narrower* of the two widened fields. The
ordering is not monotone in `fov`, which is direct evidence that scene content
dominates these numbers — 105 was mostly a station interior, 100 and 104 were
mostly space. **These logs cannot attribute frame time to the field of view.**
Since "performance is king" decides whether a wider field can be the default,
that needs a controlled measurement, not a re-read of what is on disk.

### P106 — committed before the next run

Takes 106 and 107 are one measurement in two halves: the same save, the same
parked viewpoint, no input for 60 s, differing only in `X4VR_FOV`.

1. `proj CHANGED` no longer contains any pair of lines that swap two values back
   and forth within a few milliseconds — the mis-credit oscillation is gone from
   the log, not merely reduced.
2. The budget survives the whole session: no `further changes suppressed` line,
   and therefore no blind window in the score.
3. With the budget intact, the zoom-stop camera appears and reads
   `sx = 29.18689` at `fov = 1.437` (this is P105.4, retried).
4. The parked frame time at `fov = 1.437` is **no more than 1.35× ** the parked
   frame time at `fov = 1.1111`. The field is 1.29× wider in angle and the eye
   extent is unchanged, so anything near 1.0 means the cost is culling-bound
   rather than fill-bound, and anything above 1.35 means widening costs more
   than the geometry it adds.
5. Prediction 4 is the one that decides whether `X4VR_FOV` can ever be defaulted
   on, so it is stated as a threshold before the numbers exist rather than
   described afterwards.

### Takes 106 and 107 — the runs

    X4VR_TAKE=106-FOV-PERF-WIDE X4VR_FOV=1.437 X4VR_DUMP_MATRICES=1 \
    X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 \
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 \
    X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 \
    X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1 \
    X4VR_LOG=/tmp/x4vr-take106.log ./launch/x4vr-launch.sh

    X4VR_TAKE=107-FOV-PERF-BASE X4VR_FOV=1.1111 X4VR_DUMP_MATRICES=1 \
    X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 \
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_PROJ_SX=1.3333 X4VR_MV=1 \
    X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 \
    X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1 \
    X4VR_LOG=/tmp/x4vr-take107.log ./launch/x4vr-launch.sh

`X4VR_FOV=1.1111` is stated explicitly in take 107 rather than left unset. The
profile holds the same value, so the config X4 reads is identical either way,
but the log line then records what the run intended instead of leaving it to be
inferred from a file that can change between takes.

Both are **interaction** takes: `X4VR_MV_PROBE`, `X4VR_MV_DUMP` and
`X4VR_MV_DUMP_PRESENT` stay unset. The probe's `vkQueueWaitIdle` would dominate
exactly the number being measured.

Protocol, and the protocol is the measurement:

1. Load the **same save** in both, fly to the **same spot**, point the camera at
   the **same view** — a busy one, station or traffic, not empty space.
2. Let go of the controls and **do not touch anything for 60 s**. The frame-time
   comparison is only valid if the two runs are drawing the same thing.
3. Then, in take 106 only, zoom all the way in and back out once, for
   prediction 3.
4. Quit.

Judged by `python3 tools/score_run.py` on each log. Predictions 1–3 read off take
106's score. Prediction 4 compares the new `perf quietest … stretch` line
between the two scores: the scorer finds the calmest run of at least 55 s and
prints its median with its spread.

The spread is the check on the protocol, not decoration. Takes 100, 104 and 105
score 22%, 21% and 15% — none of them was parked, and that is what an unparked
minute looks like. **If either run's quietest stretch spreads more than about
10%, the camera was not actually still and prediction 4 is untested rather than
answered.** Read the spread before reading the milliseconds.

## Takes 106 and 107 — three predictions confirmed, and the fourth was unmeasurable by construction

### P106 scored

**1. The mis-credit oscillation is gone. Confirmed.** Not reduced — absent. Every
`proj CHANGED` line in both runs belongs to a monotone ramp on its own slot:

    cam#27 #1: sx 0.75405  -> 29.18689     cam#37 #1: sx 34.08758 -> 34.84346
    cam#27 #2: sx 29.18689 -> 29.17002     cam#38 #1: sx 34.48065 -> 35.16979
    cam#33 #1: sx 29.18689 -> 29.11610     cam#37 #2: sx 34.84346 -> 35.50332

Two slots interleave, but neither flips back and forth. Change counts fell from
399 (take 105, capped) to 61 and 101.

**2. The budget survived. Confirmed.** No suppression line in either log, no
blind window in either score. 61 and 101 changes against a per-slot cap of 120
and a global 2000.

**3. The stop reads `sx = 29.18689`. Confirmed to five decimals**, which is every
decimal the log prints — and better than the prediction asked for, because the
two runs reach it from opposite directions. Take 106 (`fov 1.437`) *starts* at
`29.18689` and eases down; take 107 (`fov 1.1111`) eases *up* and arrives at
`37.75372`, the value takes 53 and 54 recorded before any of this was understood.
One stop, two fields, four sessions, and the ratio of their angles is the ratio
of the settings.

**4. Untested — and for three independent reasons, one of which I built in.**

### The frame-time metric did not discriminate

The quietest-55 s selector returned ~17.3 ms for take 100, take 104, take 106
*and* take 107 — four unrelated sessions agreeing to a tenth of a millisecond,
across a 35% change in the setting under test. That is the tell. A metric that
returns the same number whatever the knob does is not measuring the knob.

Segmenting the same data by phase shows what it had been finding:

    take 106                              take 107
      0–4   s   1.13 ms                     0–2   s   0.93 ms
      4–23  s   2.31 ms                     4–19  s   2.32 ms
     23–93  s  17.31 ms   <- selected      19–91  s  17.38 ms   <- selected
     93–121 s   7.66 ms                    91–111 s   6.92 ms

The calmest long stretch of an X4 session is the **loading and menu phase**, not
the parked camera. The selector found it every time. `score_run.py` now prints
the phases and selects nothing; the last phase is labelled, and only when it is
long enough to be a parked minute at all — takes 100 and 105 end on a single
window, and calling that "the number to compare" is how a 5.65 ms sample of
nothing becomes a result.

### The other two reasons, including the one I designed in

The pre-committed rule was that a spread above ~10% means the camera was not
still. Both last phases spread ~20% — `7.86 → 6.59` and `7.34 → 6.08`, the same
settling curve half a millisecond apart. That is probably not camera motion, but
"probably not" is not something the log can show, so the rule stands.

Worse, the protocol said *"then, in take 106 only, zoom all the way in and back
out"*. The `proj CHANGED` timestamps say that zoom happened at **111 s** — inside
take 106's measured phase (93–121 s). Take 107's zoom came at 115 s, **after** its
last perf window at 111 s. So the one asymmetry deliberately introduced landed
inside the compared window of one run and outside the other's. A perturbation
was written into the protocol and then the phase containing it was compared.

### The provisional number, labelled as provisional

| | last phase | median | windows |
|---|---|---|---|
| take 106, `fov 1.437` | 93–121 s | 7.66 ms | 7 (6.59–7.86) |
| take 107, `fov 1.1111` | 91–111 s | 6.92 ms | 6 (6.08–7.34) |

**1.107×**, against a threshold of 1.35× committed before the runs, for a field
1.29× wider in angle. Excluding the windows the zoom could have touched gives
7.76 vs 7.01 — **1.107×** again, so the confound does not explain it. (After the
zoom take 106 reads 6.59 and 6.61, *lower* than before it, so if anything the
zoom depressed the number rather than inflating it.)

This is the current best estimate and it is **not** a confirmed prediction: the
extractor that produced it was written after the numbers were on screen, and two
of the three objections above are unfixed. It is recorded because it is the only
estimate there is, and because it is nowhere near the threshold that would rule
a wider default out.

To settle it properly: park, **wait 30 s for the frame time to settle, then hold
60 s untouched**, and put any zoom *before* the parked window, in both runs
equally.

### cam#N is a slot, not a camera

Take 106 logged 36 `proj CAMERA` lines, 25 of them reading `sx=1.33333`; take 107
logged 41 with 29. X4 multi-buffers one projection across many descriptor slots,
so:

* the per-slot fix was still the right one — it is what removed the oscillation;
* but a projection that moves logs its change **once per slot it occupies**, so
  a change count carries a buffering factor of roughly 25;
* the per-slot budget of 120 is therefore ~3000 for a multi-buffered projection,
  which is why nothing came near the cap this time;
* `cam#N` must not be read as a camera identity. The distinct-`sx` set is the
  object to reason about. The scorer now says so on its own line: *"36 slot(s)
  carried 6 distinct projection(s) — cam#N is a UBO slot, not a camera"*.

### Where #24 stands

Measured and confirmed: the law (`fov = target° / 73.7399`), the value at three
points, `|sy/sx|` on the camera the player looks through, and the zoom stop at
two fields. Played: a full session at 106° — station interior, elevator, NPC,
combat — smooth and in correct SBS.

Open: a default, which waits on a cost number that survives its own protocol.

## Task #25 — what comfort is a function of, and what #24 just did to it

`make_eye_shear` puts `x_ndc' = x_ndc - sx·(ipd/2)/z` per eye. Two consequences,
and the second is the one that matters:

1. The **pixel** offset is `(W/2)·sx·(ipd/2)/z`. It depends on the field.
2. The **angle** between the two eyes' images of a point is `ipd/z` — `sx`
   cancels. It does not depend on the field.

So a wider field does not change what the stereo asks the eyes to do. It changes
only how many pixels that request occupies. The `~30/z px` recorded earlier in
this file was computed at `sx = 1.3333`, which is neither the scene camera nor
the current setting:

| | sx | per-eye px at 1 m | at 10 m |
|---|---|---|---|
| the baked constant (fov 1.0) | 1.33333 | 30.0 | 3.0 |
| profile, fov 1.1111 | 1.15174 | 25.9 | 2.6 |
| fov 1.437 | 0.75405 | 17.0 | 1.7 |

Vergence, from `ipd/z` at `ipd = 0.064`: total disparity passes **1° at 3.67 m**
and **2° at 1.83 m**, whatever the field or the resolution. Those are the angles
a real object at those distances subtends, which is why they are comfortable in
the world — vergence and accommodation agree there. On a fixed-focus display
they do not, and that disagreement is the whole of task #25.

### What #24 did to #23, and therefore to #25

The world modules with no camera block cannot read `sx` per draw and fall back to
the baked `X4VR_PROJ_SX`. Against the scene camera:

    fov 1.1111    1.3333 / 1.15174 = 1.16x too much separation
    fov 1.437     1.3333 / 0.75405 = 1.77x too much separation

**Widening the field multiplied #23's defect by 1.53.** Disparity 1.77× too large
places that geometry at `1/1.77 = 0.57×` its true distance — conspicuously too
close, in a subset of world geometry while everything around it is correct. That
is not a cosmetic staleness; it is a depth conflict inside one frame, which is
the exact class of thing #25 exists to remove.

A mitigation is available today with no code change: `X4VR_PROJ_SX` is a knob and
the correct value for `fov 1.437` is **0.75405**, measured three times now. That
takes those modules from 1.77× to 1.00× at the scene camera. It leaves them wrong
under zoom, which is #23 proper and unaffected.

### The scorer prices the stereo now

Every run reports the scale it actually produced, from its own numbers, against
the camera that honours the setting — not against `cam#0`, which is whichever
camera drew first and is how `1.3333` survived fifty takes:

    stereo  ipd=0.064 m on the fov camera (sx=0.75405, 1408px eye): per-eye
            offset 17.0px at 1m, 3.4px at 5m, 1.7px at 10m, 0.6px at 30m
    stereo  total disparity passes 1° at 3.67 m and 2° at 1.83 m — closer than
            that is a vergence load, and it is FOV-independent
    stereo  X4VR_PROJ_SX=1.3333 against a scene sx of 0.75405: the 12 module(s)
            that cannot read sx live separate 1.77x too much (task #23)

It refuses to report when no camera honoured the setting, rather than pricing the
stereo off whichever camera happened to draw first.

### What a log cannot settle

Whether it *looks* right, and at what IPD it stops being tiring. That needs the
frames viewed as stereo, and the answer depends on how they are viewed — an HMD,
a cross-eyed pair, or an SBS-capable display all magnify differently, and the
angular argument above only holds when the displayed field matches the rendered
one. That is #31's question arriving inside #25, and it is recorded here as open
rather than assumed away.

### P108 — committed before the next run

Take 108 is take 106 with the baked constant corrected to the value take 106
itself measured.

1. `stereo  X4VR_PROJ_SX=0.75405 against a scene sx of 0.75405: … matches the
   scene camera`.
2. The `baked-sx=` count is unchanged for a comparable point in the session. The
   knob changes the value those modules bake, not how many of them there are.
3. **Nothing else in the score moves**: same `split`, same `masked
   fullscreen=36`, same `|sy/sx|`, same fov camera at `0.75405`, same distinct-sx
   set modulo where the session went. This is the real prediction — the constant
   is consumed by 12–20 of ~430 world modules, so anything else moving means it
   reaches further than the code says it does.
4. To Patola's eye: geometry that sat conspicuously nearer than its
   surroundings should now sit with them. If nothing looked wrong before, that is
   information too — it would mean the affected modules draw nothing prominent,
   which is what takes 85 and 86 suggested and never confirmed.

### Take 108 — the run

    X4VR_TAKE=108-BAKED-SX X4VR_FOV=1.437 X4VR_PROJ_SX=0.75405 \
    X4VR_DUMP_MATRICES=1 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1 \
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1 \
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1 \
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take108.log \
    ./launch/x4vr-launch.sh

An interaction take — no probe, no dumps. Fly somewhere with close geometry: a
station exterior, a docking approach, or a cockpit view, where 1 m to 10 m is
actually on screen. Deep space cannot show this: at 30 m the whole disagreement
is under a pixel.

Judged by `python3 tools/score_run.py /tmp/x4vr-take108.log`, on the three
`stereo` lines and on everything above them being unchanged from take 106.

## Take 108 — the constant is correct, and the budget proved itself under load

Flown close to a station and circled it for a couple of minutes: the one scene
where 1 m to 10 m is actually on screen, which is what this run needed.

### P108 scored

**1. Confirmed, exact text.**

    stereo  X4VR_PROJ_SX=0.75405 against a scene sx of 0.75405: the 12 module(s)
            that cannot read sx live matches the scene camera (task #23)

**2. Confirmed.** `baked-sx=12` at module #350, the same count take 106 reached.
The knob changed the value those modules bake, not how many of them there are.

**3. Confirmed.** `split on`, `masked fullscreen=36`, `sx=1.33333 sy=-1.33333
near=0.10000` at first read, `|sy/sx| = 1.000`, the extent attribution, the fov
camera at `0.75405` with `|sy/sx|=1.0000`, and 6 distinct projections across the
slots — every one identical to take 106. The distinct-`sx` *set* is 4 rather than
24 because this session did not zoom, which the prediction allowed for
explicitly. The four that remain are the four persistent projections:
`0.75405` (scene), `1.33333` (fov-independent base), `3.78085` (the 3:2 camera)
and `1.00000` (the 90° one, absent in take 105 and back here).

**4. Not evidence, by Patola's own account.** It looked right in the SBS pair,
and he is right that this cannot be judged from a side-by-side pair on a flat
screen — depth and scale are the whole question and they are not visible that
way. Recorded as unjudged rather than as a pass.

### The per-camera budget did the thing it was built for

Take 108 produced **314 changes to take 106's 61**, 247 of them from blocks with
a different `near` — 74 distinct values including a monotone negative ramp
(`-1.212, -1.213, -1.214, -1.215, -1.216`). That is the same drifting-block
pattern that exhausted take 105's single global budget 182 s into a 382 s
session.

This time two slots hit their own 120-change caps and **every other camera kept
logging**:

    proj: cam#44 reached 120 changes -- further changes from THIS camera
          suppressed, the others continue
    proj: cam#43 reached 120 changes -- ...

Under the old global budget those two slots alone would have consumed 60% of it.
The score now says so rather than staying quiet about it:

    note  2 slot(s) hit the per-camera change cap (cam#44, cam#43) — contained
          to those slots, the rest kept logging

### #25 is blocked, and the blocker is not in this repository

The machine already has a full Linux VR stack: **WiVRn** (`wivrn-server`,
`wivrn-dashboard`, a Monado-based runtime at
`/usr/share/openxr/1/openxr_wivrn.json`), **envision**, **wlx-overlay-s**, and
SteamVR. No `active_runtime.json` is selected in either the user or the system
path, so nothing claims the runtime at rest.

`wlx-overlay-s` shows Wayland/X11 windows inside VR, but it has no side-by-side
or stereo mode — its `--help` and its strings carry nothing of the sort. So
mirroring X4's SBS window into the headset would put **a flat panel** in front of
the eyes: both eyes see the same squashed pair, which is worse than the monitor,
not better. It cannot answer #25.

What #25 needs is the two array layers the layer already produces, handed to
OpenXR as a projection layer with one view each. Everything upstream of that
exists — the 2-layer eye image, per-eye constants via `gl_ViewIndex`, the shear,
and now a correct `sx` at a chosen field. What does not exist is the submission:
an `XrSession` bound to *X4's* `VkDevice` and queue rather than one of our own,
an `XrSwapchain` to blit the eye image into, and a frame loop driven from
`vkQueuePresentKHR` instead of from our own main loop.

That is a new task, not a step of #25, and it is the largest remaining piece.
Head tracking (#33) sits on top of it; a first cut can submit with the runtime's
view FOVs while ignoring head pose, which is enough to sense depth and scale
standing still and not enough to move around in.

## Task #32 — the right eye, judged from the dumps. Answered from data already on disk

585 present-dump frames were already in `/tmp`, across seven takes — 253 from
take 97 alone. I nearly specified a run to produce them: the `ls` that would have
shown them was truncated by a `head -30`, and the six frames I did see were
`img103` end-of-pass dumps, a different instrument. The sweep rule this file
already carries covers this exactly, and it was applied and then abandoned
halfway.

    /tmp/x4vr-t93   19   /tmp/x4vr-t97  253   /tmp/x4vr-t101  40
    /tmp/x4vr-t94   16   /tmp/x4vr-t98  227   /tmp/x4vr-t103  20
    /tmp/x4vr-t99   10

### The metric, and why not the old one

The check being replaced asked whether layer 1 held roughly as much light as
layer 0. Takes 41–44 showed that shape of metric cannot tell correct stereo from
broken — a bit-exact copy of layer 0 scores a perfect 1.00, and so does a correct
right eye. It was never measuring stereo.

What separates them is not *how much* the images differ but **how the difference
is distributed**:

| | per-region alignment |
|---|---|
| bit-exact copy | every region at shift 0, r = 1.000 |
| uniform translation | every region at the same nonzero shift |
| true stereo | regions at **different** shifts, because the shear displaces by `sx·(ipd/2)/z` and `z` varies across a frame |

So the number is the **spread** of per-tile shifts, not their mean. A single
global shift, however large, is a slid image — the failure `make_eye_shear`'s own
comment warns about: *a plain clip-space translation would slide the whole image
uniformly and produce no depth cue at all*.

### Two things the controls caught before any conclusion did

`tests/eye_stereo_selftest.py` slides a real X4 frame by known amounts and checks
the recovered value, not its magnitude. Both findings below came from that, not
from reading the output and being surprised.

**The sign was inverted.** The first window was `b[m-s : w-m-s]`, which returns
every displacement negated. Magnitudes were exact, so a magnitude-only test would
have passed — and the tool would have reported the eyes swapped, forever, in a
project that has already spent takes on which eye is which.

**Saturation reads as depth.** Take 97's first pass reported 31 frames at
`shift -40..+40, spread 80` — 0.75 m of parallax, in deep space, on frames of
mean luma 9. Tiles pinned at *both* ends of the search window are the correlator
finding nothing and returning its boundary. A number meaning "I failed" was being
printed in the same units as the answer. Tiles at ±`MAX_SHIFT`, and tiles
matching below `r = 0.90`, now do not vote, and both counts are printed rather
than filtered away. A fourth control was added: two unrelated images must yield
**no** confident tile, which the first version would have failed.

### What the dumps say

**Take 103 recovers a number this file derived a different way.** The start menu
holds one object at about 20 m and everything else at infinity, and the note
recorded for it was `disparity_px · z = 60.07`. The tool, built from the shear
equation and validated on synthetic slides, reads the real pixels as:

    eye  frame 7: 58 matched tile(s), shift -3..+0 px (spread 3), r=0.9999,
         luma 73.8, nearest ~20.0 m

Two derivations, one number, no shared arithmetic.

**Takes 97 and 98 hold real near-field stereo.** 87 and 75 frames with
depth-varying parallax, down to `-39 px` — about 1.5 m, which is cockpit
distance:

    eye  253 present dump(s): 86 bit-identical, 80 measured with no parallax,
         87 with parallax, 0 with nothing alignable

**The eye order is right, over 1800 tiles.** A nearer object must sit *left* in
the right eye. Take 97: all 443 displaced tiles move left. Take 101: all 130.
Take 103: all 88. Take 98: 1171 left and 2 right — those two are +3 px in frames
whose other tiles read −30, which is correlation noise on a repeating texture,
and it is reported as the 0.2% it is rather than as a sign error. The threshold
for calling it a real inversion is 5% of displaced tiles, set because an extremum
must not decide a verdict about the whole pipeline.

### Two takes change verdict

**Takes 97 and 98 go FAIL → PASS.** They were failing on *"the probe never
sampled a swapchain image"* — the probe walks the frame one image at a time,
roughly one every 30 s, and in those runs it never got as far as #50–#53. The
dumps answer the same question directly and say the eye image is correct stereo.

Not waved away, though: the two instruments watch **different links**. The dumps
prove the *eye image* is stereo; the probe would have proved the copy from it
into the swapchain kept both layers. So the run now prints

    warn  the probe never reached a swapchain image (#50-#53), so the
          eye-image → swapchain copy is unverified in this run.

which is what is actually true, rather than either a pass or a failure.

Every other verdict across the 74 logs is unchanged. Scoring a dump-heavy log is
now slow — 25 s for take 97's 253 frames — and that is per take, not per run.

## Task #31 — the three extents, and the check that nearly failed every good run

### The launcher half was already fixed

Take 101 ran gamescope at 2816×792 while X4 rendered 1408×1408, because the
launcher re-read `sbs_dim()` instead of the `W`/`H` it had just honoured. That is
fixed: `X4VR_RES` now derives from `$W`/`$H`, and take 103 shows it working —
`X4VR_W=2816 X4VR_H=792` produced `X4VR_RES=1408x792`, and the warning that had
appeared in every log since take 41 stops at exactly that run.

    logs carrying "WARNING swapchain": 69 of 74, and none after take 102.

### Two copies of the eye size were still live

`expected_eye()` was added after take 102 to make `X4VR_RES` the single source.
Two places never got converted:

* **The `SPLIT OFF` message** printed `X4VR_SBS_WIDTH/2, X4VR_SBS_HEIGHT` while
  the test three lines above it used `expected_eye()`. With `X4VR_RES` set to
  anything else the line can read *"X4 asked for 1408x792 but one eye is
  1408x1408"* when the compared values were equal, or declare `SPLIT OFF` while
  printing two identical numbers. An error message that contradicts itself sends
  the diagnosis somewhere else entirely, which is what this task is a list of.
* **`X4VR_FAKE_EXTENT=1`** offered the compiled constant as the surface's
  `currentExtent`, ignoring `X4VR_RES`. Latent, because the knob is off by
  default — but the `SPLIT OFF` message above **recommends that exact knob** as
  the remedy, so anyone following the advice with a non-default `X4VR_RES` would
  have been handed the wrong extent by the fix.

Both now read `expected_eye()`.

### One line instead of three to compare

    extents: X4 renders 1408x1408, this run's eye is 1408x1408 (from X4VR_RES),
             the composite presents 2816x1408 -- they agree

Take 101's three numbers were 1408×1408, 1408×1408 and 2816×1408 against a
2816×792 window, and **every component was behaving exactly as written**. That is
why nothing reported it: there was no single place where the disagreement
existed. Now there is.

### The check I nearly shipped would have failed 60 of the 74 logs

The first version scored the old `WARNING swapchain is AxB, expected CxD` line as
a defect. Re-running the scorer over every log — which is the only reason this
was caught — failed **60 runs, including every tagged known-good state**.

Take 60's line reads:

    WARNING swapchain is 2816x1408, expected 2816x1408

A warning whose two numbers are equal. In those builds the check compared against
one value and printed another (the compiled constant), which the layer's own
comment already records. What it actually compared cannot be recovered from the
log, so it cannot be scored on — and treating it as evidence was reading an
instrument without checking what it measured, for the third time in this file.

It is now a `note` that says the line predates the fix and is not being judged.

### What is judged instead, and it works on every build

The eye the run asked for (`X4VR_RES`, or `common/x4vr_sbs.hpp` when unset)
against the window X4 was actually given (`SDL_GetWindowSize`). Both are in the
log regardless of which layer build wrote it, and neither is a message whose
meaning has changed. Validated offline across all 74 logs **before** being
enabled: it fires on take 101 and on nothing else.

    FAIL  the eye is 1408x1408 (from X4VR_RES) but X4's window is 2816x792 — a
          side-by-side frame of 2816x1408 does not fit it, so the two are
          different rectangles and anything measured about aspect is about
          neither (take 101)

The scorer reads the default from `common/x4vr_sbs.hpp` rather than copying it,
for the same reason the launcher does: a fourth copy of the SBS size is how three
extents became four.

### Take 101 changes verdict: PASS → FAIL

It was passing. Its aspect measurement was already recorded in this file as
having measured nothing, and now the scorer says so too. With takes 97 and 98
going the other way for #32, this session moves three verdicts, all deliberate,
all recorded here.

### Deferred by decision, not blocked: #25

Patola's call, and it is the right sequencing: the IPD and the sense of depth and
scale get judged when **true SBS** (done), **head tracking** (#33) and **a VR
projection path** are all in place. Until then it stays adjustable and is not a
blocker on anything. `active_runtime.json` is absent from this machine at rest
because SteamVR (Steam Link) and WiVRn each install it when they start — both
paths are live, neither claims the runtime idle.

So the earlier note that "#25 is blocked" was the wrong frame. It is scheduled.

### What take 109 has to show

Nothing needs a run to *fix*, so this rides along with whatever runs next:

1. `extents: … -- they agree`, with X4's render, the eye and the composite all
   from one number.
2. **No** `WARNING swapchain` line. Takes 103–108 already have none, so its
   return would mean this change broke something rather than that X4 did.

## Task #23 — the twelve modules, what they are, and where sx was hiding

### Identified with the layer's own code, not a reimplementation

`x4vr_test_spirv_patch classify` and `vert-eye-offset` over a dumped module set
give the exact list the layer would produce, because they *are* the layer's
`classify()` and `patch_vertex_eye_offset()`. Over take 74's 397 modules:

    348 World vertex modules, 12 refused for having no camera block:
    25 26 133 134 145 146 157 158 222 223 226 227

First attempt used `/tmp/x4vr-shaders` (409 modules), which has **no matching
log** — and module serials are per-run, so nothing in it can be correlated to a
render pass. Redone against take 74, which has both.

### They are combined vertex+fragment modules with exactly one descriptor

    OpEntryPoint Vertex   %main "main" ... %SPECIAL_VERTEXLOCATION_POSITION
    OpEntryPoint Fragment %main_0 "main" %OUT_RT0
    OpDecorate %_ DescriptorSet 3
    OpDecorate %_ Binding 0
    OpMemberName %BLOCK_BUFFER_BINDING_SLOT_WORLD 0 "M_worldviewprojection"

One Uniform block, set 3 binding 0, the per-object block. **No camera block in
either stage** — so this is not the combined-module aliasing trap that
`classify(wide_camera)` exists for. They genuinely cannot see the camera.

### Only four of them draw anywhere that is sheared

Correlating each against take 74's `mv final: (present )?rp #N <- frag module #M`
lines, and the pass classification:

| modules | passes | classified |
|---|---|---|
| 26, 134, 146, 158 | rp #13, #16, #23 | **STEREO (world)** — these matter |
| 223, 227 | rp #34, #36, #38, #40, #42 | MONO (depth-only/shadow) — never sheared |
| 25, 133, 145, 157, 222, 226 | none | never bound in this session |

The pairs are consistent: X4 compiles two variants of each and binds the second.
That is the same late-selected-variant shape recorded for the hull/menu-quad
module. Six of the twelve are the variant that never runs, two run only in the
shadow cascades the layer already excludes, and **four draw real geometry into
the G-buffer and the lighting passes with a baked `sx`**.

### sx is not stored where they can see it — but it is recoverable

The block carries `M_worldviewprojection`, `M_world`, `M_prevworldviewprojection`,
five shadow matrices and some scalars. No projection, no view, no `sx`.

X4's projection, read from take 108's dump and already modelled in
`tests/view_math.cpp`:

    row0(P) = [sx  0  0  0]      x_c = sx*x_v
    row3(P) = [ 0  0  1  0]      w_c = z_v
    row2(P) = [ 0  0  0  near]   z_c = near, constant -- m[10] is 0

Therefore, for `MVP = P·V·W`:

    row0(MVP) = sx * row0(VW)
    row3(MVP) =      row2(VW)

and if the view is rigid and the object matrix is rigid times a **uniform** scale
`s`, both those rows have linear part of magnitude `s`:

    |row0(MVP).xyz| / |row3(MVP).xyz| = (sx*s)/s = sx        exactly

The object's own scale cancels. One divide and one square root per vertex, from a
block the module already reads — **no new descriptor, no push constant, no
pipeline-layout change**, which is what makes this viable at all: adding a push
constant range would alter layout compatibility for pipelines the layer does not
own.

### Validated before any shader was touched

`tests/view_math.cpp` now checks the recovery against every `sx` this project has
measured — 1.33333, 1.15174, 0.83923, 0.75405, 0.69231, 3.78085, 29.18689,
37.75372 — with an identity view *and* a rigid non-identity one, at object scales
of 0.01, 0.25, 1, 7.5, 120 and 3000:

    worst relative error 2.37e-07

Two more cases pin the edges rather than leaving them as prose. A **2× non-uniform
x scale** reports `2·sx`, so the assumption is recorded as the exact factor it
costs when it fails, not as "rare in practice". A **zero matrix** — what an
unpopulated block reads as — recovers `0`, not a NaN and not a plausible number.

### What this buys beyond #23

`X4VR_PROJ_SX` and `X4VR_FOV` currently have to be kept in step by hand; the
`stage5-wide-field` entry says so in as many words. Every module that recovers
`sx` per draw stops consulting the baked constant at all, so the pair that must
be kept in sync shrinks toward nothing.

### The transform, and what it was checked against

`patch_vertex_eye_offset_mvp` in `common/x4vr_spirv.hpp`, a sibling of
`patch_vertex_eye_offset` rather than a refactor of it — the same reason that
one duplicates `patch_vertex_clip`'s scan: those two are proven in the field,
this one is new, and a refactor breaking all three at once is the expensive
mistake. It emits, before every `OpReturn` of the **vertex** entry point (keyed
on the function id, because these are combined vertex+fragment modules and
"inside any function" would patch the fragment stage too):

    n0 = sum of squares of (M[0][0], M[1][0], M[2][0])     row 0
    n3 = sum of squares of (M[0][3], M[1][3], M[2][3])     row 3
    ok = n3 > 0
    sx = ok ? sqrt(n0 / (ok ? n3 : 1.0)) : 0.0
    d  = d_left + float(gl_ViewIndex) * (d_right - d_left)
    gl_Position.x -= sx * d

The guarded denominator is not decoration: X4's blocks read as all zeroes before
the first real frame, and an unguarded divide would put a NaN into
`gl_Position.x` and take the whole vertex with it. The expression is written to
match `tests/view_math.cpp`'s host-side `recover()` exactly, including the
zero case, so the two cannot drift.

`OpSelect` here is scalar-condition, scalar-result, which is core SPIR-V 1.0 —
`patch_vertex_clip`'s note about avoiding it concerns the *vector*-result form
that only relaxed in 1.4. `GLSL.std.450` is matched by name rather than assumed,
and the transform **refuses** when it is absent: emitting an `OpExtInst` against
the wrong set produces a module that validates and computes something else.

Checked over take 74's whole corpus, not just the four modules that motivated it:

    patched=328  refused=69  spirv-val failures=0  refusals that modified code=0

Plus the paths a corpus sweep does not reach: the mono form (no `d_right`)
validates; a wrong set refuses and leaves the module **byte-identical**; a member
that is not a mat4 refuses. Disassembly of the patched `mod-0026` confirms the
access chains index (member 0, column c, row r) as intended.

### Wired in behind a knob that defaults off

`X4VR_PROJ_MVP=1`, and only as the fallback where `patch_vertex_eye_offset` has
already refused — it never runs on a module that can see the camera. The gate
short-circuits, so **unset, the patch is not called at all** and every module
takes exactly the path `stage5-wide-field` was tagged on. That is the same
introduction `X4VR_PROJ_LIVE` and `X4VR_SHEAR_LIGHTS` got, and the reason is the
project's own history: a known-good state has to stay one unset variable away.

The `patched vertex shader` line now carries three counts instead of two —
`live-sx=`, `mvp-sx=`, `baked-sx=` — so which path each module took is a number
rather than an inference.

### P109 — committed before the next run

1. With `X4VR_PROJ_MVP` **unset**, every line of the score is identical to take
   108's, and `mvp-sx=0`. This is the control, and it is the prediction that
   matters most: a knob that changes something while off is the failure this
   project has spent the most takes on.
2. With `X4VR_PROJ_MVP=1`, `mvp-sx=` reads what `baked-sx=` read in take 108 at
   the same point in the session (12 at module #350), and `baked-sx=` falls to
   **0** — every World module now gets a per-draw `sx` from one source or the
   other.
3. No new `WARNING: driver rejected patched module` line. 328 of 397 passed
   `spirv-val` offline, but the driver is the one that has to compile them.
4. Under zoom, nothing separates from its surroundings. This is the defect the
   task exists to remove and the only part of it Patola can see: at the zoom
   stop the baked constant is wrong by 29.18689/0.75405 = 38.7x, so geometry
   drawn by these modules sat at 1/38.7 of its true distance.

Take 109 is take 108's command line with `X4VR_PROJ_MVP=1` added, and take 108
itself is the control that has already been run.

### P109 corrected before the run: take 108 is not a control for this build

P109.1 said "with `X4VR_PROJ_MVP` unset, every line of the score is identical to
take 108's". That cannot hold, and noticing it now rather than after the run is
the point of writing predictions down. The layer has changed since take 108 in
ways that alter the log whatever this knob does:

* the `extents:` line is new (task #31) — take 108's log has no such line;
* the `patched vertex shader` line gained a third count, so its format differs
  from the one take 108 wrote.

So the control has to be **run on this build**, not read off an older log. Two
takes, same scenario, differing in one variable:

    take 109   X4VR_PROJ_MVP unset   the control
    take 110   X4VR_PROJ_MVP=1       the change

P109 restated against that pair:

1. Take 109 reproduces take 108's *judgements* — `split on`, `masked
   fullscreen=36`, the same four-projection `sx` set, `|sy/sx|=1.0000` on the
   fov camera, `stereo … matches the scene camera` — plus an `extents: … they
   agree` line, and `mvp-sx=0`. Not a byte comparison; the two builds do not
   write the same lines.
2. Take 110 reads `mvp-sx=` equal to what take 109's `baked-sx=` read at the
   same module index, and `baked-sx=0`. Every World module then gets a per-draw
   `sx` from one source or the other.
3. Neither run logs `WARNING: driver rejected patched module`. 328 of 397
   modules passed `spirv-val` offline; the driver is what actually compiles
   them, and this is the first time it sees this transform.
4. Take 110's `perf` phases are within noise of take 109's. The added work is
   six loads, six multiplies, four adds, a divide and a square root, on the
   handful of modules that take the fallback — it should be unmeasurable, and
   "performance is king" means saying so in advance rather than after.
5. To Patola's eye, under zoom: nothing separates from its surroundings in take
   110 that did not in 109. At the zoom stop the baked constant is wrong by
   29.18689/0.75405 = **38.7x**, which places geometry these modules draw at
   1/38.7 of its true distance. Recorded as his observation, not as the score.

The comparison only works if both runs draw the same things — the module set X4
compiles depends on what has been on screen, and `baked-sx=` counts modules. So
the scenario below is one routine, run twice.

## Takes 109 and 110 — every module moved, and the flatscreen cannot say more

### P109 scored

**1. Control confirmed.** Take 109 reproduces take 108's judgements — `split on`,
`masked fullscreen=36`, `|sy/sx|=1.0000` on the fov camera, `stereo … matches the
scene camera` — and adds `extents: X4 renders 1408x1408, eye 1408x1408 (from
X4VR_RES), composite 2816x1408, window 2816x1408`, all four agreeing. `mvp-sx=0`.
The distinct-`sx` set is 306 rather than take 108's 4 because this pair zoomed
twice; that is the ramp, and it is the same reason take 106 differed from 108.

**2. Confirmed exactly, and better than the prediction asked.** At every
checkpoint the two runs are identical except for the one split:

    take 109  #400 [world=338 nonworld=62 stereo=338 live-sx=326 mvp-sx=0  baked-sx=12]
    take 110  #400 [world=338 nonworld=62 stereo=338 live-sx=326 mvp-sx=12 baked-sx=0]

`live-sx=326` unchanged is the part worth reading twice: the new patch never ran
on a module that could already see the camera. All 12 moved, none was left
behind, and nothing else was touched. The scorer now says the consequence
plainly:

    stereo  X4VR_PROJ_SX=0.75405 is unused — no module fell back to it, every
            one reads sx per draw (task #23)

**3. Confirmed.** Zero `driver rejected patched module` lines in either run. The
driver compiled all twelve. `spirv-val` accepting 328 of 397 offline predicted
this but could not establish it.

**4. Confirmed.** Aligned gameplay phases: 6.95 vs 7.02 ms, 12.29 vs 12.22 ms;
session medians 9.99 vs 9.68 ms. The signs disagree and the magnitudes are about
1%, which is noise. The added work — six loads, six multiplies, four adds, a
divide and a square root on twelve modules — is unmeasurable, as predicted before
the numbers existed.

**5. Unjudgeable on this display, and the arithmetic says why.** Patola could not
tell the two runs apart, and noted that IPD differences are very hard to
distinguish on a flatscreen.

That is not a null result, and it must not be recorded as one. What the patch
changes is **disparity**, at the zoom stop:

    per-eye offset      50 m      200 m     1000 m
    baked 0.75405       0.34 px   0.08 px   0.02 px
    true  29.18689     13.15 px   3.29 px   0.66 px

A 13-pixel disparity change at 50 m is a large depth cue and **no monoscopic
difference at all** — a side-by-side pair on a flat monitor carries no depth, so
the one thing that changed is the one thing the display cannot show. The
observation is consistent with the fix working and equally consistent with those
modules drawing nothing prominent; this run cannot separate those, and neither
could any run on this screen.

It is weak evidence in one direction: a *wrong* recovered `sx` would have shifted
that geometry by up to 13 px per eye horizontally, visible as objects sliding out
of place inside each half. Nothing of the sort was seen, so the patch is at least
not grossly wrong.

Settling it properly costs a measurement pair — takes with
`X4VR_MV_DUMP_PRESENT` at high zoom, read through `tools/eye_stereo.py`, whose
per-tile shift is exactly this quantity. Worth doing when something else needs a
dump run; not worth a dedicated pair now, since the mechanism is confirmed and
the appearance belongs to #25, which is deferred until VR by decision.

### What this settles

`X4VR_PROJ_SX` is now consulted by **nothing** when `X4VR_PROJ_MVP=1`. The
`stage5-wide-field` entry warns that the constant and `X4VR_FOV` have to be kept
in step by hand, and that requirement disappears: every World module derives `sx`
per draw, from the camera block or from the object's own matrix.

## Task #34 — head tracking starts with the session, and the shear cannot carry a head

"Next is head tracking" is right about the goal and wrong about the first
step. There is no pose without an `XrSession`, and a session cannot be created
after the fact: the runtime decides which Vulkan **instance** extensions, which
**device** extensions and which **`VkPhysicalDevice`** the session may use, and
it decides them before the device exists. So the `XrInstance` has to be created
inside our `vkCreateInstance` hook, ahead of the down-chain create.

`XR_KHR_vulkan_enable2` is built for exactly this shape.
`xrCreateVulkanInstanceKHR` takes the *application's own* `VkInstanceCreateInfo`
plus a `pfnGetInstanceProcAddr`, merges in what the runtime needs, and calls
through. In a layer, "the application's create info" is X4's and "call through"
is the next layer down — so X4 still creates its own instance, with the
runtime's additions folded in, and never learns that anything happened. That is
the non-intrusive shape this project asks for, and it is why the v1 extension
(`XR_KHR_vulkan_enable`) is not an acceptable fallback: it hands back a list of
names and then creates the device *itself*, which from inside a layer chain
means reproducing the runtime's device setup by hand.

### What a head rotation does to the clip transform

Worth doing the algebra rather than assuming, because the answer is not the one
the eye offset trained us to expect. X4's projection, column-major, from take
108's dump:

    row0(P) = [sx  0  0    0 ]
    row1(P) = [0   sy 0    0 ]
    row2(P) = [0   0  0    near]
    row3(P) = [0   0  1    0 ]

so `P·(x,y,z,1) = (sx·x, sy·y, near, z)`, and inverting it,

    P⁻¹·(a,b,c,d) = (a/sx, b/sy, d, c/near)

**Eye offset** `d_x`, a pure view-space translation, through `K = P·T(−d)·P⁻¹`:

    x_c' = a − sx·d_x·(c/near)        y_c' = b
    z_c' = c                          w_c' = d          ← unchanged

`w` survives untouched, and X4's `c` is always exactly `near`, so `c/near = 1`
and the whole 4×4 collapses to `x_c' = x_c − sx·d_x`. That collapse is the
reason stereo costs nothing per vertex, and it is the thing the patched shaders
implement.

**Head yaw** θ about the up axis, `K = P·R_y·P⁻¹`:

    x_c' = a·cosθ + sx·d·sinθ         y_c' = b
    z_c' = c                          w_c' = −(a/sx)·sinθ + d·cosθ   ← changes

`w_c` moves. The perspective divide is no longer the one X4 computed, so
nothing collapses: yaw is a genuine clip-space 4×4, per vertex, and `K` must
stop being a baked constant. Pitch is the same story about the other axis.

**Head roll** φ about the view axis is the exception:

    x_c' = a·cosφ − (sx/sy)·b·sinφ    w_c' = d          ← unchanged
    y_c' = (sy/sx)·a·sinφ + b·cosφ    z_c' = c

Roll leaves `w` alone, so it stays a 2×2 on `(x, y)` with the aspect factors —
as cheap as the shear already is.

### Why that is not merely an extra multiply: X4 culls

Rotating in clip space can only move geometry X4 already submitted. X4 culls on
the CPU against its own frustum, so a clip-space yaw reveals nothing beyond the
edge of the rendered field — it drags the black border into view.

The headroom is quantifiable, and it is not enough. With X4 rendering `h`
degrees horizontally and the headset displaying `v`, a clip-space yaw has
`(h − v)/2` degrees of margin per side. #24 got `h` to 106°; a headset showing
~100° leaves about **3°**. Buying ±30° would need `h ≈ 160°`, where
`sx = cot 80° = 0.176` and a rectilinear projection spends almost all of its
pixels on the periphery — a resolution cost that fails "performance is king"
before it is built.

So the shape of #33 is now visible, and it is not mainly a layer job:

* **yaw and pitch** have to reach X4's *own* camera, so the engine culls and
  renders for the direction the head is actually facing. That is the injector's
  side of the house, and it is the piece `v0.1` got wrong for reasons
  (OpenTrack Euler angles, wrong pivot, gimbal lock) that a quaternion taken
  straight from `xrLocateViews` does not have.
* **roll**, and the last few milliseconds of yaw/pitch as a late correction,
  can stay in clip space where they are nearly free.

That claim needs its own verification — whether the injector can drive X4's
free-look orientation at all is unproven — so it is written here as the reading
of the algebra, not as a decision. It is recorded now because it changes what
"head tracking" means for this project, and re-deriving it later would be worse
than being wrong in public.

### The probe, and what each answer changes

`tests/xr_probe.cpp` drives `common/x4vr_xr.hpp` on a real GPU before a line of
it runs inside the game — the same rule that made the cursor overlay work on
its first take. `tests/run-xr-probe.sh` runs it in two halves: the test card
against a plain 2-layer image (no runtime needed, includes the case that must
fail), then the live session.

| What it reports | What it decides |
|---|---|
| `KEY_PHYSICAL_CHOSEN` | whether the runtime's device is the one X4 would have picked. If not, the layer has a real problem, and finding out here costs nothing |
| `xr: runtime adds … extensions` | how much of X4's `VkDeviceCreateInfo` we have to edit — the multiview edit gets company |
| `KEY_EYE_RECOMMENDED` | whether 1408×1408 per eye is anywhere near right, i.e. whether `X4VR_SBS_WIDTH/HEIGHT` and the gamescope size have to move |
| `KEY_FOV_ASYMMETRIC` | whether X4's symmetric projection can be submitted as-is. X4 has no off-axis term; if the headset's frusta are asymmetric, one has to be added or the image is submitted into the wrong frustum |
| `KEY_FOV0`/`KEY_FOV1` | the `h` in the headroom arithmetic above, and hence what `X4VR_FOV` should be |
| `KEY_IPD_M` | the runtime's own eye separation, against the 0.064 m the shear assumes — #25's number, arriving from the runtime rather than from a guess |
| `KEY_HEAD_SPAN_M` | that the pose is *moving*. A pose that never changes is the failure this whole task exists to avoid, and an average would hide it |

### Predictions, before the run

Committed here before the measurement, as usual. This is a **tool run**, not an
X4 take — it costs a minute and no load screen — so the bar for what it must
settle is higher than for a take, not lower.

**P111.1: the runtime reports exactly 2 views for `PRIMARY_STEREO`.** Anything
else and the two-layer eye image is the wrong container.

**P111.2: the per-view FOV is asymmetric** — `|angleLeft| ≠ |angleRight|` on at
least one view, by more than a degree. Nearly every HMD cants its displays. If
this is confirmed, X4's symmetric `sx` cannot be submitted as the view's own
frustum and the eye transform needs an off-axis term it does not have today.

**P111.3: the recommended per-eye extent is not 1408×1408**, and is likely
taller than wide. 2816×1408 was chosen for a flat monitor, not for a headset,
so `X4VR_SBS_*` becomes a knob rather than a constant.

**P111.4: `xrGetVulkanGraphicsDevice2KHR` returns index 0** — the one discrete
GPU on this machine — so the "the runtime wants a different device" risk does
not bite here. Confirming it does not retire the risk for other people's
machines; it only says it is not this machine's problem.

**P111.5: the runtime adds at least one Vulkan device extension X4 does not
enable** (external memory/semaphore, most likely as fds). If so, editing X4's
device create-info is mandatory rather than a possibility, and the multiview
edit already proved that path works.

**P111.6: `KEY_IPD_M` lands between 0.058 and 0.072 m**, and the sign check
holds — view 1 is to the **+x** side of view 0, i.e. view 1 is the right eye. A
negative `dx` would mean the array-layer convention (layer N = view N = left,
right) is backwards, which would silently swap the eyes.

And one that the card answers rather than the log: **in the headset, the blue
tint and the outer white bar must agree** — blue background with a bar hard
against the *left* edge is the left eye. If the tint says left and the bar says
right, the views are crossed somewhere between `imageArrayIndex` and the
display.

### How to run it

    cd /home/patola/workspace/claude/X4VRMOD && cmake --build build -j8
    ./tests/run-xr-probe.sh 20

with WiVRn started and the headset connected and awake. The card half runs
either way; the live half prints `FAIL=` and says what to start if no runtime
is up. Output is teed to `/tmp/x4vr-xrprobe.txt`.

Already verified here, with no runtime present: the card is correct on this GPU
under validation, the negative control (view 1 painted as a copy of view 0) is
caught, and the no-runtime path reports
`XR_ERROR_RUNTIME_UNAVAILABLE` with the sentence that names the cause rather
than the enumerant.

**The live half cannot be run here, and it is worth writing down why so nobody
looks again.** Monado's simulated-HMD driver plus `XRT_COMPOSITOR_NULL` would
give a full session with no headset at all — the ideal offline harness for this
task. There are two Monado builds on this machine:

    ~/.local/share/envision/prefixes/a2f5a706-…/bin/monado-service
    /dados/wivrn-test/build-monado/src/xrt/targets/openxr/libopenxr_monado.so

and **both are stale**: they were built against `librealsense2.so.2.55`/`.2.56`
and `libopencv_*.so.412`, none of which are installed now (11 unresolved
`DT_NEEDED` each). Reviving them means rebuilding Monado, which is a project of
its own and not on this critical path. WiVRn is the installed, working runtime
and has no simulated driver, so the live half needs the headset. If offline
iteration on the session path ever becomes the bottleneck, rebuilding Monado
with `-DXRT_BUILD_DRIVER_REALSENSE=OFF -DXRT_HAVE_OPENCV=OFF` is the cheap way
back, not chasing the old prefixes.

## The first live session — all six predictions confirmed, and the card found the real defect

WiVRn 26.6, Meta Quest 3, seated, 20 s, head moved throughout.
`/tmp/x4vr-xrprobe.txt`. The bring-up worked end to end on the first attempt:
1799 frames, 1799 located, 1798 projection layers accepted, session reached
`FOCUSED`.

    KEY_RUNTIME=WiVRn 'v26.6-151-gca59f467'   KEY_SYSTEM=Meta Quest 3 on WiVRn
    KEY_VIEWS=2            KEY_EYE_RECOMMENDED=3096x3243   KEY_EYE_MAX=6192x6486
    KEY_PHYSICAL_DEVICES=1 KEY_PHYSICAL_CHOSEN=0           KEY_SPACE=STAGE
    KEY_FORMAT=43 (R8G8B8A8_SRGB)             KEY_SWAPCHAIN_IMAGES=3
    KEY_IPD_M=0.0630       KEY_FOV_ASYMMETRIC=1
    KEY_FOV0=-54.0000,40.0000,44.0000,-55.0000
    KEY_FOV1=-40.0000,54.0000,44.0000,-55.0000
    KEY_HEAD_SPAN_M=0.2642,0.1731,0.2755

**P111.1 confirmed.** 2 views.

**P111.2 confirmed, and by far more than the "more than a degree" the
prediction hedged with.** Each view is canted 14° outward horizontally
(−54/+40 and −40/+54) and 11° down vertically (+44/−55, the same in both).
This is now the single most consequential number in the file — see below.

**P111.3 confirmed.** 3096×3243 per eye, taller than wide as predicted. That is
**10.04 Mpx per eye against 1408×1408 = 1.98 Mpx — 5.1×**. `X4VR_SBS_*` is
definitively a knob now, and what to set it to is a performance question, not a
correctness one.

**P111.4 confirmed.** One device, index 0. The "runtime wants a different
`VkPhysicalDevice`" risk does not bite on this machine. It is not retired for
anyone else's.

**P111.5 confirmed**, and observable after all — WiVRn logs the merged lists
itself. Instance: `external_fence/memory/semaphore_capabilities`,
`get_physical_device_properties2`, `debug_utils`. Device:
`dedicated_allocation`, `external_fence/memory/semaphore` and all three `_fd`
variants, `get_memory_requirements2`, `image_format_list`,
`timeline_semaphore`. So editing X4's `VkDeviceCreateInfo` is mandatory, and
the multiview edit already proved that path. (The probe's own query said "not
observable": `xrGetVulkanInstance/DeviceExtensionsKHR` only resolve if
`XR_KHR_vulkan_enable` is *enabled* on the instance, not merely offered. Fixed
— the v1 extension is now requested alongside enable2 purely so the merge can
be narrated.)

**P111.6 confirmed.** 0.0630 m, and `dx = +0.0628` — view 1 is the right eye,
so layer N = view N is the correct convention and the eyes are not crossed.

### The card was wrong, in exactly the way the real submission would be

Reported from the headset: the bars for each eye did not agree, and the fusible
bar appeared as **two bars**. That is not a runtime problem and not an eye-order
problem. It is arithmetic.

A rectilinear projection is linear in **tangent**, not in angle, so the angle at
image column *x* is `atan(tanL + (x/W)·(tanR − tanL))`. With WiVRn's canted
views, column `W/2` is:

    view 0:  -15.04 deg        view 1:  +15.04 deg        divergence 30.07 deg

The card painted its fusible bar at `x = W/2` in both eyes, so it asked the eyes
to **diverge by 30°**. Human eyes converge freely and essentially cannot diverge
at all, so it could only ever appear as two separate bars. The wide markers,
being at opposite image edges, are ±54° apart and monocular — "the bars don't
agree" is what that looks like with both eyes open.

The card is now placed **by angle**: the marker at ∓45°, and the fusible bar at
±0.9°, which for a 0.063 m IPD is an object at 2.0 m. Verified offline against
the FOV WiVRn actually reported, with the defect added as a third negative
control that reproduces `−30.074 deg` — the measured value, from the same
arithmetic that produced the symptom.

### What this forces on the eye transform

X4's projection is symmetric by construction: `row0(P) = [sx 0 0 0]`, no
off-axis term. The runtime's frusta are not. Submitting X4's image as the
runtime's view is the same mistake the card made, applied to the whole scene —
every object 15° off where it belongs, in opposite directions per eye.

Two ways out, and the numbers pick one.

**(a) Submit a symmetric FOV of our own.** `XrCompositionLayerProjectionView`
carries the FOV of the image we submit; it does not have to be the one
`xrLocateViews` recommended. A symmetric frustum covering both eyes needs
±54° h and ±55° v, i.e. **110°**, so `X4VR_FOV = 110/73.7399 = 1.4917` (today
1.437 = 106° — already most of the way there, courtesy of #24). Costs
**1.48× the frustum area** in rendered pixels that the headset will never show,
on top of the 5.1× the eye extent already wants.

**(b) Map the symmetric render onto the asymmetric frustum in clip space.**
For a render at ±55° and a target frustum `[tanL, tanR]`:

    x_c' = A·x_c + B·w_c        A = (2/(tanR−tanL)) / cot(55°)
                                B = −(tanR+tanL)/(tanR−tanL)

    view 0:  A = 1.2892   B = +0.2425
    view 1:  A = 1.2892   B = −0.2425

Two constants per eye and **one multiply-add on a value `gl_Position` already
carries**. No wasted pixels: the visible field fills the whole eye image. X4
still has to render wide enough that nothing is culled — the union is ±55° — but
the clip-space map then crops to each eye's real frustum at full density.

(b) is the one to build. It is the same shape as the existing shear, it composes
with it, and it costs about what the shear costs. The vertical follows the same
form with X4's Y-flip (`sy` is negative) to be settled in the implementation
rather than asserted here. Filed as **#35**.

Note what (b) does *not* fix: it is a static per-eye frustum correction, not
head tracking. The yaw/pitch argument in the #34 section above is unchanged.

### The second run — P5's gate, passed

With the card placed by angle, in the headset: **blue on the left, green on the
right, and the centre bar fused into a single bar floating about 2 m away.**

Three separate things settle at once, and it is worth separating them because a
single "it looked right" would have conflated them:

* **Eye order.** `imageArrayIndex` N = the runtime's view N, and view 0 is the
  left eye. This matches the pose evidence (`dx = +0.0628`, view 1 to the +x
  side) and the FOV evidence (view 0 reaches further left, −54° against +40°),
  so three independent signals agree. The two-layer eye image the compositor
  already builds can be submitted layer-for-view with no reordering.
* **Disparity sign.** Converged reads as *nearer*. The bar was painted at
  +0.9° in the left eye and −0.9° in the right; had the sign been inverted it
  would have fused *behind* the background, which is the defect that would
  otherwise have been discovered much later and blamed on the shear.
* **Scale.** ±0.9° at the measured 0.063 m IPD is `ipd/angle = 2.0 m`, and it
  was seen at about 2 m. That is the first time anything in this project has
  put a predicted distance in front of a person and had the distance come back.
  It is not #25 — nothing of X4 is in that image — but it does say the
  arithmetic #25 will be judged against is sound.

Tagged `stage7-xr-session-proven`, with the run and its checks in
`docs/known-good-runs.md`. **#34 is closed.**

## Take 111 — the session inside X4, submitting nothing

`stage7-xr-session-proven` proved the bring-up in a standalone program. This
puts the same code on **X4's own** instance, device and queue, and deliberately
stops there.

**Why stop there.** Two risks are being separated. One is that the runtime adds
extensions to structs X4 owns — ten of them, measured in the last run — and X4
has to keep working with them. The other is that X4's frames reach a swapchain
and a compositor with its own pacing. Landing both at once means any failure has
two candidate causes, and this project has lost more runs to that than to
anything else. So this take changes X4's *object creation* and nothing about its
rendering. The headset shows WiVRn's idle scene; X4 renders to the monitor as
usual.

**What the layer now does when `X4VR_VR=1`:**

* `vkCreateInstance` — opens the runtime *before* the down-chain create, then
  lets `xrCreateVulkanInstanceKHR` create X4's instance from X4's own
  create-info, calling back through our chain.
* `vkCreateDevice` — asks the runtime which `VkPhysicalDevice` it requires and
  compares it with X4's choice. **If they differ, X4's choice stands and the
  session is refused.** A mod that moves the game onto another GPU to suit a
  headset has stopped being non-intrusive; the log says so in those words.
  Otherwise `xrCreateVulkanDeviceKHR` creates the device, with the multiview
  edit already applied.
* then creates the session on that device and starts a frame loop **on its own
  thread**, submitting zero layers.

The thread is not in `vkQueuePresentKHR`, and that is a design decision rather
than convenience: `xrWaitFrame` blocks until the runtime's next frame boundary,
so driving it from the present hook would peg X4's frame rate to the headset's
refresh and couple two cadences with no reason to agree. When submission
arrives, the pacing stays on this thread and only the recording moves.

### Predictions, before the run

**P112.1: X4 starts, loads a save and plays normally.** This is the whole point
of the take. The failure it is looking for is X4 refusing to create a device, or
crashing later, because of extensions it never asked for.

**P112.2: the runtime's `VkPhysicalDevice` is the one X4 chose**, so the session
is created. P111.4 established this for the probe's own instance; it is not the
same claim, because X4 enumerates and selects for itself.

**P112.3: the session reaches `FOCUSED`, and the frame count accumulates at the
headset's rate rather than X4's** — order 72–90 Hz, so several thousand frames
in a few minutes, whatever frame rate X4 is running at. If `frames` instead
tracks X4's fps, the thread is being throttled by something and the eventual
submission design is wrong.

**P112.4: X4's frame time is unchanged against take 110** within the noise the
`perf` section already shows. The XR thread submits nothing and touches no queue.
A *large* regression would more likely be WiVRn's compositor contending for the
GPU than anything the layer does — worth separating before blaming this code.

**P112.5: X4 creates more than one queue family, and the graphics family has a
queue to spare.** Informational for this take and load-bearing for the next: a
`VkQueue` is externally synchronised, so if X4 uses every queue of its graphics
family, the runtime and X4 will be submitting to the same one and that needs
handling before any layer is submitted.

### The run

Exactly `stage6-sx-per-draw`'s line plus `X4VR_VR=1` — one change, so a
regression has one candidate:

    X4VR_TAKE=111-VR-BRINGUP X4VR_VR=1 X4VR_FOV=1.437 X4VR_DUMP_MATRICES=1
    X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take111.log
    ./launch/x4vr-launch.sh

**Interaction take**, not a measurement: no probe, no dumps. WiVRn must be
running and the headset connected *before* X4 starts, because the runtime is
opened during `vkCreateInstance`.

Acceptance is `python3 tools/score_run.py /tmp/x4vr-take111.log`, which now has
a `vr` section. It is gated on **intent** — `X4VR_VR=1` on the command line, not
on whether a session happened to come up — so a run that asked for VR and got
none fails rather than reading as "not a VR run". It fails on: no session (and
quotes the reason), never `FOCUSED`, no XR frame, or a pose for under 90% of
frames. Head span is informational: sitting still is a legitimate run.

Validated before the take, on this GPU with no runtime present: the layer says
`NO SESSION THIS RUN` and every existing suite still passes with `X4VR_VR=1`
set — `run-multiview-render.sh`, `run-cursor.sh`, `run-multiview-enable.sh`,
`view_math`. The scorer's new section was exercised against eight crafted logs
(six that must fail, two that must pass) and re-run over all 77 logs in `/tmp`:
the verdict set is unchanged at FAIL = {44, 45, 48, 101, 102}.

## Take 111 — aborted in `xrCreateSession`. A layer's handles are not the application's

**P112.1 is refuted.** X4 did not survive. It reached `vkCreateDevice`, created
the device through the runtime, logged its queue families, and aborted inside
`xrCreateSession`:

    [Vulkan Loader] ERROR: vkGetPhysicalDeviceMemoryProperties:
                           Invalid physicalDevice

    #3  vkGetPhysicalDeviceMemoryProperties ()   from libvulkan.so.1   -> abort
    #4  vk_init_from_given (physical_device=0x23071c50,
                            vkGetInstanceProcAddr=<from libvulkan.so.1>)
    #5  client_vk_compositor_create (getProc=<from libvulkan.so.1>)
    #7  oxr_session_populate_vk
    #10 oxr_xrCreateSession
    #11 x4vr::xr::session_create (phys=0x23071c50)
    #13 x4vr_CreateDevice (phys=0x23071c50)      <- the handle a LAYER is given
    #16 vkCreateDevice ()  from steamoverlayvulkanlayer.so
    #19 vkCreateDevice ()  from libvulkan.so.1

The rest of the predictions: **P112.5 answered** — X4 creates queue family 0
with one queue (graphics) and family 1 with one queue, so there is no spare
graphics queue and the runtime will have to share X4's when submission starts.
**P112.3 and P112.4 unanswered**; the run never got that far.

**P112.2 needs its verdict written carefully**, because it "passed" and the
passing was misleading. The check compared the runtime's required
`VkPhysicalDevice` against X4's and found them equal — and they were equal
because *both* were chain-level handles, not because the comparison was
meaningful.

### Why, and why it is not a bug in either program

* The Vulkan loader hands the **application** a wrapped `VkPhysicalDevice` and
  passes the **unwrapped** one down the layer chain. The `phys` a layer is
  given in `vkCreateDevice` is not the handle X4 itself holds.
* `XrGraphicsBindingVulkan2KHR` carries handles and **no
  `pfnGetInstanceProcAddr`**. A runtime therefore has no choice but to use the
  loader's public entry points on whatever handles it is given.

Monado hid the seam by being inconsistent: it cached the
`pfnGetInstanceProcAddr` we passed to `xrCreateVulkanInstanceKHR` and used it
for `vkEnumeratePhysicalDevices` — which is why `xrGetVulkanGraphicsDevice2KHR`
returned a chain-level handle and the comparison agreed — and then used the
public loader for the session's Vulkan bundle. Frame #4 shows both facts in one
line: our handle, its `vkGetInstanceProcAddr`.

**Measured, not inferred.** The layer now prints both handles, and on this
machine with no runtime at all:

    vr: physical device "AMD Radeon RX 7900 XTX (RADV NAVI31)" — this layer was
        handed 0x623edccb0ba0, the loader's public handle is 0x623edccb1040 —
        not the same handle, which is what aborted take 111

### The fix, in two parts

1. **Give the session the application-level handle**, found by matching
   `VkPhysicalDeviceIDProperties::deviceUUID` rather than by comparing
   pointers. The loader's own `vkEnumeratePhysicalDevices` is reached by
   `dlopen`ing `libvulkan.so.1` and taking its `vkGetInstanceProcAddr` — the
   public trampoline, which is the space the runtime works in.
2. **Create the session off the loader's chain.** The lookup calls back into
   the loader on the instance side, and doing that from inside the loader's own
   `vkCreateDevice` is a hazard worth not taking. The session is now built on
   its own thread, started from the first `vkGetDeviceQueue` (either spelling,
   with `vkQueuePresentKHR` as a backstop) — after `vkCreateDevice` has
   returned to X4, which is also when the loader has finished installing its
   dispatch on the new device.

The physical-device comparison is gated on **intent** — `X4VR_VR=1` — not on a
runtime being present, which is the only reason it could be checked here at all
with no headset attached.

### A second defect, found by the first one's fix

Adding that log line broke `run-multiview-render.sh`'s "probe: layers match"
case, reproducibly. The case read its verdict with

    grep -o 'IDENTICAL\|DIFFER' <<<"$out" | head -1

— the **first** such word anywhere in the output — and the new line contained
`DIFFERENT`. The layer was fine; the matcher was. It now reads the verdict off
the instrument that produces it:

    grep 'mv probe:' <<<"$out" | grep -ow 'IDENTICAL\|DIFFER' | head -1

anchored to the probe's own lines and word-matched. The log line was reworded
as well: a diagnostic that trips a test's grep will trip a person's. The other
matchers in the suite were swept for the same shape — the rest are anchored to
`rp #`, `mv probe:`, or `spirv-dis` output, and none collide.

### Take 112 — the same run again

    X4VR_TAKE=112-VR-BRINGUP X4VR_VR=1 X4VR_FOV=1.437 X4VR_DUMP_MATRICES=1
    X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take112.log
    ./launch/x4vr-launch.sh

**P113.1: X4 starts, loads a save and plays normally** — P112.1 again, against
the handle fix.

**P113.2: the log prints two different physical-device handles**, as it does
here with no runtime. If they come out equal under gamescope, the wrapping
depends on the layer stack and the whole diagnosis needs re-reading.

**P113.3: the session reaches `FOCUSED`, and frames accumulate at the headset's
rate rather than X4's.** Carried over from P112.3, still unanswered.

**P113.4: X4's frame time is unchanged against take 110.** Carried over from
P112.4, still unanswered.

## Take 112 — X4 survives, the session does not, and my take-111 explanation was wrong

**P113.1 confirmed.** X4 started, loaded, flew, opened and closed the map,
landed on a station, all in SBS on the flatscreen. The runtime's ten extra
extensions in X4's instance and device cost it nothing visible. `perf` reads
7.75 ms median with a 7.75–9.52 ms flight phase, in the same country as take
110 — **P113.4 is not yet properly answered**, because the two runs did not
park on the same scene, but nothing here looks like a regression.

**P113.2 confirmed**, and under gamescope with three other layers in the stack:

    vr: physical device "AMD Radeon RX 7900 XTX (RADV NAVI31)" — this layer was
        handed 0x464352e0, the loader's public handle is 0x461e2810

**P113.3 unanswered.** No session:

    vr: NO SESSION THIS RUN — xrCreateSession: XR_ERROR_VALIDATION_FAILURE

### Reading the runtime instead of guessing again

Monado's source is on disk under `~/.local/share/envision/wivrn/build/_deps/`,
and `oxr_session.c:1151` is exact:

```c
if (sys->suggested_vulkan_physical_device != vulkan->physicalDevice) {
    return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
        "XrGraphicsBindingVulkanKHR::physicalDevice %p must match device %p "
        "specified by %s", ...);
}
```

So the binding must carry **exactly** the handle
`xrGetVulkanGraphicsDevice(2)KHR` returned. Take 111 passed the chain-level
handle and aborted in the compositor; take 112 passed the loader's public one
and was rejected here.

**And the explanation I wrote for take 111 was wrong.** I claimed Monado had
cached the `pfnGetInstanceProcAddr` we passed to `xrCreateVulkanInstanceKHR`
and used it for `vkEnumeratePhysicalDevices`. It does not:
`oxr_api_system.c:314` and `:333` both pass the runtime's own linked
`vkGetInstanceProcAddr` — the loader's public one — into
`oxr_vk_get_physical_device`. That leaves take 111's `want == phys` comparison
unexplained by anything I have read, and it is left unexplained here rather
than given a second story. The new log prints all three handles in one line, so
take 113 settles it as data.

What *is* established, and is enough to act on: **`XR_KHR_vulkan_enable2` is the
wrong extension for a Vulkan layer.** It takes a `pfnGetInstanceProcAddr`, a
layer only has a down-chain one, and handing that over is the only way our
internal handle space ever reaches the runtime. Two takes died on it.

### The fix — the v1 extension, which has no such channel

`XR_KHR_vulkan_enable` returns **lists of extension names** and lets the
application put them in its own create-infos. That is an edit this layer
already makes, for multiview. Nothing of ours crosses into the runtime.

* `vkCreateInstance` / `vkCreateDevice` — merge
  `xrGetVulkanInstanceExtensionsKHR` / `xrGetVulkanDeviceExtensionsKHR` into
  X4's lists, dedup, and for the device **filter against what the driver
  advertises**: an unsupported name fails `vkCreateDevice` outright, which
  would take X4 down over a VR knob. Anything dropped is logged.
* the session binds the handle `xrGetVulkanGraphicsDeviceKHR` returns,
  **verbatim**, because that is what the runtime compares against.
* and that handle is checked against the loader's own public enumeration,
  matched by `VkPhysicalDeviceIDProperties::deviceUUID` to the GPU X4 chose.
  **This is a guard with two independent sources**, which the one it replaces
  was not — the old one compared two values that could both come from the same
  enumeration, so "equal" was a property of the plumbing. If they disagree now,
  the session is refused and logged; X4 keeps running.

`tests/xr_probe.cpp` now runs the **v1 path by default**, so the standalone
program exercises the layer's code rather than a parallel one.
`run-xr-probe.sh enable2` still runs the path `stage7-xr-session-proven` was
taken with, which is what makes the two comparable.

### Before take 113 — one minute, no X4

    ./tests/run-xr-probe.sh 20

with WiVRn up. This is the same v1 bring-up the layer now does, minus the layer
chain, and it costs a load screen less than a take. `KEY_PATH=v1` confirms which
path ran.

### Take 113

    X4VR_TAKE=113-VR-BRINGUP X4VR_VR=1 X4VR_FOV=1.437 X4VR_DUMP_MATRICES=1
    X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_GAMESCOPE=1
    X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1
    X4VR_SBS=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_BINDLESS_MIRROR=1
    X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take113.log
    ./launch/x4vr-launch.sh

**P114.1: X4 still starts and plays**, now with the extensions merged by us
rather than by the runtime. The filter against the driver's advertised list is
the new thing that could break it.

**P114.2: the three-handle line shows the runtime's handle equal to the
loader's public one**, and both different from the layer's. That is the shape
the v1 path predicts, and it is what would explain both earlier takes.

**P114.3: the session reaches `FOCUSED`** — P112.3 and P113.3 again, twice
unanswered.

**P114.4: X4's frame time is unchanged against take 110** on a comparable
parked phase.

## Take 113 — the session lives inside X4. All four predictions confirmed

`score_run.py` exit 0. Run from inside WayVR's virtual screen; the VR view went
black when X4 started and returned to normal when it quit, which is exactly what
a focused session that submits no layers looks like.

    vr  runtime "WiVRn" session=1 focused=1 frames=12840 located=12840 submitted=0
        head span 0.132 x 0.143 y 0.243 m — the pose moves

**P114.1 confirmed.** X4 started and played normally with the extensions merged
by us rather than by the runtime, and with the driver-support filter in the
path. It needed 3 instance extensions added (`external_fence_capabilities`,
`external_memory_capabilities`, `external_semaphore_capabilities` — X4 already
had the other one) and **all 9** device extensions (`dedicated_allocation`,
`external_fence`/`_memory`/`_semaphore` and their three `_fd` forms,
`get_memory_requirements2`, `image_format_list`). Nothing was dropped by the
filter.

**P114.2 confirmed, exactly.**

    vr: physical device handles — layer 0x123ab150, loader public 0x123ae500,
                                  runtime asks for 0x123ae500

The runtime's handle *is* the loader's public one, and both differ from the
layer's. That is the shape the v1 path predicted, and it is why take 111 — which
bound the layer's handle — aborted inside the compositor.

**Left open, and immaterial:** why take 111's `want == phys` comparison agreed
at all, given Monado uses the public `vkGetInstanceProcAddr` for that query. The
enable2 path is abandoned, so nothing depends on the answer. It is recorded as
unexplained rather than given a story that fits.

**P114.3 confirmed, with a clean number.** `FOCUSED` reached, 12840 frames,
**12840 located — 100%**. And the rate:

    29 samples between summaries: min 90.0 Hz, median 90.0, max 90.2
    overall 12839 frames / 142.6 s = 90.01 Hz

Dead flat at the Quest 3's refresh while X4 ran at 17.28 ms (58 fps) loading and
7.54 ms (133 fps) in flight. The XR loop is on the headset's clock and X4 is on
its own — which is the property the separate thread exists to provide, and the
reason `xrWaitFrame` must never move into `vkQueuePresentKHR`.

**P114.4 confirmed as far as it can be.** Flight phases: take 110 **7.02 ms**,
take 112 **8.23 ms**, take 113 **7.54 ms**. Take 113 sits between the two runs
without VR frames and inside their spread. This is not a controlled comparison —
the three parked on different scenes — and it is recorded as "no sign of a
regression", not as a measurement. A real A/B needs a parked camera.

Tagged **`stage8-xr-session-in-x4`**, with the run in
`docs/known-good-runs.md`.

### What this state does not do, and the two things in the way of it doing more

It submits nothing. Two known obstacles stand between here and X4 appearing in
the headset, and both are now measured rather than suspected:

1. **The frustum.** #35. X4's projection is symmetric; the runtime's views are
   canted 14° outward. Submitting as-is puts every object 15° off, in opposite
   directions per eye — the defect the test card demonstrated at 30° of
   divergence.
2. **The queue.** Take 111 measured it and take 113 confirms it: X4 creates
   **queue family 0 with exactly one queue** (graphics) and family 1 with one.
   The runtime's client compositor submits on the queue the graphics binding
   names, and a `VkQueue` is externally synchronised — so as soon as we hand it
   layers, the runtime and X4 are submitting to the same queue from two
   threads. This run does not trip it only because it submits nothing. The fix
   is in reach: the layer already edits `VkDeviceCreateInfo`, so it can ask for
   a second queue on that family and give the runtime its own. Filed as **#36**.

## Task #36 — the plan was impossible, and the hardware said so in one line

#36 was filed as "ask for one more queue on X4's graphics family and bind the
session to it". The first offline run of that code answered it:

    vr: X4 created queue family 0 x1 graphics (the device offers 1)
    vr: NO SESSION THIS RUN — no graphics queue can be reserved …

`vulkaninfo` confirms it is the device, not the run:

    family 0   queueCount 1   GRAPHICS | COMPUTE | TRANSFER | SPARSE_BINDING
    family 1   queueCount 4   COMPUTE | TRANSFER | SPARSE_BINDING
    family 2   queueCount 1   VIDEO_DECODE
    family 3   queueCount 1   VIDEO_ENCODE
    family 4   queueCount 1   SPARSE_BINDING

**RADV Navi31 has exactly one graphics queue on the whole device**, and that is
AMD's shape in general, not a quirk of this card. So the runtime cannot be given
one of its own here, and refusing the session — which is what the first version
of the code did, deliberately, to avoid a data race — would have meant "no VR on
AMD".

### Sharing, serialised by the layer

External synchronisation is a requirement *on the application*, not a
prohibition, and a Vulkan layer is exactly where it can be met. Every submission
to that queue passes through code this project owns:

* **X4's** — `x4vr_QueueSubmit`, and `x4vr_QueuePresentKHR`, which also covers
  the SBS composite, the cursor overlay and the dump path's `vkQueueWaitIdle`,
  because all three submit from inside the present hook.
* **the runtime's** — inside `xrEndFrame`, called from the layer's own XR
  thread.

One mutex across both sides. `xrWaitFrame` is deliberately outside it: it blocks
until the runtime's next frame boundary, and holding a queue lock across it
would stall X4 for a whole headset frame, every frame.

The dedicated-queue path is kept and tried first — a GPU that has spare graphics
queues (NVIDIA typically exposes 16) gets one, and the log says which mode is in
effect. On this machine it reads:

    vr: this device has no spare graphics queue — the runtime shares X4's
        (family 0 index 0), serialised by the layer.

On the sharing path X4's `VkDeviceCreateInfo` queue array comes out byte-for-byte
what X4 asked for, so this costs the game nothing at device creation. What it
does cost is one uncontended lock per submit and per present, and a possible
brief stall when `xrEndFrame` holds it. Both are inert until the layer actually
submits layers, and neither has been measured yet — that goes with the
submission take, not before it.

**Still unhooked, and worth checking before submission:** `vkQueueBindSparse`.
Family 0 advertises `SPARSE_BINDING`, the layer does not intercept it, and it is
a queue operation with the same external-synchronisation rule.

## Task #35 — the affine, derived and locked offline. No take spent

`tests/view_math.cpp` grew 22 cases; all pass, and the whole thing still runs
without a GPU or a running X4.

### The map

X4 gives `x_c = sx·x`, `w_c = z`, so the true ray is `x/z = x_c/(sx·w_c)`. A
rectilinear image is linear in TANGENT, so the target's NDC for that ray is
`(2·(x/z) − (tan_r+tan_l))/(tan_r − tan_l)`. Multiplying by `w_c`:

    x_c' = [2/((tan_r − tan_l)·sx)]·x_c − [(tan_r + tan_l)/(tan_r − tan_l)]·w_c
    y_c' = [−2/((tan_u − tan_d)·sy)]·y_c + [(tan_u + tan_d)/(tan_u − tan_d)]·w_c

At the union half-angle of 55°, against WiVRn's measured angles:

    view 0   A_x = 1.2892   B_x = +0.2425   A_y = 1.1932   B_y = −0.1932
    view 1   A_x = 1.2892   B_x = −0.2425   A_y = 1.1932   B_y = −0.1932

### The two things the previous section left open are now settled

**The vertical sign.** `A_y` is negative over `sy`, and `sy` is negative, so
`A_y` comes out **positive** — the affine preserves X4's Y-flip rather than
adding a second one. Asserted as a *direction* ("a point above the camera lands
at negative NDC y"), because a magnitude check cannot catch a flip and a flipped
image is invisible in every aggregate this project computes.

**Composition with the existing shear.** The shear leaves `x_c = sx·(x − d)`, so
dividing by `sx` recovers the eye's own ray and the affine needs no eye-offset
term at all. Apply the shear, then the affine — proven against the *definition*
of the target frustum (not against a second copy of the implementation) at a
worst NDC error of 2.4e-07, over both eyes, six points, and five cameras
including the 1.5:1 non-square map camera. The folded single-multiply form is
proven equivalent and left unused: the shear it would replace is what 60 passing
takes ran.

### A prerequisite that turned out not to exist

The vertical needs no separate `X4VR_FOV`. `score_run.py` already reads it out
of every log: on the fov camera `|sy/sx| = 1.0000` against an eye aspect of
1.0000 — X4 **widens** the field rather than stretching it, so the eye is a
square symmetric frustum and one setting covers both axes. The union is
therefore `max(54, 40, 44, 55) = 55°`, giving **`X4VR_FOV = 1.4917`** — which is
the number the previous section had already derived horizontally, now known to
be sufficient vertically too.

### A number in this file was wrong: the views are not "canted 14°"

`14°` is `54 − 40` — a subtraction in ANGLE space, in a file whose central
lesson is that rectilinear projection is linear in tangent. The correct figure
is `atan((tan_l + tan_r)/2) = 15.04°`, which the probe has been printing all
along. Both occurrences are left in place above; this is the correction.

More importantly, **neither number is a pose cant.** Both describe the FOV. A
runtime may put the same optics in the poses or in the FOV, and this file has
never measured which — see the prediction below.

### Two routes to first light, and why the cheap one should go first

|                        | symmetric submission | the affine |
|------------------------|----------------------|------------|
| shader changes         | none                 | every world vertex module |
| also needs             | nothing              | the inverse folded into `patch_fragment_invproj_eye` |
| rasterised / displayed | **1.54×**            | 1.0× |
| X4-side culling        | `X4VR_FOV = 1.4917`  | `X4VR_FOV = 1.4917` |

Submitting a symmetric FOV of our own is legal and normal: the `fov` field of
`XrCompositionLayerProjectionView` states what the image *was rendered with*,
not what the runtime recommended, and the compositor crops. The earlier section
rejected it — correctly, for the steady state, on the 1.5× fill cost. As a
**first milestone** that reasoning does not apply: it needs no shader work at
all, and it is the negative control the affine must be judged against. The
affine's saving is real (it maps the target frustum onto the full NDC box, so
nothing wasted is ever rasterised) and it stays the destination.

The coupling that makes the affine the larger job is already documented above:
the deferred passes reconstruct view position from NDC through
`M_invprojection`, and `patch_fragment_invproj_eye` corrects that for the eye
offset today. Change what NDC means and that correction changes with it.

### Probe run 2 — does the cant live in the pose or in the FOV?

Cheap, decisive, and not an X4 take: 20 s, no X4, no take number. `xr_probe`
now locates `VIEW` space and reports each view's rotation against the head, and
the two views against each other.

    tests/run-xr-probe.sh 20        # WiVRn running, headset on

**P115.1: the poses are PARALLEL** — `KEY_VIEW_REL_DEG` under 0.5°,
`KEY_POSES_PARALLEL=1`. The 15.04° is expected to sit entirely in the FOV,
because a runtime that rotated the poses *as well* would be describing 30° of
outward cant, which is not what a Quest 3's optics do.

**Why it is worth a run rather than an assumption.** If P115.1 holds, one camera
orientation serves both eyes and the per-eye difference is the lateral offset
the layer already applies — first light needs no new vertex math whatsoever. If
it fails, each eye needs its own rotation, and rotation is the case where the
algebra stops collapsing: a translation leaves `w_c` untouched, a yaw gives
`w_c' = −(a/sx)·sinθ + d·cosθ` and does not. That is a different patch, and
knowing which one to write is worth twenty seconds.

The probe prints its own verdict in words (`xr: VERDICT poses are …`) so the
answer does not have to be re-derived from two angles later.

### P115.1 is probably WRONG, and Monado's source says why — recorded before the run

Written after P115.1 and before the measurement, because the rule is that
predictions are committed before the run that tests them and wrong turns stay in
the file. Reading `oxr_session.c` after making the prediction turned up this:

    // oxr_session.c:680, inside the per-view loop of xrLocateViews
    if (sess->sys->inst->quirks.parallel_views) {
            view_pose.orientation = (struct xrt_quat)XRT_QUAT_IDENTITY;
    }
    ...
    if (sess->sys->inst->quirks.parallel_views) {          // :699
            adjust_fov(&fovs[i], &poses[i].orientation, &fov);
    }

`parallel_views` does **both** halves: it flattens the pose to identity *and*
adds the pose's Euler angles into the four fov angles. It is precisely the
"where does the cant live" switch, sitting in the runtime.

And our own probe log already recorded its state, fifteen lines from the top:

    quirks.parallel_views: false

So the runtime is **not** flattening anything — it passes the device's poses
through as they come. P115.1 argued the poses would be parallel because 15° of
fov offset plus 15° of pose would be an implausible 30° of cant. That argument
is about the *device*, and this code is about the *runtime*; it does not say
what WiVRn's driver reports. The prediction now leans the other way, and it is
left standing as written so the file records the reasoning that was wrong rather
than the reasoning that survived.

**Probe run 2 therefore has two halves**, so the mechanism is demonstrated and
not merely inferred:

    tests/run-xr-probe.sh 20                          # as the runtime comes
    OXR_PARALLEL_VIEWS=1 tests/run-xr-probe.sh 20     # the quirk forced on

`OXR_PARALLEL_VIEWS` is a Monado debug tristate (`oxr_instance.c:53`), so the
second half is a real control: `KEY_VIEW_REL_DEG` must go to ~0 and `KEY_FOV0/1`
must widen by the angle the poses were carrying. If the first half already reads
0, both halves agree and the poses were parallel all along.

**Either answer is now survivable, which the affine is the reason for.** The
quirk converts pose cant into fov asymmetry — and fov asymmetry is exactly what
`make_off_axis` already handles, to 2.4e-07. So the general solution does not
change; only whether this runtime hands us the easy form of the problem or the
hard one. Depending on a Monado-specific environment variable is a fallback and
not the design.

### Two more things the compositor source settles

**Uncovered field goes black, visibly.** `do_projection_layer` binds
`clamp_to_border_black` as the layer sampler (`comp_render_gfx.c:397`), so if
our submitted fov does not cover the display's field the border is black rather
than smeared. The union requirement is real and its failure mode is obvious on
sight — a good property for a first light.

**The submitted fov and pose really are the app's.** The same function does

    render_calc_uv_to_tangent_lengths_rect(&vd->fov, &data.to_tanget);
    calc_mvp_rot_only(state, layer_data, &vd->pose, &scale, &data.mvp);

on `vd`, the submitted `XrCompositionLayerProjectionView`. So declaring a
symmetric fov of our own is consumed as written — the milestone-A route is
sound. Note `calc_mvp_rot_only`: timewarp compensates **rotation only**, so the
pose we submit must carry the position we actually rendered from.

## Probe run 2 — P115.1 confirmed. The poses are parallel; the cant is all FOV

    /tmp/x4vr-xrprobe-20260809-175326.txt     as the runtime comes
    /tmp/x4vr-xrprobe-20260809-175358.txt     OXR_PARALLEL_VIEWS=1

Both halves, 1801 frames each, 1800 layers submitted:

    VIEW_REL_DEG=0.0000    POSES_PARALLEL=1    VIEW_CANT_DEG=-0.0000,-0.0000
    FOV0=-54.0000,40.0000,44.0000,-55.0000
    FOV1=-40.0000,54.0000,44.0000,-55.0000

    xr: view 1 is rotated 0.000 deg relative to view 0 (yaw -0.000 deg)
    xr: view 0 vs head — 0.000 deg total, yaw -0.000 deg
    xr: view 1 vs head — 0.000 deg total, yaw -0.000 deg
    xr: VERDICT poses are PARALLEL — the 15.04 deg sits in the FOV alone.

**The control was real, and it was checked rather than assumed.** The two logs
differ at line 14 — `quirks.parallel_views: false` against `true` — so the
environment variable took effect. Forcing the quirk on then changed *nothing*:
the FOV is byte-identical across the pair.

**That is a second, independent proof, through code I did not write.**
`adjust_fov` adds the pose rotation's Euler angles to all four FOV angles. An
unchanged FOV to four decimal places means those angles were zero, so the poses
carried no rotation — established without trusting my own quaternion math at
all. Two readouts, two code paths, same answer.

This is the shape a control is supposed to have. A no-op control is only
informative because the *mechanism* was verified to have engaged; had I only
compared the FOVs and not line 14, "nothing changed" would have been consistent
with the variable being ignored — a guard that cannot fail.

### My doubt was the thing that was wrong

The note above ("P115.1 is probably WRONG, and Monado's source says why")
overcorrected. Reading `oxr_session.c` established something true — the runtime
is not flattening anything — and I turned it into a claim about the *device*,
which it never addressed. WiVRn's driver reports parallel poses natively; the
runtime's non-intervention leaves them that way. The correct conclusion from
that source read was "so it depends on the driver, which is unmeasured", and
that is exactly the run I then specified. The prediction was right and the
paragraph doubting it was wrong; both stay in the file.

### What this buys: first light needs no new vertex math

The per-eye difference is a **lateral offset and nothing else** — which is
precisely what the layer's existing shear already applies. So:

* **Milestone A (symmetric submission)** is unblocked and needs no shader work:
  render ±55° symmetric, submit the located pose with a symmetric FOV of our
  own, let the compositor crop. 1.54× fill.
* **Milestone B (the affine)** removes that 1.54× and is derived, implemented
  and tested (2.4e-07). It is an optimisation, not a prerequisite.

Neither needs per-eye rotation, so the head-rotation algebra
(`w_c' = −(a/sx)·sinθ + d·cosθ`, which does not collapse) stays unwritten. That
was the expensive branch and it is now closed off by measurement.

### Two details worth keeping

**IPD moved, 0.0630 → 0.0633 m.** The runtime re-reports it per frame, as
`locate_views`' comment already says nothing may cache it. The code doesn't.

**Separation in x was 0.0593 m against a total of 0.0633 m.** The views are
located in STAGE space and the head was being moved, so the eye vector is not
aligned with stage x — the difference is head orientation, not an asymmetry.
Both parallelism readouts are *relative* (view-to-view, and view-to-head), so
they are unaffected by this; the eye offset itself must be applied along the
head's own x axis, which is #33's business.

## Probe run 3 — the copy path works, and the compositor honours our own FOV

    /tmp/x4vr-xrprobe-20260809-180959.txt        tests/run-xr-probe.sh copy 20

    xr: runtime offers 11 swapchain format(s), best first:
        91 97 43 50 37 44 4 100 126 124 130
    xr: chose format 50 (candidate 1 of 2)
    xr: copy mode — source 1408x1408 B8G8R8A8_UNORM x2 layers
    xr: swapchain 1408x1408 x2 layers, 3 image(s)
    FRAMES=1799  LOCATED=1799  SUBMITTED=1798  KEY_FORMAT=50

Patola: *"The bar still looks ok, 2m from me."*

**The format question is answered.** `50` is `VK_FORMAT_B8G8R8A8_SRGB` — same
channel order as X4's format-44 eye image, so `vkCmdCopyImage` is a raw
byte-preserving copy with no swizzle and no colour conversion, and declaring
sRGB tells the compositor those bytes are display-encoded, which they are.
`44` (`B8G8R8A8_UNORM`) is offered too, so there is a fallback if the sRGB
interpretation ever proves wrong. The list is now logged every run rather than
inferred.

**The milestone-A mechanism is proven on hardware, not just in Monado's
source.** The card was built from the ±55° symmetric FOV we *declared*, which
puts the fusible bar at `u ≈ 0.5054` of the image width. Had the compositor
interpreted the image with its own canted FOV instead, that pixel would sit at
`atan(tan(−54°) + 0.5054·(tan 40° − tan(−54°))) ≈ −14.4°` in view 0 and +14.4°
in view 1 — about 29° of divergence, which is the original unfusible defect and
would have shown as two bars. One bar at 2 m means `XrCompositionLayerProjection
View::fov` was honoured exactly as `do_projection_layer` reads it.

So X4 may render one symmetric ±55° frustum per eye and say so, and the
compositor does the rest. No shader change, no off-axis affine, no per-eye
rotation.

**What this run does not establish**, and should not be read as: it says nothing
about the *cost*. The source here is painted once and copied; X4's eye image is
the output of a full frame, the copy shares X4's one graphics queue, and #36's
mutex is still unmeasured. That is the submission take's business.

# State at `stage8-xr-session-in-x4` — resume here

Written to survive a context compaction. Everything below is checkable from the
repository or from `/tmp`; nothing depends on remembering a conversation.

## Where the work stands

Closed: **#23** (per-draw `sx`), **#24** (a chosen field of view), **#31** (the
three extents), **#32** (the right eye from present dumps), **#34** (an
`XrSession` on X4's own instance, device and queue, running a 90 Hz pose loop
inside the game while it plays flat), **#36** (the shared graphics queue,
serialised — built, and *unmeasured* because nothing submits yet).

Open: **#25** (deferred by decision), **#33** (head tracking), and **#35**, which
is the next task and is where work stopped.

Tag **`stage8-xr-session-in-x4`**, take 113, `score_run.py` exit 0. The command
line and what to check are in `docs/known-good-runs.md`. The one change from
`stage6-sx-per-draw` is `X4VR_VR=1`.

## The expensive lesson, so it is never re-learned

Takes 111, 112 and 113 were all one question: **`XR_KHR_vulkan_enable2` is the
wrong extension for a Vulkan layer.** It takes a `pfnGetInstanceProcAddr`; a
layer only has a *down-chain* one; handing that over is the only way the layer's
internal handle space ever reaches the runtime. Take 111 aborted X4 in
`vkGetPhysicalDeviceMemoryProperties` ("Invalid physicalDevice"); take 112 was
rejected by `oxr_session.c:1151`, which requires the graphics binding to carry
exactly the handle `xrGetVulkanGraphicsDevice(2)KHR` returned.

The layer uses **`XR_KHR_vulkan_enable` (v1)**, which returns *lists of
extension names* and has no such channel. Do not "simplify" it back to enable2.

Three handle spaces exist and take 113 printed all three in one line:

    vr: physical device handles — layer 0x123ab150, loader public 0x123ae500,
                                  runtime asks for 0x123ae500

The runtime's handle is the **loader's public** one; the layer's is different.
Left unexplained on purpose: why take 111's `want == phys` check agreed at all,
given Monado uses the public `vkGetInstanceProcAddr` for that query
(`oxr_api_system.c:314`/`:333`). The enable2 path is abandoned and nothing
depends on the answer.

## Facts measured, not to be re-derived

**The runtime** (WiVRn 26.6, Quest 3), from the first headset probe run and
take 113 (the probe file no longer exists — see "A measurement I destroyed"
below; these values are also quoted in the take-111 section above):

    2 views, PRIMARY_STEREO       recommended 3096x3243 per eye (max 6192x6486)
    view 0 fov  -54 / +40 h       view 1 fov  -40 / +54 h      both +44 / -55 v
    IPD 0.0630 m, view 1 at +x    reference space STAGE        format 43 (SRGB)
    3 swapchain images            frame loop pinned at 90.01 Hz

3096×3243 is **10.04 Mpx per eye, 5.1×** the current 1408×1408.

**The instance/device extensions the runtime adds** (take 113 log, and X4 needed
all of them): instance — `external_fence_capabilities`,
`external_memory_capabilities`, `external_semaphore_capabilities`; device —
`dedicated_allocation`, `external_fence`/`_memory`/`_semaphore` plus all three
`_fd` forms, `get_memory_requirements2`, `image_format_list`. None dropped by
the driver-support filter.

**The queue**, from `vulkaninfo` and confirmed in-game: RADV Navi31 has
**exactly one graphics queue** (family 0, `queueCount 1`; family 1 is 4× compute;
the rest video/sparse). The runtime shares X4's, serialised by one layer-owned
mutex taken in `x4vr_QueueSubmit` and across the whole of `x4vr_QueuePresentKHR`
(which covers the SBS composite, the cursor overlay and the dump `QueueWaitIdle`),
and around `xrEndFrame` on the XR thread. `xrWaitFrame` is deliberately outside
it. Still unhooked and carrying the same rule: **`vkQueueBindSparse`**.

## #35 — the next task, with its arithmetic already done

X4's projection is symmetric (`row0(P) = [sx 0 0 0]`, no off-axis term); the
runtime's views are canted 14° outward. Submitting as-is puts every object ~15°
off, in opposite directions per eye. The test card demonstrated it at **30.07°
of divergence** and it is reproduced as a negative control in
`x4vr_test_xr_probe selftest`.

The fix is one multiply-add on a value `gl_Position` already carries. For a
symmetric render at half-angle `t` and a target frustum `[tanL, tanR]`:

    x_c' = A·x_c + B·w_c      A = (2/(tanR−tanL)) / cot(t)
                              B = −(tanR+tanL)/(tanR−tanL)

    at t = 55°:  view 0  A = 1.2892  B = +0.2425
                 view 1  A = 1.2892  B = −0.2425

X4 must render the union so nothing is culled: **±55°, i.e. `X4VR_FOV = 1.4917`**
(today 1.437 = 106°). The clip map then crops each eye to its real frustum at
full density. Rejected alternative: submitting a symmetric FOV of our own, which
costs **1.48×** the frustum area in pixels the headset never shows.

**Both open items are now settled** — see "Task #35 — the affine, derived and
locked offline" above. The vertical sign comes out positive (`A_y = −2/(span·sy)`
with `sy` negative), the composition is shear-then-affine, and both live in
`common/x4vr_view.hpp` (`make_off_axis`, `apply_off_axis`) under 22 passing cases
in `tests/view_math.cpp`. The vertical needs no separate FOV: the eye is square,
so `X4VR_FOV = 1.4917` covers both axes.

**What is left of #35 is the SPIR-V, not the maths** — and it is an
optimisation, not the next thing to build. Probe run 2 confirmed P115.1: the
view poses are **parallel**, so the per-eye difference is the lateral offset the
layer already applies and first light needs no new vertex math at all. A
symmetric submitted FOV reaches the headset with no shader change, at 1.54× the
fill; the affine removes that 1.54× but touches every world vertex module *and*
the `M_invprojection` correction. The table above compares them.

**The next thing to build is the submission itself** — swapchain, copy, layer —
which is milestone A and needs nothing from #35.

## Decisions taken, so they are not re-litigated

* `X4VR_PROJ_MVP` defaults **on** (takes 109/110); `=0` restores stage5.
* `X4VR_PROJ_LIVE` still defaults **off**; every take since 52 sets it
  explicitly. Defaulting it on is a claim about 326 modules rather than 12 and
  deserves its own control run.
* `X4VR_VR` defaults **off**. It needs a runtime *running* — `active_runtime.json`
  exists only while WiVRn or SteamVR is up, so its absence is "not started",
  not "not installed".
* `X4VR_FOV=1.437` is 106°; the law is `fov = target° / 73.7399`.
* The layer **never links** `libopenxr_loader` — it is `dlopen`'d, because a
  missing `DT_NEEDED` in an injected layer is X4 refusing to start. Verify with
  `ldd build/layer/libVkLayer_X4VR_core.so | grep -i openxr` (must be empty).
* Verdict changes: takes 97/98 FAIL→PASS (#32); take 101 PASS→FAIL (#31).
* Current verdicts over all 80 logs: **60 pass, 6 fail
  {44, 45, 48, 101, 102, 112}, 13 unscoreable**.

## Tooling — what exists and what each answers

* `tools/score_run.py <log>` — the acceptance check for every run. Sections:
  `split`, `masked`, `extents`, `swapchain`/`eye`, `proj`, `stereo`, `vr`,
  `perf`. Exit 0 pass, 1 fail, 2 unscoreable. The `vr` section is gated on
  **intent** (`X4VR_VR=1` on the command line), never on a session having
  appeared. **Re-run it over every log in `/tmp` after any change to it.**
* `tests/run-xr-probe.sh [v1|enable2] [seconds]` — the OpenXR bring-up on a real
  GPU. First half is the eye test card with **no runtime needed** and three
  cases, two of which must fail. Second half needs WiVRn running. Defaults to
  the **v1** path, which is the one the layer uses; `enable2` runs the path
  `stage7-xr-session-proven` was taken with.
* `tools/eye_stereo.py <dump-prefix>` — per-region horizontal disparity from
  present dumps; called automatically by `score_run.py`.
* `tests/eye_stereo_selftest.py`, `build/tests/x4vr_test_spirv_patch`,
  `build/tests/x4vr_test_view_math`, `build/tests/x4vr_test_share`.
* `tests/run-multiview-render.sh`, `run-cursor.sh`, `run-multiview-enable.sh` —
  run all three **with and without `X4VR_VR=1`**; the VR path changes device
  creation, and a matcher there already broke once on a new log line.

## Data on disk, and the rule that goes with it

    80 run logs                     /tmp/x4vr-takeNN.log
    585 present-dump frames         /tmp/x4vr-t{93,94,97,98,99,101,103}-present-*
    3 shader dumps WITH a log       /tmp/x4vr-shaders-take{61,74,80} (397 each)
    1 shader dump WITHOUT a log     /tmp/x4vr-shaders (409) — unusable
    2 headset probe runs            /tmp/x4vr-xrprobe-20260809-1753{26,58}.txt
                                    (the second is OXR_PARALLEL_VIEWS=1)
    3 no-runtime probe failures     the other /tmp/x4vr-xrprobe*.txt, 956 bytes
                                    each — none of them is data

**Module serials are per-run.** A number from one log may only be opened in the
dump directory of that same run. Sweep `/tmp` before specifying any run.

### A measurement I destroyed, and the two labelling errors it exposed

`tests/run-xr-probe.sh` wrote to a fixed `/tmp/x4vr-xrprobe.txt`. Running it
twice on a machine with no runtime attached — to verify an unrelated shell fix —
replaced the 6583-byte headset measurement with 956 bytes of
`XR_ERROR_RUNTIME_UNAVAILABLE`. There was no second copy. The clobbered file is
kept as `/tmp/x4vr-xrprobe-clobbered-noruntime.txt` so it cannot be mistaken for
data.

**What survived:** every number that mattered was already transcribed into this
file (the take-111 section and "Facts measured, not to be re-derived"), and the
run is repeatable. Nothing in the analysis rests on the lost bytes.

**The line above it in this file was already wrong.** It named
`/tmp/x4vr-xrprobe-run1.txt` as "the first headset probe run"; that file is
956 bytes and has always been a no-runtime failure. So the file's own inventory
pointed at the wrong artifact before anything was overwritten — the kind of
error that only surfaces when someone tries to open the file it names.

**The fix is in the harness, not in a habit.** The probe now defaults to
`/tmp/x4vr-xrprobe-<timestamp>.txt`, per run, for the same reason `X4VR_LOG` is
per run: a failed run must not be able to overwrite a good one.

## The Monado source is on disk, and reading it settled two takes

    ~/.local/share/envision/wivrn/build/_deps/monado-src/src/xrt/state_trackers/oxr/

`oxr_session.c`, `oxr_session_gfx_vk.c`, `oxr_vulkan.c`, `oxr_api_system.c`.
Two takes were spent guessing at behaviour that is fifteen lines of C. Read it
first. (The Monado *builds* under `envision/` are stale — 11 unresolved
`DT_NEEDED` each — so there is no local runtime to test against without the
headset; rebuilding with `-DXRT_BUILD_DRIVER_REALSENSE=OFF -DXRT_HAVE_OPENCV=OFF`
is the cheap way to get one if offline iteration ever becomes the bottleneck.)
