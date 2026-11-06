#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Builds the x4vrmod extension as it ships: extension/x4vrmod, plus an ext_01 catalog
holding X4's loading screen, redrawn with the mod's art and its version, build and date.

    tools/make_extension.py --src extension/x4vrmod --art launcher/art/loading-screen.png \\
        --version v0.9.9-12-g1f80757 --date 2026-11-06 --out build/extension/x4vrmod

X4 stacks its extensions in load order and a later one's file wins, as Timelines' art
beats the base game's. content.xml's optional dependencies put x4vrmod after the DLCs that
ship these files. The extension is on only in VR sessions, so a flat game keeps X4's own.
Sizes are X4's, one per screen shape. Without Pillow the pictures are left out and the rest
is copied: nothing else in the mod needs them.
"""
import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import time

CATALOG_DIR = "assets/textures/ui/loadingscreens"
SIZES = {"x4_1_1": (1920, 1920), "x4_5_4": (1920, 1536), "x4_4_3": (1920, 1440),
         "x4_3_2": (1920, 1280), "x4_16_10": (1920, 1200), "x4_16_9": (1920, 1080),
         "x4_17_9": (2040, 1080), "x4_21_9": (2560, 1080), "x4_32_9": (3840, 1080)}
# A crop that loses less than this is better than bars; more would cut the pilot or the name.
MAX_CROP = 0.12


def stamp_words(version, date):
    """`v0.9.9-12-g1f80757-dirty` -> the release, then the build, then the date."""
    release, _, build = version.partition("-")
    words = [f"X4 VR Mod {release}"]
    if build:
        words.append(f"build {build}")
    words.append(date)
    return "   ".join(words)


def font(image_font, size):
    try:
        path = subprocess.run(["fc-match", "-f", "%{file}", "DejaVu Sans:style=Bold"],
                              capture_output=True, text=True, check=True).stdout
        return image_font.truetype(path, size)
    except (OSError, subprocess.CalledProcessError):
        return image_font.load_default(size)


def picture(modules, art, size, text):
    Image, ImageDraw, ImageFilter, ImageFont = modules
    w, h = size
    aw, ah = art.size
    cover = max(w / aw, h / ah)
    lost = 1.0 - min(w / (aw * cover), h / (ah * cover))
    canvas = art.resize((round(aw * cover), round(ah * cover)), Image.LANCZOS)
    left, top = (canvas.width - w) // 2, (canvas.height - h) // 2
    canvas = canvas.crop((left, top, left + w, top + h))
    margin = min(w, h) // 40
    fnt = font(ImageFont, max(14, min(w, h) // 45))
    box = ImageDraw.Draw(canvas).textbbox((0, 0), text, font=fnt)
    tw, th = box[2] - box[0], box[3] - box[1]
    x, y = w - margin - tw, h - margin - th
    if lost > MAX_CROP:
        # Too different a shape to crop: all of it, over a blurred and darkened copy.
        canvas = canvas.filter(ImageFilter.GaussianBlur(h // 28))
        canvas = Image.eval(canvas, lambda v: v * 45 // 100)
        contain = min(w / aw, h / ah)
        sharp = art.resize((round(aw * contain), round(ah * contain)), Image.LANCZOS)
        top = (h - sharp.height) // 2
        canvas.paste(sharp, ((w - sharp.width) // 2, top))
        # Near the middle, under the picture or inside it: a headset's lens hides the corners.
        x = (w - tw) // 2
        y = top + sharp.height + margin if h - top - sharp.height > th + 2 * margin \
            else top + sharp.height - margin - th
    draw = ImageDraw.Draw(canvas)
    draw.text((x, y), text, font=fnt, fill=(235, 240, 245), stroke_width=max(2, fnt.size // 10),
              stroke_fill=(0, 0, 0))
    out = io.BytesIO()
    canvas.save(out, "JPEG", quality=90, optimize=True)
    return out.getvalue()


def write_catalog(out, files):
    now = int(time.time())
    with open(os.path.join(out, "ext_01.dat"), "wb") as dat, \
            open(os.path.join(out, "ext_01.cat"), "w", encoding="utf-8") as cat:
        for name, data in files:
            dat.write(data)
            cat.write(f"{name} {len(data)} {now} {hashlib.md5(data).hexdigest()}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--art", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    # Only when something changed: a catalog's times would differ, and the extension's a copy
    # players compare against.
    stamp_path = a.out + ".stamp"
    def newest(root):
        return max(os.path.getmtime(os.path.join(d, f))
                   for d, _, fs in os.walk(root) for f in fs)
    stamp = f"{a.version} {a.date} {newest(a.src)} {os.path.getmtime(a.art)} " \
            f"{os.path.getmtime(__file__)}"
    if os.path.isdir(a.out) and os.path.exists(stamp_path) and open(stamp_path).read() == stamp:
        return 0

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    shutil.copytree(a.src, a.out)
    try:
        if os.environ.get("X4VR_NO_PILLOW"):
            raise ImportError("asked not to")
        from PIL import Image, ImageDraw, ImageFilter, ImageFont
    except ImportError:
        print("make_extension: no Pillow, so X4's own loading screen stays", file=sys.stderr)
    else:
        modules = (Image, ImageDraw, ImageFilter, ImageFont)
        art = Image.open(a.art).convert("RGB")
        text = stamp_words(a.version, a.date)
        write_catalog(a.out, [(f"{CATALOG_DIR}/{name}.jpg", picture(modules, art, size, text))
                              for name, size in SIZES.items()])
    with open(stamp_path, "w") as f:
        f.write(stamp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
