# D3D11 reference renderer, its C ABI adapter, and the synchronous and two-frame sample hosts.
weft_add_library(render_d3d11
    SOURCES renderer.cpp backend.cpp two_frame_pipeline.cpp
    PUBLIC weft::render d3d11
    PRIVATE dxgi)
weft_add_hlsl(weft_render_d3d11 srgb_convert srgb_convert.hlsl)
weft_add_hlsl(weft_render_d3d11 encoded_convert srgb_convert.hlsl DEFINES ENCODED_PREMULTIPLIED=1)
# The linear pass of display lists: the shared scene shader compositing linear premultiplied colors.
weft_add_hlsl(weft_render_d3d11 display_linear "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl" ALWAYS_OPTIMIZE DEFINES SPACE_LINEAR=1)

# Native D3D11 pass of compiled scenes, prepared by the portable weft::scene_native; the D2D reference raster draws
# only text whose color glyphs are images.
weft_add_library(scene_d3d11
    SOURCES scene_renderer.cpp
    PUBLIC weft::render_d3d11 weft::scene_d2d weft::scene_native)
weft_add_hlsl(weft_scene_d3d11 scene_native "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl" ALWAYS_OPTIMIZE)
# Its backdrop passes: a fullscreen triangle with the area downsample, the Gaussian pass and the color matrix.
foreach(variant AREA BLUR MATRIX)
    string(TOLOWER "${variant}" name)
    weft_add_hlsl(weft_scene_d3d11 backdrop_${name} "${PROJECT_SOURCE_DIR}/src/render/shaders/backdrop.hlsl"
        ALWAYS_OPTIMIZE DEFINES BACKDROP_${variant}=1)
endforeach()
# Its particle pass: both stages of the same shader's WEFT_PARTICLES variant.
weft_add_hlsl(weft_scene_d3d11 scene_particles "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl" ALWAYS_OPTIMIZE
    DEFINES WEFT_PARTICLES=1)
