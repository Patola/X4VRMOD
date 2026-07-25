# injector

`libx4vr_inject.so` — `LD_PRELOAD` component. See [../DESIGN.md](../DESIGN.md)
for its role in the project. This file records the behaviour of the
`config.xml` interception, which has more sharp edges than it looks.

## How the config interception works

X4 reads `config.xml` with `fopen()`. We interpose it, read the real file,
rewrite the tags we need **in a memory buffer**, and hand X4 a stream over
that buffer. Anything that fails falls back to the untouched file.

The stream is backed by `memfd_create()`, **not** `fmemopen()`. An
`fmemopen()` stream has no real file descriptor, so `fileno()`/`fstat()` in
X4's XML reader fail and X4 silently falls back to its defaults — which
presents as the game launching at desktop resolution for no visible reason.
A memfd has a real fd and is still purely in memory.

## We do not write config.xml — but X4 does

This is the part that is easy to get wrong. X4 **saves its settings** with the
values it is currently running, which are ours. Left alone, a single modded
session permanently rewrites the player's resolution and effect settings.

So the injector snapshots the pre-run value of every tag it overrides and puts
those back after X4 has written the file, keeping everything else X4 saved.

Two details that only live testing revealed:

- **X4 rewrites `config.xml` repeatedly during a session**, not just at exit.
- **A tag is restored only if X4 wrote back exactly the value we injected.**
  If X4 wrote anything else, the player changed that setting in the options
  menu this session and meant it, so it is left alone. Without this rule the
  restore silently undoes the player's own changes.

Writes always pass straight through to the real file and are only fixed up
afterwards, so a failure in this path can fail to restore but can never lose
settings. The `atexit` fallback calls `fflush(NULL)` first, because `exit()`
runs handlers *before* flushing stdio — restoring first would be undone by
the flush a moment later.

## Knock-on effects: overrides can change settings we do not manage

**Observed live:** forcing `fullscreen=false` caused X4 to change
`presentmode` from `immediate` to `mailbox` on its own. We do not override
`presentmode`, so the restore correctly left it — but the value persisted
after the session.

The implication is that "we only touch our own tags" is **not** the same as
"the player's config is unaffected". Our overrides change the conditions X4
evaluates its other settings under, and X4 may legitimately rewrite those.

This is currently accepted rather than fixed, because the two available fixes
both have real costs:

- Snapshot the *whole* file and restore any tag X4 changed that we did not
  ask it to. Bigger hammer, and it would also revert genuine player changes
  unless we can tell them apart — which is exactly the problem the
  "did X4 write back our injected value" rule solves for our own tags, and
  which does not generalise to tags we never touched.
- Grow the override list to cover every setting X4 might derive from ours.
  Requires knowing the derivations, which we do not.

If a knock-on effect is ever found that actually harms the player (a lost
keybind, say, rather than a present-mode difference), revisit with evidence
rather than pre-emptively.

## Config value gotchas

X4's enum strings do not always match the option label shown in the menu.

| Tag | Menu label | Stored value |
|---|---|---|
| `antialiasing` | Off | `none` |
| `pom` | Off | `none` (**not** `off`) |
| `pom` | Low / Medium / High | `low` / `medium` / `high` |

An **invalid** value does not error and is not rejected on load: X4 writes it
back to the file happily, and the only symptom is that the options menu shows
`--` for that setting, meaning it matched no known option. So a wrong guess
silently does nothing while looking like it worked. Verify a new value by
setting it in-game once and reading back the file.

## Environment

- `X4VR_NO_CONFIG=1` — disable config interception entirely.
- `X4VR_NO_INJECT=1` (launcher) — do not preload the injector at all.
