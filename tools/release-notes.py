#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Prints the section of docs/releases.md for one version, for the draft release CI makes.

    tools/release-notes.py v0.9.10
"""
import pathlib
import re
import sys

tag = sys.argv[1]
text = pathlib.Path(__file__).resolve().parent.parent.joinpath("docs/releases.md").read_text()
m = re.search(rf"^## {re.escape(tag)}\b.*?$(.*?)(?=^## |\Z)", text, re.M | re.S)
if not m:
    sys.exit(f"docs/releases.md has no section for {tag}: write it before tagging")
print(m.group(0).strip())
