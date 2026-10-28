// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_menus.hpp — what X4's own state says about its menus, READ, never
// called (take 341, Patola: "Go with the no-setting option first").
//
// Two readers, both through the caller's Reader (process_vm_readv in the
// injector, so a stale pointer is a failed read, not a crash):
//
// * **X4's table of open menus.** IsFullscreenMenuDisplayed (0xbb07e0,
//   declared in ego_helptext/helptext.lua) looks a menu up BY NAME in a tree
//   whose root sits at 0x3dfc208 and reads the found node's full-screen flag.
//   The cockpit's right-click menu is Lua's "InteractMenu" and not
//   full-screen, so asking the function about it says nothing; walking the
//   tree says whether it is there at all. Node layout, from the lookup and
//   from the in-order walk at 0x117f7c0: left child +0x08, right +0x10, the
//   name a std::string at +0x20 (data) / +0x28 (length), the full-screen
//   flag at +0x40.
// * **The conversation.** IsConversationActive (0xc02dc0, declared in
//   ego_detailmonitorhelper/helper.lua) is `*(int *)(*(void **)0x3db8300 +
//   0x1a0) != 1` with NO null check on the global -- so it is read here, not
//   called, like the steering bytes.
//
// Every instruction those facts come from is checked byte for byte before
// anything is read (verify_sites); a different X4 build reads nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x4vr {
namespace menus {

struct CodeBytes {
    uintptr_t va;
    uint8_t n;
    uint8_t bytes[48];
};

// IsConversationActive: mov 0x31b5539(%rip),%rax; cmpl $1,0x1a0(%rax);
// setne %al; ret.
inline constexpr CodeBytes kConversation = {
    0xc02dc0, 18,
    {0x48, 0x8b, 0x05, 0x39, 0x55, 0x1b, 0x03, 0x83, 0xb8, 0xa0, 0x01, 0x00,
     0x00, 0x01, 0x0f, 0x95, 0xc0, 0xc3}};
constexpr uintptr_t kConvGlobal = 0x3db8300;
constexpr uintptr_t kConvStateOff = 0x1a0;
constexpr int32_t kConvIdle = 1;

// IsFullscreenMenuDisplayed, by name: mov 0x324b9ce(%rip),%r13 -- the root.
inline constexpr CodeBytes kMenuRoot = {
    0xbb0833, 7, {0x4c, 0x8b, 0x2d, 0xce, 0xb9, 0x24, 0x03}};
// The descent: mov %r13,%rbp; mov 0x8(%r13),%r13 (left); test; je;
// mov 0x28(%r13),%r14 (length); ...; mov 0x20(%r13),%rdi (data);
// mov %rbx,%rsi; call memcmp; test; jne.
inline constexpr CodeBytes kMenuDescend = {
    0xbb0860, 47,
    {0x4c, 0x89, 0xed, 0x4d, 0x8b, 0x6d, 0x08, 0x4d, 0x85, 0xed, 0x74, 0x4c,
     0x4d, 0x8b, 0x75, 0x28, 0x4c, 0x89, 0xfa, 0x4d, 0x39, 0xfe, 0x49, 0x0f,
     0x46, 0xd6, 0x48, 0x85, 0xd2, 0x74, 0x10, 0x49, 0x8b, 0x7d, 0x20, 0x48,
     0x89, 0xde, 0xe8, 0x45, 0x63, 0xaa, 0xff, 0x85, 0xc0, 0x75, 0x1c}};
// mov 0x10(%r13),%r13 (right); test; jne.
inline constexpr CodeBytes kMenuRight = {
    0xbb08af, 9, {0x4d, 0x8b, 0x6d, 0x10, 0x4d, 0x85, 0xed, 0x75, 0xb4}};
// movzbl 0x40(%rbp),%eax -- the flag the function returns.
inline constexpr CodeBytes kMenuFlag = {0xbb0906, 4, {0x0f, 0xb6, 0x45, 0x40}};
constexpr uintptr_t kMenuRootSlot = 0x3dfc208;
constexpr uintptr_t kNodeLeft = 0x08, kNodeRight = 0x10;
constexpr uintptr_t kNodeName = 0x20, kNodeNameLen = 0x28, kNodeFull = 0x40;

template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    const CodeBytes *sites[] = {&kConversation, &kMenuRoot, &kMenuDescend,
                                &kMenuRight, &kMenuFlag};
    for (const CodeBytes *c : sites) {
        uint8_t b[48];
        if (!read(c->va, b, c->n)) {
            *bad = c->va;
            return false;
        }
        for (uint8_t i = 0; i < c->n; i++)
            if (b[i] != c->bytes[i]) {
                *bad = c->va + i;
                return false;
            }
    }
    int32_t r = 0;
    memcpy(&r, kConversation.bytes + 3, 4); // the global it reads
    if (kConversation.va + 7 + (intptr_t)r != kConvGlobal) {
        *bad = kConversation.va;
        return false;
    }
    memcpy(&r, kMenuRoot.bytes + 3, 4); // the root slot
    if (kMenuRoot.va + 7 + (intptr_t)r != kMenuRootSlot) {
        *bad = kMenuRoot.va;
        return false;
    }
    return true;
}

