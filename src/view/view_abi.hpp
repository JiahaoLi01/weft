#pragma once

#include "view/view.hpp"

#include <memory>

// The opaque frame of the C ABIs (weft_view_frame of include/weft/weft.h and render_backend.h): a View's published
// frame, held for as long as the host holds the handle.
struct weft_view_frame {
    std::shared_ptr<const weft::ViewFrame> frame;
};
