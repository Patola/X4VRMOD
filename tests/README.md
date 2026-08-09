# tests

Small standalone Vulkan programs that pin down layer behaviour without
launching X4. A live run costs minutes and a load screen; these cost a
second, so anything that can be settled here should be.

They are built by the normal `cmake --build build` and link `libvulkan`
(unlike the layer, which only uses the headers).

| Test | Question it answers |
|---|---|
| `run-multiview-enable.sh` | Does the layer really enable the multiview feature on a device the application created without it? |
| `run-multiview-double.sh` | Does the layer turn a single-layer frame into a two-layer one, and is the result valid Vulkan? |
| `run-multiview-render.sh` | Does a real draw through the layer reach **both** array layers? Written to settle in one second what three live runs could not. |
| `run-cursor.sh` | Does the cursor land where X4 hit-tests, in both eyes, in the right colours — and does it follow task #30's canvas when there is one? |
| `x4vr_test_view_math` | Does the analytic eye shear agree with `P·T(−d)·P⁻¹` computed the long way, and does the canvas shift agree with the shear it claims to be? |
| `run-xr-probe.sh` | Does an `XrSession` come up on a Vulkan device the runtime created, and what does it then say about the eyes — physical device, added extensions, per-eye extent, per-view FOV, IPD, head pose? Its first half checks the eye test card with no runtime at all. |

## Conventions

* **Every suite includes at least one case that must fail.** A test whose
  passing case is the only case cannot distinguish "the feature works" from
  "the check is broken" — if validation is missing, or the driver enables
  something on its own, an all-clean suite still reports success.
* **Declare what X4 declares.** The multiview test names itself `X4` and asks
  for Vulkan 1.2 because both feed decisions the layer makes; a test running
  as a 1.0 app would take a different path and prove nothing about the game.
* **Prefer a consequence over a claim.** The multiview test does not inspect
  what the layer passed to `vkCreateDevice` — it creates a two-view render
  pass and lets the validation layer render the verdict.

Needs `VK_LAYER_KHRONOS_validation` installed.