// The conversation's state word, or false if there is no conversation
// object to read (the global NULL: loading, or torn down).
template <class Reader> bool conversation_state(Reader read, int32_t *state) {
    uintptr_t g = 0;
    if (!read(kConvGlobal, &g, sizeof g) || !g)
        return false;
    return read(g + kConvStateOff, state, sizeof *state);
}
inline bool conversation_active(int32_t state) { return state != kConvIdle; }

struct Entry {
    char name[48]; // NUL-terminated, cut at 47
    bool full;
};

// Every node of the open-menu tree, depth first, up to `max` entries and
// kMaxNodes visited. -1 when a read fails or the tree is not a tree (a
// cycle, a name that is not printable): X4 is mid-change or the layout is
// not what was read, and nothing from it is to be believed.
constexpr int kMaxNodes = 64;
template <class Reader>
int walk(Reader read, uintptr_t root_slot, Entry *out, int max) {
    uintptr_t root = 0;
    if (!read(root_slot, &root, sizeof root))
        return -1;
    uintptr_t stack[kMaxNodes];
    uintptr_t seen[kMaxNodes];
    int sp = 0, nseen = 0, n = 0;
    if (root)
        stack[sp++] = root;
    while (sp > 0) {
        const uintptr_t node = stack[--sp];
        for (int i = 0; i < nseen; i++)
            if (seen[i] == node)
                return -1; // a cycle
        if (nseen == kMaxNodes)
            return -1;     // more nodes than any menu set X4 has
        seen[nseen++] = node;
        uint8_t raw[kNodeFull + 1];
        if (!read(node, raw, sizeof raw))
            return -1;
        uintptr_t left, right, data;
        uint64_t len;
        memcpy(&left, raw + kNodeLeft, sizeof left);
        memcpy(&right, raw + kNodeRight, sizeof right);
        memcpy(&data, raw + kNodeName, sizeof data);
        memcpy(&len, raw + kNodeNameLen, sizeof len);
        if (n < max) {
            Entry &e = out[n];
            const size_t take = len < sizeof e.name - 1 ? (size_t)len
                                                        : sizeof e.name - 1;
            if (len == 0 || !data || !read(data, e.name, take))
                return -1;
            e.name[take] = 0;
            for (size_t i = 0; i < take; i++)
                if (e.name[i] < 0x20 || e.name[i] > 0x7e)
                    return -1;
            e.full = raw[kNodeFull] != 0;
        }
        n++;
        if (right) {
            if (sp == kMaxNodes)
                return -1;
            stack[sp++] = right;
        }
        if (left) {
            if (sp == kMaxNodes)
                return -1;
            stack[sp++] = left;
        }
    }
    return n;
}

// "InteractMenu, MapMenu[full]": names sorted, full-screen ones marked, so
// the same set always prints the same line.
inline void describe(Entry *e, int n, char *out, size_t cap) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(e[j - 1].name, e[j].name) > 0; j--) {
            Entry t = e[j - 1];
            e[j - 1] = e[j];
            e[j] = t;
        }
    size_t at = 0;
    out[0] = 0;
    for (int i = 0; i < n && at + 1 < cap; i++) {
        const int w = snprintf(out + at, cap - at, "%s%s%s", i ? ", " : "",
                               e[i].name, e[i].full ? "[full]" : "");
        if (w < 0)
            break;
        at += (size_t)w;
    }
    if (n == 0)
        snprintf(out, cap, "none");
}

inline bool has(const Entry *e, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, name))
            return true;
    return false;
}

} // namespace menus
} // namespace x4vr
