# Sample UI shader effects that read what lies behind their element (docs/effects.md): refraction and heat haze,
# compiled as a host compiles its effects (cmake/compile_effect.cmake) beside a copy of their manifest, so that
#   weft_page_runner ... --effects <build>/samples/effects/effects.json
# registers them through the C ABI. tests/CMakeLists.txt embeds the same sources for the renderer tests.
set(out "${CMAKE_BINARY_DIR}/samples/effects")
set(outputs "${out}/effects.json")
foreach(sample refraction heat-haze)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${sample}.hlsl")
    set(products "${out}/${sample}.encoded.dxbc" "${out}/${sample}.linear.dxbc")
    set(compilers "-DFXC=${WEFT_FXC_EXECUTABLE}")
    if(WEFT_RENDER_VULKAN)
        list(APPEND compilers "-DDXC=${WEFT_DXC_EXECUTABLE}")
        list(APPEND products "${out}/${sample}.encoded.spv" "${out}/${sample}.linear.spv")
    endif()
    add_custom_command(OUTPUT ${products}
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${source}" "-DOUTPUT=${out}/${sample}" ${compilers}
            -P "${PROJECT_SOURCE_DIR}/cmake/compile_effect.cmake"
        DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/compile_effect.cmake"
            "${PROJECT_SOURCE_DIR}/cmake/check_effect.cmake" "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl"
        COMMENT "Compiling sample effect ${sample}"
        VERBATIM)
    list(APPEND outputs ${products})
endforeach()
add_custom_command(OUTPUT "${out}/effects.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/effects.json" "${out}/effects.json"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/effects.json"
    VERBATIM)
add_custom_target(weft_sample_effects ALL DEPENDS ${outputs})
