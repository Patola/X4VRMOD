# X4 modding quirks, pitfalls and traps

Things about X4: Foundations (native Linux, 9.00) and its surroundings that
cost us time, surprised us, or silently did the wrong thing. Every entry is
something we hit *live* — not documentation, not inference.

[docs/frame-analysis.md](frame-analysis.md) is the reference for how the
frame is built. **This file is the reference for what will bite you.**

Each entry says what happens, how it presents (the symptom you will actually
see), and what to do. When a quirk cost us a wrong turn, that is recorded
too — the wrong turn is usually the useful part.

---

## 1. Process and environment

### X4's process is called `Main()`, not `X4`

`pgrep -x X4` and `pkill -x X4` **never match**. The process name is
`Main()`.

*Symptom:* cleanup commands silently succeed while the game is still
running; "lingering X4" that nothing seems to kill.

*Do:* match the command line — `pkill -f "X4 Founda[t]ions/X4"`. The bracket
keeps the pattern from matching the issuing shell's own command line, which
otherwise makes `pkill` return 144 and kill your terminal's process instead.

### Vulkan layer environment is inherited by every child process

`VK_INSTANCE_LAYERS` / `VK_LOADER_LAYERS_ENABLE` are environment variables,
so under gamescope the layer is loaded into **gamescope and its Xwayland**
as well as into X4. All of them create Vulkan instances and swapchains.

