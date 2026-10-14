# Architecture probes. Their WARP and browser-diff runs are registered as tests when WEFT_BUILD_TESTS is on.
set(artifacts "${CMAKE_BINARY_DIR}/artifacts")
set(fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures")

if(WIN32)
    # main and the color/SDF/React fixtures in d3d11_probe.cpp; one translation unit per probe mode family.
    weft_add_executable(weft_d3d11_probe
        SOURCES d3d11_probe.cpp
        cheetah_alert_probe.cpp cheetah_flow_window_probe.cpp cheetah_probe.cpp complex_css_probe.cpp
        external_texture_probe.cpp flow_probe.cpp gallery_canvas_probe.cpp gallery_cluster_gpu_probe.cpp
        gallery_interaction_profile.cpp gallery_preview_probe.cpp react_window_host.cpp render_backend_probe.cpp
        render_cache_probe.cpp render_stages_probe.cpp sampling_profiler.cpp scene_gpu_surface_probe.cpp
        scene_surface_retirement_probe.cpp two_frame_probe.cpp particle_benchmark.cpp particle_page_benchmark.cpp
        glass_page_benchmark.cpp scene_interaction_benchmark.cpp hud_benchmark.cpp
        LINK weft::render_d3d11 weft::scene_d3d11 weft::text weft::script weft::layout weft::input weft::canvas_d2d weft::scene_d2d weft::platform_win32
        d3d11 dxgi windowscodecs ole32 user32 dbghelp
        SOURCE_DIR LARGE_STACK GC_STRESS)
    if(WEFT_RENDER_VULKAN)
        # The CaseCheetah flow window (vulkan_flow_window_probe.cpp) and the native scene pass (scene_vulkan_probe.cpp)
        # on Vulkan.
        target_sources(weft_d3d11_probe PRIVATE vulkan_flow_window_probe.cpp scene_vulkan_probe.cpp
            scene_vulkan_window.cpp)
        target_link_libraries(weft_d3d11_probe PRIVATE weft::render_vulkan weft::scene_vulkan)
        target_compile_definitions(weft_d3d11_probe PRIVATE WEFT_PROBE_VULKAN=1)
    endif()
    # UI shader effects the gallery's pages name (probes/gallery_effects.hpp): the CRPG UI's smoke background, for the
    # nested tooltip page (examples/crpg-gallery). The initial values are the original's DEFAULT_SMOKE_PARAMS, its
    # 48 x 64 canvas and its common rarity color.
    add_library(weft_gallery_effects STATIC)
    weft_project_options(weft_gallery_effects)
    target_link_libraries(weft_gallery_effects PUBLIC weft::paint)
    target_include_directories(weft_gallery_effects PUBLIC "${CMAKE_BINARY_DIR}/generated")
    weft_add_effect(weft_gallery_effects crpg_smoke "${PROJECT_SOURCE_DIR}/examples/crpg-gallery/effects/smoke.hlsl"
        PARAMETERS smoke-scales:vec4=7.2,2.8,0.1,0.3 smoke-speeds:vec4=1,1,-6,-10 smoke-shape:vec3=2,0,0
            smoke-canvas:vec2=48,64 "smoke-color:color=#9ca3af" smoke-start:number=0
        TEXTURES cloud noise ANIMATED)
    target_link_libraries(weft_d3d11_probe PRIVATE weft_gallery_effects)
endif()

if(WIN32 AND WEFT_BUILD_TESTS)
    if(WEFT_RENDER_VULKAN)
        # The 4K flow window on this machine's Vulkan device: React input, wheel scrolling, the letterbox, resize, the
        # pixel oracle and retirement; skipped (77) without a Vulkan device that presents.
        weft_add_test(NAME vulkan_cheetah_flow_window_4k
            COMMAND weft_d3d11_probe --cheetah-flow-window-test-4k-vulkan
            "${artifacts}/cheetah-flow-window-vulkan-$<CONFIG>/hardware" TIMEOUT 180)
        set_tests_properties(vulkan_cheetah_flow_window_4k PROPERTIES SKIP_RETURN_CODE 77)
        # The gallery's scenes through the native Vulkan scene pass on this machine's device, against the D3D11 native
        # pass on WARP and the Chromium references; skipped (77) without a Vulkan device.
        weft_add_test(NAME vulkan_scene_native
            COMMAND weft_d3d11_probe --scene-vulkan-test "${artifacts}/scene-vulkan-$<CONFIG>/hardware" TIMEOUT 300)
        set_tests_properties(vulkan_scene_native PROPERTIES SKIP_RETURN_CODE 77)
        # The gallery's scenes in a Vulkan swapchain window: every scene presented as draw() draws it; skipped (77)
        # without a Vulkan device that presents.
        weft_add_test(NAME vulkan_scene_window
            COMMAND weft_d3d11_probe --scene-vulkan-window-test "${artifacts}/scene-vulkan-window-$<CONFIG>/hardware"
            TIMEOUT 300)
        set_tests_properties(vulkan_scene_window PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # weft_add_d3d11_test(<test> <mode> <artifact dir> [weft_add_test options...]) runs one WARP mode.
    function(weft_add_d3d11_test name mode directory)
        weft_add_test(NAME ${name} COMMAND weft_d3d11_probe ${mode} "${artifacts}/${directory}" ${ARGN})
    endfunction()

    # weft_add_d3d11_browser_diff(<test> <script> <suite> <fixture>) compares <suite>-<config>/warp/report.json with
    # tests/fixtures/<suite>_browser_reference.json (hyphens become underscores).
    function(weft_add_d3d11_browser_diff name script suite fixture)
        string(REPLACE "-" "_" reference "${suite}")
        weft_add_python_test(NAME ${name}
            SCRIPT tools/${script}
            ARGS "${fixtures}/${reference}_browser_reference.json" "${artifacts}/${suite}-$<CONFIG>/warp/report.json"
            --output "${artifacts}/${suite}-$<CONFIG>/warp/browser-comparison.json"
            FIXTURES_REQUIRED ${fixture})
    endfunction()

    weft_add_d3d11_test(d3d11_warp_owner_async --owner-async-test-warp owner-async-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_scene_native --scene-native-test-warp scene-native-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_render_stages --render-stages-test-warp render-stages-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_complex_css --complex-css-test-warp complex-css-$<CONFIG>/warp TIMEOUT 60)
    weft_add_d3d11_test(d3d11_warp_scene_surface_retirement --scene-surface-retirement-warp
        scene-surface-retirement-$<CONFIG>/warp TIMEOUT 180)
    # The particle benchmarks' 4K frames, briefly: that they run (the hardware runs measure, by hand).
    weft_add_d3d11_test(d3d11_warp_particle_benchmark --particles-benchmark-warp particles-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_particle_page_benchmark --particle-page-benchmark-warp
        particle-page-benchmark-$<CONFIG>/warp TIMEOUT 300)
    weft_add_d3d11_test(d3d11_warp_glass_page_benchmark --glass-page-benchmark-warp
        glass-page-benchmark-$<CONFIG>/warp TIMEOUT 300)
    weft_add_d3d11_test(d3d11_warp_scene_interaction_benchmark --scene-interaction-benchmark-warp
        scene-interaction-benchmark-$<CONFIG>/warp TIMEOUT 300)
    weft_add_d3d11_test(d3d11_warp_hud_benchmark --hud-benchmark-warp hud-benchmark-$<CONFIG>/warp TIMEOUT 300)
    weft_add_d3d11_test(d3d11_warp_gallery_cluster --gallery-cluster-gpu-warp gallery-cluster-$<CONFIG>/warp TIMEOUT 300)
    weft_add_d3d11_test(d3d11_warp_scene_gpu --scene-gpu-test-warp scene-gpu-$<CONFIG>/warp
        TIMEOUT 240 GC_STRESS_TIMEOUT 600)
    weft_add_d3d11_test(d3d11_warp_external_texture --external-texture-warp external-texture-$<CONFIG>/warp TIMEOUT 60)
    weft_add_d3d11_test(d3d11_warp_gallery_preview --gallery-window-test-warp gallery-preview-$<CONFIG>/warp
        TIMEOUT 240 GC_STRESS_TIMEOUT 600)
    weft_add_d3d11_test(d3d11_warp_gallery_canvas_window --gallery-canvas-window-test-warp
        gallery-canvas-$<CONFIG>/window-warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_gallery_canvas --gallery-canvas-warp gallery-canvas-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_two_frame --pipeline-warp two-frame-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_render_cache --cache-warp render-cache-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_flow --flow-warp flow-$<CONFIG>/warp TIMEOUT 60)
    weft_add_d3d11_test(d3d11_warp_cheetah_flow_window --cheetah-flow-window-test-warp
        cheetah-flow-window-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_cheetah_flow_window_4k --cheetah-flow-window-test-4k-warp
        cheetah-flow-window-4k-$<CONFIG>/warp TIMEOUT 180)
    weft_add_d3d11_test(d3d11_warp_cheetah_flow_window_motion --cheetah-flow-window-test-4k-motion-warp
        cheetah-flow-window-motion-$<CONFIG>/warp TIMEOUT 180)

    weft_add_d3d11_test(d3d11_warp_cheetah_flow --cheetah-flow-warp cheetah-flow-$<CONFIG>/warp
        TIMEOUT 180 FIXTURES_SETUP cheetah_flow_report)
    weft_add_d3d11_browser_diff(cheetah_flow_browser_diff compare_style_reports.py cheetah-flow cheetah_flow_report)

    weft_add_d3d11_test(d3d11_warp_cheetah_style --cheetah-style-warp cheetah-style-$<CONFIG>/warp
        TIMEOUT 60 GC_STRESS_TIMEOUT 180 FIXTURES_SETUP cheetah_style_report)
    weft_add_d3d11_browser_diff(cheetah_style_browser_diff compare_style_reports.py cheetah-style cheetah_style_report)

    weft_add_d3d11_test(d3d11_warp_cheetah_auto --cheetah-auto-warp cheetah-auto-$<CONFIG>/warp
        TIMEOUT 180 FIXTURES_SETUP cheetah_auto_report)
    weft_add_d3d11_browser_diff(cheetah_auto_browser_diff compare_style_reports.py cheetah-auto cheetah_auto_report)

    weft_add_d3d11_test(d3d11_warp_cheetah_alert --cheetah-alert-warp cheetah-alert-$<CONFIG>/warp
        TIMEOUT 60 GC_STRESS_TIMEOUT 180 FIXTURES_SETUP cheetah_alert_report)
    weft_add_d3d11_browser_diff(cheetah_alert_browser_diff compare_cheetah_alert.py cheetah-alert cheetah_alert_report)

    weft_add_d3d11_test(d3d11_warp_cheetah --cheetah-warp cheetah-$<CONFIG>/warp
        TIMEOUT 60 GC_STRESS_TIMEOUT 180 FIXTURES_SETUP cheetah_report)
    weft_add_d3d11_browser_diff(cheetah_browser_diff compare_cheetah_reports.py cheetah cheetah_report)

    weft_add_d3d11_test(d3d11_warp_backend_api --backend-warp backend-$<CONFIG>/warp TIMEOUT 30)
    weft_add_d3d11_test(d3d11_warp_sdf --sdf-warp sdf-$<CONFIG>/warp TIMEOUT 30)
    weft_add_d3d11_test(d3d11_warp_window --react-window-test-warp window-$<CONFIG>/warp
        TIMEOUT 30 GC_STRESS_TIMEOUT 180)

    weft_add_d3d11_test(d3d11_warp_react --react-warp react-$<CONFIG>/warp
        TIMEOUT 30 GC_STRESS_TIMEOUT 180 FIXTURES_SETUP react_report)
    weft_add_d3d11_browser_diff(react_browser_diff compare_react_reports.py react react_report)

    weft_add_d3d11_test(d3d11_warp_color_lifetime --warp gpu-$<CONFIG>/warp TIMEOUT 30 FIXTURES_SETUP gpu_report)
    weft_add_python_test(NAME d3d11_browser_color_contract
        SCRIPT tools/compare_gpu_reports.py
        ARGS "${fixtures}/gpu_color_browser_reference.json" "${artifacts}/gpu-$<CONFIG>/warp/report.json"
        --output "${artifacts}/gpu-$<CONFIG>/warp/browser-comparison.json"
        FIXTURES_REQUIRED gpu_report)
endif()

weft_add_executable(weft_flex_layout_probe SOURCES flex_layout_probe.cpp LINK weft::layout)
if(WEFT_BUILD_TESTS)
    weft_add_test(NAME flex_layout_report
        COMMAND weft_flex_layout_probe "${PROJECT_SOURCE_DIR}/third_party/ahem/Ahem.ttf" "${artifacts}/flex-$<CONFIG>"
        FIXTURES_SETUP flex_report)
    weft_add_python_test(NAME flex_browser_diff
        SCRIPT tools/compare_flex_reports.py
        ARGS "${fixtures}/flex_browser_reference.json" "${artifacts}/flex-$<CONFIG>/css-adapter.json"
        --output "${artifacts}/flex-$<CONFIG>/browser-comparison.json"
        FIXTURES_REQUIRED flex_report)
endif()

weft_add_executable(weft_gallery_probe SOURCES gallery_probe.cpp LINK weft::script SOURCE_DIR LARGE_STACK)
if(WEFT_BUILD_TESTS AND NOT QJS_ENABLE_GC_STRESS AND Python3_Interpreter_FOUND)
    weft_add_test(NAME gallery_native_observation
        COMMAND weft_gallery_probe "${artifacts}/gallery-$<CONFIG>"
        TIMEOUT 60
        FIXTURES_SETUP gallery_observation)
    weft_add_python_test(NAME gallery_observation_audit
        SCRIPT tools/verify_gallery_execution.py
        ARGS "${artifacts}/gallery-$<CONFIG>/native-execution.json"
        FIXTURES_REQUIRED gallery_observation)
endif()

weft_add_executable(weft_script_dom_probe SOURCES script_dom_probe.cpp LINK weft::script SOURCE_DIR)
if(WEFT_BUILD_TESTS)
    weft_add_test(NAME d3_native_dom
        COMMAND weft_script_dom_probe "${PROJECT_SOURCE_DIR}/third_party/d3/d3.js" "${fixtures}/d3_probe.js"
        TIMEOUT 30)
endif()
# The engine bridge against CRPG's JSON spliced into a script (docs/engine-communication.md, 13.2): the quick run checks
# that the three ways hand the page the same values; --full measures, by hand.
# What a View costs its owner each frame (docs/engine-communication.md, 13.3): the gallery's anchored HUD, with and
# without health changes; the quick run checks that it runs, --full measures, by hand.
set(view_benchmark_fonts)
if(WIN32)
    set(view_benchmark_fonts weft::platform_win32)
elseif(ANDROID)
    set(view_benchmark_fonts weft::platform_android)
endif()
weft_add_executable(weft_view_benchmark SOURCES view_benchmark.cpp LINK weft::view ${view_benchmark_fonts} SOURCE_DIR LARGE_STACK)
if(WEFT_BUILD_TESTS)
    weft_add_test(NAME view_benchmark_quick
        COMMAND weft_view_benchmark --quick "${artifacts}/view-benchmark-$<CONFIG>" TIMEOUT 120)
endif()
# The CRPG floating UI driven by JSON batches and by anchors and events (docs/engine-communication.md, 13.4): the quick
# run (WARP) checks that both draw alike, that the anchored page publishes no frames while only anchors move and that
# instances that went leave the document; --full measures on this machine's GPU, by hand.
if(WIN32)
    weft_add_executable(weft_floating_benchmark SOURCES floating_benchmark.cpp
        LINK weft::view weft::scene_d3d11 weft::platform_win32 d3d11 windowscodecs ole32 SOURCE_DIR LARGE_STACK)
    if(WEFT_BUILD_TESTS)
        weft_add_test(NAME floating_benchmark_quick
            COMMAND weft_floating_benchmark --quick "${artifacts}/floating-benchmark-$<CONFIG>" TIMEOUT 600)
    endif()
endif()
weft_add_executable(weft_bridge_benchmark SOURCES bridge_benchmark.cpp LINK weft::script)
if(WEFT_BUILD_TESTS)
    weft_add_test(NAME bridge_benchmark_quick
        COMMAND weft_bridge_benchmark --quick "${artifacts}/bridge-benchmark-$<CONFIG>" TIMEOUT 120)
endif()
