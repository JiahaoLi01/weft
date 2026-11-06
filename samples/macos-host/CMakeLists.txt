# The engine host sample on macOS (Objective-C++): the HUD page of examples/engine-host in a window, over the engine's
# picture of its world, drawn by weft's Metal backend into a CAMetalLayer; --shot draws frames off screen into a PNG.
# Public headers only (include/weft).
add_executable(weft_macos_demo MACOSX_BUNDLE main.mm)
weft_project_options(weft_macos_demo)
target_compile_options(weft_macos_demo PRIVATE -fobjc-arc)
target_compile_definitions(weft_macos_demo PRIVATE WEFT_EXAMPLES="${PROJECT_SOURCE_DIR}/examples")
target_link_libraries(weft_macos_demo PRIVATE weft::core weft::view weft::scene_metal
    "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics"
    "-framework Foundation")
set_target_properties(weft_macos_demo PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    OUTPUT_NAME weft_macos_demo)