*Symptom:* the layer's log shows extra devices and swapchains at sizes you
never asked for (we saw 2816×1385 and 2816×1357 alongside X4's 2816×1408);
any present-time effect gets applied a second time on the way to the display.

*Do:* gate on the application. `VkApplicationInfo::pApplicationName` is `X4`
for the game and `gamescope` for gamescope; fall back to the executable name
since `pApplicationInfo` is optional.

### `LD_PRELOAD` is inherited too

The injector is loaded into `dirname`, `bash`, `xkbcomp`, `gamescope`,
`Xwayland` — everything the launcher spawns. Harmless as long as every
interposer is a pass-through by default, but it means log noise and it means
an interposer bug can break unrelated programs.

---

## 2. `config.xml`

### X4 rewrites `config.xml` repeatedly *during* a session

Not just at exit. Observed live: the write count kept climbing while the game
was simply running.

*Cost us:* an entire design. The original scheme let X4 write the file and
restored the player's values afterwards, which required distinguishing "X4
saved our injected value" from "the player changed this in the options menu"
— a heuristic that ran constantly and got it wrong. Three offline tests
passed and could not have caught it.

*Do:* give the mod its own file (`config-x4vrmod.xml`) and redirect writes
into it. See [../injector/README.md](../injector/README.md).

### Overrides have knock-on effects on settings you do not manage

Forcing `fullscreen=false` made X4 change `presentmode` from `immediate` to
`mailbox` **on its own**.

*The lesson:* "we only touch our own tags" is **not** the same as "the
player's config is unaffected". Your overrides change the conditions X4
evaluates its other settings under. There is no way to enumerate the
derivations — which is the second reason the profile approach wins: it does
not need to.

### Invalid enum values are accepted silently

X4 does not reject an unknown value, does not log, and does not fall back
visibly. It round-trips the bad value through the config file happily.

*Symptom:* the **options menu shows `--`** for that setting, meaning it
matched no known option. That is the only signal. Everything else looks like
it worked.

*Cost us:* `pom="off"` looked correct for a whole session. POM was never
actually disabled.

*Do:* verify a new value by setting it in-game once and reading the file
back. Never guess an enum string.

| Tag | Menu label | Stored value |
|---|---|---|
| `antialiasing` | Off | `none` |
| `pom` | Off | `none` (**not** `off`) |
| `pom` | Low / Medium / High | `low` / `medium` / `high` |

### `fmemopen()` cannot be used to serve a config file

An `fmemopen()` stream has no real file descriptor, so `fileno()`/`fstat()`
in X4's XML reader fail.

*Symptom:* X4 silently ignores the file and uses its built-in defaults — the
game launches at desktop resolution for no visible reason.

*Do:* use `memfd_create()`. Real fd, still purely in memory.

---

## 3. Window and resolution

### `res_width` / `res_height` are conditional on `borderless`

| `borderless` | What X4 does with the requested size |
|---|---|
| `false` | Honours it, **minus the window decoration** (1408 → 1385) |
| `true` | **Ignores it** and sizes to the display |

Measured with an identical served config: 2816×1408 under a 2816×1408
gamescope, 3440×1440 on a 3440×1440 desktop.

*Symptom:* **none in any log.** The override is applied to the config buffer
exactly as expected in both cases. The only evidence is the swapchain extent.

*Cost us:* a run that looked like a regression in the config code. It was
not — earlier runs were correct only because gamescope's display happened to
*be* the size we wanted.

*Do:* run under gamescope sized exactly to the target, and check the
swapchain extent rather than the config. The layer warns on a mismatch.

### The WSI backend changes where X4 gets its resolution from

| Backend | `currentExtent` | X4 sizes itself from |
|---|---|---|
| X11 / Xwayland | the real window size | **`currentExtent`** — `res_width`/`res_height` ignored |
| Wayland | `0xFFFFFFFF` ("no preferred size") | **`res_width`/`res_height`** — the only source left |

So the "`borderless` makes X4 ignore the configured resolution" rule above is
**X11-specific**. On Wayland the config is authoritative again.

X4 links **SDL3**, which renamed the driver variable: SDL2 reads
`SDL_VIDEODRIVER`, SDL3 reads **`SDL_VIDEO_DRIVER`**. Clearing
`WAYLAND_DISPLAY` alone does *not* force X11 — SDL's Wayland driver still
connects to a default socket.

*Cost us:* two live runs. The launcher had cleared `WAYLAND_DISPLAY` and its
comment said the game "runs on gamescope's XWayland", which was simply not
true, and nothing checked.

### On Wayland, the presented buffer size *is* the window size

If the layer presents an image wider than the window, the surface grows to
match, the application sees a resize, and recreates its swapchain at the new
size — silently undoing any scheme that has the app render smaller than what
is presented.

*Symptom:* it works for exactly one swapchain generation, then reverts.

*Do:* anything that decouples render size from presented size needs **X11**,
where the window size is owned by the window manager and is independent of
the swapchain buffer.

### X4 lays out its UI from the *window* size, not the swapchain extent

The two are normally identical, so nothing reveals this until you deliberately
decouple them — which is exactly what a half-width split render does.

*Symptom:* the game appears to draw only the left portion of the screen, and
that portion is then duplicated by the SBS composite. Reported live as "two
left halves". Crucially the **mouse still behaves as if the full width were
there**: to click a menu item you must aim at where it would have been on the
wide screen, and the right half's hitboxes exist but are never drawn.

*Cause:* the swapchain extent was halved (1408 of a 2816-wide window), but X4
keeps asking SDL for the window size and lays the HUD, menus and hit-testing
out for 2816. Render target and UI coordinate system disagree.

*Do not* diagnose this as a composite bug. The composite is doing its job; the
frame it is given is genuinely one eye's worth of a mis-laid-out UI.

*Consequence for stereo:* the render size must stay equal to the window size.
That rules out "wide window, narrow render" as a way to get side-by-side, and
is the reason the second eye is pursued through **multiview layers** rather
than horizontal splitting — an extra layer is invisible to X4's sizing, an
extra half-width is not. It also compounds with the CPU-side hit-testing in
§4: X4's input mapping consistently follows the window, never the GPU.

### X4 creates several swapchains at startup

Typically three, back to back, before the first frame. Anything you attach to
a swapchain must handle being set up repeatedly and torn down.

### X4 requests more than one queue family

*Symptom:* none, until you submit a command buffer to a queue whose family
does not match its pool's — undefined behaviour, no validation error unless
you are running the validation layer.

*Do:* track `VkQueue` → family through `vkGetDeviceQueue`/`vkGetDeviceQueue2`
and build pools for the family that actually presents. Do not assume family 0.

---

## 4. Rendering

### X4 renders camera-relative — `M_view` is always identity

Surveyed all 128 blocks of the view arena: `M_view` is identity in every one.
Object transforms are baked against the camera on the CPU (normal for a space
sim, to avoid float precision loss at astronomical distances).

*Symptom:* writing `M_view` / `M_viewprojection` / `M_viewinverse` has **no
visual effect at all**, while zeroing the whole arena turns the screen black
— proving the buffer *is* consumed.

*Why:* geometry is transformed by `M_worldviewprojection` in the *per-object*
set-3 block. The set-1 camera block feeds deferred lighting and post
(`M_invprojection`, `V_cameraposition`, …). Both need patching, for different
reasons.

### The UI is hit-tested on the CPU, in unshifted screen space

*Symptom:* transform UI geometry on the GPU and it draws in the new place but
**clicks in the old one**. To select an item you must put the mouse where it
would have been, which can be far from where it appears.

*This is a correctness constraint, not cosmetics.* Any VR repositioning of
menus must apply the inverse transform to pointer coordinates before X4 sees
them — in the `LD_PRELOAD` injector, not the Vulkan layer.

Menu, cockpit HUD and map share one reference frame, so they move together.

### Shadow passes reuse the same shader modules as main geometry

X4 bakes the light-space matrix into the same `M_worldviewprojection` slot,
so **no static analysis of a shader module can tell them apart**.

*Do:* separate them at **pipeline** creation, where they are cleanly
distinguishable — shadow passes are depth-only (2048×2048 D16, no colour
attachments), and shadow and main passes share **zero** pipelines. Keep both
module variants and choose per pipeline. Static, zero per-frame cost.

The same mechanism separates the UI: passes 46/47 are 8-bit UNORM/SRGB, while
every world pass writes multi-attachment float targets.

### Antialiasing off paints the frame with saturated RGB blocks (RADV)

With `antialiasing=none` — which the mod forces — the frame fills with a
regular grid of saturated red/green/blue/magenta/cyan/yellow blocks over
otherwise-correct rendering. Reproduces reliably in the map view. FXAA shows
it too, MSAA less so, **TAA not at all**.

*Cause:* stale **DCC** (Delta Color Compression) metadata. Confirmed by
elimination on Mesa 26.1.5 / RX 7900 XTX: `RADV_DEBUG=nodcc` clears it
completely.

*Do:* `RADV_DEBUG=nodcc` (the launcher forces it; `X4VR_NODCC=0` opts out).
Costs memory bandwidth.

*Not* `RADV_DEBUG=zerovram`. That addresses a different, older X4 bug fixed
in Mesa around 25.1, and does nothing here — the artefact looks like
uninitialised memory but the many *distinct* saturated colours in fixed-size
blocks are the giveaway for compression metadata rather than garbage VRAM. A
fast-clear bug would paint blocks of a *single* colour instead.

*Why not just use TAA:* it is clean, but it keeps a history buffer. Once two
eyes are rendered, one eye's history blends into the other's frame — per-eye
ghosting with no fix available from a layer. The jitter is the easy half
(`M_jitter` / `M_projection_uj` are mapped); the history is not.

### Setting `pom` to Off can break X4's own shader compilation

With POM genuinely disabled (`pom=none`) *and* our other effect overrides
active, X4 fails at startup:

```
Failed to compile shader 'shadergl\ogl\p2_complex_water'!
ERROR: common_frag.glsl:1732: 'textureLod' : no matching overloaded function
ERROR: common_frag.glsl:1732: '=' : cannot convert from 'const float'
```

Consistent with POM's height-map sampler being compiled out while a use of it
survives — a shader variant Egosoft never builds.

*Not* `pom=none` alone: vanilla with `pom=none` starts. *Not* our other
overrides alone: `ssr=false` was in effect during a run that reached the
cockpit. It is the combination.

*Symptom:* a fatal error dialog, exit code 1119, **no core dump** (X4 exits
cleanly rather than crashing). Intermittent-looking, because whether it fires
depends on whether the scene needs that shader.

*Do:* leave `pom` alone. Revisit only with the error reproduced and
understood.

---

## 5. Debugging the environment, not the game

### gamescope always core-dumps at shutdown

```
[gamescope] [Info]  launch: Primary child shut down!
terminate called without an active exception
```

Harmless. "without an active exception" is **not** a thrown exception — it is
`std::terminate` from a `std::thread` destroyed while still joinable, inside
gamescope's teardown, *after* X4 has already exited. Reproduces with
`X4VR_NO_LAYER=1 X4VR_NO_INJECT=1`. gamescope 3.16.24, `--backend sdl`.

*The trap:* every `coredumpctl` listing during a debugging session shows a
fresh gamescope core that looks like evidence.

*Do:* check the executable and signal before concluding anything.

| Core | Size | Signal |
|---|---|---|
| X4 actually crashed | ~430 MB | SIGSEGV |
| gamescope teardown (ignore) | ~1.8 MB | SIGABRT |

### gamescope's own window steals height from its nested display

If gamescope's host window is decorated, the titlebar takes 23px off it
(1408 → 1385). gamescope then scales its square nested display down to fit
and pads the sides, which shows up as thin black bars left and right of the
game — nothing to do with the game's own rendering.

*Do:* run gamescope with `-b` (borderless). The launcher does;
`X4VR_DECORATED=1` opts out and accepts the bars.

### Leaving the pilot seat pins the view to the floor under gamescope

Leave the seat to walk around the ship and the first-person view locks
looking down and cannot be raised. Walking still works.

*Cause:* gamescope switches between relative and absolute mouse mode
**depending on whether the cursor is visible**. X4 hides the cursor and
switches to mouse-look when you leave the seat; if gamescope is still in
absolute mode, X4's warp-the-pointer-to-centre has no effect and it reads the
same offset every frame. Walking is unaffected because it is keyboard.

*Do:* run gamescope with `--force-grab-cursor` (always relative). The
launcher does; `X4VR_GRAB_CURSOR=0` opts out. Verified live to fix walking
while leaving mouse steering, direct mouse steering, the map and the menus
working — the obvious worry, that forcing relative mode breaks the menu
cursor, does not happen because X4 draws and moves its own cursor.

*The broader point:* this is the cursor shim's problem showing up early. X4
and the compositor disagreeing about who owns the pointer is the same
boundary as X4 hit-testing its UI on the CPU in unshifted screen space. Any
VR pointer design has to own the relative/absolute switch that happens on
leaving the seat.

### Relaunching gamescope too quickly fails

```
Failed to bind socket @/tmp/.X11-unix/X0: Address already in use
```

The previous gamescope's Xwayland has not released the display yet. Killing
gamescope and sleeping one second is not enough.

*Symptom:* X4 starts and dies after ~2 frames.

*Do:* wait for the process to actually exit before relaunching.

### Screenshots of the game are not capturable the usual way

`import` and `ffmpeg x11grab` against gamescope's display return black
(direct scanout), and the window may not be visible to `xdotool` at all.

*Do:* capture the host session instead and crop, or **just ask Patola** — he
is watching the screen and can take one instantly. Do not chain capture
workarounds.

---

## 6. Testing lessons

- **Offline tests cannot catch X4's runtime behaviour.** Three passing
  offline config tests missed that X4 rewrites `config.xml` mid-session. If a
  quirk is about *when* or *how often* X4 does something, only a live run
  finds it.
- **Plain Vulkan validation is not enough for present-path work.** It catches
  layout and region errors but not read-after-write hazards. Add
  `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.
- **`vkcube` is a good stand-in for X4** for anything instance/device/present
  shaped — fast, and its flat background makes a mis-composite obvious. With
  an app-name override you can make the layer treat it as the target.
- **Bisect with the launcher switches** before theorising: `X4VR_NO_LAYER=1`,
  `X4VR_NO_INJECT=1`, `X4VR_NO_CONFIG=1`. Under gamescope, `X4VR_NO_CONFIG=1`
  still gives the right resolution (the display provides it), so it stays a
  valid test of everything else.

## 7. Measuring the frame

### The probe's PPM dumps are tone-mapped, not raw

`write_ppm` applies **Reinhard then gamma** before the byte reaches the file:

    v = v / (1 + v);
    byte = powf(v, 1.0f / 2.2f) * 255.0f;

*Symptom:* brightness ratios that look plausible and are wrong by a large
factor — a defect measured at "1.7x" that is really 3.2x. Worse, a
multiplicative-versus-additive test run on these bytes measures the tone curve:
Reinhard is not affine, so a constant gain in the render produces a *varying*
ratio in the file.

*Do:* invert it before comparing brightness — `t = (byte/255)**2.2`,
`linear = t/(1-t)` — and only over a middle band. At byte 250 one code step is
15% of the reconstructed value and 255 means infinity; at the bottom, byte 1 to
2 is a factor of 4.5. `tools/shading_model.py` does this and fits only 10..235.

Alignment is unaffected: NCC is affine-invariant, so every disparity found on
the encoded bytes is still right. Only the ratios were wrong.

### A knob that returns a null may be patching the wrong thing

*Symptom:* a knob fires (the log confirms the count), coverage looks complete,
and the measurement barely moves — so the hypothesis is recorded as refuted.

`X4VR_PROJ_INVPROJ` did this for thirteen takes. It corrected camera member 2
while X4's shadow cascades read member 4. 236 fragment modules load member 2
and **2** load member 4, so every coverage metric — "100 of 138 eligible", "no
coverage hole" — was healthy while the two modules that mattered went untouched.

*Do:* before believing a shader-level null, `spirv-dis` the shader and name the
instruction that consumes the patched value. Coverage counts measure how many
modules were *touched*, never whether the right *data path* was.

*The tell:* the knob moved the measurement by **0.04%**. Exactly zero means
"never ran". Large means "ran and mattered". A **tiny non-zero means ran on the
wrong thing**, and it is the most misleading of the three.

*Do:* when one knob covers two populations of very different size, log them as
**separate counters**. A combined "236 corrected" reads healthy in exactly the
broken state; a separate line showing `0` does not.

### Compare brightness only after locking disparity, on a patch that varies

*Symptom:* ratio and difference statistics that are pure noise, from a patch
that was quietly comparing two different surfaces.

*Do:* two guards, both of which cost us a wasted measurement when skipped.
Require the patch to align above **NCC 0.9** — one attempt here aligned at 0.38
and its numbers were discarded. And require the patch to **span** a brightness
range: on a flat surface gain and offset are algebraically indistinguishable,
so any `(a, b)` fits and the answer is meaningless.

*Do:* report *why* tiles were rejected, never a bare count. "0 tiles matched"
has been misread as a verdict three times in this project and was never once
one.

### `X4VR_CLIP_K_NONWORLD` is set and nothing moves

*Symptom:* the knob is accepted, the log confirms modules were patched
(`patched vertex shader #1 (nonworld)`), and the geometry does not budge. It
looks like the vertex patch is broken.

Two independent gates decide whether `K` reaches a draw, and both must pass:

* `classify()` reads the **module** -- `World` (per-object matrix, or the camera
  block under `wide_camera`) or `NonWorld` -- and picks `K_world` or
  `K_nonworld`;
* `needs_original()` reads the **pass** -- and an unsheared pass binds the
  **unpatched** module, whatever the patched one contains. Unsheared means
  depth-only shadow, all-LDR/UI, and since take 71 *any* colour pass with no
  depth.

*Do:* look for this line, which is the whole answer when it appears:

    unsheared pipeline: using unpatched modules (shadow + UI exclusion active)

*Do:* in the offline harness, every rendered pass is colour-with-no-depth, so
the exclusion always fires and the knob is inert. `X4VR_SHEAR_NODEPTH=1`
disables exactly that exclusion and is the only way to reach
`patch_vertex_clip` there. Three cases in `run-multiview-render.sh` failed for
years on this and read as a shear regression.

*The wrong turn:* take 82 set this knob across all 54 `NonWorld` modules, saw no
change, and recorded "unsheared geometry is dead as a family". It is only dead
for passes that are *sheared* -- which the lighting passes are, so that part
stands. Modules drawn in unsheared passes were never tested by it.

*Symptom, related:* a command line copied out of `frame-analysis.md` sets
`X4VR_CLIP_K_UI`, `X4VR_CLIP_K_UI_RIGHT` or `X4VR_CLIP_SHIFT_UI` and the run
behaves as though no matrix were set.

Those were the names before the set was measured -- 54 of 350 modules, mostly
fullscreen and procedural shaders, not HUD. They are gone, with no alias and no
warning, so setting one now does nothing at all and says nothing about it. Every
take recorded before the rename uses them.

*Do:* translate `_UI` to `_NONWORLD` when reusing an old command line, or check
out the commit the take was run against -- which is the correct move anyway,
since a known-good state is code *and* knobs.

### 1408x1408 is this machine's convenience, not the mod's

*Symptom (waiting to happen):* something works perfectly here and is wrong for
anyone else, because a constant measured on one screen was written down as if it
were a property of the renderer.

The eye size is whatever the headset asks for. **It will differ per user and it
need not be square** — most HMDs are not 1:1 — and the SBS composite this
project is driven from is a bring-up view, not the target.

*Do:* derive every extent from what the runtime reports.

* **W, X4's window width** — read from the `SDL_GetWindowSize` calls X4 itself
  makes, gated on the caller being the game. gamescope asks the same question
  about its own surface and must not be mistaken for it.
* **The eye extent for a dump** — from `SbsCompositor::EyeInfo`, never a
  literal.
* **The composite half-width** — from the real swapchain extent.
* **Offline tools** — from the image being measured. `measure_parallax.py` had
  `HALF = 2816 / 2.0` baked in; it now takes the width from the frame.

*The subtle one:* a constant can be derived and still encode a layout. The input
fold's origin term defaults to `W/2`, which is correct only for a **2W-wide
side-by-side composite with X4's surface centred**. That is a geometry, not a
law, so it is overridable with `X4VR_INPUT_FOLD_ORIGIN` and the log says whether
it was assumed or forced. Only `x` is folded, because side-by-side duplicates
horizontally; an over-under layout would need the same treatment on `y`.

*Also:* `X4VR_PROJ_SX` is the projection's x scale and must match the eye's
aspect. It is a knob rather than a derivation today, which is fine while the
eye is square and is a thing to revisit when it is not.

*Audited at take 93:* no `1408`, `704`, `2816` or `1407` appears in the layer or
the injector. The two that existed were in the offline tools, and are gone.
