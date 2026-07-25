# launch

`x4vr-launch.sh` — starts X4 with the layer and injector wired up. See
[../DESIGN.md](../DESIGN.md) for this component's role, and the script's own
header comment for every environment switch.

## gamescope is required, not optional

X4 honours `res_width`/`res_height` only when `borderless` is off, and then
loses the window decoration from the height (1408 → 1385). With `borderless`
on it ignores them and sizes to the display. Running under gamescope at
exactly the SBS size is what makes "the display" and "what we asked for" the
same thing — see [../common/x4vr_sbs.hpp](../common/x4vr_sbs.hpp). The layer
warns if the swapchain comes up any other size.

`X4VR_W`/`X4VR_H` default to the size in that header, so there is one place
to change it.

## Known harmless: gamescope aborts at shutdown

Every gamescope session ends with a core dump:

```
[gamescope] [Info]  launch: Primary child shut down!
terminate called without an active exception
(EE) failed to read Wayland events: Broken pipe
zsh: abort (core dumped)
```

**Ignore it.** "without an active exception" is not a thrown exception — it is
`std::terminate` from a `std::thread` destroyed while still joinable, in
gamescope's own teardown. It fires *after* `Primary child shut down!`, i.e.
after X4 has already exited, so nothing is lost.

Confirmed not ours: it reproduces identically with
`X4VR_NO_LAYER=1 X4VR_NO_INJECT=1`, and the aborting thread is
`gamescope-xwm` with no Vulkan frames in its stack. Observed on gamescope
3.16.24 with `--backend sdl`.

The trap this sets: a `coredumpctl` listing during a debugging session will
always show a fresh gamescope core, which looks like evidence and is not.
Check the **executable and the signal** first — X4's own cores are ~430 MB
SIGSEGV, gamescope's teardown abort is ~1.8 MB SIGABRT.
