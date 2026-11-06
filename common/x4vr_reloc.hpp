// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_reloc.hpp -- the mod's patches on any build of X4 whose code is the same but in
// different places: GOG's, or a hotfix that only moves things (2026-11-06: of the 1457 bytes
// the checks read, Steam's and GOG's differ only in the addresses inside instructions).
//
// The mod is written in Steam's addresses, which act as names. This finds where they are in
// the running build, from a short signature per block of code, and reads data's places out of
// the instructions that use them (tools/make_reloc_table.py generates the table). Then:
//   * va(steam) is that address in this build: for stub operands, patch sites, read-backs;
//   * read() shows this build's bytes in Steam's terms, each address inside an instruction
//     put back to Steam's value if, and only if, it points at the same thing. So the checks
//     stay exactly as strict: compare with Steam's bytes, and any real difference still
//     fails and turns that feature off.
// A signature not found leaves its block where Steam has it, where the checks then fail.
// On Steam's build everything is found where it's named and all of this does nothing.
//
// Never give va() or read() an address that's already this build's, such as one decoded from
// an exported function or a vtable pointer read from an object: it could land in a block
// and be moved twice. Those go straight to memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace x4vr::reloc {

struct CodeBlock {
    uint32_t lo, hi, anchor_va;
    const char *anchor;
};
struct DataBlock {
    uint32_t lo, hi, field;
    uint8_t tail, kind;
};
struct Reloc {
    uint32_t field, target;
    uint8_t tail, kind;
};
enum Kind : uint8_t { kRelative = 0, kAbs32 = 1, kAbs64 = 2, kPlt = 3 };
constexpr size_t kAnchorLen = 24;
// The stubs at the start of the code segment, reordered by a build that imports other
// libraries (GOG's doesn't link Steam). A call into them is the same call.
constexpr size_t kPltSpan = 0x4000;
// Nearly everything moves by far less than this, and looking near costs almost nothing.
constexpr size_t kNearSearch = 1024 * 1024;

#include "x4vr_reloc_table.inc"

constexpr size_t kNumCode = sizeof kCodeBlocks / sizeof kCodeBlocks[0];
constexpr size_t kNumData = sizeof kDataBlocks / sizeof kDataBlocks[0];
constexpr size_t kNumRelocs = sizeof kRelocs / sizeof kRelocs[0];

class Map {
public:
    // `text` is this build's code, starting at address text_va. `raw(va, out, n)` reads this
    // build's memory, as it stands.
    template <class Raw>
    void build(const uint8_t *text, size_t size, uintptr_t text_va, Raw raw) {
        n_ = missing_ = 0;
        identity_ = false;
        text_va_ = text_va;
        moved_ = false;
        for (const CodeBlock &b : kCodeBlocks) {
            int64_t delta = 0;
            if (find(b, text, size, text_va, &delta))
                add(b.lo, b.hi, delta);
            else
                missing_++;
        }
        for (const DataBlock &d : kDataBlocks) {
            int64_t target = 0;
            if (!target_of(raw, d.field, d.tail, d.kind, &target)) {
                missing_++;
                continue;
            }
            add(d.lo, d.hi, target - (int64_t)d.lo);
        }
        top_ = 0;
        for (size_t i = 0; i < n_; i++)
            if (ranges_[i].hi > top_)
                top_ = ranges_[i].hi;
        identity_ = !moved_ && missing_ == 0;
    }

    uintptr_t va(uintptr_t steam) const {
        if (identity_ || steam >= top_)
            return steam;
        // The nearest start wins: a block's end can overlap the next one, which moved apart.
        const Range *best = nullptr;
        for (size_t i = 0; i < n_; i++)
            if (ranges_[i].lo <= steam && steam < ranges_[i].hi &&
                (!best || ranges_[i].lo > best->lo))
                best = &ranges_[i];
        return best ? (uintptr_t)((int64_t)steam + best->delta) : steam;
    }

