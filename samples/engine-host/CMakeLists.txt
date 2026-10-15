# A game engine's side of Weft through the public headers only: the HUD page of examples/engine-host driven by anchors,
# events, a view model and a call, drawn over the game on D3D11, D3D12 or Vulkan. The tests run it on WARP (D3D11 and
# D3D12) and, when this machine has one, on its Vulkan device (skipped, 77, without).
add_executable(weft_engine_host engine_host.cpp)
weft_project_options(weft_engine_host)
target_link_libraries(weft_engine_host PRIVATE weft::core weft::scene_d3d11 weft::scene_d3d12 weft::scene_vulkan
    weft_vulkan_headers d3d11 d3d12 dxgi)
target_compile_definitions(weft_engine_host PRIVATE WEFT_SAMPLE_PAGE="${PROJECT_SOURCE_DIR}/examples/engine-host")
if(MSVC)
    # The View's owner thread: 8 MiB of stack (include/weft/weft.h).
    target_link_options(weft_engine_host PRIVATE /STACK:8388608)
endif()
if(WEFT_BUILD_TESTS)
    set(samples_out "${CMAKE_BINARY_DIR}/artifacts/engine-host-$<CONFIG>")
    weft_add_test(NAME engine_host_d3d11_warp
        COMMAND weft_engine_host --d3d11 --warp --frames 90 --out "${samples_out}/d3d11-warp.bmp" TIMEOUT 120)
    weft_add_test(NAME engine_host_d3d12_warp
        COMMAND weft_engine_host --d3d12 --warp --frames 90 --out "${samples_out}/d3d12-warp.bmp" TIMEOUT 120)
    set_tests_properties(engine_host_d3d12_warp PROPERTIES SKIP_RETURN_CODE 77)
    weft_add_test(NAME engine_host_vulkan
        COMMAND weft_engine_host --vulkan --frames 90 --out "${samples_out}/vulkan.bmp" TIMEOUT 120)
    set_tests_properties(engine_host_vulkan PROPERTIES SKIP_RETURN_CODE 77)
endif()
