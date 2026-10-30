// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_PTRWATCH, checked without X4 (x4vr_ptrwatch.hpp): a real hardware
// watchpoint on this process's own memory, opened per thread exactly as the
// injector opens it on X4's threads, its ring drained by the injector's
// parser. A leaf reader must be named with its caller (the return address at
// [sp]), a write must be seen too (read/write watch), another thread's
// reads must arrive on that thread's watch with its tid, and an unwatched
// variable must produce nothing. Self-checking; SKIPs (exit 0, said loudly)
// only where the kernel refuses perf_event_open.
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

#include <sys/mman.h>

#include "../injector/x4vr_ptrwatch.hpp"

using namespace x4vr;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

alignas(8) static volatile int32_t g_pointer[2] = {100, 200};
alignas(8) static volatile int32_t g_other[2] = {1, 2};

extern "C" __attribute__((noinline)) int32_t leaf_x() { return g_pointer[0]; }
extern "C" __attribute__((noinline)) int32_t caller_of_leaf() {
    const int32_t v = leaf_x();
    asm volatile("" ::: "memory");
    return v + 1;
}
extern "C" __attribute__((noinline)) void writer(int32_t v) { g_pointer[1] = v; }
extern "C" __attribute__((noinline)) int32_t other_reader() { return g_other[0]; }

struct Ring {
    int fd = -1;
    void *base = nullptr;
    size_t page = 0, data = 0;
    bool open(pid_t tid, uintptr_t addr) {
        fd = ptrwatch::open_watch(tid, addr, 8);
        if (fd < 0)
            return false;
        page = (size_t)sysconf(_SC_PAGESIZE);
        data = page * 8;
        base = mmap(nullptr, page + data, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        return base != MAP_FAILED;
    }
    std::vector<ptrwatch::Sample> take(uint64_t lo, uint64_t hi, uint64_t *lost) {
        std::vector<ptrwatch::Sample> v;
        ptrwatch::drain(base, page, data, lo, hi, lost,
                        [&](const ptrwatch::Sample &s) { v.push_back(s); });
        return v;
    }
};

int main() {
    // The "text" range for the caller: this test's own functions.
    const uint64_t a = (uint64_t)(void *)&leaf_x, b = (uint64_t)(void *)&caller_of_leaf,
                   c = (uint64_t)(void *)&writer, d = (uint64_t)(void *)&other_reader,
                   m = (uint64_t)(void *)&main;
    uint64_t lo = a, hi = a;
    for (uint64_t x : {b, c, d, m}) {
        lo = x < lo ? x : lo;
        hi = x > hi ? x : hi;
    }
    hi += 0x10000;
    lo = lo > 0x10000 ? lo - 0x10000 : 0;
    // A function's extent: up to the next of these functions in memory
    // (they can be laid out back to back, closer than any fixed span).
    auto end_of = [&](uint64_t f) {
        uint64_t e = f + 256;
        for (uint64_t x : {a, b, c, d, m})
            if (x > f && x < e)
                e = x;
        return e;
    };
    const uint64_t a_end = end_of(a), b_end = end_of(b), c_end = end_of(c);

    Ring r;
    if (!r.open(0, (uintptr_t)g_pointer)) {
        printf("SKIP   perf_event_open refused here (%s) — the watch is "
               "unchecked on this machine\n", strerror(-r.fd));
        return 0;
    }
    uint64_t lost = 0;
    r.take(lo, hi, &lost); // whatever opening produced
    int32_t sum = 0;
    for (int i = 0; i < 5; i++)
        sum += caller_of_leaf();
    writer(7);
    sum += other_reader(); // unwatched: nothing
    auto v = r.take(lo, hi, &lost);
    size_t leaf = 0, named = 0, writes = 0;
    for (const auto &s : v) {
        if (s.ip >= a && s.ip < a_end) {
            leaf++;
            if (s.top >= b && s.top < b_end)
                named++;
        }
        if (s.ip >= c && s.ip < c_end)
            writes++;
    }
    char what[160];
    snprintf(what, sizeof what,
             "5 leaf reads sampled with the instruction inside the leaf "
             "(%zu of %zu samples)", leaf, v.size());
    check(leaf == 5, what);
    check(named == 5, "each leaf read names its caller: the return address at [sp]");
    check(writes == 1, "the write is sampled too (a read/write watch)");
    check(v.size() == 6 && lost == 0, "nothing else: the unwatched variable is silent, nothing lost");

    // Another thread, watched by its own tid, as the injector watches X4's.
    Ring r2;
    std::atomic<pid_t> tid{0};
    std::atomic<int> go{0};
    std::thread t([&] {
        tid.store((pid_t)syscall(SYS_gettid));
        while (!go.load())
            ;
        for (int i = 0; i < 3; i++)
            (void)leaf_x();
    });
    while (!tid.load())
        ;
    const bool ok2 = r2.open(tid.load(), (uintptr_t)g_pointer);
    go.store(1);
    t.join();
    if (!ok2) {
        check(false, "a watch on another thread by its tid");
    } else {
        auto v2 = r2.take(lo, hi, &lost);
        size_t mine = 0;
        for (const auto &s : v2)
            if (s.tid == (uint32_t)tid.load() && s.ip >= a && s.ip < a_end)
                mine++;
        snprintf(what, sizeof what,
                 "another thread's 3 reads arrive on its own watch with its tid "
                 "(%zu of %zu)", mine, v2.size());
        check(mine == 3, what);
    }
    (void)sum;
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
