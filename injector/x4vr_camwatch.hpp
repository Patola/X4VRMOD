// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_camwatch.hpp — WHO writes X4's camera orientation? (read-only)
//
// **Why this exists.** Head-look moves X4's camera through MOUSELOOK, which
// kills cockpit hover; Patola wants the view moved "by our own means". Take
// 315's value scan (x4vr_camscan.hpp) found no slot storing the free-look
// angle in step with the camera, and the disassembly of GetCameraRotation's
// internal call said why: the camera keeps its orientation as 3x3 MATRICES in
// per-frame buffers inside the camera object, and the free-look state that
// produces them lives in the controller that WRITES them.
//
// So instead of guessing the representation, catch the writer. A hardware
// WRITE watchpoint (perf_event_open, PERF_TYPE_BREAKPOINT, user space only —
// perf_event_paranoid 2 allows it on our own process) on the matrix's first
// row reports every store to it: the instruction, the thread, the registers
// at that moment and a slice of the stack. The instruction's function is the
// controller, and its disassembly names where the state lives.
//
// Nothing here runs in X4's threads. Each watched thread gets its own event
// and ring buffer; the kernel fills them, and OUR thread reads them. X4 sees a
// debug trap per store, which the kernel absorbs.
//
// Anchors are decoded from X4's own code and the whole fetch prologue must
// match byte for byte, so a different X4 build refuses to arm instead of
// watching a wrong address.
#pragma once

