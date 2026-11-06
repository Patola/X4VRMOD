// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// common/x4vr_reloc.hpp on a made-up X4: every signature and data reference planted in memory,
// where Steam has them and then moved, so it runs with no game installed. A moved block is found
// and an address inside an instruction reads back in Steam's terms only if it points at the
// same thing. Self-checking.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../common/x4vr_reloc.hpp"

using namespace x4vr::reloc;

static int g_fail = 0;
static void check(bool ok, const std::string &what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    if (!ok)
        g_fail++;
}

namespace {
constexpr uintptr_t kText = 0x600000, kTextEnd = 0x3200000;
constexpr size_t kMem = 0x4800000;

struct Fake {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kMem, 0x90);
    bool raw(uintptr_t a, void *o, size_t n) const {
        if (a + n > mem.size())
            return false;
        memcpy(o, &mem[a], n);
        return true;
    }
    void put(uintptr_t a, const void *p, size_t n) { memcpy(&mem[a], p, n); }
    template <class T> void poke(uintptr_t a, T v) { put(a, &v, sizeof v); }
    const uint8_t *text() const { return &mem[kText]; }
    size_t text_size() const { return kTextEnd - kText; }
};

// Plants every signature and data reference, code moved by `code` and data by `data`.
void plant(Fake &f, int64_t code, int64_t data) {
    for (const CodeBlock &b : kCodeBlocks)
        f.put((uintptr_t)((int64_t)b.anchor_va + code), b.anchor, kAnchorLen);
    for (const DataBlock &d : kDataBlocks) {
        const uintptr_t at = (uintptr_t)((int64_t)d.field + code);
        const int64_t target = (int64_t)d.lo + data;
        if (d.kind == kAbs32)
            f.poke<uint32_t>(at, (uint32_t)target);
        else
            f.poke<int32_t>(at, (int32_t)(target - (int64_t)at - 4 - d.tail));
    }
}

Map built(const Fake &f) {
    Map m;
    m.build(f.text(), f.text_size(), kText,
            [&f](uintptr_t a, void *o, size_t n) { return f.raw(a, o, n); });
    return m;
}
} // namespace

int main() {
    check(kNumCode > 0 && kNumData > 0 && kNumRelocs > 0, "the table has blocks, data and addresses");
    for (const CodeBlock &b : kCodeBlocks)
        if (b.lo >= b.hi || !b.anchor) {
            check(false, "a code block is empty");
            break;
        }

    // A signature must not hold an address inside an instruction: a build that only moves
    // things farther would change it for no reason.
    bool clean = true;
    for (const CodeBlock &b : kCodeBlocks) {
        for (const Reloc &r : kRelocs)
            clean = clean && (r.field + 4 <= b.anchor_va || r.field >= b.anchor_va + kAnchorLen);
        for (const DataBlock &d : kDataBlocks)
            clean = clean && (d.field + 4 <= b.anchor_va || d.field >= b.anchor_va + kAnchorLen);
    }
    check(clean, "no signature holds an address");

    // Nothing built: as Steam's, which is what every other test relies on.
    Map fresh;
    check(fresh.identity() && fresh.va(0x1929f9d) == 0x1929f9d, "unbuilt, nothing moves");

    {
        auto f = std::make_unique<Fake>();
        plant(*f, 0, 0);
        const Map m = built(*f);
        check(m.identity() && m.missing() == 0, "everything where it's named: nothing moves");
        check(m.va(kCodeBlocks[0].lo) == kCodeBlocks[0].lo, "...and addresses are themselves");
    }
    {
        const int64_t code = -0x12340, data = -0x1a000;
        auto f = std::make_unique<Fake>();
        plant(*f, code, data);
        const Map m = built(*f);
        check(!m.identity() && m.missing() == 0, "all of it moved: all of it is found");
        bool codeOk = true, dataOk = true;
        for (const CodeBlock &b : kCodeBlocks)
            codeOk = codeOk && m.va(b.lo) == (uintptr_t)((int64_t)b.lo + code);
        for (const DataBlock &d : kDataBlocks)
            dataOk = dataOk && m.va(d.lo) == (uintptr_t)((int64_t)d.lo + data);
        check(codeOk, "...every block of code where it went");
        check(dataOk, "...every piece of data, read from the instruction that uses it");
        check(m.va(0x10) == 0x10 && m.va(0x7f0000000000ull) == 0x7f0000000000ull,
              "an address outside the table isn't touched, a heap pointer's included");

        // An address inside an instruction shows as Steam's, if it points at the same thing.
        const Reloc *rel = nullptr;
        for (const Reloc &r : kRelocs)
            if (r.kind == kRelative) {
                rel = &r;
                break;
            }
        check(rel != nullptr, "the table has a relative address to try");
        if (rel) {
            const uintptr_t at = m.va(rel->field);
            const uintptr_t target = m.va(rel->target);
            const int32_t here = (int32_t)((int64_t)target - (int64_t)at - 4 - rel->tail);
            const int32_t steam = (int32_t)((int64_t)rel->target - (int64_t)rel->field - 4 - rel->tail);
            auto raw = [&f](uintptr_t a, void *o, size_t n) { return f->raw(a, o, n); };
            f->poke<int32_t>(at, here);
            int32_t got = 0;
            check(m.read(raw, rel->field, &got, 4) && got == steam,
                  "an address to the same thing reads as Steam's");
            uint8_t part[2] = {};
            check(m.read(raw, rel->field + 1, part, 2) && part[0] == (uint8_t)(steam >> 8) &&
                      part[1] == (uint8_t)(steam >> 16), "...even read in part");
            f->poke<int32_t>(at, here + 0x40);
            got = 0;
            check(m.read(raw, rel->field, &got, 4) && got == here + 0x40,
                  "an address to anything else stays as it is, so the check fails");
        }
    }
    {
        // A signature that's gone leaves its block where Steam has it, for the check to fail.
        auto f = std::make_unique<Fake>();
        plant(*f, -0x12340, -0x1a000);
        const CodeBlock &b = kCodeBlocks[kNumCode - 1];
        const uintptr_t at = (uintptr_t)((int64_t)b.anchor_va - 0x12340);
        f->poke<uint8_t>(at + 3, (uint8_t)(f->mem[at + 3] ^ 0xff));
        const Map m = built(*f);
        check(m.missing() >= 1 && !m.identity(), "a signature that's gone is counted");
        check(m.va(b.lo) == b.lo, "...and its block stays where Steam has it");
        check(m.va(kCodeBlocks[0].lo) == (uintptr_t)((int64_t)kCodeBlocks[0].lo - 0x12340),
              "...and the others are still found");
    }
    {
        // Far from where it's named, it has to be the only one.
        auto f = std::make_unique<Fake>();
        const int64_t code = 0x300000;
        plant(*f, code, 0);
        Map once = built(*f);
        check(once.missing() == 0 && once.va(kCodeBlocks[1].lo) == (uintptr_t)((int64_t)kCodeBlocks[1].lo + code),
              "moved further than the near search: found");
        const CodeBlock &b = kCodeBlocks[1];
        f->put((uintptr_t)((int64_t)b.anchor_va + code + 0x500000), b.anchor, kAnchorLen);
        Map twice = built(*f);
        check(twice.va(b.lo) == b.lo, "...but two of them is no answer");
    }

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
