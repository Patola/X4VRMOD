// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The injector's hand-written SDL_MouseMotionEvent, checked against the real
// SDL3 headers.
//
// #33 synthesises relative mouse motion so the head can drive X4's free-look.
// The injector cannot include SDL headers: SDL2 (gamescope) and SDL3 (X4) share
// that process tree, and the injector's own hand declarations -- SDL_GetWindowSize
// and friends, deliberately taking `void *` for opaque types -- would collide
// with the real prototypes. So the event is laid out positionally there, the
// same way SDL_Surface already is.
//
// A positional layout is a silent liability: if a future SDL reorders a field
// or changes a width, the injector keeps compiling and starts steering the
// camera with whatever float now sits at offset 36. **This test is what makes
// that loud.** It has no such constraint -- it is a standalone binary that
// touches neither SDL2 nor the injector -- so it can include the real header
// and assert every offset at compile time.
//
// If this stops building, do not adjust the numbers until you have read the new
// SDL_MouseMotionEvent: the whole point is that the compiler noticed before a
// take did.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <SDL3/SDL_events.h>

// The declaration as the injector has it. Kept byte-identical to
// injector/x4vr_inject.cpp's SdlMouseMotion; if you change one, change both --
// which is exactly what the asserts below will make you do.
struct SdlMouseMotion {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint32_t state_or_buttons;
    float x, y;
    float xrel, yrel;
};

#define SAME_OFFSET(field)                                                     \
    static_assert(offsetof(SdlMouseMotion, field) ==                           \
                      offsetof(SDL_MouseMotionEvent, field),                   \
                  "SDL3 moved SDL_MouseMotionEvent::" #field                   \
                  " — the injector's positional layout is now wrong")

SAME_OFFSET(type);
SAME_OFFSET(reserved);
SAME_OFFSET(timestamp);
SAME_OFFSET(windowID);
SAME_OFFSET(which);
static_assert(offsetof(SdlMouseMotion, state_or_buttons) ==
                  offsetof(SDL_MouseMotionEvent, state),
              "SDL3 moved SDL_MouseMotionEvent::state");
SAME_OFFSET(x);
SAME_OFFSET(y);
SAME_OFFSET(xrel);
SAME_OFFSET(yrel);

static_assert(sizeof(SdlMouseMotion) == sizeof(SDL_MouseMotionEvent),
              "SDL3 changed the size of SDL_MouseMotionEvent");

// The injector strides an SDL_Event array by this when scanning SDL_PeepEvents
// output for a real motion event to copy windowID/which from. Too small and it
// reads a misaligned field from the middle of the next event; too large and it
// walks off the end of X4's buffer.
static_assert(sizeof(SDL_Event) == 128,
              "SDL_Event is no longer 128 bytes — kSdlEventSize in the "
              "injector strides an event array by that number");

// The event type the injector pushes. An enum value, so a rename would be
// caught by the compiler but a renumber would not.
static_assert((uint32_t)SDL_EVENT_MOUSE_MOTION == 0x400u,
              "SDL_EVENT_MOUSE_MOTION renumbered — kSdlEventMouseMotion in the "
              "injector is stale");

// The scancode the rebound free-look key sits on, which has to agree with
// x4vr::headlook_code()'s INPUT_KEYCODE_F13 on X4's side.
static_assert((int)SDL_SCANCODE_F13 == 104,
              "SDL_SCANCODE_F13 moved — headlook_scancode()'s default is stale");

int main() {
    printf("ok    SDL_MouseMotionEvent layout matches the injector's "
           "(%zu bytes, %zu fields checked)\n",
           sizeof(SDL_MouseMotionEvent), (size_t)10);
    printf("ok    SDL_Event stride %zu, SDL_EVENT_MOUSE_MOTION 0x%x, "
           "SDL_SCANCODE_F13 %d\n",
           sizeof(SDL_Event), (unsigned)SDL_EVENT_MOUSE_MOTION,
           (int)SDL_SCANCODE_F13);
    printf("\nall cases passed\n");
    return 0;
}
