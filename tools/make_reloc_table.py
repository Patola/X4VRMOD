#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
"""Generates common/x4vr_reloc_table.inc: how to find, in any build of X4 9.00, the code
and data the mod's patches use. Its addresses and short signatures come from Steam's
build, in whose terms the mod is written; another build proves they hold there too.

    tools/make_reloc_table.py READS STEAM_X4 OTHER_X4 [--out FILE]

READS is the output of build/tests/x4vr_record_reads: every byte the checks read. The
addresses come from the sources. Its verdict is the point: every byte the checks read must
be the same in the other build, apart from the addresses inside instructions, each of
which must point at the same thing. Anything else is a real difference, and it says so.
Reads the binaries only. What it writes is short signatures, like the checks' own bytes.
"""
import argparse
import collections
import glob
import os
import re
import struct
import sys

import capstone
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
import numpy as np

ANCHOR = 24  # bytes in a code block's signature


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        phoff = struct.unpack_from("<Q", d, 0x20)[0]
        es, n = struct.unpack_from("<HH", d, 0x36)
        self.segs = []
        for i in range(n):
            o = phoff + i * es
            t, fl = struct.unpack_from("<II", d, o)
            off, va = struct.unpack_from("<QQ", d, o + 8)
            fs, ms = struct.unpack_from("<QQ", d, o + 32)
            if t == 1:
                self.segs.append((va, fs, ms, fl, off))
        va, fs, _, _, off = [s for s in self.segs if s[3] & 1][0]
        self.tva, self.t = va, d[off:off + fs]

    def kind(self, a):
        for va, _, ms, fl, _ in self.segs:
            if va <= a < va + ms:
                return "text" if fl & 1 else ("rodata" if not fl & 2 else "data")
        return None

    def read(self, a, n):
        for va, fs, _, _, off in self.segs:
            if va <= a and a + n <= va + fs:
                return self.d[off + a - va:off + a - va + n]
        return None


