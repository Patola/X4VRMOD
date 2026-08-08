// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// One reading of a knob, shared by the layer and the injector.
//
// This exists because of a specific hazard rather than for tidiness. The
// injector decides whether to hide the compositor's pointer by asking whether
// *the layer* is going to draw one, and it answers that from the same
// environment the layer reads -- `X4VR_SBS`, `X4VR_SBS_SPLIT`, `X4VR_CURSOR`.
// Two components deriving the same fact from the same variables is exactly the
// shape that drifts: one of them grows a `!= "0"` where the other has a
// `== "1"`, and a configuration that means "on" over here means "off" over
// there. Reading a list positionally and first-matching an aliased binding are
// the same class of mistake, and both have cost this project runs.
//
// So the truthiness rule lives in one place and both callers use it.
//
//     unset or empty  -> the caller's default
//     leading '0'     -> off
//     anything else   -> on
//
// That is the rule the layer's knobs already used, written down instead of
// re-typed.
#pragma once

#include <cstdlib>

namespace x4vr {

inline bool env_on(const char *name, bool dflt) {
    const char *e = getenv(name);
    if (!e || !*e)
        return dflt;
    return *e != '0';
}

} // namespace x4vr
