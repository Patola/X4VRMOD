// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// injector/x4vr_menus.hpp: the sites against the real binary, and the
// open-menu walk and the conversation read against fakes laid out as the
// disassembly says X4's are.
#include <cstdio>
#include <cstring>
#include <vector>

#include "../injector/x4vr_menus.hpp"
#include "x4_path.hpp"

using namespace x4vr::menus;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// The ELF reader of tests/foot.cpp: X4 is non-PIE, a VA maps through the
// PT_LOAD headers.
struct Elf {
    std::vector<uint8_t> d;
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        d.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        const bool ok = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        return ok && d.size() > 64;
    }
    bool read(uintptr_t va, void *out, size_t n) const {
        uint64_t phoff;
        uint16_t phentsize, phnum;
        memcpy(&phoff, &d[0x20], 8);
        memcpy(&phentsize, &d[0x36], 2);
        memcpy(&phnum, &d[0x38], 2);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint8_t *p = &d[phoff + (size_t)i * phentsize];
            uint32_t type;
            uint64_t off, vaddr, filesz;
            memcpy(&type, p, 4);
            memcpy(&off, p + 8, 8);
            memcpy(&vaddr, p + 16, 8);
            memcpy(&filesz, p + 32, 8);
            if (type == 1 && va >= vaddr && va + n <= vaddr + filesz) {
                memcpy(out, &d[off + (va - vaddr)], n);
                return true;
            }
        }
        return false;
    }
};

// A fake process: named regions; a read outside them fails, as
// process_vm_readv does on an unmapped address.
struct Fake {
    struct R {
        uintptr_t va;
        std::vector<uint8_t> b;
    };
    std::vector<R> r;
    uintptr_t add(size_t n) {
        const uintptr_t va = 0x10000000 + r.size() * 0x1000;
        r.push_back({va, std::vector<uint8_t>(n, 0)});
        return va;
    }
    uint8_t *at(uintptr_t va) {
        for (auto &x : r)
            if (va >= x.va && va < x.va + x.b.size())
                return &x.b[va - x.va];
        return nullptr;
    }
    bool read(uintptr_t va, void *out, size_t n) {
        for (auto &x : r)
            if (va >= x.va && va + n <= x.va + x.b.size()) {
                memcpy(out, &x.b[va - x.va], n);
                return true;
            }
        return false;
    }
    template <class T> void put(uintptr_t va, T v) { memcpy(at(va), &v, sizeof v); }
    // A node as the disassembly lays it out; the name in its own region,
    // like a heap std::string.
    uintptr_t node(const char *name, bool full) {
        const uintptr_t n = add(0x48);
        const uintptr_t s = add(strlen(name) + 1);
        memcpy(at(s), name, strlen(name));
        put<uintptr_t>(n + kNodeName, s);
        put<uint64_t>(n + kNodeNameLen, strlen(name));
        *at(n + kNodeFull) = full ? 1 : 0;
        return n;
    }
};

