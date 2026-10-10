#pragma once
#include "caseelephant/script_probe.hpp"

namespace weft {
    // Experimental owner entry point, not stable C ABI.
    inline AnimationFrameResult run_animation_frame(ScriptDom& owner, double time_ms, bool drawable = true) {
        return owner.animation_frame(time_ms, drawable);
    }
} // namespace weft
