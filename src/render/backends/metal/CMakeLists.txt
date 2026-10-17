# Metal backend (docs/render-backend-api.md, "Metal"): the shared shaders as MSL (cmake/WeftMetal.cmake), text the
# device compiles, built and checked wherever the translation runs. The Metal device and its pass need Apple's Metal
# and are not built yet.
weft_add_library(render_metal
    SOURCES shaders.cpp)
# The shared scene shader's encoded pass.
weft_add_msl(weft_render_metal scene_encoded "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl")
# Its backdrop passes: a fullscreen triangle with the area downsample, the Gaussian pass and the color matrix.
foreach(variant AREA BLUR MATRIX)
    string(TOLOWER "${variant}" name)
    weft_add_msl(weft_render_metal backdrop_${name} "${PROJECT_SOURCE_DIR}/src/render/shaders/backdrop.hlsl"
        DEFINES BACKDROP_${variant}=1)
endforeach()
# Its particle pass: both stages of the same shader's WEFT_PARTICLES variant.
weft_add_msl(weft_render_metal scene_particles_encoded "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl"
    DEFINES WEFT_PARTICLES=1)
