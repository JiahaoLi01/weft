# The in-game UI sample on macOS (Objective-C++; docs/ingame-ui.md): weft pages drawn into export textures
# (include/weft/render_export_texture.h) that a 3D scene samples on world-space panels, with the pointer ray-picked onto
# them; --shot draws frames off screen into a PNG, --bench measures. Public headers only (include/weft).
add_executable(weft_macos_ingame MACOSX_BUNDLE main.mm)
weft_project_options(weft_macos_ingame)
target_compile_options(weft_macos_ingame PRIVATE -fobjc-arc)
target_link_libraries(weft_macos_ingame PRIVATE weft::core weft::scene_metal
    "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics"
    "-framework Foundation")
set_target_properties(weft_macos_ingame PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    OUTPUT_NAME weft_macos_ingame)
# The sample off screen, as a test: the pages load without diagnostics, and a click aimed through the camera's ray at the
# terminal's +10 button reaches the game.
if(WEFT_BUILD_TESTS)
    weft_add_test(NAME ingame_sample_shot
        COMMAND weft_macos_ingame --shot "${CMAKE_BINARY_DIR}/artifacts/ingame-shot.png" TIMEOUT 120)
endif()
