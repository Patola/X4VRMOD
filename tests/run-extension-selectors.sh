#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Every `<replace sel=...>` in our extensions' libraries/*.xml must address
# exactly ONE attribute in X4's own library file. X4 applies a diff whose
# selector matches nothing without a word, so a typo would ship an extension
# that silently does nothing -- the run would read as "with the extension".
#
# X4's libraries are Egosoft's and live in /tmp/x4-ui only (extracted by
# tools/extract_glsl.py's walk); without them this SKIPS, loudly.
set -uo pipefail
CDPATH=
cd "$(dirname "$0")/.." || exit 1
LIB=/tmp/x4-ui/libraries
if [ ! -d "$LIB" ]; then
    echo "SKIP  $LIB absent — extract X4's libraries to check the selectors"
    exit 0
fi
python3 - "$LIB" <<'PY'
import glob, os, sys
import xml.etree.ElementTree as ET
lib = sys.argv[1]
fail = 0
for diff in sorted(glob.glob("extension/*/libraries/*.xml")):
    base_path = os.path.join(lib, os.path.basename(diff))
    if not os.path.exists(base_path):
        print(f"FAIL  {diff}: X4 has no {os.path.basename(diff)}"); fail = 1; continue
    base = ET.parse(base_path).getroot()
    for r in ET.parse(diff).getroot():
        sel = r.get("sel") or ""
        if "/@" not in sel:
            print(f"FAIL  {diff}: {sel!r} is not an attribute selector"); fail = 1; continue
        path, attr = sel.rsplit("/@", 1)
        parts = path.strip("/").split("/")
        nodes = [base] if parts and parts[0] == base.tag else []
        for p in parts[1:]:
            nodes = [c for n in nodes for c in n if c.tag == p]
        hits = [n for n in nodes if n.get(attr) is not None]
        name = diff.split("/")[1]
        if len(hits) == 1:
            print(f"ok    {name}: {sel} (X4: {hits[0].get(attr)} -> ours: {r.text})")
        else:
            print(f"FAIL  {name}: {sel} matches {len(hits)} attribute(s)"); fail = 1
print()
print("all cases passed" if not fail else "SOME CASES FAILED")
sys.exit(fail)
PY
