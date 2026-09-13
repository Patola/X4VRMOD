// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// **Task #75: does SDL_PushEvent preserve a timestamp we set ourselves?**
//
// The injector stamps `timestamp` on every synthetic event before pushing it,
// and the drain hook subtracts it to measure how long the command sat in SDL's
// queue. That number is the one quantity the servo is blind to — it commands
// again each tick with no knowledge that the last command has not landed — so
// the whole measurement rests on SDL keeping the value.
//
// SDL3's documented behaviour is to fill `timestamp` only when it is zero. This
// asserts it against the real library rather than the documentation, because a
// version that overwrote it would leave the injector reporting a latency of
// roughly zero with no other symptom: a plausible number, silently wrong, which
// is the worst way for an instrument to fail.
//
// It also pins the two properties the census depends on besides that: that our
// marker field survives the round trip, and that the queue is FIFO, so "several
// of ours in one drain" means a real backlog rather than reordering.
#include <cstdio>
#include <cstdint>

#include <SDL3/SDL.h>

static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        g_fail++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}

int main() {
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        printf("SKIP  SDL_Init(SDL_INIT_EVENTS) failed: %s\n", SDL_GetError());
        return 0; // no display/session is not a failure of this assertion
    }
    printf("      SDL %d.%d.%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_MICRO_VERSION);

    const uint32_t kMagic = 0x52565834u; // kOurEventMagic
    const uint64_t kStamp = 1234567890123ull;

    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    // ---- a NON-ZERO timestamp must survive the push
    {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.common.timestamp = kStamp;
        e.motion.xrel = 3.0f;
        e.motion.yrel = -2.0f;
        e.motion.reserved = kMagic;
        check(SDL_PushEvent(&e), "the event was accepted");

        SDL_Event got{};
        const int n = SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                                     SDL_EVENT_LAST);
        check(n == 1, "and came back out");
        if (n == 1) {
            check(got.common.timestamp == kStamp,
                  "SDL PRESERVES a non-zero timestamp — the queue-time "
                  "measurement is valid");
            if (got.common.timestamp != kStamp)
                printf("      wanted %llu, got %llu — the injector's `evflow` "
                       "queue time would be meaningless\n",
                       (unsigned long long)kStamp,
                       (unsigned long long)got.common.timestamp);
            check(got.motion.reserved == kMagic,
                  "and the marker field survives, so ours stay identifiable");
            check(got.motion.xrel == 3.0f && got.motion.yrel == -2.0f,
                  "as does the payload");
        }
    }

    // ---- a ZERO timestamp must be filled in, or ours would be ambiguous
    {
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.common.timestamp = 0;
        SDL_PushEvent(&e);
        SDL_Event got{};
        if (SDL_PeepEvents(&got, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                           SDL_EVENT_LAST) == 1)
            check(got.common.timestamp != 0,
                  "a zero timestamp is filled in by SDL, so zero cannot be "
                  "mistaken for one of ours");
    }

    // ---- FIFO, so a multi-event drain is a BACKLOG and not reordering
    {
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        for (int i = 0; i < 8; i++) {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.common.timestamp = kStamp + (uint64_t)i;
            e.motion.reserved = kMagic;
            SDL_PushEvent(&e);
        }
        SDL_Event got[16]{};
        const int n = SDL_PeepEvents(got, 16, SDL_GETEVENT, SDL_EVENT_FIRST,
                                     SDL_EVENT_LAST);
        check(n == 8, "all eight queued events come back in one drain");
        bool ordered = (n == 8);
        for (int i = 0; i < n; i++)
            if (got[i].common.timestamp != kStamp + (uint64_t)i)
                ordered = false;
        check(ordered,
              "the queue is FIFO — several of ours in one drain is a real "
              "backlog, not reordering");
        printf("      one SDL_PeepEvents(GET) returned %d event(s), which is "
               "how the injector's burst histogram is populated\n", n);
    }

    SDL_Quit();
    printf(g_fail ? "\n%d case(s) FAILED\n" : "\nall cases passed\n", g_fail);
    return g_fail ? 1 : 0;
}
