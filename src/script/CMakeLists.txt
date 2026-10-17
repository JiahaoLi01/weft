# QuickJS DOM bindings. The owner platform JS modules are compiled into the library, so a running engine needs no
# source tree.
set(embedded_js)
foreach(module web_host_bootstrap animation_frame_bootstrap scene_platform_bootstrap engine_bootstrap
        crpg_bridge_bootstrap web_platform_bootstrap)
    set(input "${CMAKE_CURRENT_SOURCE_DIR}/js/${module}.js")
    set(output "${CMAKE_BINARY_DIR}/generated/script/js/${module}.js.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" "-DOUTPUT=${output}" -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
        DEPENDS "${input}" "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
        COMMENT "Embedding ${module}.js"
        VERBATIM)
    list(APPEND embedded_js "${output}")
endforeach()

weft_add_library(script
    SOURCES script_dom.cpp script_platform.cpp web_storage.cpp ${embedded_js}
    PUBLIC weft::bridge weft::canvas weft::dom
    PRIVATE qjs)
target_include_directories(weft_script PRIVATE "${CMAKE_BINARY_DIR}/generated")
