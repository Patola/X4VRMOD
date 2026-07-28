# Phase 4b test plan — doubling the frame

Written **before** the change, so the predictions below cannot be retrofitted
to whatever actually happens. Baseline for every comparison is tag
`mv_partition_measured`.

## What changes

1. A `per_eye` classification, separate from `unsheared`.
2. `arrayLayers = 2` on the ~21 attachment images that back per-eye passes.
3. `viewMask = 0x3` on those passes.
4. Array image views substituted at `vkCreateFramebuffer`.
5. **`K` identical for both views** — no stereo yet.

## The property under test

Because `K` is identical for both views, **the visible output must not
change**. That is the entire point of staging it this way: if the screen
changes, the cause is the doubling, not the stereo maths. One suspect, not
two.

Corollary worth stating plainly, because it is easy to over-read a pass:
**this stage cannot detect an error that only manifests when the eyes
differ.** See "What these gates cannot catch" at the end.

---

## Gate 0 — offline, no game (seconds)

* Build clean; `tests/run-multiview-enable.sh` still passes all four cases.
* New `tests/multiview_double.cpp`, mimicking X4's call shape: create a
  1-layer image, a view, a render pass, a framebuffer.
  * With the layer: image comes back **2 layers**, pass carries a view mask,
    framebuffer **validates clean**.
  * Control (`X4VR_MV=0`): image stays **1 layer**, no view mask. A suite
    where the passing case is the only case proves nothing.
  * Third case: app creates a `VK_IMAGE_VIEW_TYPE_2D` view with explicit
    `layerCount = 1` over a doubled image — must still validate. This is the
    path X4 takes whenever it samples one of these images as a texture.

Validation is the oracle throughout: an attachment that should have been
doubled and was not is a hard VUID error at framebuffer creation, naming the
framebuffer and the attachment index. We never have to guess that set.

## Gate 1 — live plumbing (one run, main menu is enough)

Run with `VK_LOADER_LAYERS_ENABLE=...,VK_LAYER_KHRONOS_validation`.

| Check | Pass |
|---|---|
| Validation errors | **zero** |
| Layer summary | `doubled=N masked=M substituted=S fallbacks=0` |
| Game reaches the menu | yes |

**`fallbacks=0` is the load-bearing one.** If an attachment cannot be
upgraded, the layer must say so loudly and by name, never silently leave it
single-layer — a silent fallback is exactly the failure that would surface
three gates later as an unexplained artifact.

Failure here is plumbing. Nothing about stereo is implicated.

## Gate 2 — layer 0 and layer 1 are identical (the critical gate)

Everything above can pass while **layer 1 is never rendered at all**: the view
mask is set, the framebuffer is valid, and we would not notice, because what
gets presented is layer 0.

Add `X4VR_MV_PRESENT_LAYER=0|1` and dump both layers of the same frame.

* **Primary check — bit-exact.** With identical `K`, identical descriptors and
  the same command stream, the two views differ only in `gl_ViewIndex`, which
  nothing reads yet. Same hardware, same inputs, so floating point is
  deterministic: **layer 0 must equal layer 1 byte for byte.**
* Failing that: black, garbage, or a partial image means the second view is
  not being shaded.
* A *small* difference is worse news than a large one — it means something
  reads per-view state we did not account for.

This gate is exact and needs no cross-run camera alignment, which makes it far
stronger than any screenshot comparison. It is the one to run first.

## Gate 3 — the cost must be visible

Doubling fragment work has to cost something.

* **Expect:** a measurable frame-time increase, but well under 2× overall —
  shadows and exposure are untouched, and CPU-bound stretches do not change.
* **Suspicious: no change at all.** That suggests layer 1 is not being shaded,
  i.e. Gate 2 passed for the wrong reason.
* **Also suspicious: far worse than 2×** on the doubled work — that points at
  something pathological, such as lost compression or per-view state thrash.

Note the inversion: **here a performance regression is confirmation, not a
bug.** A free change would be evidence of failure.

## Gate 4 — visible output unchanged (optional)

Formal version of "does the game still look right". Only worth the runs if
something looks subtly off, since Gate 2 already proves both views render and
the eyeball check catches gross breakage.

X4 animates, so two frames are never bit-identical across runs. A naive
"difference must be zero" test would fail for reasons unrelated to the change,
so the noise floor has to be measured, not assumed:

* **Control:** two screenshots from the *same* build, same save, same pose →
  baseline difference `N`.
* **Signal:** one screenshot from `mv_partition_measured`, one from the new
  build, same save and pose → difference `S`.
* **Pass:** `S ≈ N`. **Fail:** `S >> N`.

Reuse the starfield-correlation trick from `tools/measure_parallax.py` as the
camera-alignment check: if the poses do not match, the stars move, and the
comparison is void rather than failed.

---

## Predicted failure modes

Committed in advance. Anything here that does *not* happen is as informative
as anything that does.

