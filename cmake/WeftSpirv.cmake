# Build-time HLSL to SPIR-V compilation for the Vulkan backend, with the DXC of the Vulkan SDK (its SPIR-V backend;
# the Windows SDK's DXC has none). The SPIR-V is embedded in the program like the DXBC of the D3D11 shaders. The DXC
# version is pinned; a different one stops the configuration unless WEFT_DXC_VERSION names it.

set(WEFT_DXC_VERSION "1.9.0.5512" CACHE STRING "DXC version the SPIR-V shaders are compiled and verified with")
find_program(WEFT_DXC_EXECUTABLE dxc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "C:/VulkanSDK/1.4.363.0/Bin"
    NO_DEFAULT_PATH
    DOC "DXC with the SPIR-V backend (Vulkan SDK 1.4.363.0)")

if(NOT DEFINED WEFT_RENDER_VULKAN)
    if(WEFT_DXC_EXECUTABLE)
        set(weft_vulkan_default ON)
    else()
        set(weft_vulkan_default OFF)
    endif()
endif()
option(WEFT_RENDER_VULKAN "Build the Vulkan backend and its tests" ${weft_vulkan_default})

if(NOT WEFT_RENDER_VULKAN)
    message(STATUS "Weft: the Vulkan backend is not built (no DXC with SPIR-V; install the Vulkan SDK 1.4.363.0)")
    return()
endif()
if(NOT WEFT_DXC_EXECUTABLE)
    message(FATAL_ERROR "WEFT_RENDER_VULKAN needs the DXC of the Vulkan SDK 1.4.363.0 (WEFT_DXC_EXECUTABLE)")
endif()
execute_process(COMMAND "${WEFT_DXC_EXECUTABLE}" --version OUTPUT_VARIABLE weft_dxc_reported
    ERROR_VARIABLE weft_dxc_reported)
string(FIND "${weft_dxc_reported}" "${WEFT_DXC_VERSION}" weft_dxc_found)
if(weft_dxc_found EQUAL -1)
    message(FATAL_ERROR "DXC at ${WEFT_DXC_EXECUTABLE} is not version ${WEFT_DXC_VERSION}: ${weft_dxc_reported}"
        "Set WEFT_DXC_VERSION to use another version deliberately.")
endif()

# Register shifts of the Vulkan backend's one descriptor set; constants::render::vulkan names the same bindings.
set(WEFT_SPIRV_SHIFTS -fvk-b-shift 0 0 -fvk-t-shift 10 0 -fvk-s-shift 20 0)

# weft_add_spirv(<target> <name> <source.hlsl> [DEFINES <name=value>...])
# Compiles vs_main (vs_6_0) and ps_main (ps_6_0) of <source.hlsl> to SPIR-V for Vulkan 1.1 into byte tables included
# by <target> as "shaders/<name>.vs.spv.inc" and "shaders/<name>.ps.spv.inc". Constant buffer b<n> binds at n, texture
# or structured buffer t<n> at 10 + n and sampler s<n> at 20 + n. The vertex stage flips y into Vulkan's clip space.
# Always optimized (-O3); warnings are errors.
function(weft_add_spirv target name source)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "DEFINES")
    set(defines)
    foreach(define ${arg_DEFINES})
        list(APPEND defines -D "${define}")
    endforeach()
    get_filename_component(source "${source}" ABSOLUTE)
    set(directory "${CMAKE_BINARY_DIR}/generated/shaders")
    set(outputs)
    foreach(stage vs ps)
        set(flags)
        if(stage STREQUAL "vs")
            set(flags -fvk-invert-y)
        endif()
        set(spirv "${directory}/${name}.${stage}.spv")
        set(output "${directory}/${name}.${stage}.spv.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${WEFT_DXC_EXECUTABLE}" -spirv -T ${stage}_6_0 -E ${stage}_main -O3 -WX
                -fspv-target-env=vulkan1.1 ${WEFT_SPIRV_SHIFTS} ${flags} ${defines} -Fo "${spirv}" "${source}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spirv}" "-DOUTPUT=${output}" -DBINARY=ON
                -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
            DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
            COMMENT "Compiling ${name} ${stage}_main to SPIR-V"
            VERBATIM)
        list(APPEND outputs "${output}")
    endforeach()
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
endfunction()