def strip(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def source_addresses(steam):
    """Every address constant in the sources. Not the layer's SPIR-V, which only looks like
    addresses, and not the ptrwatch instrument's two bounds for the code segment."""
    out = set()
    for f in sorted(glob.glob("injector/*.hpp") + glob.glob("injector/*.cpp") +
                    glob.glob("common/*.hpp")):
        if os.path.basename(f) in ("x4vr_reloc.hpp",):
            continue
        for m in re.findall(r"\b0x([0-9a-fA-F]{6,8})\b", strip(open(f, errors="replace").read())):
            a = int(m, 16)
            if steam.kind(a) and a not in (0x65a000, 0x2c74000):
                out.add(a)
    return out


def find_relocs(steam, reads):
    """The addresses inside the bytes the checks read."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    relocs = {}  # field va -> (target, tail, kind) ; kind 0 relative, 1 abs32, 2 abs64
    undecoded = []
    in_image = lambda v: steam.kind(v) is not None
    for va, n in reads:
        k = steam.kind(va)
        if k != "text":
            if n == 8:
                v = struct.unpack("<Q", steam.read(va, 8))[0]
                if in_image(v):
                    relocs[va] = (v, 0, 2)
            continue
        code = steam.read(va, n)
        end = va
        for i in md.disasm(code, va):
            end = i.address + i.size
            if i.disp_size == 4:
                for op in i.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        tail = i.size - i.disp_offset - 4
                        relocs[i.address + i.disp_offset] = (i.address + i.size + op.mem.disp, tail, 0)
                    elif op.type == X86_OP_MEM and in_image(op.mem.disp):
                        relocs[i.address + i.disp_offset] = (op.mem.disp, 0, 1)
            if i.imm_size == 4:
                rel = i.group(capstone.CS_GRP_JUMP) or i.group(capstone.CS_GRP_CALL)
                for op in i.operands:
                    if op.type == X86_OP_IMM:
                        if rel:
                            tail = i.size - i.imm_offset - 4
                            relocs[i.address + i.imm_offset] = (op.imm, tail, 0)
                        elif in_image(op.imm & 0xffffffff):
                            relocs[i.address + i.imm_offset] = (op.imm & 0xffffffff, 0, 1)
        if end < va + n:
            undecoded.append((va, n, end))
    return relocs, undecoded


class Index:
    """Where 8-byte windows of the first build's code are, in the second's."""

    def __init__(self, a, b):
        self.a, self.b = a, b
        tb = b.t
        parts, poss = [], []
        for o in range(8):
            n = (len(tb) - o) // 8
            parts.append(np.frombuffer(tb, dtype="<u8", count=n, offset=o))
            poss.append(o + 8 * np.arange(n, dtype=np.int32))
        vals = np.concatenate(parts)
        pos = np.concatenate(poss)
        order = np.argsort(vals)
        self.sv, self.sp = vals[order], pos[order]

    def votes(self, i, lo, hi):
        ta = self.a.t
        js = [j for j in range(i + lo, i + hi) if 0 <= j and j + 8 <= len(ta)]
        if not js:
            return collections.Counter()
        vs = np.array([int.from_bytes(ta[j:j + 8], "little") for j in js], dtype=np.uint64)
        l, r = np.searchsorted(self.sv, vs, "left"), np.searchsorted(self.sv, vs, "right")
        out = collections.Counter()
        for j, x, y in zip(js, l, r):
            if y - x == 1:
                out[int(self.sp[x]) - j] += 1
        return out

    def delta(self, a):
        """How far the code at `a` moved: what follows it decides, since `a` may be the
        first or the last thing in a function that moved apart from its neighbours."""
        i = a - self.a.tva
        base = self.b.tva - self.a.tva
        fwd, bwd = self.votes(i, 0, 96), self.votes(i, -96, 0)
        def same_after(d):
            j = i + d
            ta, tb = self.a.t, self.b.t
            return sum(ta[i + k] == tb[j + k] for k in range(32) if j + k < len(tb) and i + k < len(ta))
        cands = list(fwd) + [d for d in bwd if d not in fwd]
        if not cands:
            return None, 0
        best = max(cands, key=lambda d: (same_after(d), fwd[d], bwd[d]))
        return best + base, fwd[best] + bwd[best]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reads")
    ap.add_argument("steam")
    ap.add_argument("other")
    ap.add_argument("--out", default="common/x4vr_reloc_table.inc")
    a = ap.parse_args()
    steam, other = Elf(a.steam), Elf(a.other)
    reads = []
    for l in open(a.reads):
        v, n = l.split()
        reads.append((int(v, 16), int(n)))
    addrs = source_addresses(steam)
    relocs, undecoded = find_relocs(steam, reads)
    print(f"sources: {len(addrs)} addresses; checks read {len(reads)} places, "
          f"{sum(n for _, n in reads)} bytes; {len(relocs)} addresses inside them")
    for u in undecoded:
        print("  couldn't decode all of", hex(u[0]), u[1], "stopped at", hex(u[2]))

    # A call into the stubs at the start of the code: they're reordered by what a build links.
    plt_end = steam.tva + 0x4000
    for f, (tg, t, k) in list(relocs.items()):
        if k == 0 and steam.tva <= tg < plt_end:
            relocs[f] = (tg, t, 3)
    need = set(addrs) | {t for t, _, k in relocs.values() if k != 3}
    for va, n in reads:
        need |= {va, va + n - 1} if steam.kind(va) == "text" else {va}
    code = sorted(x for x in need if steam.kind(x) == "text")
    data = sorted(x for x in need if steam.kind(x) != "text")

    idx = Index(steam, other)
    print("indexed", flush=True)
    moved, unfound = {}, []
    for x in code:
        d, votes = idx.delta(x)
        if d is None:
            unfound.append(x)
        else:
            moved[x] = d

    def field_target(fv, tail, kind, new_field):
        if kind in (0, 3):
            return new_field + 4 + tail + struct.unpack("<i", other.read(new_field, 4))[0]
        if kind == 1:
            return struct.unpack("<I", other.read(new_field, 4))[0]
        return struct.unpack("<Q", other.read(new_field, 8))[0]

    def nearest_delta(x):
        keys = [k for k in moved if abs(k - x) < 0x1000]
        return moved[min(keys, key=lambda k: abs(k - x))] if keys else None

    # Data: read from the instructions that use it, in the other build.
    ta = steam.t
    i32 = [np.frombuffer(ta, dtype="<i4", count=(len(ta) - o) // 4, offset=o) for o in range(4)]
    def brute_refs(D):
        out = []
        for o in range(4):
            idxs = o + 4 * np.arange(len(i32[o]), dtype=np.int64)
            for tail in (0, 1, 2, 4):
                for h in np.nonzero(steam.tva + idxs + 4 + tail + i32[o].astype(np.int64) == D)[0][:30]:
                    p = int(o + 4 * h)
                    if p >= 1 and (ta[p - 1] & 0xC7) == 0x05:
                        out.append((steam.tva + p, tail, 0))
            for h in np.nonzero(i32[o].astype(np.int64) == D)[0][:30]:
                out.append((steam.tva + int(o + 4 * h), 0, 1))
        return out

    data_moved, data_ref, thin = {}, {}, []
    for D in data:
        refs = [(f, t, k) for f, (tg, t, k) in relocs.items() if tg == D and steam.kind(f) == "text"]
        use_brute = not refs
        if use_brute:
            refs = brute_refs(D)
        votes = collections.Counter()
        first = {}
        for f, t, k in refs[:40]:
            d = moved.get(f)
            if d is None:
                d, n = idx.delta(f)
                if d is None or n < 6:
                    continue
            got = field_target(f, t, k, f + d)
            if other.kind(got) is None:
                continue
            votes[got - D] += 1
            first.setdefault(got - D, (f, t, k, d))
        if not votes:
            # A slot inside something found on its own, as a vtable's are.
            if not any(0 <= D - x < 256 for x in addrs if x != D):
                unfound.append(D)
            continue
        dd, c = votes.most_common(1)[0]
        if c < 2:
            thin.append(D)
        # Read-only data must read the same, whatever the build calls it.
        if steam.kind(D) == "rodata" and steam.read(D, 8) != other.read(D + dd, 8):
            print(f"  read-only data at {D:#x} differs: {steam.read(D, 8).hex()} "
                  f"vs {other.read(D + dd, 8).hex()}")
        data_moved[D] = dd
        f, t, k, fd = first[dd]
        data_ref[D] = (f, t, k)
        if f not in moved:
            moved[f] = fd
            moved[f + 3] = fd

    # Group the code by how far it moved.
    blocks = []
    for x in sorted(moved):
        d = moved[x]
        if blocks and blocks[-1][2] == d and x - blocks[-1][1] < 0x10000:
            blocks[-1][1] = x
        else:
            blocks.append([x, x, d])
    tb = other.t
    def unique(t, w):
        p = t.find(w)
        return p >= 0 and t.find(w, p + 1) < 0
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    field_starts = {f + 4 + t for f, t, _ in data_ref.values()}
    def has_address(i):
        if i.disp_size == 4 or (i.imm_size == 4 and (i.group(capstone.CS_GRP_JUMP) or
                                                     i.group(capstone.CS_GRP_CALL))):
            return True
        return any(op.type == X86_OP_IMM and steam.kind(op.imm & 0xffffffff) for op in i.operands)
    def clean_windows(start, span):
        """Where a signature can start: runs of whole instructions with no address in them,
        which a build that only moves things can't change, however far it moves them."""
        begin = None
        for i in md.disasm(steam.read(start, span), start):
            if has_address(i):
                begin = None
                continue
            if begin is None:
                begin = i.address
            while i.address + i.size - begin >= ANCHOR:
                yield begin
                begin += i.size
                break
    table = []
    no_anchor = []
    for lo, hi, d in blocks:
        chosen = None
        # An instruction's start: `lo` may be a field inside one, read for a data address.
        starts = sorted(x for x in moved if lo <= x <= hi and moved[x] == d and
                        not any(x in (f, f + 3) for f, _, _ in data_ref.values()))
        starts += sorted(s for s in field_starts if lo <= s <= hi + 8)
        for begin in starts:
            for p in clean_windows(begin, 0x400):
                i, j = p - steam.tva, p + d - other.tva
                w = ta[i:i + ANCHOR]
                if j >= 0 and tb[j:j + ANCHOR] == w and unique(ta, w) and unique(tb, w):
                    chosen = (p, w)
                    break
            if chosen:
                break
        # A function too short for that: any bytes the two builds share, around no address.
        if chosen is None:
            fields = [(f, 8 if k == 2 else 4) for f, (_, _, k) in relocs.items()]
            fields += [(f, 4) for f, _, _ in data_ref.values()]
            for p in list(range(lo, min(lo + 0x600, hi + 0x200))) + list(range(lo - 1, lo - 0x200, -1)):
                i, j = p - steam.tva, p + d - other.tva
                if i < 0 or j < 0 or any(f < p + ANCHOR and p < f + n for f, n in fields):
                    continue
                w = ta[i:i + ANCHOR]
                if tb[j:j + ANCHOR] == w and unique(ta, w) and unique(tb, w):
                    chosen = (p, w)
                    break
        if chosen is None:
            no_anchor.append((lo, hi, d))
        else:
            table.append((lo, hi + 64, d, chosen[0], chosen[1]))
    print(f"code: {len(moved)} places, {len(blocks)} blocks, {len(no_anchor)} with no signature; "
          f"data: {len(data_moved)} found, {len(thin)} by one use; not found: {[hex(u) for u in unfound]}")
    for b in no_anchor:
        print("  no signature:", hex(b[0]), hex(b[1]), b[2])
    for D in thin:
        print("  one use only:", hex(D))

    # The verdict.
    def where(x):
        if x in data_moved:
            return x + data_moved[x]
        for lo, hi, d, _, _ in table:
            if lo <= x < hi:
                return x + d
        for D in sorted(data_moved):
            if D <= x < D + 256:
                return x + data_moved[D]
        return None
    bad = 0
    keep = {}
    for va, n in reads:
        nv = where(va)
        if nv is None:
            print("  unmapped read:", hex(va), n)
            bad += 1
            continue
        s, g = steam.read(va, n), other.read(nv, n)
        if g is None:
            print("  not in the file:", hex(va), n)
            bad += 1
            continue
        mask = bytearray(n)
        for f, (tg, t, k) in relocs.items():
            size = 8 if k == 2 else 4
            if va - size < f < va + n:
                nf = where(f)
                if nf is None:
                    continue
                gt = field_target(f, t, k, nf)
                if k == 3:
                    if not other.tva <= gt < other.tva + 0x4000:
                        print(f"  call at {f:#x} isn't to the stubs any more: {gt:#x}")
                        bad += 1
                        continue
                    keep[f] = (tg, t, k)
                    for z in range(max(f, va), min(f + size, va + n)):
                        mask[z - va] = 1
                    continue
                wt = where(tg)
                if s[max(0, f - va):f - va + size] == g[max(0, f - va):f - va + size]:
                    if k != 0 and gt == tg:
                        continue  # a constant that only looks like an address
                if wt == gt:
                    keep[f] = (tg, t, k)
                    for z in range(max(f, va), min(f + size, va + n)):
                        mask[z - va] = 1
                elif k == 0:
                    print(f"  address inside {va:#x} points elsewhere: field {f:#x} "
                          f"wants {tg:#x}->{wt if wt is None else hex(wt)}, has {gt:#x}")
                    bad += 1
        diff = [z for z in range(n) if not mask[z] and s[z] != g[z]]
        if diff:
            print(f"  REAL DIFFERENCE at {va:#x}+{diff[0]}: {s.hex()} vs {g.hex()}")
            bad += 1
    print(f"verdict: {len(reads)} places checked, {len(keep)} addresses translated, {bad} problem(s)")

    # Data entries: adjacent ones that moved together are one range.
    drange = []
    for D in sorted(data_moved):
        hi = D + 16
        for va, n in reads:
            if va >= D and va < D + 256 and steam.kind(va) != "text":
                hi = max(hi, va + n)
        f, t, k = data_ref[D]
        drange.append((D, hi, f, t, k))
    with open(a.out, "w") as out:
        out.write("// Generated by tools/make_reloc_table.py -- do not edit. Short signatures of X4's\n"
                  "// code and where it stands in Steam's build; see common/x4vr_reloc.hpp.\n")
        out.write("static const CodeBlock kCodeBlocks[] = {\n")
        for lo, hi, d, p, w in table:
            hexs = "".join(f"\\x{c:02x}" for c in w)
            out.write(f'    {{{lo:#x}, {hi:#x}, {p:#x}, "{hexs}"}},\n')
        out.write("};\nstatic const DataBlock kDataBlocks[] = {\n")
        for D, hi, f, t, k in drange:
            out.write(f"    {{{D:#x}, {hi:#x}, {f:#x}, {t}, {k}}},\n")
        out.write("};\nstatic const Reloc kRelocs[] = {\n")
        for f in sorted(keep):
            tg, t, k = keep[f]
            out.write(f"    {{{f:#x}, {tg:#x}, {t}, {k}}},\n")
        out.write("};\n")
    print("wrote", a.out, os.path.getsize(a.out), "bytes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
