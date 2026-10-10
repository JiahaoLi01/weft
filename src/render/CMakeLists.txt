# GPU resource retirement, the C render backend table and rounded-rect clip geometry.
weft_add_library(render
    SOURCES resource_lifetime.cpp render_backend.cpp rounded_clip.cpp
    PUBLIC weft::base)
