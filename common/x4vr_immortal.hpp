// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_immortal.hpp — state that is never destroyed.
//
// Both of our libraries live inside someone else's process, and that process
// keeps calling into them after exit() has started: gamescope destroys its
// image views from its own exit-time destructors, and every libc call we
// interpose (close, fclose, mmap) can come from anyone's. A static object's
// destructor is registered when our library loads -- AFTER the host's -- so it
// runs FIRST, and every later call then reads a freed table. Take 323:
// x4vr_DestroyImageView's g_devices.at() found nothing in the destroyed map,
// threw, and the exception leaving a Vulkan entry point aborted gamescope.
//
// So nothing here has an exit-time destructor. `immortal<T>()` builds a T on
// storage that is never freed, zeroed first so it starts exactly as an object
// of static storage duration would (zero-initialised, then constructed):
//
//     std::unordered_map<K, V> &g_table = x4vr::immortal<std::unordered_map<K, V>>();
//
// The process reclaims the memory when it ends. tests/run-no-exit-destructors.sh
// fails the build's libraries if any static destructor is registered again.
#pragma once

#include <cstring>
#include <new>
#include <utility>

namespace x4vr {

// The reference never dangles, whatever temporaries built the object; GCC's
// -Wdangling-reference cannot know that without being told.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#define X4VR_NO_DANGLING [[gnu::no_dangling]]
#else
#define X4VR_NO_DANGLING
#endif

template <class T, class... Args> X4VR_NO_DANGLING T &immortal(Args &&...args) {
    void *p = ::operator new(sizeof(T), std::align_val_t(alignof(T)));
    std::memset(p, 0, sizeof(T));
    return *::new (p) T(std::forward<Args>(args)...);
}

} // namespace x4vr
