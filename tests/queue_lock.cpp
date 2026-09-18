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
    // BOUNDED. The first draft spun forever here: xr_old holds `ours` while
    // waiting, so a partner blocked on `ours` never arrives, and the harness
    // hung for a reason that had nothing to do with what it was testing.
    void rendezvous() {
        arrived.fetch_add(1);
        auto until = std::chrono::steady_clock::now() + 300ms;
        while (arrived.load() < 2 && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
    }
};

// The whole thing reduces to LOCK ORDER, and saying so is clearer than four
// hand-written thread bodies:
//
//   OURS_FIRST     take g_vr_queue_mu, then enter the runtime, which takes its
//                  own mutex inside. That is a VrQueueLock wrapped around
//                  xrEndFrame or around a 557-line present hook.
//   RUNTIME_FIRST  enter the runtime with nothing of ours held; the runtime's
//                  submit comes back through x4vr_QueueSubmit and takes ours
//                  there.
//
// A cycle needs one of each. Two threads that agree on the order never
// deadlock however wide the scopes are -- which is why the ORIGINAL code
// survived 298 takes on WiVRn, where the runtime never submits in-process at
// all and the second edge simply does not exist.
enum Order { OURS_FIRST, RUNTIME_FIRST };

static void actor(Pair &p, Order order, std::atomic<bool> &stuck) {
    std::timed_mutex &first = order == OURS_FIRST ? p.ours : p.runtime;
    std::timed_mutex &second = order == OURS_FIRST ? p.runtime : p.ours;
    if (!first.try_lock_for(400ms)) {
        stuck.store(true);
        return;
    }
    p.rendezvous();
    if (!second.try_lock_for(400ms)) {
        stuck.store(true);
        first.unlock();
        return;
    }
    second.unlock();
    first.unlock();
}

static bool deadlocks(Order present, Order xr) {
    Pair p;
    std::thread a(actor, std::ref(p), present, std::ref(p.present_stuck));
    std::thread b(actor, std::ref(p), xr, std::ref(p.xr_stuck));
    a.join();
    b.join();
    return p.present_stuck.load() || p.xr_stuck.load();
}

static bool hangs(Order present, Order xr, int rounds) {
    for (int i = 0; i < rounds; i++)
        if (deadlocks(present, xr))
            return true;
    return false;
}

int main() {
    // **The truth table, and my first version of it was wrong.**
    //
    // I assumed three of four cells hang. They do not: a cycle needs the two
    // threads to DISAGREE on the order, so the two matching cells are both
    // safe. That is also why the original code ran for 298 takes — on WiVRn
    // the runtime never submits inside X4, so there is no RUNTIME_FIRST actor
    // and no disagreement is possible.
    //
    //   present        xr             result
    //   OURS_FIRST     RUNTIME_FIRST  deadlock  <- takes 298-301
    //   RUNTIME_FIRST  OURS_FIRST     deadlock  <- take 302, the half-fix
    //   OURS_FIRST     OURS_FIRST     safe      <- but one call into the
    //                                              runtime away from not being
    //   RUNTIME_FIRST  RUNTIME_FIRST  safe      <- where the code is now
    check(hangs(OURS_FIRST, RUNTIME_FIRST, 5),
          "present holds ours into the runtime, xr holds the runtime into "
          "ours — takes 298-301");
    check(hangs(RUNTIME_FIRST, OURS_FIRST, 5),
          "and the mirror — take 302, which is what 56ab22f left behind by "
          "narrowing only the present side");
    check(!hangs(RUNTIME_FIRST, RUNTIME_FIRST, 30),
          "BOTH narrowed: neither side holds our lock while entering the "
          "runtime, 30 rounds clean — this is where the code is now");
    check(!hangs(OURS_FIRST, OURS_FIRST, 30),
          "both un-narrowed is also acyclic, which is why this survived 298 "
          "takes on WiVRn and says nothing about the fix");

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
