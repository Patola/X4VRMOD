# Frame analysis (Phase 2, in progress)

Renderdoc findings on X4 9.0 (RADV, RX 7900 XTX). This is the empirical
basis for the double-render stereo mechanism; keep it updated as we learn
more.

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
* The **exposure reductions are still masked** and should not be — shared
  exposure, or the eyes auto-expose independently and flicker against each
  other. Invisible until `K` differs.
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

Stage 2 is next, and it is the first change that makes the two layers *differ*
on purpose: per-eye camera constants selected by `gl_ViewIndex`. Before that,
two pieces of groundwork — the UI has to become a single two-layer image
(`SbsCompositor`; the swapchain cannot take a second layer), and the exposure
reductions (4096×1, passes 55/57/59) have to be un-masked so both eyes keep
sharing one exposure value rather than drifting apart.
