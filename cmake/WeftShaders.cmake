# Build-time HLSL compilation with the Windows SDK fxc. The bytecode is embedded in the program, so a running
# engine needs neither the shader source nor d3dcompiler.

if(NOT WIN32)
    return()
endif()

# Prefer the SDK the C++ code is built against: the Visual Studio target platform, else a developer prompt's SDK.
cmake_host_system_information(RESULT weft_kits_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 64_32 ERROR_VARIABLE weft_kits_error)
set(weft_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT weft_sdk_version AND DEFINED ENV{WindowsSDKVersion})
    string(REGEX REPLACE "[\\\\/]$" "" weft_sdk_version "$ENV{WindowsSDKVersion}")
endif()
find_program(WEFT_FXC_EXECUTABLE fxc REQUIRED
    HINTS "${weft_kits_root}/bin/${weft_sdk_version}/x64" "$ENV{WindowsSdkVerBinPath}/x64"
    DOC "Windows SDK HLSL compiler")

# weft_add_hlsl(<target> <name> <source.hlsl> [ALWAYS_OPTIMIZE])
# Compiles vs_main (vs_5_0) and ps_main (ps_5_0) of <source.hlsl> into byte tables included by <target> as
#   "shaders/<name>.vs.inc", "shaders/<name>.ps.inc"              optimized (/O3), for NDEBUG builds;
#   "shaders/<name>.vs.debug.inc", "shaders/<name>.ps.debug.inc"  /Zi /Od, for builds without NDEBUG.
# Both variants use /Ges /WX (strict, warnings are errors). ALWAYS_OPTIMIZE produces only the optimized tables,
# for shaders that every configuration runs optimized.
function(weft_add_hlsl target name source)
    cmake_parse_arguments(PARSE_ARGV 3 arg "ALWAYS_OPTIMIZE" "" "")
    get_filename_component(source "${source}" ABSOLUTE)
    set(directory "${CMAKE_BINARY_DIR}/generated/shaders")
    set(outputs)
    foreach(stage vs ps)
        set(variants release)
        if(NOT arg_ALWAYS_OPTIMIZE)
            list(APPEND variants debug)
        endif()
        foreach(variant ${variants})
            if(variant STREQUAL "release")
                set(flags /O3)
                set(suffix "")
            else()
                set(flags /Zi /Od)
                set(suffix ".debug")
            endif()
            set(bytecode "${directory}/${name}.${stage}${suffix}.cso")
            set(output "${directory}/${name}.${stage}${suffix}.inc")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${directory}"
                COMMAND "${WEFT_FXC_EXECUTABLE}" /nologo /T ${stage}_5_0 /E ${stage}_main /Ges /WX ${flags}
                    /Fo "${bytecode}" "${source}"
                COMMAND "${CMAKE_COMMAND}" "-DINPUT=${bytecode}" "-DOUTPUT=${output}" -DBINARY=ON
                    -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
                DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
                COMMENT "Compiling ${name} ${stage}_main (${variant})"
                VERBATIM)
            list(APPEND outputs "${output}")
        endforeach()
    endforeach()
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
endfunction()
