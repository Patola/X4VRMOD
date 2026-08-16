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
