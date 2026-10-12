# GPU resource retirement, the C render backend table, rounded-rect clip geometry, portable scene pass plans, the
# display lists of quad frames, the texture cache and glyph atlas policy every renderer shares, and box-shadow
# images.
weft_add_library(render
    SOURCES resource_lifetime.cpp render_backend.cpp rounded_clip.cpp scene_pass.cpp quad_list.cpp atlas_packer.cpp
        texture_cache.cpp shadow_image.cpp
    PUBLIC weft::base weft::paint
    PRIVATE weft::vector)

if(WIN32)
    add_subdirectory(backends/d3d11)
endif()
if(WEFT_RENDER_VULKAN)
    add_subdirectory(backends/vulkan)
endif()
