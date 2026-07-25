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
