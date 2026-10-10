#pragma once

#include "probe_common.hpp"
#include "gallery_cluster_input.hpp"
#include "gallery_constants.hpp"

namespace probes {
    // Owner CPU profile: original libraries and native input dispatch, fixed 60 Hz
    // clock. No GPU/readback, no sleep. This isolates UI updates from Present FPS.
} // namespace probes
