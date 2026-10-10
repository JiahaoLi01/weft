#pragma once
#include "caseelephant/script_probe.hpp"

namespace caseelephant {
    // Experimental owner entry point, not stable C ABI.
    inline ProbeAnimationFrameResult run_animation_frame_probe(ScriptDomProbe& owner, double time_ms,
                                                               bool drawable = true) {
        return owner.animation_frame(time_ms, drawable);
    }
} // namespace caseelephant
