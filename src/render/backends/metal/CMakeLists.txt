# Metal backend (docs/render-backend-api.md, "Metal"): the shared shaders as MSL (cmake/WeftMetal.cmake), text the
# device compiles, built and checked wherever the translation runs; on Apple platforms, the device side (M.3).
weft_add_library(render_metal
    SOURCES shaders.cpp
    PUBLIC weft::render)
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

# The device side (M.3), on Apple platforms: the native scene pass that runs the portable schedule
# (scene/scene_schedule.hpp) with Metal encoders, and the View backend of weft_render_metal_create
# (include/weft/render_backend_metal.h). Objective-C++ with ARC.
if(APPLE)
    weft_add_library(scene_metal
        SOURCES scene_renderer.mm scene_record.mm scene_encode.mm view_backend.mm
        PUBLIC weft::render_metal weft::scene_native
        PRIVATE weft::view "-framework Metal" "-framework Foundation")
    target_compile_options(weft_scene_metal PRIVATE -fobjc-arc)
endif()
