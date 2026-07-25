// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_sbs.hpp — the one true side-by-side framebuffer size.
//
// The injector forces config.xml to it; the layer checks the swapchain
// against it and warns if they disagree. Both need the same number, so it
// lives here rather than being written out twice.
//
// Getting it exactly is not automatic. X4 only honours res_width/res_height
// when `borderless` is off, and then subtracts the window decoration from
// the height (observed: 1408 -> 1385). With `borderless` on it ignores them
// entirely and sizes to the display -- the same served config produced
// 2816x1408 under a 2816x1408 gamescope and 3440x1440 on a 3440x1440
// desktop. So gamescope at exactly this size is load-bearing, not a
// convenience: it is what makes "the display" and "what we asked for" the
// same thing. See injector/README.md.
//
// The width must stay even -- the SBS split is width/2 per eye.
#pragma once

#define X4VR_SBS_WIDTH 2816
#define X4VR_SBS_HEIGHT 1408

#define X4VR_STRINGIFY_(x) #x
#define X4VR_STRINGIFY(x) X4VR_STRINGIFY_(x)
#define X4VR_SBS_WIDTH_STR X4VR_STRINGIFY(X4VR_SBS_WIDTH)
#define X4VR_SBS_HEIGHT_STR X4VR_STRINGIFY(X4VR_SBS_HEIGHT)

static_assert(X4VR_SBS_WIDTH % 2 == 0, "SBS width must halve exactly");
