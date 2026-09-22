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


// The keyboard event, same discipline. Take 118 forced this into existence: X4
// imports SDL_GetKeyboardState and never calls it, so the held free-look key is
// pushed as an event and its layout matters exactly as much as the mouse one's.
struct Sdl3KeyEvent {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint32_t scancode;
    uint32_t key;
    uint16_t mod;
    uint16_t raw;
    bool down;
    bool repeat;
};

#define SAME_KEY_OFFSET(field)                                                 \
    static_assert(offsetof(Sdl3KeyEvent, field) ==                             \
                      offsetof(SDL_KeyboardEvent, field),                      \
                  "SDL3 moved SDL_KeyboardEvent::" #field                      \
                  " — the injector's positional layout is now wrong")

SAME_KEY_OFFSET(type);
SAME_KEY_OFFSET(reserved);
SAME_KEY_OFFSET(timestamp);
SAME_KEY_OFFSET(windowID);
SAME_KEY_OFFSET(which);
SAME_KEY_OFFSET(scancode);
SAME_KEY_OFFSET(key);
SAME_KEY_OFFSET(mod);
SAME_KEY_OFFSET(raw);
SAME_KEY_OFFSET(down);
SAME_KEY_OFFSET(repeat);
static_assert(sizeof(Sdl3KeyEvent) == sizeof(SDL_KeyboardEvent),
              "SDL3 changed the size of SDL_KeyboardEvent");
static_assert((uint32_t)SDL_EVENT_KEY_DOWN == 0x300u,
              "SDL_EVENT_KEY_DOWN renumbered — SDL_EV_KEY_DOWN is stale");

int main() {
    printf("ok    SDL_MouseMotionEvent layout matches the injector's "
           "(%zu bytes, %zu fields checked)\n",
           sizeof(SDL_MouseMotionEvent), (size_t)10);
    printf("ok    SDL_Event stride %zu, SDL_EVENT_MOUSE_MOTION 0x%x, "
           "SDL_SCANCODE_F13 %d\n",
           sizeof(SDL_Event), (unsigned)SDL_EVENT_MOUSE_MOTION,
           (int)SDL_SCANCODE_F13);
    printf("ok    SDL_KeyboardEvent layout matches (%zu bytes), "
           "SDL_EVENT_KEY_DOWN 0x%x\n",
           sizeof(SDL_KeyboardEvent), (unsigned)SDL_EVENT_KEY_DOWN);
    printf("\nall cases passed\n");
    return 0;
}

// ---------------------------------------------------------------------------
// The joystick/gamepad axis event, same discipline. **Take 307 forced this
// into existence.** The pad census counted SDL_GetJoystickAxis and reported
// "OPENED, NEVER POLLED — no binding could have acted" while Patola was
// steering the ship with the pad perfectly well: X4 does not import
// SDL_GetGamepadAxis at all and called SDL_GetJoystickAxis zero times, because
// the values arrive as EVENTS through the drain. The injector now reads them
// positionally there, which is only safe if these offsets hold.
struct Sdl3AxisEvent {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t which;
    uint8_t axis;
    uint8_t padding1, padding2, padding3;
    int16_t value;
    uint16_t padding4;
};

#define AXIS_OFFSET(field)                                                     \
    static_assert(offsetof(Sdl3AxisEvent, field) ==                            \
                      offsetof(SDL_JoyAxisEvent, field),                       \
                  "SDL3 moved SDL_JoyAxisEvent::" #field                       \
                  " — the injector's positional layout is now wrong")

AXIS_OFFSET(type);
AXIS_OFFSET(reserved);
AXIS_OFFSET(timestamp);
AXIS_OFFSET(which);
AXIS_OFFSET(axis);
AXIS_OFFSET(value);
static_assert(sizeof(Sdl3AxisEvent) == sizeof(SDL_JoyAxisEvent),
              "SDL3 changed the size of SDL_JoyAxisEvent");

// The gamepad axis event is read through the SAME struct, so its agreeing with
// the joystick one is a precondition rather than a coincidence. If SDL ever
// splits them, the injector needs two readers and this is what says so.
static_assert(offsetof(SDL_GamepadAxisEvent, axis) ==
                  offsetof(SDL_JoyAxisEvent, axis),
              "SDL_GamepadAxisEvent::axis no longer matches SDL_JoyAxisEvent — "
              "pad_note_axis() reads both through one layout");
static_assert(offsetof(SDL_GamepadAxisEvent, value) ==
                  offsetof(SDL_JoyAxisEvent, value),
              "SDL_GamepadAxisEvent::value no longer matches SDL_JoyAxisEvent");
static_assert(sizeof(SDL_GamepadAxisEvent) == sizeof(SDL_JoyAxisEvent),
              "SDL_GamepadAxisEvent and SDL_JoyAxisEvent diverged in size");

// The type values the injector switches on. A renumber is exactly the failure
// an enum name cannot catch, and folding an axis count into a hotplug count is
// how a census comes to describe a device nobody moved.
static_assert((uint32_t)SDL_EVENT_JOYSTICK_AXIS_MOTION == 0x600u,
              "SDL_EVENT_JOYSTICK_AXIS_MOTION renumbered — SDL_EV_JOY_AXIS is stale");
static_assert((uint32_t)SDL_EVENT_JOYSTICK_BUTTON_DOWN == 0x603u,
              "SDL_EVENT_JOYSTICK_BUTTON_DOWN renumbered — SDL_EV_JOY_BUTTON_DOWN is stale");
static_assert((uint32_t)SDL_EVENT_JOYSTICK_ADDED == 0x605u,
              "SDL_EVENT_JOYSTICK_ADDED renumbered — SDL_EV_JOY_ADDED is stale");
static_assert((uint32_t)SDL_EVENT_GAMEPAD_AXIS_MOTION == 0x650u,
              "SDL_EVENT_GAMEPAD_AXIS_MOTION renumbered — SDL_EV_PAD_AXIS is stale");
static_assert((uint32_t)SDL_EVENT_GAMEPAD_BUTTON_DOWN == 0x651u,
              "SDL_EVENT_GAMEPAD_BUTTON_DOWN renumbered — SDL_EV_PAD_BUTTON_DOWN is stale");
static_assert((uint32_t)SDL_EVENT_GAMEPAD_ADDED == 0x653u,
              "SDL_EVENT_GAMEPAD_ADDED renumbered — SDL_EV_PAD_ADDED is stale");

// X4's steering is bound to INPUT_JOYAXIS_RX/RY and its strafe to X/Y, so the
// census names axis indices 0..5. That naming is only correct if SDL's gamepad
// axis enum still orders them the way the labels assume.
static_assert((int)SDL_GAMEPAD_AXIS_LEFTX == 0 && (int)SDL_GAMEPAD_AXIS_LEFTY == 1 &&
                  (int)SDL_GAMEPAD_AXIS_RIGHTX == 2 && (int)SDL_GAMEPAD_AXIS_RIGHTY == 3,
              "SDL gamepad axis order changed — pad_report()'s X/Y/Z/RX/RY/RZ "
              "labels describe RAW JOYSTICK indices and must not be read as "
              "gamepad ones; check which family the census line came from");
