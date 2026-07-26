# tests

Small standalone Vulkan programs that pin down layer behaviour without
launching X4. A live run costs minutes and a load screen; these cost a
second, so anything that can be settled here should be.

They are built by the normal `cmake --build build` and link `libvulkan`
(unlike the layer, which only uses the headers).

| Test | Question it answers |
|---|---|
| `run-multiview-enable.sh` | Does the layer really enable the multiview feature on a device the application created without it? |

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
