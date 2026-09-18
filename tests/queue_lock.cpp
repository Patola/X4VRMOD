// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// queue_lock.cpp — the shape of #36's lock, against the SteamVR deadlock.
//
// Takes 298-301 hung X4 under SteamVR. A live `gdb -p` on the wedged process
// gave the whole cycle:
//
//   X4's present thread    x4vr_QueuePresentKHR  holds g_vr_queue_mu
//                            -> vrclient.so      xrReleaseSwapchainImage
//                            -> BLOCKED on a SteamVR-internal mutex
//
//   our XR session thread  vr_thread -> vrclient inside xrEndFrame,
//                            holding that same SteamVR mutex
//                            -> x4vr_QueueSubmit
//                            -> BLOCKED on g_vr_queue_mu
//
// Textbook ABBA across a library boundary, and neither side can see the
// other's mutex. WiVRn never exposed it because an out-of-process runtime does
// not call vkQueueSubmit inside X4, so our XR thread never holds a runtime
// lock while wanting ours.
//
// **This file tests the SHAPE, not the arithmetic.** It models two mutexes and
// two threads and asserts that the old nesting deadlocks and the new one does
// not. That distinction matters: the rate fix in take 295 was verified in a
// regime where it could not misbehave, and shipped broken.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

static int g_fail = 0;
static void check(bool ok, const char *what) {
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        g_fail++;
}

using namespace std::chrono_literals;

// `ours` is g_vr_queue_mu. `runtime` stands for whatever vrclient.so takes
// inside xrEndFrame and xrReleaseSwapchainImage -- we never see its name, only
// that both of those calls contend for it.
struct Pair {
    std::timed_mutex ours, runtime;
    std::atomic<int> arrived{0};
    std::atomic<bool> present_stuck{false}, xr_stuck{false};

    // Both threads take their FIRST lock before either reaches for its second.
    // Without this the race is real but intermittent, and a test that only
    // sometimes reproduces the bug is not a regression test.
    void rendezvous() {
        arrived.fetch_add(1);
        while (arrived.load() < 2)
            std::this_thread::yield();
    }
};

// The XR session thread, identical in both shapes: it is inside the runtime,
// holding the runtime's lock, when the runtime submits through our hook.
static void xr_thread(Pair &p) {
    std::unique_lock<std::timed_mutex> rt(p.runtime);   // inside xrEndFrame
    p.rendezvous();
    // vrclient calls vkQueueSubmit -> x4vr_QueueSubmit -> VrQueueLock
    if (!p.ours.try_lock_for(400ms)) {
        p.xr_stuck.store(true);
        return;
    }
    p.ours.unlock();
}

// OLD: the lock is taken at the top of a 557-line present hook and held to the
// end, so xrReleaseSwapchainImage is called underneath it.
static void present_old(Pair &p) {
    std::unique_lock<std::timed_mutex> ou(p.ours);      // VrQueueLock, line 35
    p.rendezvous();
    if (!p.runtime.try_lock_for(400ms)) {               // xrReleaseSwapchainImage
        p.present_stuck.store(true);
        return;
    }
    p.runtime.unlock();
}

// NEW: our lock wraps only the queue operation. The call into the runtime is
// made with nothing of ours held. Ordering -- release must follow the
// composite's submit -- is program order on this one thread and never needed
// the mutex; the old comment above the lock said as much and kept it anyway.
static void present_new(Pair &p) {
    {
        std::lock_guard<std::timed_mutex> ou(p.ours);   // the composite's submit
    }
    p.rendezvous();
    if (!p.runtime.try_lock_for(400ms)) {               // xrReleaseSwapchainImage
        p.present_stuck.store(true);
        return;
    }
    p.runtime.unlock();
    {
        std::lock_guard<std::timed_mutex> ou(p.ours);   // the present itself
    }
}

static bool deadlocks(void (*present)(Pair &)) {
    Pair p;
    std::thread a(present, std::ref(p));
    std::thread b(xr_thread, std::ref(p));
    a.join();
    b.join();
    return p.present_stuck.load() || p.xr_stuck.load();
}

int main() {
    // The negative control comes FIRST and is the one that earns the rest: a
    // deadlock test that cannot reproduce the deadlock proves nothing about
    // the fix that follows it.
    bool old_hangs = false;
    for (int i = 0; i < 5 && !old_hangs; i++)
        old_hangs = deadlocks(present_old);
    check(old_hangs,
          "the OLD shape deadlocks — our lock held across the runtime call, "
          "which is what takes 298-301 did");

    bool new_hangs = false;
    for (int i = 0; i < 20; i++)
        new_hangs |= deadlocks(present_new);
    check(!new_hangs,
          "the NEW shape does not, over 20 rounds — nothing of ours is held "
          "while entering the runtime, so the cycle cannot form");

    // And the property the narrowing must NOT cost: two submitters still get
    // exclusive turns on the queue. #36 exists because this device has one
    // graphics queue and vkQueueSubmit is externally synchronised.
    {
        std::timed_mutex q;
        std::atomic<int> inside{0};
        std::atomic<int> overlaps{0}, turns{0};
        auto submitter = [&] {
            for (int i = 0; i < 2000; i++) {
                std::lock_guard<std::timed_mutex> g(q);
                if (inside.fetch_add(1) != 0)
                    overlaps.fetch_add(1);
                turns.fetch_add(1);
                inside.fetch_sub(1);
            }
        };
        std::thread t1(submitter), t2(submitter);
        t1.join();
        t2.join();
        check(overlaps.load() == 0 && turns.load() == 4000,
              "narrowing keeps the exclusion #36 needs — 4000 submits, 0 "
              "overlapping");
    }

    std::printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n",
                g_fail);
    return g_fail ? 1 : 0;
}
