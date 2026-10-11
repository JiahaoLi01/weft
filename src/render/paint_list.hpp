#pragma once

#include "paint/display_list.hpp"
#include "weft/render_backend.h"

#include <memory>

// The renderer C ABI's opaque display list (weft_paint_list, include/weft/render_backend.h): one immutable list that
// its producer and every frame prepared from it share.
struct weft_paint_list {
    std::shared_ptr<const weft::paint::DisplayList> list;
};
