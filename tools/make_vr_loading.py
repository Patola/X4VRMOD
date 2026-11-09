#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Draws the words of the headset's loading panel, one strip per language, under the art.

    tools/make_vr_loading.py            writes launcher/art/vr-loading-<code>.png and
                                        launcher/art/vr-loading.txt

The panel is what the headset shows from the moment the VR session is up until X4 draws
steadily: a first start can take a minute, and an empty headset for that long reads as a
hang (the RTX 5090 report, 2026-11-09). The layer embeds launcher/art/loading-screen.png
and the strip for the player's language, and puts the strip under the art.

The words are the launcher's own, translated with everything else: launcher/core/
x4vr_i18n.hpp marks them, and each language's comes from its launcher/po/<code>.po. Run
this after changing either; vr-loading.txt records what was drawn, and tests/run-i18n.sh
fails when it no longer matches the catalogues.

DejaVu Sans, because it covers every language here, Cyrillic included. Without it this
refuses rather than drawing boxes. The strips are committed, so building needs no font.
"""
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "launcher", "art")
PO = os.path.join(ROOT, "launcher", "po")
LANGS = ["en", "pt_BR", "fr", "ru", "de"]  # launcher/core/x4vr_i18n.hpp's kLanguages
LINES = ["X4 is loading", "The first start can take a minute or two."]
HEIGHT = 112


def unquote(s):
    """A .po string's escapes, and nothing else: the text itself is UTF-8 already."""
    esc = {"n": "\n", "t": "\t", '"': '"', "\\": "\\"}
    return re.sub(r"\\(.)", lambda m: esc.get(m.group(1), m.group(1)), s)


def translations(code):
    if code == "en":
        return {m: m for m in LINES}
    text = open(os.path.join(PO, code + ".po"), encoding="utf8").read()
    out = {}
    for entry in text.split("\n\n"):
        if "#, fuzzy" in entry:
            continue
        mid = re.search(r'^msgid ((?:"[^\n]*"\n?)+)', entry, re.M)
        mstr = re.search(r'^msgstr ((?:"[^\n]*"\n?)+)', entry, re.M)
        if not mid or not mstr:
            continue
        join = lambda block: "".join(unquote(q) for q in re.findall(r'"(.*)"', block))
        out[join(mid.group(1))] = join(mstr.group(1))
    missing = [m for m in LINES if not out.get(m)]
    if missing:
        sys.exit(f"make_vr_loading: {code}.po has no translation for {missing}")
    return out


def font(style, size):
    path = subprocess.run(["fc-match", "-f", "%{file}", f"DejaVu Sans:style={style}"],
                          capture_output=True, text=True).stdout
    if not os.path.basename(path).startswith("DejaVuSans"):
        sys.exit(f"make_vr_loading: DejaVu Sans {style} isn't installed (fc-match gave {path!r})")
    return ImageFont.truetype(path, size)


def fitted(draw, text, style, size, width):
    while size > 12:
        f = font(style, size)
        if draw.textlength(text, font=f) <= width:
            return f
        size -= 1
    return font(style, size)


def strip(width, title, sub):
    im = Image.new("L", (width, HEIGHT), 0)
    d = ImageDraw.Draw(im)
    for text, style, size, top in ((title, "Bold", 44, 10), (sub, "Book", 25, 68)):
        f = fitted(d, text, style, size, width - 32)
        d.text(((width - d.textlength(text, font=f)) / 2, top), text, font=f, fill=255)
    return im


def main():
    width = Image.open(os.path.join(ART, "loading-screen.png")).size[0]
    record = []
    for code in LANGS:
        t = translations(code)
        title, sub = t[LINES[0]], t[LINES[1]]
        strip(width, title, sub).save(os.path.join(ART, f"vr-loading-{code}.png"), optimize=True)
        record.append(f"{code}\t{title}\t{sub}")
        print(f"vr-loading-{code}.png: {title} / {sub}")
    with open(os.path.join(ART, "vr-loading.txt"), "w", encoding="utf8") as f:
        f.write("# What tools/make_vr_loading.py drew, per language: tests/run-i18n.sh compares it\n"
                "# with launcher/po. Not to be edited: run the tool.\n")
        f.write("\n".join(record) + "\n")


if __name__ == "__main__":
    main()