#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace x4vr {
namespace camwatch {

// ---------------------------------------------------------------- anchors
//
// GetCameraRotation+0x5d: `40 e8 rel32` (a REX-prefixed call) into the fetch
// function, which on X4 9.00 is 0x1925bf0. Its first 162 bytes, verbatim:
// the buffer-index globals, the buffer stride (idx*11*64 = 0x2c0), the inline
// small-vector (capacity 8 at +0x290, data at +0x10), the count at +0x2d0, the
// 0x50-byte entries keyed at +0, and the row loads at +0x20/+0x30/+0x40.
inline constexpr uint8_t kFetchSig[162] = {
    0x64, 0x8b, 0x04, 0x25, 0xe0, 0xd9, 0xff, 0xff, 0x49, 0x89, 0xf9, 0x49,
    0x89, 0xf0, 0x8b, 0x3d, 0x28, 0x18, 0x55, 0x02, 0x48, 0x89, 0xd6, 0x85,
    0xc0, 0x74, 0x06, 0x8b, 0x3d, 0x1f, 0x18, 0x55, 0x02, 0x48, 0x8d, 0x04,
    0xbf, 0x48, 0x8d, 0x04, 0x47, 0x48, 0xc1, 0xe0, 0x06, 0x49, 0x8d, 0x44,
    0x01, 0x30, 0x48, 0x83, 0xb8, 0x90, 0x02, 0x00, 0x00, 0x08, 0x48, 0x8d,
    0x50, 0x10, 0x74, 0x04, 0x48, 0x8b, 0x50, 0x10, 0x48, 0x8d, 0x04, 0xbf,
    0x48, 0x8d, 0x04, 0x47, 0x48, 0xc1, 0xe0, 0x06, 0x49, 0x8b, 0x84, 0x01,
    0xd0, 0x02, 0x00, 0x00, 0x48, 0x8d, 0x0c, 0x80, 0x48, 0xc1, 0xe1, 0x04,
    0x48, 0x01, 0xd1, 0x48, 0x39, 0xd1, 0x0f, 0x84, 0xf4, 0x00, 0x00, 0x00,
    0x48, 0x89, 0xd0, 0xeb, 0x1c, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x40, 0x00, 0x48, 0x83, 0xc0, 0x50,
    0x48, 0x39, 0xc1, 0x0f, 0x84, 0xdb, 0x00, 0x00, 0x00, 0x48, 0x39, 0x30,
    0x75, 0xee, 0x41, 0x0f, 0x28, 0x08, 0x0f, 0x28, 0x58, 0x30, 0x0f, 0x28,
    0x68, 0x20, 0x0f, 0x28, 0x50, 0x40};

// The layout the signature pins. Named so a reader need not re-derive them
// from the bytes above.
inline constexpr uintptr_t kBufBase = 0x30;      // lea 0x30(%r9,idx*0x2c0)
inline constexpr uintptr_t kBufStride = 0x2c0;   // idx*5*2... then <<6
inline constexpr uintptr_t kCapOff = 0x290;      // cmpq $0x8,0x290(buf)
inline constexpr uint64_t kInlineCap = 8;
inline constexpr uintptr_t kDataOff = 0x10;      // inline data, or pointer
inline constexpr uintptr_t kCountOff = 0x2d0;    // from cam + idx*0x2c0
inline constexpr uintptr_t kEntryStride = 0x50;  // add $0x50,%rax
inline constexpr uintptr_t kRow0 = 0x20;         // movaps 0x20(%rax)

struct Anchors {
    bool ok = false;
    uintptr_t fetch = 0, g0 = 0, g1 = 0;
    const char *why = "not decoded";
};

// `read(va, out, n)` returns false for anything it cannot read: the injector
// reads its own mapping, the test reads the ELF file on disk.
template <class Reader>
Anchors decode_anchors(uintptr_t get_camera_rotation, Reader read) {
    Anchors a;
    uint8_t call[6];
    if (!get_camera_rotation || !read(get_camera_rotation + 0x5d, call, 6)) {
        a.why = "GetCameraRotation unreadable";
        return a;
    }
    if (call[0] != 0x40 || call[1] != 0xe8) {
        a.why = "GetCameraRotation+0x5d is not the REX call into the fetch";
        return a;
    }
    int32_t rel;
    memcpy(&rel, call + 2, 4);
    a.fetch = get_camera_rotation + 0x63 + (intptr_t)rel;
    uint8_t sig[sizeof kFetchSig];
    if (!read(a.fetch, sig, sizeof sig)) {
        a.why = "fetch function unreadable";
        return a;
    }
    if (memcmp(sig, kFetchSig, sizeof sig) != 0) {
        a.why = "fetch prologue differs from X4 9.00's — another build; "
                "refusing to aim at a guessed layout";
        return a;
    }
    int32_t d0, d1;
    memcpy(&d0, sig + 0x10, 4);
    memcpy(&d1, sig + 0x1d, 4);
    a.g0 = a.fetch + 0x14 + (intptr_t)d0;
    a.g1 = a.fetch + 0x21 + (intptr_t)d1;
    a.ok = true;
    a.why = "decoded and signature-verified";
    return a;
}

struct Target {
    uintptr_t addr = 0; // first rotation row of the key-0 entry
    uint32_t buf = 0;   // which per-frame buffer
};

// The key-0 entry's row 0, in EVERY buffer that looks valid (up to four, the
// number of hardware watch slots per thread) -- not just the two the index
// globals name right now. If X4 triple-buffers, the writer fills a buffer
// neither global points at yet, and watching only the named ones would report
// a clean null over a store that happened next door. A buffer is valid if its
// count is sane and it holds a key-0 entry; the index globals are returned for
// the log. This is the entry GetCameraRotation reads (it passes key 0).
template <class Reader>
std::vector<Target> targets(uintptr_t cam, const Anchors &a, Reader read,
                            uint32_t *named0 = nullptr,
                            uint32_t *named1 = nullptr) {
    std::vector<Target> out;
    if (!a.ok || !cam)
        return out;
    uint32_t idx[2] = {~0u, ~0u};
    read(a.g0, &idx[0], 4);
    read(a.g1, &idx[1], 4);
    if (named0)
        *named0 = idx[0];
    if (named1)
        *named1 = idx[1];
    for (uint32_t k = 0; k < 4; k++) {
        const uintptr_t buf = cam + kBufBase + k * kBufStride;
        uint64_t cap = 0, count = 0;
        uintptr_t data = 0;
        if (!read(buf + kCapOff, &cap, 8) ||
            !read(cam + k * kBufStride + kCountOff, &count, 8))
            continue;
        if (count == 0 || count > 64 || cap < count)
            continue;
        if (cap == kInlineCap)
            data = buf + kDataOff;
        else if (!read(buf + kDataOff, &data, 8) || !data)
            continue;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t key = 1;
            if (!read(data + i * kEntryStride, &key, 8))
                break;
            if (key == 0) {
                out.push_back({data + i * kEntryStride + kRow0, k});
                break;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- samples
//
// x86-64 perf register numbering: AX BX CX DX SI DI BP SP IP = 0..8, then
// FLAGS CS SS DS ES FS GS = 9..15 (DS..GS are refused for 64-bit), R8..R15 =
// 16..23. Seventeen registers, in ascending bit order in the record.
inline constexpr uint64_t kRegsMask = 0x1ffull | (0xffull << 16);
inline constexpr int kNumRegs = 17;
inline const char *reg_name(int i) {
    static const char *n[kNumRegs] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                      "rbp", "rsp", "rip", "r8",  "r9",  "r10",
                                      "r11", "r12", "r13", "r14", "r15"};
    return i >= 0 && i < kNumRegs ? n[i] : "?";
}
inline constexpr uint32_t kStackBytes = 256;

struct Sample {
    uint64_t ip = 0;   // AFTER the store: data breakpoints trap post-write
    uint32_t pid = 0, tid = 0;
    uint64_t abi = 0;
    uint64_t regs[kNumRegs] = {};
    uint32_t stack_len = 0;
    uint8_t stack[kStackBytes] = {};
};

// One PERF_RECORD_SAMPLE body (after the header), laid out for
// IP | TID | REGS_USER | STACK_USER. Pure, so the layout is tested without a
// kernel.
inline bool parse_sample(const uint8_t *p, size_t n, Sample *s) {
    size_t o = 0;
    auto take = [&](void *dst, size_t k) {
        if (o + k > n)
            return false;
        memcpy(dst, p + o, k);
        o += k;
        return true;
    };
    if (!take(&s->ip, 8) || !take(&s->pid, 4) || !take(&s->tid, 4) ||
        !take(&s->abi, 8))
        return false;
    if (s->abi && !take(s->regs, 8 * kNumRegs))
        return false;
    uint64_t size = 0;
    if (!take(&size, 8))
        return false;
    if (size) {
        const size_t keep = size < kStackBytes ? (size_t)size : kStackBytes;
        if (o + size > n)
            return false;
        memcpy(s->stack, p + o, keep);
        o += size;
        uint64_t dyn = 0;
        if (!take(&dyn, 8))
            return false;
        s->stack_len = (uint32_t)(dyn < keep ? dyn : keep);
    }
    return true;
}

inline std::vector<pid_t> threads_of_self() {
    std::vector<pid_t> out;
    if (DIR *d = opendir("/proc/self/task")) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
                out.push_back((pid_t)atoi(e->d_name));
        closedir(d);
    }
    return out;
}

// ---------------------------------------------------------------- the watch
//
// **Two kinds of event, because locked memory is the scarce thing.** A ring
// buffer is locked memory, and an unprivileged user gets perf_event_mlock_kb
// (516 KB here) per CPU plus RLIMIT_MEMLOCK. One 68 KB ring per X4 thread per
// target would run out partway through X4's threads and silently leave the
// rest unwatched. So every thread gets a COUNTING event (no ring, no memory,
// just the hardware counter), and only threads whose counter moves -- the
// writers -- are upgraded to a SAMPLING event with a ring. The count carried
// out of the counting event keeps stores() exact across the upgrade.
class WriteWatch {
public:
    // Power of two. A sample is ~450 bytes (17 registers, 256 bytes of
    // stack), so 64 KB holds ~140 between polls. It WILL overflow on a burst,
    // and the kernel only writes a LOST record when there is room for the next
    // record -- tests/camwatch.cpp caught 74 of 1000 stores with "lost 0". So
    // the exact number of stores comes from the counters (stores()), and
    // samples are what we could keep.
    static constexpr size_t kDataPages = 16;

    struct Ev {
        int fd = -1;
        uint8_t *mm = nullptr;
        size_t len = 0;
        pid_t tid = 0;
        Target target;
        bool sampling = false;
    };

    ~WriteWatch() { disarm_all(); }

    // 0 on success, else errno. ENOSPC: no free debug register on that
    // thread. EPERM from mmap: the locked-memory allowance is spent.
    int arm(pid_t tid, const Target &t, bool sampling) {
        perf_event_attr a;
        memset(&a, 0, sizeof a);
        a.size = sizeof a;
        a.type = PERF_TYPE_BREAKPOINT;
        a.bp_type = HW_BREAKPOINT_W;
        a.bp_addr = t.addr;
        a.bp_len = HW_BREAKPOINT_LEN_8;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        if (sampling) {
            a.sample_period = 1;
            a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
                            PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
            a.sample_regs_user = kRegsMask;
            a.sample_stack_user = kStackBytes;
            a.wakeup_events = 1;
        }
        const int fd = (int)syscall(SYS_perf_event_open, &a, tid, -1, -1,
                                    PERF_FLAG_FD_CLOEXEC);
        if (fd < 0)
            return errno;
        Ev e;
        e.fd = fd;
        e.tid = tid;
        e.target = t;
        e.sampling = sampling;
        if (sampling) {
            const size_t page = (size_t)sysconf(_SC_PAGESIZE);
            e.len = (1 + kDataPages) * page;
            void *mm = mmap(nullptr, e.len, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
            if (mm == MAP_FAILED) {
                const int err = errno;
                close(fd);
                return err;
            }
            e.mm = (uint8_t *)mm;
        }
        ev_.push_back(e);
        return 0;
    }

    bool armed_on(pid_t tid) const {
        for (const Ev &e : ev_)
            if (e.tid == tid)
                return true;
        return false;
    }

    // Counting events whose counter moved: the threads that WRITE a target.
    std::vector<std::pair<pid_t, Target>> writers_to_upgrade() const {
        std::vector<std::pair<pid_t, Target>> out;
        for (const Ev &e : ev_)
            if (!e.sampling && count_of(e) > 0)
                out.push_back({e.tid, e.target});
        return out;
    }

    // Replace (tid, target)'s counting event with a sampling one, carrying
    // its count so stores() stays exact. The counting event is closed FIRST:
    // two events on one address would spend two of the thread's four debug
    // registers. 0 or errno; on failure the store count is still carried.
    int upgrade(pid_t tid, const Target &t) {
        for (size_t i = 0; i < ev_.size(); i++) {
            Ev &e = ev_[i];
            if (e.sampling || e.tid != tid || e.target.addr != t.addr)
                continue;
            carry_ += count_of(e);
            close(e.fd);
            ev_.erase(ev_.begin() + (long)i);
            return arm(tid, t, true);
        }
        return ENOENT;
    }

    void disarm_all() {
        for (Ev &e : ev_) {
            carry_ += count_of(e);
            if (e.mm)
                munmap(e.mm, e.len);
            if (e.fd >= 0)
                close(e.fd);
        }
        ev_.clear();
    }

    size_t events() const { return ev_.size(); }
    size_t sampling_events() const {
        size_t n = 0;
        for (const Ev &e : ev_)
            n += e.sampling;
        return n;
    }
    uint64_t lost() const { return lost_; }

    // Every store the hardware saw, sampled or not, including events already
    // closed: read() on a perf event returns its count.
    uint64_t stores() const {
        uint64_t sum = carry_;
        for (const Ev &e : ev_)
            sum += count_of(e);
        return sum;
    }

    // Drain every ring; f(const Sample&, const Target&) for each store.
    template <class F> void poll(F f) {
        const size_t page = (size_t)sysconf(_SC_PAGESIZE);
        const size_t size = kDataPages * page;
        std::vector<uint8_t> rec;
        for (Ev &e : ev_) {
            if (!e.sampling)
                continue;
            auto *mp = (perf_event_mmap_page *)e.mm;
            const uint64_t head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
            uint64_t tail = mp->data_tail;
            const uint8_t *data = e.mm + page;
            while (tail + sizeof(perf_event_header) <= head) {
                perf_event_header h;
                copy_ring(data, size, tail, &h, sizeof h);
                if (h.size < sizeof h || tail + h.size > head)
                    break;
                rec.resize(h.size - sizeof h);
                copy_ring(data, size, tail + sizeof h, rec.data(), rec.size());
                if (h.type == PERF_RECORD_SAMPLE) {
                    Sample s;
                    if (parse_sample(rec.data(), rec.size(), &s))
                        f(s, e.target);
                } else if (h.type == PERF_RECORD_LOST && rec.size() >= 16) {
                    uint64_t n;
                    memcpy(&n, rec.data() + 8, 8);
                    lost_ += n;
                }
                tail += h.size;
            }
            __atomic_store_n(&mp->data_tail, tail, __ATOMIC_RELEASE);
        }
    }

private:
    static uint64_t count_of(const Ev &e) {
        uint64_t v = 0;
        return read(e.fd, &v, sizeof v) == (ssize_t)sizeof v ? v : 0;
    }

    static void copy_ring(const uint8_t *data, size_t size, uint64_t at,
                          void *dst, size_t n) {
        const size_t off = (size_t)(at & (size - 1));
        const size_t first = n < size - off ? n : size - off;
        memcpy(dst, data + off, first);
        if (first < n)
            memcpy((uint8_t *)dst + first, data, n - first);
    }

    std::vector<Ev> ev_;
    uint64_t lost_ = 0, carry_ = 0;
};

} // namespace camwatch
} // namespace x4vr
