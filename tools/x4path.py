# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Where X4 is, on whoever's machine this runs: X4VR_GAME when set, else what
`x4vrmod locate` finds from this tree's build (X4VR_BUILD overrides the folder)."""
import os
import subprocess


def x4_binary():
    e = os.environ.get("X4VR_GAME")
    if e:
        return e
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cli = os.path.join(os.environ.get("X4VR_BUILD", os.path.join(root, "build")),
                       "launcher", "x4vrmod")
    try:
        out = subprocess.run([cli, "locate"], capture_output=True, text=True,
                             check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit("can't find X4: set X4VR_GAME, or build the launcher")
    return out
