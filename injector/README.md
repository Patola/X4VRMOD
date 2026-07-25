# injector

`libx4vr_inject.so` — `LD_PRELOAD` component. See [../DESIGN.md](../DESIGN.md)
for its role in the project. This file records the behaviour of the
`config.xml` interception, which has more sharp edges than it looks.

## The mod runs off its own settings file

The first time X4 asks for `config.xml`, the injector copies it verbatim to
**`config-x4vrmod.xml`** in the same directory. From then on, in a modded
session:

- every **read** of `config.xml` is answered from the profile, with the
  overrides in [`x4vr_config.hpp`](x4vr_config.hpp) applied in memory;
- every **write** to `config.xml` is redirected into the profile.

So the player's `config.xml` is opened read-only, exactly once, and never
written. Settings changed in-game while modded persist — in the profile —
and vanilla launches are completely unaffected.

**Uninstalling:** delete `config-x4vrmod.xml`. Deleting it also re-forks from
the current `config.xml`, which is how to pull in settings changed in a
vanilla session (the profile does not track them; it is a fork, not a view).

The read path serves a `memfd_create()` stream, **not** `fmemopen()`. An
`fmemopen()` stream has no real file descriptor, so `fileno()`/`fstat()` in
X4's XML reader fail and X4 silently falls back to its defaults — which
presents as the game launching at desktop resolution for no visible reason.
A memfd has a real fd and is still purely in memory.

Overrides are re-applied on **every** read, not baked into the profile at
fork time. If the player sets one of our managed tags in the options menu,
X4 writes their choice to the profile and we override it again next launch —
we neither lose their value nor obey it.

Why a profile at all: the earlier design let X4 write `config.xml` and then
put the player's values back afterwards. That needed an `atexit` hook with
careful `fflush` ordering, and a heuristic to distinguish *"X4 saved our
injected value"* from *"the player changed this in the menu"* — and live runs
showed X4 rewrites the file repeatedly **during** a session, not just at
exit, so the heuristic ran constantly. The profile makes all of it moot.

### Knock-on effects, and why the profile settles them

**Observed live:** forcing `fullscreen=false` caused X4 to change
`presentmode` from `immediate` to `mailbox` **on its own**. We never override
`presentmode`, so a restore-based scheme correctly left it alone — and the
value persisted into the player's config anyway.

The lesson generalises: *"we only touch our own tags"* is not the same as
*"the player's config is unaffected"*. Our overrides change the conditions X4
evaluates its other settings under, and X4 may legitimately rewrite those.
Any scheme that lets X4 write the real file has to enumerate derivations it
cannot know. Redirecting the write is the only fix that does not need to.

### Fallbacks

Every step degrades to the pre-profile behaviour rather than to data loss:
if the profile cannot be read *or* created, reads pass through to the real
file untouched and writes go where they always would have. If `config.xml`
itself is missing (fresh install), X4 creates its own default file — running
unmodded for that one launch — and the next read forks from it.

`X4VR_NO_CONFIG=1` disables the whole path: no profile, no overrides, no
redirect.

## Config value gotchas

X4's enum strings do not always match the option label shown in the menu.

| Tag | Menu label | Stored value |
|---|---|---|
| `antialiasing` | Off | `none` |
| `pom` | Off | `none` (**not** `off`) |
| `pom` | Low / Medium / High | `low` / `medium` / `high` |

`res_width` / `res_height` are the sharpest of these, because they are
silently *conditional*:

| `borderless` | What X4 does with `res_width`/`res_height` |
|---|---|
| `false` | Honours them, minus the window decoration (1408 → **1385**) |
| `true` | **Ignores them** and sizes to the display |

Measured with an identical served config: 2816×1408 under a 2816×1408
gamescope, 3440×1440 on a 3440×1440 desktop. Neither run logged anything
unusual — the override was applied to the buffer both times — so this is
invisible unless you check the swapchain. The layer now warns when it is not
the expected size. See [`../common/x4vr_sbs.hpp`](../common/x4vr_sbs.hpp).

An **invalid** value does not error and is not rejected on load: X4 writes it
back to the file happily, and the only symptom is that the options menu shows
`--` for that setting, meaning it matched no known option. So a wrong guess
silently does nothing while looking like it worked. Verify a new value by
setting it in-game once and reading back the file.

## Files and environment

- `config-x4vrmod.xml` — the mod's settings, beside X4's `config.xml` in
  `~/.config/EgoSoft/X4/<id>/`. Delete to reset or uninstall.
- `X4VR_NO_CONFIG=1` — disable config interception entirely.
- `X4VR_NO_INJECT=1` (launcher) — do not preload the injector at all.
