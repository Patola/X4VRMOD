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

> **"Always" is wrong, and the heading is left as it was written.** See
> "Correction 2" near the end of this file: 9 of 64 stored runs log a camera
> carrying a 15° yaw and an 8.70 m offset. Everything below is right about the
> camera that draws the picture and about why the eye offset went into the
> vertex stage instead; what it cannot support is the quantifier. The layer now
> decodes `M_view` **per credited camera** every run (`proj VIEW cam#N`) rather
> than printing one block once, so this is measured from here on instead of
> asserted.

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
  **no-shear twin**, whatever the patched one contains. Unsheared means
  depth-only shadow, all-LDR/UI, and since take 71 *any* colour pass with no
  depth.

  **The twin is NOT the pristine bytes.** It is built from `frag_only` -- after
  the fragment edits, before the vertex ones -- so it keeps the bindless index
  offset and the per-eye `M_invprojection` including #61's `R⁻¹`, and loses
  only the geometry edit. This bullet said "**unpatched**" until take 197 and
  that was false from take 23 onward, when the pristine-twin bug was fixed and
  this sentence was not. It cost take 196 (see that section): read as
  "nothing of ours runs in this pass", it revived a correctly-dead hypothesis.
  `tests/run-multiview-render.sh`'s case *"unsheared twin keeps the frag
  patch"* asserts the truth on a combined vertex+fragment module.

*Do:* look for this line, which names the exclusion when it appears:

    unsheared pipeline: binding the no-shear twin (shadow + UI exclusion
    active) — vertex edit dropped, fragment edits KEPT

(Before take 197 the same line read "using unpatched modules". Logs from takes
1–196 carry the old wording and mean the same thing.)

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

### X4's VR API is a stubbed surface — names only, no implementation

X4 exports a full VR-shaped API: `IsVRMode`, `IsVRPointerActive`,
`IsVROculusTouchActive`, `IsVRViveControllerActive`, `SetVRWindowMode`,
`SetMouseVRSensitivityYaw`/`Pitch`, `SetVRVivePointerHand`,
`GetVRViveTouchpadLockTime` and more. They are all stubs, which `nm -S` shows
before anything is disassembled — the setters are **1 byte** and the getters
**3**:

    IsVRPointerActive     xor %eax,%eax ; ret      always false
    IsVRMode              xor %eax,%eax ; ret      always false
    SetVRWindowMode       ret                      does nothing
    GetVRVivePointerHand  mov $0x1,%eax ; ret      a constant

None of them appears in the FFI declarations carried in X4's UI Lua bytecode
either, so nothing in the game calls them.

**Do not try to switch VR mode on.** Interposing `IsVRMode` to return true wakes
nothing up: the implementations are stripped, so there is no VR path behind the
name, and the only achievable effect is to confuse code that asks. Patola
recalled this from an earlier attempt at this mod and the disassembly confirms
it — recorded here so the thread is not reopened a third time.

Worth knowing *why* the names exist: Egosoft shipped VR for **X Rebirth**, not
for X4 — X4: Foundations never had a VR edition — so this is residue carried
forward from the previous game, not a feature that was removed from this one.
The camera API that IS live and useful — `GetCameraRotation`,
`IsExternalViewActive`, `IsFloatingViewActive`, `IsGamePaused`, `IsHUDActive`,
`IsInPanelMode` — has nothing to do with it.

**The residue is much wider than `IsVRMode`, and that is how this thread keeps
getting reopened.** It was reopened a *fourth* time from a direction the
paragraph above did not name, so the whole surface is listed here. A symbol
sweep of the binary turns up all of this, and none of it is a way in:

* `INPUT_SOURCE_OCULUS_REMOTE_*`, `INPUT_SOURCE_OCULUS_TOUCH_*`,
  `INPUT_SOURCE_VIVE_LEFT_*`, `INPUT_SOURCE_VIVE_RIGHT_*`, `INPUT_SOURCE_VRBUTTONS`
  in the input-source vocabulary. **No `INPUT_STATE_*` or `INPUT_RANGE_*` id
  uses any of them** — grep returns nothing — so there is no binding to make.
* The twenty-two `*ActiveHeadTracker*` accessors, plus `IsHeadTrackingActive`,
  `GetOpenTrackConnectionStatus`, `GetTobii*`, `GetMouseVRSensitivity*`.
* The `enableopentrack`, `opentrackfilterstrength`, `opentrackanglefactor` and
  `opentrackpositionfactor` config tags, which are present in the player's own
  `config.xml` with `enableopentrack` set to `false`.

That last one is the most seductive, because we already intercept X4's config
load non-intrusively, so flipping it looks free. **OpenTrack is a forbidden path
for this mod and has been since the start** — it is the `v0.1` proof-of-concept
approach this project exists to replace (see `DESIGN.md`, and the comfort
failures listed at `DESIGN.md:210`: wrong pivot, gimbal lock, roll). An Euler
angle channel with X4's own filtering and deadzone in front of it cannot carry
the pose we need, whatever the tag does.

**The general rule this keeps teaching:** an exported symbol, a vocabulary
string and a config tag are each evidence that a feature *was named*, never that
it is *implemented here*. `nm -D` was already established as necessary and not
sufficient; so is a string, so is a settings tag, and so is a tag sitting in the
player's live config file. Before spending anything on a VR-shaped name in this
binary, re-read this section.

## The map does not pause the game, and only the HUD sees it

Four view queries look interchangeable by name. They are not, and picking the
wrong ones cost takes 145 and 146.

X4's **map is a live view**: it is not an external view, it is not floating, and
it does **not** pause the game. So `IsExternalViewActive`, `IsFloatingViewActive`
and `IsGamePaused` all report "cockpit" for the entire time the map is up —
measured in take 146, where the map was open 9.2 s and not one of the three
changed. The ESC menu *does* pause, which makes this easy to misread: a run that
counts view transitions sees two of them and concludes the map is covered, when
both belong to the ESC menu.

`bool IsHUDActive();` is the query that separates them, and it is declared in
X4's own FFI cdefs. `bool IsInPanelMode(void);` is also declared and is the next
candidate if the HUD ever proves insufficient.

**`IsHUDActive` does not null-check its global.** This matters more than the
semantics:

```
a431d0:  mov    0x3373c71(%rip),%rdx
a431d7:  movzbl 0x418(%rdx),%eax        <- dereferenced with no test/je
```

`IsExternalViewActive` and `IsInPanelMode` both test their pointer before using
it; `IsHUDActive` does not, so calling it before X4 has built that object
segfaults the game. Being declared in the cdefs is *necessary and not
sufficient* — the declaration gives the prototype, the disassembly gives the
preconditions, and `IsFullscreenMenuDisplayed` failed the first test while this
one fails the second. Read both before calling anything.

(`IsFullscreenMenuDisplayed` is exported but has **no FFI declaration** anywhere
in the Lua bytecode. It was called on the strength of its name with an invented
`bool()` signature and crashed X4 twice. Do not reach for it again.)

## The numpad free-look is a spring, and the clamp is not in the input path

Head-look holds `INPUT_STATE_CAMERA_MOUSELOOK`, and holding that is what X4 uses
to suspend ship control — so it costs the player mouse steering and the aiming
cursor. The eight `INPUT_STATE_CAMERA_*` directional states never touch
MOUSELOOK, which made them the obvious candidate for a rotation channel that
leaves the mouse alone. **They cannot carry it.** Measured in take 154, driven
by real keypresses on X4's factory numpad bindings so that no synthesis question
could confound the answer:

| hold | limit | rise | limit reached | recentre 50% / 90% |
|---|---|---|---|---|
| LEFT 4.02 s  | 65.00° | 92.0 °/s | 1.86 s | 0.38 s / 0.62 s |
| UP 5.03 s    | 35.00° | 62.6 °/s | 1.76 s | 0.29 s / 0.61 s |
| LEFT 13.21 s | 65.00° | 89.3 °/s | 1.85 s | 0.39 s / 0.63 s |

**It springs home on release** — every hold, back to 90% of centre in 0.62 s,
the three agreeing to within 0.02 s. A spring cannot hold an absolute head pose
however it is driven: the moment the servo stops commanding, the view returns.
Pulsing does not rescue it either, because the pull starts immediately on
release. This retires the directional states as a channel, and it retires them
*before* any work went into whether we can synthesise the keys — which was the
point of measuring with real fingers first.

**The far more useful finding is the limit.** Yaw stops at 65.00° and pitch at
35.00°, which are *the same numbers* already measured on the MOUSELOOK path
(±65.00° yaw confirmed live in take 150, ±35.00° pitch). Two unrelated input
paths, one pair of constants: **the clamp lives in X4's free-look camera, not in
the input channel.** So no change of input can widen it, and the standing hope
that some other entry point might reach past ±65° is closed. Widening it, if it
is ever wanted, is a different kind of problem than picking a better channel.

The recentre figure is worth keeping for its own sake: it is the transient the
map gate produces every time it hands the mouse back and takes it again. X4
pulls the view home in about six tenths of a second, and the servo then drives
it back on re-acquire — which is what take 148 looked like when it worked.

**What survives.** One option is left for decoupling the mouse, and it does not
touch the head's channel at all: give *ship steering* a different physical
source. X4's inputmap already binds 22 things to `INPUT_SOURCE_JOYAXES`, we
already fork and serve that file, and we already intercept the player's mouse
deltas — and currently throw them away to stop them fighting the head. Feeding
them instead to a virtual joystick bound to the steering ranges would put the
head and the hand on different sources, so they stop competing. Unverified, and
the open questions are real: whether `/dev/uinput` is writable here, whether X4
hot-detects a device that appears after launch, whether the steering ranges are
relative or absolute, and whether the aiming cursor is a separate problem from
steering. Those are worth answering before any of it is built.

## The Lua route is closed for the camera, and it was never needed

X4's UI Lua was the last unexplored channel, so it was checked before any of the
virtual-device work above. It cannot rotate the camera, and the reason is worth
stating precisely because "Lua modding" sounds like a wider surface than it is.

X4's UI Lua does not talk to the engine through some private scripting bridge.
It declares a LuaJIT FFI block and calls **exported C functions of the `X4`
binary** — the same symbols the injector already reaches with `dlsym`. The cdef
block is therefore a *prototype table for functions we can already call*, not a
capability we lack. That is the whole reason for this project's rule that a cdef
must be read before a call: the cdef supplies the signature, the disassembly
supplies the preconditions, and the call itself has never needed Lua.

The declarations are recoverable without running the game — they sit as plain
text in `08.dat`:

```
strings -n 4 08.dat |
  grep -E "^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]*]+[A-Za-z_][A-Za-z0-9_]*\(.*\);[[:space:]]*$"
```

That yields **1931 declarations**. Exactly one of them names camera rotation:

```
Rotation GetCameraRotation();
```

A getter. There is no setter — not in the cdefs, and not in the export table
either, where a sweep for `Set*Rotation` / `Rotate*` returns only `SetAutoRoll`
and the `SetMouseVRSensitivity{Pitch,Yaw}` residue. The camera setters that do
exist change the view *mode* or a base position, never an angle:
`SetPlayerCameraCockpitView(bool)`, `SetPlayerCameraTargetView`,
`SetPlayerCameraCinematicView`, `SetFollowCameraBasePos(Coord3D)`,
`SetCockpitCameraScaleOption(float)`, `SetSceneCameraActive(bool)`.

**Conclusion: X4 exposes its camera orientation read-only, at every level.**
Exports and FFI agree. No script, in any language, can set the free-look angle;
the input system is the only way to move that camera, which is why every route so
far has run through it and hit `MOUSELOOK`.

Two corollaries worth keeping:

* The savegame question is **moot, not answered**. Whether Lua can be injected
  without setting `modified="1"` never has to be resolved, because there is
  nothing in Lua worth injecting — anything Lua could call, the injector already
  calls directly, touching no game file and loading no script. Do not spend a
  take establishing the flag's behaviour for a route with no payload.
* `SetMouseSteeringPersistent`, `SetMouseSteeringAdapative`,
  `SetMouseSteeringLine` and `SetForceShootingAtCursorOption` look like runtime
  overrides that might dissolve the mouselook/steering exclusivity. They are
  not: X4's own Lua calls each of them from a `callbackInputMouseSteering*`
  handler in the Controls menu, and the state surfaces to MD as
  `player.input.mousesteering.permanent`. They are the settings toggles the
  player already has, reachable from code. They do not change what holding
  `MOUSELOOK` does.

(Wrong turn, recorded: the first extraction pass filtered declarations by a fixed
list of primitive return types and reported "no camera rotation entry at all" —
it had dropped `Rotation GetCameraRotation();` because `Rotation` was not in the
list. The filter's own output contained the tell, since a function known to be
callable was missing from it. Same class as the off-axis test card: the
instrument assumed the shape of what it was measuring.)

## The guard band prices head rotation, and #35 is what makes it affordable

Rotating the head in the layer is a homography, `x' = K·R·K⁻¹·x`, which is
*exact* for a pure rotation about the projection centre at every depth — no
parallax term, no depth buffer, no disocclusion. Head **translation** has none of
those properties, which is why the rotation half of #33 is the tractable half.

What limits rotation is coverage, not correctness: a ray the runtime's eye
frustum wants must have been rendered, and X4 renders its frustum with no idea
the head moved. So range is bought with render area. `tools/guard_band.py`
prices it from measured inputs only — the take-112 per-eye frusta
(`-54,40,44,-55` / `-40,54,44,-55`, Quest 3 over WiVRn), X4's
`horizontal FOV = X4VR_FOV × 73.7399`, and the shipped `X4VR_FOV=1.4917` at
1408×1408 per eye. Its self-check is that a zero-degree budget must reproduce
1.4917, and it does, so the model agrees with the state takes 114d–154 ran.

Pixel counts hold tangent-space sample density constant: on-axis angular
resolution is preserved and only the covered angle grows.

**Yaw and pitch budgeted together** (`--max-deg 20`):

    yaw/pitch   X4VR_FOV   sym px/eye   sym area   asym px/eye   asym area
        0.0      1.4917     1408 sq       1.00x     1092x1180      0.65x
        5.0      1.7214     1975 sq       1.97x     1533x1636      1.27x
       10.0      1.9644     3113 sq       4.89x     2402x2496      3.02x
       15.0      2.2278     7141 sq      25.72x     5061x5042     12.87x
       20.0      -- a ray reaches 90 deg: no planar render covers this --

**Yaw only** (`--pitch-deg 0`):

    yaw        X4VR_FOV   sym px/eye   sym area   asym px/eye   asym area
        5.0      1.6002     1641 sq       1.36x     1313x1347      0.89x
        7.5      1.6680     1816 sq       1.66x     1446x1454      1.06x
       10.0      1.7358     2021 sq       2.06x     1598x1582      1.28x
       15.0      1.8714     2568 sq       3.33x     1988x1936      1.94x
       20.0      2.0071     3438 sq       5.96x     2573x2516      3.27x

Three things fall out of this.

**There is a hard wall just under 20° when both axes are budgeted.** It is not a
performance cliff, it is geometry: the corner ray already sits at (54°, 55°), and
stacking yaw and pitch onto it drives it to 90°, where a planar render has no
finite extent. No pixel budget buys past it. Anything wanting a real
over-the-shoulder look needs a different rendering strategy, not a bigger number.

**#35 is on #33's critical path.** The per-eye off-axis projection is filed as a
stereo-correctness fix, but the `asym` column says it is worth **0.65×** the
pixels *at zero head budget* — a 35% saving standing still, because X4's
symmetric frustum currently renders a large wedge that neither canted eye can
see. With #35 done, ±7.5° of yaw costs 1.06× and ±10° costs 1.28×. Without it the
same budgets cost 1.66× and 2.06×. On a project where performance is king that is
the difference between shippable and not, so #35 should land before any head
rotation is wired.

**The affordable envelope is a glance, not a look** — roughly ±10° of yaw with
#35, less with pitch spent too. That is enough for the small continuous head
motion that makes VR read as VR, and nowhere near enough to look around the
cockpit. The reading is therefore not "layer rotation instead of `MOUSELOOK`" but
a split: the layer covers the small, always-on, latency-critical motion where its
homography is exact and cheap, and any deliberate large look stays a
player-triggered `MOUSELOOK` action. That leaves the mouse free during normal
flight, which is the thing the input-channel work could never achieve.

Not yet measured: how much of the budget the head-look servo's own residual would
consume if X4's camera were coarse-tracking underneath. That number decides
whether the split above is one mechanism or two.

## The head-look servo lags by ~50 ms, and that is what the guard band must cover

The guard-band budget above (±10° of yaw with #35) raised the question of whether
X4's camera coarse-tracking underneath would eat it. Answered from takes 148 and
150, which already logged `camread:` — X4's **actual** angle from
`GetCameraRotation` beside the head pose, so the residual is measured, not
inferred from the servo's own belief.

Two contaminants had to come out first, and both matter:

* **Camera not engaged.** Most samples have X4's camera at 0 while the head is
  well away from centre — the servo is not steering. That is a disengaged servo,
  not a tracking error, and it dominates the raw distribution (take 148 p95 rises
  from 4.7° to 33.1° if these are left in).
* **Clamp saturation.** In take 150 the head reached **93.32°** of yaw while X4
  capped at exactly **65.00°**; 61 samples sit on a 65/35 limit. That residual is
  the clamp, not lag. Take 148 stayed inside the limits in yaw and is the usable
  one there.

With both removed, the residual is linear in head angular speed:

    take 148, engaged and off-clamp, 116 points
      head yaw speed      median  p90    max
        0.0 -   0.3 deg/s   0.05   0.09   0.12
        1.2 -   5.7         0.24   1.35  14.97   <- see note
        5.8 -  17.3         0.25   1.59   2.73
       18.0 -  41.6         1.63   3.01   7.67
       43.6 - 103.2         1.96   5.21   5.92

    implied lag = residual / speed, points above 20 deg/s
      take 148:  median  50 ms,  p90 101 ms   (37 points)
      take 150:  median  52 ms,  p90  57 ms   ( 3 points)

**Residual ≈ head angular speed × 50 ms**, from two takes that agree and match
the servo's own constants independently: `Kp=0.25` at the ~86 Hz tick rate these
logs imply gives 1/(86 × 0.25) = 47 ms. Measurement and mechanism agree, which is
the only reason to trust a number this convenient.

**State the instrument's range.** `camread:` samples 1 tick in 30, which in these
logs is **2–3 Hz**. The observed speeds top out near 103 deg/s and are averages
over ~0.4 s windows, so they understate the peak of a fast head snap. The 50 ms
figure should hold — it is a property of the loop, not of the sample rate — but
the *peak residual* during a 300–500 deg/s snap has not been observed and must
not be quoted from this data. Measuring it needs the camread cadence raised
during motion, the way the pan probe does it; that is a code change, not a take.
(The 14.97° outlier at 1.2–5.7 deg/s is a transient at engage, where the camera
had not yet caught up. Left in the table rather than filtered out.)

**So: one mechanism, not two.** The layer applies *true head pose minus X4's
camera pose*, always, and whether X4's camera is moving underneath is only a
question of whether `MOUSELOOK` is engaged:

* **disengaged** (normal flight, mouse free) — X4's camera is still, so the
  residual *is* the whole head rotation, bounded by the ±10° guard band.
* **engaged** (deliberate look) — X4 coarse-tracks to its ±65° clamp and the
  residual is the 50 ms lag, ~5° at 100 deg/s and ~10° at 200 deg/s: inside the
  same budget, at the same cost, through the same code path.

The second reading is the one worth keeping: layer correction is not only a way
to extend range, it **removes the 50 ms servo lag outright** during an engaged
look. 50 ms is far above the motion-to-photon budget VR comfort needs, so this is
likely the largest comfort win available and it does not depend on the range
argument at all.

## Decision: head rotation is a vertex-stage clip-space rotation, and free-look is out

**Patola's ruling, 2026-08-16:** mouse free-look is excluded from the design
entirely. As long as holding `MOUSELOOK` suspends mouse aiming and pointer
steering, it cannot be used in practice, so it is not available as a complement
to head tracking and must not appear in any cost assessment. Everything the
previous sections priced against a coarse-tracking servo underneath is therefore
withdrawn as a *plan* — the measurements stand, the architecture they served does
not.

What survives is the vertex-stage rotation. The layer already applies per-eye
clip-space transforms; the rotation goes in at the same site:

    M = P_narrow · R · P_wide⁻¹     applied to c = P_wide · V · x
    M·c = P_narrow · R · V · x

which is exactly a render with view `R·V` and projection `P_narrow` — exact,
per-vertex, perspective-correct, with clipping happening afterwards against
`P_narrow`. `X4VR_FOV` widens what X4 **culls**, and the narrow output projection
keeps the pixel count at the display's. So the guard band is paid in draw-call and
vertex work, not in fragment shading, and the reprojection cost table two sections
up prices only the fallback.

Consequences of dropping free-look, stated so they are not rediscovered:

* The ±65.00°/±35.00° camera clamp is **irrelevant** — we never drive X4's camera.
* The 50 ms servo residual is **irrelevant** — there is no servo. The measurement
  keeps its value only as the characterisation of a mechanism now abandoned.
* Range is whatever the culling frustum buys, roughly ±27° of yaw at
  `X4VR_FOV=2.21` and ~±34° near the 180° asymptote. There is no ±65° to compose
  with, so **that is the whole range.** Over-the-shoulder is a ship turn.
* The mouse is free at all times, which was the point.

**Predictions, before the checks that test them:**

1. **X4's CPU culling follows the intercepted `<fov>`.** Takes 114d–154 ran at
   `X4VR_FOV=1.4917`, widened from 1.437, and no take reported an empty annulus or
   edge popping. An engine culling against a frustum narrower than it projects
   would have shown one. Expected to hold; the falsifier is a black or
   geometry-free ring at the edge of a widened render.
2. **There is no single clip-space site.** The general MVP path plus the 12
   baked-sx modules will both need the rotation term, exactly as the shear did.
   This is the prediction I expect to cost work, and it is a solved-once problem
   rather than a new one.
3. **The rotation WILL move the HUD, unlike the shear.** `K_eye = P·T(−d)·P⁻¹`
   displaces in proportion to inverse depth, so a screen-space draw at `w = 1` was
   immune by construction. A rotation is depth-*independent* — that is the same
   property that makes the homography exact — so the identical transform that left
   the HUD alone will drag it. The HUD therefore cannot be left to fall out for
   free; it needs #30's canvas explicitly, and a screen-space element carried
   through `R` would behave as if infinitely distant rather than sitting at the
   cockpit.

### Checking the three predictions — and finding the gate

**Prediction 1 (culling follows `<fov>`): confirmed, and already written down.**
`injector/x4vr_config.hpp:95` gives that as the *reason* for the config route:
"patching the projection behind X4's back would instead desynchronise its
culling, its HUD placement and its depth range from what is drawn." The question
was answered before it was asked.

**Prediction 2 (no single clip-space site): confirmed, and better than feared.**
There are three, and the third is universal — `layer/x4vr_layer.cpp:3966`:

    patch_vertex_eye_offset      world modules with a live camera block  (n_live)
    patch_vertex_eye_offset_mvp  the 12 that only have M_worldviewprojection
    patch_vertex_clip(code,K,KR) everything else -- an arbitrary clip-space 4x4

`patch_vertex_clip` takes exactly the kind of matrix the rotation is, and the
call site already carries a `world` flag that separates world geometry from UI.
So prediction 3's consequence is handled by machinery that exists: apply the
rotation where `world` is true and the HUD stays screen-locked for #30 to place.

**The gate, which neither prediction anticipated.** X4's own FOV slider is
`min = 60, max = 120, exceedMaxValue = false`, over `GetFOVOption() * 90` — so
the UI tops out near a tag value of 1.333. We already run at **1.4917**, above
it, so the engine is not hard-clamped there, and **1.5 is the largest value ever
run** across all 125 stored logs. The plan wants ≈**2.21**, which is 47% beyond
anything tested.

This is load-bearing in a way worth stating plainly: `X4VR_FOV=1.4917` was chosen
to cover the eye frusta with *zero* headroom, so if X4 refuses much past 1.5 the
head-rotation range is about **±1°** — and that kills the image-reprojection
fallback too, because it needs X4 to render wider by the same amount. **One
number gates both architectures.** Find it before building either.

**Trap, recorded so the sign is not "corrected" later.** A doc string in `08.dat`
reads: values above 1.0 "zoom in", below 1.0 "zoom out" — the opposite of what
this project measured. That text describes a *different* `fov`: the cutscene
`<camera fov="20">` zoom factor. The config tag goes the other way, and two
independent sources agree — the slider (higher units = more degrees) and four
measured projection matrices (`sx = cot(fov × 73.7399 / 2)`, so higher fov =
smaller sx = wider). Same-name-different-thing is the aliasing mistake this
project keeps making; it was caught only because the measured direction
contradicted the prose.

**Correction to the Lua section above.** `GetFOVOption`/`SetFOVOption` are called
from X4's Lua *without* the `C.` prefix, and appear in neither the FFI cdefs nor
`nm -D`. X4 has a **second C→Lua bridge** — classic registered globals — so
"anything Lua could call, the injector already calls directly" was wrong as
stated. The registered names do live in the binary's string table, and sweeping
that surface for camera setters returns the same family as before plus
`Set/GetMouseLookToggleOption` and `SetRequireFinalRotationOrder`. **No
`SetCameraRotation`.** The conclusion survives; the reasoning that reached it did
not, and a symbol sweep from now on must cover both bridges.

### The scorer now fails a run whose `<fov>` X4 refused

`score_run.py` already computed the field each camera implies and marked the one
that honours `X4VR_FOV` — the "compare the SET, not the range" machinery from
take 104. What it did **not** do was gate on it: the no-camera-honours case was a
`print("warn ...")`, so a run whose tag X4 silently rejected still scored **PASS
and was recommended for `known-good-runs.md`**.

That is fine while the field is incidental and fatal for #33's ceiling
experiment, whose entire question is whether X4 accepts a wider tag. Promoted to
a `fails.append`, with the nearest observed tag in the message — because a bare
"not honoured" cannot separate the two causes the old warning named, and they
need different responses:

* **far from the ask** → X4 fell back to its own default or the player's profile
  value: it rejected the tag, and the ceiling is below what was asked.
* **near the ask** → the linear `sx → degree` law bending outside the
  1.111..1.500 range it was calibrated over. X4 may well have accepted the field;
  what failed is our arithmetic for naming it.

Validated on four controls, four distinct outcomes, per the rule that a changed
check must be run against a log that flips:

    take 105, tag rewritten to 2.2100   FAIL "X4 fell back ... rejected the tag"
    take 105, tag rewritten to 1.5000   FAIL "0.063 off ... law bending"
    take 105, untouched                 PASS, sx=0.75405 <- honours X4VR_FOV
    all 53 stored logs setting X4VR_FOV verdicts unchanged (24 FAIL / 27 PASS / 2 UNSCORABLE)

The 2.2100 control is the important one: **before** this change it scored PASS
with a warning, which is the exact way the ceiling run could have been thrown
away — answering "no" in the voice of "yes".

### Take 155 — the `<fov>` ceiling, predicted before the run

Aimed straight at **`X4VR_FOV=2.21`** rather than climbing a ladder: it is the
value the plan needs, so a pass ends the question in one run and a refusal tells
us to bisect. Testing the falsifier first is what made take 154 cost one run.

Derived from take 154's own `env: run =` line. Four differences, each with a
reason:

* `X4VR_FOV` 1.4917 → **2.21**, the subject.
* **`X4VR_DUMP_MATRICES=1` added.** Take 154 produced *zero* `proj` lines; the
  dump is a plain flag (`e && *e && *e != '0'`) and without it there is nothing
  to score. Not a knob added to fix a failed run — the instrument the
  measurement is made of.
* **`X4VR_PANPROBE=1` removed.** This is a measurement take, not an interaction
  take, and the pan probe drains the event queue on a cadence.
* Fresh `X4VR_LOG`, fresh `X4VR_TAKE`.

The dump requires ≥50 draws credited to one camera block, so the run must reach
an actual scene. A menu-only run scores "no `proj MEASURED`" and measures nothing.

**Predictions:**

1. **X4 accepts it** — maybe 60/40. The slider's `max = 120` /
   `exceedMaxValue = false` is a *UI* construct, we already run 12% past it at
   1.4917, and the injector validates only the shape of the string. The falsifier
   is an engine-side clamp we have never reached because we never went far enough.
2. **If accepted, expect visible degradation that is not a refusal.** At 163°
   horizontal, objects subtend far fewer pixels, and X4's LOD selection is
   normally screen-coverage driven — so a blurrier, simpler-looking scene is the
   predicted *success* case, not evidence the tag was ignored. Recorded now so it
   is not misread on the day.
3. **A near-miss is our arithmetic, not X4's answer.** The `sx → degree` law was
   fitted over 1.111..1.500 and 2.21 is 47% beyond it. If the scorer reports "law
   bending", recalibrate the law; do not conclude a refusal.

### Take 155/156 — X4 honoured 2.21. The scorer said otherwise and was wrong.

**Result: X4 accepts `X4VR_FOV=2.21` and renders the full 163°.** Prediction 1
holds. The ceiling is at or above 2.21, so **±27.5° of yaw** is available to the
vertex-stage rotation and the plan in the decision section is viable.

Measured without the layer's instrument at all. A `<fov>` change at a fixed pose
is an exact uniform scale about the principal point (`x_ndc = sx·tan θ`), so two
cockpit screenshots register by a single factor `k = sx_a/sx_b`. Registration of
Patola's pair put the peak at **k = 0.215** — expected 0.214 for fov 2.21 —
giving a rendered **fov 2.209 against 2.21 asked, 0.05% off.** A genuine peak
(ncc 0.632, falling away on both sides), and the method recovered three
synthetic scales (0.30/0.50/0.75) exactly before being believed.

Two side results worth keeping:

* **The `sx → degree` law does not bend at 2.21.** Prediction 3 feared it would,
  47% outside its 1.111..1.500 calibration. It is linear to 0.05% there.
* **The HUD did not scale between the two shots** — same screen position, same
  size, at both fields. Prediction 3's other half confirmed: it is drawn at
  `w = 1`, it will not follow a clip-space rotation for free, and #30's canvas
  is required.

**Why the scorer got it wrong, exactly.** The layer credits only the
most-drawn block per frame, and take 156 measures the consequence directly:

    156b, fov 1.4917:  sx 1.33333 x35   sx 0.70024 x5   sx 3.78085 x2
    156a, fov 2.21  :  sx 1.33333 x36                   sx 3.78085 x2

`0.70024` is the expected `sx` for 1.4917 to five digits — the scene camera, and
it won only **5 of 42** samples even there. At 2.21 it won **none**, because a
wider field spreads draws across more blocks. The camera rendering the picture
was never sampled, and everything the log did contain was the two cameras that
ignore `<fov>` — the same pair that appears in takes 104 and 105 *alongside* the
honouring one. "One number, several cameras", again.

**The scorer's message asserted a cause it could not observe.** It read "X4 fell
back to a different field, so it rejected the tag" from an absence in a
credit list. Reworded to state what was credited and to say explicitly that this
is not evidence about X4's config parsing. The run still FAILs — it genuinely
failed to *measure* the field it asked for — but a FAIL now means "this run did
not measure what it set out to", which is true, instead of a verdict on X4.

**And the process failure, which is the one worth remembering.** Take 155's
prediction block ended: "the picture should look dramatically wider; if it looks
identical to 1.4917, X4 refused, and your eye will tell us faster than the log
will." That was written, committed, and then not acted on: the log was read, the
FAIL was believed, and the person watching the screen was never asked. Patola
stopped it. **An independent channel named in advance as decisive has to be
consulted before the instrument's verdict is accepted, not after it is
challenged.**

Still owed: the layer must log every camera block it sees rather than the
most-drawn winner. Until then no run can be trusted to sample the scene camera,
and this one was marginal (5 of 42) even at the field we ship.

### The camera dump now reads every credited block

`layer/x4vr_layer.cpp` sampled the projection of the **most-drawn** descriptor
block per frame. That is the defect behind take 155, and take 156 measured it:

    fov 1.4917 -> the scene camera won  5 of 42 credited samples
    fov 2.21   -> the scene camera won  0 of 38

A wide field spreads draws across more blocks, so the block actually rendering
the picture loses the per-frame vote *precisely when the field is unusual* —
which is exactly when a run is asking about it. A camera never sampled cannot be
told apart from a camera that does not exist, so an instrument gap read as a
fact about X4.

The `X4VR_DUMP_MATRICES` block now loops over every block credited that frame
that clears the same ≥50-draw bar the winner had to clear, with the same affine
sanity test (an unpopulated block reads back zeros and would otherwise enter the
tally as a camera at an absurd field). The per-camera and global change budgets
are unchanged and still announce themselves.

Two things deliberately left alone:

* **The winner is registered before the loop**, so `cam#0` still means "the
  camera that drew most". Every stored log and the scorer's `proj MEASURED`
  line assume that; renumbering would silently invalidate 71 logs of regression
  material.
* **The eye-offset path still acts on the winner only.** Widening the
  *observation* is the fix; widening what the layer *mutates* is a different
  change with different risk, and this one is meant to make measurement
  trustworthy, not to alter behaviour.

Cost is confined to `X4VR_DUMP_MATRICES` runs — the loop is inside the `dump`
guard, so a normal run does the same single-block work it always did.

Verified: builds clean, and 11 of 13 test binaries pass. The two that do not —
`x4vr_test_multiview_render` (`shaders_missing`) and `x4vr_test_spirv_patch`
(a CLI that printed its usage) — fail *identically on the unmodified layer*,
checked by stashing the change and rebuilding rather than by assuming.

### Take 157 — the loop was right, the threshold was still wrong

The all-blocks loop widened coverage exactly as intended: 85 changes against 38,
57 slots against 28, and a third projection appeared. **And the 2.21 camera was
still missing.** Reported as a FAIL, correctly, by the reworded message that no
longer claims X4 rejected anything.

The remaining filter was the draw-count bar the loop had inherited from the
winner. Take 156b names the number:

    fov 1.4917, the honouring camera:  draws=51,  five samples, every one at 51

**One draw above a threshold of 50.** At the wider field it fell below and
vanished entirely. A bar the subject of the measurement clears by one is not a
filter, it is a coin toss — and the same class of defect as the winner-take-all
it was meant to fix: a rule adopted for one purpose quietly deciding which
cameras exist.

`50` was only ever a proxy for "has X4 populated this block". The loop already
runs the direct test — an unpopulated block reads back zeros, the affine check
`|m[15] − 1| > 1e-3` rejects it, and `read_proj_terms()` refuses whatever it
cannot decode — so the proxy is removed and every credited block is examined on
its own merits.

**Two fixes, one bug, and both had to land before anything could be measured.**
Worth recording as a pattern rather than two incidents: the first fix widened
*which* blocks were considered, the second removed the bar that then excluded
the one that mattered. Neither alone would have shown the 2.21 camera, and the
first one passing its build and tests proved nothing about whether it worked.

### The frame cap is ruled out, and the perf A/B was never testing the guard band

**No cap.** Take 158's per-batch medians run from **0.87 ms** (1148 fps) to
19.69 ms with no floor at 16.67 or 8.33 ms; take 156b spans 1.05..19.54. A vsync
or frame-rate limiter would show as a floor and there is none.

So the alarm fired correctly — identical timings across a 2.4× change in
rendered solid angle *is* suspicious — but the diagnosis was wrong, and the real
explanation is one I should have reached before proposing a cap:

**`X4VR_RES` was `1408x1408` in every run.** Widening `<fov>` at a fixed render
size adds no pixels. It spreads the same pixels over more solid angle, trading
angular resolution for coverage. Fragment cost is unchanged *by construction*,
so 6.92 ms at fov 1.4917 and 6.80 ms at 2.21 are not a surprising result, they
are the only possible one. All 136 stored logs are `1408x1408`; two are
`1408x792`. **Resolution has never been varied, so the guard band's cost has
never been measured.**

That splits the plan into two products that had been conflated:

* **fov 2.21 at 1408²** — genuinely free in frame time, and 2.4× blurrier. This
  is what take 158 rendered and what the screenshot shows; it is also what
  take 155's prediction 2 anticipated as the success case.
* **fov 2.21 at the resolution that preserves sharpness** — ~4489×4693 per eye
  with #35, which the guard-band table prices at ~10.6× the baseline pixels.
  *That* is the cost the head-rotation plan actually incurs, and it is unmeasured.

A second reading worth keeping: widening the field put far more geometry in the
frustum for no measurable time, so the renderer is **not** draw-call or vertex
bound at this setting. If it is not fragment bound either, the guard band may be
far cheaper than the pixel ratio implies — which is exactly what a resolution
A/B decides and reasoning cannot.

**How to vary it.** `X4VR_RES` cannot be passed on the command line in SBS mode:
`launch/x4vr-launch.sh` *exports* `X4VR_RES="$((W/2))x$H"` and overwrites
whatever was set. Resolution is changed through `X4VR_W`/`X4VR_H`, which the
per-eye size is derived from. (Take 101 lost a run to the older version of this,
which re-read the header instead of the override and left the window, the render
and the composite at three different sizes.)

### Take 159 — the renderer is not fragment bound, and the guard band is affordable

Single variable, resolution, with `<fov>` held at 2.21 and confirmed by the
scorer (`extents X4 renders 2816x2816`, `sx=0.14976 = fov 2.210`):

    take 158   1408x1408/eye   last phase  6.80 ms
    take 159   2816x2816/eye   last phase  9.81 ms

**4× the pixels for 1.44× the time.** A fragment-bound renderer would have given
4.00×. Corroboration that the scenes were comparable: the 14–94 s phase reads
17.19 ms against 158's 17.21 ms — that phase does not move with resolution at
all, which is what a CPU/streaming-bound stretch looks like, and its agreement
across two sessions is what makes the last-phase comparison worth reading.

A two-point fit gives **5.80 ms fixed + 1.00 ms per baseline-pixel-unit**, where
one unit is 1408² per eye. Extrapolated against `tools/guard_band.py`'s
per-eye-asymmetric column, which is what #35 delivers:

    +- 7.5 deg    1.06x px     6.9 ms   146 fps
    +-15.0 deg    1.94x px     7.7 ms   129 fps
    +-20.0 deg    3.27x px     9.1 ms   110 fps
    +-22.5 deg    4.50x px    10.3 ms    97 fps
    +-25.0 deg    6.59x px    12.4 ms    81 fps
    +-27.5 deg   10.63x px    16.5 ms    61 fps

    without #35:  +-20 deg  5.96x  11.8 ms  85 fps ; +-27.5 deg  21.95x  27.8 ms  36 fps

**So roughly ±22° of yaw at 90 fps, with #35 done** — a 90 fps budget of 11.1 ms
buys 5.3× baseline pixels. #35 is worth about 5° of head rotation at constant
frame rate, which is a far better argument for it than the 0.65× pixel saving
alone.

**Treat the extrapolation as weak.** It is two points fitted to a straight line
and then run out to 10×. Nothing here tests whether the per-pixel term stays
linear once bandwidth or memory pressure enters, and the 5.80 ms fixed term was
measured on one scene at one moment. A third point — 3× linear, 9× pixels — would
test the model where it is actually being used rather than where it was fitted,
and should be taken before any resolution is chosen. The headline that does not
depend on the fit is the measured one: **4× pixels, 1.44× time.**

### Take 160 — the third scaling point, predicted before the run

3× linear, **9× the baseline pixels**: `X4VR_W=8448 X4VR_H=4224`, which the
launcher must turn into `X4VR_RES=4224x4224` per eye. `<fov>` stays at 2.21 so
resolution remains the only variable, exactly as in 158 → 159.

**Prediction from the two-point fit: 14.8 ms** (5.80 fixed + 1.00 × 9). Baseline
repeatability across three runs was ±0.1 ms, so anything beyond about ±1 ms is a
real departure and the linear model is wrong where we would actually use it.

**A confound that could make it superlinear, stated now rather than discovered
later.** X4's LOD selection is screen-coverage driven. At 1408² with a 163° field
every object subtends very few pixels and the renderer picks low LODs; at 4224²
each object is 3× larger in pixels and the LODs come back up, adding vertex and
draw work that has nothing to do with fill rate. So this A/B was never a pure
pixel-count experiment — **the 1.44× measured at 159 already contains an LOD
increase**, and 9× may overshoot 14.8 ms for that reason.

That does not spoil the measurement, because a real deployment at that resolution
would pay the same LOD cost. It does mean "5.80 ms fixed + 1.00 ms per unit" is
two mechanisms wearing one coat, and the third point is what says whether the
coat still fits at the size we would order.

**Falsifier:** a result near 14.8 ms confirms the table and ±22° at 90 fps stands.
Materially above it (say >16 ms) means the cost curve steepens with resolution
and every extrapolated row above ±20° is optimistic. Materially below would be
more surprising than welcome and would want explaining before being used.

**Risk noted:** 4224×4224 per eye is ~143 MB per full-rate target before the
frame graph multiplies it. If X4 or the driver refuses the size, the scorer's
`extents` line will say so — and a run whose extents still read 1408x1408
measured nothing, which is the take-101 failure and the first thing to check.

### Take 160 — the falsifier fired, and the 17.2 ms plateau was the loading screen

**Predicted 14.8 ms at 9× pixels. Measured 20.75 ms.** The stated falsifier —
"materially above, say >16 ms, means the cost curve steepens with resolution and
every extrapolated row above ±20° is optimistic" — is what happened. The
resolution confound predicted alongside it (LOD rising with pixel coverage) is
the likeliest cause and was recorded before the run.

Getting there required throwing out how the previous two takes were compared.

**The 17.2 ms phase present in all three runs is X4's loading screen.** X4's own
reported state says so: `draws` sits at **~12 with ~3 slots** from load+10 s to
load+70 s, then jumps to **~97 draws across ~40 slots** as the world appears.
Frame time *drops* from 17–19 ms to 7 ms at that moment — more geometry, faster
frames — which only makes sense if the earlier stretch was not rendering the
world at all. The save takes about 70 s to load.

So the scorer's phase clustering, which groups by frame time and then reports
frame time, merged take 160's loading and gameplay into one 17.74 ms phase and
buried the measurement. Circular by construction. Re-cut on `draws > 50` —
X4's own state, not a frame-time cluster:

    take 158   1x px   n=13   median  6.80 ms   147 fps
    take 159   4x px   n= 6   median  9.82 ms   102 fps
    take 160   9x px   n= 2   median 20.75 ms    48 fps

    marginal 1x->4x : 1.01 ms per pixel-unit
    marginal 4x->9x : 2.19 ms per pixel-unit

**The scaling is superlinear** — marginal cost more than doubles — so last
section's "5.80 ms fixed + 1.00 ms per unit" fit is **withdrawn**, along with
every row it produced above ±20°.

What survives is stronger than what it replaces, because the operating point is
now *bracketed* rather than extrapolated. A 90 fps budget of 11.1 ms lands at
**4.59× baseline pixels**, inside the measured 4×..9× segment:

    +-22.5 deg   4.50x px   10.9 ms   92 fps   (interpolated)
    +-25.0 deg   6.59x px   15.5 ms   65 fps   (interpolated)
    +-27.5 deg  10.63x px   24.3 ms   41 fps   (extrapolated past 9x -- unsafe)

**±22.5° of yaw at ~90 fps, with #35** — the same figure the linear fit gave, but
now by interpolation between two real measurements instead of extrapolation from
two. Everything past ±25° is worse than previously stated and ±27.5° is not
affordable.

**Also settled: X4 is not capping.** `<frameratelimit>false</frameratelimit>` and
`<frameratetarget>197</frameratetarget>` in the live config, and gameplay reaches
147 fps. The plateau was never a cap; it was a screen with nothing on it.

**Weakness to keep in view:** the 9× point rests on **two** samples, because that
run ended shortly after the world appeared. The 90 fps conclusion depends on the
4×→9× slope, so that slope deserves a longer run before a resolution is fixed —
park in the world for 60 s, not 10.

### The scorer segments frame time on draws, not on frame time

The phase display grouped perf samples **by frame time** and then reported frame
time in the winning group. Circular, and it misled twice in one session: it
buried take 160's 20.75 ms inside a 17.74 ms "phase" by merging gameplay with
the loading screen, and the 1×/4× pair before it was compared on a window nobody
had checked was the same state in both runs.

The comparable number is now cut on **X4's own draw count** — `draws > 50`,
measured as ~12 while loading and ~97–117 in flight — taking the *last*
contiguous busy stretch so a flicker of geometry during the splash cannot open
the window early. The `<- the one a parked A/B compares` tag is gone from the
phase list, since the last phase is not reliably the parked one.

Validated against the three runs it was written for, and it reproduces the
by-hand analysis exactly:

    take 158  1x px  GAMEPLAY  6.80 ms (147 fps)  13 windows over 51s  12 -> 117 draws
    take 159  4x px  GAMEPLAY  9.82 ms (102 fps)   6 windows over 32s  21 -> 117 draws
    take 160  9x px  GAMEPLAY 20.75 ms ( 48 fps)   2 windows over 21s  65 -> 117 draws
                                                   ^ warns: too short to compare

Two things it refuses to do quietly. A run whose gameplay stretch is under 30 s
or under 5 windows says so and names the cause — this save needs ~70 s to load,
so a "sit 20 s" protocol lands almost entirely in the loading screen. And the
draw-step line only claims "a clean step" when the after-median is more than
twice the before-median; take 160 reports **NOT a clean step** because its splash
drew enough geometry to blur the comparison. Asserting a split the numbers do not
show is the same over-claim this file already had removed once today.

All 53 stored `X4VR_FOV` logs keep their verdicts.

### Take 161 — the 9× point confirmed, and ±22.5° at 90 fps is the operating point

Parked 60 s in the world by stopwatch, and the draws-segmented number now has the
samples to stand on: **20.77 ms over 5 windows spanning 56 s**, range 19.77–20.82,
`29 before vs 117 after — a clean step`, no short-stretch warning. Take 160's
two-sample 20.75 ms was right.

    1x px (1408²)   6.80 ms   147 fps
    4x px (2816²)   9.82 ms   102 fps
    9x px (4224²)  20.77 ms    48 fps

    marginal 1x->4x : 1.01 ms per pixel-unit
    marginal 4x->9x : 2.19 ms per pixel-unit

**Superlinear, confirmed.** A 90 fps budget of 11.1 ms interpolates to 4.6×
baseline pixels, so with #35's per-eye off-axis projection:

    +-22.5 deg   4.50x px   10.9 ms   ~92 fps   <- the operating point
    +-25.0 deg   6.59x px   15.5 ms   ~65 fps
    +-27.5 deg  10.63x px      --     not affordable

### A HUD element changes side with render resolution, not with FOV

**Patola's observation, take 161:** the notification window that normally sits on
the **left** appeared on the **right** — not corrupted, but "well-formatted and in
the right perspective/positioning", i.e. deliberately laid out there. He checked
vanilla X4 to confirm it belongs on the left, and every prior SBS run put it on
the left.

**FOV is not the cause.** The take-155 screenshot pair is at `X4VR_FOV=2.21` with
a 1408² render, and cropping the bottom of its left eye shows the box plainly on
the **left**. The 163° field alone leaves it where vanilla puts it.

What differs in takes 159/160/161 is the **render resolution and window size** —
1408²/2816×1408 against 4224²/8448×4224. So the element tracks a size, not a
field. Unexplained, and it matters beyond curiosity: **#30 puts the HUD on a
canvas, and a HUD whose element positions move with resolution is a harder target
than one that does not.** Recorded before it is explained, and not yet chased —
the next step is a screenshot at 4224² to see where it lands and what else moved.

### The relocated notification is a **cockpit monitor**, and X4 has an API for it

Chased with two crops and a symbol sweep; no run needed.

**What the screenshot shows.** Zoomed, the box on the right is **skewed in
perspective, lying along the cockpit console surface** — world-space geometry,
not a flat overlay. And the bottom-left of the same frame is **empty**: the flat
notification did not duplicate, it *moved*. The identical console region in the
take-155 screenshot (same ship, same seat, same `X4VR_FOV=2.21`, but a **1408²**
render) is **bare**. So the trigger is render resolution, not field of view, and
the panel is genuinely new rather than something previously overlooked.

**What it is.** X4's own FFI names the feature outright:

    bool IsTargetMonitorNotification(const int notificationid);
    MonitorExtents GetMonitorExtents(const char* monitorid);
    void SetMonitorExtents(const char* monitorid, float x, float y, float w, float h);
    void SetMonitorRenderState(bool state);
    float GetHUDUIScale(const bool scalewithresolution);
    float GetUIScale(const bool scalewithresolution);
    ResolutionInfo GetGameResolution(void);
    ResolutionInfo GetRenderResolutionOption(void);

**All eight are exported**, so the injector can reach them the way it already
reaches `GetCameraRotation`. X4 routes some notifications to a *cockpit monitor*
instead of the flat HUD, and the UI scale has an explicit
`scalewithresolution` flag — which is the obvious candidate for why 4224²
flips the routing and 1408² does not.

**Why this matters more than the oddity did.** Patola's ruling was that the HUD
should track the cockpit, "as it's the vanilla look". A monitor notification
*already is* that: world-space, at the cockpit's depth, so it gets correct stereo
for free, it would follow the vertex-stage rotation of #33 for free, and it needs
no canvas at all. **#30's canvas may be the wrong mechanism for any UI X4 can
route to a monitor** — the right move would be to ask X4 to put it there rather
than to catch a screen-space quad and place it ourselves.

**Not yet established, and the standing rule applies before any call:** none of
these eight has had its disassembly read for preconditions, and `IsHUDActive` is
the precedent for a declared-and-exported function that segfaults if called at
the wrong time. What decides the routing is also still a guess — resolution is
correlated, not shown to be causal, and `GetHUDUIScale(scalewithresolution)` is
a hypothesis about the mechanism, not a reading of it. The next step is the
disassembly and the Lua that calls `IsTargetMonitorNotification`, not a run.

### The radar is not missing — it loses its background. That is an FOV cost.

Patola's second observation, and his wording was exact: "seems missing, or at
least I am not seeing it clearly". Cropping the same bottom-centre region from
all three shots settles it — the radar is **present in every one**:

    fov 1.4917, 1408²   dome + green arc against DARK SPACE      -- crisp
    fov 2.21,   1408²   dome + green arc against the CABIN FLOOR -- washed out
    fov 2.21,   4224²   dome + green arc against the CABIN FLOOR -- washed out

So the two oddities have **different causes, and neither is what the other looks
like**:

* **Notification relocation → render resolution.** Bare console at 2.21/1408²,
  monitor panel at 2.21/4224².
* **Radar legibility → field of view.** Identical washout at 2.21 on *both*
  resolutions, absent at 1.4917.

The mechanism is simple once seen. The radar is screen-space at a fixed screen
position; widening the field changes **what world geometry sits behind that
position**. At 110° it is dark space, at 163° the cockpit's bright floor and
consoles have swung into that part of the frame. Nothing moved and nothing broke;
the backdrop changed underneath it.

**This is an unpriced cost of the guard band.** Every section above costs the
wide field in pixels and milliseconds. It also costs *HUD legibility*, because
X4's screen-space elements were positioned and coloured against the backdrops a
~90° cockpit view produces. Any element we leave screen-space will get whatever
geometry the widened field puts behind it, and no amount of render resolution
fixes it — 4224² is exactly as washed out as 1408².

It is also a second, independent argument for the cockpit-monitor route: a panel
drawn **on** the console has its contrast designed against the console, and
cannot have its backdrop swapped by a field-of-view change.

### X4's HUD already lives on cockpit monitors, and one is a config tag away

Read out of X4's UI Lua in `08.dat`. No run.

**X4 has three named monitors**, and it treats their positions as geometry:

    targetmonitor    uianchorindex 0
    radar            uianchorindex 2   noright
    messageticker    uianchorindex 3   noright

`MonitorExtents GetMonitorExtents(const char* monitorid)` returns them, and X4's
own comment on that call says the extents are **"in worldspace coordinates"** —
the UI then projects them to screen positions to build exclusion zones so menus
do not overlap them. `void SetMonitorExtents(const char*, float, float, float,
float)` writes them, and X4 disables a monitor by zeroing it
(`SetMonitorExtents("radar", 0, 0, 0, 0)`).

**Notifications are routed by type, not by layout.** `queueNotification()` keeps
two queues — `messageTickerNotifications` and `targetMonitorNotifications` — and
picks between them with `C.IsTargetMonitorNotification(notificationID)`. So the
panel Patola circled is the **target monitor**, a distinct destination that X4
maintains on purpose, not a misplaced message ticker.

**The radar has three destinations**, chosen in `updateRadarExtents()`: the
target monitor, a separate radar element, or folded into the message ticker
(external view only). The switch is a config setting read straight into a local:

    private.enableSeparateRadar = C.GetConfigSetting("separateRadar") ~= 0

and `<separateRadar>true</separateRadar>` is a **shipped config tag**. Patola's
live `config.xml` carries `<separateRadar>1`.

**Why this matters for #30.** The injector already owns every read of
`config.xml` — that is how `X4VR_FOV` is served, non-intrusively, with no mod
registered and no savegame flag. `separateRadar` is the same kind of tag. If
turning it off routes the radar to the target monitor, then the radar becomes a
**world-space panel on the cockpit**: correct stereo for free, follows the #33
vertex-stage rotation for free, no canvas, and — directly answering the washout
Patola spotted — its contrast is designed against the console rather than
against whatever the widened field swings in behind it.

`GetMonitorExtents` returning world-space extents is the larger prize. It means
X4 will *tell us where its HUD is in the cockpit*, which is exactly the input
#30's canvas was going to have to guess.

**What is NOT established, kept separate from the above:**

* That `separateRadar=0` puts the radar on the target monitor. The three-way
  branch is read, but `isTargetMonitorRadarEnabled()` and the state machine it
  drives are not, so this is the plausible reading and not a fact.
* **Why the notification moved with render resolution — still open.** The one
  resolution-flavoured path found (`rendertargetheight` scaled by
  `private.uiScale` feeding `SetMonitorExtents("messageticker", ...)`) sits
  inside a `C.IsExternalViewActive()` branch, and Patola was in cockpit view. So
  that is **not** the explanation, and the earlier guess that UI scale drives it
  is withdrawn pending evidence.
* No disassembly read for any of these. `IsHUDActive` remains the precedent for
  a declared, exported function that segfaults when called at the wrong moment,
  and the standing rule applies before a single call.

### CONFIRMED: `separateRadar` is the switch, read from X4's own predicates

The previous section left this as "a plausible reading, not a fact". It is now a
fact, from X4's Lua. The two routing predicates are **exact complements on one
term**:

    isSeparateRadarEnabled()      = allowRadar and (not tickerOnlyMode) and radarEnabled and     enableSeparateRadar
    isTargetMonitorRadarEnabled() = allowRadar and (not tickerOnlyMode) and radarEnabled and not enableSeparateRadar

with X4's own comment above the second stating the integrated target monitor
radar is enabled when "we are not using the separate radar". The state machine
is an either/or:

    checkRadarActivation(force):
        isSeparateRadarEnabled()      -> activateSeparateRadar(force)  else deactivateSeparateRadar()
        isTargetMonitorRadarEnabled() -> updateActiveState(force)

and `activateMonitor()` keeps the target monitor alive **even in its inactive
state** when the radar lives there — so it is a persistent panel, not one that
blinks in and out with target selection.

Identical preconditions, one differing term, and that term is
`C.GetConfigSetting("separateRadar") ~= 0`. `<separateRadar>` is a shipped tag
and Patola's live config carries `1`.

**Wired as `X4VR_SEPARATE_RADAR`**, env-gated exactly like `X4VR_FOV`: unset
serves the profile byte-for-byte (the control), `0` or `1` overrides, anything
else is rejected rather than served malformed. The effective value is logged
every run whether overridden or not, for the same reason `fov` is — a run that
changed the routing without recording it could not be told from one that did not.

This is the project's existing non-intrusive lever: no mod registered, no
savegame flag, the player's `config.xml` read and never written.

### Take 162 — `separateRadar=0` makes the radar WORLD geometry. Measured, not eyeballed.

> **RETRACTED by #55 — see the end of this file.** It was measured, and the
> measurement was of the wrong thing: the −30 px belongs to the cockpit panel
> the radar is drawn on. Splitting the same window by chroma gives the radar
> overlay **+0 px**. The section is left exactly as written, including the
> "measured, not eyeballed" boast, because the way it went wrong is worth more
> than the conclusion was.

The override landed (`config: effective separateRadar=0 (from X4VR_SEPARATE_RADAR)`)
and the radar moved from the flat bottom-centre dome to the right of the cockpit.
Zoomed, its axes looked screen-aligned and I was ready to call it "repositioned
but still flat". **That read was wrong.**

The screenshot contains its own test. Our stereo shear displaces by *inverse
depth*, so a screen-space draw at `w = 1` is immune by construction — that is why
the HUD could never be sheared. So per-eye disparity separates the two cases
without any new instrument. Cross-correlating each element between the eyes of
the SBS frame:

    radar (right side)          disparity -30 px   ncc 0.744   <- WORLD geometry
    message ticker (left)       disparity  +0 px   ncc 0.893   <- screen-space
    left console panel          disparity -40 px   ncc 0.864   <- world, near
    cockpit centre (distant)    disparity  +0 px   ncc 0.722   <- world, far

The radar sits at a disparity comparable to the nearby console and nothing like
the ticker's zero. **It is world geometry at the cockpit's depth.**

So the routing does what the Lua said it would, and the consequences are real:

* the radar **already has correct stereo** — not "will get", has, in this frame
* it would follow #33's vertex-stage rotation for free, being world geometry
* it needs **none** of #30's canvas
* and its backdrop is the console, so the wide-FOV washout cannot recur on it

The message ticker's `+0 px` is the other half of the finding: it is still
screen-space, so **the canvas is still required for it.** The two HUD elements
are now known to be in different classes, measured rather than assumed.

### Decision: do not force `separateRadar`

Patola: "I thought it could be whatever the user chooses and we would adapt."
Agreed, and the override list needed a distinction drawn that had been implicit:

* `ssr`, `antialiasing`, `chromaticaberration` are forced because they are
  **incompatible with stereo** — SSR reprojects one view's depth buffer and would
  mismatch between the eyes.
* `separateRadar` is a **preference**. Both modes render correctly; only their
  VR *cost* differs.

Only the first category justifies overriding a player's choice.
`X4VR_SEPARATE_RADAR` therefore stays unset by default, serving the profile
byte-for-byte, and is an experiment lever rather than a shipped default.

The asymmetry is worth stating plainly for planning: **supporting both modes
means the canvas work still has to happen**, because a player who keeps the flat
radar needs it placed. `separateRadar=0` does not delete that work unless we
force the setting, which we will not. What it earns is a mode where the radar
costs nothing, and a good reason to *recommend* it in VR.

## X4 ships FSR3, and the odds it survives our layer — predicted before the test

Patola asked what the chances are that X4's FSR helps with VR's resolution
demands. Answered from the binary and the Lua, with the prediction recorded
before any run.

**What X4 has.** `ffxFsr3UpscalerContextCreate`, `ffxFsr3ContextCreate`,
`ffxFrameInterpolationContextCreate`, `ffxOpticalflowContextCreate`,
`ffxCasContextCreate`, and DLSS alongside. So this is **FSR3** — a temporal
upscaler — not the spatial FSR1 that would have composed harmlessly.

**How it is selected.** Through `<antialiasing>`, whose accepted values are
`none temporal dlaa msaa_2x/4x/8x/16x fsr fsr_ultra_quality fsr_quality
fsr_balanced fsr_performance`. `SetMultipleGfxModes3(aamode, upmode, ...)` sets
AA and upscaling together and `IsFSROnWithoutAA()` exists, so X4 treats the two
as one control. We have served `antialiasing=none` since the beginning.

**Prediction: the FSR3 upscaler will not work with the layer as it stands.**
Three concrete reasons, in order of confidence:

1. **Jitter lands in the shear's own slots.** `layer/x4vr_layer.cpp:4528` already
   records that TAA jitter occupies `m[8]/m[9]` — the elements the per-eye offset
   writes. A temporal upscaler *requires* sub-pixel jitter, every frame. This is a
   documented collision, not a guess, and it is why every temporal mode in that
   list is suspect, not just FSR.
2. **The history is single-view and we render two.** FSR3 accumulates across
   frames for one camera; we render both eyes as array layers under multiview.
   The reactive masks X4 carries (`fbo_fsr_mask_reactivity`,
   `fbo_fsr_mask_transparency`) are single-view resources with the same problem.
3. **Motion vectors are computed for X4's projection, not the patched one**, which
   is the standard recipe for ghosting.

**CAS alone should survive** — spatial, no history, no jitter — but it sharpens
rather than upscales, so it does not buy the resolution saving that motivated the
question.

**Frame generation stays out of reach on purpose.** X4 keeps it in its own tags
(`dlssg`, `fsrframegen`) and `X4VR_AA` deliberately cannot select it: an
interpolated frame carries no valid head pose and adds latency, so it is harmful
in VR whether or not upscaling works.

**Falsifiable outcomes**, so the run reports rather than puzzles:

* ghosting or smearing on motion → reason 2 or 3
* one eye right and one wrong → reason 2
* stereo collapses or depth goes flat → **reason 1**, and that also settles
  whether TAA could ever be enabled

**Worth stating: the goal may not need FSR at all.** The measured cost curve is
superlinear, so plain resolution reduction is already a strong lever — 9× → 4×
pixels took 20.77 ms → 9.82 ms, less than half, with no upscaler. And the render
size and the eye/swapchain size are independent knobs we own, so rendering small
and upscaling in our own composite is available without entangling anything.

**Wired as `X4VR_AA`**, unset by default (the control), validated against the
twelve accepted strings and rejecting anything else rather than serving a value
X4 would silently ignore. It *overrides* the base `none` entry rather than being
appended after it, so one tag has one source. The effective value is logged every
run either way — a run that changed it must say so, or broken stereo reads as a
regression in the shear rather than as the knob that caused it.

### RETRACTED: FSR3 works. The prediction above was wrong, and here is why.

Take 163, in VR at 4224² per eye, with a paired `X4VR_AA=none` control in the
same log:

    config: effective antialiasing=fsr_quality (from X4VR_AA)
    config: effective antialiasing=none (our default, not the profile)

Patola's verdict: FSR3 works well. No ghosting, no smearing, no misplaced eyes,
**depth still perceived correctly**, on both the flatscreen test (which had
`X4VR_STEREO`/`X4VR_SBS` active, so it did exercise the shear) and in VR. The
prediction two sections up is **withdrawn**, and it is left standing there rather
than edited, because the reasoning failure is more useful than the conclusion.

**Why reason 1 was wrong — a misread comment.** `x4vr_layer.cpp:4528` says TAA
jitter occupies `m[8]/m[9]`, "the shear's own slots". I read that as a live
collision. It is not: that comment explains why the layer *reads* `M_projectionUJ`
rather than the jittered `M_projection`. **The shear is never written into X4's
matrix at all** — `patch_vertex_eye_offset()` applies it as a vertex transform in
the patched shader, downstream of whatever X4 put in its projection. Jitter and
shear occupy the same *named slots* in two different matrices that never meet.
Reading a comment about which matrix to trust and inferring a collision the
architecture does not have is the same error as the guard that could only agree.

**Why reasons 2 and 3 were wrong — Patola's hypothesis, with numbers.** He
proposed that the two eyes are near-identical apart from a small IPD offset, so
motion vectors and the temporal history still apply cleanly. The scorer's own
stereo line quantifies exactly that, and it is stronger than it first looks:

    per-eye offset  10.1 px at 1 m,  2.0 at 5 m,  1.0 at 10 m,  0.3 at 30 m  (4224 px eye)

The between-eye error in any shared motion vector is bounded by that disparity —
which is largest exactly where motion vectors are smallest. **The cockpit is rigid
with the camera, so its motion vectors are ~zero however large its disparity is;
distant geometry has real motion vectors but ~0.3 px of disparity.** The bad case
needs an object both close *and* moving fast relative to the camera, which a
cockpit view rarely produces. His reasoning was right and mine was a list of
mechanisms that never met their preconditions.

**Kept as a live option, not a default.** `X4VR_AA` stays unset by default. Deeper
testing is deferred to release-time on Patola's call.

### The map and steering failures in take 163 were head-look, not FSR

Both runs carried `X4VR_HEADLOOK=1`, and the log says what happened:

    1118  headlook: head ... (steering)
      34  headlook: NOTE commanded with little camera response — reported only
       3  headlook: view is now the cockpit — steering
       3  headlook: view is now external/floating — idle

Head-look holds `INPUT_STATE_CAMERA_MOUSELOOK`, and holding it is what X4 uses to
suspend ship control. "I could not turn" is therefore the **known, measured cost
of head-look** — the exact conflict that led to free-look being excluded from the
design — and the 34 stall notes plus the map misbehaviour are takes 146–148's
map-gate territory, not new. Neither has anything to do with the upscaler.

**Consequence for future FSR testing: drop `X4VR_HEADLOOK`.** It is not in the
design, it suspends steering, and it disturbs the map — three confounds on a test
that does not need it. Ship motion supplies all the motion a temporal upscaler
needs to be stressed.

## #35 audit — the math is done and tested; three wiring pieces remain, in order

**Done, and better than the task description suggests.** `common/x4vr_view.hpp`
carries the whole derivation with `EyeFrustum`, `frustum_of_angles()`,
`make_off_axis()`, `apply_off_axis()`, `apply_off_axis_folded()` and
`union_half_angle()`. `tests/view_math.cpp` is 625 lines and ~51 assertions, and
it tests the **deployment case**, not a toy one: the take-112 frusta, from which
it re-derives `union_half_angle = 55.0°` and `X4VR_FOV = 1.4917`, and pins the
concrete coefficients `A_x = 1.2892`, `B_x = +0.2425` / `−0.2425` mirrored. It
also keys on the property that the map **collapses to the identity** when the
target is symmetric — a map that is not the identity where it must be is wrong
everywhere.

**Not done: `make_off_axis`/`apply_off_axis` are called from nowhere but the
tests.** That is the entire remaining task, and it is three pieces with a
correctness constraint on their order.

1. **The SPIR-V emission.** The affine has the same shape the eye-offset patch
   already emits — a scale of `clip.x` plus a `w`-proportional term, with `sx`
   read live from the camera block. `x4vr_view.hpp:363` states the intended
   composition: applied *after* the shear, so no eye-offset term is needed in the
   off-axis step. `patch_vertex_eye_offset()` is ~385 lines
   (`common/x4vr_spirv.hpp:698`), and the twelve camera-blind modules fall back to
   `patch_vertex_eye_offset_mvp()` — so **both** need the term, exactly as the
   shear did, or those twelve get the right shear and the wrong frustum.
2. **The runtime declaration.** The layer currently declares a symmetric ±55°
   field and the runtime honours it (`x4vr_layer.cpp:7142`, and take 161's log:
   "declaring a symmetric field of +-55.00 deg per eye"). It must declare the
   runtime's own canted frusta instead.
3. **The render extent.** Only then does the pixel saving arrive: 1092×1180 per
   eye instead of 1408², the 0.65× the guard-band table prices.

**The ordering is load-bearing, not stylistic.** (2) before (1) declares an
asymmetric field while still rendering a symmetric one — the image would be
wrong in the headset with every piece individually "correct". (3) before (1)
crops away geometry that nothing has yet remapped. **(1) must land first, and
alone, with its own take.** Each of the three is separately observable, which is
what makes them separate takes rather than one.

Recorded rather than begun: (1) is a careful edit inside a 385-line function that
sixty passing takes depend on, and `x4vr_spirv.hpp:1078` says why the three
patches deliberately repeat each other's scans instead of sharing them — "a
refactor that broke all three at once is the expensive mistake here". That is
work for a fresh session, not the tail of a long one.

## #35 piece 1 — the off-axis affine is emitted; the take that tests it

Piece (1) of the three above. The map from X4's symmetric frustum to the
runtime's canted one now exists in the shaders instead of only in
`tests/view_math.cpp`, in **both** vertex patches, and the layer can ask for it.

### What changed

`emit_off_axis()` in `common/x4vr_spirv.hpp` emits `apply_off_axis` verbatim:

    x' = (ax_num/sx)·x + bx·w        y' = (ay_num/sy)·y + by·w

after the eye shear, on clip x and clip y, with `sx` and `sy` read live per
draw. `patch_vertex_eye_offset` reads `sy` from `M_projection[1][1]` beside the
`[0][0]` it already read; `patch_vertex_eye_offset_mvp` recovers `|sy|` from
`row1(MVP)` by the same argument that gives it `|sx|` from `row0`.

The emitter is **shared** by the two patches, where their scans are deliberately
duplicated. That duplication protects field-proven code from a refactor; this is
new in both callers at once, and the failure it guards is the opposite one — a
sign fixed in one copy and left wrong in the other, which is a defect this
project has already shipped once.

### The one assumption, named

The mvp form recovers a *magnitude*. The sign of `sy` is not in the block and is
supplied: **negative**, from 35,783 sampled camera blocks across every log kept,
none positive. It is the Y-flip Vulkan's downward NDC y requires, and it is the
same convention `make_off_axis`'s negative `ay_num` is derived from — so this
adds no failure mode of its own, and if X4 ever flipped it every module would
flip together rather than these twelve alone. The layer now warns once, by name,
if any camera ever reports a non-negative `sy`.

### What is verified before the run

* **Byte-identical when not requested.** The pre-#35 build was compiled from
  `git show HEAD:` and run against the same inputs: 2188 bytes for the camera
  block form and 3240 for the mvp form, mono and stereo, all four identical. So
  every state tagged before #35 reproduces by leaving `X4VR_OFFAXIS` unset.
  Reproduce with:

      g++ -std=c++17 -O1 -o /tmp/ref tests/spirv_patch.cpp   # from an old tree
      cmp <(ref ...) <(new ...)

  The suite cannot hold this check permanently — there is no "before" once it is
  committed — so it asserts the structural form instead: zero `OpLogicalAnd` and
  zero `OpFDiv` added when the affine is absent.
* **The emitted constants are `make_off_axis`'s**, checked against the numbers
  `tests/view_math.cpp` already pins for the take-112 frusta: `ax_num` 0.902738,
  `bx` +0.242513, `ay_num` −0.835479, `by` −0.193187, and the eye-1 mirror as a
  difference of −0.485025.
* `spirv-val` passes on every form; the affine writes exactly two components.
* A backwards frustum, a malformed string and a right-eye map with no left are
  all **refused**, leaving the module untouched.

### PREDICTION, before the take

Two runs, identical but for one value.

**A — the negative control.** `X4VR_OFFAXIS="-55,55,55,-55"` at
`X4VR_FOV=1.4917`. This is the identity, and not empirically: the take-112 pair's
`union_half_angle` is 55.00°, `X4VR_FOV` 1.4917 puts X4's own half-angle at the
same 55.00°, so `ax_num = cot 55° = sx` and `A_x = 1` exactly with `B_x = 0`.
**The picture must be indistinguishable from the same command line with
`X4VR_OFFAXIS` unset.** Any visible difference is a defect in the emission, and
it is the emission that is on trial here, not the frusta.

**B — the canted target.** `X4VR_OFFAXIS="-54,40,44,-55"`, the measured Quest 3
eye 0. Against the fov camera the scorer will report `A_x=1.2892 B_x=+0.2425,
A_y=1.1931 B_y=-0.1932`, and the picture should be **magnified 1.289×
horizontally and 1.193× vertically, and shifted 12.1% of the image width and
9.7% of its height**. An unchanged picture means the affine reached no draw —
which is a different failure from a wrong picture, and the scorer's `baked-sx`
line separates them.

The horizontal shift goes **opposite ways in the two SBS halves** and that is a
second, independent prediction rather than a detail: `B_x` is +0.2425 for view 0
and −0.2425 for view 1 (the layer logs both), so the left half moves right and
the right half moves left, by the same 12.1%. The vertical shift is the *same*
in both, because a headset cants its eyes horizontally and the mirrored frusta
share `u` and `d`. If both halves move the same way horizontally, the per-view
selection is not selecting — the affine ran with one eye's coefficients in both
views, which is a distinct defect from it not running at all and would otherwise
be invisible in a run that only checked "did the picture change".

I expect A to be clean and B to look exactly as stated. The risk I cannot price
from here is the **other cameras**: `A_x = ax_num/sx_live` divides by whichever
camera is drawing, so a camera at a different `sx` is remapped from its own field
into the target — correct by construction for the eye, and a behaviour change for
the cockpit monitors and the map, which run at `|sy/sx|` 1.0 and 1.5. That is
inherited from the live-sx shear rather than introduced here, and B is the run
that will show it.

**What this take does NOT establish.** Nothing about the headset. The layer still
declares a symmetric ±55° field (piece 2) and still renders 1408² per eye (piece
3), so in VR the declaration and the render disagree with the affine on purpose.
B is a flatscreen measurement of a shader transform, and reading it as "off-axis
works in VR" would be reading three pieces from one.

## #35 piece 1 — RESULT: confirmed, and the instrument was wrong twice first

Takes 164a (baseline), 164b (identity control) and 164c (canted target), all
three scorer PASS, measured by registering the screenshots with
`tools/register_affine.py`.

### 164b, the identity control — exact

    view 0:  A_x=1.0010  B_x=-0.0014   A_y=0.9990  B_y=-0.0014
    view 1:  A_x=0.9990  B_x=+0.0014   A_y=0.9990  B_y=-0.0014

Every deviation is one step of the estimator's own resolution (scale step
0.002, shift step 1 px = 0.0014 NDC). The 2D check agrees more sharply still:
correlation at the identity 0.9464, correlation at the "predicted" identity
0.9464, and a coordinate descent free to move any coefficient by ±0.10 moved
none of them. **A symmetric target at X4's own half-angle is a no-op through the
whole emission**, which is the arithmetic the prediction claimed and not an
approximation to it.

### 164c, the canted target — all eight coefficients

Two independent methods: 1D gradient-profile registration, and a 2D
hypothesis test that scores the predicted warp against the identity and then
descends from it.

    coefficient   predicted     1D view0 / view1     2D best-fit view0 / view1
    A_x            1.2892       1.2850 / 1.2490      1.2809 / 1.2892
    B_x           +0.2425      +0.2385 / -0.2512    +0.2425 / -0.2425
    A_y            1.1931       1.1950 / 1.1870      1.1931 / 1.2014
    B_y           -0.1932      -0.1950 / -0.1936    -0.1932 / -0.1932

The 2D test is the decisive one: the predicted warp lifts correlation from
**0.219 to 0.647** in view 0 and **0.233 to 0.595** in view 1, and the local
optimum sits on the prediction — of the eight coefficients it was free to move,
six did not move at all and two moved by under 0.7%.

**`B_x` reverses sign between the views and nothing else does**, which was the
second prediction and the one that separates "the affine ran" from "the affine
ran per view". The eye-1 constant is emitted as a difference of −0.485025, and
the picture carries it.

The 1D method flags a low margin on 164c and that flag is correct: a 1.29×
magnification leaves competing global alignments, and the cockpit's evenly
spaced struts are exactly the structure that aliases. It is resolved by the 2D
test rather than by ignoring it — agreement with eight coefficients predicted
before the run is not something an alias produces.

The HUD did not take the affine, as the classification requires: the bottom-left
HUD arc moved ≤ 25 px between 164a and 164c while world geometry at that corner
moved 171 px and magnified 1.29×. The correlation peak there is weak (0.041
against the control's 0.220), so this is corroboration and not a measurement —
some cockpit console bleeds into the crop, and it did move.

### The two instrument defects, which are the part worth remembering

**1. The self-check fixture was periodic.** Bars on a 38 px pitch; the third
case came back +0.1016 against a wanted −0.1500, which is 64.4 px out — exactly
two pitches after the 0.85 scale. The fixture was ambiguous, not the estimator.
Fixed by making the bar spacing random, and by making `fit_1d` return a
**margin**: the best score minus the best score at least one lobe away. A search
that reports only its winner will hand back an alias with full confidence.

**2. Cropping to a band re-referenced the transform, and I nearly reported it as
a shader defect.** The affine's fixed point is NDC 0 — the centre of the *frame*
— and `B_y` is a fraction of the frame's half-height. Cropping rows to the
0–0.72 band and then taking the band's own centre biases the vertical shift by

    (A_y - 1) x (frame centre - band centre) = 0.1931 x 196.9 px = 38 px
                                            = 0.0539 in NDC

against a `B_y` of 0.1932. The first measurement read −0.248 against −0.193
predicted, in both views, consistently — which is exactly what a real
disagreement looks like. **A residual that is proportional to the effect being
measured is a sign the instrument shares the effect's parameters, not a finding
about the subject.** The self-check had passed because it only ever ran on the
full frame, the one configuration in which the bug cannot appear; it now runs
every case through both the full frame and the 0–0.72 band.

### What this does and does not close

Piece (1) is done: the emission is correct, per-view, and reaches every world
module (`baked-sx=0` in all three runs, so nothing was left in X4's frustum).

It still says nothing about the headset. The layer declares a symmetric ±55°
field (piece 2) and renders 1408² per eye (piece 3). Those remain the next two
steps, in that order.

## #35 piece 2 — declare the runtime's frusta, and what it is actually worth

Piece (2). The layer now latches ONE frustum pair and uses it for both the
affine's baked coefficients and `XrCompositionLayerProjectionView::fov`. Sources
in order: `X4VR_OFFAXIS` (an override, and `=off` disables everything), the
runtime's located views, off.

### Correctness needed them equal to each other, not equal to the runtime's

Worth stating plainly, because it changes what this take can claim. If the
affine remaps X4's field into frustum F and we declare F, the compositor is
right *whatever F is* — it resamples. Matching the runtime's own F is a quality
choice. **Take 163 was therefore already geometrically correct**: it declared a
symmetric ±55° field, the eye's real frustum (−54..+40 × +44..−55) sits entirely
inside that, and Monado cropped to it. Nothing was wrong with the picture.

**What was wrong was the pixel budget.** The eye's tangent span is
`tan40 − tan(−54) = 2.2155` against the declared `2·tan55 = 2.8563`, so 22.4% of
every row was rendered and then discarded, and the same again vertically. The
affine maps the smaller span onto the whole texture, so the *same* 1408 pixels
now cover only what the display shows.

That ratio is not a new number to measure. It is `A_x` and `A_y`:

    horizontal pixel density  x1.2892      vertical  x1.1931

The magnification the affine applies IS the angular-resolution gain, because it
is the same tangent-span ratio read the other way round. Piece 3 will spend that
back as performance instead (1092×1180 per eye at the original density); until
then it arrives as sharpness at unchanged cost.

The texture stays square while the declared frustum's aspect is
`2.2155/2.3938 = 0.9255`. The affine reconciles them — that is exactly why
`A_x ≠ A_y` — and piece 3 is what stops paying for the mismatch.

### Latched once, and the race that decides the source

The coefficients are baked at `vkCreateShaderModule`; the runtime's frusta only
exist after `xrLocateViews`. Take 163's timestamps settle whether that works:

    122569.463   first shader module patched     (NONWORLD — no affine on it)
    122569.475   first located frame
    122570.269   ...still nonworld
    ~122570.3    first WORLD module              (+0.8 s after the locate)
    122647.125   world modules STILL arriving    (+78 s)

So the frusta are in hand 0.8 s before the affine needs them, and world modules
keep coming for 77 s after — which is why the target is latched once rather than
read per module. A target that could change between two of those calls would put
half the world in one frustum and half in another, and no single frame would
look wrong enough to explain it.

The 12 ms gap at the top is why the per-module log line uses a non-latching
peek: latching there would fix the target at the first *nonworld* module, before
any locate, and the runtime would lose the race every run with nothing saying so.

### PREDICTION, before the take

Two runs, identical but for `X4VR_OFFAXIS`.

**A — `X4VR_OFFAXIS=off`.** Reproduces take 163's geometry exactly. This switch
exists because piece 2 turns the affine on by itself in any VR run, which
changes what every VR command line written before this commit does;
`X4VR_PROJ_INVPROJ` already cost this project a documented control that silently
stopped being one.

**B — `X4VR_OFFAXIS` unset.** The runtime supplies the target. The log must show:

    vr fov: runtime reports eye0 l=-54.00 r=40.00 u=44.00 d=-55.00, eye1 ...
    offaxis: target from the runtime's located views
    offaxis: X4VR_FOV 1.4917 covers the target's union half-angle of 55.00 deg
    vr: declaring the CANTED field the affine was baked for

If the first line disagrees with take 112's measurement, that is news about the
runtime and the rest of this section is about the wrong numbers.

**In the headset, B should look SHARPER than A and otherwise identical** — 1.29×
horizontally, 1.19× vertically. Cockpit monitor text and distant ship outlines
are where that shows. Geometry must be unchanged: straight lines straight, the
two eyes fusible with no vertical disparity, the world the same size.

**A wrong-size or shifted world in B refutes the assumption that Monado honours
an asymmetric declared fov the way probe run 3 showed it honours a symmetric
one.** That is the one thing here resting on an untested extrapolation —
asymmetric is OpenXR's ordinary case, but this runtime has only been observed
with our symmetric declaration.

Both runs drop `X4VR_HEADLOOK` and the `X4VR_CAMREAD`/`X4VR_CAMLOOP` readback:
take 163's map and steering failures came from head-look holding `MOUSELOOK`,
and this take asks nothing about head tracking. The image is world-locked, so
looking around walks off the rendered edge into black in both runs — sooner in
B, which now has no margin at all where A had about 1° horizontally.

**What this take does NOT establish.** Performance. B renders the same 1408²
per eye as A; the pixel saving is piece 3 and nothing here measures it.

## #35 piece 2 — RESULT: the world is right and everything else is now unfusable

Takes 165a (`X4VR_OFFAXIS=off`) and 165b (runtime frusta). **Every mechanism
piece 2 was built to provide worked, and the run is still a failure.**

### What worked

    vr fov: runtime reports eye0 l=-54.00 r=40.00 u=44.00 d=-55.00,
                             eye1 l=-40.00 r=54.00 u=44.00 d=-55.00
    offaxis: target from the runtime's located views
    offaxis: X4VR_FOV 1.4917 covers the target's union half-angle of 55.00 deg
    vr: declaring the CANTED field the affine was baked for
    ... baked-sx=0

The runtime reports take 112's frusta to the centidegree, the latch took them,
the declaration is built from the same object, and no world module was left
behind. Registering 165a→165b puts the world at `A_x=1.2975 B_x=±0.2425
A_y=1.1848/1.1931 B_y=−0.1932` — the prediction, with `B_x` reversing per view.
165a is crisp and correct.

### What broke, and it is not a wiring bug

165b in the headset: the cockpit's 3D is correct, the **HUD has its eyes
switched**, so does part of the stars plane and the whole loading screen, and
the menu's shadows are completely different between the two eyes.

**Cause 1 — everything the affine does not touch is now in the wrong frame.**
The affine rides on the two live-sx World patches. NonWorld modules, the
unsheared twin, the skybox's procedural fullscreen shader and the loading screen
get none of it, so they stay in X4's symmetric frame while the declaration says
canted. A screen-locked element sits at image centre in both eyes; declared
canted, image centre *is* the frustum centre, which is −15.04° in eye 0 and
+15.04° in eye 1:

    the eyes are asked to diverge by 30.07 deg

That is not a new number. `common/x4vr_view.hpp:291` already carries it, as the
**negative control**: "Submitting X4's frame as if it were centred asks the eyes
to diverge by 30.07 deg, which nobody can fuse; the probe reproduces exactly
that as a negative control." Piece 2 reproduced the project's own documented
unfusable case for every draw the affine misses, and I did not notice while
writing the code that cites it.

Measured, to be sure it is divergence and not a swap: in 165b's eye image the
HUD panel sits at the same place in both eyes (dx = +1 px of a 1412 px half),
exactly as in 165a (+2 px). The pixels are fine. The frame they are declared in
is not.

**Cause 2 — the deferred passes reconstruct through an affine they cannot see.**
`patch_fragment_invproj_eye` corrects `M_invprojection` for the eye *shear*
(task #22). The affine is a second transform on `gl_Position` downstream of
that, so every deferred reconstruction now recovers the wrong view position —
by an amount that differs in sign between the eyes, because `B_x` does. Hence
per-eye lighting and shadows in the menu. This one is a **piece-1** defect, not
piece 2: it was present in take 164c and went unobserved because that scene was
a starfield with almost nothing shadowed.

It is also the same defect class #22 already fixed once. `x4vr_spirv.hpp`'s own
note — "the deferred passes then read the depth buffer and reconstruct with the
centre-frame `M_invprojection`" — describes the affine's failure exactly, one
transform later.

### Why piece 1's verification could not have caught either

Worth stating plainly rather than filed as bad luck. Takes 164a/b/c were
**flatscreen SBS**: no declaration exists there, so cause 1 is structurally
invisible. And I measured **world geometry only** — I registered the cockpit and
checked that the HUD had not moved *between* takes, never that the two eyes
agreed *within* one. A per-eye check inside a single frame would have shown
cause 1's precondition, and a scene with a lit surface would have shown cause 2.

### What changed as a result

The runtime source is now **opt-in** (`X4VR_OFFAXIS=runtime`). It was the default
for exactly one take, and that default ships the 30.07° divergence to anyone
running the VR command line. Unset is off and silent, which restores take 163's
behaviour as the VR default. `X4VR_OFFAXIS="l,r,u,d"` still reproduces takes
164a/b/c.

### What piece 3 now depends on

Nothing about piece 3 has changed, but it is no longer next. Two things are:

1. **The non-affine set has to reach the declared frame.** Not by giving every
   NonWorld module the affine — that would corrupt every fullscreen post pass,
   which is why `K_nonworld` exists separately at all. The canvas variant (#30)
   is the machinery that already chooses a transform per *pass* rather than per
   module, and this is the same question it was built for.
2. **The deferred reconstruction has to undo the affine**, the way it already
   undoes the shear. Same file, same shape, one transform further on.

Until both, the off-axis map is a flatscreen-verified transform and not a VR
feature, and the honest state of #35 is: piece 1 correct, piece 2 correct and
insufficient.

## Take 165b's stars — the affine cannot reach the skybox, and P12 no longer holds

Patola's follow-up: the radar, the speed gauge and **some** stars moved by large
amounts while other stars did not, which reads like a classification error.
Measured per feature, by scoring each region against two hypotheses — that it
carries the affine, or that it is unchanged — with the global warp as the
affine hypothesis:

    cockpit strut          affine +0.464  static +0.293   AFFINE   (world control)
    dashboard              affine +0.568  static +0.020   AFFINE   (world control)
    open sky, no geometry  affine -0.019  static +0.460   STATIC
    speed gauge "0 m/s"    affine +0.216  static +0.558   STATIC
    radar disc (dashboard) affine +0.430  static +0.001   AFFINE
    the circled star       affine +0.203  static +0.152   undecided

**It is not a misclassification.** Every one of those is the current rule
working exactly as written:

* The **skybox is a compute shader** — `docs/frame-analysis.md:3490` and take
  165b's own log ("6 of those are compute: no gl_ViewIndex exists there"). It
  has no vertex stage, so there is nothing for either vertex patch to edit, and
  no `gl_ViewIndex` to select on. The affine cannot reach it **by any mechanism
  this project currently has.**
* Things in the sky that are *world geometry* — distant ships, asteroids,
  station lights — go through World modules and do carry it. That is the split
  behind "some stars but not others". The specific circled star is undecided at
  this crop size and I will not claim it either way.
* The **HUD is two things, not one**, which #30 already measured: the radar disc
  is projected onto the cockpit dashboard and is world geometry (AFFINE), the
  speed gauge is a screen-space overlay (STATIC). Both are classified right.

### P12 is retracted

`docs/frame-analysis.md:3505` records:

> **P12** — forcing the two eyes to differ everywhere *except* the skybox
> produces no visible seam or misregistration at the sky.

with the justification "parallax at infinity is zero: both eyes share a rotation
and differ only by a translation, so an infinitely distant sky is genuinely
identical between them."

**That reasoning is sound for the shear and void for the affine.** The shear is
a translation, and a translation has no effect at infinity — so a mono skybox
cost nothing, and P12 was right for every take up to 164. The affine is a
re-centring in TANGENT space: it changes where a given *direction* lands on
screen, and a direction is the only thing an infinitely distant sky has. It
applies at infinity exactly as strongly as at one metre.

So the affine does not merely leave the skybox unimproved. It moves the world
out from under a sky that stays put, in opposite directions per eye. That is
what Patola saw, and it is a consequence of piece 1 rather than of piece 2 —
visible in 164c too, had anyone compared the two halves' sky against their
cockpit.

### The complete list of what the affine does not reach

1. **The compute skybox.** No vertex stage, no `gl_ViewIndex`. No mechanism.
2. **Screen-space UI** — the speed gauge, the message panel, the loading screen.
   Needs #30's decision, not a classification change: giving every NonWorld
   module the affine would corrupt every fullscreen post pass, which is the
   reason `K_nonworld` is a separate matrix at all.
3. **The deferred reconstruction.** 234 fragment modules are corrected per eye
   for the *shear* (#22); none of them knows about the affine, so every
   reconstructed position is wrong by it, and by opposite amounts per eye.

Piece 3's entire prize is 0.65x the pixel area. Take 165a is already correct.
The trade is therefore explicit: that saving costs all three of the above, one
of which has no known mechanism, against a symmetric declaration that works
today and only wastes pixels.

## Reading the skybox module — a correction, and what is actually established

I told Patola the skybox is a compute shader and therefore unreachable, citing
`docs/frame-analysis.md:3490`. **I had not verified that, and the part I could
check does not support it.**

**What the dumps say.** Exactly two of X4's 409 modules are compute *and* sample
a cube: `mod-0266` and `mod-0267`. Both build a `ray_dir` array of **six**
`vec3`s — `(1,-y,-x)`, `(-1,-y,x)`, `(x,1,y)`, `(x,-1,-y)`, `(x,-y,1)`,
`(-x,-y,-1)` — the six faces of a cube. They are cubemap **bakers**, not the
screen draw. Of the other 140-odd cube-sampling modules, every one is
Vertex+Fragment and every one declares a set-3 block, so all classify World and
would carry the affine.

Of the ten compute modules in the frame: three are the cursor, one a composite,
one deferred lighting with shadow samplers, two the cubemap bakers, one
volumetric fog (`vol`, `sampler3D`), one samples an SRGB 2D, and `mod-0361`
writes a module-scope constant — a clear. **None is identifiably the screen sky,
and I did not find the module that draws it.**

**What IS established, by measurement rather than by reading.** Star positions
in take 165b, left eye against right eye, matched as a point set:

    sky (canopy opening)   dx=0: 54.5%   dx=+342: 10.6%   dx=-342: 10.9%
    lower frame (cockpit)  dx=0: 19.6%   best 30.8% at dx=-388

If the sky carried the per-eye affine the two eyes would differ by
`2·B_x = 0.485` NDC = 342 px. They differ by **4 px**. The cockpit in the same
frame differs by hundreds. So:

* **the sky does not carry the per-eye affine** — measured, not inferred;
* **the cockpit does**;
* and that is "some stars moved, others did not": bright points that are world
  geometry move, the sky behind them does not.

An earlier region test of mine reported "open sky STATIC" and an A→B star count
reported 62% moving with the affine. Those disagree because the star detector
also picks up distant ships, station lights and cockpit edges. The
left-eye/right-eye test above is the one to trust: it needs no cross-run
alignment and carries its null (±342) inside the same frame.

**The lead I did not chase.** `mod-0361` writes with `OpImageWrite` whose
coordinate is a **3-component** bitcast of `gl_GlobalInvocationID` — an array or
3D target, with `z` from the dispatch. If X4's compute passes address layers
through `z`, then `gl_GlobalInvocationID.z` *is* a view index and compute is not
the dead end the compute-gap note assumes. Decidable with
`X4VR_MV_INVENTORY=1`, which take 165b's log says was not enabled.

So the honest state is: **the affine does not reach the sky, and whether it
*can* is unknown** — not "cannot", which is what I said.

## The sky DOES carry the affine — retracting the previous two sections

Patola asked about the nebula: the star he circled sits on blue nebula in one
eye and on black in the other, "which makes it appear like very different
images". Measuring that properly overturns what I wrote above.

**Template-matching a patch of the right eye inside the left eye**, four
independent pure-sky patches, no HUD and no cockpit in any of them:

    run A (affine off)   dx = -2, -2, -2, +51     (peaks 0.94, 0.95, 0.98, 0.80)
    run B (affine on)    dx = +251, +383, +369, +385  (peaks 0.65, 0.78, 0.71, 0.66)

Run A: the sky is identical in both eyes, which is right — the shear is a
translation and parallax at infinity is zero. Run B: the sky is displaced by
~342 px plus the `A_x` stretch, varying with position exactly as the affine
does. **The sky carries the per-eye affine.**

**Why I got this wrong.** The point-set test that said otherwise searched `dx`
with **`dy` pinned at 0**, and the affine also displaces vertically. The correct
alignment was never evaluated, so a chance peak at `dx=+4` won. Two other
region-based tests were confounded by mixed content — the star detector picks up
distant ships and station lights, and any crop containing cockpit edges is
dominated by them. **Image forensics was the wrong instrument for a question the
layer can answer directly**, and I should have reached for the layer's own
classification instead of measuring pixels three times.

**What Patola actually saw is correct behaviour.** In run B the two eye images
*should* look very different side by side, because each is drawn for a
differently canted frustum. The bright star at a given screen position in the
right eye is **not the same star** as the one at that position in the left; the
whole sky has moved ~370 px between them, so its background differs. Judging the
flat SBS mirror by comparing halves is misleading for a canted render — that is
precisely the work the compositor undoes with the declared frusta.

### The list is TWO items, not three

`mod-0266`/`mod-0267` bake a cubemap and are irrelevant; whatever draws the sky
to the screen goes through a World module and is already handled. **The compute
skybox is off the list, and it should never have been on it** — I put it there
from a doc line I had not verified, and then measured badly enough to confirm my
own error.

What remains, both real and both seen:

1. **Screen-space UI.** ~52 modules classify NonWorld in take 165b and are given
   `K_nonworld` = identity, so they stay in X4's symmetric frame while the
   declaration says canted: 30.07° of divergence. This is #30's canvas problem,
   and #30's machinery is the right shape because it chooses per *pass* — the
   fullscreen post passes must keep their identity while the UI pass does not.
2. **The deferred reconstruction.** `patch_fragment_invproj_eye` composes
   `T(d)·M_invprojection` for the shear; the affine is a further transform on
   `gl_Position`, so `gl_FragCoord` no longer corresponds to the NDC that matrix
   expects. Undoing it is one more constant composition — `A⁻¹` on the NDC before
   the inverse projection — and `A` is latched, so it is known at patch time.
   The menu's per-eye shadows are this.

## #35 — STATE OF PLAY (read this first)

The sections above are the working trail, including two retractions. This is the
settled position as of take 165b.

### Done and proven

* **Piece 1 — the off-axis affine is emitted** by both live-sx vertex patches,
  per view, `sx`/`sy` read live. Takes 164a/b/c: the identity control is exact,
  the canted target lands on all eight predicted coefficients, `B_x` reverses
  sign between views. Tagged `stage15-offaxis-emission`.
* **Piece 2 — one latched frustum pair** feeds both the shader coefficients and
  `XrCompositionLayerProjectionView::fov`. Take 165b: the runtime reports take
  112's frusta to the centidegree, the latch takes them, `baked-sx=0`.
* **The world, the cockpit and the SKY all carry the affine correctly.** The sky
  was measured wrong twice before this was established; see the retraction
  above.

### Not done — exactly two things, both understood

1. ~~**Screen-space UI is left in X4's symmetric frame.**~~ **DONE — task #40.**
   See below.
2. ~~**The deferred reconstruction ignores the affine.**~~ **DONE — task #39.**
   See below.

**Both are done. The next thing #35 needs is a VR take (#41), specified below
with its predictions — written before the run, as this project requires.**

### #41 — the take, and what it should show

Two runs, control first, the same structure that localised 165a/165b in one
sitting. Both lines are complete; neither is "the last one plus X".

**A — control, affine off. Reproduces take 163 and proves #39/#40 changed
nothing when the map is not asked for.**

    X4VR_AA=fsr_quality X4VR_TAKE=166a-CTRL X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_H=4224 X4VR_W=8448 X4VR_DUMP_MATRICES=1 X4VR_RES=4224x4224 X4VR_VR=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_PROJ_INVPROJ=1 X4VR_SBS=1 X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take166a.log X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_OFFAXIS=off X4VR_BINDLESS_MIRROR=1 ./launch/x4vr-launch.sh

**B — the test, affine on from the runtime's frusta, canvas at infinity.**

    X4VR_AA=fsr_quality X4VR_TAKE=166b-OAUI X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_H=4224 X4VR_W=8448 X4VR_DUMP_MATRICES=1 X4VR_RES=4224x4224 X4VR_VR=1 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_PROJ_INVPROJ=1 X4VR_SBS=1 X4VR_MV_INVENTORY=1 X4VR_LOG=/tmp/x4vr-take166b.log X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_OFFAXIS=runtime X4VR_BINDLESS_MIRROR=1 ./launch/x4vr-launch.sh

Diffed against take 165b's own `env: run =` line, the changes are: the take
name and log path; `X4VR_OFFAXIS` stated explicitly (it was the default in
165b and is opt-in since); `X4VR_PROJ_INVPROJ=1` stated rather than left to the
default, because this project has already been bitten by "omitted" and "=0"
ceasing to mean the same thing; and `X4VR_MV_INVENTORY=1`, which is a one-shot
line per render pass at creation and is the only thing that would say *which*
passes classify as a canvas if none of them take one.

No `X4VR_CANVAS_M`. **Infinity is the state that ships** when the affine is on
and no distance is named, so it is the one that has to be right. If the UI
turns out to be fusable but uncomfortably far, that is a comfort question for
a follow-up with `X4VR_CANVAS_M=2` — stage4's proven value — and not a defect
in this take.

#### Predicted, before the run

Run B's log should carry:

    offaxis: target from the runtime's located views — eye0 l=-54.00 r=40.00 u=44.00 d=-55.00, eye1 l=-40.00 r=54.00 …
    canvas: at infinity, CANTED — screen half-angle 55.00 deg (sx=0.70021) -> A_x=1.2892 A_y=1.1931, eye0 x offset +0.24251 eye1 -0.24251 NDC …
    canvas final: ~350 variant(s) built, 0 REFUSED, swapped into >0 pipeline stage(s)
    invproj final: per-eye M_invprojection — ~226 modules corrected
    invproj final: off-axis affine UNDONE in the deferred reconstruction

The swap count will be **small — takes 98–100 saw 18** — because only a handful
of pipelines are built against a UI pass. Small is expected; **zero is the
failure**, and it is the one that looks like success.

Run A's log should carry `offaxis: OFF by request`, no `canvas:` line, and the
same `invproj final: … ~226 modules corrected` **without** the affine line.

#### What the SBS screenshot will look like, and why it is not alarming

**The HUD will sit noticeably right-of-centre in the left half and left-of-centre
in the right half — about 0.2425 NDC each way, ~512 px on a 4224-wide half.**
That is `B_x`, and it is the correction, not a defect. Straight ahead really is
+15.04° off image centre in one eye and −15.04° in the other; putting the HUD
at the same *pixel* in both halves is precisely what made 165b unfusable.

**The HUD will also look bigger and slightly stretched** — magnified 1.289×
horizontally and 1.193× vertically. The 8% anisotropy is correct: the target
frustum spans 94° horizontally and 99° vertically, so equal angles are not
equal NDC. The headset undoes exactly this.

Both halves are therefore *supposed* to differ, side by side, and the flat
screenshot is the wrong instrument for judging it. The question the headset
answers is only: **does it fuse into one image, in one place?**

#### What to do in the run

This is an **interaction** take, not a measurement one — the questions are
about content that only appears when it is used.

1. Let it reach the main menu. Look at the menu's 3D animation: the shadows
   should now match between the two halves (that is #39; in 165b they were
   completely different).
2. Load a save. The loading screen should be fusable, and its eyes not swapped.
3. In the cockpit, look at the **HUD panel, the radar, and the weapon gauges**
   — the three things that moved wrongly in 165b. Each should fuse into a
   single object at a single place.
4. Move the mouse over a menu button. **The pointer must land on the button it
   activates.** If it sits a constant distance away, the canvas moved and the
   cursor did not.
5. Open the map (M). It is drawn by a different camera (|sy/sx| = 1.5) and is
   the case most likely to expose a wrong screen field.
6. Park in the world for ~60 s after the save has loaded, so the perf window
   has something to measure — #39 added arithmetic to 244 fragment modules and
   this is the first run that can price it.
7. Exit cleanly.

#### Acceptance

    python3 tools/score_run.py /tmp/x4vr-take166a.log
    python3 tools/score_run.py /tmp/x4vr-take166b.log

Both must come back with no `FAIL`. Run B specifically must **not** produce
`the declared frusta are canted … and screen-locked content was NOT put on the
canvas`, which is the check that would have failed 165b before the headset went
on, and must not produce the half-applied `#39` or wrong-field `A_x` failures.
Patola's eye is the authority on *characterisation* — "does it fuse", "does the
pointer land" — and the scorer is the authority on whether the run happened.

### #39 — the deferred reconstruction, done

`patch_fragment_invproj_eye` now composes `T(d)·M_invprojection·A⁻¹`: the affine
undone first because it was applied last, the shear undone last. 244 fragment
modules, plus the 2 that take `M_invprojection_uj` — the shadow cascades, which
are the ones the menu's per-eye shadows came from.

`A`'s linear part is `ax_num/sx` and `sx` is per-draw, so the scales are read
back out of **the matrix being corrected**: X4's projection has a diagonal
top-left 2×2 in every dumped take, so `minv[0][0] = 1/sx` and `minv[1][1] =
1/sy`. That spares a second access chain and makes it impossible for the scale
used to disagree with the matrix it is applied to. Everything in the chain is
uniform over the draw, so it hoists into scalar registers.

**Scope, established by reading the shaders rather than reasoning about "screen
space".** Ten fragment and two compute modules also read `M_projection`, and the
first instinct — compose `A` onto that too — is **wrong**:

* Eight of them (`mod-0167`, `0168`, `0365`–`0370`) unproject with member 2,
  reproject with member 1, and sample a `sampler3D`: a **froxel volume**. That
  volume is filled by compute (`mod-0177`, `mod-0364`), which has no
  `gl_ViewIndex` and is therefore parametrised by X4's *symmetric* frustum for
  ever. The lookup must use the symmetric projection with a **correct** view
  position — which is exactly what #39 now produces. Composing the affine onto
  member 1 as well would send every froxel sample off by the full map.
* The other two (`mod-0111`, `mod-0112`) read `M_projection[1][1]` as a scalar,
  clamp `|sy|` between two configured half-FOV cotangents and smoothstep it —
  a zoom-keyed intensity fade, not a transform. The whole-matrix load the patch
  keys on does not match a deep access chain, so they are excluded by shape.
  (The affine does change the effective vertical FOV by `A_y ≈ 1.19`, so the
  fade point shifts slightly. Cosmetic; recorded, not fixed.)
* The two compute modules stay wholly uncorrected on both members, so they
  remain internally consistent. The compute gap is unchanged, not widened.

**How it was verified, with no run.**

| check | result |
|---|---|
| host round trip, 2 eyes × 5 cameras × 6 points | worst view-space error **1.5e-5 m** |
| ...against its negative control (affine ignored) | **104 m** |
| symmetric target at X4's own half-angle | `M_invprojection` unchanged, exactly |
| all 409 dumped modules, affine unrequested | **byte-identical** to the pre-#39 patcher |
| all 244 patched modules, affine requested | `spirv-val` clean |
| emitted arithmetic vs `common/x4vr_view.hpp` | **0.000e+00** over 10 configurations |

That last row is `spirv_patch frag-invproj-check`, which interprets the emitted
graph instead of trusting that a valid module is a correct one. It was proved to
have range before being believed: extracting `minv[1][0]` instead of `minv[1][1]`
scores 1.163, and swapping the operands of the col-3 subtraction scores 2.000.
Both of those validate perfectly under `spirv-val`, which is the whole point.

### #40 — screen-locked content, in the declared frame

**The framing in the section above was wrong, and measuring it is what fixed
it.** "~52 NonWorld modules, and that set contains every fullscreen post pass"
made the problem look like one with no clean predicate. It has one. Splitting
X4's 394 vertex modules by *how they produce a position*:

| | attributes | procedural (`gl_VertexIndex`, no attributes) |
|---|---|---|
| **World** | 348 | **0** |
| **NonWorld** | **5** | 41 |

The 41 procedural ones are the fullscreen post passes — they must keep drawing
exactly where they are, and any transform on them leaves unrendered borders.
The other 353 are draws that were *placed* by data and can therefore be
re-placed. So the canvas's exclusion is **`is_procedural_fullscreen`**, not
`World`: #30 keyed on World and got the right answer only because every World
module has attributes. The two readings differ on exactly five modules —
`mod-0000`, `0228`, `0229`, `0397`, `0398` — NonWorld UI that #30 could not
move, and 0 modules go the other way, so nothing that used to get a canvas
loses one.

**The transform is #30's, evaluated in the canted frame.** A draw at screen NDC
`x` is the ray `tanθ = x·tan(half)`; put it `z` metres away and view it from an
eye offset `d`, and where it belongs is what `A` already defines:

    x' = A_x·x + B_x − ax_num·d/z        A_x = ax_num / sx_screen
    y' = A_y·y + B_y

Every term is constant — `sx_screen` is a property of the SCREEN, not of a
draw's camera — so this needed **no new shader machinery at all**.
`patch_vertex_clip` has been applying constant 4×4s to UI draws since #30; only
the matrix changed.

`sx_screen` comes from `X4VR_FOV` through the new `x4_half_for_fov`, never from
a camera block, for the reason `vr_declared_fov()` already gives at length: the
layer sees several cameras per frame with sx from 0.75 to 3.78 and picking one
is the mistake fifty takes were built on. Both the declaration and the canvas
now go through that one function so they cannot disagree about what field the
eye image spans. (`assumed_proj_sx()`, which #30 used, defaults to 0.889 — the
2816×1408 measurement — and would have placed the UI against a field 27% off
the union render's. The scorer now fails on that specific disagreement.)

**`z ≤ 0` means infinity**, which is the pre-#30 behaviour and the right
default when the affine is on and no distance was chosen: zero disparity is
fusable, and leaving the map off is not. `X4VR_CANVAS_M` still chooses a
finite distance.

**The cursor is a coupled consumer and was handled with it.** X4 hit-tests the
UI CPU-side at an unshifted window coordinate, so a pointer that does not take
the same map lands away from every button it activates — both halves behaving
as designed and the result visibly wrong (`docs/known-good-runs.md`, stage4
says this in as many words). #30 published a single float because its map was a
translation; this one is not, so `CanvasNdc` is published whole. The map is
applied to the **hotspot** and the quad translated by the difference: running
it over the quad's corners would stretch a 1:1 bitmap by 1.29×/1.19× into a
blurred pointer, while the hotspot — the only part that has to be anywhere in
particular — lands identically either way.

**How it was verified, with no run.**

| check | result |
|---|---|
| no cant: `make_canvas_k` vs #30's `canvas_shift`, 3 distances, both eyes | **exactly 0.00e+00** |
| canted at infinity: do both eyes place the UI at the same angle? | 6.8e-6° |
| ...and at the angle X4 drew it at? | 6.8e-6° |
| the negative control, untransformed | **30.07°**, the number from take 165b |
| canted at 0.5/2/10 m: is each eye's ray the real one? | 6.8e-6° |
| `is_procedural_fullscreen` over all 409 dumps | 41/5/348/0, as tabled |
| cursor overlay on a real GPU, canted and scale-only | passes, and **fails** when the overlay is seeded to drop the scale |

The GPU cases are posed at a hotspot off centre in both axes on purpose: a
scale applied about the centre moves nothing at the centre, so a case posed
there would pass with the scale silently dropped.

**A latent flake found on the way.** `tests/run-multiview-render.sh` asserted the
emitted coefficients with `spirv-dis … | grep -q`. `grep -q` exits at the first
match, `spirv-dis` takes SIGPIPE, and under `set -o pipefail` the pipeline then
reports failure *even though the constant was found* — so the verdict depended on
whether the module was small enough for `spirv-dis` to have finished writing.
Piece 1's assertion passed for that reason alone; the #39 one, on a larger
module, failed all five. Both now disassemble once into a variable.

### Current default, deliberately

`X4VR_OFFAXIS` unset = affine OFF = take 163's behaviour. The runtime source is
opt-in (`X4VR_OFFAXIS=runtime`); explicit angles (`"l,r,u,d"`) reproduce
164a/b/c; `off`/`0` force it off. This is not tidiness — piece 2 briefly made the
affine default-on in VR, which ships the 30.07° divergence.

### Acceptance

`tools/score_run.py` now FAILs a run whose declared frusta are canted while the
affine reaches World modules only — it reads the latched frusta from the log,
computes both frustum centres and fails on their separation. That check would
have failed 165b before the headset went on. `tools/register_affine.py --verify`
measures the affine from screenshots; run `--self-check` first.

Since #40 the divergence FAIL is conditional on the canvas's own swap count,
and it needs **positive** evidence: a build predating #40 prints no canvas
line, and "no line" reads as "not covered" rather than as "fine". Both branches
were exercised against a synthesised log before either was believed — covered
passes, `swapped into 0` fails twice over, and a canvas placed against the
wrong field fails on the A_x identity.

It also FAILs the **half-applied** state #39 introduces the possibility of: the
affine on `gl_Position` with `M_invprojection` left alone. That is reachable
through `X4VR_PROJ_INVPROJ=0` and through the bindless mirror being off, and it
is strictly worse than either end state, so the layer names it in one line —
`invproj …: off-axis affine NOT undone` — and the scorer fails on that string.
The latch itself cannot split the difference: one static object serves both call
sites, so a session is affine everywhere or affine nowhere.

### Method note for whoever picks this up

Three of my screenshot measurements of "does X carry the affine" disagreed with
each other, and I published the wrong one. For "which module drew this", add a
layer-side instrument that logs its own classification against what it binds —
do not do image forensics. See docs above and the retraction for the details.

### Where this stands after takes 166–171 (READ THIS BEFORE PLANNING ANYTHING)

**#39 and #40 are proven in the headset, not just offline.** Take 169b, with
`X4VR_OFFAXIS="-54,40,44,-55"`: the cockpit, the radar and the speed meter all
fuse. Those are exactly what take 165b broke. World geometry through the affine
and screen-locked UI through the canvas both land correctly.

**The cant itself is confirmed working, from inside.** Patola, closing one eye
at a time in 169b, described the left eye extending much further left and
stopping nearer the middle on the right, and the right eye doing the mirror.
That *is* the canted frustum pair (−54/+40 and −40/+54), with binocular overlap
across the central ±40° where he reports correct depth. It is not a defect and
must not be filed as one. It also explains the weapon brackets appearing in one
eye only: they are anchored out in the monocular wing, which is #45's problem,
not a stereo one.

**Four takes were lost to one race, and it is not winnable.** `X4VR_OFFAXIS=runtime`
must latch at the first shader module (coefficients are baked at
`vkCreateShaderModule`), X4 compiles that module ~1 ms after creating its
device, and the runtime needs ~8 ms from a standing start. 166b latched before
the session existed; 167b's bounded wait blocked the call that spawns the
session thread; 168b started the thread from the latch and it still executed
nothing for 5 s, because `vr_session_thread` re-enters the Vulkan loader while
X4's thread is parked inside `vkCreateShaderModule`. `VrState` already warned
that re-entering the loader from inside its own chain calls is a hazard; I
quoted that warning in the commit that added the wait. **Use explicit angles.**
Task #47 (cache the frusta between runs) is the real fix.

**What is eliminated for the backdrop**, so nobody re-tests it: it is not the
eye shear (169c at IPD 0.016 showed the same disconnect at a quarter of the
disparity), not a module missing the affine at the aggregate level
(`baked-sx=0`), and not wrong coefficients (they are `view_math`'s, logged).
Take 170 also showed the unsheared `original` twin going only to rp #40/42/44/
46/48 — five passes, the five shadow cascades, by design.

**Take 170's instrument lied and its `path`/`affine` fields must not be read.**
A missing pair of braces stamped `clip` over every module's real path; 1007
lines agreed with each other and contradicted the counter four lines above.
Fixed, and `score_run.py` now FAILs any run whose pipe inventory omits a path
the patch-site counters say was used. Its other fields are sound, and they
established that #40 reaches real draws (16 World + 8 NonWorld stages took the
CANVAS variant) and that 48 procedural stages correctly did not.

### Take 171 — exactly what to extract

Same line as 170 with the name and log bumped. Measurement only; nothing to
judge by eye. **Check `score_run.py` first** — if it reports the pipe inventory
disagreeing with the patch-site counters, the instrument is lying again and
nothing below is readable.

Then, in order:

1. `python3 tools/module_map.py /tmp/x4vr-take171.log` — per pass, which dumped
   `mod-NNNN` drew it, how it classified, which path took it, whether it
   carries the affine, and which variant the pipeline got.
2. **Find the backdrop's pass.** Cross-reference `docs/frame-analysis.md`'s
   frame inventory. Remember Patola's domain fact: X4's backdrop is **not one
   layer** — its planets are reachable geometry ~2 h of flight away, so expect
   several passes with different depth behaviour, and an argument that assumes
   infinity holds only for the part actually at infinity. It also broke all
   three previous attempts at this mod.
3. **The two floating disks in the cockpit are the sharper probe.** They sit
   inside geometry that fuses, so anything global is already excluded; the
   difference is entirely which draw path they take. Look for a pass whose
   modules differ from the cockpit's in path, affine or variant.
4. The question to answer is one sentence: *which passes draw content that
   reaches the screen without the affine, while the declaration is canted?*

### Take 171 answered it — task #48, and two documented claims were wrong

The sentence in step 4 has an answer: **the modules X4 positions with the
camera instead of a per-object matrix.** Eighteen of X4's 409, and the two that
matter here are `p1_star` — the suns and the bright named stars — and the
instanced deferred light volumes.

`classify()` calls a module World only if its vertex stage reads member 0 of the
set-3 block. These declare no set 3 at all: `p1_star`'s vertex shader ends

    gl_Position = M_projection * M_view * vec4(quadPos, 1.0);

so it is positioned straight off the camera. `wide_camera` exists precisely for
them (task #22 / P70) and is **off by default**, so they classify NonWorld, take
`K_nonworld` — identity — and get `KR == nullptr`, which the layer's own comment
says "is what keeps the module mono". They therefore draw **mono and in X4's
uncanted frustum inside `rp #23`, a STEREO pass**, while the 152 world modules
around them are canted by ±15.04°. That is the 30.07° divergence from
`common/x4vr_view.hpp:291` — the negative control — applied to a few objects
instead of the whole frame. Patola: *"the cockpit feels ok except for part of
the sky (stars / nebulas)"*.

**Two things this project had written down were false, and both were load-bearing.**

1. **"The skybox is a compute shader, so the affine cannot reach it by any
   mechanism this project currently has"** — `docs/frame-analysis.md:3490` and
   the P12 retraction above. `mod-0267` is not the skybox. Reading it end to end
   shows six cube-face direction bases, a 2048-sample Hammersley loop and an
   `ImageWrite` into a cube RT: it is **environment-map prefiltering for
   reflections**, it never reads the camera projection, and it is eye-independent
   and correct as it stands. The identification came from an instrument that
   searched for `samplerCube` and found one in a compute module; the leap to
   "that is the skybox" was never checked. **X4's procedural starfield is
   `mod-0111/0112` (`p1_starfield`), it is `World,live-sx`, and it carries the
   affine today.** The affine reaches the sky fine. What it does not reach is
   `p1_star`.
2. **"Every one of those is the current rule working exactly as written"** —
   take 165b's per-feature measurement found *"some stars moved by large amounts
   while other stars did not"* and concluded it was not a misclassification. It
   is exactly a misclassification, of exactly 18 modules. The starfield stars
   move because they are World; the `p1_star` suns do not because they are not.
   The measurement was right and the reading of it was wrong.

Both errors have the same shape: **an identification made by an instrument that
could not tell the two cases apart, then quoted downstream as established.**

X4 ships its GLSL. `shadergl/glsl/**` and `shadergl/ogl/*.xml` extract straight
out of the `.cat`/`.dat` archives in plain text — 587 files — and joining a
shader's rarest property name against the dumps names a module outright
(`layer1_boostmin` → `mod-0111/0112`, `base_hue_shift` → `mod-0226/0227`).
Four months of reverse-engineering SPIR-V had a source tree sitting next to it.
Egosoft's copyright: extract to `/tmp`, never commit, same standing as the dumps.

#### The instrument was lying again, and differently

Take 170's instrument lied through a missing pair of braces. Take 171's lied
through a **different predicate**:

    const auto k = x4vr::spv::classify(orig_words, true);   // instrument
    const x4vr::spv::Kind kind = classify(code, wide_camera); // dispatch

The inventory printed the *wide* reading beside `path`, where it reads as "this
is how the patch saw it". So all 18 logged `World` after being handled as UI.
The braces bug made every line agree with every other; this one made every line
agree with *reality as it could have been*, which is harder to spot because the
number is not suspicious. The struct comment said `(wide reading)` and was
right — the field was read at its point of use, not at its declaration.

The summary line was worse, because it stated the false claim in words:

    baked-sx=0] +offaxis (live-sx and mvp-sx only; baked-sx keeps X4's frustum)

`baked-sx` is **0 in every run** — when the mvp patch declines, `vert_patched`
stays false and the module falls into the clip branch, which overwrites the path.
`clip` is a second door to the same wrong frustum, it was never on that line, and
it is the door that is actually open. A guard whose counter cannot rise is not a
guard. Fixed: the line now carries `clip=` and `cam-pos=`, the per-module entry
carries `,camera-positioned` and `,MONO`, `kind` is the dispatch's own value, and
`score_run.py` FAILs on a camera-positioned module drawing uncanted in a stereo
pass — **tested by rewriting take 171's log into what the fixed layer would have
emitted, and confirming it FAILs there and stays silent on the original.**

#### Committed BEFORE take 172

`X4VR_SHEAR_LIGHTS=1` is an existing knob, default off since task #22. All 18
modules were run through `patch_vertex_eye_offset` offline: **18/18 accept it and
produce SPIR-V that passes `spirv-val`**, so with the knob on they move to
`live-sx` and gain the shear and the affine, reading `sx` live rather than baked.

- **P82** — with `X4VR_SHEAR_LIGHTS=1`, `score_run.py` reports `cam-pos=18`
  with no camera-positioned module drawing uncanted in a stereo pass, and
  `live-sx` rises by ~18 while `clip` falls by the same.
- **P83** — the suns and bright stars fuse with the rest of the world. This is
  the eye check, and it is the *only* one Patola is asked for.
- **P84** — dark regions get *less* wrong, not more: this is the same set of
  light volumes as take 60's "additive lift in the dark", which was never fixed.
- **P85 (the refutation to watch)** — shadows are unaffected, because the
  pass-level MONO gate substitutes the unsheared twin for depth-only passes.
  Shadows broke Patola's *previous* attempt at this mod and are a first-class
  hazard; if they move, P85 is refuted and the knob goes straight back off.

If P83 fails while P82 holds, `p1_star` is not what Patola is seeing and the sky
question reopens with the starfield already excluded.

### Take 172 — P82–P85 all held, and #48 is done

`X4VR_SHEAR_LIGHTS=1`, everything else identical to 171. From the log:
`live-sx` 284 → **296**, `clip` 22 → **1**, `cam-pos=0`, and the new check prints
*"no camera-positioned module draws uncanted in a stereo pass"*. Patola, on the
headset: *"lighting perfect, stars and sun and planets — the entire backdrop now
fuses almost perfectly"*, and **the shadows did not move** (P85, the one that
would have sent the knob back off). P84 held too: the light volumes are the same
set as take 60's unexplained additive lift in the dark, and the lighting is now
described as perfect rather than merely unchanged.

**P82 was mis-phrased and is recorded wrong rather than corrected away.** It
predicted `cam-pos=18`. `cam-pos` counts the *disagreement* between the narrow
and wide readings, so turning the knob on drives it to 0 **by construction** — a
prediction about an instrument whose definition I had written an hour earlier.
The substantive half (clip collapses, live-sx rises, nothing uncanted in a stereo
pass) is what carried the claim, and it held.

`X4VR_SHEAR_LIGHTS=1` is now part of the known-good state. **A state is code AND
knobs** — any run from here that omits it is not comparable to 172.

### The Sun is brighter in the right eye — task #49

The one exception Patola reports: *"the only exception is the Sun; the right eye
is consistently brighter than the left eye. Not much, but very perceivable, and
changing angles and directions doesn't change this."*

Angle-independent and eye-fixed, which rules out anything positional. What the
shader source allows:

    OUT_RT0.rgb = IO_lightcolor.rgb * texture(S_diffuse_map, newUV, U_mipbias).rgb
    OUT_RT0.rgb = mul(vec4(OUT_RT0.rgb,1), make_ColorMatrix(...)).rgb

`star.frag.glsl` has **no view-dependent term at all**. `IO_lightcolor` comes
from instance data, the colour matrix is uniforms, and `newUV` is a varying into
an 8-tile atlas. The only input that can differ between eyes is the **texture
fetch itself** — and `mod-0227` samples `SRGB_sampler2D` at **set 0, binding 7**,
one of exactly the two bindless tables the per-eye index offset rewrites
(`element = index + gl_ViewIndex * OFFSET`, 657 modules APPLIED in this run).

That makes the bindless redirect the leading candidate, but **it is not yet
evidence, and the mechanism does not obviously produce "brighter"**: the mirror
installs *"a view of layer 1 where the image is doubled and the identical
descriptor otherwise"*, so a static atlas should mirror to an identical
descriptor and a doubled image's unwritten layer 1 would read black, not bright.
Two ways to get brightness out of it that have NOT been checked — an sRGB decode
skipped on the twin view (`SRGB_sampler2D` is the name, and sRGB→linear on a
near-white texel is ~14%, which matches "not much but very perceivable"), and
the 159 460 284 slots the mirror reports as *undecided*.

**Why it only shows now:** before take 172, `p1_star` drew MONO — the same
pixels in both eyes — so a per-eye difference in its texture fetch could not
appear. The fragment patch was always applied to it. This is very likely an
old defect newly *visible*, not a new one, which also means it is not a
regression from #48.

Two prior wrong identifications this session (`mod-0267` as the skybox,
`p1_star` as a light volume from a 4-of-6 property match) both came from
believing a plausible mechanism before eliminating it. So: **eliminate, do not
instrument.**

- **P86** — with `X4VR_BINDLESS_PATCH=0` the Sun reads the same brightness in
  both eyes. Confirms the per-eye texture redirect is the cause and nothing
  else in the pipeline is; the follow-up is then *which descriptor*, not
  *whether*.
- **P87 (the one that would hurt)** — if the Sun stays brighter in the right eye
  with the redirect off, the bindless path is excluded outright and the cause is
  downstream of the draw: the sRGB resolve, the tonemap mask, or FSR. That
  reopens the per-eye brightness question last measured at take 60, where the
  sky matched to **0.2%** and only shading surfaces differed.

Expect the rest of the frame to regress in that run — `X4VR_BINDLESS_PATCH=0`
puts the HUD composite back to sampling view 0 in both eyes (take 58). That is
the knob doing its job and is not the thing being judged. **The only question is
the Sun.**

### Take 173 is VOID as evidence — one knob silently switched off two features

The run came back *"very difficult to see, because the separation was so big that
nothing fused"*, with the Sun looking equal-brightness and a new **bright orange
halo about 20° from the Sun**. `score_run.py` FAILed it, and not for the thing
being tested:

    invproj final: off-axis affine NOT undone in the deferred reconstruction —
    HALF APPLIED: gl_Position carries the affine and M_invprojection does not

`X4VR_PROJ_INVPROJ=1` was in the command line — the run's own `env` line confirms
it, and confirms `X4VR_BINDLESS_PATCH` is the **only** variable that differs from
172. The layer corrected **236 modules in take 172 and 0 in take 173.**

**The cause is a shared gate.** `#22`/`#39`'s invproj block sat *inside*
`if (g_bindless_patch && g_bindless_mirror)` (lines 4241–4343), so turning the
bindless patch off silently turned the deferred-reconstruction affine undo off
with it. Two unrelated features, one gate, no log line saying so. The knob
documentation says `X4VR_BINDLESS_PATCH` "rewrites every fragment module that
samples a bindless table" and says nothing about `M_invprojection`, because
nobody intended this.

So take 173 removed **two** things, and the second is the documented
half-applied-affine failure. That failure *is* "the separation was so big that
nothing fused", and it is very likely also the orange halo — deferred passes
reconstructing a frame the affine already moved will place a glare term away
from the object that caused it. **P86 is neither confirmed nor refuted. #49 is
exactly where it was**, and the take bought a different bug instead.

This is the same lesson as the take-100/104/106/107 zoom asymmetry: **an A/B's
arms must differ in exactly one thing, and the asymmetry to check for first is
the one you introduced yourself.** Here it was introduced by the *layer*, not the
protocol, which is why reading the command line could not have caught it and the
scorer could.

Fixed by hoisting the invproj block out of the bindless gate, preserving the
transform order exactly (index-offset → invproj → survey) so the bindless-on path
is unchanged down to the coverage counters. Locked with a case in
`tests/run-offaxis-bringup.sh` that drives a module which actually takes the
patch — `fullscreen.vert` has no camera block and would pass with the coupling
still in place — and **the case was shown to FAIL with the coupling restored and
pass with the fix**, which is the standard that file sets for itself.

`X4VR_TEST_EARLY_SHADER` now optionally names a module, so any of X4's dumps can
be pushed through `vkCreateShaderModule` without pairing it with a compatible
fragment stage.

#### Take 174 — P86/P87 again, on a layer where the knob means what it says

Same line as 173, unchanged, on the rebuilt layer. It is only now a
one-variable control against 172.

- **P88** — the log reads `invproj final: ... UNDONE` and ~236 modules corrected
  *with* `X4VR_BINDLESS_PATCH=0`. If this fails the fix is wrong and nothing
  about the Sun is readable. Check it before reading anything else.
- **P89** — the frame fuses about as well as 172 did, because the only thing
  still removed is the per-eye texture redirect. If the separation is still
  large, something *else* rides on that knob too.
- The orange halo is expected to be **gone**. If it survives into 174 it is a
  real defect of its own and gets its own task rather than being folded into #49.

### Take 174: the fix works, P89 was my error, and the eye has run out of range

`invproj final: off-axis affine UNDONE`, **228 modules corrected with
`X4VR_BINDLESS_PATCH=0`** — P88 held, the hoist works in the game, and 174
PASSes with `live-sx=296 mvp-sx=12`, matching 172 exactly. It is finally a
one-variable control.

**P89 was wrong, and wrong from something already established.** I predicted the
frame would fuse about as well as 172. It cannot: the redirect is precisely what
makes the right eye's composite sample view 1, and take 58 already recorded that
this knob is load-bearing. Not a new defect — a prediction I had no business
making.

The orange halo did NOT return, which supports it having been the half-applied
affine placing a glare term away from the object that caused it.

On the Sun, Patola: *"the left and right eye sun's brightness seemed to match. I
say 'seemed' because without the fusion it gets much more difficult to compare"*
— using a nearby star as a reference. That is the **second** "seemed" in a row.

**The eye is out of range, and saying so is the point.** It is the right and
only instrument for "does this fuse", "is this the wrong way round", "is the
panel on the wrong side" — questions this project has repeatedly got right from
it. A ~10% luminance ratio on a small disc in an unfused image is not one of
those questions. Asking a third time would be spending a take on an instrument
already known to be at its limit.

#### Take 175 — measured, not judged

`X4VR_MV_DUMP_PRESENT=N` writes the finished eye image, both layers, every N
presents. Two arms, one variable, and **no eye judgement at all**:

    175a  the known-good config  (X4VR_BINDLESS_PATCH=1)
    175b  the redirect removed   (X4VR_BINDLESS_PATCH=0)

`tools/bright_object.py` finds the bright object **independently in each layer**
and compares it. A fixed pixel box cannot be used: the Sun is at a different
screen position in each eye by construction — that is the parallax plus a 30.07°
cant — so a box would measure the disparity, not the brightness. Luminance is
computed in **linear light**; a ratio taken on 8-bit sRGB code values understates
by roughly 2× near white.

Validated before use, three ways, against data already on disk:
byte-identical layers (t65) give exactly 1.0000; genuinely differing layers (t69)
are detected, with the −47 px parallax reported and the object ratio (0.9711)
separated from the whole-frame control (**1.1532**); and the numpy and pure-Python
implementations agree to every digit printed.

**The whole-frame control is not decoration.** Take 60's table is the precedent:
the background matched to 0.2% while lit surfaces ran 1.7–1.9, and that contrast
is what made "not exposure, not tonemapping" a conclusion rather than a guess. If
175a's frame ratio moves as much as its Sun ratio, the Sun is not the finding.

- **P90** — 175a shows a Sun integrated ratio clearly away from 1.0 in the
  direction Patola reports (layer 1 brighter), with a whole-frame control
  nearer 1.0. This is the first *number* for #49.
- **P91** — 175b's Sun ratio is nearer 1.0 than 175a's. That is the causal
  claim, and it replaces two "seemed" readings with a measurement.
- **P92 (the one that would reset #49)** — if 175a's Sun ratio is already ~1.0,
  the defect is not in the presented image at all, and what Patola sees is
  produced after it: the compositor, the runtime's own submission, or the
  headset's per-panel response. The task then changes completely.

### Take 175 measured it, and the answer is not "brighter"

**The cant is confirmed from pixels, for the first time.** Predicted eye-to-eye
offset for a backdrop object: `B_x = 0.24251 NDC x 4224/2 x 2 = 1024.36 px`.
Measured, every frame in 175a: **-1024.0, -1024.4, -1024.5, -1024.7, -1022.0**.
The affine does exactly what the arithmetic said it would.

**P91 is unanswerable, and 175b was vacuous BY CONSTRUCTION.** Every frame of
175b reports the Sun at ratio exactly 1.0000 with dx exactly 0.0: with the
redirect off, the right eye's *scene* is view 0's pixels, so the Sun in the
right eye **is** the Sun in the left eye. A per-eye difference cannot survive
that, whatever its cause. The arm could only ever return "symmetric".

That is three takes -- 173, 174 and 175b -- spent on an elimination that cannot
eliminate, and it was derivable in advance from take 58's own note that the knob
is load-bearing for exactly this path. **Before running an elimination arm, state
what the arm would show if the suspect were innocent**; if that is the same
picture it shows when the suspect is guilty, the arm is a
[[x4vr-guard-that-cannot-fail]] and the take is already spent.

**P90 held in existence and failed in shape.** There is a real, repeatable
per-eye difference at the Sun, and it is **not a brightness scale**:

    region              frame 9   frame 10   frame 11
    peak                 1.0000     1.0000     1.0000
    core   0-25 px        0.919      0.920      0.929
    halo  25-50 px        0.692      0.716      0.761   <- the whole effect
    100-200 px            0.888      0.953      0.957

Core and peak match; the **inner halo is 24-31% weaker in the right eye**. The
saturated-area difference that looked so large at first (3220 px vs 1549 px) is
a consequence of it, not a second finding: with a stronger halo, more of layer
0 clears the 80%-of-peak threshold. One defect, not two.

Looking at the crops directly: the left eye's Sun is a broad, soft, warm bloom;
the right eye's is a tight white core with sharp rays. **A tight white core reads
as brighter than a soft orange haze**, which is why Patola's "the right side's
Sun was visibly brighter" and a measurement showing the right eye's halo *weaker*
are the same observation. Neither is wrong.

#### Two readings I produced and then killed

Recorded because they were wrong, not despite it.

1. **"The right eye is ~15% dimmer overall."** The whole-frame ratio reads
   0.82-0.87 on the cockpit frames. It is comparing **different content**: the
   two eyes see different fields by construction -- that is the cant -- so their
   full-frame means are not comparable quantities. Killed by the absolute
   distribution: **median (R-L) = 0.00000**, 89% of pixels within 0.02, and
   symmetric tails (p99.9 +0.49, p0.1 -0.51). That is structure at slightly
   different positions, not gain.
2. **"There is a horizontal gradient, 0.735 at the left of frame to 1.564 at the
   right."** Every one of those bands is near-black (means 0.011-0.024), where a
   ratio of means is unstable and meaningless; the absolute differences are
   0.0006-0.0063. I then hypothesised a screen-centred vignette to explain it and
   **checked before believing it**: X4 has no vignette shader anywhere in its 587
   GLSL files. Hypothesis dead in one grep.

Both mistakes are the same one: **a ratio computed where the denominator is
noise.** Report the absolute level next to every ratio, or the ratio will invent
a defect.

#### The instrument's range, stated

`tools/bright_object.py` and the aligned comparison are only valid for content
**at infinity**, because 1024 px is the offset for infinite distance alone. The
cockpit and the HUD sit at finite distance with their own disparity, so
comparing them at that offset produces garbage -- ratios of 0.048 and 1.241 on
what are simply mismatched pixels. Only the Sun and the far starfield can be
compared this way, and the tool does not know that; the operator must.

#### Where #49 stands

Open, better specified, and no longer about brightness. X4's bloom is
**kinobloom** -- `kinobloom_prefilter`, `_downsample`, `_upsample`,
`_upsample_high`: a pyramid. A halo band that is 30% weaker in one eye while the
core matches to 1% is the signature of one level of that pyramid resolving
differently per eye. That is a claim about which *image* differs, and
`X4VR_MV_PROBE=1` answers it directly -- it hashes both layers of one per-eye
attachment per frame and reports DIFFER or IDENTICAL, in the run's own serials,
so no cross-run join is needed. Serials restart every run; the probe is the
instrument that does not care.

- **P93** -- some image in the bloom chain reads IDENTICAL where the passes
  around it read DIFFER. That names the level.
- **P94 (the refutation)** -- if every per-eye attachment reads DIFFER, the bloom
  is being computed per-eye correctly and the halo difference is produced
  somewhere that is not an attachment: the `p1_star` quad's own texture fetch,
  or its mip selection.

### Take 176 — P94, and an instrument that cost more than it measured

**P94 held: no image is wrongly mono.** Not from the probe, which only managed
15 samples in a run that barely ran, but from the layer's own **complete** writer
inventory, which does not depend on sampling at all:

    mv final: img #70..#74 writers — masked rp [] unmasked rp [39,41,43,45,47]

Those five are the shadow cascades, which are depth-only and view-independent by
design. **Every other colour image has masked, per-eye writers**, and
`per-eye images written layer-0-only=0`. Every probed image with real content
read DIFFER; the only IDENTICAL readings were on buffers that were all-zero or
uniform-`0x10`, i.e. cleared. So the kinobloom pyramid IS computed per eye, and
the Sun's halo difference is **not** a missing per-eye level. #49 moves to
`p1_star`'s own fetch or its mip selection.

#### The take was nearly unusable, and that is a defect in me

Patola: *"the game would freeze for 30 seconds then go for 10 frames where I
could still do some input. It was only by chance that I was able to focus on the
Sun. Even quitting the game was painful."*

Two causes, and the second is worse than the first:

1. The probe drains the queue and reads back both layers on the CPU **every
   frame**. It says so in its own first log line. That cost was known.
2. **`X4VR_MV_DUMP` was a path that also acted as a trigger.** It was set only to
   give the present dumps a per-run prefix — and that switched on an
   opportunistic dump of *every* probed image whose layers differ: ~20 extra
   4224x4224 pairs, gigabytes of uncompressed PPM written in bursts. Nothing in
   the command line says "dump twenty images"; the knob's own documentation
   describes it as a prefix.

**That is the same defect shape as the one fixed one commit earlier**, where
`X4VR_BINDLESS_PATCH` silently disabled `X4VR_PROJ_INVPROJ`. One knob doing two
jobs, the second undocumented, found the same day in the same file. Finding a
defect class and fixing one instance is not fixing the class —
[[x4vr-fixed-here-broken-next-door]] again, and this time between two commits
rather than within one.

Fixed:

* **`X4VR_MV_DUMP` is now a path and nothing else.** The opportunistic dump is
  `X4VR_MV_DUMP_AUTO=1`, opt-in.
* **Dumps are lossless PNG.** Patola asked for png or jpg; png is the half that
  keeps the data. These files are *photometry* — `tools/bright_object.py` takes
  luminance ratios off them — and jpeg's artefacts would land exactly on a
  saturated Sun where the ratio is measured. 5-10x smaller, so far less of the
  I/O burst. `X4VR_DUMP_PPM=1` restores the old format.
* Verified by writing the same dump twice, once per format, and asserting the
  decoded pixels are **identical**; `bright_object.py` reads both and reproduces
  the t69 baseline numbers unchanged.

**Rule for every future take: price the instrument before asking for the run.**
A measurement that makes the game unplayable does not just cost the take, it
biases it — Patola got the Sun in frame by luck, and a run where the operator
cannot aim is a run whose content is not the content that was asked for.

### #49 localised: the volumetric in-scatter, which is mono by construction

Measured with an **identity check** on the object, which the first pass lacked:

    frames 2-7   menu / loading / 3D scene   SAME OBJECT   core 0.985-1.016  halo 0.985-1.001
    frames 9-11  cockpit                     SAME OBJECT   core 0.919-0.929  halo 0.692-0.761

The first attempt reported "consistent" halo ratios across frames 9-11 without
checking that the blob found in each eye was the *same* blob. It was not always:
one frame paired a planet limb with an asteroid at a 1024 px offset that only
applies at infinity, and produced a confident 2.597. **`dx` near 1024 is not
proof of identity** — the check is a correlation of the two neighbourhoods, and
with it the unusable frames (8, and the all-frame ones) name themselves.

So: **the difference exists only once the cockpit scene is drawn.** In the menus
and the loading screens the brightest object is identical between eyes to within
1.5%.

Excluded, each on its own evidence:

* **The `p1_star` sprite.** Its quad size comes from `M_projection[1][1]`,
  `V_cameraposition` and `M_view`. The per-eye patch touches `[2][0]` (shear)
  and `[3][0]` (`B_x`) and **not** `[1][1]`, so `boost`, `fovy` and `worldScale`
  are identical in both eyes and the sprite is the same size in each. Read off
  the shader source, not inferred.
* **The kinobloom pyramid.** Level `img #66` measures `dx = -256.0` exactly
  (1024/4 at quarter resolution) with core and halo ratios of **1.000**.
* **A global gain difference.** Ruled out earlier: median `(R-L) = 0.00000`.
* **Occlusion.** The side-by-side at frame 9 shows the star cluster, HUD arc,
  reticle, canopy edge and speed meter all matching; nothing crosses the glow.

What is left is exactly one thing, and it was documented at
`docs/frame-analysis.md:7209` long before this task existed:

    OUT_RT0.rgb = scene · fog.a + fog.rgb        // transmittance and in-scatter

> "The volume is built by compute, so it cannot be per-view. The fog volume is a
> 88x88x128 3D image ... written by compute. There is **one** volume, built once
> per frame for one camera, and both views composite from it."

Take 176 confirms the mechanism is still live: `7 of those are compute: no
gl_ViewIndex exists there`. The in-scatter term is **additive**, **warm**,
**spatially broad** and **only present in the world scene** — every property of
the difference. It is the same shape as task #48's finding: not a computation
that is wrong, but one that is *mono* while the frame around it is per-eye.

#### Take 177 — one knob, and it is NOT vacuous

`X4VR_DISABLE_FOG=1` already exists: *"volumetric composite forced to scene*1+0
— DIAGNOSTIC, not a fix"*.

**Checked against take 173's mistake first**, because that is the rule now:
what does this arm show if the suspect is INNOCENT? With fog off, both eyes
still draw the Sun through `p1_star` and still bloom it, per-eye, so a halo
difference *can* still appear. It removes one additive term rather than
collapsing the two eyes into one image. That is a real elimination, unlike
`X4VR_BINDLESS_PATCH=0`, which made the right eye a copy of the left and could
only ever answer "symmetric".

- **P95** — with fog disabled the cockpit frames read core and halo ratios near
  1.0, matching what the menu frames already read. The in-scatter is confirmed
  and #49 becomes "make the froxel volume per-eye, or accept it".
- **P96 (refutation)** — the halo stays ~0.7. Then the in-scatter is innocent
  and the only remaining candidate in the frame is the eye image itself, i.e.
  our own compositor, which would be the first defect of this task located in
  our code rather than X4's.

**Cost, stated before the run**, because take 176 was not:
`X4VR_DISABLE_FOG=1` changes a shader constant and costs nothing per frame. The
only telemetry is `X4VR_MV_DUMP_PRESENT=600` — one readback every ~20 s, and
dumps are PNG now. **No probe.** The game should be as playable as take 175a.

### Take 177: P96 — the fog is innocent, and the culprit is X4's LENS FLARE

`X4VR_DISABLE_FOG=1`, 8 passthrough patches applied. The numbers did not move:

    175a frame 9  (fog ON )   core 0.919   halo 0.692   area 3220 -> 1549
    177  frame 6  (fog OFF)   core 0.918   halo 0.692   area 3200 -> 1553

Identical to three decimals. **The volumetric in-scatter is not the cause**, and
the previous section's localisation was wrong. Recorded, not edited away: the
in-scatter matched every *qualitative* property — additive, warm, world-scene
only — and that was not enough. "Fits every property I listed" is a statement
about my list.

Also excluded on this run's data:

* **Exposure.** The star cluster, a second object at infinity, reads p99
  **0.3631 vs 0.3638** between eyes — the same features at the same peak
  luminance. Nebula bands likewise. There is no per-eye exposure difference.
* **The sprite.** At 4x zoom the octagonal `p1_star` core is the same size in
  both eyes and the flare star beside it matches. Only the surrounding glow
  differs.

#### What it is

The difference is **chromatic**, which a luminance-only analysis could not see:

    annulus     R      G      B
      0-20    1.00   0.99   0.97      <- core: identical
     20-35    0.68   0.69   0.88
     35-50    0.60   0.75   0.93      <- red down 40%, blue down 7%
     50-80    0.78   0.88   0.95

A warm ring, 20-50 px, present in the left eye and largely missing in the right.
`shadergl/glsl/kinobloom/prefilter.frag.glsl`:

    if (U_lens_pass) {
        vec2 texcoord = -IO_uv0 + vec2(1.0);                    // mirror about CENTRE
        vec2 ghostVec = (vec2(0.5) - texcoord) * uGhostDispersal;
        float weight = length(vec2(0.5) - offset) / length(vec2(0.5));
        weight = pow(1.0 - weight, 10.0);                       // 10th power
        result.rgb *= max(0.01, ColorBaseDiffuse.r);            // scaled by RED
        ...
        float uHaloWidth = 0.56f;                               // a ring, in UV
        weight = pow(1.0 - weight, 4.0);
        pix += result.xyz * 0.5;                                // ADDITIVE
    }

Every term is anchored to `vec2(0.5)` — **the image centre** — and the result is
weighted by the **red** channel. This is a screen-space lens flare: ghosts
mirrored through the centre, plus a halo ring at a fixed UV radius.

In VR the two eyes' image centres are **different world directions**, -15.04°
and +15.04°. The Sun sits 489 px from centre in the left eye and 719 px in the
right, and `pow(1 - d/dmax, 10)` turns that into a large intensity difference.
Hence: same core, warm ring present in one eye and not the other, red-dominant,
world-scene only, indifferent to fog and to exposure. Every measured property,
from the code rather than from a list of adjectives.

**This is task #40's class, not a new one.** Screen-locked content has to be
placed in the DECLARED frame; #40 did it for the HUD canvas. The lens flare is
the same defect one layer deeper, inside a shader X4 owns, and the fix is the
same idea: the `vec2(0.5)` the pass mirrors about must become the per-eye
declared centre, offset by `B_x`.

> **RETRACTED before it was implemented.** Going to write the patch, the first
> step was to find the module — and it is not there:
>
>     U_lens_pass   0 of 409 dumped modules
>     U_threshold AND S_input_rt together (the prefilter)   0 modules
>     ogl/kinobloom_prefilter.xml:  <bool name="lens_pass" ... value="false"/>
>
> `U_lens_pass` is a uniform bool, not a `#define`, so the branch and its
> constants (`0.56`, `0.0005`, `0.40`) would be compiled into the module even
> with the flag false — and none of them appear anywhere. The kinobloom
> prefilter is not in the dump set at all.
>
> **So the lens pass is not shown to run, and the identification is worthless.**
> It was made by reading X4's GLSL and matching it against adjectives —
> additive, warm, screen-centred, red-weighted — without checking that the code
> executes. That is [[x4vr-imports-are-not-calls]]: source proves a path exists,
> not that it is taken. Finding X4's shader sources made this error *easier*,
> not harder, because a plausible mechanism is now always one grep away.
>
> **Third wrong localisation in a row for #49** — the bindless redirect, the
> volumetric in-scatter, and now the lens flare. Each fitted every property I
> had listed. Three in a row is not three unlucky guesses: it is a method
> failure. The method was "find a mechanism whose description matches the
> measurement", and the space of such mechanisms is large. What has actually
> held up in this project is the opposite order — localise **in the frame**
> first (which pass, which image), then read the shader that pass binds. That
> is how #48 was found.

#### The format change broke the analyser, and the intent gate then lied

Take 177 first scored **FAIL — "no present dumps were found ... -present-n*-layer*.ppm"**
with twenty PNGs on disk. `tools/eye_stereo.py` globbed `.ppm`, `score_run.py`
trusted it, and the gate that exists to catch a run that failed to write instead
accused a good run. **An intent gate reading a stale analyser is worse than no
gate**: it produces a confident failure about the wrong subject.

Fixed in both analysers, and locked with `tests/run-dump-format.sh`, which
drives the layer twice, asserts the PNG and PPM decode to **identical pixels**,
and asserts the two analysers agree with each other on the same file. The
assertion is **captured and judged**, not printed — the first version printed
`PIXELS-IDENTICAL` and returned 0 regardless, a test that could not fail, in
the file written to catch tests that cannot fail.

### The method changes: localise in the FRAME, then read the shader

Three wrong localisations in a row for #49 — bindless redirect, volumetric
in-scatter, lens flare — each arrived the same way: read a shader, find a
mechanism whose *description* matches the measurement, believe it. The space of
mechanisms that match a description is large, and having X4's GLSL made that
error easier rather than harder, because a plausible candidate is now one grep
away.

What has actually worked in this project is the other order. #48 was found by
asking *which pass, which module, which variant* and only then reading the
shader that pass binds. So #49 goes back to that, and the instrument needed one
change first.

#### `X4VR_MV_PROBE_MAX` — the probe is now boundable

The probe's information is bounded: one hash, and with `X4VR_MV_DUMP_AUTO` one
dump, per per-eye image. Its cost was not — take 176 probed every frame for a
whole session. `X4VR_MV_PROBE_MAX=N` stops it after N samples, and it says so.

Asserted at the site where a sample is **taken**, not on sweep completion:
sweep completion is only observable from the present path, and nothing in the
test tree presents. **A bound that cannot be exercised is a bound that ships
broken** — this session has already shipped two. The suite drives it at unset,
0 and 1. Stated limitation: the harness renders one pass, so the cases show the
cap *branch* is taken instead of the sampling branch, not a long-run count.

While adding them: `tests/run-multiview-render.sh` had its
`if (( fails )); then exit 1; fi` at line 1441 with **three case groups running
after it**, counting failures into a variable nothing read again before an
unconditional `echo "all cases passed"`. Those cases could not fail. The guard
is now at the end, and that was verified by forcing a tail case to fail and
checking the suite exits 1.

#### Take 178 — walk the chain, in a parked scene

    X4VR_MV_PROBE=1 X4VR_MV_PROBE_MAX=40 X4VR_MV_DUMP_AUTO=1
    X4VR_MV_DUMP_PRESENT=600

40 samples, then the probe stops. One dump pair per per-eye image, PNG, ~8 MB
each instead of 106 MB. **The scene must be PARKED** — take 176's intermediates
and present dumps came from moments ~30 s apart in a session that could barely
run, which is why they could not be compared to each other at all.

- **P97** — the warm ring is absent from every intermediate and present only in
  the final eye image. Then it is introduced after X4's last colour target, and
  the suspect is our own compositor.
- **P98** — some intermediate already carries it. Then that image's writing pass
  is named by `mv final: img #N writers`, and the shader to read is the one that
  pass binds — which is the order that worked for #48.

### Take 178 — the chain walk lands: X4's targets are CLEAN, our eye image is not

Patola looked at the dumps and corrected two readings of mine, both of which had
the same shape: **I measured whatever the blob finder returned instead of the
object in question.**

1. `img #65` is the **savegame loadscreen**, not the cockpit. Its "no per-eye
   difference" is worthless — take 175a already established that menu and
   loading frames are identical between eyes (0.985–1.016). I had cleared a
   suspect with an instrument sampled in a scene where the defect does not
   exist.
2. In `present-n1` the finder locked onto a **HUD element** (area 10415), and
   reported 1.00/1.00/1.00 on it. Patola: *"the asymmetry is very clear on those
   images."* It is; the finder simply was not looking at the Sun.

Replacing "brightest blob" with "the warm peak in the upper band" — the Sun is
not the brightest thing in a cockpit frame, the cyan HUD arc is — gives the
chain walk, and every row now finds the Sun at the cant offset in both layers:

    image        dx      halo R/L    halo R/G/B
    img50      -1024       1.009     1.01/1.01/1.00
    img52      -1024       1.030     1.02/1.03/1.06
    img53      -1024       1.016     1.02/1.02/1.00
    present-n1 -1019       0.790     0.73/0.80/0.92

**Three of X4's own colour targets show no per-eye difference; the presented eye
image shows a 21% red-weighted deficit.** The dumper's tone map cannot be hiding
it: `v/(1+v)` then gamma turns a true 0.79 into ~0.93, not 1.01.

`img50/52/53` are written by `masked rp [7,0]` — X4's present passes. So the
difference appears **after X4's last colour target**, which means it is ours: the
SbsCompositor, the copy into the two-layer eye image, or the path to the XR
swapchain. **First defect in this task located in our own code**, and unlike
every previous candidate it can be investigated without spending a take.

P97 holds; P98 is refuted.

#### On the visual and the numbers disagreeing

At 3x zoom the left eye's Sun sits in a broad **warm orange** glow and the right
eye's core is **white** with the warm halo largely gone and sharper rays. That is
why it reads as *brighter* on the right while measuring *dimmer*: the core is
saturated in both (peak 1.0000) and what differs is the warm surround. Patola's
original report and the measurement are the same observation, and the same
direction holds in take 175a — the effect is reproducible across runs, not a
sign that flips.

**The lesson is about the finder, not the eye.** Three separate times now a
conclusion has rested on a heuristic that picked the wrong object: a planet limb
against an asteroid, a loadscreen against a cockpit, a HUD arc against the Sun.
`dx ≈ 1024` and a high correlation are necessary and *not sufficient* —
`present-n1` passed correlation 0.854 on the wrong object. **Look at the crop
before believing the number**, every time; it costs one Read and it has caught
every one of these.

### #49 narrowed to five passes — the probe dumps are after rp #0, not after the frame

The compositor read settles where the two measurements sit in the frame.
`SbsCompositor` copies FROM the eye image (X4's own render target, which X4
believes is the swapchain) INTO the real swapchain image, and the present dump is
taken from that same source — its comment says so:

    // Task #29: the finished eye image, every layer, while it is still in
    // TRANSFER_SRC. This is the frame as X4 left it -- after every present
    // pass, so after the UI and anything drawn over it, which is exactly what
    // the end-of-render-pass probe cannot see.

And the probe's own log line says when it sampled:

    img50/51/52/53 ... (fmt 44, after rp #0, DIFFER)

**All four are the same thing at different times: the eye image (fmt 44, one per
swapchain slot), sampled after rp #0 — the FIRST of seven present passes.** The
present dump is after all of them. So:

    after rp #0        halo R/L 1.009 / 1.030 / 1.016   clean
    after everything   halo R/L 0.790                   the defect

**The defect is introduced by one of rp #1, #4, #7, #10, #33, #51.** That is a
five-pass suspect list out of a frame of ~60 passes, and it corrects the previous
section: this is not "after X4's last colour target", it is *between the first
present pass and the last*.

Cleared already:

* **rp #33** binds `mod-0261/0263/0265/0345`, which the source join names
  `xu_ui_unlit` and `xu_ui_unlit_sdf` — UI shaders, correctly on the canvas.
  That is #40 working, not a defect.

Standing, with what each binds:

* **rp #1, #7** bind `mod-0000`: real vertex attributes (`pos`, `uv_in`,
  `color_in` at locations 0/1/2), so `is_procedural_fullscreen` is FALSE and it
  **receives the canvas variant** — a per-eye shift of ±0.2425 NDC.
* **rp #4, #10** bind `mod-0006`: `gl_VertexIndex` only, `PROCEDURAL=1`, so it is
  excluded from the canvas and takes `original`.
* **rp #51** writes `img #65` (fmt 50), the format the runtime also uses.

#### What the next instrument has to be

Neither existing dump can see between two present passes: the probe fires at the
end of the FIRST pass that ends with that attachment, and the present dump fires
after the last. `X4VR_MV_DUMP_AFTER_RP=N` — capture the eye image at the end of
a NAMED render pass — bisects the five in one run, at the cost of one readback
per dump, which is the present dump's cost and not the probe's.

**Note for whoever writes it:** the present dump is bounded and cheap because it
fires every N presents; the probe was not, and cost take 178 its own
measurement. Give the new one a cadence, not a per-frame trigger, and price it
in the run spec before asking for the run.

### Take 179 — exactly what to extract (READ THIS FIRST, IT IS THE WHOLE STATE)

One command does the analysis:

    python3 tools/sun_halo.py --prefix=/tmp/x4vr-t179 --crop=/tmp/c

Files land as `/tmp/x4vr-t179-afterrpN-nM-layer{0,1}.png`, one pair per capture,
plus `-present-nM-` pairs. The tool prints one block per pair, in PASS ORDER.

**The question is one line:** which of rp #1, #4, #7, #10, #33, #51 is the first
whose `halo 35-50` ratio drops from ~1.0 to ~0.75. Everything before it is clean;
that pass introduces the defect; read the shader that pass binds.

Reference values, measured, so a new run can be judged against them:

    clean   (X4's eye image after rp #0)   peak ~0.73/0.73   halo 1.00-1.05
    defect  (the presented eye image)      peak 1.0000/1.0000 halo 0.751
                                           halo R/G/B 0.70/0.76/0.91

#### Three rules, each of which was learned by getting it wrong

1. **`same_object` must be true or the row means nothing.** The tool prints
   `<- NOT THE SAME OBJECT` when `dx` is not within 40 px of −1024 (the
   eye-to-eye offset at infinity) or the y positions differ. On take 178 that
   flag correctly rejects img51, img59, img61 and img67 — every row that
   produced a wrong conclusion — while passing img50/52/53 and present-n1.
2. **Look at the crop before believing the number.** `--crop=` writes the two
   crops side by side at 2x. A HUD arc passed a correlation of 0.854 against the
   Sun; only the picture showed it. This costs one Read and has caught every
   mis-identification in this task.
3. **`peak` tells you where in the chain you are.** X4's own targets are not
   saturated at the Sun (~0.73); the presented image is (1.0000). A row whose
   peak is 1.0 in both eyes and whose halo is ~1.0 is genuinely clean, not
   clipped into looking clean.

#### What is already settled — do NOT re-litigate

* The defect is a **warm halo** difference, not brightness. Core and peak match;
  the 20–50 px ring is 20–30% weaker in the RIGHT eye and red-dominant
  (R 0.70 / B 0.91). It reads as "the right Sun is brighter" because a white
  core unmuted by orange haze looks brighter. Patola's report and the numbers
  are one observation.
* **Eliminated**, each on its own evidence: the volumetric in-scatter (take 177,
  `X4VR_DISABLE_FOG=1`, numbers unchanged to three decimals); per-eye exposure
  (a second object at infinity reads p99 0.3631 vs 0.3638); the `p1_star`
  sprite (its quad size comes from `M_projection[1][1]`, which the per-eye patch
  does not touch); kinobloom's pyramid (level `img #66`, dx exactly −256 at
  quarter res, ratios 1.000); a global gain difference (median R−L = 0.00000);
  X4's lens flare (`U_lens_pass` is in **0 of 409** modules and defaults false).
* **`X4VR_BINDLESS_PATCH=0` is a vacuous control here** — it makes the right
  eye's scene view 0's pixels, so the Sun is trivially identical. Three takes
  went into that. Do not spend a fourth.
* rp #33 is cleared: its World-on-canvas modules are `xu_ui_unlit`/`_sdf`, UI
  shaders correctly canvassed by #40.

#### If the bisect comes back all-clean

Then the defect is after the last present pass, and the only thing left is the
`SbsCompositor` copy or the cursor overlay — both ours, both inspectable without
a run. Note that `cursor_.record()` draws into the eye image **before** the
present dump is taken, so the cursor is *in* the dumped image and is a live
suspect for anything drawn near the frame centre.

### Piece 3 is NOT next

The 1092×1180 render extent (0.65× area) stays parked until the two items above
land. Take 165a is already geometrically correct and merely wastes 22% of every
row, so there is no correctness pressure — only the performance prize.

### Take 179 — the bisect never ran, and the present dumps answered anyway

**The run did not do what it was asked to do.** `X4VR_MV_DUMP_AFTER_RP=1,4,7,10,33,51`
produced zero files, and the layer said so:

```
mv dump: X4VR_MV_DUMP_AFTER_RP=1 produced NOTHING — the pass was never reached
         with a masked two-layer attachment.
```

Two instrument defects, both mine, both of the class this project keeps hitting:

1. **The round-robin cannot step over an unreachable target.** `dump_after_rp_idx()`
   advances only *after* a capture lands, so target #1 being unreachable starved
   #4, #7, #10, #33 and #51 as well. One bad entry in the list voided the list.
   The end-of-run report names only `[0]`, which is why it said "=1" for a
   six-target run.
2. **The end-of-run report was the only thing that fired.** The per-pass
   diagnostic ("reached but not captured") sits *inside* `if (!atts.empty())`,
   so the one case that actually happened — `atts` empty — is the one case it
   cannot report. Same shape as P-nothing-and-no-reason it was written to prevent.

The premise was also wrong. I called rp #0/#1/#4/#7/#10/#33/#51 "the seven present
passes"; `+PRESENT-CAND` is a *candidate* predicate. This run's own summary says
`present passes — 3 pass(es) draw into a swapchain image`, and they do not form a
chain on one image — they write #1–#4, #50–#53, #65 and #100. A serial bisect
assumed a chain that does not exist.

#### What the present dumps establish (all offline, all reproducible)

Five dumps of a nearly-static cockpit scene with the Sun in frame (n6–n10):

* The two eyes are **precisely registered**: core centroid dx = **−1025.0**,
  dy = −0.3, against the −1024.36 the cant predicts. Same object, to the pixel.
* On **aligned smooth sky** away from the Sun the eyes agree to 1 %
  (R/L = 0.990, 0.991, 0.990, 0.988, 0.989). This is Sun-specific. A whole-frame
  mean says 0.894 — that is **framing**, not brightness: at dx = −1025 the eyes
  share only 76 % of their pixels. Do not quote the whole-frame number.
* The halo is **concentric** with the core in both eyes (best annulus alignment
  +2,0; residual identical to 0,0). Not a displaced screen-space effect.
* The left eye's Sun **changes between frames and the right eye's barely does**.
  Patch residual between consecutive frames: left 0.011–0.024, right
  0.00006–0.0001 — 100–200× less, at comparable patch brightness (0.408 vs
  0.382). The right eye's saturated core is **exactly 1111 px in all five
  frames**; the left's swings 1199→1684.

  **Do not read that as "the right eye is frozen".** The plume saturates, so it
  inflates the left eye's saturated area and drags its centroid; a left centroid
  that moves 5 px while the right's does not is what an animated component
  present in one eye alone produces, and is *not* evidence of a stale buffer.
  The two readings call for completely different investigations, and only the
  first is supported. What is established is that the varying part of the Sun
  exists in the left eye only.

**Looking at it settles what no ratio could.** Left eye at n6: a round orange
glow. Left eye at n10: a large diagonal orange plume up-and-right. Right eye at
n6 and n10: a smaller white core with radial spikes and **no warm component at
all**, essentially the same picture twice.

#### RETRACTED: "the halo is 20–30 % weaker in the right eye"

It is not weaker. **A warm, red-dominant, animated glow component is ABSENT in
the right eye**, and the band ratio could not tell "attenuated" from "missing"
because it only ever compared two numbers at one radius. The right eye's spikes
look *sharper* for the same reason — nothing is washing them out. The earlier
per-channel reading (R 0.70 / B 0.91) was a real measurement of the wrong model.

This also explains why no single transform fitted: best radial scale s = 0.855
(residual 0.0213) and best uniform gain g = 0.875 (residual 0.0235) both sit far
above a no-op 0.0382 because neither "smaller" nor "dimmer" is what happened.

#### The new suspect: doubled images that only an unmasked pass ever writes

```
mv final: img #70 writers — masked rp [] unmasked rp [41]      2048x2048 fmt=124 DOUBLED
                     #71                        [43]           (fmt 124 = D16_UNORM)
                     #72                        [45]
                     #73                        [47]
                     #74                        [49]
rp #41.0: 0 colour [] depth 124 final=-1 -> MONO (depth-only/shadow)
```

Five shadow cascades, doubled, written by mono passes — so **layer 1 is cleared
and never rendered**. `mv_double_candidate()` says so on purpose:

> *Doubled but never rendered per-eye → layer 1 is simply never touched. Costs
> memory, nothing else. … In particular the shadow atlas is doubled and wasted*

**"Costs memory, nothing else" is only true if nothing samples layer 1**, and the
bindless index-offset patch exists precisely to make view 1 sample elsewhere.
A warm directional plume that moves with the camera is what ray-marched
in-scatter through a shadow cascade looks like. This is NOT the in-scatter
hypothesis take 177 refuted: that one asked whether in-scatter existed, this one
asks what view 1's shadow lookup returns.

**And the counter that should have caught it cannot.** `per-eye images written
layer-0-only=0` is computed by `note_layer0_only()`, which is called **only from
transfer and clear commands** — `vkCmdCopyImage`, `BlitImage`, `ResolveImage`,
`Clear*`, `CopyBufferToImage`. It is never called for a *render pass* that writes
a doubled image without a viewMask. The name promises coverage the check does not
have, so it read 0 with five such images on screen.

#### Prediction, before the next measurement

If the plume is in-scatter sampling shadow layer 1, then making view 1's shadow
lookup read layer 0 restores the warm component to the right eye and leaves the
left eye unchanged. If the plume is unchanged in both eyes, shadows are not the
carrier and the next place to look is any masked module that samples a
doubled-but-layer-0-only image.

#### Why each take-179 target failed, exactly

`atts` requires an attachment that is doubled **and** carries
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` — you cannot read back an image that is not a
transfer source. Cross-referencing this run's own `img #N:` records against its
writers list:

| target | verdict | why |
|--------|---------|-----|
| rp #1  | uncapturable | writes doubled #1–#4, **usage has no TRANSFER_SRC** |
| rp #4  | not a writer | never appears in the writers list |
| rp #7  | uncapturable | writes doubled #50–#53, no TRANSFER_SRC |
| rp #10 | not a writer | never appears in the writers list |
| rp #33 | **would have worked** — writes #65 (0x97, TRANSFER_SRC) |
| rp #51 | **would have worked** — writes #100 (0x97, TRANSFER_SRC) |

So two of the six were capturable and produced nothing purely because rp #1 sat
at index 0 and the round-robin only advanced after a capture landed. That is the
starvation bug, and this table is what it cost.

**Rule for choosing a target:** it must appear in an
`img #N writers — masked rp [...]` line whose image's `usage` has bit 0 set.
The capturable serials in take 179 were 13, 17, 19, 21, 22, 23, 24, 25, 27, 29,
31, 32, 33, 51, 53, 54, 55, 58, 61. Present passes are never capturable — they
draw into the swapchain.

#### Take 180 — where does the warm component first go missing?

Targets `13,25,51,61`, all capturable, spread across the frame: #13 early scene
(img #59/#60/#61, 4224² fmt 97 HDR), #25 mid scene (adds #57), #51 post
(img #100), #61 late scene. Read with:

```
python3 tools/sun_halo.py --prefix=/tmp/x4vr-t180 --crop=/tmp/c
```

**The question is one line:** in each `afterrpN` row, is `WARM EXCESS` present in
both eyes or in the left only? The first pass whose output has it in the left eye
and not the right is where it is introduced — or, if every capturable pass has it
in both, it survives the scene chain and is lost in the present chain
(rp #0/#1/#7), which no readback can reach and which is then a code question, not
a run question.

Reference values from take 179's presented image: left warm excess at 35–50 px
**0.247–0.415**, right **0.1294 in every frame**, ratio 0.31–0.52.

Three rules for reading a row, each learned by getting it wrong:
1. **A row that says "no solar disc" or "neutral in BOTH eyes" means nothing** —
   there was no Sun in that frame. Four wrong conclusions in this task came from
   measuring an object that was not the Sun.
2. **Look at the crop before believing the number.**
3. **`same_object` must be true**, and the core centroids must differ by ≈ −1024.

#### Still open, and NOT to be re-litigated as settled

`X4VR_SHEAR_LIGHTS=0` is a **vacuous control** here, exactly like
`X4VR_BINDLESS_PATCH=0`: it puts the suns back on the mono path, so both eyes get
identical pixels by construction and the Sun is trivially symmetric. It proves
nothing about the mechanism. Three takes went into learning that lesson on the
bindless knob; do not spend a fourth here.

### Take 180 — and the knob that was never in a known-good run

Take 180's captures all landed in the first seconds: two were entirely black
(max luma 0.0000, layers identical) and the third, which Patola identified on
sight, is **the X4 logo on the menu screen** — not a planet, as I first read it.
Fifth wrong object in this task, fifth time looking settled it.

The present dumps reproduce take 179 exactly, at a different screen position:
left warm excess 0.210–0.396, right **0.1270–0.1284**, ratio 0.32–0.61. Across
two takes, many frames and two different Sun positions, the right eye's warm
excess is a constant ≈0.128 — consistent with the right eye contributing *no*
glow at all, so that the annulus reads the background nebula and nothing else.

#### The after-rp bisect is unsound by construction — do not use it again as-is

Two independent reasons, both proven by this run:

1. **Pass serials are not stable between runs.** Creation order past roughly #33
   depends on what content got loaded. rp #51 wrote `[100]` in take 179 and
   `[54,55,59,60,61]` in take 180; rp #61 wrote five images in 179 and **does not
   exist** in 180. Serials 0/1/7/13/25/33 were stable, everything above was not.
   So a target list copied from the previous run's log can silently mean
   something else.
2. **The round-robin still stalls.** The give-up added after take 179 covers
   "reached with nothing to read". rp #61 was *never reached*, which it does not
   cover, so after three captures the instrument parked on #61 and was dead for
   the remaining five minutes — exactly the stretch with the Sun in frame.

**Redesign before reuse: target by IMAGE serial, not pass serial.** Image
serials for the early buffers (#50–#53, #59–#61, #65) were identical across both
runs. "Dump img #59 whenever a masked pass writes it" is stable in a way that
"dump after rp #61" is not.

#### REFUTED before implementing: the mirrored-plume prediction

I predicted that if the plume came from mixing the per-eye-patched
`M_projection` with an unpatched `M_projection_uj`, the mismatch would be `+B`
in one eye and `−B` in the other, so the right eye should carry a plume pointing
the opposite way (down-left, since `(B_x,B_y) = (+0.2425,−0.1932)` points
up-right at ~38°). A 1040 px gamma-lifted crop of both eyes shows **no mirrored
plume**: the scene matches nearly everywhere and the right Sun is simply whiter
and smaller. Recorded because it was committed before the check, not because it
worked.

#### THE CONTROL WAS NEVER RUN

`X4VR_AA=fsr_quality` has been on the command line of **every take since ~158**,
including all of 170–180. It appears **zero times in docs/known-good-runs.md**.
Its own definition says what that means:

> *"We serve `none` by default and have since the beginning. … it is unset by
> default like `X4VR_FOV`: setting it is the experiment, omitting it is the
> control."*

And the default is not arbitrary:

> *"this tag also selects X4's upscaler, and every temporal mode here
> (`temporal`, `dlaa`, the `fsr_*` family) jitters the projection — **into
> m[8]/m[9], which are the eye shear's own slots**."*

`m[8]`/`m[9]` are `M[2][0]`/`M[2][1]` — where an off-axis shear lives.
Independently, `docs/frame-analysis.md` line 621 lists `M_projection_uj` as
**PATCH — eye projection without TAA jitter (== #2 with AA off)**, and the layer
patches members 1, 2 and 4 but **not member 3**. That omission is inert with AA
off and live with any temporal mode.

So every measurement in #49 was taken with a temporal upscaler writing the eye
shear's own matrix slots, under an experimental knob, while a matrix the design
says to patch went unpatched — and I called the result a defect in X4's glow.

**Take 181 is the control: take 180's command with `X4VR_AA` REMOVED, nothing
else changed.** Predictions, committed before the run:

* If the Sun's warm asymmetry **disappears**, the cause is the temporal path —
  the jitter in m[8]/m[9] and/or the unpatched `M_projection_uj`. #49 becomes
  "patch member 3 and decide what FSR is allowed to do", and `X4VR_AA` comes off
  the standard command line until it is settled.
* If it **persists unchanged**, AA is exonerated on its own evidence and the
  next suspect is the shadow cascades #70–#74, the only doubled images whose
  layer 1 is never rendered (5 of 29, confirmed by this run's own report).
* Expect it to be **slower**: with `none`, X4 renders 4224² natively instead of
  upscaling, so the 33 ms gameplay figure will rise. That is not a regression,
  it is the control's cost.

### Take 181 — AA exonerated, and the "negative control" that wasn't

**The asymmetry is unchanged with the temporal upscaler off.** Left warm excess
0.259–0.450, right pinned at **0.1298–0.1300** — the same numbers as takes 179
and 180 with `fsr_quality`. AA is eliminated on its own evidence.

That does not undo the finding that it was never the control. `X4VR_AA` stays
**off the standard command line** from here: it is unset by default, the default
is `none`, and no known-good run has ever set it. What take 181 buys is that the
20 takes measured under it were not measuring the upscaler.

`M_projection_uj` (camera member 3) is still unpatched while
`docs/frame-analysis.md` line 621 says **PATCH**. Take 181 shows it is not
causing #49, because with AA off member 3 equals member 1 and the defect is
identical. It remains a real gap for any future run that enables a temporal
mode — logged as its own task, not as part of #49.

#### present-n3 looked like the control this task never had. It is the loading screen.

```
core 1916/1909 px  (R/L 0.996)   luma 0-20:1.000 ... 50-80:1.000
at 35-50 px: left 0.1851, right 0.1852   <- both eyes carry it
```

A large warm saturated disc, symmetric to four decimals, with matching spikes,
streaks and ghost ring. It is **flat artwork on X4's savegame loading screen** —
letterboxed, with a "Loading" spinner and a tip line underneath. Its Sun is a
*picture* of a sun, placed identically in both eyes. Sixth wrong object in this
task.

`sun_halo.py` now rejects it generally rather than by name: **99.8 % of that
frame aligns at the infinity offset**, because there is no near geometry in it at
all. A rendered 3D scene does not do that. Any row whose overlap matches above
95 % is canvas artwork and cannot be a control for anything.

#### What is left standing

* Absent, not attenuated; concentric; precisely registered (dx ≈ −1025).
* Everything else in the frame agrees to 1 % on aligned smooth sky.
* Not AA, not exposure, not a global gain, not a displaced screen-space effect,
  not the `p1_star` quad size, not kinobloom, not X4's lens flare.
* The deficient eye has been **layer 1 in every gameplay frame of takes 179, 180
  and 181** — but layer 1 and the negative shear sign are confounded, and
  nothing so far separates them. Task #27 was the same shape and turned out to
  follow the *shear sign*, not the layer.
* The only doubled images whose layer 1 is never rendered are the five 2048²
  shadow cascades #70–#74 (5 of 29 tracked). Note 68 images were doubled and
  only 29 have tracked writers — the other 39 are unaccounted for and that gap
  has never been checked.

#### Take 182 — does it need the cockpit?

One run, three phases, no new instrument: cockpit with the Sun in view, then
X4's **external camera** with the Sun in view, then back to the cockpit.

* If the Sun is **symmetric in external view**, near geometry is required to
  provoke it, which points at a depth-dependent test (occlusion/soft-shadowing
  of the glare) reading a position that is not per-eye.
* If it is **asymmetric in both**, geometry is exonerated and the cause is in how
  the Sun's glow is drawn, independent of what is in front of it.

Second question in the same run, for Patola's eye: pointing the ship in several
directions, is it **always the right eye** that lacks the orange haze? An
orientation where the LEFT eye is the deficient one would separate "follows the
layer" from "follows the geometry" — and take 172 already reported that changing
angles does not change it, which is worth reconfirming now that the measurement
is trustworthy.

### Take 182 — IT FLIPS. #49 is a shared volumetric, not a broken eye.

Patola, from the headset: *"it's not always the same eye that's bigger, it
depends on the rotation!"* He is right, and my instrument had hidden it.

```
frame   left    right
n8      0.4021  0.1289     right deficient
n9      0.4128  0.1293     right deficient
n12     0.2734  0.2667     BOTH EYES HAVE IT
n13     0.2446  0.1247     right deficient
n14     0.1282  0.2626     LEFT DEFICIENT — the flip
n15     0.3921  0.1261     right deficient
n17     0.4664  0.1279     right deficient
```

Two things fall out of that table:

1. **The deficient eye is always at ≈0.128** — the same floor in every row, and
   in n14 it is the LEFT eye sitting on it. The healthy eye ranges 0.24–0.48.
   The glow is not attenuated by some amount; it is **binary, per eye, per
   frame**: drawn or not drawn. 0.128 is the background nebula with nothing
   added.
2. **It is not "layer 1 is broken".** Every earlier take happened to catch
   right-deficient frames, and I read a scene-dependent gate as a property of
   the eye. The confound between "layer 1" and "the negative shear sign" is
   resolved: it is **neither** — it depends on the scene.

#### The instrument bug that hid it

My flat-frame check compared `|L-R|` against a **fixed** 0.02 over the whole
overlap. Space is mostly black, so most pixels were below 0.02 in both eyes and
matched regardless of alignment — it measured **darkness, not flatness**, and
rejected n12, n13 and n14 as "loading screen". Those are three of the four
frames that carry the flip. It now masks to pixels with signal and scales the
tolerance with it, and returns "cannot tell" when too little of the frame is
lit. Patola caught this from the pictures.

#### Two hypotheses killed by a static scan of the 409 dumped modules

A SPIR-V scan for `OpAccessChain` into set=1 binding=0 (declaration is not use):

* **`M_projection_uj` (member 3) is read by ZERO modules.** It cannot cause
  anything. Take 181's dismissal of it was right for the wrong reason — I argued
  "with AA off member 3 equals member 1", when the real answer is that nothing
  reads it. The unpatched-member-3 gap is cosmetic, not live.
* **No FRAGMENT shader reads `M_projection` (member 1) at all** — all 34 readers
  are vertex or compute. The "fragment shaders re-project with the centre frame"
  idea is dead too. mod-0365–0370 are **Vertex**, so they do get the vertex
  patch.

#### What the scan found instead

A tight cluster reads the sun's direction *and* the volumetric set:

```
mod-0365..0370 (Vertex)   1:M_projection 2:M_invprojection 11:V_viewportpixelsize
                          16:V_lightcolor1 21:V_light_direction_view
                          28..33:V_volume_scatter_power/phase*/I_volume_phases
                          58:I_maindepth 65:F_volume_off 66:F_volume_scale
mod-0364     (GLCompute)  21:V_light_direction_view 25:V_volume_size
                          29:V_volume_sigma_extinction 34:I_volume_bind_index
mod-0358                  2:M_invprojection 24:V_volume_range 25:V_volume_size
```

and the run's own log names the structure:

```
img #33/#75/#91:   64x64x64  usage=0x8f      <- STORAGE: written by compute
img #34/#76/#92: 128x128x64  usage=0x8f      <- froxel volume
mv final: compute — 15 pipeline(s), 7 shader(s) dispatched
bindless mirror: 9 REFUSED (7 of those are compute: no gl_ViewIndex exists there)
invproj:         248 corrected (3 compute cannot be)
```

**`mv_double_candidate()` requires `imageType == VK_IMAGE_TYPE_2D`, so a 3D image
is never doubled.** There is exactly one scattering volume, and the compute
shaders that fill it cannot be per-eye because `gl_ViewIndex` does not exist in
compute — the layer already says so in two separate lines.

**Hypothesis (not yet proven): X4 builds its volumetric in-scatter into a shared
froxel volume in the CENTRE camera's frustum. Both eyes then sample it with their
own per-eye reconstruction, so the lookups land offset by ±B in opposite
directions. Where an eye's lookup falls outside the volume — or into a froxel the
centre camera resolved as occluded — that eye gets no in-scatter at all.**

That predicts binary, per-eye, rotation-dependent, flipping, warm, directional,
absent-not-attenuated, correct-when-present, with the rest of the frame
identical. Every one of those is observed.

#### IN-SCATTER IS RE-OPENED

Take 177 "refuted" in-scatter (`X4VR_DISABLE_FOG=1`, "numbers unchanged to three
decimals"). That refutation used the band-ratio instrument which has since been
shown to measure the wrong model, sometimes on the wrong object, and its frames
were never checked for whether the Sun was in them. **It does not stand.** Do not
cite it again.

#### Next, and it is not a run

1. Confirm which pass writes img #34/#76/#92 and which modules sample them.
2. Then choose: build the volume per-eye (costly, and compute has no view
   index), build it once over a union frustum wide enough for both eyes, or
   **sample it with centre-frame positions in both eyes** — which makes the
   in-scatter mono. Mono is slightly wrong in depth but **symmetric**, and a
   symmetric diffuse glow is what comfort requires: the disparity is the harm,
   not the offset. A knob (`X4VR_INSCATTER_MONO`) would test it in one take.

### #49 localised: ten FRAGMENT shaders mix a per-eye frame with the centre frame

#### First, the instrument error that had to be cleared out of the way

X4's dumped modules carry **two OpEntryPoints — `(0,'main')` vertex and
`(4,'main')` fragment — in one SPIR-V module.** A whole-module scan attributes
every access to whichever entry point it happens to read. My first scan took the
first and reported "mod-0365 is Vertex"; my second kept the last and reported
"Fragment". **Two of my own instruments disagreed about the same fact**, which is
the tell that neither was measuring it.

This is recurring error class 3 verbatim: *whole-module scans on combined
modules* ([[x4vr-recurring-error-classes]]). The conclusion it produced —
"no fragment shader reads `M_projection`" — was **unfounded, not disproven**.

`stage_scan.py` now walks the call graph from each entry point and attributes an
access only if a function reachable from that entry contains it.

#### What that shows

| entry point | reads `M_projection` (member 1) | patched? |
|---|---|---|
| Vertex   | 22 modules | **yes** — `patch_vertex_eye_offset` |
| Fragment | **10 modules** | **NO — nothing patches it** |
| Compute  | 2 modules | cannot be (no `gl_ViewIndex`) |

All ten fragment readers of member 1 **also read `M_invprojection` (member 2)**,
which `X4VR_PROJ_INVPROJ` *does* patch per-eye. Six of the ten also sample a 3D
image:

```
mod-0111, 0112, 0167, 0168   member 1 + member 2, no 3D
mod-0365 .. 0370             member 1 + member 2 + 3D sample + I_maindepth
                             + V_light_direction_view + the V_volume_* set
mod-0364 (Compute)           member 1 + member 2 + V_light_direction_view
                             + V_volume_size, WRITES the 3D volume
```

**So inside one fragment shader, `M_invprojection` is in the per-eye frame and
`M_projection` is in X4's centre frame.** A position reconstructed with the
per-eye inverse and then re-projected with the centre-frame forward differs by
exactly the affine — `+B` in one eye, `−B` in the other. That is the opposite-sign,
scene-dependent, binary behaviour take 182 measured, on precisely the six modules
that do volumetric in-scatter.

This is [[changing-the-frame-changes-everything-in-it]]: `X4VR_PROJ_INVPROJ`
re-declared the frame for member 2 and did not enumerate what else was in it.

#### TWO CANDIDATE FIXES, AND THEY ARE OPPOSITES — do not guess

* **(a) Patch member 1 per-eye too**, matching member 2. Correct **if** the
  shader uses member 1 to project into the *current eye's* screen space (e.g. to
  fetch `I_maindepth` at a reprojected location).
* **(b) Stop patching member 2 in these modules**, so both stay in the centre
  frame. Correct **if** member 1 projects into the *volume's* space — the volume
  is written by `mod-0364`, a compute shader that cannot be per-eye, so the
  volume is necessarily in the centre frame and a centre-frame lookup is the
  consistent one.

They predict opposite edits, and no measurement so far distinguishes them.
**Settle it by reading X4's own GLSL** rather than by reasoning about SPIR-V:
`shadergl/` ships in the .cat/.dat ([[x4-ships-its-shader-sources]]); grep it for
the rarest property in the set — `V_volume_sigma_extinction` or
`I_volume_bind_index` — and read what the source does with `M_projection`.
That is a local, zero-run step and it is the next one.

#### Numbering caveat, now proven twice

The dump's `mod-NNNN` is creation order from the run that produced it and does
**not** line up with a later log's `module #N`: take 182 lists compute modules
#101/#130/#153, and `mod-0101`/`mod-0130`/`mod-0153` all have vertex entry
points. Pass serials drift the same way (take 179 vs 180). **Join dumps to logs
by content, never by number.**

### The GLSL read — #49's glow term named, and my own hypothesis refuted

Extracted `shadergl/` from the `.cat`/`.dat` again (587 files, **/tmp only, never
committed — Egosoft's copyright**). The parser reads each `.cat` line from the
RIGHT (`md5`, `timestamp`, `size`, then the path), because X4's paths contain
spaces.

`volume_apply.frag.glsl` is the in-scatter applier — the warm glow:

```glsl
RetrieveZBufferViewPos(view_pos);                  // -> UnProject -> M_invprojection
vec3 vdir = normalize(view_pos);
vec3 ldirs = normalize(V_light_direction_view[0].xyz);
float vdotl = dot(vdir, ldirs);                    // concentrates it on the Sun
...
float shad = GetDepthGodRay();                     // <-- THE GATE
color.rgb += shad * V_lightcolor1.rgb * Integrate_SimpleDirectional_VOLUME_NUM(vol.rgb, vdotl);
```

`vdotl` is why it is a halo around the Sun and nothing else; `V_lightcolor1` is
why it is warm.

#### MY MIXED-FRAME HYPOTHESIS IS REFUTED — by arithmetic, before any run

`common.glsl` defines the pair exactly:

```glsl
vec3 Project  (vec3 v) { vec4 t = M_projection    * vec4(v,1); return t.xyz/t.w; }  // member 1
vec3 UnProject(vec3 v) { vec4 t = M_invprojection * vec4(v,1); return t.xyz/t.w; }  // member 2
```

So `volume_apply` really does use the patched member 2 and the unpatched member
1, and `view2volume()` → `Project()` really is the volume lookup. But compose
them: the patched reconstruction is `T(d)·M_invproj·A⁻¹`, so

```
Project ∘ UnProject_patched = M_proj · T(d) · M_invproj · A⁻¹ = A⁻¹  (+ the T(d) term)
```

because `M_proj · M_invproj = I`. **`A⁻¹` is exactly the right mapping into a
centre-frame volume**, and the volume IS centre-frame — `volume_walk.comp.glsl`
builds it through the same `view2volume()`, in compute, which cannot be per-eye.
The only residue is the ±3.2 cm eye translation, which is a depth-dependent
shift of ~0.03/z radians — nothing at all at the Sun's distance.

So the mixed frame is **real but not the cause**. Recorded as refuted; it was
committed as a prediction and it did not survive contact with the source. It may
still matter for near-field content and is worth its own task, not this one.

#### The gate, and why it is ours to break

```glsl
float GetDepthGodRay() {
    float depth = saturate((GetViewPos().z - volume_fom_min) / volume_fom_max);
    return GetDepthGodRay(GetFragUV(), depth);
}
float GetDepthGodRay(vec2 uv, float depth) {
    float ret;
    FOM_GETF_2DARRAY(ret, PLS_GODRAYS, 4, uv, depth)   // <-- 2D ARRAY lookup
    ret /= 3;  ret = exp(-ret);  return saturate(ret);
}
```

`PLS_GODRAYS` is a **2D ARRAY** texture (Fourier Opacity Maps) sampled at
`GetFragUV()` with a depth slice, and the result goes through `exp(-ret)` and
`saturate`. That is a **binary-ish gate**: a large `ret` drives `shad` to 0 and
the whole in-scatter term with it — *absent, not attenuated*, which is exactly
what take 182 measured, and it depends on what occludes the Sun, which is why it
tracks rotation and flips between eyes.

**A 2D array sampled from a fragment shader is precisely what the bindless
index-offset patch redirects per view** (`X4VR_BINDLESS_MIRROR` /
`X4VR_BINDLESS_PATCH`; the run logs `366 modules edited, 9 REFUSED`). If view 1
is sent to a layer of `PLS_GODRAYS` that was never rendered — or to the wrong
descriptor — `shad` collapses for that eye alone.

#### Next, still no run needed

1. Find `PLS_GODRAYS`' image in the layer's inventory: is it doubled, and is its
   layer 1 ever rendered? The `LAYER 1 NEVER RENDERED` report added for take 180
   lists 5 of 29 — check whether the godray array is among the 39 doubled images
   that have **no tracked writers at all**, which has never been examined.
2. Check whether `volume_apply`'s fragment entry is one of the 366 modules the
   index-offset patch edited, and whether the offset is applied to *this*
   sampler.
3. Only then choose the fix. `X4VR_BINDLESS_PATCH=0` remains a vacuous control —
   it makes both eyes view 0 — so it cannot be the test.

### The two checks — the godray gate reads a screen-space buffer at per-eye UV

**Check 2 (the sampler).** `FOM_GETF_2DARRAY` resolves to a plain `sampler2D`
lookup, and the coordinate is

```glsl
vec2 GetFragUV() { return gl_FragCoord.xy / V_viewportpixelsize.xy; }
```

i.e. **the current eye's own screen UV**. So `GetDepthGodRay()` reads a
screen-space godray buffer at per-eye coordinates. That is correct **only if
that buffer is itself rendered per-eye**. If it is mono — rendered once in X4's
centre frame — then each eye reads it at its own UV, and the same world
direction sits at UVs differing by the affine: opposite directions, up to
±512 px. Whichever eye's lookup lands inside an occluder's godray footprint gets
`ret` large, `exp(-ret)` → 0, and **the entire in-scatter term vanishes for that
eye**. Absent, not attenuated; occlusion-driven, so rotation-dependent; and able
to flip. Every property take 182 measured.

Note this is a *different* failure from the volume lookup, and it does not
cancel the way that one did: there is no reconstruction here to undo the affine,
just a raw screen-space UV.

**Check 1 (the buffers), and a gap that has never been looked at.** Take 182:

```
67 doubled images, 30 with tracked writers, 45 with NONE
```

Forty-five doubled images have **no writer the layer ever observed**. The
`LAYER 1 NEVER RENDERED` report added after take 180 only ranges over the 30
tracked ones — so it reported "5 of 29" while saying nothing at all about these
45. Among them, at quarter and three-quarter resolution:

```
img #24/#25   1056x1056  fmt=97      <- quarter res HDR pair
img #26/#27   3168x3168  fmt=97
img #68/#69   3168x3168  fmt=97
img #42/#43/#44, #84/#85/#86, #102   4224x4224 fmt=76
```

A Fourier-opacity godray buffer is normally half or quarter resolution, which
makes #24/#25 the first candidates.

**This is the next concrete step**, and it is still not a run: extend the writer
tracking so it covers every doubled image rather than only those a recorded
render pass wrote, then re-read the report. "No tracked writer" currently
conflates *never written* with *written by something the tracker does not
watch* — and those are opposite diagnoses. Until that is separated, do not
conclude which.

### Writer tracking now covers every doubled image

`g_img_writers` only gained an entry when a **framebuffer named the image as an
attachment**, so take 182 tracked 30 of 67 doubled images and the report said
"5 of 29" while being silent about the other 37. Entries are now seeded at
`vkCreateImage` for every doubled image, and the report separates three cases
instead of two:

* `MIXED WRITERS` — masked and unmasked passes both write it.
* `LAYER 1 NEVER RENDERED` — every writer is a mono pass; layer 1 holds the
  clear value.
* `NO RENDER PASS WROTE IT` — **new.** Reports `usage`, and says explicitly
  whether `STORAGE` (a compute dispatch could write it) or `TRANSFER_DST` (a
  copy or clear could) is set.

The summary line now reads `N total, U written only by mono pass(es), P never
named by any framebuffer`, and states in the log itself that **P is a question,
not a defect** — compute and transfer writes are still not attributed per image,
so "no render pass wrote it" is proof that no framebuffer named it and nothing
more. Conflating that with "layer 1 is empty" is what made the old number
unreadable.

Exercised headlessly: `tests/multiview_render.cpp` now creates a doubled colour
image that no framebuffer ever names (`ORPHAN_DOUBLED`), and `orphan_case` in
`run-multiview-render.sh` asserts it is both reported and counted in the total.
It is created **last**, because creating it early consumed an image serial and
shifted the ones the transfer-inventory case asserts on — the probe must not
perturb the numbering it shares a run with.

### Take 183 — exactly what to extract (READ THIS FIRST, IT IS THE WHOLE STATE)

Run:

```
python3 tools/score_run.py /tmp/x4vr-take183.log
python3 tools/sun_halo.py --prefix=/tmp/x4vr-t183 --crop=/tmp/c
grep -a "NO RENDER PASS WROTE IT\|LAYER 1 NEVER RENDERED\|doubled images —" /tmp/x4vr-take183.log
```

**The question is one line: which image is X4's godray (FOM) buffer, and is it
rendered per-eye?** `GetDepthGodRay()` samples it at `GetFragUV()` —
`gl_FragCoord.xy / V_viewportpixelsize.xy`, the *current eye's* screen UV. That
is correct only if the buffer is rendered per-eye. If it is mono, the two eyes
read it at UVs differing by the full affine in opposite directions, `exp(-ret)`
collapses `shad` to 0 for whichever eye lands on an occluder, and the whole
in-scatter term vanishes for that eye — absent, not attenuated, and it flips.

**How to read the new report.** Take 182 had 67 doubled images and only 30 with
tracked writers; every doubled image now gets a line. Three verdicts:

* `MIXED WRITERS` — fine, both kinds write it.
* `LAYER 1 NEVER RENDERED` — every writer is a mono pass. **If the godray buffer
  is here, that is #49's root cause.**
* `NO RENDER PASS WROTE IT` — prints `usage`. `STORAGE` means a compute dispatch
  could have written it (compute has no `gl_ViewIndex`, so it is necessarily
  mono); `TRANSFER_DST` means a copy/clear could. **This is a QUESTION, not a
  defect** — it proves no framebuffer named the image and nothing more. Do not
  report it as "layer 1 is empty".

**Identifying the godray buffer.** A Fourier-opacity buffer is typically half or
quarter resolution and HDR. Take 182's unattached candidates were
`img #24/#25 1056x1056 fmt=97` (quarter res, a pair — first suspects),
`#26/#27` and `#68/#69` at `3168x3168 fmt=97`. **Confirm by content, not by
number** — image serials drift between runs exactly like pass serials.

**Already settled — do NOT re-litigate:**

* The glow term is `shad * V_lightcolor1 * Integrate_SimpleDirectional(vol,
  vdotl)` in `volume_apply.frag.glsl`; `vdotl` is why it hugs the Sun.
* **AA is exonerated** (take 181, identical with the upscaler off). `X4VR_AA`
  stays off the command line.
* `M_projection_uj` is read by **zero** modules.
* The mixed-frame idea is **refuted by arithmetic**: `Project ∘ UnProject_patched
  = A⁻¹`, which is the correct mapping into a centre-frame volume.
* `X4VR_BINDLESS_PATCH=0` and `X4VR_SHEAR_LIGHTS=0` are both **vacuous
  controls** — each makes both eyes view 0 or mono, so the Sun matches by
  construction. Four takes have already gone into learning this.
* Take 177's "in-scatter refuted" **does not stand** (discredited instrument).

**Reading `sun_halo.py`:** the deficient eye sits at warm excess ≈0.128, the
healthy one at 0.24–0.48, and *either* eye can be the deficient one. Rows saying
"no solar disc", "neutral in BOTH eyes" or "FLAT FRAME" mean there was no Sun —
they are not clean results. Look at the crop before believing any number.

---

## Take 183 — the godray hypothesis is dead, and I never checked which eye is right

**Knobs.** Same line as take 182 with `X4VR_TAKE=183-WRITERS`, plus one that is
*not* an environment variable: Patola set `<autoroll>false</autoroll>` in
`~/.config/EgoSoft/X4/96679891/config-x4vrmod.xml` (line 35) so X4 would stop
correcting his orientation. That is the mod's own forked config, so the
non-intrusive rule is intact — but it is part of this take's state, and a
re-run without it will roll differently. He rolled clockwise only, at a steady
rate, giving roughly even angular increments across the 19 present dumps.

### What the writer report answered

Not the question I asked it. There is no godray buffer in the list, because
**#49 is not godrays** (below). What it did find is a separate, real defect:

    img #70..#74   2048x2048x1  fmt=124 (D16_UNORM)  usage=0xa6  DOUBLED
    written by rp  #39, #41, #43, #45, #47 — each "MONO (depth-only/shadow)"
    -> LAYER 1 NEVER RENDERED

**Five cascaded shadow maps are doubled but written mono**; layer 1 holds only
the clear value. This is the shadow class Patola flagged from the third
attempt, where globally-applied shadows were the recurring killer. It is filed
as its own task. It is **not** #49's mechanism — a constant right-eye defect
cannot produce a flip — but it is live the moment anything samples layer 1.

The other 44 `NO RENDER PASS WROTE IT` lines remain a question, not a defect.

### Three retractions, measured from the take-183 dumps

**1. It is not godrays, and `PLS_GODRAYS`/`GetDepthGodRay()` is off the table.**
Within one frame the two eyes are the same instant, and at the Sun's distance
parallax is zero, so after centroid alignment the eye difference *is* the
component. Take 183 supplies two positives of opposite sign (n10 left carries
it, n16 right carries it) and two negatives (n12 both, n17 neither):

| frame | Δ peak, 20–40 px | warm excess of Δ |
|---|---|---|
| n10 (L carries) | **+0.176** | +0.058 |
| n16 (R carries) | **−0.265** | −0.071 |
| n12 (both)      | +0.007 | +0.001 |
| n17 (neither)   | −0.005 | −0.002 |

Controls sit ~35× below the positives and the sign flips. Screen-space light
shafts are radial streaks; this is not that.

**2. "Isotropic" was my instrument, not the data.** The first pass reported the
angular profile at the radius of peak *amplitude* — 20–40 px, hard against the
disc edge, the least anisotropic place in the field — and called it isotropic.
The crop plainly shows a diagonal plume. Measured across radii, at 40–84 px the
anisotropy is 0.10–0.49 with a consistent within-frame axis that **changes
between frames**: n09 −44°/−50°, n10 +59°/+60°, n16 +48°/+47°. The both-healthy
control is 50–100× lower with random directions. *Do not measure angular
structure at the radius of maximum amplitude.*

**3. The component is ABSENT, never displaced.** If a mono screen-space buffer
were being sampled at each eye's own UV, the component would still exist,
drawn ~1027 px away. It does not. Searching the whole deficient eye for warm
annuli: both predicted displacement sites read ≈0 or negative, and the
strongest warm annulus anywhere else is +0.06 at 130 px from the Sun — the
Sun's own tail — against +0.31 in the healthy eye. So the mechanism is a
**gate**, not a misread UV.

### What #49 actually is, as of take 183

A **warm directional plume**, amplitude ~0.10–0.14 linear luma at 40–56 px,
decaying to ~2% by 130 px, whose axis rotates frame to frame. Its gate is
**binary and independent per eye** — all four states occur (n09/n10/n11/n18 left
only, n15/n16 right only, n12/n13/n14 both, n17 neither). When both eyes have
it they agree to **1–2%**, so the rendering is correct and only the gate
misfires. Each eye's gate is a slow square wave over the roll, the two eyes
phase-shifted; period ≈ 10 dumps ≈ 100 s, consistent with one full roll.

**Also refuted:** distance from the eye's image centre does not predict the
gate. Deficient right-eye frames include both the closest Sun (714 px) and the
farthest (915 px); healthy ones sit inside that range. Any "the flare fades with
distance from screen centre" story is dead on the existing dumps.

**A constraint worth keeping.** The two eyes differ *only* by a 64 mm
translation and the frustum cant. At the Sun's distance the translation is
nothing. So whatever gates the plume must read **the projection**, or something
derived from it — the Sun's screen position, an NDC test, a clip-space compare.

### The gap: I never established which side is correct

Every line above calls the eye with the plume "healthy" and the other
"deficient", on no evidence beyond *more looked like better*. That is the exact
mistake that cost 13 takes when the vanilla run showed the brighter eye had
been the correct one all along. There is **no vanilla Sun baseline** anywhere —
`~/x4vr-baselines/` holds only take 65–70 frames.

**Prediction, committed before the check.** I expect vanilla X4 to show the warm
plume **continuously** while the Sun is in view, with its axis rotating as the
ship rolls — i.e. the plume is correct, the eye that loses it is the defective
one, and the gate is something our projection patch breaks. If instead the
plume comes and goes in vanilla on the same slow cadence, then neither eye is
defective, X4's own effect is orientation-gated, and #49 becomes "the two eyes
disagree about an orientation the ship holds only once" — a different fix.

### The vanilla reference — my prediction was wrong, and it reframes #49

Nine unmodded screenshots (`/tmp/scr3`, 3440x1440, cockpit, Sun in view, rolled
clockwise). **I predicted the warm plume would be present continuously. It is
not there at all.** Vanilla shows a compact symmetric warm glow, two horizontal
bars, and a small starburst — and nothing resembling the diagonal plume.

Measured with the annulus scaled to the disc radius (2–3R), because vanilla is
a different resolution and FOV from a VR eye and fixed pixel radii would not
compare:

| | core R | warm excess (2–3R) |
|---|---|---|
| vanilla, 9 orientations | 13–16 px | **0.159–0.211** — stable to ±13% |
| n16 layer0 — no plume | 19 px | 0.122 |
| n16 layer1 — has plume | **29 px** | 0.158 |
| n10 layer1 — no plume | 17 px | 0.119 |
| n10 layer0 — has plume | 23 px | 0.218 |

**Neither eye matches vanilla.** The plume eye has a plume vanilla never shows.
The no-plume eye sits *below* vanilla's warm level and carries a dense fine
radial spike pattern vanilla has not got. And at one instant our two eyes
disagree about the saturated disc radius by 1.5x — 19 px against 29 px, same
Sun, same moment.

So #49 is **not** "one eye loses a component". It is **two different wrong
answers**, and the earlier framing — including this file's own "the deficient
eye sits at 0.128, the healthy one at 0.24–0.48" — had the polarity question
backwards by never asking it.

**Caveat, stated so nobody later treats these as commensurable:** vanilla is a
different scene (its Sun is smaller, R≈15 against our 17–29) and the shots are
JPEG. Absolute photometry does not cross from vanilla to a VR dump. The
morphology does, and the morphology is unambiguous.

### Patola's observation, which names the mechanism class

> "even when I am rolling the ship, some horizontal projections from the Sun
> remain horizontal. And the Sun rays roll whenever I pitch up or down or yaw
> left or right."

The horizontal bars are **screen-locked** — they do not rotate when the world
does. The other flare elements move when the Sun's position in the frame moves.
The nine vanilla crops show the same thing: the starburst wanders around the
disc from frame to frame. **X4's flare/glare geometry is a function of where
the Sun sits in the rendered image.**

That is exactly the constraint take 183 arrived at from the other direction:
the eyes differ only by 64 mm and the frustum cant, and at the Sun's distance
the translation is nothing, so whatever drives the difference must read the
projection. It does — through the Sun's screen position, which our ±0.2425 NDC
cant moves by **±512 px in opposite directions**. Each eye then computes flare
geometry for a Sun that is a quarter of a frame away from where the other eye
has it, and neither position is the one vanilla uses.

### Take 184 — the discriminating run, and what it must show

`X4VR_OFFAXIS="-55,55,55,-55"` is arithmetically the identity affine
(`layer/x4vr_layer.cpp:3760`), so both eyes render a symmetric frustum and the
Sun lands at the **same pixel** in both. The IPD is passed to
`patch_vertex_eye_offset` independently of the off-axis arguments
(`x4vr_layer.cpp:4609–4621`), so the eyes still differ by ±32 mm: this is
**not** one of the vacuous controls. Non-vacuity is checked from the run's own
scorer output — the present dumps must still show parallax, not come back
bit-identical.

**Predictions, committed before the run:**

1. The Sun looks the **same in both eyes** — `eye_component.py` reports BOTH
   EYES AGREE on every frame with a Sun in it.
2. Both eyes look like **vanilla**: no diagonal plume, no dense spike pattern,
   warm excess at 2–3R near 0.17, and the two eyes' core radii within ~10% of
   each other instead of 1.5x apart.
3. Near-field parallax is still present — cockpit and HUD differ between eyes.

If 1 holds but 2 fails, the flare depends on screen position but our symmetric
frustum is still not X4's. If 1 fails, the screen-position story is wrong and
something per-eye that is not the Sun's position is driving it.

**Instrument note.** Both `sun_halo.py` and `eye_component.py` had the −1024 px
infinity offset as a constant. At zero cant that offset is 0, and the default
would have reported every real frame as "NOT THE SAME OBJECT" and probed two
arbitrary points for a displaced copy. Both now take `--dx`, and
`eye_component.py` refuses the displacement probe at `--dx=0` instead of
testing the anchor against itself.

### Take 184 — all three predictions confirmed; the mechanism is settled

| prediction | result |
|---|---|
| both eyes agree on every Sun frame | **yes** — warm-excess ratios 1.00 across all 13 Sun frames; `eye_component.py` reports BOTH EYES AGREE at 0–1% |
| core radii within ~10% instead of 1.5× | **yes, and far better** — R/L 0.996–1.004 |
| near-field parallax survives | **yes** — 3 frames with matched tiles shifting up to 9 px, and the Sun carries a genuine 0.6–0.9 px sub-pixel disparity |

The Sun's own sub-pixel disparity is worth keeping: at zero cant, content at
true infinity would land at **exactly** the same pixel. 0.6–0.9 px says X4's
sun is far but **finite**, which agrees with the skybox note — its planets are
reachable geometry, not infinity. It is also what proves the run was not
vacuous: the eyes still differ.

**#49's mechanism is established.** X4 computes flare/glare geometry from the
Sun's position *within the rendered image*. The off-axis cant moves that
position by ±512 px in opposite directions per eye, so each eye computed flare
for a Sun a quarter-frame from the other's. Remove the divergence and the
difference vanishes completely — not "improves", vanishes to 0–1%.

**A second defect the cant was causing.** Patola: the static load screen is no
longer cross-eyed. That is consistent with take 183's own data, where the flat
frames aligned at the **infinity offset** — the 2D artwork was carrying the
full eye offset. Screen-locked canvas content should sit at a chosen depth, not
be displaced as though it were at infinity. Filed separately; it is a #40 gap
that only the cant exposes.

**So the cant breaks an unbounded class**, not two things. Anything X4 keys to
the image centre — flare, ghosts, vignette, lens dirt, any centre-weighted
post-process — diverges per eye under it, and we cannot enumerate them from
outside.

### What the cant is worth, in numbers

Both configurations render 4224² per eye; `X4VR_RES` fixes the extent, so the
cant buys no pixels — it decides *where content lands inside the same buffer*.
Against take 183's declared target (`-54,40,44,-55` for eye 0):

    canted tangent span     H = 2.2155   V = 2.3938
    symmetric +/-55 span        2.8563
    fraction used           H = 0.776    V = 0.838   -> area 0.650

So a symmetric render spends **1.54× the pixels** for the same headset view, or
equivalently the cant gives **1.24× linear resolution**. That is the price of
turning it off, and it is not nothing. (The tangent-space centre of that target
comes out at −15.04°, exactly what the scorer reports, so the arithmetic is
sound.)

Frame time does not decide it: 33.40 ms at identity against 33.65 ms canted —
the same number.

**This is a design decision on #35, not a bug fix, and it is Patola's to make.**
Option A: drop the cant, render symmetric, declare symmetric — closes the whole
class, costs 1.24× linear resolution. Option B: keep the cant and repair each
screen-position-dependent effect — preserves resolution, but the set is not
enumerable from outside X4 and every member is a per-eye rivalry defect.
Option C: reduce the cant — trades the two continuously, and is only attractive
if the flare gate turns out to be threshold-like rather than continuous, which
is not yet known.

### #51 was not a defect, and the report is why I thought it was

I recommended refusing to double depth-only shadow targets, on the grounds that
`LAYER 1 NEVER RENDERED` plus 636 applications of the bindless index offset made
"nothing samples layer 1" an assumption we were not entitled to. **That was
wrong, and the change would have risked a validation error to fix nothing.**

`layer1_state()` tests the *same predicate* on the *same* `g_img_writers` entry
the report prints — `masked.empty() && !unmasked.empty()` — and returns
`Shared`, on which the mirror **keeps the twin descriptor at layer 0** so view 1
samples the mono content. Take 184's own accounting says so out loud: `44928
kept at layer 0 as shared (unmasked writers only)`. The guard's comment records
why it exists:

> only unmasked passes write it, so its layer 1 is empty and substituting it is
> how the right eye lost its shadows

So this was found and fixed before take 73. The five cascades cost ~40 MB and
nothing else — exactly what `mv_double_candidate()`'s comment already said, and
that comment was right to be permissive: doubled-and-unused costs memory, while
per-eye-rendered-and-not-doubled is a hard validation error at
`vkCreateFramebuffer`.

**The lesson is about the report, not the shadows.** A line that names a risk
without naming the guard that already covers it will generate work. Fixed:

* `LAYER 1 NEVER RENDERED` now ends "NOT a defect: `layer1_state()` reads it as
  Shared and the mirror keeps view 1 on layer 0. Costs memory only" — and if
  `X4VR_MIRROR_ALL_LAYER1` is set, which defeats that guard, it says so instead.
* The summary line named its counts rather than numbering them. "The second
  number is a QUESTION" was ambiguous between the two counts after `total`, and
  the sentence explaining it fitted the other one.

### The cant warning

A canted target now warns at latch, naming `|B_x|` and the divergence as a
percentage of image width — 24.3% for the take-183 target, against a measured
1024/4224 = 24.24%. It warns rather than refuses: the cant is worth 1.24x
linear resolution and may be wanted again once the render extent shrinks to
match it (#43). What it must not do is switch on silently.

Both directions are locked in `tests/run-multiview-render.sh`: `cant: canted
target warns` and `cant: identity stays silent`. The second matters as much as
the first — the identity control is an everyday configuration and an instrument
that cries wolf on it would be turned off.

---

## X4's renderer, read from its own sources — and five corrections to this file

Brainstorming pass, 2026-08-22, before #33 starts. No take. Everything below
comes from three sources already on disk: X4's own GLSL (`/tmp/x4-shadersrc`,
587 files, Egosoft's copyright, **never committed**), the 409 dumped SPIR-V
modules with their `OpMemberName` debug info intact, and the 190 stored run
logs. The point of doing it now is that #33 is the first task whose design
depends on what X4's renderer *is* rather than on what one pass does.

Two new tools carry the measurements so they can be re-run rather than quoted:
`tools/uniform_block_scan.py` and `tools/headlook_clip.py --self-check`.

### What X4's renderer is, in seven decisions

1. **One GLSL source tree, `#include`-composed, specialised by `#define`.**
   `shadergl/glsl/*.glsl` plus 290 `shadergl/ogl/*.xml` pass descriptions that
   name a vertex and a fragment file and declare material properties, with
   `dynamic="true"` marking the ones the engine writes. There are OSX comments,
   an `nvidia` pragma block, `#define mul(a,b) ((b)*(a))` and a
   `simple_dx9offset.vert.glsl`: this is a long-lived multi-backend engine, and
   Vulkan is one of its outputs. **Consequence: the module population is a
   property of the configuration, not of X4.** "409 modules" is what our
   overrides compile.

2. **Camera-relative world space.** Object transforms are baked against the
   camera on the CPU, `M_view` is (mostly — see correction 2) identity, and so
   "world" space *is* view space. There is no world frame in the shaders to
   rotate. This is the precision decision a space sim has to make, and it is
   why every space that matters to us is the camera's.

3. **Reversed-Z with an infinite far plane.** Measured every run:
   `x_c = sx·x_v, y_c = sy·y_v, z_c = near, w_c = z_v`, i.e. `m[10] == 0`.
   X4 says it itself in `hiddenareamask.vert.glsl`: *"since we have an inverse
   z-buffer, 1.0 is closest and 0.0 is furthest"*. `BGDIST` is 2 000 000 and
   `BGZ` is its reciprocal. This one fact is why the eye shear collapses to a
   single scalar — and why the head rotation does not.

4. **Bindless everything.** One descriptor set of `GFX_BINDLESS_COUNT` texture
   arrays, indexed by `uint` out of the uniform blocks. Already known from
   take 20; the source confirms it is the design and not an artifact.

5. **Blocks are organised by lifetime**, and X4 says so in the names:
   `SLOT_CAMERA` (per view), `SLOT_WORLD` (per object), `SLOT_DYNAMIC` (per
   draw), `SLOT_MATERIAL`, `SLOT_GENERIC_STORAGE`. Per-eye is per-**view**, so
   the camera block is the correct home for our corrections, and it is the one
   X4 keeps 128 of per frame.

6. **A jittered/unjittered matrix pair for everything temporal**, plus
   `M_prevworldviewprojection`, `M_prevboneworld`, `mv_fill` motion vectors,
   `taa_mva`, FSR3 and a `dlssg_ui_verify` pass. The renderer is *built* for
   temporal upscaling. Our config switches all of it off, which is why the
   dumps read zero on those members.

7. **Screen space is a first-class space, with exactly two doors.**
   `GetFragUV() = gl_FragCoord.xy / V_viewportpixelsize.xy`, then
   `GetViewPos()` through `M_invprojection` and `GetViewPosUJShad()` through
   `M_invprojection_uj`. Every reconstruction in the engine goes through one of
   those two members. **The layer already owns both** (#22, #39). That is the
   single most reassuring fact for #33.

And one that is not a decision but a discovery: **`hiddenareamask.xml` exists,
with a real vertex and fragment shader** that draws a mesh at the near plane so
everything behind it is rejected. That is the VR lens-mask optimisation by name
and by implementation. It does **not** reopen the OpenTrack question or make X4
a VR title — no module in the dumps is it, and
[the VR-residue rule](#x4s-vr-api-is-a-stubbed-surface--names-only-no-implementation)
stands — but it is a working shader rather than a dangling symbol, and that is
a different category from the stubs. Worth knowing it is there before someone
rediscovers it and over-reads it.

### Correction 1 — the camera block has 72 members, and nobody had counted the readers

**Provenance first, because I nearly overclaimed this.** `frame-analysis.md`'s
Phase-2 section "Per-view frame-constants UBO" already tabulates the **first 11
matrices** with float offsets, from a RenderDoc capture — `M_jitter`,
`M_prevjitter`, `M_viewprojection`, `M_viewinverse` and the two CSM clip
matrices included. That table is right and it is a year of this project older
than I assumed. What was missing is everything after member 11 (61 more
members), and — the part that actually decides work — **how many shader stages
read each one**.

`tools/uniform_block_scan.py`, over the 409 dumps, attributed per entry point.
The full table is in the tool's output; the part that matters:

    mem  name                    Compute  Fragment   Vertex
      0  M_view                        0       160      262
      1  M_projection                  2        10       22
      2  M_invprojection               3       244        0
      3  M_projection_uj               0         0        0
      4  M_invprojection_uj            0         2        0
      5  M_jitter                      0         0        0
      6  M_prevjitter                  0         0        0
      7  M_viewprojection              0         0        2
      8  M_viewinverse                 0        16        0
     11  V_viewportpixelsize           0       252       18
     12  V_screenresolution            0         0        0
     13  V_cameraposition              0        86      296

Three things in there were not known:

* **`M_viewprojection` (7) is a real path.** `GetInstanceData()` in
  `common_vert.glsl` builds `wvp = M_viewprojection * inst.world` in the
  *shader* for instanced draws, rather than taking the per-object
  `M_worldviewprojection`. Two vertex entry points do it. This is the family
  #48 was about; it is now named rather than inferred.
* **`V_viewportpixelsize` (11) is read by 18 VERTEX entry points.** `ui.vert`
  is one of them (a half-texel offset). Anything that changes the render extent
  — #43 — moves vertex output, not just fragment sampling. That is a lead for
  #45 as well.
* **`M_view` (0) is the most-read matrix in the block.** 262 vertex + 160
  fragment entry points. It is identity, so they are no-ops — which is exactly
  what makes the next correction matter.

**And the two documents disagree with each other, which is worse than either
being wrong.** `frame-analysis.md`'s Phase-2 table marks `M_view`,
`M_viewprojection` and `M_viewinverse` **PATCH** — that was the plan before any
of it ran. Section 4 of *this* file says writing them "has no visual effect at
all". A reader who opens one gets the opposite instruction from the other, and
both were written with confidence. Correction 2 lands between them: the Phase-2
plan could not have worked, because those matrices are identity for nearly
every camera, and section 4's blanket is not safe either. `frame-analysis.md`
now carries a pointer here rather than being edited — the wrong turn stays
where it was taken.

### Correction 2 — `M_view` is NOT always identity

This file has said since section 4: *"Surveyed all 128 blocks of the view
arena: `M_view` is identity in every one"*, and drew from it that writing it has
"no visual effect at all". The survey was one instant of one run. Of the 64
runs that logged the matrix dump, **53 print exact identity, 2 differ only in
the fourth decimal, and 9 print a real view transform**:

    M_view  [ 0.966  0  0.258  0 | 0  1  0  0 | -0.258  0  0.966  0 | 8.48  0  2.26  1 ]

A yaw of about 15° and an 8.70 m offset, in takes 155, 161, 165a, 166b, 167a,
171, 172, 175b and 178 — nine independent runs spanning five weeks, agreeing to
three decimals. That is not a transient. (The log prints three decimals, so
decoding the printed matrix recovers 14.76° from take 178's row and 14.98° from
the other eight; the underlying angle is 15°, and `tests/view_math.cpp` pins
the decoder against take 178's sixteen floats as logged rather than against a
synthetic rotation.) Take 178's dump shows the
consequence directly: its `M_viewprojection` is genuinely `P·M_view`, with the
w row reading `0.256·x + 0.967·z + 2.225` instead of plain `z`.

**What survives.** The shear and the rotation are derived from
`z_c == near` and `w_c == z_view`, and *both still hold* for such a camera —
`P·M_view` still puts that camera's own view z into w. The math is safe.

**What does not.** "Writing `M_view` has no visual effect" is false for
whatever this camera draws, and 422 entry-point reads are waiting for it.
`world2view()` and `view2world()` are not no-ops there. Anyone tempted to
commandeer member 0 as a per-frame channel — it is the widest one in the block
and it currently carries nothing — must know that this camera exists first.

**Why the instrument could not have said so.** `x4vr_layer.cpp:5395` picks the
block with the *most draws* and prints its matrices **once per run**, and the
comment right above it states the belief as a reason:

    // Prefer the projection matrix: X4 renders camera-relative, so its
    // M_view is identity and cannot disambiguate the storage order.

The same file already fixed exactly this defect for `sx` — `proj CAMERA cam#N`
reports **every** credited block, added after the "one number, several cameras"
mistake — and the matrix dump beside it was left on one block. Same defect
class, same file, one field over. **Before #33: report `M_view` per credited
camera and flag any non-identity one.** It costs nothing at runtime; the block
is already being read.

Not established, and deliberately not guessed at: *which* camera this is. The
value is fixed across menu-phase and gameplay-phase samples alike, which rules
out a scene camera following the player. A cockpit monitor, an environment
probe and a second viewport all fit. The instrument above answers it as a side
effect, so it does not need answering now.

### Correction 3 — "`M_projection_uj` is read by zero modules" is about our config

True of the dumps, and this file has used it as a fact about X4. It is not.
`ProjectUJ()` has exactly two callers in X4's GLSL — `ssao_v2.frag.glsl` and
`ssr.frag.glsl` — and the injector forces `ssao=0` and `ssr=false`. Member 3 is
empty **because we emptied it**. Same for `M_jitter`/`M_prevjitter` (5, 6),
empty because `antialiasing=none`, and for `M_prevworldviewprojection`
(set 3 member 2), which has zero readers because nothing temporal is compiled.

Every zero in correction 1's table carries that caveat. A run that enables FSR3
— and take 163 proved FSR3 works — repopulates three of them at once.

### Correction 4 — "uj" is un-jittered, and today members 2 and 4 hold the *same* matrix

`M_jitter`/`M_prevjitter` sit next to them in the block and
`TAA_GetJitterClip()` reads `M_jitter[3].xy`; `GetViewPosUJShad()` subtracts
the jitter before unprojecting. So `_uj` is *un-jittered*, and its purpose is
to keep TAA's sub-pixel offset out of shadow lookups, SSR and SSAO.

With `antialiasing=none` the jitter is identity, and take 184's own dump shows
`M_projection` and `M_projectionUJ` printed identically. **So take 83's finding
was never about jitter.** Members 2 and 4 hold the same numbers in X4; they
differ only *after our patch*, and the defect was that we corrected one copy of
a matrix and left the other. Stating it that way matters, because the current
phrasing invites "we do not use TAA, so member 4 cannot matter" — which is the
opposite of true.

It also names a coupling worth having in writing: **enabling FSR3 and enabling
head rotation are the same risk taken twice.** Jitter lands in `m[8]/m[9]`,
which the injector's own comment already flags as "the eye shear's own slots",
and it wakes the temporal reprojection that a per-frame head rotation
desynchronises. Do not turn both on in one take.

### Correction 5 — set 2's member indices are not comparable between modules

`MACRO_BUFFER_MATERIAL` expands per shader family, so set 2 binding 0 has a
different layout in each module. The scan shows `S_diffuse_detail_idx` at
members 31, 37 and 55 in different modules. Any cross-module reasoning about a
set-2 index is invalid by construction. Sets 1, 3 and 4 are shared and safe.

### What this says about #33, worst-to-learn-late first

**1. The head rotation is exact, and it is NOT a constant matrix.**
`tools/headlook_clip.py --self-check` proves both offline, in a second:

    M = P·R·P⁻¹ agrees with rendering at view R·V to 5e-11 NDC, at every
    depth, over six measured cameras — that is the whole case for this route

    M[0][3] = sx·sin(yaw)     M[3][0] = −sin(yaw)/sx     M[0][0] = cos(yaw)

Six of sixteen entries carry `sx` or `sy`, and they are precisely the ones that
couple the rotation through w — the ones that do the rotating. The free entries
hold only `cos`. Baking `M` at the menu camera's `sx = 1.33333` and running it
at the shipped `X4VR_FOV=1.437` camera (`sx = 0.75405`) is **72 px wrong on a
1408 px eye at 10° of yaw**; at the zoom stop it is 2845 px; at the `fov 2.21`
camera the guard band wants, it drives `w` through zero and geometry inverts
through the eye point.

The decision section above predicted "the general MVP path plus the 12
baked-sx modules will both need the rotation term, exactly as the shear did".
That was right about *coverage* and understates the problem: **the third site,
`patch_vertex_clip`, cannot carry a correct rotation at all**, because its
`float k[16]` is frozen at `vkCreateShaderModule`, twenty-six seconds before X4
has a camera. For the shear a stale `sx` was a wrong magnitude. For the
rotation it is wrong geometry.

This is not an obstacle, it is a routing rule, and it happens to be the routing
we want anyway:

    patch_vertex_eye_offset      loads M_projection[0][0]/[1][1] live  -> rotate
    patch_vertex_eye_offset_mvp  recovers them from M_worldviewprojection -> rotate
    patch_vertex_clip            baked k[16], carries the non-World modules
                                 -> do NOT rotate; this is #30's canvas

**2. The per-frame delivery problem, which nothing in the layer solves yet.**
Every correction the layer applies today is a *constant*: the eye offset is our
choice, the off-axis affine is latched once, and `patch_fragment_invproj_eye`'s
own comment says the quiet part — *"Both are constant compositions on a matrix
that is already being rewritten, so this adds no load and no descriptor."* A
head pose is none of those things. Options, none of them costed yet:

* extend the pipeline layout at `vkCreatePipelineLayout` with a layer-owned set
  and bind it alongside X4's;
* `vkCmdUpdateBuffer` into X4's camera block before the pass, using a member
  nothing reads — member 3 is empty, but correction 3 says that is a property
  of our config, not a guarantee;
* push constants, which X4's layouts do not declare.

Deciding this is the first real design step of #33, and it should be decided
before any shader work, because it determines what the shader can read.

**3. `R` is eye-independent, and that buys something per-eye work cannot.**
The IPD and the cant differ between the eyes; the head *orientation* does not.
So the head rotation can reach places `gl_ViewIndex` cannot — notably the three
compute modules that read `M_invprojection`, and the froxel volume fill, which
`patch_fragment_invproj_eye` records as "parametrised by X4's SYMMETRIC frustum
for ever". Mono-forever and rotation-wrong are separate problems, and only one
of them is structural.

**4. The fragment correction generalises unchanged, and that is the good news.**
The invariant that makes #39 work — *reconstruct back into X4's own view space,
then everything X4 computed in its own frame stays consistent* — is exactly the
invariant a rotation needs. Compose `R⁻¹` onto members 2 and 4 beside the
affine and the shear, and the froxel lookup, the CSM lookup and the deferred
reconstruction all keep working, with member 1 correctly left alone for the
same reason it is left alone now. What changes is only *when* the value arrives,
which is point 2.

**5. The declared pose must be the pose we rotated by.** The layer already gets
this right for the servo, and the reason is written at `x4vr_layer.cpp:8700`:
declare the injector's *command*, not the live head pose, so the compositor
reprojects a frame that matches. Under a vertex rotation the same rule reads
**declared = X4's camera pose ⊕ our `R`**. Getting it wrong has two symmetric
failure modes that look nothing alike — omitting `R` leaves the compositor
correcting motion we already applied (the world swims at double rate), and
double-counting it locks the world to the head. Both are one line.

**6. The HUD splits into two classes and X4 draws them in one pass.** This is
the hardest open problem in #33 and it is not the canvas. A screen-locked panel
that follows the head is a legitimate helmet-mounted display; a screen-locked
*aiming reticle* is a functional break, because it would mark wherever you look
instead of where the guns point. X4 puts menus, cockpit HUD and map in one
reference frame (section 4), so no pass-level predicate separates them.

Take 162 already measured the split for two elements and the method is free:
our shear displaces by inverse depth, so per-eye disparity in any SBS frame
separates screen-space (0 px) from world geometry. Re-running that grid over
`/tmp/x4-162b.jpg` reproduces it — a broad 0 px band through the middle of the
frame against −20 to −60 px of cockpit around it — and the centre tile reads
−8 px at a correlation too low to call, which is what a reticle overlaid on
world geometry would look like and also what a badly chosen tile would look
like. **So: undecided, method known, cost zero.** Settle it from a frame we
already have before designing around either answer.

The structural answer may be better than a per-element transform anyway.
`GetMonitorExtents` returns HUD panel positions *in worldspace* (section
above), and take 162 proved `separateRadar=0` moves the radar into world
geometry, where it follows the rotation for free. The more of the HUD that
lives in the cockpit, the less of this problem exists.

**7. A cost of the guard band nobody has priced: shadow resolution.**
Prediction, before any measurement. X4's cascades are selected by view-space
distance (`V_csmthresholds`, `F_shadowmaxdistance` "set according to
videoparameters") and rendered into fixed 2048×2048 D16 maps — ten of them in
take 184's inventory. The map size comes from the video settings; the *volume*
each cascade must cover comes from the frustum. Widening `<fov>` from 1.4917
(110°) to 2.21 (163°) for head-rotation range therefore multiplies each
cascade's footprint by roughly the tangent-area ratio and divides its texel
density by the same, **across the whole frame, including the part the player is
actually looking at.**

The falsifier is a 2048 that is not 2048: if X4 sizes its cascades from the
field rather than from the video setting, the map dimensions move with
`X4VR_FOV` and this prediction is dead. Take 158 ran at `fov 2.21` but without
`X4VR_MV_INVENTORY`, so its image list is absent and the comparison cannot be
made from stored logs — the cheapest test is one flatscreen take at 2.21 with
the inventory on, read against take 184's ten 2048×2048 D16 entries.

This matters more than a pixel count. The memory of the previous X4 VR attempt
is that globally-applied shadows were its recurring problem, and this is the
mechanism by which a VR-only setting degrades shadows globally. It also puts a
second term in the guard-band table, which currently prices range in render
area alone.

### What did NOT change

The froxel volume is still mono forever, and this pass found nothing that
changes it. The `M_view` correction does not touch the shear or the affine.
`X4VR_OFFAXIS` stays at identity and #43 stays the blocker for the cant. No
finding here contradicts `stage16-binocular-clean`; the tag reproduces exactly
as recorded.

### #53 done — `M_view` is decoded per camera, and what a healthy run reads

The instrument, not a run. `X4VR_DUMP_MATRICES=1` now emits one line per
credited camera block beside the existing `proj CAMERA cam#N`, with the same
`cam#N` numbering so the two join:

    proj VIEW cam#0: M_view IDENTITY -- camera-relative, world space IS view
                     space (draws=60, 60 in view-masked passes)
    proj VIEW cam#1: M_view is NOT IDENTITY -- 14.76 deg about
                     (+0.000,-1.000,+0.000), offset 8.704 m (draws=10, 10 in
                     view-masked passes). ... -- and it DRAWS INTO VIEW-MASKED
                     PASSES, so our per-eye transforms reach it

**What a healthy run reads**, stated before any run is scored against it: one
`proj VIEW` line per `proj CAMERA` line, all of them `IDENTITY`, and
`score_run.py` printing `view  N slot(s) decoded, 0 with a non-identity
M_view`. Same caveat the sx report already carries — **`cam#N` is a UBO slot,
not a camera**; take 184 credited 126 slots carrying 3 distinct projections, so
the scorer collapses the non-identity ones to distinct (angle, offset) pairs.

**The identity lines are not noise, they are the control.** A report that only
spoke up on the interesting case would make "nothing found" and "nothing
looked" the same silence, which is the failure this whole task is about. For
the same reason the scorer prints a loud note rather than a clean line when a
log has no `proj VIEW` at all — every log this project has kept predates this
and cannot carry one.

**The masked column is the question the report exists to answer.** A camera
whose `M_view` is not identity only concerns us if our per-eye transforms ride
on it, and that is `draws in view-masked passes`. Wording it carefully matters:
this is **not** a known defect. The shear and the affine are derived from
`z_c == near` and `w_c == view z`, and *both still hold* for such a camera,
because `P·M_view` puts that camera's own view z into w. Take 178's dump shows
exactly that. What is not established is whether transforming a camera that is
not the player's eye is what we want.

**Driven headless before it can cost a take.** `X4VR_TEST_VIEW_ARENA=1` makes
`tests/multiview_render.cpp` build two range-1792 blocks at set 1 — block 0
identity with 60 draws, block 1 carrying take 178's logged matrix with 10 — and
the suite asserts both paths report. The asymmetry is the test: 10 draws never
wins the frame, and reading only the winner is precisely how this sat unseen in
9 stored runs. Validation-clean, checked by running the harness with and
without the flag and diffing the VUID set.

> That check originally read "four pre-existing, none added", and the phrase is
> the problem rather than the count. Diffing against a non-empty baseline passes
> whenever a new error shares an ID with an old one, and it normalises leaving
> errors in place.
>
> **Scoped precisely, because the first rewrite of this note overclaimed.** The
> configurations the suite *draws* are empty, and `run-multiview-render.sh` now
> asserts that across seven of them. What is **not** clean is drawing
> `sample_two`, `sample_bindless`, `sample_invproj` or `sample_alias_binding` —
> each still raises `VkGraphicsPipelineCreateInfo-layout-07988`, because they
> declare bindings the harness's pipeline layout does not. That is harmless
> today for one reason worth writing down: those four are **never drawn**. They
> reach `patch_case` / `idx_case` / `inv_case`, which run the SPIR-V CLI and
> `spirv-val` and never create a device. If one of them is ever handed to
> `frag_case` or `mirror_case`, it will need a matching layout first.

`tests/view_math.cpp` pins the decoder against take 178's sixteen floats **as
logged**, including that reading them with the wrong storage order silently
loses the translation — a camera that is rotated but not displaced, which is a
wrong answer shaped like a finding.

**Cost.** The masked-draw tally is gated on `X4VR_DUMP_MATRICES`, the same
variable its only consumer is gated on. `credit_draw()` runs on every draw call
X4 makes and already pays one mutex; a run that asks for matrices pays a second
one, and every other run pays a branch on a static.

**Still open, deliberately:** which camera the 15° one is. A cockpit monitor, an
environment probe and a second viewport all fit, the value is identical in
menu-phase and gameplay-phase samples alike, and the next dump run answers it
as a side effect. Not worth guessing at now.

### #55 — the reticle is flat, and take 162's radar finding was the panel behind it

No run. `tools/hud_frame.py` over `/tmp/x4-162b.jpg`, the SBS frame take 162
already produced. Our shear displaces by inverse depth, so disparity within one
frame says which frame an element was drawn in.

    region      channel    dx     ncc  margin
    reticle     hud        +0   0.944   0.551     flat
    radar       hud        +0   0.910   0.590     flat
    radar       grey      -29   0.593   0.269     world geometry
    ticker      hud        +0   0.995   0.558     flat
    console     luma      -43   0.772   0.052     world geometry
    stars       luma       +0   0.980   0.227     flat (infinity)

**The aiming reticle carries zero per-eye disparity** — the four green corner
brackets, the green triangle and the cyan aim circle all land at +0 while the
cockpit around them lands at −42/−43. It is not cockpit-depth geometry. An
anaglyph of the same window shows it directly: the brackets come out sharp and
single, every strut behind them is doubled red/cyan.

**What that does NOT settle, and it is the half #33 needs.** 0 px is what
screen-locked content reads *and* what world geometry at infinity reads — the
starfield in the same window reads +0 too. Under a head rotation those diverge:
screen-locked follows the head (a reticle that marks wherever you look instead
of where the guns point), world-at-infinity rotates correctly. **This method
cannot separate them**, and neither can a canted take, because #40 deliberately
makes the canvas agree with infinity. What separates them is a canvas at a
*finite* distance — `X4VR_CANVAS_M=2`, which moves screen-space content and
leaves world alone. Filed rather than guessed.

#### RETRACTED: "take 162 — `separateRadar=0` makes the radar WORLD geometry"

That section says "Measured, not eyeballed", and the measurement was
`radar (right side) disparity -30 px`. Splitting the same window by chroma
gives **hud +0, grey −29**: the −30 is the **cockpit panel the radar is drawn
on**, not the radar. Four overlay-free control windows around it read −32, −40,
−65 and the overlay core reads +0 with the sharpest margin in the whole frame
(0.876). The anaglyph is unambiguous — the blue radar arcs and crosshair are
single, the panel behind them is split in two.

So the four consequences that section drew do not follow:

* ~~the radar **already has correct stereo**~~ — it has none; it is flat
* ~~it would follow #33's vertex-stage rotation for free~~ — it will not
* ~~it needs **none** of #30's canvas~~ — it needs the canvas like the ticker
* its backdrop is the console, so the wide-FOV washout cannot recur on it —
  **this one survives**, and it was always the argument that did not depend on
  the disparity number

The decision that section fed into is unaffected: `separateRadar` stays unset
by default because it is a preference, not a stereo-correctness fix. What
changes is the planning claim underneath it — routing the radar to the target
monitor does **not** remove canvas work, so "supporting both modes means the
canvas work still has to happen" is now true for a second reason.

**How the error happened, because it is a class and not an accident.** A thin
cyan overlay lying on textured geometry puts two things at two depths in one
window, and a single correlation returns whichever carries more contrast —
wearing the other one's name. The window I first tried had the same problem in
reverse (luma followed the overlay, at +0). `hud_frame.py` therefore reports
**three channels always**: luma, an overlay-only chroma channel, and an
overlay-suppressed background channel. Where they disagree, the disagreement is
the finding. A tool that printed one number could not have caught this and
would have reproduced the mistake.

**Two instrument failures of my own, recorded because both produced a number.**
The first search capped at −40 px and returned exactly −40 for every
near-cockpit window — the bound, not a measurement; `PEGGED` now says so. The
second was a whole-frame registration whose argmax read −39 px: printing the
curve showed a flat plateau from −60 to −20 varying by 0.006, aperture
ambiguity on the cockpit's repeated ribs, and no evidence at all. Both were
caught by looking at the shape rather than the extremum.

**Not converted to metres, deliberately.** px→depth needs the sx of the camera
that drew the element, and this frame ran `X4VR_FOV=2.21` whose scene camera
reads 0.14976 while 89 of its 124 credited slots read 1.33333 — the two give
0.08 m and 1.4 m for the same −43 px. Which camera draws the cockpit is not
established, so the frame is reported in pixels against its own extremes.

### #56 — the falsifier did not fire: X4's shadow atlas ignores `<fov>`

Answered from stored logs, no run. The prediction filed with #56 was that
widening `<fov>` for #33's guard band divides CSM texel density across the
whole frame, with the falsifier *"a 2048 that is not 2048 — if X4 sizes its
cascades from the field, the map dimensions move with `X4VR_FOV`"*.

**It does not move.** Every run that carries `X4VR_MV_INVENTORY` reports
exactly the same depth images:

    fov (default 1.0)  take 83    10 x 2048x2048 D16   render 1408x1408
    fov 1.1111         take 107   10 x 2048x2048 D16   render 1408x1408
    fov 1.437          take 105   10 x 2048x2048 D16   render 1408x1408
    fov 1.5            take 104   10 x 2048x2048 D16   render 1408x1408
    fov 1.4917         take 166a  10 x 2048x2048 D16   render 4224x4224
    fov 1.4917         take 184   10 x 2048x2048 D16   render 4224x4224

A 1.5× spread in field and a 3× spread in render extent, and not one dimension
moves. (Ten, not five, because the layer doubles them — the #51 cascades.) The
size comes from the video settings, exactly as X4's own comment on
`F_shadowmaxdistance` says: *"is set according to videoparameters"*.

**So the shadow texel budget is fixed while the field grows.** That is half the
prediction confirmed and the cheap half.

#### What X4's own shader says about the mechanism

* `F_shadowmapsize * F_texturefactorCSM{i}` is the effective texel count of
  cascade *i* — it is what `GetCSMSample3Taps` divides to get its kernel width
  ("want to stride samples by 2 texels"). So the cascades are packed into the
  atlas at per-cascade resolutions.
* Cascade selection is **containment, not distance**: `getCSMWeights` builds
  `2 * min(coord, -coord + 1)` per cascade, a pyramid weight that falls to zero
  at the edges of that cascade's own box. Which also fixes the convention —
  shadow coords are in **[0,1]**, so a cascade spans `1 / |row0|` metres.
* `F_shadowmaxdistance` drives only the fade-out, and it is a video setting.

None of that says whether the cascade box is fitted to the camera frustum. That
fit is CPU-side, and it is the half still open.

#### The price, if cascades are frustum-fitted — now a column in `guard_band.py`

A frustum-fitted cascade spans `2·z·tan(half)`, so its world extent scales as
the tangent — **the same ratio that sets the `sym px/eye` column**. The
difference is that render pixels can be bought back and a fixed atlas cannot:

       yaw    X4VR_FOV   sym area   asym area   shadow texel
       0.0     1.4917      1.00x      0.65x        1.00x
       5.0     1.6002      1.36x      0.89x        1.17x
      10.0     1.7358      2.06x      1.28x        1.44x
      15.0     1.8714      3.33x      1.94x        1.82x
      20.0     2.0071      5.96x      3.27x        2.44x     (yaw only)

      10.0     1.9644      4.89x      3.02x        2.21x     (yaw AND pitch)
      15.0     2.2278     25.72x     12.87x        5.07x

**`#35` does not help here, and that is the finding.** Its off-axis projection
takes ±10° of yaw from 2.06× to 1.28× in pixels — the reason it is on #33's
critical path. It does nothing for the shadow column, because X4's `<fov>` is
one symmetric number and the cascades follow *it*, not our per-eye frusta. So
"the affordable envelope is a glance" costs **1.44× coarser shadows at ±10° of
yaw** even after the saving that made it affordable.

That matters more than a ratio usually would: the memory of the previous X4 VR
attempt is that globally-applied shadows were its recurring problem, and this
is precisely a VR-only setting degrading shadows globally.

#### The instrument, and the run that settles it

`proj SHADOW cam#N` now reports, per credited camera and beside `proj VIEW`:

    proj SHADOW cam#0: mapsize=2048 maxdist=8000 factors=[...] thresholds=[...]
                       csm0-scale=0.002 csm1-scale=0.0005
    proj SHADOW cam#0: cascade 0 spans 500.0 m across 1024 texels -> 0.4883 m
                       per texel

**What answers #56 is the RATIO of `csm0-scale` between two `X4VR_FOV` values**,
which is why the raw scale is printed beside the derived metres-per-texel: if
the derivation's convention is wrong the ratio still answers, and the two
hypotheses differ by 1.00× against ~1.44×.

**PREDICTION, before the run.** Frustum-fitted: `cascade 0 spans` grows by
1.44× going 1.4917 → 1.7358, and `m per texel` with it. Fixed extent: both
unchanged to four digits, and the shadow column above becomes 1.00x throughout
and #56 closes as not-a-cost.

**Caveat that could void the take.** Members 9 and 10 are read by **zero** of
the 409 dumped modules — the shaders take their cascade matrices from the
per-object block instead — so X4 may leave them at whatever the buffer held.
The report says `ALL ZEROS` in that case rather than reporting an infinite
cascade, which would read as "fixed extent" and answer the question backwards.
If that is what comes back, the extent has to be read from set 3 members 3–7
instead, which the layer does not track today.

Both runs, flatscreen, no headset, one line each:

    X4VR_TAKE=185a-CSM-NARROW X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_DUMP_MATRICES=1 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take185a.log X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1 ./launch/x4vr-launch.sh

    X4VR_TAKE=185b-CSM-WIDE X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_DUMP_MATRICES=1 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take185b.log X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.7358 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1 ./launch/x4vr-launch.sh

In game: load the save, sit in the cockpit somewhere with **ground or hull
shadows visible**, and stay put for ~30 s. Nothing else — this is a
measurement take, not an interaction one, and the two must be the same scene or
the cascades are fitted to different worlds and nothing is comparable.

Cost: two short flatscreen runs. Acceptance: `python3 tools/score_run.py` on
each, then compare their `shadow` lines. Interaction-safe (no probe, no dumps);
`X4VR_DUMP_MATRICES` is what produces every line above.

### #54 — DECISION: the head pose arrives on a layer-owned descriptor set

No run. Written comparison, as the task asked, and every fact below was checked
today rather than recalled.

**The requirement.** Every correction the layer applies is a constant today —
the eye offset is our choice, the off-axis affine is latched once, and
`patch_fragment_invproj_eye`'s comment says the quiet part: *"Both are constant
compositions on a matrix that is already being rewritten, so this adds no load
and no descriptor."* A head pose is none of those. It has to reach

    vertex     ~330 World modules   the rotation itself
    fragment   244 (member 2) + 2 (member 4)   R⁻¹ into the reconstruction
    compute    3 (member 2)         desirable, and REACHABLE: R is the same for
                                    both eyes, so unlike every per-eye
                                    correction it is not blocked by compute
                                    having no gl_ViewIndex

changing every frame, written as late as possible. And it carries **only R** —
`sx`/`sy` stay live from X4's own block, because `headlook_clip.py` shows six of
the sixteen entries of `P·R·P⁻¹` scale with them. That keeps the per-frame
payload at two mat4s and makes it **camera-independent**, which matters below.

#### What is already in place, measured

* `vkCreateGraphicsPipelines`, `vkCreateComputePipelines` and
  `vkCmdBindPipeline` are **all hooked**. `vkCreatePipelineLayout` is **not**.
* X4 declares descriptor sets **0–4 and nothing higher**, across the dumps.
  This device reports `maxBoundDescriptorSets = 32` and
  `maxPushConstantsSize = 256`.
* The dumps are SPIR-V **1.2** (398 of 409; the other 11 are 1.0). The rule
  that `OpEntryPoint` must list *every* global variable starts at 1.4, so a new
  Uniform variable does not have to be threaded into the interface list.
* The patcher **already synthesises a global**: `OpTypePointer` + `OpVariable` +
  `OpDecorate`, with anchor logic for where declarations and annotations may
  legally go (`x4vr_spirv.hpp:638` and `:1262`, for `gl_ViewIndex`). Adding a
  uniform block is that pattern with different decorations.

#### A — a layer-owned descriptor set  ← CHOSEN

Hook `vkCreatePipelineLayout`, append our set at the first free index (5), keep
X4's layout → extended layout map, substitute at pipeline creation, and bind our
set at `vkCmdBindPipeline` so it can never be missing when a pipeline that
declares it runs.

* **Layout compatibility survives, and that is the load-bearing point.** Vulkan
  compatibility for set K requires sets 0..K to match; appending *above* every
  set X4 uses leaves 0–4 identical, so X4's bind-once-use-many keeps working.
  Appending anywhere else would break it silently.
* **We own the memory.** No race with X4's CPU, no member to squat on, no
  dependence on config we may want to change.
* **One write per frame, not 126.** Our buffer is not X4's arena, so the value
  is written once and every draw sees it. (Option B has to write every credited
  slot — take 184 credited 126 — and a slot first seen late is stale for a
  frame.)
* **It fails loudly.** A pipeline whose layout declares set 5 with nothing bound
  is a validation error at bind, not a wrong matrix that looks like bad
  tracking.
* Cost: a new hook, a layout map, a small host-visible ring, and a SPIR-V block
  synthesiser. The most code of the three, all of it conventional.

#### B — write into X4's camera block

Reuse `patch_view_before_submit`, which already reads the mapped block, and put
R in a member no shader reads. Tiny: the shader side is an access chain, the
same shape as the existing member-1 read.

**Rejected on two facts, not on taste.**

1. **The free member is free only because of our own overrides, and two of
   them are marked provisional.** Member 3 (`M_projection_uj`) reads zero
   because the injector forces `ssao=0` and `ssr=false` — X4's own
   `ssao_v2.frag` and `ssr.frag` both call `ProjectUJ()`. Members 5/6
   (`M_jitter`, `M_prevjitter`) read zero because `antialiasing=none`, and take
   163 proved FSR3 works, which would repopulate them. The `ssr` override says
   in its own comment *"Revisit after per-eye lighting lands"*. Building #33's
   foundation on three switches we expect to flip is a dependency in the wrong
   direction.
2. **The write timing is UNMEASURED, and the evidence people cite is about a
   different question.** "Zeroing the whole arena turns the screen black" comes
   from `X4VR_TEST_HAMMER`, whose own comment calls it *"a race-based oracle"*
   that zeroes the blocks from a background thread at ~10 kHz. That proves the
   GPU reads the blocks we identified — the *address* is right. It says nothing
   about whether a single well-timed write at submit survives to the GPU.
   `X4VR_TEST_VERIFY` exists precisely to answer that and has produced **no
   line in any of the 190 stored logs**; `X4VR_TEST_EYE_*` and
   `X4VR_TEST_HAMMER` appear in no recorded `env: run =` either.

   That does not make B wrong. It makes B's central assumption untested, while
   A has no timing question at all because nothing else writes our buffer.

#### C — push constants

`maxPushConstantsSize = 256` leaves room for two mat4s. But X4's layouts declare
no push range, so C needs **all of A's pipeline-layout surgery** and then adds
per-bind bookkeeping (push constants are invalidated by layout-incompatible
binds) and a shader-side pattern the layer has never emitted. Strictly dominated
by A.

#### D — the 236 spare bytes at the end of the camera block

X4's block ends at byte 1556 (`F_camerarotationspeed` at 1552) and the
descriptor range is 1792, so there is room for three mat4s the struct never
declares. Dominated: reading them needs the block's `OpTypeStruct` rewritten —
*more* SPIR-V surgery than declaring a fresh variable — while still inheriting
B's write-timing question, because it is still X4's buffer.

#### E — no per-frame channel at all: let the compositor rotate

Render wide, submit the projection layer with the pose we rendered at, let the
runtime reproject. Zero shader work, zero delivery problem, and **lower
motion-to-photon**, because the correction is late-latched at display time
rather than frozen when the command buffer is recorded.

Not an alternative — a **complement**, and one we get anyway. Both routes widen
`<fov>` by the same amount, so both pay #56's shadow cost identically; what E
adds is that X4 must actually *shade* the guard band (±10° yaw-only is 2.06×
area, 1.28× with #35) where the vertex route shades only what is displayed. The
right architecture is A **plus** E's residual reprojection, with the declared
pose being the pose we rotated by — the rule already written at
`x4vr_layer.cpp:8700`, generalised to `declared = X4's camera pose ⊕ our R`.

#### What would overturn this

If the `vkCreatePipelineLayout` hook turns out to break X4 — it caches layout
handles, or compares them somewhere we cannot see — fall back to B. In that
case **run `X4VR_TEST_VERIFY=1` first**: B's whole case rests on a write-timing
assumption nobody has measured, and discovering that at the same time as a
head-tracking bug is how a week disappears.

Acceptance for A, before any of it is believed: validation clean with extended
layouts, a headless harness case where a shader reads the block and the value
arrives on a real GPU, and a counter in the log pairing pipelines created
against pipelines that got the extended layout — an unequal pair being a fail,
not a warning.

### #59 — the pose channel is built, and it is proven rather than assumed

`X4VR_POSE=1`. Off by default: with it unset not one Vulkan call changes, no
module is patched, and the suite asserts that the log carries no `pose` line at
all — so every tagged state still reproduces.

    pose: channel up — set 5 binding 0, 4 region(s) of 128 B, identity until #33 drives it
    pose: 1 module(s) read the pose block (set 5)
    pose final: set 5, 2/2 pipeline layout(s) extended, 2/2 pipeline(s) got it, 2 bind(s) — OK

**What was built**, exactly as #54 decided: `vkCreatePipelineLayout` is hooked
and appends our set at index 5 (the first index X4 never uses), filling any
gaps with empty layouts because `pSetLayouts` has no holes. Compatibility for
sets 0–4 is untouched, so X4's bind-once-use-many still works. Pipelines are
substituted at creation, our set is bound at `vkCmdBindPipeline` — so
"declared" and "bound" are the same event — and the value is written at submit,
the same moment `patch_view_before_submit` already uses.

**The set index is a constant read before any module is patched.** SPIR-V bakes
it at `vkCreateShaderModule`, which can run long before a pipeline layout
exists, so reading it out of channel state would bake whatever the field held
and an `X4VR_POSE_SET` override would apply to the layout and not the shader.
The same class as the baked-`sx` trap: a value fixed at module creation cannot
come from state established later.

**Regions are keyed to the command buffer's recording epoch, not to a frame
counter.** Vulkan forbids re-recording a command buffer that is still
executing, so a region assigned at `vkBeginCommandBuffer` is free by exactly
the rule that lets X4 reuse the buffer — no fence of ours, and no frame counter
that could disagree with X4's own pacing.

**It carries `R` alone.** `sx`/`sy` stay live from X4's block, because six of
the sixteen entries of `P·R·P⁻¹` scale with them. That is what keeps the
payload camera-independent, and it is why one write per frame serves every draw
where writing X4's arena would need one per credited slot.

#### The two bugs the harness caught, both of which reported success

**1. The channel was dead unless the shear was on.** The patch went in beside
the four vertex paths — which sit *below* `if (!have_k) return`. So with
`X4VR_POSE` and no `X4VR_STEREO` nothing was patched at all, and yet: the
channel came up, `2/2` layouts extended, `2/2` pipelines got it, 2 binds, `OK`,
and the frame looked right. Every report said success and nothing was
delivered. The patch now runs before that return.

**That is the whole reason the acceptance test drives a value.** Identity and
"never arrived" produce the same picture, so `X4VR_POSE_TEST_DX=2` shifts clip
x by `2·w` and pushes the fullscreen triangle off screen: coverage goes to
zero, or the channel is not there. `pose: identity is inert` is the control
that keeps the probe honest.

**2. My second test asserted the opposite of the right answer.** It expected
the pose to arrive under `X4VR_STEREO` too, on the reasoning that delivery and
classification answer different questions. They do at the `have_k` gate — bug 1
— and they do **not** at the variant selection: a pose-patched module still
goes through `needs_original()`, so a pass the shear excludes gets no pose
either. Those exclusions are the shadow passes, whose transform is light-space
and must never be head-rotated, and the UI passes, which are #30's canvas and
must stay screen-locked. Both are exactly what #33 must also leave alone.

So the case now asserts the **exclusion**, and asserts the log line that
explains it rather than just the number.

One consequence worth stating because it is a trap: with no `K` there is no
second variant to fall back to, so every module is patched and the coverage
differs from a real run. The layer warns when that happens and names it a test
configuration.

#### What is NOT in this

No head tracking, and no rotation. The block ships identity, so a patched
module is a no-op — which is what makes it safe to leave on while the rest of
#33 is built. Applying member 0 straight to clip space is a **transport** test;
`P·R·P⁻¹` needs the live `sx`/`sy` conjugation, and that arithmetic is #33's,
not the channel's. What #33 changes here is what the shader does with the
block, not how it gets it.

Fragment and compute are reachable — the set declares all three stages and the
compute pipelines are substituted too — but nothing reads it there yet. The
member-2/4 correction is #33's.

Validation-clean: the VUID set is **empty** with the channel on and off, in both
the default and stereo configurations.

> Strengthened from "identical" after the fact, and the weaker word was hiding
> something. The harness carried a permanent `VUID-vkCmdDraw-None-08114` in
> every configuration where nothing mirrored into the top half of the bindless
> table: set 0 binding 0 declares eight descriptors and writes four, and an
> ordinary array binding promises all eight are valid. Against a non-empty
> baseline, "identical" is satisfied by a genuinely new error that happens to
> share an ID, and it teaches readers to skip VUID lines. The table now declares
> `PARTIALLY_BOUND` — which is what X4 must declare for its 53306-entry table
> anyway, so the harness is closer to X4's shape than it was — and
> `run-multiview-render.sh` asserts an **empty** set across seven
> configurations, having never enabled the validation layer at all before.

### #33 step 1 — the conjugation is written and validated; it has NOT executed

`X4VR_POSE_ROTATE=1`, with `X4VR_POSE_YAW/_PITCH/_ROLL` as a **static** stand-in
for head tracking. Read the last paragraph of this section before reading the
rest as progress.

**The two identities everything rests on**, both pinned in
`tools/headlook_clip.py --self-check` over the six measured cameras:

    decompose by sx/sy, rotate, recompose  ==  P·R·P⁻¹      2.6e-09 NDC
    M_invprojection · Rclip⁻¹              ==  R⁻¹ · M_invprojection   8.9e-16

The first says the shader may do the conjugation *the long way* — divide clip
x/y by the live `sx`/`sy`, rotate the resulting view vector, multiply back —
instead of building a matrix it cannot bake. `z_c` passes through untouched,
because X4's `m[10] == 0` makes clip z the constant near plane.

The second is the one that saves the fragment stage: because `M_invprojection`
*is* `P⁻¹`, the P's cancel and the deferred correction is a **plain
left-multiply by `R⁻¹` with no `sx` anywhere**. Had it been false, all 244
fragment modules that reconstruct would each have had to find the live `sx`
too. It rides the same rewrite as the affine.

**Validated offline over X4's own shaders**, which is the check this stage
earns:

    366 of 409 dumped modules patch and spirv-val clean
     43 refuse — no camera block at set 1 binding 0
      0 invalid

The 43 refusals are correct and load-bearing. A module without the camera block
cannot be given a rotation *at all*: a baked one would be the wrong geometry,
not a degraded rotation, so the patch refuses and the layer says so rather than
falling back. `tests/run-multiview-render.sh` pins both directions against
committed bytes.

**Scoped deliberately to a static angle.** Wiring the runtime's head pose in at
the same time would put two unverified things in one take, and a wrong picture
would have two causes. This is take 164's shape exactly: measure the transform
on a flatscreen before any headset is involved.

#### WHAT HAS NOT HAPPENED: the emitted code has never run

`spirv-val` says the module is well-formed. It does not say the arithmetic
computes the conjugation. The harness cannot answer it either — its only vertex
shader with a camera block is not one it can render, and the fullscreen shader
it *does* render has no camera block, so the patch correctly refuses and the
GPU sees nothing. The 180°-yaw discriminator (which would clip every vertex to
`w = −1` under the conjugation while merely mirroring under #59's transport
form) was written and could not be exercised for the same reason.

So the state is: **math proven, emission validated at scale, execution
unverified.** No claim beyond that is supported, and the fragment `R⁻¹`
composition, the runtime pose source and the declared-pose update are not
written at all.

Next, in order, and the first is a prerequisite for trusting any take:

1. A harness pass that renders a vertex shader declaring the camera block, so
   the conjugation executes and can be checked against the reference — the
   180° discriminator is the cheapest form.
2. The fragment `R⁻¹` composition. Not optional: with the vertex rotated and
   the reconstruction unrotated, a point at 50 m lands 8.7 m out at 10° of yaw,
   which breaks the CSM lookup outright. That is the #57 class, and it cost 13
   takes the last time.
3. Only then a flatscreen take at a static angle, read with
   `tools/register_affine.py` against the predicted warp.

#### 14 World modules refuse the rotation, and that is a gap not a refusal

Asked while answering "what are the constraints". Of the 43 dumped modules
`patch_vertex_pose_rotate` refuses, `classify` calls **29 non-World and 14
World**. The non-World ones are right to refuse — fullscreen passes and UI,
which must not rotate. The 14 World ones are a **defect in waiting**: they are
scene geometry that would keep X4's frame while everything around them turns
with the head.

They are the same class task #23 already met — *"18 of X4's 341 world modules
have no camera block"* — and it has a solved answer:
`patch_vertex_eye_offset_mvp` recovers `sx` from `M_worldviewprojection`
(`sx = |row0(MVP).xyz| / |row3(MVP).xyz|`) for exactly these modules. The
rotation needs the same recovery, and then it needs `w_c`, which the MVP form
also has.

Until that exists the constraint is concrete and worth stating in the plain
form: **a small amount of world geometry will not follow the head.** Which
geometry is not yet named — the 14 can be identified against X4's shader
sources by their rarest property name, the same join that named the starfield
and the suns.

This is the shape of #48, found before a take rather than after one, and it is
the reason the module counter in the layer names its refusals rather than
counting them silently.

### #60 and #62 — the conjugation executes, and every World module now gets it

**#60: it runs, and the test rules out the other implementation.**
`tests/fullscreen_camera.vert` is procedural geometry the harness can already
draw carrying the camera block shaped the way the patch verifies, and the
output pass binds set 1 before its *first* draw rather than only before the
credit loop — the shader reads it now, and an unbound descriptor is undefined
behaviour rather than a wrong picture.

The test is a **discriminator**, not a "the picture changed" check:

    180° yaw, conjugation   v'z = −vz, so w' = −1 for every vertex at w = 1.
                            Everything is behind the eye. Nothing draws.
    180° yaw, #59 transport mirrors x and z, w untouched, still covers.

Measured: 0° / 30° / 180° give drawn = 1 / 1 / **0** through the conjugation,
and 180° gives drawn = **1** through the transport form. A test that only
asserted "something moved" would have passed on the wrong implementation.

**#62: the 14 camera-blind World modules are reached, via the object block.**
Naming them against X4's sources failed — they declare no material properties
at all, and two look like depth-only shaders (one vertex input, a fragment
entry point with no interface). But `tools/module_map.py` against take 184
answered it from real data instead: all 14 are classified
`World,mvp-sx,+affine`, i.e. exactly task #23's family, drawn `-> patched` in
some passes and `-> original` in others. Real scene geometry.

So the rotation takes the same fallback the shear does, recovering the scales
from `M_worldviewprojection`:

    sx  = |row0(MVP).xyz| / |row3(MVP).xyz|
    |sy| = |row1(MVP).xyz| / |row3(MVP).xyz|,  sign supplied as negative

**That sign is load-bearing here in a way it is not for the shear.** `sy`
appears twice in the conjugation, once dividing and once multiplying. The
`R11` term's two sign errors cancel; the `R10`, `R12` and `R21` terms' do not.
Get it wrong and yaw survives while pitch and roll come out mirrored — which is
worse than not rotating, and would read as a tracking-axis bug.

Over the dumps, before and after:

    366 patched / 43 refused   camera block only
    384 patched / 25 refused   with the object-block fallback
      0 invalid, both times

and **all 25 remaining refusals are non-World** — fullscreen passes and UI,
which must not rotate. Every World module in the dumps is now reached.

#### A transcription bug that refused 18 modules with a plausible reason

The fallback found nothing at first. The cause was one word: the check for
X4's `GLSL.std.450` import used `0x30353475` for `".450"` where it is
`0x3035342e` — `'u'` for `'.'`, retyped instead of copied from the proven twin
40 lines away. The sqrt the recovery needs is an `OpExtInst`, so with no import
found the fallback declined, and the module then refused with **"no camera
block"** — which is *true*, and is exactly the kind of correct-but-not-the-cause
message that ends an investigation early. The fixed check is now copied rather
than retyped, and says so.

One real gap it exposed: a depth-only shader declares no `OpTypeBool` at all,
which the guarded divide needs. That one is synthesised; `OpExtInstImport` is
not, because it has its own section anchor, and a module without one is
refused rather than half-built.

### #61 — `R⁻¹` into the deferred reconstruction, and the third first-match slip

The vertex stage rotates; without this the reconstruction does not, and
`gl_FragCoord` is then the rotated camera's while `M_invprojection` is X4's. A
point at 50 m lands 8.7 m out at 10° of yaw, and the CSM lookup goes through
exactly that position — so shadows break outright, and the froxel lookup and
specular with them. That is the `#57` class, which cost thirteen takes.

**The fix is one runtime multiply, and that is the finding from #33 step 1.**
`M_invprojection` *is* `P⁻¹`, so the P's cancel in `M_invprojection · Rclip⁻¹`
and the whole correction collapses to a **left-multiply by `R⁻¹`** — no `sx`
anywhere, which is what lets it ride the rewrite
`patch_fragment_invproj_eye` already performs instead of needing the live
camera in 244 separate fragment modules. Pinned at 8.9e-16.

Applied to **members 2 and 4 both**, for take 83's reason: they hold the same
numbers in X4 and differ only after our patch, so correcting one and leaving
the other is precisely the defect that took thirteen takes to find.

Over the dumps: **244 patched and spirv-val clean, 165 refused, 0 invalid** —
and 244 is exactly the count of fragment entry points that load member 2 in
`tools/uniform_block_scan.py`. Every module that reconstructs is reached.

With the rotation off the patch is passed `0xffffffff` and not one emitted word
changes, so every tagged state still reproduces.

#### The same first-match mistake, for the third time

This file already records it twice — take 48's aliased binding, and again in
`patch_fragment_invproj_eye`'s own comment ("Recorded here because it is now the
second time"). The third: the annotation block for the pose descriptor was
inserted by matching

    if (in.start == anno_at && !existing_view_var) { ... }
    if (in.start == first_fn) out.insert(...decls...);

on its **first** occurrence in the file — which is in
`patch_fragment_index_offset`, 700 lines past the function being edited. Four
identical-looking emission tails, and the edit landed in the wrong one. The
compiler caught it, because the ids were out of scope; had the two functions
shared a variable name it would have compiled and patched the wrong shaders.

The lesson that generalises is not "be careful": it is that a repeated code
shape in one file makes textual anchors unsafe, and an edit to one of N
identical tails has to be bounded by the enclosing function, not by the first
match. The move that fixed it asserts the insertion point lies between this
function and the next.

Also fixed on the way: the decoration was first written under the
`!existing_view_var` guard, which is about `gl_ViewIndex`, not about the pose
block. Hanging a new decoration off an unrelated condition is how it goes
missing on exactly the modules that already did something else.

---

## Takes 186a / 186b — the flatscreen measurement of #33's rotation

> **THE RUN HAPPENED. Read "Take 186 — RESULT" at the end of this file
> first.** 186a is a clean control and stands. 186b died on a descriptor-set
> index collision before it could test anything, so **none of the predictions
> below were reached** — they are not wrong, they are unmeasured, and they
> carry forward to take 187 unchanged. This section is kept exactly as written.

**READ THIS FIRST AFTER A CONTEXT RESET.** Written before the run, which is the
rule: the predictions below are what the take tests, and they are dated by
being in this commit rather than a later one.

### State of #33 going in

Built, and each verified to a stated level:

| piece | verified how |
|---|---|
| the math — `P·R·P⁻¹` and `M_invproj·Rclip⁻¹ = R⁻¹·M_invproj` | `headlook_clip.py --self-check`, six measured cameras, 2.6e-09 / 8.9e-16 |
| the channel (#54/#59) — set 5, one write/frame | executes on a real GPU; a driven value moves the picture and identity does not |
| the vertex conjugation (#60) | **executes**; 180° yaw clips every vertex under the conjugation and does not under #59's transport form |
| the object-block fallback (#62) | 384/409 dumps patch, 25 refuse, **all 25 non-World**, 0 invalid |
| the fragment `R⁻¹` (#61) | 244/409 patch and `spirv-val` clean — **but has never executed** |

**Not built at all:** the runtime head-pose source and the declared-pose update.
The rotation comes from a static `X4VR_POSE_YAW`, deliberately, so a wrong
picture has one candidate cause rather than two.

**The highest-risk item is the fragment `R⁻¹`, and 186b is its first
execution.** The harness proves the vertex side; nothing yet drives the
fragment side on a GPU. If something is wrong in this take, that is where to
look first.

### Predictions, before the measurement

1. **Both logs score PASS**, and 186b carries `pose: channel up — set 5
   binding 0`, `pose: static rotation yaw=10.00 pitch=0.00 roll=0.00`, and
   `pose final: … — OK` with pipelines-created equal to pipelines-extended. An
   unequal pair there is a FAIL, not a warning: a pipeline that missed the
   substitution ran with our set unbound.
2. **`headlook_verify --verify` shows a large positive lift** — raw "off vs on"
   correlation low because the world moved, high after the predicted warp — and
   the predicted centre shift is **≈ 87 px** at `sx = 0.70024`.
3. **The cockpit rotates and the HUD does not.** That split is the design.
4. **A wedge of missing world geometry on one side, about 10° wide**, because
   `X4VR_FOV` is unchanged and X4 culled it. The starfield backdrop stays,
   because it is procedural and does not rotate. Both are expected, neither is
   this take's subject.
5. **Shadows stay attached to what casts them.** This is the fragment `R⁻¹`
   working, and it is the prediction I am least able to support: 8.7 m of
   error at 50 m if it does not.

### The assumption I reasoned but did not test

The pose patch runs **before** the shear and the affine, so its edit is emitted
first and theirs applies to the already-rotated position. That is the order I
believe is right — the eye offset belongs in the *rotated* frame, because the
eye sits at `head + R·(±d,0,0)` — and the shear's own derivation survives it,
since the conjugation leaves `z_c` at the near plane exactly. But it is
reasoning, not a measurement, and if the stereo looks wrong while the rotation
looks right, this is the first thing to suspect.

`X4VR_OFFAXIS` is unset in both runs, so the affine is not a variable here.

### The two runs

    X4VR_TAKE=186a-ROT-OFF X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take186a.log X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 ./launch/x4vr-launch.sh

    X4VR_TAKE=186b-ROT-YAW10 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take186b.log X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=10 X4VR_BINDLESS_MIRROR=1 ./launch/x4vr-launch.sh

186a is take 164a's line unchanged; 186b adds exactly three variables. No
headset, no probe, no dumps — interaction-safe, and `X4VR_DUMP_MATRICES` is
what produces the `proj` lines the acceptance needs.

**In game:** load the save, sit still in the cockpit with the frame visible and
some world geometry outside it (a station or a ship — plain starfield is not
enough for the fit). One screenshot of the whole SBS window to
`/tmp/x4-186a.jpg`. Quit, run 186b, return to as close to the same pose as
possible, screenshot to `/tmp/x4-186b.jpg`.

### Acceptance

    python3 tools/headlook_verify.py --self-check
    python3 tools/score_run.py /tmp/x4vr-take186a.log
    python3 tools/score_run.py /tmp/x4vr-take186b.log
    python3 tools/headlook_verify.py --verify /tmp/x4-186a.jpg /tmp/x4-186b.jpg \
        --sx 0.70024 --yaw 10

Take `sx` from 186b's `proj` line marked `<- honours X4VR_FOV`, not from this
document: if the log says something other than 0.70024, the log is the
authority and the take is telling you something.

### If the lift is absent

Sweep `--yaw` and find what *does* fit. A fitted angle that is not 10° is
specific evidence:

* **fitted ≈ −10°** — a sign convention, in `pose_set_euler` or in the shader.
* **fitted ≈ 10° but only near the centre** — the homography is not being
  applied, only its linearisation; suspect a module taking the transport form.
* **no angle fits anywhere** — the vertex patch is not reaching the modules
  that draw the cockpit. Read `pose: N module(s) read the pose block` and the
  refusal line against `tools/module_map.py`.

Do **not** add a knob to make it fit.

## Take 186 — RESULT: 186a is clean, 186b collided with X4's set 5

**READ THIS FIRST AFTER A CONTEXT RESET.** Supersedes the specification above
as a *plan*; that section is kept unedited because its predictions are still
the acceptance criteria, just for take 187 instead.

### What each run says

**186a (rotation off) — PASS, and it stands as the control.** Cockpit, HUD,
starfield, both eyes healthy. `sx=0.70024` on the fov camera (109.998° =
fov 1.492, x196), GAMEPLAY 6.67 ms. The code changed after this run, but only
inside `pose_wanted()`-gated paths, and 186a set no `X4VR_POSE`, so every line
it executed is still the code on `main`. It does **not** need re-running.

**186b (yaw 10°) — FAIL. Dead as evidence, and not because the transform was
wrong: it never ran.** The layer said so itself:

```
pose: WARNING — could not extend a pipeline layout (declares 6 set(s), ours is index 5)
pose final: set 5, 5/6 pipeline layout(s) extended, 26/1135 pipeline(s) got it,
            2310217 bind(s) — FAIL
```

Twenty-six of one thousand one hundred thirty-five pipelines got the pose set.
Over three hundred modules were patched to read it.

### The root cause: the index was inferred from the wrong object

`pose_set_index()` defaulted to 5, and the code said why:

> X4 declares sets 0-4 across all 409 dumps, so 5 is the first free index

The 409 dumps are **shader modules**, and a module declares the sets it
**reads**. A **pipeline layout** declares the sets X4 **binds**, and a layout
routinely declares sets that no module in that pipeline reads. Those are two
different objects, and the survey was of the wrong one. X4's main pipeline
layout — the one carrying **1109 of its 1135 pipelines** — declares six sets,
so index 5 was X4's.

This is the same error class as `nm -D` proving a symbol is imported rather
than called, and as "all 128 blocks, M_view is always identity" being
exhaustive over a snapshot. The claim was true about the dumps and said nothing
about the layouts.

### Two corruption paths, and the second was not anticipated

1. **Our patched modules read X4's set 5 as a pose block.** Whatever X4 keeps
   there was loaded as a `mat4` and multiplied into `gl_Position`.
2. **Our binds disturbed X4's own set 5, for X4's own draws.** Descriptor set
   compatibility for set *N* depends on sets 0..*N*; our extended layout matches
   X4's only through set 4, so every one of our **2,310,217** binds at index 5
   invalidated X4's binding there. Modules we never patched were corrupted too.

Path 2 is why the damage was global rather than confined to rotated geometry,
and it is why the frame showed a hard-edged bright rectangle: this is undefined
behaviour, not a transform with a bug.

**What in 186b's log can and cannot be read — the distinction matters, and the
first version of this section got it wrong by condemning the whole log.**

*Genuine:* the `M_view` values themselves. The layer decodes them from X4's
mapped uniform memory, which our patch never writes, before any shader runs.
186b's dominant camera reads **14.7° / 8.62 m**, and the camera already
recorded in this project across 64 stored run logs reads **15° / 8.70 m** —
the same one, to the precision the log prints. That agreement is evidence the
values survived, not that they were scrambled.

*Not interpretable:* everything downstream of a shader executing. Draw counts
and credit attribution (they depend on what actually ran), `perf` — GAMEPLAY
2.66 ms, **faster** than the control, precisely because much of the scene was
degenerate or clipped — the shadow line, and the picture. Do not mine those,
and do not treat the 2.66 ms as a performance result: an undefined frame is not
a fast frame.

### The scorer printed PASS over a FAIL the layer had already printed

`score_run.py` reported `PASS` for 186b, and above it: `pose not reported —
this build predates the instrument`. Both came from the block that greps
`vr pose:` — the pose **declared to the runtime**, which a flatscreen take
legitimately never logs. The message was true about its own subject and
irrelevant to the run being scored, and there was **no check at all** for
#59's channel.

Every run handed over gets this file as its acceptance check. An acceptance
check that cannot see a FAIL the layer already printed is worse than none: it
converts a correct diagnosis into a confirmation. Fixed — 186b now exits 1, and
the runtime-pose message names its subject.

### Why no harness case caught it

Every case in `run-multiview-render.sh` passed, including the four `exec:` ones
that prove the conjugation executes. All of them built pipeline layouts
declaring **one or two** sets, so index 5 always fit. The suite certified the
configuration, not the code — the third time in this project that a self-test
passed only in its simplest configuration.

`X4VR_TEST_PAD_SETS` now builds the harness at X4's width, and the new pair is
a discriminator: identical runs, only the index differs, and the observable
flips.

| case | index | 6-set layout | result |
|---|---|---|---|
| `width: 6-set layout, derived index` | 31 | yes | `drawn=0` — conjugation clips at 180°, 0 collisions |
| `width: index 5 collides, fails closed` | 5 | yes | `drawn=1` — collision, geometry left unrotated |

### The fixes

* **The index is no longer inferred from X4.** It sits at
  `maxBoundDescriptorSets - 1` — **31** on the RX 7900 XTX (RADV NAVI31,
  limit 32). Colliding would require X4 to use every set the device allows.
  The limit is captured at `vkCreateDevice`, which necessarily precedes the
  first module patch — and the index must be frozen before that patch, because
  it is baked into the SPIR-V.
* **A collision is a hard stop.** The old code warned — the warning even said
  "so their modules must not be patched" — and then patched them for another
  two million draws. `pose_active()` now goes false, patching stops, binding
  stops. A report that names a risk without the code preventing it is not a
  guard.
* **`pose final:` reports the evidence**: `set 31 of maxBoundDescriptorSets=32,
  X4's widest pipeline layout declares 6 set(s), …`. That middle number is the
  one whose absence cost the take, and it was free — the counter was already in
  hand.

One consequence worth stating: the limit check in `pose_init` can now only fire
for an explicit `X4VR_POSE_SET`, since the derived index is legal by
construction. On the default path it compares a number against the number it
was computed from. Said out loud in the code, because a guard that cannot fail
reads like coverage it does not provide.

### Take 187 — the re-run

Only the rotation run is needed; **186a stands as the control**. Every
prediction from the 186 section applies unchanged, with one edit: the channel
now comes up on **set 31**, not set 5.

The acceptance line to look for, before anything else:

```
pose final: set 31 of maxBoundDescriptorSets=32, X4's widest pipeline layout
            declares 6 set(s), N/N pipeline layout(s) extended, M/M pipeline(s)
            got it, … — OK
```

`N/N` and `M/M` must be **equal pairs** and it must end in `OK`. If X4's widest
layout comes back wider than 6, that is new information and worth recording
either way — the index has room to 31.

**The command, in full, on one line:**

```
X4VR_TAKE=187-ROT-YAW10 X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=10 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_LOG=/tmp/x4vr-take187.log ./launch/x4vr-launch.sh
```

Derived from 186b's logged `env: run =` line, with `X4VR_LOG` renamed so 186b's
data is not overwritten, and `X4VR_TAKE` renamed. Nothing else changed — the
fix is in the code, not in the knobs.

**In-game:** load the default quicksave with Enter at the menu (it puts the ship
in the same place every time, with a shop, a gate and a station ahead at roughly
10–12 km). Sit still in the cockpit. Take one screenshot to `/tmp/x4-187.jpg`.
Do not open the map. Quit normally so the final report is written.

**What to look at, in this order:**

1. **Does the cockpit look intact?** Not "is it rotated" — intact. A repeat of
   186b's hard-edged bright rectangle means the channel is still colliding, and
   the log will say so directly.
2. **The cockpit frame should be rotated ~10° to one side while the HUD stays
   put.** The HUD is excluded on purpose (#30's canvas), so cockpit-moves-and-
   HUD-does-not is the expected picture, not a defect.
3. **A wedge of missing world geometry at one edge**, roughly 10° wide, is
   expected: it is the guard band, and it is the known cost of rotating inside
   X4's own frustum.
4. **The procedural backdrop will not rotate.** Known gap, not a new defect.
5. **Shadows should stay attached to what casts them.** This is the fragment
   `R⁻¹`'s first execution — if shadows detach, that is the suspect.

**Acceptance:**

```
python3 tools/score_run.py /tmp/x4vr-take187.log
python3 tools/headlook_verify.py --self-check
python3 tools/headlook_verify.py --verify /tmp/x4-186a.jpg /tmp/x4-187.jpg --sx 0.70024 --yaw 10
```

Take `sx` from take 187's own `proj` line marked `<- honours X4VR_FOV`, not from
this document. The predicted centre shift is `sx·tan 10° = 0.1235` NDC ≈ **87 px**
on a 1408-wide eye.

**`score_run.py` must exit 0 before the screenshots mean anything.** If
`pose-channel` reports a collision or a FAIL, stop there: the frame is undefined
and fitting a rotation to it could produce a confident wrong answer. And if the
lift is absent while the channel reads OK, the diagnostic tree in the 186
section applies unchanged — a fitted ≈ −10° is a sign convention, a fit only
near centre means a module took the transport form, and nothing fitting means
the patch is not reaching the cockpit's modules. **Do not add a knob to make it
fit.**

## Take 187 — RESULT: #33's rotation is CONFIRMED, and one new seam

**READ THIS FIRST AFTER A CONTEXT RESET.** The predictions written in the
"Takes 186a / 186b" section were finally reached. Four of five confirmed, one
partly, and one new defect found.

### The channel came up clean

```
pose-channel  set 31 of maxBoundDescriptorSets=32, X4's widest pipeline layout
              declares 6 set(s), 6/6 pipeline layout(s) extended,
              1027/1027 pipeline(s) got it, 2430057 bind(s) — OK
```

Equal pairs, `OK`, `score_run.py` exit 0. And **X4's widest layout declares
exactly 6**, which is the number take 186b's collision implied — the diagnosis
was right, and the index at 31 clears it with room to spare.

`invproj final: per-eye M_invprojection — 228 modules corrected` — **the
fragment `R⁻¹` (#61) executed for the first time**, on 228 modules, plus 2 on
the `_uj` path. It had never run on any hardware before this take.

### The rotation is exactly what was asked for

`headlook_verify --scan` over ±16° of yaw, both eyes:

| yaw | left | right |
|---|---|---|
| +6° | 0.024 | 0.024 |
| +8° | 0.080 | 0.075 |
| **+10°** | **0.714** | **0.718** |
| +12° | 0.154 | 0.176 |
| +14° | 0.019 | 0.020 |

A sharp isolated spike at **+10.00°**, against ~0.02 everywhere else and
0.001/0.004 before the warp. Not a plateau — the neighbours at ±2° are 4–9×
lower — so the argmax is a real peak and not aperture ambiguity. Correct sign,
both eyes agreeing to 0.004. **This is `P·R·P⁻¹` executing in X4 at the angle
we asked for.**

The captures came back 2816×1412 and 2818×1411 for the same 2816×1408
composite, so the tool now centre-crops to a common extent and **prints the
resulting origin bias** (1.0 px in x) next to the 87 px predicted shift. It
used to refuse outright. Cropping is not free — `ndc_map` is referenced to the
frame centre — which is why the bias is reported rather than absorbed, and why
`--scan` exists to settle the answer independently of it.

### The guard band, predicted to one pixel

The wedge of missing geometry is on the **left**, and its edge is not
approximately where the math says — it is exactly there:

```
predicted guard-band edge:  NDC -0.70021  ->  x = 211.1 px
measured hard edge:         NDC -0.70170  ->  x = 210   px
```

The prediction comes from solving the gather map for the destination whose
source sits on the frame's left edge; the measurement comes from a
brightness-step detector run on the screenshot. Two independent derivations,
**1 px apart**. The band is **15.0 % of the eye width for 10° of yaw**, and the
fit returns *zero* valid pixels left of it — there is no source data there,
which is what a guard band is.

### NEW: a hard vertical seam at x = 746, in both eyes

Not the guard band, and not scene content that the rotation moved. The luma
profile across it, median over the fit band:

```
187 measured   x=740..752:  39 40 40 40 40 40 | 27 27 26 26 27 27 28
186a warped    x=740..752:  38 39 40 40 41 41 | 41 41 41 41 40 40 40
```

A **one-pixel cliff, 40 → 27** (×0.67) in the measured frame, where the
predicted frame is flat to within 1.3 luma. Identical in both eyes, same
column. Resampling cannot make that edge and then hide it in the prediction.

> **The ×0.67 is the fit band only — do not read it as the seam's character.**
> The later section "The x=746 seam" measures the ratio by row band and gets
> 4.00 / 0.70 / 0.63 / 0.03 / 0.00 / 3.00: rows 55–80 % go to *black* on the
> right and the outer bands go the other way. This is not a gain step, and the
> single number here was a band-limited view of something that is not uniform.

What is *not* wrong: the region between the guard band and the seam reproduces
at **gain 1.000** in both eyes. The transform is right there.

**Hypothesis, to be tested first and NOT a finding.** The seam sits at
x = 746, and the eye centre plus *half* the predicted centre shift is
704 + 86.9/2 = **747.5** — 1.5 px away. That is what a pass receiving half the
rotation would look like, e.g. a half-resolution screen-space pass where the
shift was applied in pixels rather than in normalised coordinates. It is one
coincidence and nothing more until a second measurement agrees.

**How to test it, and why not from this take.** The right-hand region measures
"×0.66–0.71 darker than predicted", but those are near-black pixels
(median luma 5 against 7.6) in a **jpeg** — that ratio is compression noise
dressed as photometry, and this project has already spent eight takes on a
ratio that presumed attenuation where a component was simply absent. The
40 → 27 step is in well-exposed pixels and is solid; the darkening claim is
not. A photometric answer needs the **PNG** present dump
(`X4VR_MV_DUMP_PRESENT`), not a screenshot, and the per-pass instrument to name
which pass writes that column boundary. Filed rather than chased here.

### Scoreboard against the predictions

| # | prediction | outcome |
|---|---|---|
| 1 | both logs PASS, `pose final: … OK` | **confirmed** — 6/6, 1027/1027, exit 0 |
| 2 | large lift, ≈87 px centre shift | **confirmed** — +0.71 both eyes, peak at exactly 10° |
| 3 | cockpit rotates, HUD does not | **confirmed** — HUD elements sit where 186a had them |
| 4 | ~10° wedge of missing geometry | **confirmed** — 15.0 % of eye width, edge predicted to 1 px |
| 5 | shadows stay attached | **not contradicted**; no detachment seen, but nothing here measures it directly, and #61's first execution deserves a better instrument than an eye |

Prediction 5 is recorded as unmeasured rather than passed. Nothing in this take
isolates shadow attachment, and "it looked fine" is not the acceptance standard
this project uses.

## The x=746 seam — two hypotheses killed without a run, and take 188

Follows "Take 187 — RESULT". **No take was spent on anything below except the
one specified at the end.**

### What the seam is, measured

* **Both eyes, same column** (746 / 745, and that 1 px is the 2818-vs-2816
  capture width, not disparity). World geometry at any finite depth would sit
  at different columns in the two eyes. This is screen-space or infinitely far.
* **Geometry crosses it unbroken.** The cockpit rail and struts run straight
  through the boundary with no displacement. Only the sky/nebula tone changes.
* **It is NOT a uniform photometric step.** The ratio right/left across the
  boundary, by row band, is 4.00 / 0.70 / 0.63 / 0.03 / 0.00 / 3.00. Rows
  55–80 % go to *black* on the right, and the top and bottom bands go the other
  way. A gain difference cannot do that. The "×0.67" in the previous section is
  the fit band only, and it is not the whole story.
* **It is caused by the rotation.** 186a has no step within ±12 px of 746
  (max 2.0 against a frame-wide 17), and 186a and 187 differ only by the pose.

### Two hypotheses killed from data already in hand

**H2 — the seam is a guard band belonging to a pass with a different `sx`.**
Solving `(sx·s − c)/(c + s/sx) = +0.0597` for `sx` gives **6.020** (or a
negative root). X4 used `sx` = 0.70024, 1.00000, 1.33333, 3.78085 in this run.
No match, and 6.02 is outside the whole observed range. **Dead.**

**H4 — the seam is world content that rotated correctly.** Then it was at
NDC −0.0629 = x 659.7 in the unrotated frame. 186a's largest step anywhere in
x 647..671 is **2.0**, against a frame-wide maximum of 17. There is nothing
there to rotate. **Dead.** (This also independently re-confirms the earlier
finding that the warped prediction is flat across the seam.)

**H3 — not rotation-related** is dead too, by the 186a comparison above.

### The leading mechanism — from X4's own code and our own log

X4 ships a `volume_*` shader family: `volume_base.comp`, `volume_point.comp`,
`volume_noise.comp`, `volume_convert.comp`, plus `volume_apply/blend/medium/
dist` fragment stages. That is a **volumetric froxel grid** — `volume_base.comp`
bounds itself on `V_volume_size.xy` and stores into a 3D image indexed by
`gl_GlobalInvocationID.xyz`; `volume_point.comp` transforms an offset by
`mat3(M_viewinverse)`.

Take 187's own log says:

```
invproj final: per-eye M_invprojection — 228 modules corrected
               (offline: 244 of 409 eligible, 3 compute cannot be)
```

**Compute shaders cannot receive #61's fragment `R⁻¹`.** So a volumetric medium
built in X4's *unrotated* view, on a grid of finite extent, is then applied over
geometry we *did* rotate. A grid boundary is exactly the kind of thing that
produces one hard straight edge, and "sky changes, geometry does not" is exactly
what a fog/medium term looks like.

**This is a hypothesis, not a finding.** It is written down because it came from
reading X4's shaders and our own log rather than from guessing, but nothing here
names the pass. Do not act on it before take 188.

### Why take 188 is not a per-pass bisect

The obvious instrument is `X4VR_MV_DUMP_AFTER_RP`. **Take 179 already lost its
bisect to it**: the round-robin advances only after a capture lands, so one
unreachable target starved the whole list and the run produced zero files. The
docs record the give-up path that fixes this as **not covered by any test** —
`run-multiview-render.sh` says so in its own comment. Building this run on it
would risk repeating take 179 exactly. Take 188 therefore uses only the
**cadenced present dump**, which is bounded, proven, and writes PNG.

### Take 188 — vary the angle, and let the guard band prove the run

One run at **yaw 20°** instead of 10°. Everything else identical to 187 plus the
two dump knobs — diffed mechanically against 187's logged `env: run =` line:
`X4VR_MV_DUMP` and `X4VR_MV_DUMP_PRESENT` added, nothing dropped, nothing else
changed but the angle, the take name and the log path.

**Predictions, written before the run:**

| what | at 10° (measured) | at 20° (predicted) |
|---|---|---|
| guard-band edge — **the positive control** | 210 | **358.8** |
| seam, if it takes *half* the rotation | 746 | **793.7** |
| seam, if it takes the *full* rotation | 746 | **834.9** |
| seam, if it is fixed in screen space | 746 | **746** |

The guard band is the control and it matters more than the seam: it is already
explained to 1 px, so **if it does not move to 358.8 ± 2, the run is wrong and
the seam reading means nothing.** A measurement with no control is how this
project has lost takes before.

The three seam outcomes are 40 px apart — far outside the ±1 px this edge is
measurable to — so they cannot be confused. Any *fourth* answer is real
information: compute what map sends 746 → the observed column, and do not force
it into one of the three.

**The command, in full, on one line:**

```
X4VR_TAKE=188-ROT-YAW20 X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=20 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_MV_DUMP=/tmp/x4vr-t188 X4VR_MV_DUMP_PRESENT=300 X4VR_LOG=/tmp/x4vr-take188.log ./launch/x4vr-launch.sh
```

`X4VR_MV_DUMP_PRESENT` **is a cadence, not a flag** — 300 is a value seven prior
takes used. At 1 it is a full readback and stall on every frame (~1 fps) and it
would cost the run its own interaction, which is what happened to take 98.

**In-game:** identical to 187 — Enter at the menu for the quicksave, sit still
in the cockpit, one screenshot to **`/tmp/x4-188.png`** (PNG, not jpg), no map,
quit normally. ~2½ minutes plus load. The layer's own PNGs land at
`/tmp/x4vr-t188*.png` and remain the **primary** source; the screenshot is the
cross-check.

> **Why PNG for the screenshot, from measuring the 187 one.** Its JPEG is
> 4:2:0 — luma full resolution, chroma halved *horizontally*. Luma being full
> res is why the grayscale edge work in the 187 section stands. But two things
> do not survive it:
>
> * **DCT ringing at a hard edge.** Both edges under study sit 2 px into an 8×8
>   block, so they are not maximally distorted, but quantisation still softens
>   and rings a one-pixel cliff. The guard band's "predicted 211.1, measured
>   210" agreement is therefore *consistent with* the prediction rather than
>   confirmed to 1 px — the instrument's own resolution on a JPEG is about that
>   same scale. Take 188 measures the control on PNG and can claim it properly.
> * **Colour.** The seam is a *tone* change, and chroma is exactly the channel
>   4:2:0 halves horizontally — across a vertical edge, the worst case. Every
>   measurement so far has been luma-only partly because of this. A PNG allows
>   a chroma measurement, which may separate "fog term missing" from "different
>   exposure" in a way luma cannot.
>
> PNG does **not** fix the capture path. 186a and 187 came back 2816×1412 and
> 2818×1411 for the same 2816×1408 composite — that jitter is the compositor and
> the grab tool, not the format, and it is why the layer's own dump is primary.
> If the PNG screenshot and the present dump disagree *geometrically*, that
> isolates the compositor and is worth knowing on its own.

**Acceptance:**

```
python3 tools/score_run.py /tmp/x4vr-take188.log
```

must exit 0 with `pose-channel … — OK` before the images mean anything. Then the
guard-band control and the seam position, both from the PNG.

**What this run does NOT do:** it does not name the pass. It establishes the
seam's motion law, which is what decides whether the volumetric-froxel
hypothesis is worth the per-pass bisect — and if it is, the give-up path in
`X4VR_MV_DUMP_AFTER_RP` should get its missing test *first*, because that is the
defect that ate take 179.

## Take 188 — the seam is SOLVED: we rotate the position and not the motion vector

**READ THIS FIRST AFTER A CONTEXT RESET.** Take 188 passed its control, the seam
obeys a closed-form law to 1.1 px across six independent cases, and the
mechanism is named from X4's own source. This is a real defect in our patch and
it is **not cosmetic**.

### The run

`score_run.py` exit 0. `pose-channel  set 31 of maxBoundDescriptorSets=32, X4's
widest pipeline layout declares 6 set(s), 6/6 pipeline layout(s) extended,
1088/1088 pipeline(s) got it, 1437140 bind(s) — OK`.

**The control passed.** The guard band had to move 210 → 358.8 for the run to
mean anything; it measured **358.5**. Everything below rests on that.

### The law, from six cases

Patola took an extra screenshot nobody asked for — the frame at the instant the
cockpit loads, before the fade-in finishes — and it is what made this solvable.
X4 is at a **different `sx` (1.0)** during that transition, which breaks the
degeneracy between "half the shift" and "half the angle" that two points at one
`sx` could never separate.

| frame | sx | yaw | edge | predicted | measured | err |
|---|---|---|---|---|---|---|
| 187 | 0.70024 | 10° | guard | 211.1 | 210 | +1.1 |
| 187 | 0.70024 | 10° | seam | 747.1 | 746 | +1.1 |
| 188 | 0.70024 | 20° | guard | 358.8 | 358.5 | +0.3 |
| 188 | 0.70024 | 20° | seam | 790.9 | 791 | −0.1 |
| 188 fade-in | 1.00000 | 20° | guard | 375.7 | 375 | +0.7 |
| 188 fade-in | 1.00000 | 20° | seam | 828.1 | 827 | +1.1 |

```
guard NDC = (sx·sin θ − cos θ) / (cos θ + sin θ/sx)     the rotated frame's left edge
seam  NDC = sx · tan(θ/2)                                the ray HALFWAY IN ANGLE
```

Half the **shift** is refuted: it predicts 793.7 at 20° against 791 measured,
while half the **angle** predicts 790.9. The earlier "704 + 86.9/2 = 747.5"
coincidence was the right family and the wrong member — at one angle the two are
1 px apart and cannot be told apart, which is exactly why one coincidence was
recorded as a hypothesis and not a finding.

### The mechanism

An angular bisector is the signature of something with **one foot in each
orientation**. X4's common world vertex shader
(`shadergl__glsl__high__common.vert.glsl`) ends its motion-vector path with

```
IO_mv = M_prevworldviewprojection * IO_mv;
```

`M_prevworldviewprojection` sits in the object block beside
`M_worldviewprojection`, and `IO_mv` flows into `p1__deferred__gbuffer_access`
— the G-buffer's motion vector, which `common_taa.glsl` and `motionblur.frag`
consume.

**`patch_vertex_pose_rotate` matches only `BuiltInPosition`.** It rewrites
`gl_Position` and nothing else, and the layer contains no reference to
`M_prevworldviewprojection` anywhere. So every World module we patch emits:

* a **position** carrying our head rotation, and
* a **motion vector** that does not.

The G-buffer therefore reports a constant apparent motion of exactly our head
rotation, for every pixel, forever. A temporal pass that samples the midpoint
between where a pixel is and where it says it came from lands on the ray at
θ/2 — which is the measured law, to 1.1 px, across two angles and two `sx`.

This also explains every qualitative thing observed:

* geometry crosses the seam **unbroken** — nothing is displaced, an accumulation
  term changes;
* only the **sky tone** changes, and the ratio is wildly non-uniform by row
  (4.00 / 0.70 / 0.63 / 0.03 / 0.00 / 3.00) — that is a blur/accumulation
  weight, not a gain;
* the **fade-in appears only inside the band**, because that band is where the
  temporal buffer has content it believes is valid.

The volumetric-froxel hypothesis from the previous section is **dead**. It was
reached honestly — from X4's shaders and our own "3 compute cannot be" log line
— and it was wrong. Kept above, unedited.

### Why this is not cosmetic

With a **static** test pose the fake motion is constant, and the damage is one
seam. With **real head tracking** the injected motion vector is proportional to
head rotation *rate*, so every head turn drives TAA reprojection and motion blur
with a full-magnitude bogus velocity: smearing and ghosting that scale with how
fast the player looks around. That is a nausea mechanism, not a blemish, and it
would have been extremely hard to diagnose once the pose was live and moving —
the artifact would have looked like "VR head-tracking feels smeary".

Finding it now, with a static pose that pins the seam to a measurable column, is
the whole argument for having driven the rotation from a constant.

### The fix, and what it needs that does not exist yet

The motion-vector path must carry the rotation too — and **not the same
rotation**. The correct velocity includes genuine head motion, because the pixel
really did move on screen. So:

* `gl_Position` takes the **current** frame's `R`;
* the `M_prevworldviewprojection` path must take the **previous** frame's `R`.

`PoseBlock` currently holds `r` and `r_inv` only. It needs `r_prev`, and
`pose_publish()` must roll the current value into it once per frame. With a
static pose `r_prev == r`, the two agree, and the seam must vanish — which is
the acceptance test, and it is a **prediction registered before the fix**:

> With `r_prev = r` wired and `X4VR_POSE_YAW=20`, the seam at x=791 disappears
> while the guard band stays at 358.5. If the guard band moves, the fix touched
> the position path and is wrong.

Do this in the harness first — a module declaring both object-block members,
asserting the patch rewrites both and that `spirv-val` accepts it — before
spending a take.

## RETRACTION: the motion-vector mechanism is NOT supported by X4's modules

The section above ("Take 188 — the seam is SOLVED") names a mechanism. **The
mechanism is refuted. The measurement is not.** Both halves matter and they are
kept apart deliberately.

### What still stands

`seam NDC = sx · tan(θ/2)` and `guard NDC = (sx·sinθ − cosθ)/(cosθ + sinθ/sx)`,
fitted to **1.1 px worst case over six cases** spanning two angles and two `sx`
values. Nothing below touches that. The guard-band control passed at 358.5
against 358.8 predicted, so take 188 is sound.

### What is refuted

I claimed the seam came from X4's motion-vector path, on the strength of
`IO_mv = M_prevworldviewprojection * IO_mv` in `high__common.vert.glsl` plus
our own "3 compute cannot be" log line. Scanning all 409 dumped modules for an
access chain whose **base is the variable of the block holding that member** and
whose first index is 2:

```
409 modules (394 vertex); 341 have M_prevworldviewprojection at member 2
of a bound block; 0 actually ACCESS that member.
```

**341 declare it and none reads it.** `common.glsl` declares the whole block, so
every module that includes it names the member whether or not it uses it.
Declaring is not reading — the same distinction that cost take 186b, where 409
modules' *declared* descriptor sets were used to conclude something about the
*pipeline layouts* X4 binds. Second time in three days, in the same file, from
the same 409 dumps.

The first count I ran said "303 modules access member 2" and that number was
wrong: it matched an index-2 access chain into **any** base, not into the block
in question. A predicate that reads something other than what it claims is how
this project has produced confident wrong answers before.

### What was built, and what it is worth

The `r_prev` work is done and it is *correct*, but it currently patches **zero**
real modules, so it is **not a fix for the seam** and must not be run as one:

* `PoseBlock` grows `r_prev` (192 B, three mat4 at 0/64/128). Members 0 and 1
  keep their offsets, so `patch_vertex_pose_clip` and
  `patch_fragment_invproj_eye` read the same buffer unchanged.
* `pose_set_euler` seeds `r_prev = r` — the no-head-motion case. Seeding it with
  identity would announce a full rotation's worth of motion on the first frame.
* `pose_roll_frame()` advances it; with a static pose it is a no-op by
  construction.
* `patch_vertex_pose_rotate` finds the motion-vector output **structurally** —
  block member → `OpLoad` → `OpMatrixTimesVector` → `OpStore`, taking that
  store's pointer — rather than by the debug name `IO_mv`. It applies the same
  conjugation, factored into one lambda so the position and the motion vector
  cannot drift apart.
* Offline over all 409 dumps: **384 patched, 25 refused, 0 spirv-val failures,
  0 motion vectors** — unchanged from before the work, which is the point.
* The layer now logs `pose: N module(s) took the MOTION VECTOR correction`. If
  that line never appears in a run, the correction reached nothing. **A fix that
  patches no module must never be mistaken for a fix that worked**, and the only
  way to guarantee that is to make the count visible.

### Do not spend a take on this

The acceptance test written in the previous section — "with `r_prev = r` the
seam vanishes" — **cannot pass**, because nothing was patched. Running it would
burn a take to observe an unchanged frame.

### What the next question actually is

The seam's law is exact and its cause is unknown. `sx·tan(θ/2)` is the ray
halfway *in angle* between X4's view axis and ours, so something still has one
foot in each orientation — but it is not the prev-MVP path in any module X4
loaded. Candidates not yet excluded, and none of them may be asserted before
the same kind of check that killed this one:

* a **compute** stage — the log says 3 compute modules cannot take the fragment
  `R⁻¹`, and compute is invisible to every render-pass instrument we have;
* a pass reading `M_view`/`M_viewinverse` while its geometry took our clip-space
  rotation, which is a mismatch of exactly one rotation;
* something outside the 409 dumps entirely — that set is what one run loaded,
  and its completeness has never been established.

The last point deserves settling first and costs nothing in-game: **compare the
module count X4 creates in a run against 409.** If X4 creates materially more,
every offline sweep in this project has been over a sample, not a population.

### The module-count check — what it could and could not answer

**It could not be answered from the existing logs, and the numbers that look
like they answer it do not.** `patched vertex shader #350` is not a total: that
line prints every 50th (`patched % 50`), so it reports a **floor**. The pose
counter prints every 100th, so its "300" is a floor too. Reading either as a
count is the take-186b mistake in miniature — using an artifact's shape as if it
were a measurement.

There was **no total-modules counter in the layer at all**, and the per-patch
counters cannot substitute: they count modules a patch *matched*, and a module
matching nothing is invisible to every one of them.

**What is established offline:** the dumps are `mod-0000.spv … mod-0408.spv`,
**409 files, sequential with no gaps**, so the run that produced them created
exactly 409 modules and the capture is complete *for that run*. That is a real
fact and it is narrower than how "409" has been used: it licenses "409 in the
dump run", not "409 is what X4 has".

**What is now instrumented:** `x4vr_CreateShaderModule` counts every module by
stage from its own `OpEntryPoint`, and the end-of-run report says

```
modules: N created this run — V vertex, F fragment, C compute, O other.
The offline sweeps quoted in docs/ are over 409 dumped modules, so this run
{saw MORE than the dumps hold: every "N of 409" figure is over a SAMPLE,
 not a population | stayed within them, which is consistent with 409 being
 the whole set but does not prove it for scenes this run never entered}
```

Verified on the harness, which creates 4 modules and reports `4 created — 2
vertex, 2 fragment`. It costs nothing and rides along on the next take of any
kind; **no run should be spent on this question alone.**

Note the wording of the negative branch. A run staying under 409 does *not*
prove 409 is the population — it proves only that this scene did not exceed it.
The map, station interiors, other ships and the menu all load geometry these
takes never touched. The claim to avoid is the one this project keeps making:
treating a complete survey of one situation as a statement about all of them.

### The seam, after the retraction: what the 94 present dumps add

Take 188's PNG present dumps had not been looked at. They are 47 frames x 2
layers at the exact 1408x1408 eye extent with no compositor in the path, and
they carry three things the screenshots could not.

**1. The law holds across THREE cameras, not one.** X4 ran four `sx` values in
take 188. Predicting *both* edges for each and checking against the steps the
dumps actually contain:

| sx | guard predicted | seam predicted | found |
|---|---|---|---|
| 0.70024 | 358.8 | 790.9 | guard 358, seam 790 |
| 1.00000 | 375.7 | 828.1 | guard 375, seam 827 |
| 1.33333 | 419.3 | 869.5 | seam 869 |
| 3.78085 | 945.5 | 1173.3 | — (that camera draws little) |

Five matched predictions, worst error ~1 px. **Both edges scale with each
camera's own `sx`**, so whatever draws the seam uses the camera projection —
it is not a fixed screen-space artifact, not the compositor, and not the
capture path.

**2. Two cameras' edges coexist in one frame.** Frame 16 carries steps at
**358, 375 and 827** together — the `sx=0.70024` guard *and* the `sx=1.0` pair.
X4 composites several cameras into one presented image, which this project
already knew ("One number, several cameras") and which now has a picture.

**3. The seam is stable for the whole of gameplay.** Frames 17–44 read 358 and
790 every time, to ±1. Frames 1–3 are the load transition at `sx=1.0` and
`sx=1.33333`; frames 0, 4, 45, 46 are black.

### What the law MEANS, verified rather than asserted

Running the gather map backwards from the seam, over four `sx` and four angles:

```
worst |pre-image + seam| = 1.1e-16
the rotation maps  -sx*tan(y/2)  EXACTLY onto  +sx*tan(y/2)
```

**The seam sits where our rotation maps a point onto its own mirror image about
the screen centre.** That is a much sharper constraint than "half the angle": it
says something in X4 is *symmetric about the view axis*, and our rotation
carries one side of it onto the other, so the two disagree exactly there.

That is checked numerically to 1e-16 before being written down, which is the
lesson from the retraction directly above: the previous mechanism was a
plausible line of GLSL that no module X4 loads ever executes.

### Where the seam comes from: still unknown, and it stays unknown

**I do not know which pass draws it**, and this file has already carried one
confident wrong answer to that question this week. What is now fenced off:

* **not** the prev-MVP motion vector — 0 of 409 modules access that member;
* **not** a fixed screen-space or compositor artifact — it tracks each camera's
  `sx` across three different values;
* **not** the guard band, which is separately explained to 1 px and moves by a
  different law;
* **not** scene content the rotation moved — the warped prediction is flat
  across it.

The one instrument that would name it is the per-pass capture, and **its
give-up path still has no test** — that is the defect that ate take 179. The
correct order is: give `X4VR_MV_DUMP_AFTER_RP` its missing regression test in
the harness, *then* bisect. Not another guess from a source file.

### The give-up path: defect reproduced, fix written, test NOT achieved

**Status: incomplete, and the bisect must not be run yet.** Recorded plainly
because a half-finished guard that reads as finished is how take 179 was lost in
the first place.

**The defect is real and was mis-described.** The give-up that existed covers
"target reached but it has no doubled colour attachment" — after
`kAfterRpGiveUp` reaches it advances. What had **no give-up at all** is a target
that is *never reached*: a serial that simply does not occur. Nothing
incremented, so it held the round-robin index forever and starved every entry
behind it.

**Reproduced in the harness**, which is the part that matters:

```
X4VR_MV_DUMP_AFTER_RP=5,0   ->  target rp #5: NEVER REACHED
                                target rp #0: NEVER REACHED
```

rp #0 is *capturable* — the existing `verdict: captured` case proves it — and
under this ordering it never captures. One unreachable entry voids the list.

**Why the existing three verdict cases never caught it:** their fixture is
`X4VR_MV_DUMP_AFTER_RP=0,1,5`. The capturable target is **first**, so the index
always advances and nothing ever starves. The suite tested the ordering in which
the bug is invisible — the third instance of that pattern recorded in this file.

**The fix is written** — a give-up counted in cadence *arms*, advancing past a
target the frame never contains — **and it cannot be exercised headless.** The
arm lives in `probe_collect`, which runs from `x4vr_QueuePresentKHR`; the
harness has no swapchain, never presents, and reaches `probe_collect` only at
device destroy with `queue == VK_NULL_HANDLE`, which skips the whole block. The
run above fired the give-up **zero** times.

There is a second obstacle behind that one, and it is the more fundamental of
the two: **the harness renders its passes once.** Even with arming driven
headless, giving up on target 5 leaves target 0 with no further pass-0 end to be
captured at. A single-frame fixture can pin *starvation* but can never
demonstrate *recovery*.

**What it would take:** make `multiview_render` render N frames rather than one,
so both the arm cadence and the recovery have somewhere to happen. That is
bounded work on the harness and it is the honest prerequisite for trusting this
instrument. Until then:

* the starvation reproduction above is real and should become a case the moment
  recovery is demonstrable;
* `X4VR_MV_DUMP_AFTER_RP` must be given **one** target per run, or a list whose
  first entry is known reachable;
* **do not bisect the seam with it yet.** The instrument's failure mode is
  exactly "produces nothing and says little", which costs a take and teaches
  nothing — which is what happened the last time.

### X4VR_TEST_FRAMES — the harness renders N frames, and two traps on the way

**Status: the frame loop works and is verified. Recovery for the round-robin is
still NOT demonstrated.** Recorded as unfinished, because "the harness renders N
frames now" would read as more than was achieved.

**Trap 1: re-submitting a recorded buffer is not a frame.** The first version
looped only `vkQueueSubmit`. That runs the GPU work N times and calls the
layer's pass-end hook **once**, because `vkCmdEndRenderPass` is a *recording*
command. Every frame-counting path in the layer still saw a single frame. The
loop now encloses `vkResetCommandBuffer` → record → submit, which is what a real
frame loop looks like from the layer's side.

**Trap 2: the give-up was gated on the re-arm.** It sat behind
`!g_probe.after_armed`, and a target that is never reached never *disarms* — so
it never re-arms, so the give-up could not run in precisely the case it exists
for. A guard placed inside the path its own failure mode prevents from
executing. It is now evaluated on the frame boundary, where the question
actually belongs: *did the target appear in the window we were watching.*

**What works now**

* `X4VR_TEST_FRAMES=N` re-records and re-submits N times (default 1, capped
  512). At N=1 every existing case is unchanged — full suite green, five shell
  suites green — and at N=6 the frame's own assertions are unchanged
  (`LAYER0_DRAWN=1 LAYER1_DRAWN=1 SAMPLED_NONZERO=1`).
* A headless frame boundary: `x4vr_QueueSubmit` drives the arm tick **only when
  `presents == 0`**, so a real X4 run is untouched and the units of
  `X4VR_MV_DUMP_AFTER_RP_EVERY` stay "presents" for anyone reasoning about it.
* One shared `after_rp_arm_tick()` for both drivers, so the game path and the
  suite path cannot drift.
* The give-up now **fires** — 3 times in a 12-frame run with a starving list,
  where before it fired 0 times ever.

**What still does not work.** With `X4VR_MV_DUMP_AFTER_RP=5,0`, target 5 is
abandoned and the index advances, but **target 0 still never captures**. Since
serials are assigned at `vkCreateRenderPass` (`g_rp_serial++`), rp #0 recurs
every frame and the pass-end certainly happens, so the remaining fault is in the
capture arming or in `g_probe.pending` — which is drained in `probe_collect`,
and headless that runs only at device destroy. That is the next thing to read,
and it is a *different* bug from the two fixed above.

**Consequence, unchanged:** the seam bisect stays blocked. Give
`X4VR_MV_DUMP_AFTER_RP` one target per run, or a list whose first entry is known
reachable.

### The pending latch: fixed, and recovery still blocked one layer down

**The latch was real and is fixed.** `g_probe.pending` is cleared only in
`probe_collect`, which a real run reaches from present. With no swapchain the
first capture latched it true forever and every later one was refused. The
headless frame boundary now drains each frame via
`probe_collect(d, queue, /*count_frame=*/false)` — it empties the staging
buffer without pushing the present counter that a real run's cadence is
expressed in, so X4 is untouched.

**Recovery still does not happen, and the reason is now known and is mine.**
`X4VR_TEST_FRAMES` wraps the recording of the **output** pass, at
`multiview_render.cpp:~1018`. The harness records a *first* command buffer far
earlier, at ~384, and that one contains rp #0 and the other early passes. So the
loop re-records only the tail of the frame; rp #0 and rp #4 end exactly once,
early, while the target is still the unreachable one, and never end again.

The evidence that pins it: with `0,1,5` rp #0 reports **captured**, and with
`5,0` the same rp #0 reports **NEVER REACHED**. The pass did not change — only
whether it was the current target at the single moment it ended.

That also corrects a reading of the verdict line worth writing down:
**"NEVER REACHED" does not mean the pass never ended.** `after_rp_reached()` is
populated only for the pass that is the *current target*, so the verdict means
"never ended while it was being watched". A pass that ends before the
round-robin reaches it is indistinguishable, in that report, from one that does
not exist. That is a second instrument reporting something narrower than its
words claim.

**What remains, and it is small:** extend `X4VR_TEST_FRAMES` to enclose the
first command buffer's recording as well, so every pass in the frame recurs.
Then the starving-first case can finally demonstrate recovery and become a
regression test.

**Unchanged:** the seam bisect stays blocked. One target per run, or a list
whose first entry is known reachable.

### The give-up now works, and has a test that can fail

**Done.** `X4VR_MV_DUMP_AFTER_RP` no longer starves on a bad entry, and the
suite proves it with a pair that differs by one knob:

```
round-robin: one frame starves rp #0   target rp #0: NEVER REACHED
round-robin: give-up recovers rp #0    target rp #0: captured
```

Same list (`5,0`), same targets, `X4VR_TEST_FRAMES` 1 against 12. The first is
take 179's defect reproduced; the second is it not happening.

**Four things had to be true at once**, and each was independently sufficient to
hide the bug:

1. **The give-up for a never-reached target did not exist.** One existed for
   "reached but no doubled attachment"; a serial that simply never occurs
   incremented nothing and held the index forever.
2. **It was gated on `!after_armed`** — and a never-reached target never
   disarms, so the guard sat inside the branch its own failure mode skips.
3. **`g_probe.pending` latched.** It is cleared in `probe_collect`, which a real
   run reaches from present; with no swapchain the first capture blocked every
   later one. Now drained per frame with `count_frame = false`, so the present
   counter a real run's cadence uses stays untouched.
4. **`X4VR_TEST_FRAMES` covered only the tail of the frame.** The harness
   records *two* command buffers; the loop wrapped the second. rp #0 lives in
   the first, so it ended exactly once no matter what N said. Both recordings
   are now inside the loop, which is what "render N frames" has to mean.

**The fixture was the root of it.** The three verdict cases use `0,1,5` — the
capturable target **first** — so the index always advances and nothing can
starve. They passed over a missing give-up, then over an unreachable one, then
over a latched buffer. A test whose fixture is ordered so the bug cannot occur
is not a weak test, it is a *reassuring* one, which is worse.

Also corrected: **"NEVER REACHED" does not mean the pass never ended.**
`after_rp_reached()` is populated only for the *current* target, so the verdict
means "never ended while it was being watched". That wording sent me looking for
a nonexistent render pass when the pass was ending fine, just too early.

**The seam bisect is now unblocked.** The instrument can be given a list without
one bad entry voiding it — though a list whose first entry is known reachable
remains the cheaper habit.

## Take 189 — the seam bisect

The instrument is fixed and has a test that fails without the fix, so the
round-robin can finally be given a list without one bad entry voiding it.

### The chain to walk, from take 99's inventory

The eye image is `[97H]`. The passes writing it, in serial order:

```
13 17 22 23 24 25   STEREO (world)        ..and 44 50 53 58
14 15 26..32 46..49 51 52   MONO (fullscreen post)
```

Only a **doubled** colour attachment can be captured, so the STEREO passes are
the reliable targets and the MONO ones may report `SKIPPED — no doubled colour
attachment`. That is now harmless: the give-up advances past them.

**The targets:** `13,17,22,25,29,32,44,49,53,58` — spaced to bracket, not to
enumerate. The first dump that shows the seam brackets it between itself and the
previous one; a second run narrows inside that bracket.

### The premise that has to be checked, not assumed

**Take 99's serial numbering may not be take 189's.** Serials come from
`g_rp_serial++` at `vkCreateRenderPass`, so they follow creation order, and take
99 ran with different knobs. Take 179 died of exactly this class — a bisect
built on an assumed pass structure.

So the run carries `X4VR_MV_INVENTORY=1`, which makes the log record **this
run's own** serial → pass mapping. If the targets land wrong, the log says what
they should have been and the next run is a re-target rather than a re-discovery.
The give-up means wrong targets cost cadence, not the run.

### Predictions, before the measurement

1. `score_run.py` exits 0 with `pose-channel … — OK`.
2. Each captured dump is 1408×1408 per layer. In the ones that show the seam it
   is at **x = 791 ± 2**, and the guard band at **x = 358.5 ± 2** — the same two
   edges take 188 measured, since nothing about the geometry changed.
3. **The seam appears at some pass and not before it.** The first target showing
   a step at 791 names the bracket. If *every* captured pass shows it, including
   rp #13, then it is introduced at or before the first world pass and the
   bracket moves earlier — that is a result, not a failure.
4. If **no** captured pass shows it while the presented frame does, the seam is
   introduced after the last target, i.e. in the composite or a present pass —
   also a result, and it would point at our own SBS path rather than X4's.

### The command, in full, on one line

```
X4VR_TAKE=189-SEAM-BISECT X4VR_POSE=1 X4VR_POSE_ROTATE=1 X4VR_POSE_YAW=20 X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_BINDLESS_MIRROR=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS=1 X4VR_SBS_LAYERS=2 X4VR_SBS_RIGHT_LAYER=1 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_MV_INVENTORY=1 X4VR_MV_DUMP_AFTER_RP=13,17,22,25,29,32,44,49,53,58 X4VR_MV_DUMP_AFTER_RP_EVERY=90 X4VR_MV_DUMP=/tmp/x4vr-t189 X4VR_LOG=/tmp/x4vr-take189.log ./launch/x4vr-launch.sh
```

Diffed against take 188's logged `env: run =`: three knobs added
(`X4VR_MV_INVENTORY`, `X4VR_MV_DUMP_AFTER_RP`, `_EVERY`), one dropped
(`X4VR_MV_DUMP_PRESENT` — the named-pass capture and the present dump contend
for the same single staging buffer, and the named-pass path skips the sweep
entirely while armed). Yaw stays 20° so the edges are the ones already measured.

**In-game:** load the quicksave, sit still in the cockpit, and **stay there for
about 60 seconds** — ten targets at 90 presents each, plus two arms for any
target that turns out unreachable. No screenshot needed; the dumps are the
measurement. Quit normally.

**Cost:** one run, ~2½ min. Each capture is a full-resolution readback and a
stall, so expect ten visible hitches. That is the instrument's price and it is
why the cadence is 90 and not 1 (take 98 ran it at 1 and dropped to ~1 fps).

**Acceptance:**

```
python3 tools/score_run.py /tmp/x4vr-take189.log
grep -a 'target rp #' /tmp/x4vr-take189.log
ls /tmp/x4vr-t189-*.png
```

`score_run.py` must exit 0 first. Then the per-target verdicts, then the images.
Read the verdicts **before** the images: "captured" is the only one that means a
file is worth looking at, and `NEVER REACHED` now means "never ended while it
was the current target", not "does not exist".

## Take 189 — RESULT: the seam is a fullscreen post pass, not world geometry

`score_run.py` exit 0, `pose-channel … 6/6, 1107/1107, OK`. The instrument
worked: 6 targets captured, 1 skipped, 3 never reached, 58 dumps, and **no
starvation** — the give-up did its job on a real run.

### The bracket

| target | dumps | content | guard @358 | seam @791 |
|---|---|---|---|---|
| rp #13 | 5 | mean 0–1 | — | — |
| rp #17 | 6 | mean 0–1 | — | — |
| rp #22 | 6 | mean 0–2 | — | — |
| **rp #25** | 4 | **mean 21–23** | **0.0** | **1.0** |
| rp #29 | 4 | 352×352 | out of range | out of range |
| **rp #32** | 4 | **mean 23–30** | **19.0** | **14.0** |

**rp #25 has a lit frame and no seam. rp #32 has a lit frame and both edges.**
Their capture indices interleave (n0,2,4,6 against n1,3,5,7) at comparable mean
luma, so they are the same scene moments apart — like for like.

**The seam is introduced between rp #25 and rp #32.** From take 99's inventory
that span is rp #26–#32, and every one of them is a **MONO (fullscreen post)**
pass on the eye image `[97H]`. The world geometry passes are not where it comes
from.

Two further facts from the same table: **rp #29 renders at 352×352**, a quarter
of the eye in each axis — a downsampled post chain sits inside the bracket. And
`rp #44: SKIPPED — no doubled colour attachment`, which is the verdict working
as designed rather than a failure.

### What this does NOT establish

**rp #13, #17 and #22 are inconclusive, not clear.** All their captures came
back at mean 0–2, i.e. they never caught a lit frame — their per-target `n`
counters run to n14–n16 while #25/#32 stop at n6/n7, so those targets were
sampled during loading and the menu. Absence of a seam in a black frame is not
absence of a seam. They must be re-captured before anything is said about them.

That is the same error the earlier "NEVER REACHED" wording invited, in a new
costume: a report that looks like evidence of absence when it is absence of
evidence.

### Next: narrow inside the bracket

One run, targets **26, 27, 28, 30, 31, 32** — every pass in the span, since
there are only six. Same knobs otherwise. The first that shows a step at x=791
names the pass, and #32 is included as the positive control: it must still show
the seam or the run is not comparable to this one.

The one thing to fix in the protocol: **the early targets sampled the menu.**
Ask for the cockpit to be reached first and held, or raise
`X4VR_MV_DUMP_AFTER_RP_EVERY` so the first arm lands after the scene is up.

### The module count, riding free

```
modules: 389 created this run — 374 vertex, 5 fragment, 10 compute, 0 other
```

**Under 409, so this run stayed inside the dump set** — the offline sweeps are
consistent with it, though as the counter's own wording says, that does not
prove 409 is the population for scenes this run never entered.

Corroborated rather than trusted: `spirv-dis` over 60 dumps gives 51 vertex,
5 fragment, 4 compute, which matches the classifier's shape. Worth noting for
later: only ~5 % of X4's modules are fragment shaders, yet `invproj` reports
"244 of 409 eligible" — so `patch_fragment_invproj_eye` is evidently not
restricted to fragment stages despite its name. Not chased here, but that name
is misleading and some count somewhere is measuring a different population than
it claims.

## Take 190 — RESULT: the seam is present by rp #31, and #26/#28/#30 never run

`score_run.py` exit 0, `pose-channel … 6/6, 1131/1131, OK`.

**The premise held.** Take 190's own inventory matches take 99's exactly across
rp #25–#33, so the serial numbering is stable between runs with different knobs.
That was flagged as an assumption before the run and is now checked, which is
the only reason the numbers below can be compared with take 189's.

| target | dumps | lit | guard @358 | seam @791 |
|---|---|---|---|---|
| rp #26 | — | — | **NEVER REACHED** | |
| rp #27 | 9 | **0** | inconclusive | inconclusive |
| rp #28 | — | — | **NEVER REACHED** | |
| rp #30 | — | — | **NEVER REACHED** | |
| rp #31 | 8 | 7 | **14.0** | **10.0** |
| rp #32 | 8 | 8 | **20.0** | **14.0** |

**rp #32 reproduces take 189 exactly** (guard 20, seam 14) — the positive
control passed, so the two runs are comparable.

### What is now known

Three of the seven passes in the bracket — **#26, #28, #30 — never execute**.
They are created and never used in this scene. So the chain that actually runs
between the last clean point and the first dirty one is:

```
#25 clean (take 189)  →  #27 ?  →  #29 ?  →  #31 SEAM  →  #32 SEAM
```

**The seam is introduced at #27, #29 or #31.**

### rp #27 is inconclusive for a different reason than last time

In take 189, rp #13/#17/#22 came back black because they sampled the menu — a
protocol failure, fixed here by holding the cockpit. **That fix worked**:
#31 and #32 got 7 and 8 lit frames in this run.

rp #27 still came back black across all 9 captures *in the same run*. So it was
sampled in the cockpit and its own output is dark. That is a fact about #27, not
a mistimed capture, and it is worth knowing on its own — a pass writing `[97H]`
that leaves it near-black sits oddly between #25 (mean 23) and #31 (mean 25).

### Next, and it is cheap

Target **#29 and #31 only**, with the measurement made size-aware. rp #29
renders at **352×352** — a quarter of the eye per axis — so if it is a
downsampled copy the seam sits at **x ≈ 197.8** and the guard at **x ≈ 89.6**,
not at 791/358. Take 189 skipped it for exactly this reason and that was a
missed measurement, not an absent one.

If #29 shows a seam at 197.8, the introduction is at or before the downsample
and the quarter-res chain is implicated. If #29 is clean and #31 is not, it is
#31 itself — a single pass, nameable from the inventory.

## Take 191 — RESULT: the bisect by capture is EXHAUSTED at #27/#29/#31

`score_run.py` exit 0, `pose-channel … 6/6, 1156/1156, OK`.

* **rp #31 — positive control, reproduced.** 13 of 14 captures lit, seam at
  **x=790 (10)**, guard at **x=358 (15)**. Identical to take 190. The run is
  comparable.
* **rp #29 — 14 captures, NONE lit** (max mean 5.7), measured size-aware so a
  downsampled target could not hide the seam. Not a mistimed capture: #31 got 13
  lit frames in the same run, in the same cockpit hold.

### The final state of the bisect

```
#25  clean (mean 23)
#26  never executes
#27  BLACK  —  9 captures, cockpit
#28  never executes
#29  BLACK  — 14 captures, cockpit, size-aware
#30  never executes
#31  SEAM   guard 358 / seam 790
#32  SEAM   guard 358 / seam 791
```

**The seam is introduced at #27, #29 or #31, and it cannot be narrowed further
by end-of-pass capture**, because two of the three leave their attachment dark.
That was written down as a possibility *before* this run, so it is a reached
limit rather than a surprise.

Both dark passes write `[97H]` and leave it near-black, and take 189 measured
rp #29 at **352×352**. That is the shape of a downsample/auxiliary chain —
bloom or luminance — where the capture grabs a full attachment that the pass
only partly writes. Stated as shape, not identity: nothing here names them.

### Do NOT run another bisect

The instrument has given everything it can. Three passes remain and the method
that distinguished the first four cannot separate them. Another capture run
would produce the same three lines.

**The next method is the pipeline inventory** — `X4VR_PIPE_INVENTORY` and
task #42's "log each module's classification against the passes that bind it".
That names the shader modules bound by #27, #29 and #31, and a named module can
be read from `/tmp/x4-shadersrc` directly. One run, no images, and it answers
"what are these three passes" rather than "which one looks different".

Worth noting for whoever writes that run: **#26/#28/#30 never execute** and
**#27/#29 write dark**, so five of the seven passes in the original bracket are
invisible to every image-based instrument this project has. An inventory that
reports what X4 *created* will list all seven; only the pass-end record says
which four actually ran.

## Take 192 — the pipe inventory does NOT answer the question, and the scorer FAILED

Two results, both negative, both worth having.

### 1. The scorer FAILED, on a different defect that was dormant

```
FAIL  8 camera-positioned module(s) draw in STEREO pass(es) rp #13, rp #18, rp #23
      without the affine — world geometry left in X4's frustum, mono, while the
      rest of the pass is canted by 30.07°. p1_star (the suns and bright stars)
      is in this set. Re-run with X4VR_SHEAR_LIGHTS=1
```

This is **#49**, not the seam. It matters that it appeared now: the check needs
`X4VR_PIPE_INVENTORY` to see camera-positioned modules, and **every take from
186 to 191 ran with the inventory off**, so this check was dormant through all
of them. Those runs were not clean — they were unexamined on this axis, and
their `PASS` lines should be read with that qualifier.

### 2. The inventory reports the wrong render pass for this question

`module_map.py` lists, for the bracket:

```
rp #26.0   mod-0152, mod-0166, mod-0168   NonWorld,camera-positioned,MONO,procedural,clip
rp #28.0   mod-0168, mod-0181            "
rp #30.0   mod-0189, mod-0366, 0368, 0370 "
```

**It lists exactly the three passes that never execute, and says nothing about
#27, #29 or #31 — the three that do.** That is the inverse of what was predicted
before the run, where the worry was that it would list all seven.

The cause is almost certainly Vulkan's **pipeline/render-pass compatibility**: a
pipeline records the render pass it was *created against*, and may then be used
with any compatible one. X4 creates against #26/#28/#30 and draws through
#27/#29/#31. So the inventory answers "which modules named this pass at
creation", and the question is "which module did this pass bind at draw".

That is the same defect class as every other instrument in this chain, for the
fifth time: **the record is of a different event than the one being asked
about.** `NEVER REACHED` meant "not while watched"; `after_rp_reached` recorded
only the current target; the fixture ordered the bug away; the give-up sat
inside the branch its failure mode skipped. Now the inventory keys on creation
where the question is about use.

### What would answer it

A **bind-time** record: at `vkCmdBindPipeline`, log the pipeline together with
the render pass currently being recorded. The layer already tracks the current
pass for the mask/canvas logic, so the join exists; it is the logging that keys
on the wrong end. Until that lands, `module_map.py` output must not be read as
"pass N uses module M".

**Do not re-run take 192's shape.** It will produce the same three passes.

### Bind-time attribution — built, with a case that fails without it

`pipe: rp #N` answers *which modules named this pass at creation*.
`bind rp #N` answers *which module did this pass actually bind*. Take 192 needed
the second and got the first.

**The harness case came first and failed first.** `X4VR_TEST_XRP=1` creates
`rp_alias`, a distinct render pass with **identical create-info** — hence
compatible — and draws the pipeline created against `rp` inside it. That is X4's
shape exactly: create against one pass, draw through a compatible other.

The assertion is a **discriminator, not a fixed serial**: count the distinct
render passes that bound a pipeline, with the alias and without, and require
exactly one more with it.

```
ok   bind-time: alias pass is attributed    2 pass(es) bind, 3 with the alias
```

Negative control — the log line disabled, everything else identical:

```
FAIL bind-time: alias pass is attributed    wanted 0+1 binding passes, got 0
```

**My first version of this case was wrong and the fix is worth recording.** It
hard-coded "the alias is the last render pass created" and read the highest
serial, which was 7. The alias is created immediately after `rp`, so it is
serial **1**. The record was working and the test was looking in the wrong
place — the sixth time in this chain that the instrument and the question were
about different things, and the first time it was the *test* rather than the
layer. Counting a set difference removes the guess entirely.

**Cost:** an `unordered_map` lookup and a `set` insert per `vkCmdBindPipeline`,
deduplicated per (pass, pipeline), so a pipeline bound every frame prints once.
No readback, no stall. Unlike the capture, this can be left on for a whole run —
which is what makes it usable on passes that write nothing visible, the exact
case that ended the bisect.

**Still to do:** `module_map.py` reads `pipe:` lines. It needs to read `bind`
lines too, or its output will keep meaning "created against" while the question
is "used by".

### module_map.py now reads bind lines, and says which view it is showing

Three states, and it names which one it is in rather than printing a table that
looks the same either way:

* **`bind` lines with module detail** → a `BOUND AT DRAW` section first, then
  the creation view, plus an explicit diff:
  *passes named at CREATION but never seen binding* and *passes that BOUND a
  pipeline created against another pass*. That disagreement **is**
  render-pass compatibility, and it is the whole reason the section exists.
* **`bind` lines with no module detail** → says so, and why (the pipelines'
  modules were never inventoried).
* **no `bind` lines** → says the build predates bind-time attribution and
  labels the remaining table `CREATED AGAINST (may not be the pass that used
  it)`.

Verified against both a take-192 log (no bind lines) and a harness log (bind
lines, unpatched modules), which is what caught the bug below.

**The message was keyed on the wrong thing first.** It tested the *parsed
modules* rather than the *lines*, so a log that had bind lines carrying no
module detail reported "this build predates bind-time attribution" — absence of
parseable content dressed as absence of the feature. That is the seventh
instance in this chain of a report whose words are wider than what it measured,
and it appeared *in the fix for the sixth*. It is keyed on `binds` now.

Both views use the same module-parsing regex, so the creation list and the bind
list cannot drift into disagreeing for a reason other than the one being
studied.

## Take 193 — the bind record works, the JOIN does not

`pose-channel … 6/6, 1088/1088, OK`. Scorer FAILs on **#49** again (9
camera-positioned modules in rp #13/#18/#23), which is the dormant check from
take 192 and not this run's subject.

**The bind-time record fires and attributes correctly.** 179 distinct
(pass, pipeline) binds, naming real serials — rp #0, #33, #15 — including
passes the creation view never mentions. That half works.

**Every one of them reads `(no inventory entry)`.** 1073 `pipe:` lines
captured a description; 179 binds looked one up; **zero matched.** Not a
partial overlap — none.

So the handle the bind path looks up is not the handle the creation path
stored. The most likely cause is that the layer substitutes pipelines
(multiview variants, canvas variants) and the two paths see different sides of
that substitution — `out[i]` at creation versus what X4 later binds. That is a
hypothesis; the diagnostic that settles it is the zero overlap itself, which is
too clean to be a race or a lifetime issue.

**What worked:** `module_map.py` said exactly this, in one line, at the top of
its output — *"179 'bind rp #' line(s), none carrying module detail"*. That
message exists because keying it on the parsed modules instead of the lines was
the bug found while writing it. Had it kept the earlier wording it would have
said "this build predates bind-time attribution" and sent the next reader to
rebuild a layer that was working.

**Where this leaves the seam:** unchanged. Still #27, #29 or #31, still
unnamed. Two runs have now been spent on attribution and neither produced a
name, because both instruments answered a question adjacent to the one asked.

**Do not spend take 194 until the join is fixed and a harness case covers it.**
The existing `bind-time: alias pass is attributed` case counts *passes*, not
*module detail* — it passes on exactly the build that produced this log. That is
an eighth instance of the pattern, and this time it is in the case I wrote one
step ago specifically to avoid it: **counting the set difference proved
attribution and said nothing about content.** The case needs a second assertion
that a bind line resolves to a named module.

### The join, fixed — and it was never the substitution hypothesis

**Cause:** `g_pipe_desc[out[i]] = stagebuf` ran at ~line 5942 while
`d->CreateGraphicsPipelines` fills `out` at 5959/5972 — **twenty lines later**.
The description was keyed on a handle that had not been written yet. Take 193's
179 binds all read `(no inventory entry)` for that reason and nothing else.

I called it "most likely pipeline substitution — multiview or canvas variants".
That was wrong, and the zero-overlap figure I cited as evidence *for* it is
better evidence against: a substitution split would leave some overlap, and
reading uninitialised memory leaves exactly none. **Zero is not a small number
here, it is a different kind of number**, and I read it as the former.

**Fix:** accumulate by index, publish after creation, through **one lambda
called on both creation paths** — the `!any` early return is a second exit and
forgetting it there is the shape this project keeps producing.

### The case now asserts content, not just attribution

Two cases, and the split is the point:

```
ok   bind-time: alias pass is attributed    2 pass(es) bind, 3 with the alias
ok   bind-time: the line names a module     3/3 bind line(s) name a module
```

Negative control with the broken join restored:

```
ok   bind-time: alias pass is attributed    2 pass(es) bind, 3 with the alias
FAIL bind-time: the line names a module     3 bind line(s), 0 naming a module
```

The first case passes on the broken build — **it did pass, on take 193** — and
the second reproduces that log's exact signature. Counting the set difference
proved attribution and was blind to content; only an assertion about content
catches this.

End to end in the harness, `module_map.py` now prints a `BOUND AT DRAW` section
and, unprompted, the line that take 192 needed:

```
passes that BOUND a pipeline created against another pass: rp #1
```

That is the alias, found by the tool rather than by knowing where to look.

## Take 194 — the three passes are NAMED

`pose-channel … 6/6, 1222/1222, OK`. Scorer FAILs on **#49** (9
camera-positioned modules in rp #13/#18/#23) — the dormant check again, not this
run's subject. The join works: `=== BOUND AT DRAW ===` with real module detail.

| pass | binds | class | variant |
|---|---|---|---|
| **#25** clean | mod-0018, 0020, #41, #42, #385… | **World, live-sx** | **patched** |
| **#27** | mod-0152, 0166, 0168 | NonWorld, **camera-positioned**, MONO, procedural, clip | **original** |
| **#29** | mod-0168, 0181 | same | **original** |
| **#31** seam | mod-0370 | same | **original** |
| **#32** seam | mod-0189 | same | **original** |

**The clean/dirty boundary coincides exactly with a change of module class and
variant.** The last clean pass binds *World* modules on `live-sx`, **patched** —
they carry our rotation. Every pass from #27 on binds *NonWorld,
camera-positioned, procedural* modules taking the **original** variant — they do
not.

That is a coherent mechanism: camera-positioned procedural fullscreen geometry
drawn in X4's **unrotated** frame, over a scene the vertex patch **did** rotate.
The two disagree, and a fullscreen quad's edge is a straight line.

It also connects to the FAIL that has been firing since take 192: #49 is
*camera-positioned modules drawing without the affine*. Same class, same
"camera-positioned means it does not take the transform" shape, in a different
pass group.

### What this is NOT yet

**`-> original` does not by itself mean "wrong".** `needs_original()` excludes
shadow and UI passes on purpose, and #30's canvas is meant to stay screen-locked.
An unpatched camera-positioned module may be correctly unpatched. What the table
establishes is that the boundary in the *image* sits exactly at a boundary in
*variant selection* — a correlation across one transition, not a demonstrated
cause.

The test that would settle it is elimination, not more attribution: make those
modules take the rotation and see whether the seam moves or goes. That is
`X4VR_SHEAR_LIGHTS=1`'s shape for #49, and the seam may want its own equivalent.

### Two caveats from this run

* **`warn  the layer's change budget ran out 131 s into a 144 s session`** — the
  last 13 s produced no proj samples, so anything absent from the proj set may
  simply not have been looked at. It does not affect the bind table.
* The tool's diff shows **25 passes** binding pipelines created against another
  pass, against 29 named-at-creation-but-never-binding. This is not an edge
  case in X4, it is pervasive — take 192's creation-keyed output was
  systematically misleading, not unluckily so.

## Take 195 — RESULT: #49 is FIXED, and take 194's seam hypothesis is DEAD

`score_run.py` **PASS**, exit 0. Two results, and the second is the important one.

### #49 is fixed by `X4VR_SHEAR_LIGHTS=1`

```
pipe  clip=1 cam-pos=0; no camera-positioned module draws uncanted in a stereo pass
```

The FAIL that fired on takes 192, 193 and 194 is gone. The 18 modules — the
instanced light volumes and `p1_star` — now classify World and take `live-sx`.
**That is a real fix for a real defect**, and it should be the default rather
than a knob; filed as such.

### The seam did not move. At all.

| | 190/191/194 | **195, shear-lights on** |
|---|---|---|
| guard @358 | 14–20 | **21 / 22** |
| seam @791 | 10–14 | **15 / 18** |

**And the knob demonstrably reached the modules.** Take 194 showed the seam
passes binding `NonWorld,camera-positioned,MONO,procedural,clip`; take 195 shows
the same modules as `World,procedural,live-sx`. The reclassification happened,
the seam stayed at exactly x=791.

That is the cleanest possible negative: **the intervention reached the target
and the observable did not change.** Take 194's correlation — the image boundary
sitting at a variant-selection boundary — was the coincidence it was explicitly
flagged as possibly being. The camera-positioned classification is **not** the
seam's cause.

### The one thing take 195 adds

Those modules are now `World, procedural, live-sx` and **still read
`-> original`.** Reclassification did not change which variant they take, so
something *else* withholds the patch from them — `needs_original()`, most
plausibly because they are procedural fullscreen, which is #30's canvas
exclusion and may be entirely correct.

**That is the next thread, and it is a code question, not a run.** Read why
`needs_original()` selects the original for a `World,procedural,live-sx` module
and whether the pose rotation is withheld along with the shear. If it is, these
passes draw in X4's unrotated frame *by design* — and the design is what needs
revisiting, not the classification.

**Do not spend a take on it.** The answer is in `needs_original()` and the
harness can exercise it.

## needs_original() — read, and it names the seam's passes itself

`needs_original(rp, subpass)` is **per-pass, not per-module**: it returns
`g_variants.unsheared[rp][subpass]`. So "these modules read `-> original`" was
never about the modules — the *pass* is marked unsheared and every module in it
takes the original.

The rule that marks them, at `x4vr_layer.cpp:2027`:

```c
if (!g_shear_nodepth && !subpass_has_depth(ci, sp)) { unsheared[i] = true; }
```

**A no-depth pass is unsheared by rule**, and the comment above it explains why,
from take 70: shearing a fullscreen quad displaces it per eye while the buffers
it samples stay put, so every fragment reads the wrong texel — measured as 85 %
disagreement between the eyes, with the hull shadowed in one and blown white in
the other.

**That comment names the seam's passes explicitly:**

> *"#57, the lighting output written by the no-depth rp #31/#32, disagrees by
> 85 %"*

rp #31 and #32 are the **deferred lighting output**. The bisect landed on them
from the other end — five runs of capture and attribution — and the code had
them written down all along, in a comment about a different defect.

### What that makes the seam

The lighting output is a fullscreen quad that must **not** be transformed — it
is correctly unsheared, and the pose rides the same exclusion deliberately. But
it *reconstructs* world position from a G-buffer whose geometry **was** rotated.
The transform must not reach the quad; it must reach the **reconstruction**.

That is precisely what #61's fragment `R⁻¹` was built for, and #61 executed for
the first time in take 187 on 228 modules. So the live question is no longer
"which pass" but **"does the `R⁻¹` correction actually reach rp #31/#32's
reconstruction, and is it the right correction there"** — a code question about
`patch_fragment_invproj_eye`'s coverage, answerable offline against the dumps.

**Not a claim that this is the cause.** It is a lead with the same shape as the
`M_prevworldviewprojection` one that died: plausible, code-grounded, untested.
The difference is that this time the passes were identified by measurement
first, and the code found afterwards.

## X4VR_SHEAR_LIGHTS is now the DEFAULT

Take 195 settled #48/#49: with it on the scorer reports `clip=1 cam-pos=0` and
PASSes, where 192/193/194 all FAILed on 8–9 camera-positioned modules drawing
mono in X4's frustum inside a canted pass.

`X4VR_SHEAR_LIGHTS=0` still turns it off, because the pre-195 takes were
measured under the old classification and their numbers only mean something
against it.

Leaving a verified fix behind an opt-in is how it gets forgotten — and this one
was invisible anyway, since the check that catches it is dormant unless
`X4VR_PIPE_INVENTORY=1`.

## invproj coverage of the seam's modules — the lead is WEAKENED, not confirmed

`frag-invproj-check` over the modules each bracket pass binds:

| pass | module | invproj |
|---|---|---|
| **#31 seam** | mod-0366 | `CHECKED=10 WORST=0.000e+00` |
| **#31 seam** | mod-0370 | `CHECKED=10 WORST=0.000e+00` |
| **#32 seam** | mod-0189 | `FAIL=no_load_to_check` |
| #27 | mod-0152 | `CHECKED=10 WORST=0.000e+00` |
| #27 | mod-0166 | `FAIL=no_load_to_check` |
| #27 / #29 | mod-0168 | `CHECKED=10 WORST=0.000e+00` |
| #29 | mod-0181 | `FAIL=no_load_to_check` |

**Both of rp #31's modules load `M_invprojection` and both take the correction
exactly.** So the reconstruction at the first pass that shows the seam *is*
corrected — and the seam is there anyway.

That is evidence **against** last section's lead as stated ("the `R⁻¹` does not
reach the reconstruction"). It reaches it. Either the correction applied there
is the wrong one for what that pass does, or the seam does not come from the
reconstruction at all.

`FAIL=no_load_to_check` on mod-0189/0166/0181 means those modules never load
`M_invprojection` — there is nothing for the patch to correct, which is not a
gap in coverage but an absence of the thing covered.

**A caveat on this check:** it was run with the eye offsets only, not with a
pose set, so it verifies the per-eye invproj arithmetic and **not** the `R⁻¹`
composition #61 adds. A run of the same check with the pose set supplied would
say whether the `R⁻¹` term specifically is emitted correctly in mod-0366/0370.
That is the next offline step and it costs nothing.

**Status of the seam: still unexplained.** Bracketed to rp #31/#32, three
hypotheses dead (prev-MVP, camera-positioned classification, uncorrected
reconstruction), one narrowed and untested (`R⁻¹` composition in the two
modules that do take the correction).

## #61's R⁻¹ arithmetic has NEVER been checked against a reference

The check asked for cannot be run. `frag-invproj-check` takes
`<in> [set] [binding] [member] [dl] [dr] [oa_l] [oa_r]` — **no pose set** — so
it calls `patch_fragment_invproj_eye` with `pose_set` at its default
`0xffffffff`, i.e. with the `R⁻¹` composition **off**. Every
`CHECKED=10 WORST=0.000e+00` in the previous section verified the per-eye
invproj arithmetic and nothing about `R⁻¹`.

**What #61 actually has:**

* `rinv: reconstruction takes R-inverse` / `rinv: a non-reconstructor refuses` —
  these check that the patch **emits** and that `spirv-val` accepts it, and that
  the right modules refuse.
* `headlook_clip.py --self-check` — checks the identity
  `M_invproj·Rclip⁻¹ = R⁻¹·M_invproj` **on paper**, to 8.9e-16.
* Take 187 — first execution on real hardware, 228 modules.

**What it does not have:** any check that the SPIR-V it emits *computes* that
identity. The offline mode that exists precisely to interpret emitted arithmetic
against `common/x4vr_view.hpp` — and which reports `WORST=` for the eye offsets
— has never been pointed at the `R⁻¹` term, because the CLI cannot pass it one.

That is the ninth instance of this project's dominant pattern, and the sharpest:
**the instrument that verifies arithmetic exists, is used routinely, and has a
blind spot exactly over the newest and least-executed piece of code.** #61 is
the component that had never run at all until take 187; it is also the one whose
arithmetic has never been checked.

### The next step, and it is small

Extend `frag-invproj-check` to accept a pose set and a rotation, and compare the
emitted arithmetic against `x4vr_view.hpp`'s `R⁻¹·M_invproj` the same way it
already compares the eye offsets. Then run it over the 244 eligible modules,
mod-0366 and mod-0370 among them.

**Do this before any further seam work.** If `R⁻¹` is emitted wrongly, the seam
has a candidate cause that no run could have found — and if it is emitted
correctly, that is the fourth hypothesis eliminated and the reconstruction is
cleared for good.

### Extending frag-invproj-check for R⁻¹ — the spec, not the code

**Not started, deliberately.** The check mode is `tests/spirv_patch.cpp:203-612`
— **409 lines** — and its interpreter has **no `OpMatrixTimesMatrix` case**,
which is the opcode #61 emits for the `R⁻¹` composition. This is a real piece of
work and beginning it without room to read the interpreter, extend it and verify
it is how the last several half-built instruments in this file were made.

**What the extension needs, in order:**

1. **A pose-set argument**, passed through to
   `patch_fragment_invproj_eye(..., pose_set, pose_binding)`. Today the CLI
   never supplies one, so the emitted code has no `R⁻¹` term to interpret.
2. **A second uniform block in the interpreter.** It currently seeds one block
   with a known `M_invprojection`; the pose block at the pose set needs seeding
   with a known `R⁻¹` (use `pose_set_euler`, so the reference and the emission
   come from the same header — the property the existing check was built on).
3. **`OpMatrixTimesMatrix` in the opcode switch.** Absent today; #61's patch
   emits exactly one.
4. **The reference:** `R⁻¹ · M_invproj` composed with the existing
   `compose_inv_off_axis` / `compose_eye_untranslate` result, in that order —
   #61 left-multiplies, and getting the order wrong would make the check agree
   with a wrong emission.

**Acceptance, and it must fail first:** point it at mod-0366 with a pose set and
a non-identity rotation and confirm it reports a nonzero `WORST=` when the
reference is composed in the *wrong* order — then fix the order and watch it go
to ~1e-7. A check that has never been seen to fail on this arithmetic is worth
no more than the one that could not see `R⁻¹` at all.

**Then:** run it over the 244 eligible modules, mod-0366 and mod-0370 included,
before any further seam work.

### DONE, and the answer is: #61's R⁻¹ is exact in all 244 modules

Built as specced, plus a second negative control the spec did not ask for.
Costs nothing but a rebuild — no take.

**The two controls, both run BEFORE the result was believed:**

| control | what it breaks | fixture | mod-0366 |
|---|---|---|---|
| `X4VR_CHECK_RINV_FLIP=1` | reference composed `M·R⁻¹`, not `R⁻¹·M` | 1.736e+00 | 1.736e+00 |
| `X4VR_CHECK_RINV_ROWMAJOR=1` | pose block decoded with the other majorness | 4.960e-01 | 4.960e-01 |
| *(neither)* | — | **0.000e+00** | **0.000e+00** |

The second control is why the pose matrix is now **decoded from `PoseBlock`'s
raw bytes** through the offset, stride and majorness the module declares,
rather than copied out of `pose.r_inv`. Seeding `r_inv` directly would have
agreed with *any* layout — and `patch_fragment_invproj_eye`'s own comment
(x4vr_spirv.hpp:3126) warns that a layout mismatch "reads back as a plausible
wrong matrix, not as an error". The check now reads `LAYOUT=off64/stride16/col`
out of the emitted SPIR-V and the suite pins it.

An identity rotation is **refused**: it commutes with everything, so the
flipped order would agree with the right one and the check would pass while
proving nothing.

**The sweep, all 409 dumps, ypr = 10,−7,3:**

| outcome | modules |
|---|---|
| `CHECKED=10 WORST=0.000e+00` | **244** |
| `FAIL=no_load_to_check` | 162 |
| `FAIL=patch_refused` (compute) | 3 |

244 is exactly the eligible count the layer reports offline, which is a
cross-check that the swept set is the same set.

**And the same sweep with the flip control on: 244 checked, ZERO still at
zero, every one of them 1.724e+00.** That is the part that matters — without
it, a module that silently never received the `R⁻¹` term would have scored a
perfect zero and been counted as correct. It did not happen to any of them.

The flipped error is exactly **10·sin(yaw)** — 0.1745 at 1°, 0.8716 at 5°,
1.7365 at 10°, 5.000 at 30° — the 10 being the `1/near` term in the inverted
projection. So the number is a function of the input rather than a constant,
and the correct order stays exact at 1°, 5°, 10°, 30° and 90°.

**Hypothesis four is dead.** #61 emits, validates, executes, *and* computes the
identity it claims.

### ...but at rp #31/#32 the patched module is not the one BOUND

Checking what the result above actually licenses turned up something that
narrows the seam rather than clearing it. From take 195's own log, no new run:

```
bind rp #31  s0=mod#125/e65754b1(World,procedural,live-sx)->original s1=...->original
bind rp #31  s0=mod#127/646039c1(World,procedural,live-sx)->original s1=...->original
bind rp #32  s0=mod#109/888d50c6(World,procedural,live-sx)->original s1=...->original
```

`->original` is not "unpatched". x4vr_layer.cpp:5930 sets it when `got !=
asked` — the module **was** patched and the layer then swapped the *original
twin* back in at pipeline creation. Both stages, all three modules, every
occurrence in the log: those six module entries never appear as `->patched`
anywhere.

**RETRACTED — this paragraph was wrong, and it is left standing because the
retraction is the finding.** It read: *"So the per-eye `M_invprojection`
correction and #61's `R⁻¹` do not execute in the two passes that show the
seam,"* and concluded that the third hypothesis had died to the wrong
instrument.

**The correction DOES execute there.** `g_variants.original` is not the
pristine module: it is built from `frag_only`, captured at
x4vr_layer.cpp:4784 *after* the fragment patches and *before* the vertex ones.
The twin keeps the bindless index offset and the per-eye `M_invprojection`
including `R⁻¹`, and drops only the geometry edit — the constructor's own
comment says so, and `tests/run-multiview-render.sh`'s case *"unsheared twin
keeps the frag patch"* has asserted it on a combined vertex+fragment module
since take 23.

So **hypothesis three was correctly dead the first time.** The offline check's
verdict stood; reviving it was the error.

**What made this survive three readings** is that the code was fixed in take 23
and three sentences describing the old behaviour were not:

* the layer's own log line — *"unsheared pipeline: using unpatched modules"*;
* this file's `needs_original()` bullet — *"binds the **unpatched** module"*;
* `run-multiview-render.sh`'s header — *"binds the UNPATCHED module"*.

All three are corrected as of take 197. I read the log line and two comments
and never read the constructor, which is where the answer had been all along.
**A sentence that describes behaviour the code no longer has is not stale
documentation, it is active misinformation** — and it is more dangerous than
no comment, because it answers the question and stops the search.

18 passes bind a genuinely-World module as `->original`; 64 of 254 bind lines
are `->original` overall. So this is a rule firing broadly, not an accident at
two passes.

**A WRONG TURN, kept because it is the eleventh instance.** I first wrote that
the take-70 no-depth rule at x4vr_layer.cpp:2027 "cannot be the cause", on the
grounds that its branch is `if (!g_shear_nodepth && ...)` and take 195 ran
`X4VR_SHEAR_LIGHTS=1`. **That refutation was itself wrong.** `g_shear_nodepth`
reads **`X4VR_SHEAR_NODEPTH`** (x4vr_layer.cpp:1982). `X4VR_SHEAR_LIGHTS` is a
different knob entirely — it is `wide_camera` at x4vr_layer.cpp:4809, a
per-MODULE classification switch for #48, nothing to do with this per-PASS
rule. I matched a variable to a knob from memory instead of grepping what it
reads, which is the same mistake as hooking a symbol X4 never imports.

### The rule, read rather than guessed

From take 195's own inventory line, and the knob table:

```
rp #31.0: 1 colour [97H] no-depth final=1 -> MONO (fullscreen post) +MASKED(fullscreen)
rp #32.0: 1 colour [97H] no-depth final=1 -> MONO (fullscreen post) +MASKED(fullscreen)
```

One colour attachment, format 97, **H = not LDR**, and **no depth**. Walking
`classify_unsheared` (x4vr_layer.cpp:1999) with `X4VR_SHEAR_NODEPTH` unset,
which it was:

| rule | line | rp #31/#32 |
|---|---|---|
| `colorAttachmentCount == 0` → shadow | 2003 | no, 1 colour |
| `!g_shear_nodepth && !subpass_has_depth` | 2027 | **FIRES — unsheared = true** |
| `g_shear_ui && any && all_ldr` | 2056 | not reached |
| `unsheared = any && all_ldr` | 2060 | not reached (and would be *false*: 97 is HDR) |

**So it is the take-70 no-depth rule, and nothing else.** The last row matters:
had the rule at 2027 not fired, the all-LDR fall-through would have marked
these passes **sheared**, so the two rules disagree about rp #31/#32 and only
one of them is in force.

The chain is now complete: no depth → `unsheared` → `needs_original()` → both
stages bind the original twin → **#22/#39's per-eye `M_invprojection`
correction and #61's `R⁻¹` never execute at rp #31/#32.**

**A latent instrument defect found in passing, not yet a problem.** The
inventory's own `why` string (x4vr_layer.cpp:2352) is computed as
`... : !subpass_has_depth(ci, sp) ? "fullscreen post" : unsheared[i] ? ...`,
which consults `subpass_has_depth` **without** `g_shear_nodepth`. It happens to
read correctly here because the rule did fire — but under
`X4VR_SHEAR_NODEPTH=1` it would still print "fullscreen post" for a pass that
`classify_unsheared` had marked sheared. It recomputes the decision instead of
recording it, which is the defect
[[record-the-branch-dont-recompute-it]] names.

### What this licenses, and the one knob that discriminates

The "one gate, two transforms" shape stated above is now **supported, not
suspected**: `needs_original()` is a single flag disabling the vertex shear
*and* the fragment reconstruction correction, and rp #31/#32 need opposite
answers from the two.

`X4VR_SHEAR_NODEPTH=1` flips exactly this rule and **has never been run — 180
stored logs, zero occurrences.** It is not a fix: it restores the shear too,
and take 70 measured what that costs (#57 disagreeing 85% between the eyes,
the hull shadowed in one eye and blown white in the other). It is a
*discriminator*. If the seam moves or vanishes under it, the seam is in this
gate; the take will look worse in other ways, and that is expected rather than
a regression.

**The prediction, before the run:** the seam at `sx·tan(θ/2)` should change
under `X4VR_SHEAR_NODEPTH=1`. If it does not move at all, the gate is not
where the seam comes from and the reconstruction is cleared a second time — by
the right instrument this time.

**The shape of the suspected defect, stated before it is tested:** one gate,
`needs_original()`, switches off two different transforms — the vertex shear
*and* the fragment reconstruction correction. The take-70 rule exists for the
shear and is right for it (shearing no-depth passes gave #57 an 85% per-eye
disparity). A deferred lighting pass is a fullscreen quad that must NOT be
sheared as geometry and MUST have its `M_invprojection` corrected. If one flag
decides both, that pass cannot get what it needs.

## Take 196: the gate is NOT the seam's cause

`X4VR_SHEAR_NODEPTH=1`, everything else byte-identical to take 195.

**The knob reached the decision it names.** rp #31/#32 — still `1 colour [97H]
no-depth`, so the same passes as in 195 — bound `->patched` on **both** stages:

```
bind rp #31  s0=mod#343/646039c1(World,procedural,live-sx)->patched s1=...->patched
bind rp #32  s0=mod#143/888d50c6(World,procedural,live-sx)->patched s1=...->patched
```

11 eligible passes flipped, 0 stayed. So the per-eye `M_invprojection`
correction and #61's `R⁻¹` **did execute at the seam's passes in this take.**

**And the seam is still there.** Measured as the median signed column-to-column
step over all rows of the left eye — a seam is a coherent full-height
discontinuity, scene edges are local and cancel:

| | guard edge | seam | separation | vs law |
|---|---|---|---|---|
| law (`sx`=0.70024, θ=20°) | −0.49029 NDC | +0.12347 NDC | **432.1 px** | — |
| take 195 | x=360 (step +4) | x=792 (step −4) | 432 px | **−0.1 px** |
| take 196 | x=368 (step +7) | x=805 (step −6) | 437 px | +4.9 px |

**Separation is used because it needs no cross-take registration.** Absolute
x does: the two PNGs differ in height (1410 vs 1407), and content
cross-correlation across three bands returned dx = −5, 0, −3 — three answers,
so that instrument is wrong here, exactly because the knob changed the content
it would have to match on.

**Conclusion, as restated after the retraction above.** The correction was
already running at rp #31/#32 in take 195, so 196 did not "restore" it — what
196 added was the **vertex shear and rotation** at the no-depth passes. The
seam survived that too, and strengthened. So:

* hypothesis three (uncorrected reconstruction) is dead, for the reason the
  original offline check gave;
* **and** the seam is not removed by shearing those passes either.

Two things the seam is not, from one take.

### Two things I got wrong about this run

**The prediction was not falsifiable.** I wrote "the seam should change". A
knob that perturbs eleven passes will change *something*, so that sentence
could not lose. It should have been the pair of specific outcomes: *vanishes*
if the missing correction is the cause, *stays at 432 px separation* if not.
The answer is the second, and the 4.9 px is perturbation, not removal.

**The acceptance check FAILed a valid take.** `shear-nodepth` keyed on
"no-depth passes still binding `->original`" and named rp #0/#33/#46. Those
three are all-LDR (44L/50L/50L) and are held by the rule at
x4vr_layer.cpp:2060, which this knob does not gate. Wrong granularity: no
predicate over "no-depth passes" can split them out, because the classified
object was wrong. Corrected to "no-depth AND at least one HDR attachment",
read from the inventory's own format column, and re-proved against a doctored
log.

### The confound, stated plainly

`X4VR_SHEAR_NODEPTH=1` restores the shear as well as the correction, so this
is not a single-variable test. The seams also got **stronger** (step 4→7 and
4→6), which is what take 70's #57 defect looks like coming back.

What makes the conclusion survive the confound: **the seam sits at
`sx·tan(θ/2)` in both takes, which is a function of the head rotation.** A
shear artifact scales with the eye offset, not with θ. So the feature that
persisted is the rotation-derived one, not a shear artifact that happens to
land in the same column.

**The clean test is the third variant** — unsheared vertices, corrected
fragments — which removes the confound by construction. That is now the work,
not another knob.

## What is left on the rotation side — the complement, enumerated (take 197)

All offline, no take. The method is this file's own: a transform reaching a
subset makes the complement wrong, so **enumerate what does NOT carry it.**

### What the rotation reaches

| | count | source |
|---|---|---|
| vertex conjugation `P·R·P⁻¹` accepted | **384** of 409 | `vert-pose-rotate` sweep |
| refused (not World / no camera or object block) | 25 | same |
| fragment `R⁻¹` in the reconstruction | **244**, all exact | `frag-invproj-check` sweep |

### What it does not reach — and which of those are fine

* **10 compute modules.** No vertex stage, and the invproj patch refuses
  compute, so no patch we have can reach them. Two use `S_sampler3D`
  (mod-0177, mod-0364 — the volumetric froxel grid), two `S_samplerCube`
  (mod-0266/0267 — the skybox).
* **The froxel volume is nonetheless CONSISTENT.** `volume_apply.frag` does
  `RetrieveZBufferViewPos` then `view2volume(view_pos)`, and `view2volume`
  calls `Project()`. The reconstruction is corrected into X4's frame by #61,
  `Project()` uses `M_projection` — which the layer **cannot** alter, since
  `patch_vertex_eye_offset` only *reads* it for live `sx` and a shader cannot
  write a uniform — and the volume was built by compute in X4's frame. The
  round trip closes.
* **The motion-vector path is ABSENT in this configuration.** 341 modules
  declare `M_prevworldviewprojection`; **0 access it** (access chains resolved
  to their base variable, not matched by index); and **0 modules write any
  motion-vector output at all**. So hypothesis 1 is dead for a stronger reason
  than recorded, and **`r_prev` is inert** — correctly built, correctly wired
  (three block members, `c_prev = 2`), and matching **0 of 384** modules
  because there is nothing to correct.
* **TAA is not running.** `taa_apply.frag` writes through `imageStore`, and
  the only `OpImageWrite` users among the 409 are the 10 compute modules. That
  matters because `taa_apply` contains
  `if (reproj_uv outside [0,1]) blend = 1` — a hard binary switch on a
  reprojected coordinate, which is precisely a hard-edge generator. It is not
  firing here.

### What it does not reach, and is NOT accounted for

* **Pure screen-space reads with no reprojection.** `volume_apply` also does
  `RTResolve(S_sampler2D_AUTOMS[PLS_FOG_FLAT + i])` and `GetDepthGodRay()`,
  both at the *current* fragment position. Those are correct only if the pass
  that WROTE those targets carries the same rotation as the pass reading them.
  Unchecked.
* **`depth_godrays_pass.frag`** — the third re-projector. Unread.
* **The skybox** (mod-0266/0267, compute, cube). [[x4-skybox-is-not-one-layer]]
  says it broke all three previous attempts.

### The three unaccounted items, read (take 197)

**1. The compute skybox — ELIMINATED, on principle.** mod-0266/0267 are
cubemap→cubemap compute (`OpTypeImage Cube` sampled in, `Rgba16f` storage out):
an envmap convolution. **A cubemap is indexed by direction, not by screen
position**, so it is rotation-invariant by construction and being unreachable
by every patch we have costs nothing. The planets are separate — real geometry
with vertex shaders, classified and rotated normally.
[[x4-skybox-is-not-one-layer]]'s warning is about stereo depth, not the
rotation frame.

**2. Every rotation refusal is correct.** All **25** modules
`patch_vertex_pose_rotate` declines are NonWorld, fragment-only or compute — 0
of 25 is world geometry. So no geometry anywhere draws unrotated, and the
"unrotated geometry beside rotated geometry" shape is dead as a seam
explanation.

**3. `volume_apply`'s re-projection round-trips; `depth_godrays_pass`'s does
NOT.** Ten fragment modules both `UnProject` and `Project` — six of them are
rp #31's group (mod-0365/0366, 0367/0368, 0369/0370). The distinction between
the two uses is the finding:

* `view2volume` does `Project(UnProject(clip))` and *uses the result*.
  `UnProject` is our patched `M_invprojection` = `R⁻¹·P⁻¹`, so it lands in X4's
  unrotated frame; `Project` is X4's own `M_projection`, which we cannot alter;
  the volume was built by compute in X4's frame. **Closed loop, correct.**
* `depth_godrays_pass` does the same round trip and then **differences it
  against the fragment's own UV**:

      vec3 view1_cs = Project(UnProject(view0_cs) + ldir * 0.1);
      vec3 ray_uv  = clipZ2uv(view1_cs) - view0_uv;

  `view0_uv` is in the **rotated** frame; `clipZ2uv(view1_cs)` comes back
  **unrotated**. So `ray_uv` is *(the light-direction step)* **+** *(the full
  head-rotation displacement)*, and at 20° the second term dominates the first.
  The march built on it then has a **hard early-out**:

      if (IsOutsideUV(uv.xy)) break;

  A rotation-contaminated direction plus a hard boundary — the same shape as
  `taa_apply`'s `blend = 1`, except this one **is** compiled in: the FOM
  harmonic loop of `GetDepthGodRay` is present in mod-0366/0370 (Cos, Sin, Exp
  and six loops), absent in mod-0189.

  X4's own author documents the vanilla version of this: *"avoids a cut-out
  artifact (which I think is due to the incorrect perspective in the extension
  pass)"*, in `common_fom.glsl`'s `GetDepthGodRay()`.

**What this is NOT yet.** A candidate, not a cause. **The seam law argues
against it**, and that argument is the reason not to spend a take on a hunch:
`seam NDC = sx·tan(θ/2)` depends on `sx` and `θ` only, verified to 1.1 px over
six cases and 0.1 px on takes 195 and 197 — and it contains **no light-direction
term**. A god-ray artifact must depend on where the sun is. Every take so far
is the same savegame at the same instant, so the stored data cannot separate
them.

**The discriminator, and it costs no build and no take:** fly until the sun is
in a different direction and look. If the seam moves with the sun, it is the
god-ray march; if it stays at `sx·tan(θ/2)`, the god-ray path is eliminated and
the law's independence from the light is confirmed rather than assumed.

### And one defect found on the way — see the commit

The layer classified the module it had just written: `patch_vertex_pose_rotate`
adds the camera-block access `classify(wide)` treats as proof of world
geometry, and **30 modules** flipped `NonWorld → World` because of it — every
one a procedural fullscreen quad, rp #31's and #32's among them. Live since
take 195 only. Fixed, with a fixture and a paired case; **not yet validated in
game.**

## Take 198: the seam is independent of the light — the god-ray path is dead

Interaction take, keyboard only, rotating in place. Three sun directions, one
run, scorer PASS and the `sx` set unchanged.

| frame | sun | x=359 (guard) | x=791 (seam) | separation |
|---|---|---|---|---|
| 198a | off-screen (load) | +11 on 58.5% of rows | −8 on 62.7% | **432 px** |
| 198b | **ON SCREEN** | +4 on 30.2% | −12 on 35.8% | **432 px** |
| 198c | behind | +6 on 27.2% | −5 on 12.5% | **432 px** |

**Both edges sit at exactly the same two columns in all three.** The contrast
varies — it is a lighting discontinuity, so it tracks scene brightness — but
the position does not move at all.

`depth_godrays_pass` derives its march from `mat3(M_view)·V_direction1`, so a
god-ray artifact MUST move with the sun. It does not. **Hypothesis dead**, and
the law's independence from the light is now measured rather than assumed:
`seam NDC = sx·tan(θ/2)` holds across three very different scenes (46–50% of
pixels differ between frames) at one `sx` and one `θ`.

### Two instruments were wrong before Patola's eye was

He said *"the seam never turned invisible or moved relative to the screen."*

1. **My edge detector reported NOTHING in 198b and 198c.** It ranked columns by
   the *median* signed step over all rows — which requires the seam to span
   **more than half the height**. That held in 195/196/197, framed on open space
   through the canopy; point the ship elsewhere and a station or planet covers
   part of the frame, the seam spans 30% of rows, and a median reports zero.
   Counting the *fraction* of rows carrying a sign-consistent step finds it
   immediately. **An instrument encoding an assumption that held in every frame
   it was built on.**
2. **My "control" could not see the variable.** I used the scorer's `view`
   angles to confirm the camera had rotated. It could not: `X4VR_DUMP_MATRICES`
   caps itself at **8 reports per camera**, all of which fire at load —
   `proj VIEW: cam#N reached 8 reports — further M_view changes from THIS
   camera suppressed`. The whole flying sequence is invisible to it, and 197's
   and 198's angle sets look nearly identical *because both are just the load*.
   The control had to come from the images instead: 46–50% of pixels differ
   between the three frames.

**Neither was a close call, and his eye was right about both.** Consult the
channel you called decisive — [[consult-the-channel-you-called-decisive]].

### What this leaves, and it is a different SHAPE of hypothesis

Every remaining candidate has been about a **transform being wrong**. All are
dead. What survives is a seam that is a pure function of `sx` and `θ` and of
nothing else — not the light, not the scene, not the dispatch, not the
reconstruction.

`x=359` is the **guard-band edge**, predicted by
`(sx·sinθ − cosθ)/(cosθ + sinθ/sx)`. `x=791` is the **bisector**, `sx·tan(θ/2)`.
Both are properties of the widened-field geometry #33 introduces.

So the next hypothesis class is **extent, not transform**: a pass whose
viewport, scissor, or buffer size still describes the *unwidened* field, so its
valid region ends where the original field ended. That would be a pure function
of `sx` and `θ`, independent of light and scene — which is exactly what has now
been measured.

**The cheapest test, and it has never been run:** every take from 187 to 198
used `X4VR_POSE_YAW=10` or `20`. **`X4VR_POSE_YAW=0` with `X4VR_POSE_ROTATE=1`
has never been run at all.** At θ=0 the law puts the seam at NDC 0 and the
guard edge at −1. If both edges vanish, the seam is rotation-geometry and the
extent hypothesis is live; if an edge survives at θ=0, it is the widened FOV
alone and the rotation is not involved.

## Take 199: the seam needs the rotation — the widened FOV alone does not make it

`X4VR_POSE_YAW=0` with `X4VR_POSE_ROTATE=1`. Never run before: takes 187–198
all used 10 or 20. Scorer PASS, `sx` set unchanged, `classify` still 0.

Checked before the run, so the take could not silently become
`POSE_ROTATE=0`: `pose_rotate()` reads only `X4VR_POSE_ROTATE`, so every module
is still patched and the set still bound; the yaw block is guarded by
`if (yaw != 0 || pitch != 0 || roll != 0)`, so the value stays
`pose_identity()`. **The whole #33 machinery ran and computed identity.**

| frame | yaw | x=359 | x=791 | background |
|---|---|---|---|---|
| 198a | 20 | 58.5% of rows (1 column beats it) | **62.7% (0 beat it — strongest in frame)** | med 11.5%, p95 16.9% |
| 199 | **0** | **6.8% (1181 columns beat it)** | **12.2% (399 beat it)** | med 10.6%, p95 16.0% |

At θ=0 both predicted columns fall **to or below background** — x=359 is below
the median of all columns. Not weaker: indistinguishable from ordinary scene
texture. The frame's strongest edge is at x=613, which is scene content.

**`X4VR_FOV=1.4917` was on throughout.** The widened field was fully in effect
and produced no seam. **So the widened FOV alone is not the cause — the seam
requires a non-zero rotation.**

**The confound, which Patola named himself:** at yaw=0 the seam and the
rotation vanish *together*, so this take alone cannot separate "the rotation's
value causes it" from "anything that co-varies with a non-zero rotation". What
narrows it is the sx-dependence, and that is the next take.

### A gap in the record worth naming

**Every take from 186b to 199 — fourteen runs — used `X4VR_FOV=1.4917`.** So
the law's "verified over three `sx` values" came from the *several cameras
inside one frame*, never from moving the knob. The knob has not been varied
once during the entire seam investigation.

### PREDICTIONS for the next take, committed before it runs

`X4VR_FOV=1.75` at yaw 20. X4's `<fov>` is a linear scale on the half-angle —
`full degrees = X4VR_FOV × 73.7399`, the layer's own law at
x4vr_layer.cpp:9482, which reproduces all four of take 199's cameras — so
`sx = 1/tan(36.87° × fov)` and fov 1.75 gives `sx = 0.4765`.

| | fov 1.4917 (now) | fov 1.75 (predicted) | move |
|---|---|---|---|
| guard edge x | 358.8 | **374.1** | **+15.3, RIGHT** |
| seam x | 790.9 | **763.1** | **−27.8, LEFT** |
| separation | 432.1 | **389.1** | −43.0 |

**The two edges move in OPPOSITE directions.** No capture offset, crop or
uniform scaling can produce that, so it is a confound-proof test of both laws
at once — and the guard formula has never been tested against a knob change at
all.

**It also answers #56**, which the scorer has printed a request for on every
run since take 148: *"needs the SAME line from a run at a different
X4VR_FOV"*. Fitted-to-frustum makes the cascade span grow with the field;
fixed-extent does not. One run, two long-standing questions.

The ceiling is not a risk: take 158 measured X4's `<fov>` honoured at **2.21**.

## Take 200: both laws confirmed to 0.1 px, in OPPOSITE directions

`X4VR_FOV=1.75`, single-knob change from take 197. Scorer PASS. X4 honoured it
exactly — `sx=0.47650`, `129.044°`, against 0.47650 / 129.045 predicted — and
the other three cameras did not move, which is the control that says the field
changed and nothing else.

| | predicted | measured | error | motion from fov 1.4917 |
|---|---|---|---|---|
| guard edge | 374.1 | **374** (54.3% of rows) | **−0.1 px** | 359 → 374, **RIGHT** |
| seam | 763.1 | **763** (42.8% of rows) | **−0.1 px** | 791 → 763, **LEFT** |

**The two edges moved in opposite directions and converged, both landing within
0.1 px of a prediction committed before the run.** No capture offset, crop or
uniform scale can produce opposite motion, so this is immune to the alignment
problems that dogged takes 195–198.

**The seam is settled as geometry.** It is the widened field plus the rotation,
nothing else:

* `x = guard edge` — `(sx·sinθ − cosθ)/(cosθ + sinθ/sx)`, where the *unrotated*
  field's edge lands after rotating by θ. Beyond it the render has nothing to
  supply, because X4 culled it.
* `x = seam` — `sx·tan(θ/2)`, the bisector.

Both are now verified against **two independent variables**: θ (takes 187–199,
including the θ=0 null) and `sx` via the knob (take 200). Nine hypotheses about
a transform being wrong are dead, and none of them was ever the cause.

### #56: a pair cannot answer it, and I nearly answered it wrongly

The scorer has printed *"needs the SAME line from a run at a different
X4VR_FOV"* on every run since take 148. Comparing take 197 (0.9 m, fov 1.4917)
against take 200 (1.2 m, fov 1.75) reads as **"the span grows with the field"**.

That is wrong. **Take 199 is fov 1.4917 — the same field as 197 — and reports
1.2 m.** The whole difference appears at a *fixed* field.

| take | fov | yaw | csm0 scale set | reported span |
|---|---|---|---|---|
| 197 | 1.4917 | 20 | 0, 1.13137, 1.1359, 1.13591 | 0.9 m |
| 199 | 1.4917 | 0 | 0, **0.8**, 1.1359, 1.13591 | 1.2 m |
| 200 | 1.75 | 20 | 0, **0.8**, **1.2291** | 1.2 m |

199 was run to test the seam at θ=0 and happens to be the fixed-FOV control
#56 needed. Without it, a fifty-take question would have been closed with the
wrong answer off a summary line.

**The prompt is corrected** to demand three runs — two at one field to
establish the run-to-run spread, one at another — and to say compare the
**sets**, not the summary. Which is what this file has said since
[[one-number-several-cameras]] and what the scorer's own `proj` section
already says about `sx`.

**#56 stays open.** There is a hint in the sets — `1.1359x` at fov 1.4917
becomes `1.2291` at fov 1.75, i.e. NDC-per-metre **grew**, so the cascade
covers *fewer* metres at a wider field, which is neither "fitted to frustum"
(shrinks) nor "fixed extent" (unchanged) — but the sets are not stable at fixed
fov either, so that is a lead and not a result.

## Take 201: #56 is unanswerable by runs — the instrument reads a dead member

A pure repeat of take 197. Same knobs, same binary — `git log bcf2fad..HEAD --
layer/ common/` is empty, so the layer was byte-identical. It disagrees with
197.

| take | fov | yaw | csm0 scale set | span |
|---|---|---|---|---|
| 197 | 1.4917 | 20 | 0, **1.13137**, 1.1359, 1.13591 | 0.9 m |
| 199 | 1.4917 | 0 | 0, **0.8**, 1.1359, 1.13591 | 1.2 m |
| 200 | 1.75 | 20 | 0, **0.8**, **1.2291** | 1.2 m |
| 201 | 1.4917 | 20 | 0, **0.8**, 1.1359, 1.13591 | **1.3 m** |

Three things at once, and each kills a different reading:

* **197 vs 201 — identical configuration, different set.** There is run-to-run
  variation at a completely fixed config.
* **199 vs 201 — identical set across different yaw.** So yaw is not the
  driver; 197 is simply the odd one out.
* **199 vs 201 — identical set, different span (1.2 vs 1.3).** The summary is
  not even a function of the numbers printed above it.

### The root cause, base-resolved over all 409 dumps

| member | index | declared by | **sampled by** |
|---|---|---|---|
| `M_shadowCSM0` | 3 | 341 | **189** |
| `M_shadowCSM0Clip` | 9 | 370 | **0** |

**They are in different blocks.** 99 modules declare both and **none** shares a
struct. `ShadowTerms` reads the second one. X4 has no reason to keep a member
nothing samples up to date, so what comes back is whatever the buffer held —
which is exactly the non-reproducibility above.

**The risk was written down and built on anyway.** `ShadowTerms`'s own comment
says *"Members 9 and 10 are read by ZERO of the 409 dumped modules … so X4 may
leave these at whatever the buffer held"*, and adds a `populated` guard. That
guard catches an **all-zero** matrix. It cannot catch **plausible stale data**,
which is what this is — and plausible stale data is the harder failure, because
it reads as a measurement.

**So #56 needs no further takes.** It needs `ShadowTerms` pointed at member 3,
in the block the shaders actually sample. Until then every shadow number in
every log back to take 148 describes an unmaintained buffer.

### The contrast that proves this is specific

The same take's seam measurement reproduces essentially perfectly:

| take | guard | seam |
|---|---|---|
| 197 | x=360 on 58.5% of rows | x=792 on 62.9% |
| 201 | x=359 on 58.5% of rows | x=791 on **63.0%** |

Row fractions agree to 0.1 points; the 1 px offset is capture alignment between
sessions. **The pipeline is reproducible.** It is this one read that is not, and
that is why "the game is nondeterministic" is not an available excuse.

## ShadowTerms: the fix is half a fix, and the other half is plumbing

`read_shadow_terms` now **refuses** rather than reporting the dead member. The
cascade matrices are explicit arguments defaulting to `nullptr`; with no live
source the fields stay zero, `cascade_measured` stays false, and the layer
prints the scalars — which really are camera-block members X4 maintains — plus
why the scales are absent.

**It cannot simply "read member 3", and the reason is the point:**

| | block | set/binding | member | byte | block size | samplers |
|---|---|---|---|---|---|---|
| `M_shadowCSM0` | WORLD/object | **3 / 0** | 3 | 192 | **~576 B** | **189** |
| `M_shadowCSM0Clip` | CAMERA | 1 / 0 | 9 | 576 | 1792 B | **0** |

Different **buffers**, not different offsets. And the layer cannot reach the
object block at any set index today: `g_track` only records UBO slots whose
range is `kViewBlockBytes` (1792), so a ~576-byte block is never a candidate.
**`X4VR_VIEW_SET=3` does not help** — the filter is on size, not on set.

**What `populated` could never do.** It guarded the all-zero case. The failure
that actually happened was *plausible* stale data, which reads as a
measurement — the harder failure, and the one a zero-check cannot see.
`cascade_measured` is that guard.

**The arithmetic was never wrong**, so it moved to `tests/view_math.cpp` where
the matrix is passed directly: `|row0| = 0.002` spans 500 m, 1024 texels
across it, 0.4883 m per texel — the same numbers the layer case asserted, now
over a source that *can* be live. The two harness cases assert the refusal and,
separately, that **no span is printed at all**; the old text is named in the
second so a revert cannot pass silently.

**A green test on a dead member** is what this was for as long as it existed —
worth keeping in mind next time a case passes on a fixture we wrote ourselves.

## The object-block capture — built, GPU-driven, and seen to fail

The other half of #56. The layer now tracks X4's per-object block (set 3)
beside the camera block, and `read_shadow_terms` gets the cascade X4's own
shaders sample.

**The range is recorded, not assumed.** The camera block proves range is not
the struct size — last member at byte 1556, X4 binds 1792 — so nothing in the
SPIR-V predicts the object block's. The slot carries whatever the descriptor
write declared, `obj_slot_host_ptr` bounds its read by *that*, and the observed
`(set, range)` inventory is logged so the next run can assert it. **The harness
binds 576 deliberately**: at 1792 the slot would ride the camera path and the
case would prove nothing.

**Cost is gated.** `vkUpdateDescriptorSets` is rare, so recording slots there
is free. `vkCmdBindDescriptorSets` is **per draw**, so the object walk there is
behind `want_matrices()` — which also replaced two copies of the same getenv
lambda. A run without `X4VR_DUMP_MATRICES` emits no `OBJBLOCK` line and does no
extra per-draw work; asserted, not assumed.

**`obj_spread` guards the assumption this could otherwise smuggle in** — that
one object slot speaks for all of them. The block is per-draw, so it counts
distinct cascade scales across the frame's slots and **warns on >1 rather than
averaging**, because a mean of cascades from different objects is a number with
no referent.

### The fixture is now poisoned, and that is the point

Camera members 9/10 hold **0.009** — a 111 m cascade nothing else here
produces. They used to hold **0.002**, *the same value the object block
carries*. So reading the wrong member gave exactly the right answer and the
case passed for as long as it existed: **a green test on a dead member**, which
is what let take 201's defect survive to take 201.

**Watched to fail.** With the old read restored, five cases go red and one
names it by value: `reported 0.009/111 m — that is camera member 9`.

| | without the object block | with it |
|---|---|---|
| cascade | `csm scales are UNMEASURED` | `csm0=0.002 csm1=0.0005` |
| span | *(none printed)* | `500.0 m across 1024 texels -> 0.4883 m per texel` |
| range | — | `set3/range576`, observed |

**#56 is answerable by measurement now** rather than blocked on the instrument.
It still wants the three runs: two at one field for the spread, one at another.

# Head roll makes the stereo disparity VERTICAL — Patola's eye found it

**Symptom, from closing each eye alternately at about 90 deg of head roll:** the
left and right images sit one ABOVE the other instead of side by side.

**That is unfusable.** Vertical disparity does not produce depth; it produces
strain and double vision. It is not a cosmetic complaint.

## The chain, every step read rather than inferred

1. The disparity is a baked projection shear:
   `make_eye_shear(sx, 0.0f, nz, +-0.5f * ipd)`. **`sy` and `dy` are zero** —
   the offset is purely along image-x, in X4's camera frame.
2. K is built ONCE at shader-variant creation and baked into the SPIR-V. It is
   a compile-time constant, not a per-frame value.
3. X4's camera has no roll, and the declared pose is
   `quat_of_angles(yaw, pitch)` — also no roll.
4. The compositor displays an image declared from D for head pose H by rotating
   it by (D - H), which therefore carries the **full head roll**.
5. The disparity direction rotates with the image. The eyes are separated along
   panel-x always, because panels are head-fixed. So `sin(phi)` of the
   disparity becomes vertical.

## The part that matters more than the 90 degree case

It scales as `sin(phi)`. A **15-20 deg head tilt** — which people hold
unconsciously for long stretches — already turns a third of the disparity
vertical. This is a candidate contributor to ordinary session discomfort, not
an artefact of a deliberate stunt, and nobody would report it as "roll is
broken" because at small angles it just feels tiring.

Patola found it by deliberately rolling to 90 and testing one eye at a time.
No instrument in this project would have.

## The fix, and the machinery is already there

`make_eye_shear` already accepts `sy` and `dy` and writes
`k.m[9] = -sy * dy / near_z`. **The math exists and has never been used.**
Drive the disparity along the rotated axis —
`dx = +-(ipd/2)cos(phi)`, `dy = +-(ipd/2)sin(phi)` — so the compositor's
`-phi` puts it back on panel-x.

It cannot be baked, because phi is per-frame. The vehicle is #59's layer-owned
descriptor set for the head pose, which already delivers per-frame data to the
patched shaders. The eye is square in current runs, so `sx == sy` and a first
version can skip the aspect question.

**Check step 4 before building.** The chain is arithmetic, but "the runtime
applies the roll" is an assumption about a pose we have never given a roll to.
Declare a deliberately rolled pose in one throwaway run and see whether the
image counter-rotates.

## #77 step 1 DONE: the fix's arithmetic is proven offline

`eye_offset_for_roll(ipd, phi, right)` points the eye offset at
`(cos phi, sin phi)` so the compositor's `-phi` lands it back on panel-x.
`tests/view_math.cpp` drives it through the real `make_eye_shear` and the
rotation the compositor applies:

| head roll | shipped: disparity left VERTICAL | with the fix |
|---|---|---|
| 0 deg | 0.0% | 0.0000% |
| 15 deg | **25.9%** | 0.0000% |
| 45 deg | 70.7% | 0.0000% |
| 90 deg | **100.0%** | 0.0000% |

25.9% at 15 degrees is `sin(15)` exactly, which is the whole argument for
priority: that is an unconscious head tilt, not a stunt. The test also asserts
the DEFECT is real at 90 deg and above 20% at 15 deg — if that ever stops
holding, the task was chasing something that does not happen.

The fix loses no magnitude: all of it returns to panel-x, checked at every
angle.

## What remains is the delivery, and it is the bigger half

The arithmetic is not the hard part. Two things stand between it and a run:

1. **The live roll is not published.** `head_angles()` drops roll on purpose,
   and the pose block's roll comes from `X4VR_POSE_ROLL` — a static env knob
   whose own comment calls it "a stand-in for head tracking, so the transform
   can be measured on a flatscreen take". It has to come from
   `views[0].pose.orientation` instead.
2. **The shear is BAKED.** `patch_vertex_clip` takes `const float k[16]` and
   bakes it into the SPIR-V at variant creation, so the disparity direction is
   a compile-time constant. Making it per-frame means the vertex stage reading
   `(cos phi, sin phi)` and scaling the two shear terms.

**(2) is more tractable than it looks**, and this was worth checking before
estimating: the pose uniform's `stageFlags` already include
`VK_SHADER_STAGE_VERTEX_BIT`, and `patch_vertex_pose_clip(pc,
pose_set_index(), 0)` already exists — a vertex patch that reads that very
buffer. The machinery is built; what is missing is a patch that uses it to
scale the shear rather than to rotate the world.

# Take 224 — the instrument could not fire, and #70 FADES rather than slides

Two things, and the second is worth more than the first.

## The instrument was gated on a knob I did not set

`proj FIELD` did not appear. Neither did `proj CAMERA`, `proj VIEW` or `proj
SHADOW` — **the entire camera walk has been dormant in every recent take**,
because it sits behind:

    if (ox == 0.0f && oy == 0.0f && oz == 0.0f && !dump)   // x4vr_layer.cpp
        return;                                            // dump = X4VR_DUMP_MATRICES

I validated the report by confirming its inputs were non-empty in take 203's
log. They were. **I never checked what take 203 set in order to produce them**
— and the answer was `X4VR_DUMP_MATRICES=1`, which take 224 did not carry. So
the validation established that the data exists under some configuration and
said nothing about the configuration I then asked for.

That is "patched is not bound" wearing different clothes: builds clean, scorer
parses, inputs exist — none of which is *executed*. The memory for it exists
and says to drive new layer code on the GPU harness before spending a take. I
did not, and it cost one.

Two changes so the absence can never be silent again:

* the layer names the knob on its way out of that early return, once per run
* `score_run.py` reads that line and says so, instead of listing every cause
  except the real one ("this build predates the instrument, or no camera block
  was credited" — take 224 was neither)

A silent early return in an instrument is a lie of omission, and the fix is one
line per run.

## The defect FADES, it does not slide

Patola, unprompted and correcting the description this task has carried since
it was filed: *"maybe a better description is that they are fading-in/fading-out,
not changing position (this time at least)."*

`#70`'s title says "darken progressively", and every model in this file so far
has been built around the CSM **coordinate** drifting — a shadow that slides.
A shadow that fades in and out is the shadow **factor** changing while its
geometry stays put, and X4's own code has exactly two places that can do that,
both driven by `view_pos.z` rather than by `coords.xy`:

    fade = saturate((view_pos.z - 0.9 * F_shadowmaxdistance) / (0.1 * F_shadowmaxdistance))
    return saturate(fade + GetCSMShadow(ndotl, coords))

and the cascade **selection/blend** inside `GetCSMShadow`, chosen against the
thresholds `[7.2 33.3 269.9 1856.4]`.

This does not contradict the field-mismatch diagnosis — it sharpens it.
`M_invprojection` maps clip back to view, so a projection mismatch corrupts the
recovered **z**, not merely the lateral coordinate. And the corruption varies
across the screen, so a cockpit surface's recovered z changes as the camera
turns even though its true z never does. That is a shadow strength that fades
with where you look, which is what he sees.

**What it changes:** a fix validated only on "does the shadow sit still" could
pass while leaving the z path wrong. The measurement has to cover the
strength, not just the position — and `csm0-scale`, being `|row0|`, sees
neither.

# Take 223 — #70 IS the field mismatch, and `<fov>`=1.0 is not a fix we can ship

Patola: *"this fixed the shadows issue... The shadows on the rails, the right
arm of the ship and other parts are consistently still now."* Not just the
cockpit — the whole ship. `#70` is the mismatch between the field X4 renders
and the field its shadow cascades are fitted to.

The injector rewrites X4's `<fov>` in an in-memory config, so this is a real
change to X4's own projection and not a layer-side reinterpretation:

    take 222   config: effective fov=1.4917 res=1408x1408 (fov from X4VR_FOV)
    take 223   config: effective fov=1.0    res=1408x1408 (fov from X4VR_FOV)

## MY PREDICTION WAS WRONG, in the way this project already documented

I committed to `sx=1.3333` appearing in take 223's `stereo:` line. It reports
**`sx=0.8890` — byte-identical to take 222's**, across a 33% change in the
knob that is supposed to set it.

`X4VR_PROJ_LIVE=1` makes that line report the *live* sx from whichever camera
block the logging draw happened to use, and take 202 already established that
**X4 runs several cameras at once and some slots ignore the `<fov>` setting**.
The existing memory says it in one line: *compare value SETS across a knob
change, not summaries.* I quoted a summary. Agreement where there should have
been a difference is an alarm, and it was sitting in the output I asked for.

The mechanism survived only because `config: effective fov` is a different
observable that did move. **Predicting the wrong observable is not the same as
predicting the wrong mechanism, and the run cannot tell them apart** — a
correct mechanism validated by a wrong prediction is one coincidence away from
a wrong mechanism validated the same way.

## Why `<fov>`=1.0 cannot be the answer, independent of shadows

    take 223   pose residual: X4VR_FOV=1.0000 renders 73.7 deg,
               runtime wants 94.0 x 99.0 deg per eye

**X4 draws 20.3 degrees per eye less than the headset displays.** The edges are
fed by geometry X4 never rendered. That is the exact condition `offaxis:
WARNING` exists to catch, and it did not fire here only because this run did
not enable the off-axis path — so the guard was absent, not satisfied.

So the floor on the field is **`X4VR_FOV` ≈ 94.0 / 73.7399 = 1.275** just to
fill the panels, before any margin for head rotation. `#70` is present there
too. **There is no value of this knob that both fills the headset and fixes the
shadows**, which makes it a knob setting we cannot ship rather than a fix.

## And the trade is NOT performance

    take 222   GAMEPLAY 6.98 ms (143 fps)   fov 1.4917, 110.0 deg
    take 223   GAMEPLAY 6.92 ms (145 fps)   fov 1.0000,  73.7 deg

Narrowing the field by a third bought **0.06 ms**. At 1408x1408 per eye this
render is not fill-bound, so the wide field costs essentially nothing in frame
time. The trade is field-of-view against shadow correctness and nothing else,
which is worth knowing before anyone proposes buying shadows with resolution.

## What has to happen next

The shadow-configured slots sit at `sx=1.33333` — X4's default field — and
appear **pinned** there while `<fov>` moves the rendering camera. At fov 1.0
the rendering camera lands on 1.33333 too, they agree, and the defect vanishes.
So:

* **(a) make the shadow side follow the wide field** — needs to know what pins
  it. NOT a config value: `config.xml` carries only `<shadows>2</shadows>` and
  `<softshadows>true</softshadows>`, which are quality, not field.
* **(b) correct the shadow term in the shader** — we already patch fragment
  modules for `M_invprojection`; this composes a known field correction before
  the CSM lookup. Same class as #22/#39, which work.
* **(c) narrow the field** — refuted above, and #24 exists to prevent it.

Before any of that, the mismatch needs to be **objectively measurable**: report
the sx *set* per run, shadow-configured slots against the rendering slot. That
is what take 223 should have been scored on, and it is the instrument that lets
a fix be validated without spending Patola's eyes on every candidate.

# Take 222 — the eye offset is NOT it, and the two camera populations get names

`X4VR_IPD=0.0`, confirmed in the log as `stereo: ipd=0.0000 sx=0.8890 near=0.100
-> shear m8 L=0.00000 R=-0.00000`. Patola: *"the shifting shadows appear even
with IPD 0."* The per-eye reconstruction hypothesis is **refuted**.

In hindsight the classification already implied it: with the deferred lighting
pass `unsheared`, the only per-eye influence on the shadow term was through
`M_invprojection`, and at ipd 0 that correction composes with an identity.

## What the yaw-dependence actually requires

Cockpit shadows are static in vanilla for a reason worth stating, because it is
the whole diagnosis. The cockpit and the sun do not move relative to each other
when you look around, so the shadow on a cockpit surface must not change. The
shader computes `mul(vec4(view_pos,1), M_shadowCSM0)`, and BOTH factors change
as the camera rotates: `view_pos` rotates one way, `M_shadowCSM0` — which X4
rebuilds each frame as view->shadow — rotates the other. **The product is
invariant, and vanilla's static shadows are that cancellation working.**

So a shadow that moves with yaw means the two factors were built from
**different camera orientations or different fields**, and the residual grows
with the angle. That is the shape of the defect, independent of which of our
changes causes it.

## The two camera populations are X4's field and ours

Take 202 recorded, as an unexplained fact, that camera slots fall into two
disjoint groups — 85 slots only ever showing `sx=1.33333`, 38 only ever showing
`sx=0.70024` — and that **all 58 shadow-configured slots are in the 1.33333
group**. The layer's own law names them:

    full_deg = X4VR_FOV * 73.7399        (x4vr_layer.cpp:10148)
    sx       = 1 / tan(full_deg / 2)

    X4VR_FOV = 1.0     ->  73.74 deg  ->  sx = 1.33333   <- all 58 shadow slots
    X4VR_FOV = 1.4917  -> 110.00 deg  ->  sx = 0.70024   <- what we render into

**X4 attaches the cascade configuration to a camera at its native field, and we
render the scene at 110 degrees.** `view_pos` is reconstructed from a depth
buffer written by the wide camera; `M_shadowCSM0` was built for the narrow one.
The cancellation above is between exactly those two, and `X4VR_IPD` does not
appear anywhere in it — which is why take 222 changed nothing.

## `csm0-scale` could never have seen this

`proj SHADOW` reports `csm0-scale = |row0|` — a **magnitude**. A rotation or
field mismatch leaves the magnitude untouched, so the instrument built to watch
the cascades is structurally incapable of reporting this defect. That is the
same blind spot as `score_run.py` taking `abs()` of the roll samples, found two
days earlier in this same session, and the same root cause: both were built to
answer "how big", and the question is "which way".

## The discriminator: X4VR_FOV=1.0

One knob, and it makes the rendered camera and the shadow-configured camera the
same field. Predicted log line: `sx=1.3333`, matching the 58 slots exactly.

- shadows stop tracking yaw -> #70 is the field mismatch, and the fix is to
  make the cascade configuration follow the field we actually render, or to
  reconstruct the shadow term in the frame the cascades were built for
- shadows still track yaw -> the field is not it either, and what remains is
  the doubled-pass machinery itself: multiview, the bindless mirror's per-view
  descriptor offset, and the 1:1 render aspect

# #70 — the vanilla control says the shadows are OURS

Patola, on stock X4, in **the same scene** where he sees the defect: *"Vanilla
X4 does not shift the shadow the way our headtracking does... The shadows
inside the cockpit stay put."*

He also reported, unprompted, that glass reflections/refractions **do** shift
subtly under vanilla mouselook, and that this is not what he is seeing with the
shadows. That is worth recording as a property of the observer, not just of the
game: he is discriminating between two view-dependent effects rather than
reporting motion in general, which is what makes the negative meaningful.

## What it is NOT

Three candidates, all eliminated from evidence already on disk:

1. **#33's world conjugation.** `X4VR_POSE_ROTATE` is absent from take 221's
   command line and every recent one, and no rotate line appears in any log.
   The rotation never ran.
2. **The stereo shear, including #77's roll.** `classify_unsheared()` marks
   depth-only passes (the cascade render) AND no-depth passes (deferred
   lighting, a fullscreen triangle) as unsheared, and the layer logs
   `binding the no-shear twin (shadow + UI exclusion active) — vertex edit
   dropped`. The vertex patch that carries the eye offset and the roll does not
   reach either pass.
3. **`M_invprojection_uj` never being corrected.** This is the member that once
   cost 13 takes of nulls, so it was the first suspect. Take 98's teardown
   report settles it: **2 modules corrected of 2 eligible**, both shadow
   cascades. The first-present count of 0 in recent logs is meaningless — the
   code says so itself: at first present X4 has typically created no deferred
   module yet.

## The mechanism that IS there, from X4's own GLSL

    RetrieveZBufferViewPos:  view_pos = (M_invprojection * vec4(uv2clip(uv), depth, 1)).xyz / w
    GetShadow(ndotl, view_pos):
        fade      = f(view_pos.z, F_shadowmaxdistance)
        coords[i] = mul(vec4(view_pos, 1), M_shadowCSM_i)
        return saturate(fade + GetCSMShadow(ndotl, coords))

`view_pos` is reconstructed by **`M_invprojection`**, the member we rewrite per
eye, and it reaches the shadow term by **two** paths: the CSM coordinate lookup,
and a distance fade that scales brightness directly. The `M_shadowCSM*`
matrices live in the per-**object** block and never carry our correction.

This would be yaw-dependent even though our error is not: X4 rebuilds
`M_shadowCSM` each frame as view->light, so a fixed view-space error is rotated
by the camera's own orientation before it reaches the atlas.

**The mechanism is proven to exist; the error is not.** If #39's correction is
exact, `view_pos` lands back in X4's frame and every line above is benign. That
is the open question, and it is not answerable by reading more source.

## The discriminator: X4VR_IPD=0

`configured_ipd()` is read by `strtof`, so `X4VR_IPD=0` is honoured as zero
rather than falling back to the default, and **no downstream guard skips
patching when it is zero**: `dl`/`dr` become 0, the shear matrices become
identity, the invproj correction composes with an identity. Every module is
still patched, every pass still doubled, `X4VR_FOV` still widened. Only the eye
offset goes away.

So it varies exactly one thing, which is what take 213's control failed to do
and what Patola's 2x2 got right where my proposal did not.

- shadows stop tracking yaw -> the per-eye reconstruction is the cause, and the
  fix has to make the shadow term read a frame the CSM matrices agree with
- shadows still track yaw -> the eye offset is NOT the cause, and the next
  suspect is #56's measured mismatch: all 58 shadow-configured camera slots sit
  in the `sx=1.33333` group and none in the widened one, so the cascade is
  configured on a camera whose field we never touched

# Take 221 — #77 CLOSED. Two independent channels, agreeing for the first time.

    roll  sign: 17 sample(s) match -head, 0 match +head (of 17 tilted past 5 deg)
    PASS  this run is a candidate for docs/known-good-runs.md

Patola: *"I was able to clearly perceive between the two states — one of them
consistently comfortable the whole time, when my head was level and when it
was rolled to either side, and the other only comfortable when it was level."*

That is the predicted signal exactly, and it was **pre-registered**: control arm
tilt-dependent, compensated arm tilt-independent, the switch to detect being
"tilt matters / tilt doesn't matter" rather than comfortable/uncomfortable.
Both directions, which kills the last residual worry that the fix was an
accident of one sign.

**What makes this different from every earlier #77 take** is that the objective
channel did not depend on the subjective one. The scorer read the sign off the
log — 17 of 17 — and would have said the same thing with the headset in a box.
Takes 215 through 220 had no such channel; the eye was the only instrument, and
it was asked a question (an absolute judgement of a steady state) that it is
known to be bad at.

## The FAIL on this run was MINE, and it was the same mistake a fourth time

The run scored FAIL on `pose fell back to identity 1 time(s) AFTER it had been
driven`. It had not. The single sample sits inside a window head-look had
announced 1.5 s earlier:

    [197886.806] headlook: view is now external/floating — idle
    [197888.290] vr pose: ... cam_valid=0 ...        <- "the gate dropped out"
    [197888.995] headlook: view is now the cockpit — steering

Head-look was idle **by design**; identity is the designed consequence. Take
220 carries the identical false FAIL at the identical offset inside its own
external-view window. The verdict even ended "check `steering`" — pointing at
the one component that had behaved correctly.

The comment directly above that check, written after take 150, says it was
already *"the third check in this file to grade by a condition it did not
actually test."* This was the fourth, inside the very check that documents the
pattern. **Writing the lesson next to the code does not prevent the lesson.**
What prevents it is the check consulting the system's own declaration, which
in every one of these cases was in the same log a second or two earlier.

Now gated on the gate's announcement — on head-look having SAID it was idle,
never on the pose having come out identity. Take 221 PASSES, take 219 is
unchanged, and take 220 still FAILS on the real defect: the fix made the check
specific, not permissive.

# Take 220 — the live path had a SIGN ERROR. Found in the stored logs, not the headset.

Patola, on the live-roll A/B: *"I could perceive the two phases changing,
[but] neither was more comfortable than the other. Both were comfortable with
the head level and uncomfortable when rolled left or right, but in different
ways."*

That is not a null. It is a **description of a specific defect**, and the log
had already recorded it three takes running.

## The calibration was in the logs the whole time

`X4VR_ROLL_FORCE` publishes a shear angle directly. Patola located the comfort
null twice, at two different angles, and the layer logged the head angle at the
same moment:

| take | shear published | Patola's null | `head_angles()` read |
|---|---|---|---|
| 216 | **+40.00** | 40 deg **left** | +0.1 .. +2.7 (head level; he tilted to find it) |
| 218 | **+40.00** | 40 deg **left** | **-52.63** during the held-left phase |
| 219 | **+65.00** | 65 deg **left** | **-76.14 / -84.58 / -82.91** |

**A left roll reads NEGATIVE, and the shear that cancels it is POSITIVE.** Two
points, opposite signs, so the map is a negation. The live path published
`head_roll` unchanged:

    roll: head -61.51 deg, weight 1.000 -> published -61.51 deg   [A/B: COMPENSATED]
    roll: head -74.77 deg, weight 1.000 -> published -74.77 deg   [A/B: COMPENSATED]

## Why BOTH arms were wrong, in DIFFERENT ways

Writing D for the disparity angle on the panel, H for head roll and V for the
published shear, the calibration gives `D = V + H`:

| arm | V | D at H = -65 | vertical | horizontal |
|---|---|---|---|---|
| control | 0 | -65 | 91% | +0.42 — correct polarity |
| compensated, **as shipped in 220** | H | **-130** | 77% | **-0.64 — eyes SWAPPED** |
| compensated, fixed | -H | **0** | 0% | full, correct |

So the control arm gave vertical disparity with the eyes the right way round,
and the compensated arm gave *less* vertical disparity with the eyes **inverted**
— pseudoscopic. Two distinct wrongnesses, no null anywhere, both vanishing at
H = 0. That is Patola's report, line for line, including "in different ways"
and "both were comfortable with the head level".

**Take 217 is the same defect** and its report — *"no situation in which the
eyes looked right, neither before nor after the timed changes"* — was accurate.
He had rolled **right** that run (`head +51.95 / +50.90 / +47.57`), which the
model says is equally broken. See the refutation note under take 219's method
section: I attributed a correct observation to the observer.

## The fix

`roll_compensation_deg()` in `common/x4vr_view.hpp`, a negation with the
calibration written next to it. `X4VR_ROLL_FORCE` deliberately stays RAW —
it means "the value that lands in the buffer", which is how 216/218/219 were
calibrated, and negating it would silently move the null those runs located.

## Both instruments were blind in the SAME dimension

This is the part worth carrying forward.

1. `tests/view_math.cpp` took the **shear** angle and used it for the
   compositor rotation as well. Both sides moved together, so any head-to-shear
   sign error cancelled itself inside the model. It passed through takes 215,
   217 and 220. Now parameterised on the head angle, with the two roles
   separated, and it fails on take 220's code (7 cases).
2. `tools/score_run.py` took `abs()` of every head and published sample before
   comparing them. It checked the magnitude — which was always right — and
   discarded the sign, which was the defect. Now checks the sign explicitly;
   re-scoring takes 217 and 220 gives **13 of 13 tilted samples matching
   `+head`** and a FAIL on both.

A model and a scorer, built at different times for different purposes, both
dropped the same dimension. The reason is the same in each: I built them to
answer "is the disparity being rotated at all", which was the open question at
the time, and neither was rebuilt when the question became "rotated which
way". **An instrument inherits the question it was built for.** When the
question changes, re-read the instrument before trusting its verdict.

There is also a plain rule underneath: **an instrument that takes `abs()` of a
quantity can never report a sign error in it**, and half of this project's
defects have been signs.

# Take 219 — confirmed at 65 deg. The mechanism is proven; only the LIVE path is untested.

Null exactly at the forced 65 deg, with the same clean alternation: level
comfortable / 65 uncomfortable, switch, 65 comfortable / level uncomfortable.

Three independent confirmations now, and they close the last mechanical
hypothesis:

| take | forced roll | result |
|---|---|---|
| 216 | 40 deg, constant | null at 40 deg left |
| 218 | 40 deg, alternating with 0 | null tracks the switch, both held and moving |
| 219 | 65 deg, alternating with 0 | null at 65 deg left |

**There is no angle-dependent defect between 40 and 65**, which is the range
take 217 was in. So the codegen, the buffer, the binding, the sign, the
per-frame delivery of a CHANGING value and the behaviour at large angles are
all established. What has never been confirmed is the one path that reads the
head instead of a constant.

## Method: a subjective VR test needs a SWITCH, not a verdict

Patola's own observation, and it is the most transferable thing in this
stretch: *"sense of depth is complex and subject to lots of noise... my brain
tries to make sense of what I'm seeing by constantly adjusting its
parameters."*

That is why takes 216, 218 and 219 produced clean answers and 217 did not. All
three working takes gave him a **transition** to detect. Take 217 asked him to
judge a steady state — and a steady wrongness is exactly what the visual system
adapts to and stops reporting.

**So: never ask for an absolute judgement of a rendered frame. Alternate, and
ask when it changes.** The same reasoning that put the lead's A/B inside one
run applies to the eye as an instrument, and for a stronger reason: the
instrument itself re-calibrates while you use it.

It also means an observer who has seen the transition is a BETTER instrument
than one who has not. Take 217 was run by an uncalibrated observer, and rerunning
it now is not repetition -- the instrument has changed.

> **REFUTED by take 220 — see "Take 220" above.** The paragraph above is kept
> because it was the published explanation and the method finding around it
> still holds, but its verdict on take 217 is WRONG. Take 217 failed because
> the live path had a **sign error**, which the log proves and which no
> property of the observer could have caused. Patola reported "no situation in
> which the eyes looked right" and that was an accurate reading of a genuinely
> two-sided defect. I doubted a correct report because I had already decided
> the mechanism was proven.

# Take 218 — the channel DOES deliver a changing value. #59 is cleared.

Patola, holding still and then tracking the null: comfortable at level, switch,
uncomfortable; roll left to ~40 and comfortable again; switch, and level is
comfortable once more. Several cycles, and separately several cycles held at
40 deg left where it alternated comfortable/uncomfortable exactly on the
boundaries.

**So a per-frame CHANGING value crosses the pose channel correctly.** That was
the leading hypothesis for take 217 and it is dead. It also clears something
larger by accident: `budget 0.00` on every run meant `r` had always been
identity, so nothing had ever shown that #59's per-frame delivery works at all.
Now something has. **#33's conjugation is not silently dead.**

## What is left, and it is a narrow window

Take 217's compensated arm is flawless in the data — published == head to the
digit across all 21 samples, weight 1.000, 12 of them past 25 deg:

    head  48.80 -> published  48.80      head -68.33 -> published -68.33
    head  63.34 -> published  63.34      head -66.18 -> published -66.18

Those are the same buffer contents a forced run at that angle would produce,
and forced runs are comfortable. So either take 217's verdict was reached
before Patola had a calibrated sense of what "right" feels like — 216 and 218
gave him a switching reference, 217 offered none if it was working — or there
is a defect that appears past ~50 deg and not at 40.

**Both takes that WORK tested 40 deg. The take that failed was at 48-68.** That
is the only untested variable left, and forcing 65 deg tests it directly with
the protocol Patola is now practised at.

# Take 217 — the live roll fails where the FORCED roll worked. Unexplained.

Patola, holding a tilt both ways across 12 phases: never right, in either arm.

**And the run is clean**, which is what makes this hard. The value chain is
exact: head 48.80 deg -> published 48.80 -> `(cos,sin)=(0.6587,0.7524)`, which
is cos and sin of 48.8 to four places. The control arm publishes exactly 0. The
sampling is adequate — 12 compensated samples past 25 deg of tilt, median 49.9.

So take 216 (a CONSTANT forced 40 deg, comfort null at 40 deg of left tilt,
asymmetric either side) and take 217 (the live head roll, never right)
contradict each other, and every link I can measure is correct in both.

**What is NOT ruled out**, in the order I would attack them:

1. **The channel has never delivered a CHANGING value.** `budget 0.00` on every
   run means `r` has always been identity, so a stale region and a fresh one
   were indistinguishable for the whole life of #59. Take 217 is the first time
   a per-frame-varying value crossed it. `pose_publish` writes at submit into
   the region assigned at record, which looks right — but "looks right" is what
   this section is about.
2. **Render-time roll against display-time roll.** The shader uses the value as
   of submit; the compositor reprojects to a newer pose. Constant forcing is
   immune to that and a live value is not. Patola held still, which should have
   made it small — should.
3. **The model fits 216 too well to be wrong and 217 too badly to be right**,
   which usually means the two runs differ in something not yet named.

## The discriminator needs NO new code

`X4VR_ROLL_FORCE=40` with `X4VR_ROLL_AB=25` already alternates **40 against 0**,
because the A/B's control arm publishes zero whatever the source. Hold a 40 deg
LEFT tilt and stay still:

* **the comfort alternates** -> a changing value IS delivered per frame, and
  the fault is in the live path's value rather than the channel;
* **it never alternates** -> the channel does not deliver change, which also
  means #33's conjugation has never worked and nobody could have known.

One knob difference from take 216, one subjective judgement of the kind Patola
trusts, and it splits the two families of cause.

## And a check of mine that had never once passed

`pose final:` prints at TEARDOWN and takes 214, 216 and 217 all fail to reach
it. The scorer failed on its absence — so it failed **every** pose run this
project has done, including take 216, which produced a clean positive result
while carrying that FAIL.

A check that has never passed is not a check; it is a false alarm on every run,
and a false alarm is how a real one comes to be ignored. It now judges on
`pose: channel up`, which is present, and reports the missing teardown line
without failing.

# Take 215 — the roll fix did NOTHING, and the run could not say why

Patola: still one image above the other at 90 deg. And the run is unreadable,
which is my error rather than the fix's.

**Everything upstream worked.** The log proves each link:

| link | evidence |
|---|---|
| pose channel up | `channel up — set 31 binding 0, 4 region(s) of 208 B` (208 = the extended block) |
| modules patched | `live-sx=300 mvp-sx=12 baked-sx=0` |
| modules read it | `pose: 300 module(s) read the pose block (set 31)` |
| the value written | `0/17513 clamped` — pose_drive_residual ran 17513 times |
| roll requested | `roll stereo: ON` |

Take 214 for contrast reads `0/0`: without `X4VR_POSE` the residual never ran
at all. So 215 is not the null 214 would have been.

## The error: no instrument on the load-bearing value

Nothing logged the roll. `pose_set_roll` appeared exactly ONCE in the layer —
the call — and the periodic block reported `applied_yaw`/`applied_pitch` and
not the roll beside them.

So **"the roll arrived as zero" and "the arithmetic is wrong" produce the same
null**, and take 215 cannot separate them. That is the single most repeated
mistake in this project's history and I made it again, in the same session I
wrote "half-finished SPIR-V codegen does not fail loudly".

Worse, I had flagged the check and then dropped it. Asked whether to verify
that the compositor applies the roll before building, I accepted "the fix would
already include a similar check" — and it does not, because **a fix that
produces no change tests nothing about why.**

## What take 216 separates

`X4VR_ROLL_FORCE=<deg>` drives the shear with a CONSTANT roll, head held level:

* the disparity visibly tilts -> the shader path is alive and the fault is the
  VALUE (extraction, publication, or the weight);
* nothing moves -> the path is dead however many modules were patched, and the
  next question is binding, not arithmetic.

`roll:` now logs head, weight, published degrees and the published `(cos,sin)`
every 600 frames, and `score_run.py` FAILS a run whose head never rolled past
5 deg (it did not exercise #77 at all) or whose published value is under half
the head's (the value is being lost). Both watched to fire.

## #77 step 3 DONE: the codegen, and two defects spirv-val caught

`patch_vertex_eye_offset` gained `roll_set`/`roll_binding`. Absent, it emits
the old words; present, it turns the disparity by the head's roll:

    x -= sx * d * cos(phi)
    y -= sy * d * sin(phi)      <- the Y half has never existed before

`(cos phi, sin phi)` arrive as DATA in a new `PoseBlock` vec4 at offset 192,
because computing them in-shader needs GLSL.std.450 and not every module
imports it — this patch must not start refusing modules it used to accept.

The block is declared as a **one-member view at offset 192**, no mat4 type, no
ColMajor, no MatrixStride. That is the same trick the two-mat4 and three-mat4
views already rely on, carried one step further.

### The disassembly, which is the real proof

    %71 = extract .x            cos
    %72 = extract .y            sin
    %73 = sx * d
    %74 = %73 * cos             delta x
    %78 = x - %74
    %79 = AccessChain cam, member 1, col 1, row 1     sy = P[1][1]
    %82 = (sy * d) * sin        delta y
    %84 = y - %82
    %85 = insert x into loaded
    %86 = insert y into **%85**  <- chained, not both into `loaded`

### Two defects, and neither was visible in C++

1. **Four decorations pushed into `annos`, which this function never flushes.**
   It writes its one existing decoration inline at `anno_at`. The result would
   have been an undecorated uniform block: a module that still assembles and
   binds to nothing.
2. **`c_one` is created only when the off-axis affine is on**, and roll is
   mutually exclusive with it — so `AccessChain(..., c_one, c_one)` carried
   **id 0**. spirv-val: `error: line 139: Id is 0`. The source read correctly;
   only the assembler could see it.

A third was avoided rather than caught: `OpVariable` takes
`{pointer type, result id, storage class}` and the first draft reversed the
first two.

### What is locked in

`tests/run-multiview-render.sh` now asserts, on a real GPU under validation:

* roll absent -> **byte-identical** to the pre-#77 words (`cmp` against the
  same module patched by the previous build);
* roll present -> exactly **2** OpCompositeInsert, one `DescriptorSet 7`, one
  `0 Offset 192`, and spirv-val OK;
* roll + off-axis -> **refused**. The affine rewrites x and y from the sheared
  x and was derived assuming the offset is horizontal. The combination may be
  fine; nothing has measured it, so it is refused rather than emitted.

**Still not wired end to end.** The layer does not yet call this with a
`roll_set`, and `pose_set_roll` is not yet driven from the live head. That is
the remaining step and it is plumbing, not codegen.

## #77 step 2 DONE: roll comes out of the quaternion

`head_angles()` now returns `roll_deg`, measured about the FORWARD axis against
the roll-free frame — exactly the angle the eye offset has to be turned by:

    u    = the head's own up
    r0   = normalise(f x worldUp)     the roll-free right
    u0   = r0 x f                     the roll-free up
    roll = atan2(u . r0, u . u0)

Round-tripped in `tests/headlook.cpp` against quaternions built the other way,
at **36 combinations** of yaw/pitch/roll rather than one convenient angle — a
sign error or an axis swap survives a single case easily, and this has to be
exact because the disparity is turned by whatever comes out. A level head
reports exactly zero, which is the case every existing run is in.

Degenerate exactly where yaw is, and for the same reason: at the ceiling, yaw
and roll ARE the same rotation. `yaw_weight` is reused as the guard rather than
given a second spelling.

## #77 step 3: the vehicle already exists, and it is NOT patch_vertex_clip

The obvious target was `patch_vertex_clip`, which bakes `const float k[16]`.
Patching that to read a per-frame roll would have been the wrong work.

**`patch_vertex_eye_offset` is the vehicle.** It already replaced the baked
matrix for the same class of reason (`sx` ranges 1.15 to 37.75 across a session
as the player zooms, so no constant can be baked), and it already:

* reads a uniform block member at a (set, binding, member) at runtime;
* selects per-view arithmetically off `gl_ViewIndex`;
* reads `sy` from `M_projection[1][1]` as well as `sx`, when #35's off-axis
  affine is on — so the Y half of the shear is already reachable.

Its form collapses to one multiply because X4's `m[10] = 0` makes clip z the
constant near plane:

    x_c' = x_c - sx * d        (near cancels)

**`d` is the only baked part**, and its doc says why: "the eye offset in metres,
our own choice, so it stays a constant". Under roll it stops being a constant.
What #77 needs is:

    x_c' = x_c - sx * d * cos(phi)
    y_c' = y_c - sy * d * sin(phi)

with `(cos phi, sin phi)` read per frame from OUR pose block — the camera block
belongs to X4 and cannot carry it. That means extending `PoseBlock` (currently
`static_assert(sizeof == 192)`, three mat4 at 0/64/128) with a vec4 at 192, and
adding the two multiplies to a 476-line codegen function.

**Not started, deliberately.** Half-finished SPIR-V codegen does not fail
loudly — it produces subtly wrong geometry in a shader that still compiles and
still draws, which is the most expensive failure mode this project has. It is
scoped here so the next session starts from the vehicle rather than
rediscovering that `patch_vertex_clip` was the wrong door.

# What is inside the reprojected eye image — ONE layer, everything

Read, not inferred, at the submission site: `const XrCompositionLayerBaseHeader
*layers[1]`, `layer_count = 1`. A single `XrCompositionLayerProjection` with two
views, both pointing at the one swapchain X4's frame was copied into. **The
scene, the cockpit, the HUD and the cursor are all pixels in that one image.**

## So #76's HUD counter-rotation is ARITHMETIC, not a defect in the HUD path

The compositor's job is to take an image declared as rendered from pose D and
display it correctly for head pose H, which it does by rotating the whole image
by (D - H). Every pixel in it moves together. Screen-locked content — which
should stay fixed relative to the head — therefore counter-rotates by exactly
(H - D), and D is X4's camera, so:

**the HUD moves opposite the head by exactly the head-look tracking error.**

Measured from take 214's own samples, inside the clamp:

| | median | p90 | p99 | max |
|---|---|---|---|---|
| IDLE | 0.46 | 2.40 | 5.40 | 8.57 deg |
| ARMED (lead) | 0.30 | 1.79 | 4.08 | 11.82 deg |

## The canvas cannot fix this, and was never on

`canvas_ndc_of()` extracts `k[0]`, `k[12]`, `k[5]`, `k[13]` — a per-axis scale
and offset in NDC. **There is no rotation term.** #30's canvas moves the UI's
stereo disparity; it cannot head-lock anything, however complete it is.

And it never ran: `canvas_wanted()` needs `X4VR_CANVAS_M` or the off-axis path,
and takes 212, 213 and 214 contain **zero** `canvas` lines between them. The
`residual 0.0000` in every `vr pose` line says the same thing from the other
side.

**The only fix is structural**: screen-locked content needs its own composition
layer, declared with the HEAD pose rather than X4's camera, so the compositor
leaves it alone. That is #57's "separate screen-locked from world-at-infinity",
and it is real work rather than a sign flip.

## #77 (roll): the compositor is ALREADY correcting it, and that is right

The declared quaternion is built by `quat_of_angles(yaw, pitch)` — no roll term
— so when the head rolls by phi the compositor rotates the image by -phi. For
the world and for the cockpit that is CORRECT: roll your head in a real cockpit
and the cockpit counter-rolls in your view.

So roll is not simply "not working". It is correct for everything that is
world- or ship-fixed and wrong for screen-locked UI, which is the same
single-layer problem as #76. **What Patola sees needs separating before any
work**: is the horizon/cockpit rolling wrongly, or is it the HUD and reticle?
Opposite causes.

## And a correction to take 214's headline

`lagfit`'s tau improved **3.3x** (35.6 -> 10.7 ms). The raw error a player
actually lives with improved only **1.3-1.5x** (table above). Tau is the SLOPE
of error against head rate; the lived distribution mixes all rates and also
carries the lead's noise, which take 214 measured at 0.00 -> 3.97 deg/s. Both
numbers are true and they answer different questions. Quote the distribution
when the subject is comfort.

# Take 214 — the lead WORKS. 4T -> 1T, measured on hardware

| | IDLE (control) | ARMED (lead) | |
|---|---|---|---|
| tracking tau | 35.6 ms | **10.7 ms** | **0.30x** |
| pose rate | 88.6 Hz | 87.2 Hz | matched |

**1 T at 88.6 Hz is 11.3 ms and the lead measured 10.7.** The arithmetic
predicted the pre-step error goes from `rate*T/kp` = 4T to exactly `rate*T` =
1T, and that is what came back. #73's core claim is settled on hardware.

Replaying take 214's own head trajectory through the real servo and take 213's
identified plant reproduces it: at delay 2, IDLE 36.1 ms, ARMED 10.2 ms, ratio
**0.28x** against the measured 0.30x. The model is right at this rate.

## So what was different about take 212?

The pose rate, and nothing else that has survived scrutiny.

| take | pose rate | measured | model (delay 2) |
|---|---|---|---|
| 214 | 88.6 Hz | 0.30x | 0.28x — **matches** |
| 212 | 23.6 Hz | 0.94x | 0.63x — does not |

The lead extrapolates `(1/kp - 1)` times the head's motion since the last pose,
so its prediction horizon is 3 T. At 88.6 Hz that is 34 ms of extrapolation; at
23.6 Hz it is 127 ms, and a head does not travel in a straight line for an
eighth of a second. The velocity feed-forward's own assumption fails as the
pose rate drops.

**That condition is one we have already ruled out**: 4224 squared runs at 29 fps
with no instrumentation at all (take 168b). The lead works at every rate the mod
is playable at. The 23.6 Hz residual is recorded as unexplained rather than
chased — the model under-predicts it too, so something beyond the horizon
argument is also present there.

## #75 ANSWERED, and the mechanism is exactly as predicted

`poselag`, at full rate with a fresh read-back (median age 7.4 ms against an
11.3 ms tick):

| | median | p90 | max |
|---|---|---|---|
| IDLE | 0.115 | 0.575 | 2.185 deg |
| ARMED | 0.230 | **1.495** | **9.315 deg** |

**The declaration error IS the pending command, proven by a coincidence that
cannot be one:** the largest `|declared - X4|` in the run is 9.315 deg, and the
largest single-frame command step the `fade` tracker recorded is 9.315 deg. The
same number. The declared pose runs ahead of the pixels by exactly the command
X4 has not yet received, which is what the share's new field was added to test.

So the lead buys tracking with reprojection error, 2.60x at p90.

## Whether that trade is BAD is NOT established

Do not assume it. The declared pose leads in the direction of motion, so the
compositor displaces the frame FORWARD — and the content inside that frame was
rendered from a camera that LAGS. The displacement may partially cancel the
render lag for distant geometry while causing parallax error on the near
cockpit. Those are opposite conclusions and nothing here separates them.

It needs Patola's eye on an A/B, or a targeted test. Recorded as a measured
trade-off with an unknown sign, not as a defect.

# #75 — the layer declares the SERVO'S COMMAND, not the head pose. Read, not inferred

Patola's "two systems fighting" is structurally right, and the code says so in
its own words at the submission site (x4vr_layer.cpp, the projection view
construction):

> *"Deliberately the injector's **command**, not the live head pose."*

`pv[e].pose.orientation` is built from `sh->cam_yaw_deg` / `cam_pitch_deg` --
the injector's belief about where X4's camera ended up -- plus the shader
residual, and falls back to identity when `cam_valid` is 0. So the compositor
DOES reproject the eye image, and it reprojects using the servo's belief.

**What follows, if the declaration is accurate:** the servo's lag does not make
the image trail the head. The compositor corrects the image's position. What
lags instead is the visible FIELD -- the rendered content's edge follows X4's
camera, so turning fast swings a black boundary in. That is exactly what Patola
saw once take 213's sequence completed and the camera froze: *"I could still
actually move my head but I would see the black space around."*

## The specific way it can be wrong, and how big that is

`sh->cam_yaw_deg` is stored AFTER `head_look_step` (x4vr_inject.cpp:2457), so
the declared pose includes a command X4 will not receive for ~2 ticks. The
declaration therefore runs ahead of the pixels by something on the order of one
step, and the compositor reprojects by an angle the frame was never drawn with.

The step size, computed per tick from the command model verified in take 212 at
99.3%:

| | median | p90 | p99 | max |
|---|---|---|---|---|
| IDLE | 0.58° | 2.76° | 5.75° | 9.89° |
| ARMED | 0.69° | **5.17°** | **11.96°** | 11.96° |

**The lead nearly doubles it in the tail.** So the lead reduces the tracking
error `lagfit` measures while roughly doubling the angle the compositor
reprojects by in error. Those pull in opposite directions on what a person
actually sees, and #73's objective -- minimise (head - camera) -- never
accounted for the second one.

## What is NOT established, and the instrument that would settle it

**The size of the mismatch AT RENDER TIME is unmeasured.** Pairing the layer's
`vr pose` lines against `camread` gives 6-7 deg, and that number is worthless:
`vr pose` fires once per 600 frames, the pairing window is 0.25 s, and the
camera moves further than the effect within it. Take 212 yields 7 pairs.

Worse, the arithmetic may cancel. At render time X4 has landed commands up to
n-2, so `declared - actual` is closer to `step_n - step_{n-1}` than to `step_n`
-- small for a smooth trajectory. **Whether the mismatch is one step or one
step's difference is exactly what is not known, and it is the whole question.**

The instrument is cheap and needs no run to design: log the declared pose and
X4's readback on the SAME line at full rate, from the layer, at submission.
Until that exists this section is a mechanism with a plausible size, not a
finding.

# #73 (1) — the acceptance threshold was unachievable, and that is my fault

**Task (1) asked whether `lagfit`'s tau is the wrong statistic under X4's
transport delay. Answer: not wrong, but its achievable value is a strong
function of that delay, and the threshold ignored it.**

Take 212's OWN head trajectory, replayed through the real `head_look_step` and
the plant take 213 identified (pure integrator, N ticks of delay), then fitted
with the same regression `score_run.py` runs:

| plant delay | IDLE tau | ARMED tau | ratio |
|---|---|---|---|
| 0 | 103.7 ms | 40.2 ms | **0.39x** |
| 1 | 118.3 ms | 58.7 ms | **0.50x** |
| 2 | 136.8 ms | 85.7 ms | **0.63x** |
| 3 | 159.5 ms | 116.4 ms | **0.73x** |

Take 213 measured X4's delay at **2 ticks**. Take 212 measured IDLE at 142.4 ms,
which sits between the delay-2 and delay-3 rows — consistent.

**So the best a perfectly working lead could read on that run was ~0.63x, and
the threshold was 0.45x.** That number came from a delay-1 RAMP simulation, and
a ramp is the lead's best case: it is exactly the input a velocity
feed-forward cancels. Deriving an acceptance bound from the easiest case is the
same error as the post-step convention earlier in #73 — twice in one task.

Threshold is now **0.85x**, derived from the table above: clear of the 1.00x a
lead that never reached the servo produces, and above everything a working one
gives at any plausible delay.

## But take 212 still fails it, and that residual is real

0.94x measured against 0.63x predicted at the matching delay. The statistic
explains most of the gap and not all of it. Four explanations have now died to
measurement — a 200 deg/s cap, a reversal asymmetry, plant nonlinearity in
general, and now the threshold — and **#73's cause is still open.**

The live candidate is task #75: the compositor reprojects the submitted image
to the head's current pose, so two systems move the view at once with different
dynamics. Establish what pose the layer actually declares per frame BEFORE
building anything on that.

## Take 213's camera did not freeze — the sequence finished

Patola reported the camera stopping 30-60 s in and never moving again. It
stopped at **t+29.8 s, at pass 2 element 13 size -104**: the last element of the
last pass. `dx` is zero for every tick afterwards because the sequence was
COMPLETE, which the log says on its own line.

The handover priced the run at ~3 minutes by assuming a 24 Hz tick. At 1408x1408
the tick rate was **76.9 Hz**, so it finished in 30 s and he watched a dead game
for 171 s. All 42 impulses landed before the freeze, so the measurement is
unaffected — but a run's price has to be quoted against the tick rate it will
actually have, not the one the previous take happened to run at.

# Take 213 — X4's camera is a PURE INTEGRATOR. Every plant story is dead

**Open-loop, head out of the loop entirely. 99.6% delivery at every command
size, and the camera stops dead when the commands do.**

| commanded | 0.23° | 0.46° | 0.92° | 1.84° | 3.68° | 7.36° | 11.96° |
|---|---|---|---|---|---|---|---|
| delivered | 99.6% | 99.6% | 99.6% | 99.6% | 99.6% | 99.6% | 99.6% |

There is **no cap, no momentum, no nonlinearity**. What there IS, and what
nothing before this run had measured: a **transport delay of 2 ticks, 42 ms**,
with 77% of the response landing in that single tick.

## That delay faked every finding take 212 built on

Take 212 credited each command's motion to the tick that issued it. The motion
arrives two ticks later, so every number it produced was really measuring the
delay:

* **"Delivery falls from 95% to 71% with size."** Artefact. The sustain burst
  is SHORT for big commands (they eat the runway fastest), so the largest sizes
  lose the largest share of their motion to the following phase. Summed over a
  window that outlives the delay, every size delivers 100%.
* **"Continuing pays 87–126%, reversing pays 21–49%."** Artefact. Delayed
  motion from the previous command adds when the new one continues it and
  subtracts when it opposes it. No nonlinearity required.
* **"The camera coasts 77% when commanded nothing."** Artefact — that was the
  delayed motion arriving, on n=128 through a closed loop. Open-loop, n=145,
  the coast is **0%**.

This scorer said **PER-STEP CAP** on its first reading of take 213, for exactly
the same reason. The verdict now comes from the IMPULSE response, which is
delay-immune, and the fix was to the metric — the run's data was right all along.

## Which means #73 is still open, and the plant is not the answer

The plant is precisely what `tests/servo_lag.cpp` models: an integrator with a
delay. The lead was designed for that plant and holds against it in simulation
across delays 0–4 on a realistic accelerating head (0.25x → 0.32x). Yet take
212 measured 0.94x with the lead provably reaching the servo (99.3% of its
commanded deltas matched the lead model).

**So the cause of take 212's null is unfound.** Three explanations have now
died — a 200 deg/s cap, a reversal asymmetry, and plant nonlinearity in general
— and each died to a measurement rather than an argument. Do not close #73 on
take 213; the scorer says so in the run itself.

**What is NOT yet ruled out**, and roughly in order of cheapness: that
`lagfit`'s tau is the wrong statistic under a 42 ms delay; that take 212's
23.6 Hz pose rate against its ~23 fps frame rate coalesced or dropped commands
in a way 76.9 Hz does not; and that the readback X4 gives us is itself delayed
differently from the camera it reports.

# Take 212 — the lead is CORRECT and does nothing. X4's response is not linear

**`lagfit` 0.94x, and the scorer's stated cause was wrong.** It said "the lead
is not reaching the servo". The lead was reaching the servo perfectly.

The exact check, against 941 commanded deltas the game actually received —
reconstruct the dx the servo should have sent, with and without the lead, and
compare to the `headlook: ... delta` lines:

| arm | matches a NO-LEAD model | matches the LEAD model |
|---|---|---|
| IDLE | **100.0%** | 49.0% |
| ARMED | 35.9% | **99.3%** |

So the arithmetic is right, the knob was read, the A/B toggled (16 phases,
exactly 20 s, `scale 1.00`), and the servo commanded exactly what the lead asks
for. The camera still ended up in the same place.

## The plant is not an integrator

`tests/servo_lag.cpp` models X4's camera as `cam += commanded`. Measured, it is
nothing of the kind. Delivery — how much of a commanded step the camera
actually performs — collapses as the command grows, and depends on **which way
it is already moving**:

| commanded | continuing the current motion | reversing it |
|---|---|---|
| 0–2° | 126.4% (n=1569) | 96.9% (n=517) |
| 2–4° | 101.9% (n=473) | 62.5% (n=103) |
| 4–6° | 95.3% (n=154) | 49.2% (n=33) |
| 10–12° | 87.5% (n=104) | **21.2%** (n=37) |

Over 1° commands the mean delivery was **85.2% in IDLE against 64.5% in ARMED**
— and ARMED issued 211 commands past 6° where IDLE issued 19. **The lead makes
commands bigger, and bigger commands are where X4 pays out least.** The closed
loop then absorbs the difference and settles at nearly the same error, which is
why 0.94x rather than a clean null.

## Two of my own explanations died on the way

**"X4 caps the camera at `anglespeed=200`."** Refuted: the camera reaches
**541 deg/s**, p99 350. An earlier draft of this section claimed the ceiling
matched 200 × dt = 8.42° "exactly" — that was an averaging coincidence across
varying frame times, and checking the speed distribution directly killed it.

**"The asymmetry is the mechanism — the lead fails because it asks for
reversals."** Refuted by restricting the fit to steady turns of 3+ samples in
one direction, where the plant delivers 95–126%:

| | IDLE | ARMED | ratio |
|---|---|---|---|
| steady turn | 151.4 ms (n=1294) | 149.3 ms (n=1123) | **0.99x** |
| reversing | 35.7 ms (n=175) | 41.0 ms (n=256) | 1.15x |

The lead does not help *even where the plant is linear*. Whatever X4 is doing,
it is not a per-step saturation that steady motion escapes.

## What is established, and what is not

**Established:** the lead is implemented correctly and commanded correctly;
X4's delivery is sub-linear in command size and strongly direction-dependent;
there is no hard 200 deg/s cap; the camera carries momentum (commanded ~nothing
while moving, it still travelled ~77% of the previous step, n=128).

**NOT established:** the form of X4's response. The coast-decay profile had
n=21 and was too noisy to fit a time constant — no model is claimed from it.

**The next measurement is `<lookaround><internal anglespeed angleacc>`** — the
same node as #68's `maxyaw`/`maxpitch`, which we already proved is live data
and already have an extension for. Raising them is a DIAGNOSTIC even if we
would never ship it: if the delivery curve straightens, the mechanism is named.

# #73 — the lag is the servo's ramp error, MEASURED from stored logs

**No run was spent on this.** Takes 209/210/211 already logged X4's own camera
readback beside the head angle (`camread:`), and the lag is the slope of
tracking error against head rate. Six measurements, two frame rates:

| take | pose rate | yaw τ | pitch τ | fps | τ / pose period |
|---|---|---|---|---|---|
| 209 | 85 Hz | 43.8 ms | 38.7 ms | ~240 | 3.73 / 3.30 |
| 210 | 85 Hz | 41.4 ms | 42.1 ms | ~120 | 3.51 / 3.57 |
| 211 | 26.6 Hz | 160.4 ms | 203.8 ms | ~20 | 4.27 / 5.42 |

**τ is not a constant — it scales with the pose period.** That refutes every
fixed-latency explanation at once: tracker latency, our render cost, and X4's
own `anglespeed="200" angleacc="500"`. A rate limiter inside X4 would hold τ
constant in milliseconds. It does not.

**The lag is the ramp-following error of a proportional controller** — a type-0
loop tracking a moving target sits behind by a fixed fraction of its step
period, forever. The servo steps once per published pose (x4vr_inject.cpp,
`if (!fresh) return;`) and the layer publishes one pose per submitted frame, so
**the lag is 3–4 frame-times**: ~40 ms at 90 Hz, ~180 ms at 20 fps.

**Which of 3T and 4T depends on where you sample, and getting that wrong cost
half a day.** Steady state on a ramp gives `e = e(1−kp) + rate·T`, so the error
*at the instant of observation* is `rate·T/kp` = **4T**, and after that tick's
correction has been applied it is `(1−kp)` times that = **3T**. X4's readback
reports the camera before our command lands, so `lagsample` and the historical
`camread` both measure 4T = 47 ms at 85 Hz. The logs read 38.7–43.8, and the
gap is in the expected direction: their head rate came from samples 350 ms
apart, and noise in a regressor *attenuates* a slope. So the stored logs are a
lower bound, not a disagreement.

**The first draft of this section predicted the lead would cancel the lag
outright.** It measured post-step while the instrument measures pre-step — a
whole frame of correction — and the two disagreed by 4× on identical data
(0.06× against 0.27×). The corrected arithmetic is below. Recorded rather than
edited away, because "my harness and my instrument measured different
quantities" is the same defect as [the selftest that passed only in its
simplest configuration](#), twice before.

## The harness reproduces take 136, which is why I believe it

`tests/servo_lag.cpp` drives the REAL `head_look_step` against a plant model.
It predicts 36.8 ms at 85 Hz (measured 41) and 112 ms at 26.6 Hz (measured
160–204). Independently, at `kp = 1.0` it reports *never settles, 46 deg/s of
camera jitter* — which is take 136's `X4 span so fast Patola could not read the
screen`, reproduced from arithmetic alone.

**Raising kp is a trap and the harness prices it.** kp 0.5 buys 12 ms but costs
+24.8% step overshoot; at a plant delay of 2 frames even kp 0.25 overshoots
13.9%. X4's true input-to-camera delay is UNKNOWN, so a gain that depends on it
is a gain that can fail in the field.

## The fix: lead the target by the head's own motion

```
want = head + (1/kp − 1) × (head − head_at_last_pose)
```

The lead lives in the **angle domain**, so it carries no clock and no `dt` and
is automatically correct at any frame rate. It sits OUTSIDE the loop, so loop
stability is untouched — this is feed-forward, not a gain change.

### PREDICTIONS for take 212, committed before it runs

With the lead, `e = 0.75e + 0.25·rate·T`, so the steady pre-step error is
`rate·T` — **exactly one frame time, whatever the frame rate**. 4T → 1T:

| condition | now (4T) | with the lead (1T) |
|---|---|---|
| 85 Hz | 47 ms | **12 ms** |
| 26.6 Hz | 150 ms | **38 ms** |

The ideal ratio is therefore **0.25**, not zero. A head that *accelerates*
leaves a second-order residual a velocity lead cannot model, and
`tests/servo_lag.cpp` measures **0.26 on a realistic trajectory at both rates**.
`score_run.py` fails above **0.45** — clear of that, and still under half the
1.00 a lead that never reached the servo produces.

1. **The A/B is visible in the log without a screenshot.** `lagfit` reports τ
   per arm; ARMED must come in under 0.45× IDLE at the same frame rate.
2. **The improvement is frame-rate independent.** Both arms are measured at
   whatever fps the run hits, and the ratio must hold across the spread. This is
   the claim that distinguishes a servo fix from a frame-time change, and it is
   why the arms alternate WITHIN one take instead of across two.
3. **The lead will amplify tracker noise ~4×.** Priced offline: at 0.05° of
   noise the camera drifts ~5 deg/s with the head still. `lagfit` reports
   `still` — camera rate while the head is stationary — per arm. If ARMED `still`
   exceeds ~5 deg/s the mitigation is the runtime's `XrSpaceVelocity` (which the
   layer does not currently request; it publishes yaw/pitch only) or a low-pass
   on the derived rate. **Predicted BEFORE the run so a bad number cannot be
   re-read as a success.**
4. **Yaw wrap and the pole.** The lead introduces a NEW yaw subtraction, and an
   unwrapped one across ±180 would command a −358° lead — the same class of
   defect the error term already carries a wrap for. It is wrapped, weighted by
   `yaw_weight`, and clamped; `tests/servo_lag.cpp` §7 covers all four,
   including a mid-run toggle differencing against a stale head.
5. **A handful of wild samples can set the answer.** Least squares weights by
   rate *squared*, so six samples out of 3186 at an implausible 1675 deg/s moved
   the fitted τ from 140 ms to 69 — a 2× error from 0.2% of the data. `lagfit`
   now drops anything past `12 × pose_rate` (the servo's own slew ceiling, where
   the error is set by the step clamp rather than the lag) and prints how many
   it dropped. A dropped count in the hundreds means the pose stream is
   glitching and the τ below it should not be read.

**What would refute the diagnosis:** ARMED and IDLE reporting the same τ. That
would mean the lead is not reaching the servo, and the first thing to check is
whether `lead_scale` was read at all — not to add a knob.

# Take 211 — the fade is LIVE, and 4224² is not viable on this hardware

**`deep 0.575` against the 1.19 the weight permits, and Patola looked fully up
and down twice with no gimbal lock.** The progression 11.960 → 2.070 → 0.575 is
the monotone damping the smoothstep is for. Take 210 measured a guard that was
switched off; this one measured it working.

Also confirmed by hand at 4224²: **HUD elements click normally**, before and
after steering, and **looking around does not disturb steering**. The mouse
grab survives a render larger than the 3440x1440 screen.

### The framerate is the resolution, not the instrumentation

| take | eye | upscaler | head-look | GAMEPLAY |
|---|---|---|---|---|
| 210 | 1408² | — | yes | **8.48 ms** (118 fps) |
| 168b | 4224² | fsr_quality | no | **34.87 ms** (29 fps) |
| 211 | 4224² | fsr_quality | yes | **44.36 ms** (23 fps) |

**168b already ran 29 fps at this extent with no head-look instrumentation at
all**, so 4224² was unviable before any of it. 9x the pixels for 5.2x the frame
time is sub-linear, which is what upscaling buys and is still far too slow.

The reprojection Patola saw is visible in the counters: **6331 blits against
19026 submitted layers — each rendered image shown about three times.** X4's
rendering is the bottleneck; the VR loop is running fine and repeating what it
has.

**The 27% gap between 168b and 211 is real but confounded** — different scenes,
different durations, and 211 had a player actively moving their head, which
changes what is drawn. `X4VR_CAMREAD`/`X4VR_CAMLOOP` call into X4 per frame and
are the obvious suspects. Settling it needs one A/B at the same extent with
those two omitted, and that is only worth spending if 4224² is ever back on the
table. It is not: **29 fps is not a VR framerate at any instrumentation level.**

# #68 CLOSED: the clamp is reachable, but not without `modified="1"`

Four runs, each a single-variable step, and the route is settled.

| run | substitution | `modified` |
|---|---|---|
| 205b (1st) | silent — hooked to the wrong door | **0** |
| 205b (2nd) | content only | **1** |
| 205c | content **+** the member's MD5 in `08.cat` | **1** |

The first row is the control the second needed: byte-identical run, the
substitution not firing, flag clean. So **the substitution is the single
variable and X4 verifies the content it reads.** The MD5 hypothesis is refuted
outright — index and content were consistent and it flagged anyway.

That leaves the signature. `08_sig.dat` holds `libraries/parameters.xml.sig`,
**1024 bytes** — RSA over the exact file — and X4 opens every `_sig.cat`/
`_sig.dat` at startup. Not forgeable.

### The seam that exists, and why it is not taken

205c shows X4 reading the same 144 KB window **three times** (hits #2, #3, #4,
all `17600512..17747968`). If one of those is the verification pass and the
others are the parse, feeding the original bytes to one and patched bytes to
the others would very likely defeat the check.

**That is not a route this project will take.** Keeping a checksum consistent
with content we changed is bookkeeping; feeding a verification routine
different bytes than the parser gets is deceiving an integrity control that
exists precisely to detect this modification — and it would ship, making other
people's installs report unmodified when they are not. The flag is how a player
tells the difference. Suppressing it is not ours to do on their behalf.

### What remains

* **The extension.** X4's own supported mechanism: opt-in, visible in Settings
  → Extensions, removable by deleting a directory. Costs `modified="1"`, which
  every other X4 mod also costs. **This is the recommendation.**
* **In-memory, after the parse.** Let X4 read and verify its pristine file,
  then change the value in its parsed state. No integrity check is deceived —
  the files genuinely are unmodified and the verification correctly says so.
  Consistent with what the layer already does (SPIR-V, projections, descriptor
  sets). Costs: finding the value in memory, which is version-fragile and
  unbuilt. **Open, not recommended without a reason to pay for it.**

### Cheaper check than saving, for next time

Patola: **X4 states "modified" at the top of the menu screen at startup.** The
save-and-inspect cycle used through all of #68 was never necessary to read the
session's state — only to read a *file's*. Worth using first.

# Take 204 — the clamp MOVED. 85 yaw is right; 85 pitch is our own singularity

**`MOVED ✓` — X4 reached exactly 85.00 deg in BOTH axes** against a command of
85.00, pinning to the new limit as precisely as it used to pin to 65.00/35.00.
`fileprobe` independently confirms X4 read the extension's `libraries/` (2
probes, which take 203 did not have).

So the free-look clamp is **data, live, and reachable**. `libraries/parameters.xml`,
`<playercamera><lookaround><internal maxpitch maxyaw>`, overridden by an
extension carrying a 4-line `<diff>`.

### Yaw 85 is a good number. Patola's eye, which is the only judge here

*"I have to look very far to the left to finally see some black strip on the
left, and that's when I stopped. Same for the right. For looking up I couldn't
even get to this point before getting to the limit of my own neck."*

At `X4VR_FOV=1.4917` the rendered field covers essentially the whole of an 85
deg turn. **The guard band is not the binding constraint at this yaw** — the
neck is. That retires the worry that raising the clamp would just expose black.

### Pitch 85 walked into a singularity we had already documented

*"some kind of gimbal lock, with it doing jumps of rotations of the camera."*

**Ours, not X4's, and predictable from a constant ten lines above the limit I
was setting.** `head_angles()` in `common/x4vr_headlook.hpp` ends with

    a.yaw_reliable = horiz > 0.09f; // ~85 deg of pitch

Yaw is a singularity when the nose points at the ceiling: `fx` and `fz` both
collapse and `atan2` returns tracker noise. **Take 128 found that the expensive
way** — Patola looked up, garbage yaw drove the estimate somewhere arbitrary
and it STAYED there. The guard holds `want_yaw` at the last command while
unreliable, so crossing in and out of the region freezes and resumes the yaw:
exactly the jumping reported.

Setting `pitch_limit_deg = 85` put the limit **exactly at the guard's own
threshold**. I took X4's `trackmaxpitch="85"` at face value without checking it
against our extraction, which is documented in the file I was editing.

**Pitch must stay clear of it.** The guard trips at ~84.8 deg, so a limit near
**70** keeps 15 deg of margin and is still double X4's stock 35. Yaw has no
such constraint — the singularity is in pitch only.

### The FAIL, and it may be the same event

`the pose fell back to identity 1 time(s) AFTER it had been driven` —
`cam_valid` went to 0 mid-run, so those frames were submitted world-locked.
**One occurrence**, and the gimbal-lock episode is a candidate cause: if the
servo lost the camera while yaw was unreliable, that is what it would look
like. Not established, and worth re-checking on a run with pitch limited below
the singularity — if it disappears, the cause was the pitch limit.

### Loading bar at ~68%: it IS the `modified` flag, and my control was blind

I said the bar correlated with the **extension** being active and that
`modified` was *"a consequence of the same cause, not a cause of anything"*.
**Wrong.** Patola ran a proper 2x2 and it tracks the SAVE's flag in both
directions, with the extension state irrelevant:

| extension | save | bar |
|---|---|---|
| ON | unmodified | 100% |
| OFF | unmodified | 100% |
| ON | unmodified → then quicksaved as modified | 100% → **68%** |
| OFF (no unofficial extensions at all) | modified | **68%** |

The last row is decisive: no extension anywhere, modified save, 68%.

**And the control I proposed could not have found this.** I suggested "remove
the extension and see if the bar returns to 100%" — but removing the extension
also meant restoring an unmodified save, so both variables would have moved
together and the confounded result would have confirmed my wrong hypothesis.
Patola varied them independently. His experiment was the better one and mine
would have entrenched the error.

Benign either way: X4 presumably skips a verification step for modified saves,
so the progress estimate measures against a total it no longer spends. Nothing
to chase, and now explained rather than guessed at.

# Take 203 — RESULT. Two of four predictions right, and one instrument blind

`PASS`, and a candidate for known-good-runs. Scored against the predictions
filed before the run, which is the point of filing them.

### 1. The #66 fix is CONFIRMED ✓

**No `csm-scale=inf` anywhere**, and take 202's `FAIL` is gone. The walk no
longer reads regions X4 recycled. `objblock` reports 106 slots against 202's
167 — I said "far fewer", which oversells a 37% drop, but the defect the number
was a symptom of is what mattered and it is gone.

The `csm0 scale` set is now `0, 4.92e-07, 2.70e-06, 0.154203, 0.154204,
108.812` — six finite values where 202 had `{0.154204, inf}`. That is the
predicted behaviour, not a regression: member 3 is `world x lightVP`, so
`|row0|` is the object's own scale times the cascade's, and a heavily scaled
object reads 108.812 while a huge one reads 4.9e-07. The two 0.15420x values
are the unscaled objects. The layer labels the spread EXPECTED and does not
call it a cascade.

### 2. `livesets` read 3 slots. WRONG, and the instrument is blind ✗

Predicted "accumulating in the hundreds". It logged **3**.

Not a fact about X4 — a fact about when I sampled. The line fired once, at the
first report (`t=267548.845`, the same instant as the first `proj CAMERA`),
when three slots had been written. The **539** in the `view` section is
cumulative over **126** reports. I conflated the two, so the instrument
measured the one moment where the accumulation it exists to detect has not
happened yet.

Same shape as take 198's blind control (`X4VR_DUMP_MATRICES` caps at 8 reports,
all at load) and as *"a complete survey of one instant"*. **#67 is not
answered.** The layer now reports a growth series — the first report, then each
doubling, capped at 8 — and the scorer reads the LAST line.

### 3. `fileprobe` answered #68, with its control ✓

* **`camerasettings` probed at 1 user-level path** — the positive control
  fired, so this run reached the code.
* **No user-level `parameters.xml` probe at all.** 186 distinct interesting
  paths, 150 archive opens, well under the 400 cap.

**The cheap door is CLOSED.** X4 does not look for a user-level
`parameters.xml`, so the existing `fopen` route cannot serve the clamp. #68
needs the extension route — and that puts the save's `modified="0"` squarely on
the table before anything is spent.

### 4. "Everything else matches 202" — half right ✗

Identical, as predicted: the `sx` set `0.70024 / 1.33333 / 1.00000 / 3.78085`,
`classify 19 / 0 World`, `pose-channel 1105/1105 OK`, thresholds
`[5.2 23.9 195.1 1338.7]`, patch-site counters `live-sx=296 mvp-sx=12
baked-sx=0`, and perf 6.75 ms inside the predicted 6.4–7.5 band.

**The `view` section is not.** 202 was dominated by `14.7–14.9 deg / 8.6 m`
cameras with thousands of draws; 203 has **no 14.x entry at all** and leads
with `116.6 deg / 0.00 m` and `124.4 deg / 14874.90 m`. The first cameras
reported differ in kind: 202's read `M_view is NOT IDENTITY -- 14.90 deg,
offset 8.755 m`, 203's read `M_view IDENTITY`.

**Scene state, not code.** Every pipeline-side number is byte-identical, and
nothing I changed touches the camera credit path — `bound_live`/`sets_live` are
additive and separate. The two takes caught different moments. But it means
**203 is not a clean camera-side comparison to 202**, and any future prediction
of "the view section will match" needs the scene pinned harder than "load and
wait for the fade".

# THE RUN THAT PRODUCED THIS — take 203, flatscreen, three passive instruments

    X4VR_POSE_ROTATE=1 X4VR_TAKE=203-PROBE X4VR_STEREO=1 X4VR_BINDLESS_PATCH=1 X4VR_SHEAR_LIGHTS=1 X4VR_DUMP_MATRICES=1 X4VR_RES=1408x1408 X4VR_GAMESCOPE=1 X4VR_SBS_RIGHT_LAYER=1 X4VR_SBS_LAYERS=2 X4VR_POSE=1 X4VR_POSE_YAW=20 X4VR_MV=1 X4VR_PROJ_LIVE=1 X4VR_SBS=1 X4VR_LOG=/tmp/x4vr-take203.log X4VR_PIPE_INVENTORY=1 X4VR_MASK_PRESENT=1 X4VR_IPD=0.064 X4VR_FOV=1.4917 X4VR_BINDLESS_MIRROR=1 X4VR_MV_INVENTORY=1 X4VR_FILEPROBE=1 ./launch/x4vr-launch.sh

Derived from take 202's logged `env: run =` line. **Only three tokens differ**:
`X4VR_TAKE`, `X4VR_LOG`, and the added `X4VR_FILEPROBE=1`. Same field, same
yaw, same everything else — so every number is directly comparable to 202.

**Flatscreen on purpose.** `X4VR_VR` is absent, as it was in 202. Both open
questions this take answers are answerable without a headset, and the third
instrument (`pose residual`) needs the runtime's frusta and will stay silent —
that is expected, not a failure. It gets its own VR take later.

### Predictions, filed before the run

1. **The take-202 `FAIL` is gone.** No `csm-scale=inf` anywhere, and
   `objblock` reports far fewer than 167 slots. 202 accumulated every
   descriptor write ever made to the set and read regions X4 had recycled;
   writes supersede now. *If `inf` survives, the fix did not take and the
   scorer will FAIL on it by name.*
2. **`livesets` shows a large gap** — accumulating in the hundreds (202
   decoded 600 camera slots), superseding a handful. Whether the WINNER moves
   is genuinely unknown and is the number #67 is waiting on.
3. **`fileprobe` sees `camerasettings`** at a user-level path. That is the
   POSITIVE CONTROL, because the player has one on disk. Whether it also sees
   a user-level `parameters.xml` is the open question and either answer is
   informative.
4. **Everything else matches 202**: `sx` set `0.70024 / 1.33333 / 1.00000 /
   3.78085`, `classify … 0 of them also reported World`, `pose-channel … OK`,
   thresholds `[5.2 23.9 195.1 1338.7]`, gameplay perf in the 6.4–7.5 ms band.

### What each answer unlocks

* `fileprobe` finds a user-level `parameters.xml` → **#68 is nearly free**: the
  injector's existing `fopen` path serves it, nothing is written, the save's
  `modified="0"` is untouched.
* It does not, but the control fired → #68 needs the extension route, and the
  `modified="0"` question has to be settled before spending it.
* The control did NOT fire → the run never reached that code and the take says
  nothing either way. The scorer says so rather than reading silence as "no".

### Cost

One ordinary flatscreen take, no headset, no flying. The probe is deduped and
capped at 400 paths and the `livesets` walk is gated behind
`X4VR_DUMP_MATRICES`, which this take already sets — so neither should move the
perf number. Prediction 4 includes that band precisely so a surprise is visible.

# RESUME HERE AFTER A COMPACT — state as of take 211

Everything below supersedes earlier "read this first" banners, including the
take-202 one further down.

## Physical state of the machine right now

* **The clamp extension is DISABLED** and the quicksave is restored to
  `modified="0"`. Verify both before reading anything into a run:
  `grep -o 'x4vrmod_lookaround" enabled="[^"]*"' ~/.config/EgoSoft/X4/96679891/content.xml`
  and `zcat .../save/quicksave.xml.gz | head -c 400 | grep -o 'modified="[^"]*"'`.
* The extension itself still exists at
  `<X4>/extensions/x4vrmod_lookaround/` carrying **maxyaw 180, maxpitch 75**.
  Delete that directory to remove it entirely.
* Last take is **211**. No run is pending.

## What is SETTLED, do not re-litigate

* **#68 is closed.** X4's free-look clamp is data in `libraries/parameters.xml`
  and raising it WORKS — take 204 pinned 85.00/85.00, take 211 reached
  177.98/75.00. The only delivery that works is **X4's extension system**, and
  it costs the savegame's `modified="1"`. Both alternatives are REFUTED by
  measurement: read-substitution of the archive (`X4VR_CATPATCH`) because X4
  verifies content and the protection is an unforgeable RSA signature, and
  patching the retained XML text (`X4VR_PARAMPATCH`) because X4 parses once at
  startup and never re-reads. Those knobs still exist; **do not spend runs on
  them again.**
* **The design decision:** ship the mod at X4's stock 65/35 with clean saves,
  and offer the clamp as a documented **opt-in extension**. Patola agreed. The
  mod itself has always been `modified="0"` — only the clamp costs the flag.
* **The yaw fade is live and confirmed** (take 211: `deep 0.575` against the
  1.19 the weight permits, and Patola looked fully up and down with no gimbal
  lock). Quaternions do NOT remove the singularity — X4's mouselook is a
  two-axis channel and the map is degenerate at the poles however computed.
* **4224² is not viable**: 29 fps with NO instrumentation (take 168b). Not an
  instrumentation problem.
* **Steering is independent of head-look** and **HUD clicks work** — both
  confirmed by hand at 4224² in take 211. Protect these.

## Open, roughly in order of value

* **#71** mouse suppression — real mouse motion does nothing while head-look is
  armed. Patola confirmed mouse steering dead, arrow keys fine.
* **#72** head-look keeps driving in external view despite logging "idle"
  (command reached 163.80 there against 69.95 in cockpit).
* **#73** the lag, reported since the beginning. Structural: a P controller
  steering through mouse deltas, so it worsens as the framerate drops.
* **#67** flip the camera-slot walk once a take shows how much attribution
  moves — `livesets` reports it and rides free on any `X4VR_DUMP_MATRICES` run.
* **#70** cockpit shadows darken as the head yaws right.
* **#33's remaining piece** and #41/#43/#44/#45/#46/#49/#52/#57/#25 unchanged.

## Instruments added this stretch, and how to read them

* `clamp` **segments by view** — `<external>` declares no `maxyaw`, so external
  samples are unbounded by design and are excluded. Pre-transition samples are
  credited by inferring from the first transition's direction. Aggregating the
  two populations is what produced a false `MOVED` on take 208.
* `fade` reads the injector's **full-rate** tracker, not the sampled headlook
  lines — those are every 30th call and a doctored 65 deg cliff hid inside take
  209's own level baseline. Threshold 1.19 is DERIVED: `max_step_deg` 12 x the
  weight 0.0994 at 82 deg.
* `fileprobe`, `livesets`, `objblock` as described in their own sections.
* New knobs: `X4VR_HEADLOOK_YAW_LIMIT` / `_PITCH_LIMIT` (default 65/35, so
  omitting them is the control), `X4VR_FILEPROBE`, `X4VR_PARAMSCAN`.

## The pattern that cost the most this stretch

**Four separate instruments reported confidently on data they were not
measuring**: the clamp aggregating two camera populations, the clamp again
judging on 24 of 537 samples, the fade inferring from a sampled log, and the
yaw guard that had never been wired into the real path at all. Every one was
caught by Patola's own account disagreeing with the log. **When his eye and the
instrument disagree, the instrument is wrong until proven otherwise.**

## Where the project is

* **The x=791 seam is SETTLED** — tagged `stage18-seam-laws-confirmed`. It is
  **geometry, not a defective transform**: the guard edge
  `(sx·sinθ − cosθ)/(cosθ + sinθ/sx)` and the bisector `sx·tan(θ/2)`, both
  confirmed to **0.1 px** against two independent variables (θ in take 199,
  `sx` in take 200, which moved the two edges in *opposite* directions).
  **Nine transform hypotheses died and none was the cause.** Do not reopen it;
  closing the band is a DESIGN question (render wider, or accept it).
* **#33's rotation is confirmed** and the classify fix is in
  (`stage17-classify-x4s-bytes`). Still not built: the runtime head-pose
  source — the rotation is a static `X4VR_POSE_YAW`.
* **#56 is ANSWERED** — from stored logs, no run. X4's cascade **splits** move
  with `<fov>` (nearer as the field widens); the **atlas** does not. The guard
  band's shadow price lands on the **far** field. Full evidence in the take-202
  section below. **`csm0-scale` is not the quantity** — `V_csmthresholds` is.
* **The object-block capture is in but is NOT #56's evidence.**
  `read_shadow_terms` reads `M_shadowCSM0`, member 3 of the object block (set
  3, 189 samplers) rather than the dead `M_shadowCSM0Clip` — but that member is
  **per-object** and composes the draw's own source frame, so its `|row0|` is a
  cascade scale only for a view-space reader and the walk does not identify
  which. Take 202's `FAIL` came from this and from a walk that was reading
  recycled memory; both are fixed, and neither was a defect in X4.

## Take 202 — #56 ANSWERED, and the FAIL was my own instrument

Take 202 ran and FAILed: *"the frame's object slots carry 2 distinct cascade
scales"*. **Both halves of that line were wrong** — the invariant it asserted
and the walk it asserted it about — and #56 turned out to be answerable from
logs already on disk, with no run at all.

### The answer

**X4's cascade split distances DO respond to `<fov>`. The atlas does not.**

`V_csmthresholds` is a `vec4` in the camera block at float 232, and X4's own
`common_frag.glsl:1071` does `if (depth < V_csmthresholds[i])` — so these are
the cascade split distances in view metres, read live from X4 rather than
derived by us.

| fov | fov-sensitive set | also present |
|---|---|---|
| 1.4917 | `[5.2 23.9 195.1 1338.7]` — **takes 190–199, 201, 202** | `[7.2 33.3 269.9 1856.4]` |
| 1.75 | `[4.7 21.1 173.6 1189.6]` — take 200 | `[7.2 33.3 269.9 1856.4]` |

The control was already run **eleven times**: every stored take at fov 1.4917
gives the identical set to the printed precision, across different yaw, shear
and classification knobs. Takes 200 and 202 differ in `X4VR_TAKE`, `X4VR_LOG`
and `X4VR_FOV` and **nothing else** — a single-variable comparison.

`mapsize` 2048, `maxdist` 5000 and `factors` [1 1 1 1 1] are unchanged at both
fields, as is the 10 × 2048² D16 atlas.

**So the splits move NEARER as the field widens** — ×0.888 for a ×1.173
widening. That is neither hypothesis in #56's framing: fitted-to-frustum would
make cascade 0 *grow* with the field, fixed-extent would leave it alone. With
the atlas fixed and the splits pulled inward, the last cascade covers
1189.6→5000 m instead of 1338.7→5000 m on the same texels — **the guard band's
shadow price lands on the far field, not the near one.**

**Direction established, law NOT.** ×0.888 matches neither `tan(θ/2)` (0.681)
nor `1/sx` (0.681) nor `cos` (0.751). Two points cannot fit a law and I am not
going to pretend otherwise; a third field would discriminate and costs one
take, whenever the law actually matters for a decision.

### Why `<fov>` reaches X4's CPU at all

Because `X4VR_FOV` is a **config override**, not a projection patch
(`injector/x4vr_config.hpp:109`) — the injector serves X4's own `<fov>` tag, so
X4 computes its splits from it. That is task #24's non-intrusive path paying
off in a place it was not designed for.

### The camera that carries the shadows is not the one we widen

In take 202, **85 camera slots only ever show `sx=1.33333`** and **38 only ever
show `sx=0.70024`** — disjoint, zero overlap, so these are two real populations
and not one slot sampled before and after our own rewrite. **All 58
shadow-configured slots are in the 1.33333 group; none is in the widened one.**
X4 never attaches a cascade configuration to the camera whose field we widen,
yet the splits it uploads still respond to the setting.

### What the FAIL actually was

Two separate defects, both mine.

**The invariant was false.** The warning said *"the CSM is light-space and
should be identical in every slot"*. X4's shipped GLSL refutes it: there is
exactly **one** declaration of `M_shadowCSM0`, member 3 of the per-**object**
`BUFFER_BINDING_SLOT_WORLD` block — and X4 feeds it three different source
frames. `mul(vec4(Pos,1), M_shadowCSM0)` with object-space `Pos` in 186 vertex
entry points, `mul(vec4(IO_world_pos,1), …)` in others, and
`mul(vec4(view_pos,1), …)` in 54 fragment ones. It carries the draw's own
transform composed with the cascade, so **a spread is the expected reading**,
and `|row0|` is a cascade scale only for a view-space reader. Fifty-odd takes
of `csm0-scale` were chasing a quantity that never had the meaning assumed.

**And the walk was reading dead memory.** `obj_sets` appended one entry per
descriptor write and was **never cleared**; `obj_bound` was never cleared
either. X4 re-points set 3 binding 0 at a fresh sub-allocation per draw — 13748
binds in this take — so the walk accumulated 167 slots and credited *all of
them on every draw*. About half had been recycled by the time the report ran,
which is where `csm0-scale=inf` came from, and the "2 distinct scales" was
`{0.154204, inf}`. A descriptor write **replaces** a binding; the layer now
models it that way, keyed by `dstBinding`, and clears `obj_bound` at
`vkBeginCommandBuffer`.

`tests/run-multiview-render.sh` gained `X4VR_TEST_OBJ_REWRITE`, which writes
one binding twice and asserts the second wins with **one** distinct scale. It
fails on the old code and passes on the new. The pre-existing cases pass either
way — the fixture only ever wrote once, which is exactly why this hid.

**The same rewrite silently disarmed three negative controls**, because they
grepped for wording I deleted (`cascade 0 spans`, `OBJBLOCK: WARNING`). All
three are re-pointed at the current text with a note to move them together.

### The rule this broke, again

**Sweep the stored logs for the line a proposed run would produce, before
specifying the run.** The three-take plan for #56 asked for a fixed-field
control that eleven takes already contained. This is the second time; the first
cost a measurement ten prior takes already had.

### What is still open

`kObjBlockBytes` was `576`, a guess from a dump. X4 binds **768** — measured,
13748 binds, no other range at set 3. The declared members reach ~560 bytes and
768 is that rounded to a 256-byte alignment. Corrected.

**CORRECTION, made while building #67.** This section first said *"`g_track.bound`
(the camera slots) has the identical never-cleared defect"*. **That named the
wrong member.** `bound` **replaces** on each bind (`bound = it->second`) — I
inferred the defect from the absence of a `.clear()` without reading the
assignment at the bind site, which is the same mistake the false CSM invariant
was.

The camera path's staleness is real but lives in **`g_track.sets`**: it appends
one entry per descriptor write, dedupes on (buffer, offset), and is never
cleared *or capped*. `bound` then copies that whole accumulated list and
`credit_draw` credits every entry on every draw. Take 202 decoded **600 slots**
with **125 of them carrying three distinct projections each** — that is the
signature, and it is the likeliest explanation for *"cam#N is a UBO slot, not a
camera"*.

Still not fixed, deliberately. Every settled camera number was measured through
the accumulating walk, and the one thing that could not be established offline
is *how much* flipping it moves. So **task #67 reports instead of acting**:
`sets_live` records the same writes with the semantics `vkUpdateDescriptorSets`
actually has, and one `proj LIVESETS` line per run says what the superseding
walk *would* have picked. Nothing above it uses that walk. The default flips
once a real X4 frame has shown the size of the change — and that line rides
free on whatever take runs next, so it confounds nothing.

`tests/run-multiview-render.sh` runs it twice: written once the two walks must
agree (an instrument that invents differences is useless), and with one binding
re-pointed at a region nothing else names they must differ in **both** the slot
count and the winner. Before that second case the `DIFFERS` branch had never
been seen to fire.

## #33: the log sweep — residual, and X4's clamp is 65/35 not 56.5

Swept before scoping any work, and it answered the design question without a
run. **The runtime pose is already located, converted and published** —
`locate_views` every VR frame, `views[0].pose.orientation` → `head_angles()` →
`head_share_write()`, which drives X4's own free-look through the injector. The
#59 channel (`g_pose`) is the *only* thing still fed a static Euler.

So the question was never "which OpenXR call". It is **what job the shader
conjugation does now that the camera-driving path exists**, since both rotate
the view and nothing guards against having both on.

### The instrument was already there

`inject camread: X4 <y>,<p> deg (raw …) | cmd <y>,<p> | head <y>,<p>` carries
all three angles. **6962 samples across 16 takes.**

**Two-thirds of them are a dead read-back** — 4564 of 6962 have `raw 0.0000,
0.0000`, X4 reporting nothing rather than reporting zero. Taken at face value
they would have said "the residual is ≈0" and closed the question backwards.
Only the 2398 live samples mean anything.

### The residual has two regimes

`|head − X4|` yaw, live samples only, binned by `|head|`:

| bin (deg) | n | median | p90 | max |
|---|---|---|---|---|
| 0–10 | 1625 | 0.31 | 22.15 | 74.74 |
| 10–20 | 249 | 3.32 | 24.73 | 81.64 |
| 20–30 | 156 | 1.99 | 28.80 | 91.28 |
| 30–40 | 110 | 1.15 | 15.16 | 56.30 |
| 40–50 | 110 | 0.22 | 3.27 | 54.43 |
| 50–56.5 | 21 | 0.54 | 3.67 | 10.79 |
| 56.5–70 | 19 | 1.18 | 12.06 | 12.78 |
| **70–200** | **108** | **27.15** | **48.67** | **79.37** |

Take 149 alone (n=571, all `CLOSED`, the cleanest session): median **0.22°**,
p90 **2.27°**, max 14.28°. The high p90 in the low bins is fast-motion
transient plus four early takes whose servo was not tracking at all (136, 139,
141, 147 have medians of 63°, 21°, 12°, 6°) — those are in the table because
excluding them would be selecting on the outcome.

**So: below the clamp the servo tracks to a few tenths of a degree. Past it the
residual is unbounded** — head reaches 136°, X4 stops, and the difference is
everything above ~27° median.

### X4's clamp is 65.00 / 35.00, and three comments said 56.5

`|X4 yaw|` never exceeds **65.00°** in any of the 2398 live samples, and 65.00
occurs **129 times exactly** while every other value above 60 occurs once.
`|X4 pitch|` never exceeds **35.00°**. Round numbers, hard stops.

**CORRECTION — that paragraph is circular on its own, and I published it as
confirmation.** The largest |command| in the same 2398 samples is *also*
exactly 65.00: the servo clamps its own target there, so "X4 never exceeded 65"
partly restates our own ceiling. An instrument whose subject cannot exceed the
limit it is being tested for cannot establish that limit.

**What is not circular is the overshoot.** 303 samples show X4's camera going
PAST the command and still pinning to exactly 65.00 — furthest is **X4 65.00
against a command of 53.04** — and 256 do the same in pitch, **35.00 against
26.83**. A ceiling that were merely ours would be sailed straight through by an
overshoot. That, plus take 136 (which ran with the OLD 56.5/40 limits and still
read 65.00/35.00), is the evidence. The maximum never was.

That matches `yaw_limit_deg = 65.0f` / `pitch_limit_deg = 35.0f`, which
`x4vr_headlook.hpp` has used all along — **the constants were right and three
comments were wrong**, all repeating take 117's superseded 56.5: the header's
own rule 2, twenty lines above the constant that contradicts it;
`x4vr_layer.cpp`'s declared-pose comment; and `tests/headlook.cpp`, whose case
`head_look_step(mild, {56.5f, 0.0f}); // exactly at the clamp` was **not at the
clamp at all**, so it never exercised `clamped`. All corrected, test still
passes.

### What this decides

**Residual, not replacement.** The conjugation is a sub-degree correction
almost always, which is the regime reprojection handles well. Its real job is
the region past X4's clamp.

**And that region is bounded by the guard band, not by the conjugation.** At
`X4VR_FOV=1.4917` the rendered half-field is ~55°. A head 71° past where X4 is
pointing (136 − 65) is asking to see something X4 never drew — reprojection
cannot invent it, and the result is the black edge. So "look past the clamp"
is a **#24 guard-band question wearing a #33 costume**, and the two have to be
sized together.

Caveat on frequency: ~5% of live samples sit past the clamp, but these are
takes where the extremes were being deliberately exercised. That fraction says
nothing about normal play.

## Is X4's 65/35 reachable? It is DATA, and there is an 85/85 beside it

The guard band cannot buy the past-the-clamp region at any price, so the clamp
itself was the only lever left. It turns out not to be compiled in.

**`libraries/parameters.xml`** (inside `08.cat`), under `<playercamera>`:

    <lookaround>
      <internal maxpitch="35" maxyaw="65" anglespeed="200" angleacc="500"
                trackmaxpitch="85" trackmaxyaw="85"/>
      <external maxpitch="85" .../>
      ...
    </lookaround>

`maxyaw="65"` and `maxpitch="35"` are **exactly** the measured stops, to the
digit. So the limit is data X4 parses, not a constant in the executable — and
that is the first requirement for reaching it non-intrusively.

**And the same element carries `trackmaxpitch="85" trackmaxyaw="85"`.** The
`track` prefix reads as head-tracking, and 85/85 would convert most of the
expensive region into something X4 renders *properly* — no reprojection, no
guard band, free.

**SETTLED: do not pursue `trackmax*`.** It is a VR-shaped name in a binary
whose entire VR surface is residue, and the section *"The residue is much wider
than `IsVRMode`"* above ends with an instruction I did not follow before
spending on this: **"Before spending anything on a VR-shaped name in this
binary, re-read this section."**

Re-reading it answers the question. What would have to switch X4 into "tracked"
mode is its head-tracker path — the twenty-two `*ActiveHeadTracker*` accessors,
`IsHeadTrackingActive`, `GetOpenTrackConnectionStatus`, and the
`enableopentrack` config tag sitting at `false` in the player's own file. All of
it is listed there as residue, reopened four separate times, and OpenTrack is a
forbidden path besides.

And the evidence I had gathered for `trackmax*` is exactly the class that
section names as insufficient. `trackmaxyaw` and `trackmaxpitch` DO appear in
the binary — but in an **alphabetically sorted attribute-name interning table**
(`…sway, tactical, takeoff…`), which is consistent with the names being
data-derived and proves only that they were *named*. X4's own
`libraries/parameters.xsd` does not cover this section at all, so the schema
adds nothing either. Quoting the rule it broke: *"an exported symbol, a
vocabulary string and a config tag are each evidence that a feature was named,
never that it is implemented here."*

**So #68 raises `maxyaw`/`maxpitch` directly.** Those are proven live to the
digit — the overshoot pins to exactly 65.00 and 35.00 — and depend on no
VR-shaped name at all. The 85/85 pair is recorded here only so the next person
does not re-find it and think it is a door.

### The delivery problem, which is the real open question

`parameters.xml` lives **inside `08.cat`**, not loose. The injector interposes
`fopen`/`open`/`openat`, which is how `<fov>` and `inputmap.xml` are served,
and both of those are loose files. An archive member is not reachable that way.

Three routes, in order of how much they respect "the player's files are read
once and never written":

1. **Synthesise an extension in memory.** X4 enumerates `extensions/` and
   merges `extensions/<name>/libraries/parameters.xml`. Interposing
   `opendir`/`readdir` alongside the existing `open` family would let the
   injector present an extension that exists nowhere on disk. Nothing written,
   same mechanism class as everything else the injector does.
2. **Archive-level interception** — hook the reads into `08.dat` and substitute
   bytes at the member's offset. Doable (the `.cat` index is plain text and the
   offset is a running sum) but it means owning X4's archive format.
3. **Write a real extension directory.** Simplest, and the only one that
   modifies the player's install. Against the standing constraint.

Route 1 is the one that fits. It is not built.

### What #68 carries

`maxyaw` and `maxpitch`, raised directly. Not `trackmax*` — see above.

The value to raise them *to* is open and is a comfort question, not a technical
one: `<external>` and `<externalengine>` in the same file already use
`maxpitch="85"`, so 85 is a number X4 itself considers reasonable for a
look-around, but the internal view has cockpit geometry behind it and the
honest answer is what the canopy actually shows. That is Patola's eye, not a
measurement.

**The one thing that could still refuse:** that these values are read once into
a camera object and clamped again somewhere downstream. Nothing suggests it,
but #68's first take is what would show it — X4's own readback going past 65.00
is the whole acceptance test, and it needs no new instrument.

Extracted to `/tmp/x4-libs` with `/tmp/x4cat.py`. **X4's content — never
committed**, same rule as the shaders and the Lua.

## Measuring the seam from a screenshot, if it ever comes up again

Count the **fraction of rows** carrying a sign-consistent column-to-column
step. **Do not use the median over rows** — that needs the seam to span >50% of
the height and returned a false ABSENT on takes 198b/198c when a station
covered part of the view. Compare guard-to-seam **separation**, never absolute
x: the PNGs differ in height between sessions and content cross-correlation
gave three different offsets.

## The two patterns that dominated this stretch

**Every instrument recorded a different event than the question asked** —
eleven instances, ending with `read_shadow_terms` reading a member nothing
samples. Before trusting one, ask which event it records.

**A comment describing behaviour the code no longer has is active
misinformation**, worse than no comment, because it answers the question and
stops the search. Three of them cost take 196. When a comment or log line
answers your question, go find the code that produces the thing it describes.

**And a green case is worth nothing until seen to fail.** The shadow fixture
held the same 0.002 in the dead member that the live one carries, so reading
the wrong member gave the right answer and the case passed for as long as it
existed. It is poisoned to 0.009 now.

Everything below this line supersedes earlier "read this first" banners.

## Where the project is — AS OF TAKE 196, SUPERSEDED

**Kept for the reasoning, not for the status.** Two bullets below are now
false: the seam is **settled as geometry** (take 199/200, tagged
`stage18-seam-laws-confirmed`), and "the next feature" list has moved on. The
current state is the *first* "Where the project is" under the RESUME HERE
banner. If these two disagree, that one wins.

* **#33's head rotation is CONFIRMED.** Take 187: `headlook_verify --scan` gives
  a sharp isolated spike at **+10.00°** in both eyes (0.714/0.718 against ~0.02
  elsewhere). The guard band was predicted to **1 px** by two independent
  derivations. Channel on **set 31**, 6/6 layouts, `OK`.
* **NOT built:** the runtime head-pose source. The rotation is still a static
  `X4VR_POSE_YAW`. **This is the next feature**, and it is separable from the
  seam below.
* **#49 is FIXED and now DEFAULT.** `X4VR_SHEAR_LIGHTS` defaults ON since take
  195; `=0` restores the old classification, which is what takes before 195 were
  measured under. That check is dormant unless `X4VR_PIPE_INVENTORY=1`, so takes
  186–191 were *unexamined* on that axis, not clean.
* **Open defect: the x=791 seam.** Law `seam NDC = sx·tan(θ/2)`, verified to
  **1.1 px over six cases, two angles, three `sx` values**, and to **0.1 px on
  take 195's guard-to-seam separation** (432 measured, 432.1 predicted).
  Appears at **rp #31/#32, the deferred lighting output**. **Take 196 forced
  the corrected modules to bind there and the seam stayed** — so it is not the
  missing correction, and the cheap discriminators are spent.
* **Take 196 also PASSes** and is a candidate for known-good-runs, but note
  what it cost: `X4VR_SHEAR_NODEPTH=1` restores take 70's #57 defect, and the
  seams measurably strengthened (step 4→7, 4→6). It is a diagnostic
  configuration, not a shippable one.

## The immediate next task — as of take 202

**No run is pending, and none is owed.** #56 closed without one. Take 202's
`FAIL` was the instrument; the layer, the scorer and the render suite are
fixed, and the suites are green.

**The next feature is the runtime head-pose source for #33** — the rotation is
still a static `X4VR_POSE_YAW`. Everything under it is built and proven:
the conjugation (#61, 244/244 modules at 0.000e+00), the per-frame channel
(#59, set 31, 1125/1125 pipelines), and the classification (#48/#49).

Two instrument tasks are open and neither blocks that: **#67** (the camera slot
walk has the same never-cleared defect the object walk had, and re-baselining
it moves settled camera numbers) and a third `<fov>` point if #56's *law* ever
has to be known rather than its direction.

---

*Everything below this line is the take-197 note it replaced, kept for the
reasoning.*

**DONE — take 197 validated the classify fix, tagged
`stage17-classify-x4s-bytes`.** World-and-procedural 14 → 0; the patch paths
balance to the module (live-sx −28, mvp-sx −2, clip +30 = exactly the 30
predicted offline); rp #31/#32 now read `(NonWorld,MONO,procedural,clip)`;
`cam-pos=0` still holds so #49 was not passing because of the defect. **The
seam did not move** — edges at 360 and 792, separation 432 px against the law's
432.1, byte-identical to take 195 — which was the prediction, and which also
retires take 196's +4.9 px as `X4VR_SHEAR_NODEPTH`'s perturbation.

**Run `X4VR_POSE_YAW=0` with `X4VR_POSE_ROTATE=1`.** It has never been run —
every take from 187 to 198 used yaw 10 or 20 — and it is the cheapest
discriminator left, using only knobs that already exist.

**Why now:** take 198 killed the last transform-shaped hypothesis. What
survives is a seam that is a pure function of `sx` and `θ` and of nothing else
— not the light, not the scene, not the dispatch, not the reconstruction. Both
edges are widened-field geometry: `x=359` is the guard-band edge, `x=791` the
bisector `sx·tan(θ/2)`.

**So the next hypothesis class is extent, not transform** — a pass whose
viewport, scissor or buffer still describes the *unwidened* field, ending where
the original field ended.

**At θ=0** the law puts the seam at NDC 0 and the guard edge at −1. Both edges
should vanish. If one survives, it belongs to the widened FOV alone and the
rotation is not involved at all — which would move the whole investigation off
#33 and onto #24.

**Not settled, and the two candidates at rp #31/#32 are both spent.** The seam
is NOT the absent reconstruction correction — it was never absent, because the
no-shear twin carries it (see the RETRACTED paragraph in the take-196 section)
— and NOT the absent vertex shear there, because take 196 added it and the
seam stayed and strengthened.

**The third variant does not need building: it already exists.**
`g_variants.original` *is* unsheared-vertices-with-corrected-fragments, built
from `frag_only` at x4vr_layer.cpp:4784, since take 23, with the harness case
*"unsheared twin keeps the frag patch"* asserting it on a combined
vertex+fragment module. Task #63 was written on the retracted premise and is
closed unbuilt.

**Read the constructor, not the log line.** Take 196 was spent because three
sentences described behaviour the code had not had since take 23. The habit
that would have saved it: when a comment or log line answers your question,
go find the code that produces the thing it describes and confirm it there.

**What is still true about the seam:** law `sx·tan(θ/2)`, verified to 1.1 px
over six cases and to **0.1 px** on take 195's guard-to-seam separation; it
appears at rp #31/#32; and it survives both variant selections available for
those passes. It tracks the head rotation, not the eye offset — so the next
thing to eliminate is on the rotation side, not the stereo side.

**Do not spend a take on a guess here.** Both cheap discriminators are gone,
and the last one was spent on a premise that three comments had made look
solid.

**The previous next task is DONE** — `frag-invproj-check` takes a pose set,
both negative controls fire, and all 244 eligible modules compute `R⁻¹·M`
exactly. That is hypothesis four eliminated.

## Seven dead hypotheses — do not revive without new evidence

1. **prev-MVP motion vector** — 0 of 409 modules access `M_prevworldviewprojection`.
2. **camera-positioned classification** — take 195 reclassified those modules
   and the seam did not move a pixel.
3. **uncorrected reconstruction — DEAD, and my revival of it was the error.**
   The offline check was right: the correction reaches rp #31/#32, because the
   no-shear twin carries every fragment edit. I revived this on the strength of
   three comments that had been false since take 23 and cost take 196 doing it.
4. **shearing rp #31/#32** — take 196 did exactly that; the seam stayed and
   strengthened (step 4→7, 4→6), and take 70's #57 defect came back with it.
5. **`R⁻¹` arithmetic** — all 244 eligible modules compute `R⁻¹·M` to
   0.000e+00, with two negative controls confirming the check has range.
6. **the classify defect** — take 197 fixed it; 30 procedural quads stopped
   being dispatched as world geometry and the seam did not move a pixel.
7. **the god-ray march** — take 198: three sun directions, both edges at the
   same two columns every time. A god-ray artifact must move with the sun.

## A SECOND pattern, and it cost more than the first — take 197

**Three sentences described behaviour the code stopped having in take 23**: the
layer's `unsheared pipeline: using unpatched modules`, this file's
`needs_original()` bullet, and the harness header's *"binds the UNPATCHED
module"*. All said the no-shear twin is the pristine module. It is not — it
carries every fragment edit, and a harness case has asserted that since take 23.

I read all three, believed them, published a mechanism, and spent take 196 on
it. The constructor at x4vr_layer.cpp:4784 is four lines long and says the
opposite.

**A comment that describes behaviour the code no longer has is not stale
documentation — it is active misinformation, and it is worse than no comment,
because it answers the question and stops the search.** All three are corrected
as of take 197.

**The habit:** when a comment or a log line answers your question, go find the
code that produces the thing it describes and confirm it there. Especially when
it is the answer you were hoping for.

## The pattern that dominated this stretch — ELEVEN instances

**Every instrument here recorded a different event than the question asked.**
`NEVER REACHED` meant "not while watched"; the give-up sat inside the branch its
own failure mode skips; the test fixture was ordered so the bug could not occur;
the harness rendered one frame; the pipe inventory keyed on pipeline *creation*
rather than use; the bind description was keyed on `out[i]` before
`vkCreateGraphicsPipelines` wrote it; the module_map message keyed on parsed
modules rather than lines; the bind-time case counted passes and was blind to
content; `frag-invproj-check` could not see the term it exists to check; and
the check that retired the reconstruction hypothesis asked whether the patch
*applies* when the question was whether the patched module *runs*; and I
refuted the take-70 rule by matching `g_shear_nodepth` to the wrong env var
from memory instead of grepping the two lines that define it.

**The newest one is the cheapest to avoid: `grep` what a variable reads before
reasoning about the branch it gates.** `X4VR_SHEAR_LIGHTS` and
`X4VR_SHEAR_NODEPTH` are different knobs at different granularities — one
per-module, one per-pass — and assuming they were the same produced a
confident, published, wrong refutation.

**Before trusting any instrument here, ask which event it records.** And a
green case is worth nothing until it has been seen to fail — the negative
control caught four of these, the newest being the whole-sweep flip run that
proved no module had silently missed the `R⁻¹` term.

## Method note

Elimination has outperformed instrumentation. Take 195 killed a hypothesis in
one run by changing a knob and re-measuring. Five runs of capture and
attribution produced one name and no cause. Prefer "switch it off and
re-measure" over "build a probe to watch it".
