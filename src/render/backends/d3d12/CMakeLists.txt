# Native D3D12 pass of View frames, prepared by the portable weft::scene_native, and its C ABI adapter: the shared scene
# shader's encoded pass as the DXBC the D3D11 pass runs, bound through root signatures of its own; the D2D reference
# raster draws only text whose color glyphs are images.
weft_add_library(scene_d3d12
    SOURCES device.cpp scene_renderer.cpp view_backend.cpp
    PUBLIC weft::render weft::scene_native d3d12
    PRIVATE weft::scene_d2d weft::view ole32)
weft_add_hlsl(weft_scene_d3d12 d3d12_scene "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl" ALWAYS_OPTIMIZE)
# Its backdrop passes: a fullscreen triangle with the area downsample, the Gaussian pass and the color matrix.
foreach(variant AREA BLUR MATRIX)
    string(TOLOWER "${variant}" name)
    weft_add_hlsl(weft_scene_d3d12 d3d12_backdrop_${name} "${PROJECT_SOURCE_DIR}/src/render/shaders/backdrop.hlsl"
        ALWAYS_OPTIMIZE DEFINES BACKDROP_${variant}=1)
endforeach()
# Its particle pass: both stages of the same shader's WEFT_PARTICLES variant.
weft_add_hlsl(weft_scene_d3d12 d3d12_scene_particles "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl"
    ALWAYS_OPTIMIZE DEFINES WEFT_PARTICLES=1)
