# Behavior tests. Native reports land in ${CMAKE_BINARY_DIR}/artifacts/<suite>-<config>; browser diffs compare them
# with the checked-in Chrome references in tests/fixtures.
set(artifacts "${CMAKE_BINARY_DIR}/artifacts")
set(fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures")

# Upstream libunibreak conformance data.
add_executable(weft_unicode_upstream_tests "${WEFT_UNIBREAK_SRC}/tests.c")
target_link_libraries(weft_unicode_upstream_tests PRIVATE weft_unibreak)
if(MSVC)
    target_compile_options(weft_unicode_upstream_tests PRIVATE /utf-8)
endif()
weft_add_test(NAME unicode_line_data COMMAND weft_unicode_upstream_tests l WORKING_DIRECTORY "${WEFT_UNIBREAK_SRC}")
weft_add_test(NAME unicode_grapheme_data COMMAND weft_unicode_upstream_tests g WORKING_DIRECTORY "${WEFT_UNIBREAK_SRC}")

# Provenance of pinned dependencies, generated inputs and browser references.
weft_add_python_test(NAME dependency_integrity SCRIPT tools/verify_dependencies.py)
weft_add_python_test(NAME flex_fixture_integrity SCRIPT tools/generate_flex_cases.py ARGS --check)
weft_add_python_test(NAME react_fixture_integrity SCRIPT tools/verify_react_fixture.py)
weft_add_python_test(NAME cheetah_fixture_integrity SCRIPT tools/verify_cheetah_fixture.py)
weft_add_python_test(NAME cheetah_alert_fixture_integrity SCRIPT tools/verify_cheetah_alert.py)
weft_add_python_test(NAME style_fixture_integrity SCRIPT tools/verify_style_fixture.py)
weft_add_python_test(NAME auto_fixture_integrity SCRIPT tools/verify_auto_fixture.py)
weft_add_python_test(NAME flow_fixture_integrity SCRIPT tools/verify_flow_fixture.py)
weft_add_python_test(NAME flow_input_fixture_integrity SCRIPT tools/verify_flow_input_fixture.py)
weft_add_python_test(NAME canvas_browser_fixture_integrity SCRIPT tools/verify_canvas_fixture.py)
weft_add_python_test(NAME gallery_source_integrity SCRIPT tools/verify_gallery_fixture.py)
weft_add_python_test(NAME gallery_runtime_integrity SCRIPT tools/verify_gallery_runtime.py)

# Foundation and C ABI.
weft_add_executable(weft_base_tests SOURCES base_tests.cpp LINK weft::base)
weft_add_test(NAME base_contract COMMAND weft_base_tests)

weft_add_executable(weft_scene_image_cache_tests SOURCES scene_image_cache_tests.cpp LINK weft::base)
weft_add_test(NAME scene_image_cache_contract COMMAND weft_scene_image_cache_tests)

weft_add_executable(weft_core_tests SOURCES core_tests.cpp LINK weft::core)
weft_add_test(NAME core_contract COMMAND weft_core_tests)

# The C smoke tests compile as C11 against include/weft only.
add_executable(weft_c_api_smoke c_api_smoke.c)
target_include_directories(weft_c_api_smoke PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(weft_c_api_smoke PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(weft_c_api_smoke PRIVATE weft::core)
weft_add_test(NAME c_api_smoke COMMAND weft_c_api_smoke)
# The View's C ABI: sizes, versions, threads, stale handles, pages, the bridge, anchors, hits, frames and faults.
weft_add_executable(weft_c_api_view_tests SOURCES c_api_view_tests.cpp LINK weft::core weft::bridge SOURCE_DIR LARGE_STACK)
weft_add_test(NAME c_api_view_contract COMMAND weft_c_api_view_tests TIMEOUT 180)

weft_add_executable(weft_resource_lifetime_tests SOURCES resource_lifetime_tests.cpp LINK weft::render)
weft_add_test(NAME resource_lifetime COMMAND weft_resource_lifetime_tests)

weft_add_executable(weft_render_backend_tests SOURCES render_backend_tests.cpp LINK weft::render)
weft_add_test(NAME render_backend_contract COMMAND weft_render_backend_tests)

weft_add_executable(weft_render_backend_concept_tests SOURCES render_backend_concept_tests.cpp LINK weft::render)
weft_add_test(NAME render_backend_concept COMMAND weft_render_backend_concept_tests)

# UI shader effects the renderer tests draw (cmake/WeftEffects.cmake), compiled for every backend built here.
add_library(weft_test_effects STATIC)
weft_project_options(weft_test_effects)
target_link_libraries(weft_test_effects PUBLIC weft::paint)
target_include_directories(weft_test_effects PUBLIC "${CMAKE_BINARY_DIR}/generated")
weft_add_effect(weft_test_effects effect_uv effects/uv.hlsl PARAMETERS 0 TEXTURES 0)
weft_add_effect(weft_test_effects effect_parameters effects/parameters.hlsl PARAMETERS 16 TEXTURES 0)
weft_add_effect(weft_test_effects effect_textures effects/textures.hlsl PARAMETERS 0 TEXTURES 2)
weft_add_effect(weft_test_effects effect_color effects/color.hlsl PARAMETERS 1 TEXTURES 0)
weft_add_effect(weft_test_effects effect_repeat effects/repeat.hlsl PARAMETERS 0 TEXTURES 1)
# An effect that CSS attaches: named parameters, a named texture, and the time.
weft_add_effect(weft_test_effects effect_css effects/css.hlsl
    PARAMETERS "tint:color=#336699" amount:number=0.5 offset:vec2=0,0 TEXTURES mask ANIMATED)

# The effect check (cmake/check_effect.cmake) on tests/effects/check/<sample>.hlsl: it accepts the sample, or with a
# reason refuses it with a message that names the reason. CMake wraps messages: the reason's spaces match any space.
function(weft_effect_check sample)
    set(test effect_check_${sample})
    weft_add_test(NAME ${test}
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/effects/check/${sample}.hlsl"
            "-DSTAMP=${CMAKE_CURRENT_BINARY_DIR}/${test}.stamp" -P "${PROJECT_SOURCE_DIR}/cmake/check_effect.cmake")
    if(ARGC GREATER 1)
        string(REPLACE " " "[ \t\r\n]+" reason "${ARGV1}")
        set_tests_properties(${test} PROPERTIES PASS_REGULAR_EXPRESSION "${reason}")
    endif()
endfunction()
# Comments that name anything, static globals, types and helper functions.
weft_effect_check(accepted)
foreach(sample register cbuffer texture sampler)
    weft_effect_check(${sample} "it samples effectTexture0-3")
endforeach()
weft_effect_check(global "a global without static")
weft_effect_check(include "without includes")

if(WIN32)
    weft_add_executable(weft_render_d3d11_tests SOURCES render_d3d11_tests.cpp
        LINK weft::render_d3d11 weft_test_effects)
    weft_add_test(NAME render_d3d11_contract COMMAND weft_render_d3d11_tests TIMEOUT 60)
endif()

# The Vulkan renderer on this machine's Vulkan device, against CPU expectations and on Windows against the D3D11
# renderer on WARP; skipped (exit 77) without a Vulkan loader or device.
if(WEFT_RENDER_VULKAN)
    set(vulkan_test_links weft::render_vulkan weft_test_effects)
    if(WIN32)
        list(APPEND vulkan_test_links weft::render_d3d11)
    endif()
    weft_add_executable(weft_render_vulkan_tests SOURCES render_vulkan_tests.cpp LINK ${vulkan_test_links})
    weft_add_test(NAME render_vulkan_contract COMMAND weft_render_vulkan_tests TIMEOUT 120)
    set_tests_properties(render_vulkan_contract PROPERTIES SKIP_RETURN_CODE 77)
    # The Vulkan adapter of the renderer C ABI, driven as a host drives it.
    weft_add_executable(weft_render_vulkan_backend_tests SOURCES render_vulkan_backend_tests.cpp
        LINK weft::render_vulkan)
    weft_add_test(NAME render_vulkan_backend_contract COMMAND weft_render_vulkan_backend_tests TIMEOUT 120)
    set_tests_properties(render_vulkan_backend_contract PROPERTIES SKIP_RETURN_CODE 77)
    # Device memory that images share: the range heap on the CPU, and images in shared blocks on the device.
    weft_add_executable(weft_render_vulkan_memory_tests SOURCES render_vulkan_memory_tests.cpp
        LINK weft::render_vulkan)
    weft_add_test(NAME render_vulkan_memory_heap COMMAND weft_render_vulkan_memory_tests --heap TIMEOUT 120)
    weft_add_test(NAME render_vulkan_memory_device COMMAND weft_render_vulkan_memory_tests --device TIMEOUT 120)
    set_tests_properties(render_vulkan_memory_device PROPERTIES SKIP_RETURN_CODE 77)
    # Host textures through the Vulkan adapter of the renderer C ABI.
    weft_add_executable(weft_render_vulkan_external_tests SOURCES render_vulkan_external_tests.cpp
        LINK weft::render_vulkan)
    weft_add_test(NAME render_vulkan_external_contract COMMAND weft_render_vulkan_external_tests TIMEOUT 120)
    set_tests_properties(render_vulkan_external_contract PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_executable(weft_render_backend_c_smoke render_backend_c_smoke.c)
target_include_directories(weft_render_backend_c_smoke PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(weft_render_backend_c_smoke PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(weft_render_backend_c_smoke PRIVATE weft::render)
weft_add_test(NAME render_backend_c_abi COMMAND weft_render_backend_c_smoke)

# Script runtime, owner executor and animation frames.
weft_add_executable(weft_script_dom_tests SOURCES script/script_dom_tests.cpp LINK weft::script SOURCE_DIR)
weft_add_test(NAME script_dom_lifecycle COMMAND weft_script_dom_tests TIMEOUT 30)

# The engine bridge: values, the channel, and weft.engine over a ScriptDom.
weft_add_executable(weft_bridge_tests SOURCES bridge_tests.cpp LINK weft::bridge)
weft_add_test(NAME bridge_contract COMMAND weft_bridge_tests TIMEOUT 120)
weft_add_executable(weft_engine_bridge_tests SOURCES script/engine_bridge_tests.cpp LINK weft::script)
weft_add_test(NAME engine_bridge_contract COMMAND weft_engine_bridge_tests TIMEOUT 60)

weft_add_executable(weft_owner_executor_tests SOURCES owner_executor_tests.cpp LINK weft::script)
weft_add_test(NAME owner_executor_contract COMMAND weft_owner_executor_tests TIMEOUT 180)

weft_add_executable(weft_gallery_trace_tests SOURCES gallery_trace_tests.cpp)
weft_add_test(NAME gallery_trace_contract COMMAND weft_gallery_trace_tests "${artifacts}/gallery-trace-$<CONFIG>")

weft_add_executable(weft_owner_hotpath_tests SOURCES owner_hotpath_tests.cpp LINK weft::script SOURCE_DIR LARGE_STACK)
weft_add_test(NAME owner_hotpath_contract COMMAND weft_owner_hotpath_tests TIMEOUT 180)

weft_add_executable(weft_animation_frame_tests
    SOURCES animation_frame_tests.cpp
    LINK weft::script
    SOURCE_DIR LARGE_STACK GC_STRESS)
weft_add_test(NAME animation_frame_contract COMMAND weft_animation_frame_tests TIMEOUT 180)

# Scenes and Canvas 2D through the Windows reference rasters.
if(WIN32)
    set(scene_libraries weft::scene_d2d weft::platform_win32 weft::script windowscodecs ole32)

    weft_add_executable(weft_scene_contract_tests
        SOURCES scene_contract_tests.cpp
        LINK weft::scene_d2d weft::platform_win32 weft::script d2d1 windowscodecs ole32
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME scene_contract COMMAND weft_scene_contract_tests)

    # The incremental pipeline against full computation: cascade, scene and display lists (step 13.5).
    weft_add_executable(weft_incremental_tests
        SOURCES incremental_tests.cpp
        LINK weft::scene_d3d11 ${scene_libraries} d2d1
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME incremental_contract COMMAND weft_incremental_tests TIMEOUT 300)

    weft_add_executable(weft_scene_cluster_tests
        SOURCES scene_cluster_tests.cpp
        LINK ${scene_libraries}
        SOURCE_DIR LARGE_STACK)
    # Debug performs fresh geometry reads for every node in 30 large scenes.
    # Keep the same 301 frames and image/geometry assertions as Release.
    weft_add_test(NAME scene_cluster_original
        COMMAND weft_scene_cluster_tests "${artifacts}/gallery-cluster-$<CONFIG>/native"
        TIMEOUT 900)
    weft_add_python_test(NAME scene_cluster_fixture_integrity SCRIPT tools/verify_gallery_cluster.py)
    weft_add_python_test(NAME scene_cluster_browser_contract
        SCRIPT tools/verify_gallery_cluster.py
        ARGS "${artifacts}/gallery-cluster-$<CONFIG>/native"
        DEPENDS scene_cluster_original)

    weft_add_executable(weft_scene_cluster_contract_tests
        SOURCES scene_cluster_contract_tests.cpp
        LINK ${scene_libraries}
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME scene_cluster_platform COMMAND weft_scene_cluster_contract_tests TIMEOUT 180)
    # The CRPG UI's nested tooltip page as a scene: body portals, nested tooltips, pin and drag, the smoke as an effect.
    weft_add_executable(weft_scene_tooltip_page_tests
        SOURCES scene_tooltip_page_tests.cpp
        LINK ${scene_libraries} weft::scene_d3d11 weft_gallery_effects windowscodecs ole32
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME scene_tooltip_page COMMAND weft_scene_tooltip_page_tests "${artifacts}/tooltip-page-$<CONFIG>"
        TIMEOUT 300)
    # The gallery's UI particle page: streams, loot flying to the counter, hover, density, and its pixels alike on
    # D3D11 WARP and Vulkan (when built) and from two runs of the same clock.
    set(particle_page_links ${scene_libraries} weft::scene_d3d11 weft_gallery_effects windowscodecs ole32)
    if(WEFT_RENDER_VULKAN)
        list(APPEND particle_page_links weft::scene_vulkan)
    endif()
    weft_add_executable(weft_scene_particle_page_tests
        SOURCES scene_particle_page_tests.cpp
        LINK ${particle_page_links}
        SOURCE_DIR LARGE_STACK)
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_scene_particle_page_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME scene_particle_page COMMAND weft_scene_particle_page_tests
        "${artifacts}/particle-page-$<CONFIG>" TIMEOUT 300)
    # The gallery's translucent window page: frosted, plain and blur-only windows over a moving background, dragged,
    # added and removed, against Chrome's replay of the same frames and inputs.
    weft_add_executable(weft_scene_glass_page_tests
        SOURCES scene_glass_page_tests.cpp
        LINK ${particle_page_links}
        SOURCE_DIR LARGE_STACK)
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_scene_glass_page_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME scene_glass_page COMMAND weft_scene_glass_page_tests
        "${artifacts}/glass-page-$<CONFIG>" TIMEOUT 300)

    weft_add_executable(weft_scene_gallery_tests
        SOURCES scene_gallery_tests.cpp
        LINK ${scene_libraries}
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME scene_gallery_original
        COMMAND weft_scene_gallery_tests "${artifacts}/scene-gallery-$<CONFIG>"
        TIMEOUT 240
        GC_STRESS_TIMEOUT 600)
    weft_add_python_test(NAME scene_fixture_integrity SCRIPT tools/verify_scene_fixture.py)
    weft_add_python_test(NAME scene_browser_geometry
        SCRIPT tools/compare_scene_geometry.py
        ARGS "${artifacts}/scene-gallery-$<CONFIG>" "${fixtures}/scene-gallery-reference"
        DEPENDS scene_gallery_original)

    # Portable scene text on the installed fonts versus Chrome's metrics and face choices.
    weft_add_executable(weft_scene_text_tests SOURCES scene_text_tests.cpp LINK weft::scene weft::platform_win32)
    weft_add_test(NAME scene_text_metrics
        COMMAND weft_scene_text_tests "${artifacts}/scene-text-$<CONFIG>/native.json"
        FIXTURES_SETUP scene_text_report)
    # Dotted and dashed border sides versus Chrome's pixels.
    weft_add_executable(weft_scene_border_tests SOURCES scene_border_tests.cpp LINK weft::scene)
    weft_add_test(NAME scene_border_patterns COMMAND weft_scene_border_tests)
    weft_add_executable(weft_scene_border_browser_tests SOURCES scene_border_browser_tests.cpp
        LINK weft::scene_d3d11 weft::platform_win32 windowscodecs ole32 SOURCE_DIR)
    weft_add_test(NAME scene_border_browser_contract
        COMMAND weft_scene_border_browser_tests "${artifacts}/scene-border-$<CONFIG>" TIMEOUT 120)
    # Scene text styles and the CSS around them: synthetic oblique against Chrome's pixels, case mappings, viewport
    # lengths, overflow axes and diagnostics.
    weft_add_executable(weft_scene_text_style_tests SOURCES scene_text_style_tests.cpp
        LINK weft::scene_d3d11 weft::platform_win32 windowscodecs ole32 SOURCE_DIR)
    weft_add_test(NAME scene_text_style_contract COMMAND weft_scene_text_style_tests TIMEOUT 120)
    # position: fixed against Chrome's geometry, hits and the native pass, and its diagnostics.
    weft_add_executable(weft_scene_fixed_tests SOURCES scene_fixed_tests.cpp
        LINK weft::scene_d3d11 weft::platform_win32 SOURCE_DIR)
    weft_add_test(NAME scene_fixed_browser_contract COMMAND weft_scene_fixed_tests TIMEOUT 120)
    # Scene inline formatting contexts (line breaking, inline box fragments, atomic inlines) versus Chrome's geometry.
    weft_add_executable(weft_scene_inline_tests SOURCES scene_inline_tests.cpp LINK weft::scene weft::platform_win32
        SOURCE_DIR)
    weft_add_test(NAME scene_inline_browser_contract COMMAND weft_scene_inline_tests)
    weft_add_executable(weft_scene_native_tests SOURCES scene_native_tests.cpp
        LINK weft::scene_native weft::platform_win32)
    weft_add_test(NAME scene_native_contract COMMAND weft_scene_native_tests)
    # Anchors on the CPU reference path: the data-anchor grammar, the tables a writer thread hands both readers, where
    # anchors put elements, and the display list, hits and geometry of anchored elements.
    # The public View: a bridge-driven page, its events, anchors, hits, diagnostics and frames, and the gallery session's
    # scene for the same page.
    weft_add_executable(weft_view_tests SOURCES view_tests.cpp
        LINK weft::view weft::scene_d2d weft::platform_win32 d2d1 windowscodecs ole32 SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME view_contract COMMAND weft_view_tests TIMEOUT 120)
    weft_add_executable(weft_scene_anchor_tests SOURCES scene_anchor_tests.cpp
        LINK weft::scene_native weft::scene_d2d weft::platform_win32 d2d1 windowscodecs ole32)
    weft_add_test(NAME scene_anchor_contract COMMAND weft_scene_anchor_tests)
    # What native scene passes keep between frames by content: SVG elements' values (alike with and without the cache
    # under random edits), text images by source, path coverage by digest.
    weft_add_executable(weft_scene_display_cache_tests SOURCES scene_display_cache_tests.cpp
        LINK weft::scene_native weft::platform_win32)
    weft_add_test(NAME scene_display_cache_contract COMMAND weft_scene_display_cache_tests)
    # UI shader effects that CSS attaches in scenes: registration, custom properties and display lists.
    weft_add_executable(weft_scene_effect_tests SOURCES scene_effect_tests.cpp
        LINK weft::scene weft::platform_win32 weft_test_effects)
    weft_add_test(NAME scene_effect_contract COMMAND weft_scene_effect_tests)
    # Host textures that CSS names in scenes (-weft-texture(name)): names, diagnostics and display lists.
    weft_add_executable(weft_scene_texture_tests SOURCES scene_texture_tests.cpp
        LINK weft::scene weft::platform_win32)
    weft_add_test(NAME scene_texture_contract COMMAND weft_scene_texture_tests)
    # Their background layouts (background-origin, -clip, -size, -position, -repeat): parsed, laid out, diagnosed, and
    # drawn by the native D3D11 pass (WARP) as the reference raster lays them out on the CPU.
    weft_add_executable(weft_scene_background_tests SOURCES scene_background_tests.cpp
        LINK weft::scene_d3d11 weft::scene_d2d weft::platform_win32 windowscodecs ole32)
    weft_add_test(NAME scene_background_contract COMMAND weft_scene_background_tests
        "${artifacts}/scene-background-$<CONFIG>" TIMEOUT 120)
    # UI particles as scene elements show them: registration, overrides, streams, bursts, flights, bounds, damage,
    # and weft.emitParticles.
    weft_add_executable(weft_scene_particles_tests SOURCES scene_particles_tests.cpp
        LINK weft::scene weft::script weft::platform_win32)
    weft_add_test(NAME scene_particles_contract COMMAND weft_scene_particles_tests)
    # backdrop-filter as scene elements take it: filter functions, refusals, the display list's backdrops, damage.
    weft_add_executable(weft_scene_backdrop_tests SOURCES scene_backdrop_tests.cpp
        LINK weft::scene_d2d weft::platform_win32 d2d1 windowscodecs ole32)
    weft_add_test(NAME scene_backdrop_contract COMMAND weft_scene_backdrop_tests)
    # backdrop-filter against Chrome's screenshots of the same cases (tools/browser_backdrop_reference.mjs), whose
    # manifest must name the cases as they are now.
    set(backdrop_cases "${fixtures}/backdrop-reference/cases.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${backdrop_cases}")
    file(SHA256 "${backdrop_cases}" backdrop_cases_sha256)
    weft_add_executable(weft_scene_backdrop_reference_tests SOURCES scene_backdrop_reference_tests.cpp
        LINK weft::scene_d3d11 weft::platform_win32 windowscodecs ole32 SOURCE_DIR)
    weft_add_test(NAME scene_backdrop_reference COMMAND weft_scene_backdrop_reference_tests
        "${artifacts}/backdrop-reference-$<CONFIG>" "${backdrop_cases_sha256}" TIMEOUT 120)
    # The CSS the CRPG floating UI needs of scene pages (text-overflow, text-shadow, drop-shadow, inset shadows, radial
    # and repeating gradients, calc() insets, transform-origin keywords) value by value: parses, lists, layouts and
    # what stays outside the profile.
    weft_add_executable(weft_scene_floating_css_tests SOURCES scene_floating_css_tests.cpp
        LINK weft::scene weft::render weft::platform_win32)
    weft_add_test(NAME scene_floating_css_contract COMMAND weft_scene_floating_css_tests)
    # The same CSS against Chrome's screenshots of the same cases (tools/browser_scene_paint_reference.mjs), whose
    # manifest must name the cases as they are now.
    set(scene_paint_cases "${fixtures}/scene-paint-reference/cases.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scene_paint_cases}")
    file(SHA256 "${scene_paint_cases}" scene_paint_cases_sha256)
    set(scene_paint_links weft::scene_d3d11 weft::scene_d2d weft::platform_win32 windowscodecs ole32)
    if(WEFT_RENDER_VULKAN)
        list(APPEND scene_paint_links weft::scene_vulkan)
    endif()
    weft_add_executable(weft_scene_paint_reference_tests SOURCES scene_paint_reference_tests.cpp
        LINK ${scene_paint_links} SOURCE_DIR)
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_scene_paint_reference_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME scene_paint_reference COMMAND weft_scene_paint_reference_tests
        "${artifacts}/scene-paint-reference-$<CONFIG>" "${scene_paint_cases_sha256}" TIMEOUT 180)
    # UI shader effects in the native scene passes: D3D11 on WARP, and Vulkan on this machine's device when built.
    set(scene_effect_links weft::scene_d3d11 weft::platform_win32 weft_test_effects)
    if(WEFT_RENDER_VULKAN)
        list(APPEND scene_effect_links weft::scene_vulkan)
    endif()
    # Scene inline formatting contracts: reuse, fragment hits and rects, sliced boxes, lines, the native pass.
    weft_add_executable(weft_scene_inline_contract_tests SOURCES scene_inline_contract_tests.cpp
        LINK weft::scene_d3d11 weft::platform_win32)
    weft_add_test(NAME scene_inline_contract COMMAND weft_scene_inline_contract_tests TIMEOUT 120)
    weft_add_executable(weft_render_scene_effects_tests SOURCES render_scene_effects_tests.cpp
        LINK ${scene_effect_links})
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_scene_effects_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_scene_effects_contract COMMAND weft_render_scene_effects_tests TIMEOUT 120)
    # UI particles in both scene passes against a CPU raster of the shared particle model.
    weft_add_executable(weft_render_scene_particles_tests SOURCES render_scene_particles_tests.cpp
        LINK ${scene_effect_links})
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_scene_particles_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_scene_particles_contract COMMAND weft_render_scene_particles_tests TIMEOUT 120)
    # backdrop-filter in the native scene passes against the same algorithm on the CPU, on both passes.
    weft_add_executable(weft_render_scene_backdrop_tests SOURCES render_scene_backdrop_tests.cpp
        LINK ${scene_effect_links})
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_scene_backdrop_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_scene_backdrop_contract COMMAND weft_render_scene_backdrop_tests TIMEOUT 120)
    # SVG rects, circles and lines drawn as boxes, against the same outlines drawn as path coverage, on both passes.
    weft_add_executable(weft_render_scene_shape_boxes_tests SOURCES render_scene_shape_boxes_tests.cpp
        LINK ${scene_effect_links})
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_scene_shape_boxes_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_scene_shape_boxes_contract COMMAND weft_render_scene_shape_boxes_tests
        "${artifacts}/shape-boxes-$<CONFIG>" TIMEOUT 120)
    # Small scene images on atlas pages against textures of their own, on D3D11 WARP and Vulkan.
    weft_add_executable(weft_render_scene_atlas_tests SOURCES render_scene_atlas_tests.cpp
        LINK ${scene_effect_links})
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_scene_atlas_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_scene_atlas_contract COMMAND weft_render_scene_atlas_tests
        "${artifacts}/scene-atlas-$<CONFIG>" TIMEOUT 120)
    # Anchored nameplates against the same plates placed by CSS, on D3D11 WARP and Vulkan.
    weft_add_executable(weft_render_scene_anchor_tests SOURCES render_scene_anchor_tests.cpp
        LINK ${scene_effect_links})
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_scene_anchor_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_scene_anchor_contract COMMAND weft_render_scene_anchor_tests
        "${artifacts}/scene-anchor-$<CONFIG>" TIMEOUT 120)
    # View frames through the renderer C ABI on the built-in D3D11 (WARP) and Vulkan backends, against their native
    # passes drawn directly.
    weft_add_executable(weft_render_view_tests SOURCES render_view_tests.cpp
        LINK weft::core weft::view ${scene_effect_links} d3d11 d3d12 dxgi ole32 LARGE_STACK)
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_view_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_view_contract COMMAND weft_render_view_tests TIMEOUT 180)
    # View frames through the renderer C ABI on the built-in D3D12 backend (WARP, with the debug layer when it is
    # installed), against the D3D11 backend drawing the same frames.
    weft_add_executable(weft_render_d3d12_view_tests SOURCES render_d3d12_view_tests.cpp
        LINK weft::core weft::view weft::scene_d3d11 weft::scene_d3d12 d3d11 d3d12 dxgi ole32 LARGE_STACK)
    weft_add_test(NAME render_d3d12_view_contract COMMAND weft_render_d3d12_view_tests TIMEOUT 180)
    set_tests_properties(render_d3d12_view_contract PROPERTIES SKIP_RETURN_CODE 77)
    # The CRPG UI's smoke as a UI shader effect against the original's tick on the CPU, on both scene passes.
    weft_add_executable(weft_render_crpg_smoke_tests SOURCES render_crpg_smoke_tests.cpp
        LINK ${scene_effect_links} weft_gallery_effects windowscodecs ole32 SOURCE_DIR)
    if(WEFT_RENDER_VULKAN)
        target_compile_definitions(weft_render_crpg_smoke_tests PRIVATE WEFT_TEST_VULKAN)
    endif()
    weft_add_test(NAME render_crpg_smoke_contract COMMAND weft_render_crpg_smoke_tests TIMEOUT 120)
    if(WEFT_RENDER_VULKAN)
        # The Vulkan scene pass recorded into a host's command buffers on this machine's Vulkan device; skipped (77)
        # without one.
        weft_add_executable(weft_render_vulkan_scene_tests SOURCES render_vulkan_scene_tests.cpp
            LINK weft::scene_vulkan weft::platform_win32)
        weft_add_test(NAME render_vulkan_scene_contract COMMAND weft_render_vulkan_scene_tests TIMEOUT 120)
        set_tests_properties(render_vulkan_scene_contract PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    weft_add_executable(weft_scene_text_image_tests SOURCES scene_text_image_tests.cpp
        LINK weft::scene_d2d weft::platform_win32)
    weft_add_test(NAME scene_text_image COMMAND weft_scene_text_image_tests)
    weft_add_python_test(NAME scene_text_browser_diff
        SCRIPT tools/compare_scene_text.py
        ARGS "${fixtures}/scene_text_reference.json" "${artifacts}/scene-text-$<CONFIG>/native.json"
        --output "${artifacts}/scene-text-$<CONFIG>/browser-comparison.json"
        FIXTURES_REQUIRED scene_text_report)

    weft_add_executable(weft_canvas_tests
        SOURCES canvas_tests.cpp
        LINK weft::script weft::canvas_d2d weft::layout
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME canvas_2d_contract COMMAND weft_canvas_tests TIMEOUT 180)
endif()

# Fonts and text.
weft_add_executable(weft_text_tests SOURCES text_tests.cpp LINK weft::text SOURCE_DIR)
weft_add_test(NAME text_font_probe COMMAND weft_text_tests)
weft_add_executable(weft_color_canvas_tests SOURCES color_canvas_tests.cpp LINK weft::text)
weft_add_test(NAME color_canvas_contract COMMAND weft_color_canvas_tests)

weft_add_executable(weft_font_catalog_tests SOURCES font_catalog_tests.cpp LINK weft::text SOURCE_DIR)
if(ANDROID)
    # Links the Android platform layer and the Android Vulkan surface; runs on a device only (no test registered).
    weft_add_executable(weft_platform_android_smoke SOURCES platform_android_smoke.cpp
        LINK weft::platform_android weft::render_vulkan)
endif()
weft_add_test(NAME font_catalog_contract COMMAND weft_font_catalog_tests)

weft_add_executable(weft_font_fallback_tests SOURCES font_fallback_tests.cpp LINK weft::text SOURCE_DIR)
weft_add_test(NAME font_fallback_contract COMMAND weft_font_fallback_tests)

weft_add_executable(weft_sdf_font_tests SOURCES sdf_font_tests.cpp LINK weft::text SOURCE_DIR)
weft_add_test(NAME sdf_font_contract COMMAND weft_sdf_font_tests)

# Style and layout, each with a Chrome reference diff.
weft_add_executable(weft_selector_tests SOURCES selector_tests.cpp LINK weft::dom)
weft_add_test(NAME selector_contract COMMAND weft_selector_tests)
weft_add_executable(weft_paint_tests SOURCES paint_tests.cpp LINK weft::paint weft::render weft::scene)
weft_add_test(NAME paint_contract COMMAND weft_paint_tests)
# The particle model the scene shader shares: random values, timing, motion, shapes, curves, frames and flights.
weft_add_executable(weft_particle_model_tests SOURCES particle_model_tests.cpp LINK weft::paint)
weft_add_test(NAME particle_model COMMAND weft_particle_model_tests)
weft_add_executable(weft_vector_raster_tests SOURCES vector_raster_tests.cpp LINK weft::vector)
weft_add_test(NAME vector_raster_contract COMMAND weft_vector_raster_tests)
weft_add_executable(weft_dom_tests SOURCES dom_tests.cpp LINK weft::dom)
weft_add_test(NAME dom_contract COMMAND weft_dom_tests)
weft_add_executable(weft_style_values_tests SOURCES style_values_tests.cpp LINK weft::scene)
weft_add_test(NAME style_values_contract COMMAND weft_style_values_tests)
weft_add_executable(weft_grammar_differential_tests SOURCES grammar_differential_tests.cpp LINK weft::scene)
weft_add_test(NAME grammar_differential COMMAND weft_grammar_differential_tests)
weft_add_executable(weft_scene_transform_tests SOURCES scene_transform_tests.cpp LINK weft::scene)
weft_add_test(NAME scene_transform_contract COMMAND weft_scene_transform_tests)
weft_add_executable(weft_style_tests SOURCES style_tests.cpp LINK weft::script weft::layout SOURCE_DIR)
weft_add_test(NAME style_cascade_contract
    COMMAND weft_style_tests "${artifacts}/style-$<CONFIG>/native.json"
    FIXTURES_SETUP style_report)
weft_add_python_test(NAME style_browser_diff
    SCRIPT tools/compare_style_reports.py
    ARGS "${fixtures}/style_browser_reference.json" "${artifacts}/style-$<CONFIG>/native.json"
    --output "${artifacts}/style-$<CONFIG>/browser-comparison.json"
    FIXTURES_REQUIRED style_report)

weft_add_executable(weft_flex_tests SOURCES flex_tests.cpp LINK weft::layout SOURCE_DIR)
weft_add_test(NAME flex_adapter_contract COMMAND weft_flex_tests)
weft_add_executable(weft_box_layout_tests SOURCES box_layout_tests.cpp LINK weft::layout)
weft_add_test(NAME box_layout_contract COMMAND weft_box_layout_tests)
weft_add_executable(weft_hit_test_tests SOURCES hit_test_tests.cpp LINK weft::layout)
weft_add_test(NAME hit_test_contract COMMAND weft_hit_test_tests)

weft_add_executable(weft_page_tests
    SOURCES page_tests.cpp
    LINK weft::script weft::layout
    SOURCE_DIR LARGE_STACK GC_STRESS)
weft_add_test(NAME page_react_contract COMMAND weft_page_tests TIMEOUT 30 GC_STRESS_TIMEOUT 180)

weft_add_executable(weft_document_cases_tests SOURCES document_cases_tests.cpp LINK weft::layout SOURCE_DIR)
weft_add_test(NAME document_css_contract COMMAND weft_document_cases_tests)

weft_add_executable(weft_auto_layout_tests SOURCES auto_layout_tests.cpp LINK weft::layout weft::script SOURCE_DIR)
weft_add_test(NAME auto_layout_contract
    COMMAND weft_auto_layout_tests "${artifacts}/auto-layout-$<CONFIG>/native.json"
    FIXTURES_SETUP auto_layout_report)
weft_add_python_test(NAME auto_layout_browser_diff
    SCRIPT tools/compare_auto_layout.py
    ARGS "${fixtures}/auto_layout_browser_reference.json" "${artifacts}/auto-layout-$<CONFIG>/native.json"
    --output "${artifacts}/auto-layout-$<CONFIG>/browser-comparison.json"
    FIXTURES_REQUIRED auto_layout_report)

weft_add_executable(weft_flow_page_tests
    SOURCES flow_page_tests.cpp
    LINK weft::layout weft::script
    SOURCE_DIR LARGE_STACK)
weft_add_test(NAME flow_layout_contract
    COMMAND weft_flow_page_tests "${artifacts}/flow-layout-$<CONFIG>/native.json"
    TIMEOUT 60
    FIXTURES_SETUP flow_layout_report)
weft_add_python_test(NAME flow_layout_browser_diff
    SCRIPT tools/compare_flow_layout.py
    ARGS "${fixtures}/flow_layout_browser_reference.json" "${artifacts}/flow-layout-$<CONFIG>/native.json"
    --output "${artifacts}/flow-layout-$<CONFIG>/browser-comparison.json"
    FIXTURES_REQUIRED flow_layout_report)

# Input routing.
weft_add_executable(weft_input_tests
    SOURCES input_tests.cpp
    LINK weft::input
    SOURCE_DIR LARGE_STACK GC_STRESS)
weft_add_test(NAME mouse_input_contract COMMAND weft_input_tests TIMEOUT 30 GC_STRESS_TIMEOUT 180)

weft_add_executable(weft_flow_input_tests
    SOURCES flow_input_tests.cpp
    LINK weft::input weft::layout
    SOURCE_DIR LARGE_STACK)
weft_add_test(NAME flow_input_contract
    COMMAND weft_flow_input_tests "${artifacts}/flow-input-$<CONFIG>/native.json"
    TIMEOUT 60
    FIXTURES_SETUP flow_input_report)
weft_add_python_test(NAME flow_input_browser_diff
    SCRIPT tools/compare_flow_input.py
    ARGS "${fixtures}/flow_input_browser_reference.json" "${artifacts}/flow-input-$<CONFIG>/native.json"
    "${artifacts}/flow-input-$<CONFIG>/comparison.json"
    FIXTURES_REQUIRED flow_input_report)
