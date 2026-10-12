# Vulkan backend: the system loader opened at run time, one device with the validation layer when installed, and the
# display list renderer with the shared scene shader compiled to SPIR-V.
weft_add_library(render_vulkan
    SOURCES backend.cpp device.cpp pipeline.cpp renderer.cpp swapchain.cpp
    PUBLIC weft::render weft_vulkan_headers)
if(WIN32)
    # Surfaces of Win32 windows for the swapchain.
    target_sources(weft_render_vulkan PRIVATE surface_win32.cpp)
else()
    target_link_libraries(weft_render_vulkan PRIVATE ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
    # Surfaces of Android native windows for the swapchain.
    target_sources(weft_render_vulkan PRIVATE surface_android.cpp)
endif()
weft_add_spirv(weft_render_vulkan display_linear "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl"
    DEFINES SPACE_LINEAR=1)

# Native Vulkan pass of compiled scenes, prepared by the portable weft::scene_native: the shared scene shader's encoded
# pass compiled to SPIR-V.
weft_add_library(scene_vulkan
    SOURCES scene_renderer.cpp
    PUBLIC weft::render_vulkan weft::scene_native)
weft_add_spirv(weft_scene_vulkan scene_encoded "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl")
