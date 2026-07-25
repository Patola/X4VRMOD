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

## Still to determine

- Whether block #2 (offset 3584) is *stably* the main view across frames, or
  ring-moves (needs a second capture / runtime logging from the layer).
- The exact remaining ~1088 bytes of the 1792 block (per-view params that may
  also need per-eye patching, e.g. camera world position for specular).
- Present-time details: swapchain image count, present mode, and how pass 47
  maps onto the swapchain image (single full-screen draw — easy overlay
  point).
