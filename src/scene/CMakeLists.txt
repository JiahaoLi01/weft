# Scene tree, paint, damage and native geometry.
weft_add_library(scene
    SOURCES scene.cpp scene_inline.cpp scene_border.cpp scene_paint.cpp scene_transform.cpp scene_damage.cpp scene_display_list.cpp scene_text.cpp scene_text_image.cpp scene_geometry.cpp scene_path.cpp scene_effects.cpp scene_particles.cpp scene_filter.cpp scene_anchor.cpp scene_gradient.cpp scene_background.cpp scene_border_image.cpp
    PUBLIC weft::style weft::text weft::paint
    PRIVATE weft::layout)

# The backend-neutral half of native scene passes: display lists, their plans, the images they sample, and the
# commands that record them on backends with render passes.
weft_add_library(scene_native
    SOURCES scene_native.cpp scene_schedule.cpp
    PUBLIC weft::scene weft::render weft::vector)

if(WIN32)
    # Direct2D/DirectWrite reference raster for scenes.
    weft_add_library(scene_d2d
        SOURCES scene_raster_d2d.cpp
        PUBLIC weft::scene
        PRIVATE weft::render d2d1 dwrite windowscodecs ole32)
endif()
