#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Who references an address in X4? Code (RIP-relative lea/mov/movss...) and
data (8-byte pointers in .data.rel.ro/.data).

    tools/xref.py <addr|"string"> [...]

Also direct calls/jumps (e8/e9 rel32) to it. A quoted argument is looked up
as a NUL-terminated string in .rodata first. Reads X4's own bytes; nothing of
X4's is written anywhere. Each byte-pattern hit is attributed to the
instruction covering it by majority vote over 32 linear decodes, and printed
only if that instruction really addresses the target.
"""
import re
import sys

import capstone
from elftools.elf.elffile import ELFFile

from x4path import x4_binary

X4 = x4_binary()


def sections(path):
    f = open(path, "rb")
    elf = ELFFile(f)
    out = {}
    for name in (".text", ".rodata", ".data.rel.ro", ".data"):
        s = elf.get_section_by_name(name)
        out[name] = (s["sh_addr"], s.data())
    return out


def main(argv):
    secs = sections(X4)
    taddr, text = secs[".text"]
    raddr, rodata = secs[".rodata"]
    targets = {}
    for a in argv:
        if a.startswith('"') or not re.fullmatch(r"(0x)?[0-9a-fA-F]+", a):
            s = a.strip('"').encode() + b"\0"
            for m in re.finditer(re.escape(s), rodata):
                # only whole strings: the byte before must end another string
                if m.start() == 0 or rodata[m.start() - 1] == 0:
                    targets[raddr + m.start()] = a
        else:
            targets[int(a, 16)] = a
    if not targets:
        print("no target found")
        return 1
    for t, a in sorted(targets.items()):
        print(f"target {a} = {t:#x}")
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False

    def covering(i):
        """The instruction covering byte i of .text, by majority vote over 32
        linear decodes started 32-64 bytes earlier (x86 resynchronises; a
        decode backwards from a displacement reads `f3 0f 11 05 ...`, movss,
        as `11 05 ...`, adc)."""
        votes = {}
        for s0 in range(max(0, i - 64), max(0, i - 32)):
            for ins in md.disasm(text[s0:i + 16], taddr + s0):
                if ins.address + ins.size > taddr + i:
                    if ins.address <= taddr + i:
                        k = (ins.address, ins.size)
                        votes[k] = votes.get(k, 0) + 1
                    break
        if not votes:
            return None
        (addr, size), _n = max(votes.items(), key=lambda kv: kv[1])
        x = list(md.disasm(text[addr - taddr:addr - taddr + size], addr, 1))
        return x[0] if x else None

    tset = set(targets)
    found = 0
    seen = set()
    # RIP-relative operands: ModRM mod=00 rm=101, disp32 after it, and the
    # instruction may end up to 4 immediate bytes later.
    for m in re.finditer(rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", text):
        i = m.start()
        if i + 5 > len(text):
            continue
        disp = int.from_bytes(text[i + 1:i + 5], "little", signed=True)
        for extra in (0, 1, 2, 4):
            end = taddr + i + 5 + extra
            if end + disp not in tset:
                continue
            ins = covering(i + 1)
            if (ins is None or ins.address in seen
                    or ins.address + ins.size != end or "rip" not in ins.op_str):
                continue
            seen.add(ins.address)
            print(f"code {ins.address:#x}: {ins.mnemonic} {ins.op_str}"
                  f"   -> {targets[end + disp]}")
            found += 1
    # Direct calls and jumps: e8/e9 rel32.
    for m in re.finditer(rb"[\xe8\xe9]", text):
        i = m.start()
        if i + 5 > len(text):
            continue
        dest = taddr + i + 5 + int.from_bytes(text[i + 1:i + 5], "little", signed=True)
        if dest not in tset:
            continue
        ins = covering(i)
        if ins is None or ins.address != taddr + i or ins.address in seen:
            continue
        seen.add(ins.address)
        print(f"code {ins.address:#x}: {ins.mnemonic} {dest:#x}   -> {targets[dest]}")
        found += 1
    for name in (".data.rel.ro", ".data"):
        base, data = secs[name]
        for t, a in targets.items():
            for m in re.finditer(re.escape(t.to_bytes(8, "little")), data):
                if m.start() % 8 == 0:
                    print(f"data {name} {base + m.start():#x}: pointer -> {a}")
                    found += 1
    print(f"{found} reference(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
