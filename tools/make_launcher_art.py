#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""The launcher's artwork: the banner and the icon, cut from Patola's picture, and the
picture of where Steam's launch options go, drawn here. Run by hand when they change;
the PNGs are committed, so a build needs none of this.

    tools/make_launcher_art.py SOURCE.png [OUTDIR]     OUTDIR defaults to launcher/help

The window shows the banner at its own width, so any 3.2:1 picture can replace it.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

# In SOURCE: everything that matters (the headset, the controller, the name, the ship
# and the hangar), without the white strip on the left edge.
BANNER_BOX = (12, 40, 894, 316)
ICON_BOX = (70, 76, 256, 262)


def font(style, size):
    path = subprocess.run(["fc-match", "-f", "%{file}", f"DejaVu Sans:style={style}"],
                          capture_output=True, text=True, check=True).stdout
    return ImageFont.truetype(path, size)


def banner(src, out):
    src.crop(BANNER_BOX).save(os.path.join(out, "banner.png"), optimize=True)


def icon(src, out):
    S = 256
    pic = src.crop(ICON_BOX).resize((S * 2, S * 2), Image.LANCZOS)
    mask = Image.new("L", pic.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S * 2 - 1, S * 2 - 1), 96, fill=255)
    img = Image.new("RGBA", pic.size, (0, 0, 0, 0))
    img.paste(pic, (0, 0), mask)
    img.resize((S, S), Image.LANCZOS).save(os.path.join(out, "icon.png"), optimize=True)


def launch_options(out):
    """Where the line goes, as a sketch of the steps, not a picture of Steam."""
    W, H = 1280, 620
    bg, panel, field, line = (34, 40, 50), (45, 52, 64), (22, 26, 33), (88, 100, 118)
    text, dim, accent = (225, 232, 240), (150, 162, 178), (90, 190, 255)
    img = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(img)
    f, fb, fs = font("Book", 30), font("Bold", 32), font("Book", 26)
    mono = ImageFont.truetype(subprocess.run(["fc-match", "-f", "%{file}", "DejaVu Sans Mono"],
                                             capture_output=True, text=True).stdout, 28)
    d.text((40, 30), "In Steam's library, right-click X4: Foundations, then Properties.",
           font=f, fill=text)
    d.rounded_rectangle((40, 96, W - 40, H - 40), 14, fill=panel, outline=line, width=2)
    d.text((70, 118), "X4: Foundations  -  Properties", font=fb, fill=text)
    d.line((42, 176, W - 42, 176), fill=line, width=2)
    d.rectangle((42, 178, 300, H - 42), fill=(38, 44, 55))
    d.rounded_rectangle((54, 192, 288, 244), 6, fill=(60, 90, 120))
    d.text((74, 200), "General", font=f, fill=text)
    for i, name in enumerate(("Updates", "Installed Files")):
        d.text((74, 268 + i * 56), name, font=f, fill=dim)
    d.text((340, 204), "Launch options", font=fb, fill=text)
    d.text((340, 250), "Paste the line from the launcher here:", font=fs, fill=dim)
    d.rounded_rectangle((340, 300, W - 80, 368), 8, fill=field, outline=accent, width=3)
    d.text((362, 318), "x4vrmod run %command%", font=mono, fill=text)
    d.text((340, 396), "The launcher's Launch options window has the exact line for", font=fs,
           fill=dim)
    d.text((340, 432), "your copy of the mod, with a Copy button.", font=fs, fill=dim)
    d.text((340, 490), "Then press Play in Steam as usual.", font=f, fill=text)
    img.save(os.path.join(out, "steam-launch-options.png"), optimize=True)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "launcher", "help")
    os.makedirs(out, exist_ok=True)
    src = Image.open(sys.argv[1]).convert("RGB")
    banner(src, out)
    icon(src, out)
    launch_options(out)


if __name__ == "__main__":
    main()