1. **View type vs layer count.** X4 creates a `VK_IMAGE_VIEW_TYPE_2D` view
   with `layerCount = VK_REMAINING_ARRAY_LAYERS` over an image we doubled →
   now invalid, since a 2D view cannot span 2 layers. *Fix:* clamp
   `layerCount` to 1 for non-array view types. **Most likely failure.**
2. **Framebuffer layers.** Multiview requires
   `VkFramebufferCreateInfo::layers == 1`. Clamp if X4 passes more.
3. **Suballocator pressure.** Doubling changes
   `vkGetImageMemoryRequirements`. If X4 sub-allocates from pre-sized pools,
   this can fail or silently corrupt. Watch for `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
4. **Copies and clears.** `vkCmdCopyImage` / `vkCmdBlitImage` /
   `vkCmdClearColorImage` with `VK_REMAINING_ARRAY_LAYERS` now touch both
   layers. Usually benign; a copy *into* a doubled image from a single-layer
   source is not.
5. **Compute writes.** A compute shader writing one of these as a storage
   image writes layer 0 only. Mostly disabled by the config overrides, but the
   `R8_UINT` and `R16_SFLOAT` targets are candidates.
6. **DCC.** We already force `nodcc`; doubling changes image layout. If the
   saturated-block artifact returns, that is why.

## What these gates cannot catch

Errors that are invisible while both eyes render the same thing:

* **The exposure reductions (`4096×1`, passes 55/57/59) are still classified
  STEREO.** If they get doubled, exposure becomes per-eye — which shows
  nothing at this stage, because both eyes see the same scene, and only
  appears as inter-eye flicker once `K` differs. *Verify from the log that
  they were not doubled; do not rely on the screen.*
* Any pass wrongly in the shared set: with identical `K` its output is correct
  for both eyes by coincidence.
* Sampling the wrong layer: layer 0 and layer 1 hold identical content, so
  reading the wrong one is undetectable.

These are all deferred to the stereo stage by construction. The point of
listing them is so that "all gates green" is not mistaken for "the partition
is right" — it means "the plumbing is right".

---

## What failure looks like on screen

Written before the runs, so "it looked fine" can be checked against something
rather than felt.

### Gate 1 (doubling on, both views read from layer 0)

Ranked by how likely the change makes them:

| Symptom | What it would mean |
|---|---|
| Wrong or black textures on specific materials | The `layerCount = 1` clamp on sampled views is wrong. It touches **every read of every doubled image**, so it is the single riskiest edit in the change. |
| Saturated colour blocks, banding | Doubling changed the image layout and with it the driver's compression path — the DCC signature we already know, arriving by a new route. |
| Flat, black, or haloed lighting | The main depth buffer is doubled; deferred lighting reconstructs position from it. A wrong stride or layer would break shading, not geometry. |
| Shadows missing, or everything shadowed | The shadow atlas is doubled (and wasted). If sampling drifted to the empty layer, shadows vanish. |
| Scene too bright/dark, or pumping | The exposure reduction is doubled and masked. Invisible while both views match — if it shows here, they do not. |
| Blurry textures, pop-in, new stutter | 565 MB less VRAM for streaming. Most likely at a station, least likely in open space. |

**What Gate 1 cannot prove.** Everything downstream reads layer 0, so this gate
shows only that *doubling did not break the layer-0 path*. It says nothing
about whether layer 1 was ever rendered. A clean Gate 1 is real but narrow —
which is the entire reason Gate 2 exists.

### Gate 2 (`X4VR_MV_PRESENT_LAYER=1`, everything reads layer 1)

The diagnostic signature is specific, because the UI never passes through a
doubled image — it is drawn after the per-eye chain, into an LDR target we did
not touch:

* **Correct:** indistinguishable from Gate 1. Same scene, same HUD.
* **Second view never shaded:** a **black or garbage 3D scene with a perfectly
  normal HUD drawn on top of it.** That combination is unmistakable and cannot
  be produced by anything else in this change.
* **Second view partially shaded:** geometry present but lighting, shadows or
  post missing — i.e. some passes carry the view mask and others do not. This
  is the informative middle case; note *which* effects survive, because that
  names the passes that took the mask.

## Follow-up: the doubling is 4× wider than it needs to be

Measured live: **92 images doubled, 565.6 MB**, against **21 images and
135 MB** actually used as per-eye attachments.

That is the permissive rule working as designed — at `vkCreateImage` the
precise question cannot be asked — but the justification ("memory is not the
constraint") was argued on a 24 GB card. It is not a safe argument on 8 GB.

**Tightening is low-risk, and for the same reason the rule was loose to begin
with:** cutting too far is caught by validation, which names the framebuffer
and attachment it wanted doubled. So the narrowing can be aggressive and
iterative rather than cautious. Candidate signals available at creation time,
none of which needed guessing before we had the live inventory:

* `D16` depth is the shadow atlas; the main depth buffer is `D32_SFLOAT`.
* Extents unrelated to the render size (or a clean downscale of it).
* `TRANSIENT_ATTACHMENT`-only images, which never survive a pass.

Deferred until stereo works, so that a tightening regression cannot be
confused with a stereo bug.
