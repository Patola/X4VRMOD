// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The injector-to-layer channel, exercised without either of them.
//
// What this DOES cover: the reader's refusals. A null pointer, a wrong
// magic, a wrong version and a half-written (odd seq) state must each be
// declined rather than read, because every one of them would otherwise hand
// the layer a plausible cursor position that is not one.
//
// What this does NOT cover, stated so the pass is not over-read: the
// writer's memory ordering. x86 is store-ordered, so a deliberately
// mis-fenced writer still passes the tearing case here -- verified with a
// negative control. The fences in publish_cursor() are for weakly-ordered
// hardware and this suite cannot speak for them.
//
// The tearing case did earn its keep once, by failing for a reason that was
// entirely its own: the earlier cases left x and y unequal, so every read
// before the writer's first update looked torn. Seeding them equal is the
// fix, and the episode is why the negative control above was run at all.
#include "../common/x4vr_share.hpp"
#include <cstdio>
#include <thread>
extern "C" x4vr::Shared *x4vr_shared_state() { static x4vr::Shared s; return &s; }
int fails = 0;
void ck(bool ok, const char *w) { printf("%-42s %s\n", w, ok?"ok":"FAIL"); if(!ok) fails++; }
int main() {
    float x=9,y=9; bool v=true;
    ck(!x4vr::share_read(nullptr,&x,&y,&v), "null pointer is refused, not dereferenced");
    x4vr::Shared *s = x4vr_shared_state();
    s->cursor_x.store(12.5f); s->cursor_y.store(34.5f); s->cursor_visible.store(1);
    ck(x4vr::share_read(s,&x,&y,&v) && x==12.5f && y==34.5f && v, "reads a settled value");
    s->version = 99;
    ck(!x4vr::share_read(s,&x,&y,&v), "version mismatch is refused");
    s->version = x4vr::kShareVersion;
    s->magic = 0xdead;
    ck(!x4vr::share_read(s,&x,&y,&v), "magic mismatch is refused");
    s->magic = x4vr::kShareMagic;
    s->seq.store(1); // odd: a write in progress
    ck(!x4vr::share_read(s,&x,&y,&v), "mid-write (odd seq) is refused");
    s->seq.store(2);

    // Task #75: the raw readback travels beside the belief, and the two must
    // stay distinguishable. They are separate fields for one reason -- the
    // belief is stored AFTER the servo step and so contains a command X4 has
    // not received -- and a future edit that aliases them would erase exactly
    // the quantity the channel was widened to carry.
    s->cam_yaw_deg.store(30.0f);
    s->cam_pitch_deg.store(5.0f);
    s->cam_raw_yaw_deg.store(27.5f);
    s->cam_raw_pitch_deg.store(4.5f);
    s->cam_raw_valid.store(1);
    s->cam_raw_ns.store(123456789ull);
    ck(s->cam_yaw_deg.load() == 30.0f && s->cam_raw_yaw_deg.load() == 27.5f &&
       s->cam_yaw_deg.load() != s->cam_raw_yaw_deg.load(),
       "belief and readback are separate fields");
    ck(s->cam_raw_ns.load() == 123456789ull && s->cam_raw_valid.load() == 1,
       "the readback carries its own timestamp and validity");
    // The version had to move with the layout. A reader built against v1 must
    // refuse a v2 block rather than read cam_raw_* out of the cursor pixels.
    ck(x4vr::kShareVersion >= 3u, "kShareVersion was bumped for cam_seq");
    ck(s->cam_raw_valid.is_lock_free() && s->cam_raw_ns.is_lock_free(),
       "the new atomics are lock-free (written from the SDL hook path)");
    // The earlier cases left x=12.5 y=34.5, which differ -- every read before
    // the writer's first update would count as a tear. Seed them equal.
    s->cursor_x.store(0.f); s->cursor_y.store(0.f);
    // concurrent writer: reader must never see a mixed pair
    std::atomic<bool> stop{false}; long mixed=0;
    std::thread w([&]{ for(int i=0;i<200000 && !stop;i++){
        uint32_t a=s->seq.load(); s->seq.store(a+1,std::memory_order_relaxed); std::atomic_thread_fence(std::memory_order_release);
        s->cursor_x.store((float)i,std::memory_order_relaxed); s->cursor_y.store((float)i,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release); s->seq.store(a+2);} });
    for(int i=0;i<200000;i++){ float a,b; bool vv;
        if(x4vr::share_read(s,&a,&b,&vv) && a!=b) mixed++; }
    stop=true; w.join();
    ck(mixed==0, "no torn pair under a concurrent writer");

    // ---- task #75: the CAMERA block, which had no seqlock at all -----------
    //
    // Seven fields published by the servo tick and read by the layer's
    // submission block. The layer took the readback near the top of that block
    // and `cam_yaw_deg` a hundred lines further down, so one `poselag` line
    // could carry `x4=` and `cmd=` from ticks an unbounded distance apart --
    // and #75's uncommanded-motion residual was measured by subtracting one
    // from the other. The declared pose has the same hazard and it is worse:
    // a yaw from one tick with a pitch from the next is an orientation the
    // frame was never rendered from, and the compositor warps by it.
    //
    // Every field is derived from one counter, so "all seven agree on k" is a
    // complete consistency check rather than a pairwise one.
    printf("\n");
    {
        auto publish = [&](int k) {
            x4vr::CamSnap c;
            c.yaw_deg = (float)k;
            c.pitch_deg = -(float)k;
            c.valid = (k & 1) != 0;
            c.raw_yaw_deg = 2.0f * (float)k;
            c.raw_pitch_deg = -2.0f * (float)k;
            c.raw_valid = (k & 1) != 0;
            c.raw_ns = (uint64_t)k;
            x4vr::share_write_camera(s, c);
        };
        auto consistent = [](const x4vr::CamSnap &c) {
            const int k = (int)c.yaw_deg;
            return c.pitch_deg == -(float)k && c.raw_yaw_deg == 2.0f * (float)k &&
                   c.raw_pitch_deg == -2.0f * (float)k &&
                   c.raw_ns == (uint64_t)k && c.valid == ((k & 1) != 0) &&
                   c.raw_valid == ((k & 1) != 0);
        };
        publish(0);
        const int kIter = 400000;

        // **The negative control, and it runs FIRST.** If the unguarded reader
        // never tears on this machine then the race is not being exercised, and
        // the guarded arm below would pass for that reason rather than because
        // the seqlock works -- a guard that cannot fail. So this arm must see
        // tears, and the test fails if it does not.
        std::atomic<bool> stop2{false};
        long torn_unguarded = 0;
        std::thread w2([&]{ for (int i = 1; i <= kIter && !stop2; i++) publish(i); });
        for (int i = 0; i < kIter; i++) {
            // The OLD read, reproduced: the readback first...
            x4vr::CamSnap c;
            c.raw_yaw_deg = s->cam_raw_yaw_deg.load(std::memory_order_relaxed);
            c.raw_pitch_deg = s->cam_raw_pitch_deg.load(std::memory_order_relaxed);
            c.raw_valid = s->cam_raw_valid.load(std::memory_order_relaxed) != 0;
            c.raw_ns = s->cam_raw_ns.load(std::memory_order_relaxed);
            c.valid = s->cam_valid.load(std::memory_order_relaxed) != 0;
            // ...then a frame of submission work...
            for (volatile int spin = 0; spin < 24; spin++) {}
            // ...and the command, loaded again much later.
            c.yaw_deg = s->cam_yaw_deg.load(std::memory_order_relaxed);
            c.pitch_deg = s->cam_pitch_deg.load(std::memory_order_relaxed);
            if (!consistent(c)) torn_unguarded++;
        }
        stop2 = true; w2.join();
        char buf[160];
        snprintf(buf, sizeof buf, "control: the OLD read tears (%ld of %d)",
                 torn_unguarded, kIter);
        ck(torn_unguarded > 0, buf);

        // Same writer, same contention, through the seqlock.
        std::atomic<bool> stop3{false};
        long torn_guarded = 0, got = 0;
        std::thread w3([&]{ for (int i = 1; i <= kIter && !stop3; i++) publish(i); });
        for (int i = 0; i < kIter; i++) {
            x4vr::CamSnap c;
            if (!x4vr::share_read_camera(s, &c)) continue;
            got++;
            for (volatile int spin = 0; spin < 24; spin++) {}
            if (!consistent(c)) torn_guarded++;
        }
        stop3 = true; w3.join();
        snprintf(buf, sizeof buf, "guarded: 0 torn of %ld completed", got);
        ck(torn_guarded == 0, buf);
        // **"Never tears" is worth nothing if it barely read.** The first
        // version of this arm refused 98% and asserted only on the survivors --
        // a pass bought by not looking. The writer here is as hot as a thread
        // can be, which is not the game, so the bar is only that enough reads
        // completed for the zero above to mean something.
        snprintf(buf, sizeof buf, "  ...and it completed enough to count (%ld)",
                 got);
        ck(got > 10000, buf);

        // **The cadence that actually ships.** The servo publishes at 90 Hz and
        // the layer reads once a frame, so the two are nowhere near this
        // contended -- and a refusal costs a frame served from the previous
        // snapshot. Refusals have to be rare at the real ratio, not merely
        // survivable at the worst one.
        std::atomic<bool> stop4{false};
        long refused_real = 0, got_real = 0, torn_real = 0;
        std::thread w4([&]{
            for (int i = 1; i <= kIter && !stop4; i++) {
                publish(i);
                for (volatile int spin = 0; spin < 2000; spin++) {}
            }
        });
        for (int i = 0; i < 200000; i++) {
            x4vr::CamSnap c;
            if (!x4vr::share_read_camera(s, &c)) { refused_real++; continue; }
            got_real++;
            if (!consistent(c)) torn_real++;
        }
        stop4 = true; w4.join();
        const double pct = 100.0 * (double)refused_real /
                           (double)(refused_real + got_real);
        snprintf(buf, sizeof buf,
                 "at a realistic write cadence: %.3f%% refused, %ld torn", pct,
                 torn_real);
        ck(pct < 1.0 && torn_real == 0, buf);
    }

    printf("\n%s\n", fails?"FAILURES":"all cases passed");
    return fails!=0;
}
