#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""
extract_shaders.py — pull X4's SPIR-V out of a renderdoc capture and report
the uniform-block layouts.

X4 ships its shaders with debug info (OpName/OpMemberName/Offset), so the
engine's constant-buffer layouts can be read exactly instead of guessed.

Usage:
  renderdoccmd convert -f cap.rdc -o cap.zip.xml -c zip.xml   # makes cap.zip.xml + cap.zip
  ./tools/extract_shaders.py cap.zip.xml cap.zip [outdir]
"""
import json, os, re, subprocess, sys, xml.etree.ElementTree as ET, zipfile

def index_modules(xml):
    out = []
    for _, el in ET.iterparse(xml, events=("end",)):
        if el.tag != "chunk":
            continue
        if el.get("name") == "vkCreateShaderModule":
            ci = next((c for c in el if c.get("name") == "CreateInfo"), None)
            blob = sid = None
            if ci is not None:
                for c in ci:
                    if c.tag == "buffer" and c.get("name") == "pCode":
                        blob = c.text.strip()
            for c in el:
                if c.tag == "ResourceId" and c.get("name") == "ShaderModule":
                    sid = c.text.strip()
            if blob and sid:
                out.append((sid, blob))
        el.clear()
    return out

def blocks_of(dis):
    names, mnames, moffs = {}, {}, {}
    for m in re.finditer(r'OpName %(\w+) "([^"]*)"', dis):
        names[m.group(1)] = m.group(2)
    for m in re.finditer(r'OpMemberName %(\w+) (\d+) "([^"]*)"', dis):
        mnames[(m.group(1), int(m.group(2)))] = m.group(3)
    for m in re.finditer(r'OpMemberDecorate %(\w+) (\d+) Offset (\d+)', dis):
        moffs[(m.group(1), int(m.group(2)))] = int(m.group(3))
    sets = {m.group(1): int(m.group(2))
            for m in re.finditer(r'OpDecorate %(\w+) DescriptorSet (\d+)', dis)}
    binds = {m.group(1): int(m.group(2))
             for m in re.finditer(r'OpDecorate %(\w+) Binding (\d+)', dis)}
    vartype = {m.group(1): m.group(2)
               for m in re.finditer(r'%(\w+) = OpVariable %(\w+) Uniform\b', dis)}
    ptr = {m.group(1): m.group(2)
           for m in re.finditer(r'%(\w+) = OpTypePointer Uniform %(\w+)', dis)}
    res = {}
    for var, pt in vartype.items():
        st = ptr.get(pt)
        if not st:
            continue
        members = [(i, mnames[(st, i)], moffs.get((st, i)))
                   for i in range(128) if (st, i) in mnames]
        if members:
            res[names.get(st, st)] = {"set": sets.get(var), "binding": binds.get(var),
                                      "members": members}
    return res

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    xml, zpath = sys.argv[1], sys.argv[2]
    outdir = sys.argv[3] if len(sys.argv) > 3 else "/tmp/x4spv"
    os.makedirs(outdir, exist_ok=True)
    z = zipfile.ZipFile(zpath)
    allblocks = {}
    n = 0
    for sid, blob in index_modules(xml):
        try:
            data = z.read("%06d" % int(blob))
        except KeyError:
            continue
        if data[:4] not in (b'\x03\x02\x23\x07', b'\x07\x23\x02\x03'):
            continue
        p = os.path.join(outdir, f"{sid}.spv")
        open(p, "wb").write(data)
        n += 1
        dis = subprocess.run(["spirv-dis", "--no-color", p],
                             capture_output=True, text=True).stdout
        for name, info in blocks_of(dis).items():
            allblocks.setdefault(name, info)
    print(f"extracted {n} SPIR-V modules to {outdir}\n")
    for name, info in sorted(allblocks.items(), key=lambda kv: kv[1]["set"] or 99):
        print(f"== set {info['set']} binding {info['binding']}  {name}")
        for i, nm, off in info["members"]:
            print(f"   [{i:2}] off {str(off):>5}  {nm}")
        print()
    json.dump(allblocks, open(os.path.join(outdir, "blocks.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
