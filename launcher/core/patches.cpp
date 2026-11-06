// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Does the mod fit this X4? Not by a checksum: a hotfix that leaves our code in
// place should stay green with no release of ours. The launcher runs the mod's
// own checks, the ones the injector makes in X4's memory at load, over the file
// (docs/packaging-plan.md, decision 8). Each group is what its feature's call
// site in x4vr_inject.cpp verifies before it patches.
#include "x4vr_launcher.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "../../common/x4vr_reloc.hpp"
#include "../../common/x4vr_steer.hpp"
#include "../../injector/x4vr_aimpick.hpp"
#include "../../injector/x4vr_camwatch.hpp"
#include "../../injector/x4vr_camwrite.hpp"
#include "../../injector/x4vr_csmfit.hpp"
#include "../../injector/x4vr_extview.hpp"
#include "../../injector/x4vr_foot.hpp"
#include "../../injector/x4vr_menus.hpp"
#include "../../injector/x4vr_uimouse.hpp"

namespace x4vr::launcher {

// ------------------------------------------------------------------- ElfFile
ElfFile::~ElfFile() {
    if (data_)
        munmap(const_cast<uint8_t *>(data_), size_);
}

bool ElfFile::open(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 64) {
        ::close(fd);
        return false;
    }
    void *m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED)
        return false;
    data_ = (const uint8_t *)m;
    size_ = (size_t)st.st_size;
    return is_x86_64();
}

bool ElfFile::is_x86_64() const {
    return data_ && size_ >= 64 && memcmp(data_, "\x7f" "ELF", 4) == 0 &&
           data_[4] == 2 && data_[5] == 1 && data_[18] == 62 && data_[19] == 0;
}

namespace {
struct Phdr {
    uint32_t type;
    bool exec;
    uint64_t offset, vaddr, filesz;
};
// The n'th program header, or false if the file doesn't hold it. The file may be
// anything the player pointed at, so every offset is checked.
bool phdr_at(const uint8_t *d, size_t size, uint16_t i, Phdr *out) {
    uint64_t phoff;
    uint16_t phentsize, phnum;
    memcpy(&phoff, d + 0x20, 8);
    memcpy(&phentsize, d + 0x36, 2);
    memcpy(&phnum, d + 0x38, 2);
    if (i >= phnum || phentsize < 56 || phoff > size ||
        (size - phoff) / phentsize <= i)
        return false;
    const uint8_t *p = d + phoff + (size_t)i * phentsize;
    uint32_t flags = 0;
    memcpy(&out->type, p, 4);
    memcpy(&flags, p + 4, 4);
    out->exec = (flags & 1) != 0;
    memcpy(&out->offset, p + 8, 8);
    memcpy(&out->vaddr, p + 16, 8);
    memcpy(&out->filesz, p + 32, 8);
    return true;
}
constexpr uint32_t kPtLoad = 1, kPtDynamic = 2;
constexpr uint64_t kDtNull = 0, kDtNeeded = 1, kDtStrtab = 5;
} // namespace

bool ElfFile::read(uintptr_t va, void *out, size_t n) const {
    if (!data_)
        return false;
    Phdr p;
    for (uint16_t i = 0; phdr_at(data_, size_, i, &p); i++) {
        if (p.type != kPtLoad || va < p.vaddr || va - p.vaddr >= p.filesz)
            continue;
        const uint64_t off = va - p.vaddr;
        if (n > p.filesz - off || p.offset > size_ || p.offset + off > size_ ||
            n > size_ - (p.offset + off))
            return false;
        memcpy(out, data_ + p.offset + off, n);
        return true;
    }
    return false;
}

bool ElfFile::text(const uint8_t **bytes, size_t *size, uintptr_t *va) const {
    if (!data_)
        return false;
    Phdr p;
    for (uint16_t i = 0; phdr_at(data_, size_, i, &p); i++) {
        if (p.type != kPtLoad || !p.exec || p.offset > size_ || p.filesz > size_ - p.offset)
            continue;
        *bytes = data_ + p.offset;
        *size = (size_t)p.filesz;
        *va = (uintptr_t)p.vaddr;
        return true;
    }
    return false;
}

