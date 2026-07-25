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

## Still to capture (next Phase-2 steps)

- The **VkBuffer + descriptor binding** for this UBO, and **how it is
  updated** (vkCmdUpdateBuffer? persistently-mapped host write? staged copy?)
  — this determines *where* we patch it per eye.
- Whether it is written **once per frame** or multiple times (e.g. a separate
  copy for shadow passes with the light's view).
- Which **passes/draws** read it (should be all main-view passes:
  depth/G-buffer, lighting, forward, post).
- The **UI/HUD pass** location and how it composites (per-eye vs single depth
  plane).
- The **final present** image and whether intermediate targets are full-frame
  (2816×1408) or already split — decides "render eye into a half" vs "render
  eye full then composite".
