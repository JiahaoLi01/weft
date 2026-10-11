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

weft_add_executable(weft_core_tests SOURCES core_tests.cpp LINK weft::core)
weft_add_test(NAME core_contract COMMAND weft_core_tests)

# The C smoke tests compile as C11 against include/weft only.
add_executable(weft_c_api_smoke c_api_smoke.c)
target_include_directories(weft_c_api_smoke PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(weft_c_api_smoke PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(weft_c_api_smoke PRIVATE weft::core)
weft_add_test(NAME c_api_smoke COMMAND weft_c_api_smoke)

weft_add_executable(weft_resource_lifetime_tests SOURCES resource_lifetime_tests.cpp LINK weft::render)
weft_add_test(NAME resource_lifetime COMMAND weft_resource_lifetime_tests)

weft_add_executable(weft_render_backend_tests SOURCES render_backend_tests.cpp LINK weft::render)
weft_add_test(NAME render_backend_contract COMMAND weft_render_backend_tests)

weft_add_executable(weft_render_backend_concept_tests SOURCES render_backend_concept_tests.cpp LINK weft::render)
weft_add_test(NAME render_backend_concept COMMAND weft_render_backend_concept_tests)

if(WIN32)
    weft_add_executable(weft_render_d3d11_tests SOURCES render_d3d11_tests.cpp LINK weft::render_d3d11)
    weft_add_test(NAME render_d3d11_contract COMMAND weft_render_d3d11_tests TIMEOUT 60)
endif()

add_executable(weft_render_backend_c_smoke render_backend_c_smoke.c)
target_include_directories(weft_render_backend_c_smoke PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(weft_render_backend_c_smoke PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(weft_render_backend_c_smoke PRIVATE weft::render)
weft_add_test(NAME render_backend_c_abi COMMAND weft_render_backend_c_smoke)

# Script runtime, owner executor and animation frames.
weft_add_executable(weft_script_dom_tests SOURCES script/script_dom_tests.cpp LINK weft::script SOURCE_DIR)
weft_add_test(NAME script_dom_lifecycle COMMAND weft_script_dom_tests TIMEOUT 30)

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
        LINK weft::scene_d2d weft::platform_win32 weft::script ole32
        SOURCE_DIR LARGE_STACK)
    weft_add_test(NAME scene_contract COMMAND weft_scene_contract_tests)

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
    weft_add_executable(weft_scene_text_image_tests SOURCES scene_text_image_tests.cpp
        LINK weft::scene weft::platform_win32)
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

weft_add_executable(weft_font_catalog_tests SOURCES font_catalog_tests.cpp LINK weft::text)
weft_add_test(NAME font_catalog_contract COMMAND weft_font_catalog_tests)

weft_add_executable(weft_font_fallback_tests SOURCES font_fallback_tests.cpp LINK weft::text SOURCE_DIR)
weft_add_test(NAME font_fallback_contract COMMAND weft_font_fallback_tests)

weft_add_executable(weft_sdf_font_tests SOURCES sdf_font_tests.cpp LINK weft::text SOURCE_DIR)
weft_add_test(NAME sdf_font_contract COMMAND weft_sdf_font_tests)

# Style and layout, each with a Chrome reference diff.
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
