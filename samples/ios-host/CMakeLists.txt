# The engine host sample on iOS (Objective-C++): samples/macos-host's engine and Metal side (engine.hpp, gpu.hpp,
# page.hpp) in a UIKit app, drawn by weft's Metal backend into a CAMetalLayer at the display's rate; a touch is the
# pointer. The HUD page of examples/engine-host and the logo pages of examples/logo travel in the bundle (a device
# cannot read the source tree). Public headers only (include/weft).
#
# The simulator builds with Ninja (CMAKE_OSX_SYSROOT=iphonesimulator); a device needs a signed app, which the Xcode
# generator makes with WEFT_IOS_DEVELOPMENT_TEAM (docs/building.md).
set(WEFT_IOS_BUNDLE_ID "dev.weft.ios-demo" CACHE STRING
    "Bundle identifier of weft_ios_demo (a signing team's own; a free team cannot take one another team holds)")
set(WEFT_IOS_DEVELOPMENT_TEAM "" CACHE STRING "Team ID that signs weft_ios_demo for a device (Xcode generator)")

add_executable(weft_ios_demo MACOSX_BUNDLE main.mm)
weft_project_options(weft_ios_demo)
target_compile_options(weft_ios_demo PRIVATE -fobjc-arc)
target_include_directories(weft_ios_demo PRIVATE "${PROJECT_SOURCE_DIR}/samples/macos-host")
target_link_libraries(weft_ios_demo PRIVATE weft::core weft::view weft::scene_metal
    "-framework UIKit" "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics" "-framework Foundation")

# The pages (the HUD, the logo variants and the touch page), at examples/<folder> in the bundle, as the source tree has them (the logo variants' page.json name the
# shared script and stylesheets one folder up).
foreach(folder engine-host logo touch bidi img effects animate)
    file(GLOB_RECURSE pages CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}/examples"
        "${PROJECT_SOURCE_DIR}/examples/${folder}/*.js" "${PROJECT_SOURCE_DIR}/examples/${folder}/*.css"
        "${PROJECT_SOURCE_DIR}/examples/${folder}/*.json" "${PROJECT_SOURCE_DIR}/examples/${folder}/*.png"
        "${PROJECT_SOURCE_DIR}/examples/${folder}/*.jpg" "${PROJECT_SOURCE_DIR}/examples/${folder}/*.webp")
    foreach(page ${pages})
        get_filename_component(directory "${page}" DIRECTORY)
        target_sources(weft_ios_demo PRIVATE "${PROJECT_SOURCE_DIR}/examples/${page}")
        set_source_files_properties("${PROJECT_SOURCE_DIR}/examples/${page}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "examples/${directory}")
    endforeach()
endforeach()

# The animate page checks Web Animations against Chrome with the steps of the test's fixture, which its page.json names
# two folders up: the same place in the bundle as in the source tree.
target_sources(weft_ios_demo PRIVATE "${PROJECT_SOURCE_DIR}/tests/fixtures/web_animations/animations.js")
set_source_files_properties("${PROJECT_SOURCE_DIR}/tests/fixtures/web_animations/animations.js" PROPERTIES
    MACOSX_PACKAGE_LOCATION "tests/fixtures/web_animations")

set_target_properties(weft_ios_demo PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${WEFT_IOS_BUNDLE_ID}"
    OUTPUT_NAME weft_ios_demo
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${WEFT_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(WEFT_IOS_DEVELOPMENT_TEAM)
    set_target_properties(weft_ios_demo PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${WEFT_IOS_DEVELOPMENT_TEAM}"
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development")
endif()
