// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_ptrwatch.hpp -- X4VR_PTRWATCH: every instruction that reads X4's mouse
// pointer, found by the CPU (read only, take 373).
//
// **Why.** Takes 369, 371 and 372 corrected X4's UI presentations' local
// pointer, its UI cast, and both together; X4's UI cast followed the drawn
// HUD each time the cast was corrected, and the comms highlight never moved.
// The highlight is decided by code none of them reached, and the static
// search over the readers of X4's mouse object found nothing else. So ask
// the hardware: a debug-register watchpoint (perf_event_open,
// PERF_TYPE_BREAKPOINT, read/write, user space only) on the pointer
// (in+0x6718) and on its reference (in+0x6734), on every thread of X4, each
// hit sampled with its instruction pointer and the top of the user stack, so
// a leaf accessor's caller (the return address at [sp]) is named too.
//
// Nothing in X4 is written; the watch costs a trap per access (a few
// thousand a second at most).
//
// The record parsing is here so tests/ptrwatch.cpp can run it on samples of
// a real watchpoint on its own memory.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace x4vr::ptrwatch {

// PERF_REG_X86_SP and PERF_REG_X86_IP (arch/x86/include/uapi/asm/perf_regs.h).
constexpr int kRegSp = 7, kRegIp = 8;
constexpr uint64_t kSampleType = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
                                 PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
constexpr uint32_t kStackBytes = 128;

// A read/write watch on `len` bytes at `addr` in thread `tid` (0 = the
// calling thread), every hit sampled. Returns the fd, or -errno.
inline int open_watch(pid_t tid, uintptr_t addr, int len) {
    perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_RW; // x86 has no read-only data watch
    a.bp_addr = addr;
    a.bp_len = len == 8 ? HW_BREAKPOINT_LEN_8 : HW_BREAKPOINT_LEN_4;
    a.sample_period = 1;
    a.sample_type = kSampleType;
    a.sample_regs_user = (1ull << kRegSp) | (1ull << kRegIp);
    a.sample_stack_user = kStackBytes;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    const long fd = syscall(__NR_perf_event_open, &a, tid, -1, -1,
                            PERF_FLAG_FD_CLOEXEC);
    return fd < 0 ? -errno : (int)fd;
}

struct Sample {
    uint64_t ip = 0;       // the instruction that touched the address
    uint64_t top = 0;      // [sp]: a leaf's return address
    uint64_t caller = 0;   // the first stack word inside X4's .text
    uint32_t tid = 0;
};

// One PERF_RECORD_SAMPLE body (after the header) with kSampleType and the
// two registers: ip; pid, tid; abi, sp, ip; size, data[size], dyn_size.
// `text_lo..text_hi` is X4's code, for the caller. False on a short record.
inline bool parse_sample(const uint8_t *p, size_t n, uint64_t text_lo,
                         uint64_t text_hi, Sample *out) {
    size_t i = 0;
    auto u64 = [&](uint64_t *v) {
        if (i + 8 > n)
            return false;
        memcpy(v, p + i, 8);
        i += 8;
        return true;
    };
    uint64_t ip = 0, pidtid = 0, abi = 0, sp = 0, rip = 0, size = 0;
    if (!u64(&ip) || !u64(&pidtid) || !u64(&abi))
        return false;
    if (abi != 0 && (!u64(&sp) || !u64(&rip)))
        return false;
    if (!u64(&size) || i + size > n)
        return false;
    Sample s;
    s.ip = ip;
    s.tid = (uint32_t)(pidtid >> 32);
    const size_t words = (size_t)(size / 8);
    for (size_t w = 0; w < words; w++) {
        uint64_t v;
        memcpy(&v, p + i + w * 8, 8);
        if (w == 0)
            s.top = v;
        if (!s.caller && v >= text_lo && v < text_hi)
            s.caller = v;
    }
    *out = s;
    return true;
}

// Every record between the ring's tail and head: `f(Sample)` for samples,
// lost counts summed into `*lost`; the tail advanced. `base` is the mmap
// (one metadata page, then `data_size` bytes of ring).
template <class F>
size_t drain(void *base, size_t page, size_t data_size, uint64_t text_lo,
             uint64_t text_hi, uint64_t *lost, F f) {
    auto *meta = (perf_event_mmap_page *)base;
    const uint8_t *data = (const uint8_t *)base + page;
    const uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = meta->data_tail;
    size_t samples = 0;
    uint8_t buf[1024];
    while (tail < head) {
        perf_event_header h;
        for (size_t k = 0; k < sizeof h; k++)
            ((uint8_t *)&h)[k] = data[(tail + k) % data_size];
        if (h.size < sizeof h || h.size > sizeof buf)
            break; // a torn or impossible record: stop, resync next drain
        for (size_t k = 0; k < h.size; k++)
            buf[k] = data[(tail + k) % data_size];
        if (h.type == PERF_RECORD_SAMPLE) {
            Sample s;
            if (parse_sample(buf + sizeof h, h.size - sizeof h, text_lo, text_hi,
                             &s)) {
                f(s);
                samples++;
            }
        } else if (h.type == PERF_RECORD_LOST && h.size >= sizeof h + 16) {
            uint64_t n;
            memcpy(&n, buf + sizeof h + 8, 8);
            *lost += n;
        }
        tail += h.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
    return samples;
}

} // namespace x4vr::ptrwatch
