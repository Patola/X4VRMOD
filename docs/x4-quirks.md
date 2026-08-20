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

### Piece 3 is NOT next

The 1092×1180 render extent (0.65× area) stays parked until the two items above
land. Take 165a is already geometrically correct and merely wastes 22% of every
row, so there is no correctness pressure — only the performance prize.
