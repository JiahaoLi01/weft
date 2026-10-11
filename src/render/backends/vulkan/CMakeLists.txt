# Vulkan backend: the system loader opened at run time, one device with the validation layer when installed, and the
# display list renderer with the shared scene shader compiled to SPIR-V.
weft_add_library(render_vulkan
    SOURCES backend.cpp device.cpp renderer.cpp
    PUBLIC weft::render weft_vulkan_headers)
if(NOT WIN32)
    target_link_libraries(weft_render_vulkan PRIVATE ${CMAKE_DL_LIBS})
endif()
weft_add_spirv(weft_render_vulkan display_linear "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl"
    DEFINES SPACE_LINEAR=1)
