#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Every instruction in X4's .text that addresses memory at a given
displacement -- `[reg + 0xcc8]` -- with the function it sits in.

    tools/xdisp.py 0xcc8 [--mnemonic movss] [--range 0x1900000-0x1a00000]

A byte scan for the disp32 (and, for displacements under 0x80, the disp8);
the instruction covering each hit is found by majority vote over 32 linear
decodes started 32-64 bytes earlier, and reported only if its operand really
is [reg +/- disp]. A vote under 24/32 is printed beside the line. "Function" is the nearest
preceding symbol objdump would name, i.e. only as good as X4's exports.
Reads X4's bytes only.
"""
import argparse
import bisect
import re
import sys

import capstone
from elftools.elf.elffile import ELFFile

from x4path import x4_binary

X4 = x4_binary()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disp")
    ap.add_argument("--mnemonic", default=None)
    ap.add_argument("--range", default=None)
    a = ap.parse_args()
    disp = int(a.disp, 16)
    lo, hi = 0, 1 << 64
    if a.range:
        lo, hi = (int(x, 16) for x in a.range.split("-"))
    elf = ELFFile(open(X4, "rb"))
    s = elf.get_section_by_name(".text")
    base, text = s["sh_addr"], s.data()
    syms = []
    for name in (".dynsym", ".symtab"):
        st = elf.get_section_by_name(name)
        if st is None:
            continue
        for sym in st.iter_symbols():
            if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"]:
                syms.append((sym["st_value"], sym.name))
    syms.sort()
    addrs = [x for x, _ in syms]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    pats = [disp.to_bytes(4, "little", signed=disp >= 1 << 31)]
    if disp < 0x80:
        pats.append(bytes([disp]))
    want = f"{disp:#x}"
    seen = set()
    for pat in pats:
        for m in re.finditer(re.escape(pat), text):
            i = m.start()
            va = base + i
            if not lo <= va < hi:
                continue
            # Which instruction covers byte i? x86 decoding resynchronises
            # within a few instructions, so sweep linearly from 32 different
            # starts before it and take the majority. (Decoding backwards from
            # the displacement takes `f3 0f 11 83 74 08 00 00`, movss, for
            # `11 83 ...`, adc.)
            votes = {}
            for s0 in range(max(0, i - 64), max(0, i - 32)):
                for ins in md.disasm(text[s0:i + 16], base + s0):
                    if ins.address + ins.size > va:
                        if ins.address <= va:
                            k = (ins.address, ins.size)
                            votes[k] = votes.get(k, 0) + 1
                        break
            if not votes:
                continue
            (addr, size), n = max(votes.items(), key=lambda kv: kv[1])
            if addr in seen:
                continue
            ins = list(md.disasm(text[addr - base:addr - base + size], addr, 1))
            if not ins:
                continue
            ins = ins[0]
            if f"+ {want}]" not in ins.op_str and f"- {want}]" not in ins.op_str:
                continue
            if "rip" in ins.op_str:
                continue
            if a.mnemonic and not ins.mnemonic.startswith(a.mnemonic):
                continue
            seen.add(addr)
            k = bisect.bisect_right(addrs, addr) - 1
            fn = f"{syms[k][1]}+{addr - syms[k][0]:#x}" if k >= 0 else "?"
            print(f"{addr:#x}: {ins.mnemonic} {ins.op_str}   <{fn}>"
                  + ("" if n >= 24 else f"   (decode vote {n}/32)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