std::vector<std::string> ElfFile::needed() const {
    std::vector<std::string> out;
    if (!data_)
        return out;
    Phdr p;
    for (uint16_t i = 0; phdr_at(data_, size_, i, &p); i++) {
        if (p.type != kPtDynamic || p.offset > size_ || p.filesz > size_ - p.offset)
            continue;
        uint64_t strtab = 0;
        std::vector<uint64_t> names;
        for (uint64_t o = 0; o + 16 <= p.filesz; o += 16) {
            uint64_t tag, val;
            memcpy(&tag, data_ + p.offset + o, 8);
            memcpy(&val, data_ + p.offset + o + 8, 8);
            if (tag == kDtNull)
                break;
            if (tag == kDtStrtab)
                strtab = val;
            if (tag == kDtNeeded)
                names.push_back(val);
        }
        for (uint64_t n : names) {
            std::string s;
            char c;
            for (uint64_t a = strtab + n;
                 s.size() < 256 && read((uintptr_t)a, &c, 1) && c; a++)
                s.push_back(c);
            if (!s.empty())
                out.push_back(std::move(s));
        }
        break;
    }
    return out;
}

bool binary_needs_steam(const std::string &exe) {
    ElfFile e;
    if (!e.open(exe))
        return false;
    for (const std::string &n : e.needed())
        if (n.rfind("libsteam_api", 0) == 0)
            return true;
    return false;
}

static reloc::Map map_of(const ElfFile &x4) {
    reloc::Map m;
    const uint8_t *bytes = nullptr;
    size_t size = 0;
    uintptr_t va = 0;
    if (x4.text(&bytes, &size, &va))
        m.build(bytes, size, va,
                [&x4](uintptr_t a, void *o, size_t n) { return x4.read(a, o, n); });
    return m;
}

std::vector<PatchResult> check_patches(const ElfFile &x4) {
    const reloc::Map m = map_of(x4);
    auto raw = [&x4](uintptr_t a, void *o, size_t n) { return x4.read(a, o, n); };
    return check_patches(VaReader(
        [&m, raw](uintptr_t a, void *o, size_t n) { return m.read(raw, a, o, n); }));
}

std::string reloc_report(const ElfFile &x4) {
    const reloc::Map m = map_of(x4);
    std::string out;
    char line[64];
    for (const reloc::CodeBlock &b : reloc::kCodeBlocks) {
        snprintf(line, sizeof line, "code %#x %lld\n", b.lo, (long long)m.va(b.lo) - (long long)b.lo);
        out += line;
    }
    for (const reloc::DataBlock &d : reloc::kDataBlocks) {
        snprintf(line, sizeof line, "data %#x %lld\n", d.lo, (long long)m.va(d.lo) - (long long)d.lo);
        out += line;
    }
    snprintf(line, sizeof line, "found %zu missing %zu\n", m.blocks(), m.missing());
    return out + line;
}

// ------------------------------------------------------------------- the groups
std::vector<PatchResult> check_patches(const VaReader &read) {
    auto rd = [&read](uintptr_t va, void *o, size_t n) { return read(va, o, n); };
    std::vector<PatchResult> out;
    auto group = [&out](const char *id, const char *feature, bool ok, uintptr_t bad) {
        out.push_back(PatchResult{id, feature, ok, ok ? 0 : bad});
    };
    uintptr_t bad = 0;
    bool ok;

    // camwrite_ready: hop 3 and the write sites.
    ok = camwatch::verify_hop3_sites(rd, &bad) &&
         camwrite::verify_write_sites(rd, &bad);
    group("headlook", "Head tracking in the cockpit", ok, bad);

    ok = foot::verify_null_exit_sites(rd, &bad);
    group("foot", "Looking around while walking", ok, bad);

    ok = extview::verify_sites(rd, &bad) && extview::early_site_ok(&bad);
    group("extview", "Head tracking in the external views (F2, F3, F4)", ok, bad);

    ok = uimouse::verify_sites(rd, &bad);
    group("uimouse", "Clicking the cockpit HUD", ok, bad);

    ok = steer::verify_sites(rd, &bad);
    group("steer", "The steering target drawn in VR", ok, bad);

    ok = aimpick::verify_sites(rd, &bad);
    group("aimpick", "Aiming at where the steering target points", ok, bad);

    ok = menus::verify_sites(rd, &bad);
    group("menus", "Menus lined up with the cursor", ok, bad);

    ok = csmfit::verify_sites(rd, &bad);
    group("shadows", "The shadow fix", ok, bad);

    return out;
}

} // namespace x4vr::launcher
