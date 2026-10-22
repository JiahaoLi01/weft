# The public View (docs/engine-communication.md section 8): one page, its VM and its scene, for hosts.
weft_add_library(view
    SOURCES view.cpp view_images.cpp view_keys.cpp view_text_field.cpp
    view_spatial_navigation.cpp
    PUBLIC weft::script weft::scene weft::bridge
    PRIVATE weft_lodepng)
if(WIN32)
    # The keyboard layout behind weft_keyboard_key (ToUnicodeEx).
    target_link_libraries(weft_view PRIVATE user32)
endif()