    template <class Raw> bool read(Raw raw, uintptr_t steam, void *out, size_t n) const {
        if (!raw(va(steam), out, n))
            return false;
        if (identity_ || steam >= top_)
            return true;
        for (const Reloc &r : kRelocs) {
            const size_t len = r.kind == kAbs64 ? 8 : 4;
            if (r.field + len <= steam || r.field >= steam + n)
                continue;
            int64_t got = 0;
            if (!target_of(raw, r.field, r.tail, r.kind, &got))
                continue;
            const bool same = r.kind == kPlt
                                  ? (uintptr_t)got >= text_va_ && (uintptr_t)got < text_va_ + kPltSpan
                                  : (uintptr_t)got == va(r.target);
            if (!same)
                continue;
            // What Steam's build has there.
            uint64_t want = r.target;
            if (r.kind == kRelative || r.kind == kPlt)
                want = (uint32_t)(int32_t)((int64_t)r.target - (int64_t)r.field - 4 - r.tail);
            for (size_t k = 0; k < len; k++) {
                const uintptr_t at = r.field + k;
                if (at >= steam && at < steam + n)
                    ((uint8_t *)out)[at - steam] = (uint8_t)(want >> (8 * k));
            }
        }
        return true;
    }

    bool identity() const { return identity_; }
    size_t missing() const { return missing_; }
    size_t blocks() const { return n_; }

private:
    struct Range {
        uint32_t lo, hi;
        int64_t delta;
    };

    void add(uint32_t lo, uint32_t hi, int64_t delta) {
        ranges_[n_++] = Range{lo, hi, delta};
        moved_ = moved_ || delta != 0;
    }

    template <class Raw>
    bool target_of(Raw raw, uintptr_t field, uint8_t tail, uint8_t kind, int64_t *out) const {
        const uintptr_t at = locate(field);
        if (kind == kAbs64) {
            uint64_t v = 0;
            if (!raw(at, &v, 8))
                return false;
            *out = (int64_t)v;
            return true;
        }
        int32_t v = 0;
        if (!raw(at, &v, 4))
            return false;
        *out = kind == kAbs32 ? (int64_t)(uint32_t)v : (int64_t)at + 4 + tail + v;
        return true;
    }

    // va(), usable while the table's still being built.
    uintptr_t locate(uintptr_t steam) const {
        const Range *best = nullptr;
        for (size_t i = 0; i < n_; i++)
            if (ranges_[i].lo <= steam && steam < ranges_[i].hi &&
                (!best || ranges_[i].lo > best->lo))
                best = &ranges_[i];
        return best ? (uintptr_t)((int64_t)steam + best->delta) : steam;
    }

    static bool find(const CodeBlock &b, const uint8_t *text, size_t size, uintptr_t text_va,
                     int64_t *delta) {
        const int64_t off = (int64_t)b.anchor_va - (int64_t)text_va;
        if (off >= 0 && (size_t)off + kAnchorLen <= size &&
            memcmp(text + off, b.anchor, kAnchorLen) == 0) {
            *delta = 0;
            return true;
        }
        // Near first; then all of it, where it must be the only one.
        const size_t lo = off > (int64_t)kNearSearch ? (size_t)off - kNearSearch : 0;
        const size_t near = off < 0 ? 2 * kNearSearch : (size_t)off + kNearSearch;
        const size_t hi = near < size ? near : size;
        const void *p = hi > lo ? memmem(text + lo, hi - lo, b.anchor, kAnchorLen) : nullptr;
        if (!p) {
            p = memmem(text, size, b.anchor, kAnchorLen);
            if (!p)
                return false;
            const uint8_t *after = (const uint8_t *)p + 1;
            if (memmem(after, (size_t)(text + size - after), b.anchor, kAnchorLen))
                return false;
        }
        *delta = (int64_t)text_va + ((const uint8_t *)p - text) - (int64_t)b.anchor_va;
        return true;
    }

    Range ranges_[kNumCode + kNumData] = {};
    size_t n_ = 0, missing_ = 0;
    uint32_t top_ = 0;
    uintptr_t text_va_ = 0;
    bool identity_ = true, moved_ = false;
};

// This process's X4. Left as it starts, it's Steam's: nothing moves, which is also what every
// test that doesn't build one gets.
inline Map &map() {
    static Map m;
    return m;
}
inline uintptr_t va(uintptr_t steam) { return map().va(steam); }

} // namespace x4vr::reloc
