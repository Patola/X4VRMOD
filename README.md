# X4VRMOD — VR for X4: Foundations on Linux (proof of concept)

Stereoscopic 3D + head tracking for the **native Linux** build of
**X4: Foundations**, with no Proton and no official VR support in the Linux
binary. Built and tested on Arch Linux / KDE Wayland, a Meta **Quest 3** via
**WiVRn**, and an AMD RX 7900 XT.

> ⚠️ **This is a proof of concept, not a polished mod.** It works and is fun
> to look at, but it is fiddly to set up, has real visual/comfort
> limitations, and almost every setting needs per-person tuning. Read the
> [Shortcomings](#shortcomings) before getting your hopes up. Developed with
> Claude Code (Claude Opus 4.8).

## How it works

The Linux X4 binary has **no OpenVR/OpenXR/SteamVR** backend, but it *does*
have first-class head tracking (an OpenTrack UDP listener on port 4242, added
in X4 7.50). So instead of true in-engine VR we assemble one from parts:

```
            depth-image-based stereo            POSIX shm
  X4  ───►  vkShade + SuperDepth3D  ──►  SBS  ──────────►  x4vr-viewer (OpenXR)  ──►  headset
   ▲          (Vulkan layer, in-process)                     │  splits SBS L/R, head-locked
   │                                                         │  cylinder/quad per eye
   └──────────────  OpenTrack UDP :4242  ◄───────────────────┘  HMD pose → 6DOF cockpit look
```

1. **vkShade + SuperDepth3D** (a Vulkan post-processing layer) reads X4's
   color + depth buffers and synthesizes a side-by-side stereo pair —
   Depth-Image-Based Rendering. No sync requirements; full frame rate.
2. The composited SBS frame is exported to **shared memory**.
3. **x4vr-viewer** (a small OpenXR app) reads the shm, shows each half to the
   correct eye on a head-locked cylinder, and sends the HMD pose back to X4's
   OpenTrack listener for 6DOF cockpit look-around.

Pure OpenXR, so the viewer runs on **WiVRn/Monado and SteamVR**.

## Requirements

- **X4: Foundations**, native Linux build (developed against 9.00), Vulkan.
- A **Wayland** session (vkShade needs a native Wayland surface; X4 must be
  launched with `SDL_VIDEO_DRIVER=wayland,x11 … -prefer-wayland`).
- A headset with an OpenXR runtime: **WiVRn/Monado** (tested) or SteamVR.
- A reasonably **high-resolution monitor** — see [Shortcomings](#shortcomings);
  per-eye sharpness is bounded by the desktop window size.
- Recent AMD/NVIDIA Vulkan driver.
- Build tools: `meson` `ninja` `gcc`/`g++` `cmake`, plus Vulkan and OpenXR
  loader + headers, and (for vkShade) glslang, SPIR-V headers, Wayland +
  X11 dev files, libxkbcommon. See the vkShade fork's README for its list.

## Reproduce it

### 1. Build & install the vkShade fork (the DIBR layer)

Our fork carries five upstreamed bug fixes **plus** the X4VR shm frame
export (branch `x4vr-export`):

```sh
git clone -b x4vr-export https://github.com/Patola/vkShadecc
cd vkShadecc
meson setup --buildtype=release --prefix="$HOME/.local" build-release
ninja -C build-release install      # installs to ~/.local (lib, bin, vulkan layer)
```

(The general fixes alone are proposed upstream as
[slobodaapl/vkShade#1](https://github.com/slobodaapl/vkShade/pull/1).)

### 2. Install the ReShade shaders SuperDepth3D needs

```sh
mkdir -p ~/.config/vkShade/reshade/Shaders ~/.config/vkShade/reshade/Textures
git clone --depth 1 https://github.com/crosire/reshade-shaders /tmp/rs
git clone --depth 1 https://github.com/BlueSkyDefender/Depth3D /tmp/d3d
cp /tmp/rs/Shaders/*.fxh ~/.config/vkShade/reshade/Shaders/
cp /tmp/d3d/Shaders/*.fx /tmp/d3d/Shaders/*.fxh ~/.config/vkShade/reshade/Shaders/
cp -r /tmp/rs/Textures/* ~/.config/vkShade/reshade/Textures/ 2>/dev/null || true
```

### 3. Build the viewer (and, optionally, the tools)

```sh
cd viewer && cmake -S . -B build && cmake --build build      # -> build/x4vr-viewer
cc -O2 -o tools/shmdump tools/shmdump.c -I shared            # optional: verify export
cd bridge && make                                            # optional: standalone pose bridge
```

### 4. Edit X4's `config.xml` (game closed)

`~/.config/EgoSoft/X4/<steam_id>/config.xml`:

```xml
<enableopentrack>true</enableopentrack>
<opentrackfilterstrength>5</opentrackfilterstrength>   <!-- light smoothing -->
<opentrackanglefactor>1.0</opentrackanglefactor>       <!-- 1:1 head->camera -->
<opentrackpositionfactor>1.0</opentrackpositionfactor>
<fov>1.3333</fov>                                       <!-- 120° = max; matches CYL_ANGLE -->
<res_width>1656</res_width>                             <!-- near-square fills more FOV -->
<res_height>1440</res_height>                           <!-- (must fit on your monitor) -->
```

Also enable **Controls → OpenTrack Support** in-game.

### 5. Launch X4 (Steam launch options)

```
ENABLE_VKSHADE=1 X4VR_EXPORT=1 SDL_VIDEO_DRIVER=wayland,x11 mangohud gamemoderun %command% -nocputhrottle -nosoundthrottle -skipintro -prefer-wayland
```

In the vkShade overlay (`Home` key), add the **SuperDepth3D** effect and set
the depth preset (see [docs/dibr-vkshade.md](docs/dibr-vkshade.md)):
Near Plane Adjustment ≈ 14, Zero Parallax Distance ≈ 0.028, DM0. (Ctrl+Click
a slider to type an exact value.) Verify the export with `./tools/shmdump
frame.ppm` if you like.

### 6. Run the viewer (with the headset connected to WiVRn)

```sh
XR_RUNTIME_JSON=~/.config/openxr/1/active_runtime.json \
  X4VR_TRACK_SROLL=0 \
  ./viewer/build/x4vr-viewer
```

Recenter the view any time with `kill -USR1 <pid>` (the viewer prints its
PID). Do **not** run `bridge/xr2x4` at the same time — two senders to :4242
fight.

## The "knobs" (expect several rounds of tuning)

**None of these are universal** — they depend on your headset, face fit, IPD,
monitor, GPU and taste. The values above/below are one person's starting
point, not gospel.

| Where | Knob | Notes |
|---|---|---|
| X4 config.xml | `opentrackanglefactor` | head→camera gain; keep ~1.0 (higher → gimbal lock) |
| X4 config.xml | `opentrackfilterstrength` | smoothing; UI floor 5, can set 0 in XML |
| X4 config.xml | `fov` | 1.3333 = 120°; match the cylinder arc |
| X4 config.xml | `res_width/height` | render res; sharpness vs FPS vs FOV fill |
| SuperDepth3D | Near Plane Adjustment | **dominant depth lever** (~14) |
| SuperDepth3D | Zero Parallax Distance | convergence (~0.028) |
| viewer | `X4VR_CYL_ANGLE` | wrap angle; set = in-game FOV (120) |
| viewer | `X4VR_VOFFSET` | vertical recenter for headset FOV asymmetry (e.g. -0.25) |
| viewer | `X4VR_TRACK_SROLL` | 0 = drop roll (comfort) |
| viewer | `X4VR_LAYER` / `X4VR_CYL_ASPECT` | cylinder vs quad; aspect auto-derives from the window |

Full details: [viewer/README.md](viewer/README.md) and
[docs/dibr-vkshade.md](docs/dibr-vkshade.md).

## Shortcomings

This is why it's a proof of concept, not a daily driver:

- **Definition / monitor dependence.** The pipeline captures X4's *desktop
  window*, so per-eye resolution ≈ (window_width ÷ 2 × window_height) and is
  bounded by your monitor. The Quest 3's 2064×2208-per-eye panels show
  visible pixelation from an upscaled ~828–1280-wide source; it is far worse
  on a 1080p monitor. Rendering a window **bigger than the screen** is
  sharper but then **the mouse can't reach the off-screen part** (X4 is
  mouse-driven) — only usable with a HOTAS/controller.
- **Head-look feels "off."** The screen is **head-locked** (it moves with
  your head), so the world is not fixed in space like real VR; rotation
  feels like less than your head movement and the pivot point is wrong.
  OpenTrack's Euler angles also gimbal-lock (roll spikes when you look far
  down). Tracking is **cockpit/on-foot only** — menus and the map don't
  track.
- **DIBR artifacts.** Stereo is *reconstructed* from one eye + depth:
  disocclusion edges are inpainted, and alpha-blended content (holographic
  cockpit MFDs, HUD brackets, engine trails, nebulae) gets the wrong depth.
- **Wayland only**, which disables Steam Input and the Steam overlay; there
  is an FPS cost from the effect passes + the frame export; and setup is a
  manual two-step launch (no launcher yet).
- **Per-setup tuning.** As above — plan to spend time dialing it in, and
  expect community values to differ from these.

## The real fix (future): in-engine injection

Most shortcomings share one cause: we work *outside* the engine. An
in-process component (an `LD_PRELOAD`/SDL shim, or a `.so` driving X4's
LuaJIT FFI à la Beko's
[X4-rest-server](https://github.com/bekopharm/X4-rest-server)) could render
two real eye views **at the headset's native resolution and FOV, world-locked
with the correct pivot** — fixing definition, the small-monitor problem,
true stereo, FOV, and gimbal-free head-look in one go. It's a large,
exploratory effort; see [CLAUDE.md](CLAUDE.md). For now, this PoC shows the
external approach is viable and surprisingly fun.

## Repository layout

- `viewer/` — **x4vr-viewer**, the OpenXR app (stereo delivery + head tracking).
- `bridge/` — `xr2x4`, a standalone HMD-pose→OpenTrack bridge (superseded by
  the viewer; kept for reference / mono head-look).
- `shared/x4vr_shm.h` — the shm frame-transport protocol.
- `tools/shmdump.c` — dumps the exported SBS frame to a PPM (verification).
- `docs/dibr-vkshade.md` — DIBR setup, tuning, and the SuperDepth3D preset.
- `probe/` — historical Alternate-Eye-Rendering feasibility experiment
  (Route 1, abandoned; see `probe/RESULTS.md`).
- `patches/0001-vkshade-fixes.patch` — our vkShade changes (the fork above is
  the canonical source).

## Credits

- **SuperDepth3D** by BlueSkyDefender (Depth3D); base shaders by crosire
  (reshade-shaders).
- **vkShade** by slobodaapl; our fork adds fixes + the shm export.
- Head-tracking inspiration: Beko (beko.famkos.net).
- Code developed with **Claude Code (Claude Opus 4.8)**.
