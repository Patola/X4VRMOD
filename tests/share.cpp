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
    printf("\n%s\n", fails?"FAILURES":"all cases passed");
    return fails!=0;
}
