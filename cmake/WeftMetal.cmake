# Build-time HLSL to Metal Shading Language for the Metal backend (docs/render-backend-api.md, "Metal"). DXC compiles
# the shared shaders to SPIR-V with the Metal backend's register shifts and no y flip (Metal's clip space is D3D's), and
# the Vulkan SDK's spirv-cross writes MSL 2.1 with each resource's SPIR-V binding as its Metal index. The text is
# embedded in the program, which the device compiles (newLibraryWithSource). spirv-cross's revision is pinned like the
# DXC version (cmake/WeftSpirv.cmake); a different one stops the configuration unless WEFT_SPIRV_CROSS_REVISION names it.
# The translation runs wherever the tools do, so the MSL is checked on Windows too (render_metal_shader_contract).

set(WEFT_SPIRV_CROSS_REVISION "vulkan-sdk-1.4.357.0-39-gf11ba9f0" CACHE STRING
    "spirv-cross revision the Metal shaders are translated and verified with (Vulkan SDK 1.4.363.0)")
find_program(WEFT_SPIRV_CROSS_EXECUTABLE spirv-cross
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "C:/VulkanSDK/1.4.363.0/Bin"
    NO_DEFAULT_PATH
    DOC "spirv-cross (Vulkan SDK 1.4.363.0)")

if(NOT DEFINED WEFT_SHADERS_METAL)
    if(WEFT_DXC_EXECUTABLE AND WEFT_SPIRV_CROSS_EXECUTABLE)
        set(weft_metal_default ON)
    else()
        set(weft_metal_default OFF)
    endif()
endif()
option(WEFT_SHADERS_METAL "Translate the shaders to MSL for the Metal backend and check them" ${weft_metal_default})

if(NOT WEFT_SHADERS_METAL)
    message(STATUS "Weft: no Metal shaders (needs the DXC and spirv-cross of the Vulkan SDK 1.4.363.0)")
    return()
endif()
if(NOT WEFT_DXC_EXECUTABLE OR NOT WEFT_SPIRV_CROSS_EXECUTABLE)
    message(FATAL_ERROR "WEFT_SHADERS_METAL needs the DXC (WEFT_DXC_EXECUTABLE) and spirv-cross "
        "(WEFT_SPIRV_CROSS_EXECUTABLE) of the Vulkan SDK 1.4.363.0")
endif()
# The DXC version, as cmake/WeftSpirv.cmake checks it when it builds the Vulkan backend, and spirv-cross's revision.
execute_process(COMMAND "${WEFT_DXC_EXECUTABLE}" --version OUTPUT_VARIABLE weft_metal_dxc ERROR_VARIABLE weft_metal_dxc)
string(FIND "${weft_metal_dxc}" "${WEFT_DXC_VERSION}" weft_metal_dxc_found)
if(weft_metal_dxc_found EQUAL -1)
    message(FATAL_ERROR "DXC at ${WEFT_DXC_EXECUTABLE} is not version ${WEFT_DXC_VERSION}: ${weft_metal_dxc}"
        "Set WEFT_DXC_VERSION to use another version deliberately.")
endif()
execute_process(COMMAND "${WEFT_SPIRV_CROSS_EXECUTABLE}" --revision OUTPUT_VARIABLE weft_metal_cross
    ERROR_VARIABLE weft_metal_cross)
string(FIND "${weft_metal_cross}" "${WEFT_SPIRV_CROSS_REVISION}" weft_metal_cross_found)
if(weft_metal_cross_found EQUAL -1)
    message(FATAL_ERROR "spirv-cross at ${WEFT_SPIRV_CROSS_EXECUTABLE} is not revision ${WEFT_SPIRV_CROSS_REVISION}: "
        "${weft_metal_cross}Set WEFT_SPIRV_CROSS_REVISION to use another revision deliberately.")
endif()

# Register shifts of the Metal backend's argument tables, which constants::render::metal names: constant buffer b<n>
# at buffer n, texture or structured buffer t<n> at index 1 + n of its table (a structured buffer is a Metal buffer, a
# texture a Metal texture: the shift keeps the records t0 off the View constants b0), sampler s<n> at sampler n.
set(WEFT_METAL_SHIFTS -fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-s-shift 0 0)
# MSL 2.1: macOS 10.14 and iOS 12 or later.
set(WEFT_METAL_MSL_VERSION 20100)
set(WEFT_METAL_CROSS_FLAGS --msl --msl-version ${WEFT_METAL_MSL_VERSION} --msl-decoration-binding)

# weft_add_msl(<target> <name> <source.hlsl> [DEFINES <name=value>...])
# Translates vs_main (vs_6_0) and ps_main (ps_6_0) of <source.hlsl> to MSL text included by <target> as
# "shaders/<name>.vs.metal.inc" and "shaders/<name>.ps.metal.inc" (an initializer of the bytes with a terminating 0);
# the .metal files stay beside them. Always optimized (-O3); DXC's warnings are errors.
function(weft_add_msl target name source)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "DEFINES")
    set(defines)
    foreach(define ${arg_DEFINES})
        list(APPEND defines -D "${define}")
    endforeach()
    get_filename_component(source "${source}" ABSOLUTE)
    set(directory "${CMAKE_BINARY_DIR}/generated/shaders")
    set(outputs)
    foreach(stage vs ps)
        set(spirv "${directory}/${name}.${stage}.metal.spv")
        set(msl "${directory}/${name}.${stage}.metal")
        set(output "${directory}/${name}.${stage}.metal.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
            COMMAND "${WEFT_DXC_EXECUTABLE}" -spirv -T ${stage}_6_0 -E ${stage}_main -O3 -WX
                -fspv-target-env=vulkan1.1 ${WEFT_METAL_SHIFTS} ${defines} -Fo "${spirv}" "${source}"
            COMMAND "${WEFT_SPIRV_CROSS_EXECUTABLE}" "${spirv}" ${WEFT_METAL_CROSS_FLAGS} --output "${msl}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${msl}" "-DOUTPUT=${output}"
                -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
            DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
            COMMENT "Translating ${name} ${stage}_main to MSL"
            VERBATIM)
        list(APPEND outputs "${output}")
    endforeach()
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
endfunction()
