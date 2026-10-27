#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
#
# Every `<replace sel=...>` in our extensions' libraries/*.xml must address
# exactly ONE attribute in X4's own library file. X4 applies a diff whose
# selector matches nothing without a word, so a typo would ship an extension
# that silently does nothing -- the run would read as "with the extension".
#
# X4's libraries are Egosoft's: read from the game's own .cat/.dat (the last
# archive that carries a file wins, as in X4), into a temporary directory that
# is removed on exit -- never committed. It used to need a /tmp/x4-ui left by
# an earlier extraction, and SKIPped with exit 0 once /tmp was cleaned by age:
# a suite that passed by testing nothing. Now only a missing GAME skips.
set -uo pipefail
CDPATH=
cd "$(dirname "$0")/.." || exit 1
GAME=${X4VR_GAME_DIR:-"/nvme/SteamLibrary/steamapps/common/X4 Foundations"}
if [ ! -f "$GAME/01.cat" ]; then
    echo "SKIP  no X4 at $GAME — the selectors are unchecked"
    exit 0
fi
LIB=$(mktemp -d)
trap 'rm -rf "$LIB"' EXIT
python3 -I - "$GAME" "$LIB" extension/*/libraries/*.xml <<'PY' || { echo "FAIL  could not read X4's libraries from $GAME"; exit 1; }
import os, sys
game, out, diffs = sys.argv[1], sys.argv[2], sys.argv[3:]
want = {"libraries/" + os.path.basename(d) for d in diffs}
found = {}
for i in range(1, 10):
    cat = os.path.join(game, "%02d.cat" % i)
    if not os.path.exists(cat):
        continue
    off = 0
    with open(cat, encoding="utf-8", errors="replace") as c:
        for line in c:
            parts = line.rstrip("\n").rsplit(" ", 3)
            if len(parts) != 4:
                continue
            size = int(parts[1])
            if parts[0] in want:
                found[parts[0]] = (cat[:-3] + "dat", off, size)
            off += size
for path, (dat, off, size) in found.items():
    with open(dat, "rb") as d:
        d.seek(off)
        data = d.read(size)
    with open(os.path.join(out, os.path.basename(path)), "wb") as o:
        o.write(data)
sys.exit(0 if found else 1)
PY
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
