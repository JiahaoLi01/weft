# Scene tree, paint, damage and native geometry.
weft_add_library(scene
    SOURCES scene.cpp scene_paint.cpp scene_transform.cpp scene_damage.cpp scene_display_list.cpp scene_text.cpp scene_text_image.cpp scene_geometry.cpp scene_path.cpp
    PUBLIC weft::style weft::text weft::paint
    PRIVATE weft::layout)

if(WIN32)
    # Direct2D/DirectWrite reference raster for scenes.
    weft_add_library(scene_d2d
        SOURCES scene_raster_d2d.cpp
        PUBLIC weft::scene
        PRIVATE weft::render d2d1 dwrite windowscodecs ole32)
endif()
