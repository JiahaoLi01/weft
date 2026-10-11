# GPU resource retirement, the C render backend table, rounded-rect clip geometry, portable scene pass plans and
# the quad lowering of display lists.
weft_add_library(render
    SOURCES resource_lifetime.cpp render_backend.cpp rounded_clip.cpp scene_pass.cpp quad_lowering.cpp
    PUBLIC weft::base weft::paint)

if(WIN32)
    add_subdirectory(backends/d3d11)
endif()