int main() {
    // 0. The sites, against the real binary.
    {
        static Elf elf;
        const char *x4 = x4_test_binary();
        if (!elf.load(x4)) {
            printf("SKIP   no X4 binary at %s — the sites are unchecked\n", x4);
        } else {
            auto rd = [](uintptr_t va, void *o, size_t n) { return elf.read(va, o, n); };
            uintptr_t bad = 0;
            check(verify_sites(rd, &bad),
                  "IsConversationActive and the open-menu lookup match X4 9.00");
            const CodeBytes *sites[] = {&kConversation, &kMenuRoot, &kMenuDescend,
                                        &kMenuRight, &kMenuFlag};
            int refused = 0;
            for (const CodeBytes *c : sites) {
                for (uint8_t k : {(uint8_t)0, (uint8_t)(c->n - 1)}) {
                    const uintptr_t flip = c->va + k;
                    auto rf = [flip](uintptr_t va, void *o, size_t n) {
                        if (!elf.read(va, o, n))
                            return false;
                        if (flip >= va && flip < va + n)
                            ((uint8_t *)o)[flip - va] ^= 0x01;
                        return true;
                    };
                    uintptr_t b2 = 0;
                    if (!verify_sites(rf, &b2) && b2 == flip)
                        refused++;
                }
            }
            check(refused == 10,
                  "one byte changed at either end of each site: refused, and named");
        }
    }

    // 1. The walk.
    {
        Fake f;
        const uintptr_t slot = f.add(8);
        auto rd = [&f](uintptr_t va, void *o, size_t n) { return f.read(va, o, n); };
        Entry e[16];
        char line[256];
        check(walk(rd, slot, e, 16) == 0, "walk: an empty tree is no menus");
        describe(e, 0, line, sizeof line);
        check(!strcmp(line, "none"), "walk: and says none");

        const uintptr_t a = f.node("MapMenu", true);
        const uintptr_t b = f.node("InteractMenu", false);
        const uintptr_t c = f.node("OptionsMenu", true);
        f.put<uintptr_t>(slot, a);
        f.put<uintptr_t>(a + kNodeLeft, b);
        f.put<uintptr_t>(a + kNodeRight, c);
        const int n = walk(rd, slot, e, 16);
        check(n == 3 && has(e, n, "InteractMenu") && has(e, n, "MapMenu") &&
                  !has(e, n, "Interact"),
              "walk: three nodes, by name, whole names only");
        describe(e, n, line, sizeof line);
        check(!strcmp(line, "InteractMenu, MapMenu[full], OptionsMenu[full]"),
              "walk: described sorted, full-screen ones marked");

        f.put<uintptr_t>(c + kNodeLeft, a);
        check(walk(rd, slot, e, 16) == -1, "walk: a cycle is refused");
        f.put<uintptr_t>(c + kNodeLeft, 0x7fff0000);
        check(walk(rd, slot, e, 16) == -1, "walk: an unreadable child is refused");
        f.put<uintptr_t>(c + kNodeLeft, 0);
        *f.at(*(uintptr_t *)f.at(b + kNodeName)) = 0x01;
        check(walk(rd, slot, e, 16) == -1, "walk: a name that is not text is refused");
        *f.at(*(uintptr_t *)f.at(b + kNodeName)) = 'I';
        f.put<uint64_t>(b + kNodeNameLen, 0);
        check(walk(rd, slot, e, 16) == -1, "walk: an empty name is refused");
        f.put<uint64_t>(b + kNodeNameLen, 12);

        const uintptr_t lng = f.node("AVeryLongMenuNameThatGoesOnAndOnAndOnPastFortySeven", false);
        f.put<uintptr_t>(c + kNodeRight, lng);
        const int m = walk(rd, slot, e, 16);
        bool cut = false;
        for (int i = 0; i < m; i++)
            cut |= strlen(e[i].name) == 47 && !strncmp(e[i].name, "AVeryLong", 9);
        check(m == 4 && cut, "walk: a long name is cut at 47, not refused");
        check(walk(rd, slot, e, 2) == 4, "walk: counts past what it stores");

        // A chain of 65: more nodes than any menu set; refused.
        Fake g;
        const uintptr_t s2 = g.add(8);
        uintptr_t prev = 0;
        for (int i = 0; i < 65; i++) {
            const uintptr_t nd = g.node("M", false);
            if (prev)
                g.put<uintptr_t>(prev + kNodeRight, nd);
            else
                g.put<uintptr_t>(s2, nd);
            prev = nd;
        }
        auto rg = [&g](uintptr_t va, void *o, size_t n) { return g.read(va, o, n); };
        check(walk(rg, s2, e, 16) == -1, "walk: 65 nodes is refused, not walked");
    }

    // 2. The conversation.
    {
        Fake f;
        const uintptr_t obj = f.add(0x200);
        uintptr_t global = 0;
        auto rd = [&](uintptr_t va, void *o, size_t n) {
            if (va == kConvGlobal && n == sizeof global) {
                memcpy(o, &global, n);
                return true;
            }
            return f.read(va, o, n);
        };
        int32_t st = 0;
        check(!conversation_state(rd, &st), "conversation: no object, nothing read");
        global = obj;
        f.put<int32_t>(obj + kConvStateOff, 1);
        check(conversation_state(rd, &st) && st == 1 && !conversation_active(st),
              "conversation: state 1 is none (IsConversationActive's != 1)");
        f.put<int32_t>(obj + kConvStateOff, 3);
        check(conversation_state(rd, &st) && conversation_active(st),
              "conversation: any other state is active");
        global = 0x7fff0000;
        check(!conversation_state(rd, &st), "conversation: a stale object reads nothing");
    }

    printf("\n%s\n", g_fail ? "SOME CASES FAILED" : "all cases passed");
    return g_fail ? 1 : 0;
}
