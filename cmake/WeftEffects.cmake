# UI shader effects (docs/architecture.md, "UI 着色器效果、粒子与背景拷贝"). An effect's HLSL source defines
#   float4 effect(EffectInput input)
# and compiles with the shared scene shader (src/render/shaders/scene.hlsl, WEFT_EFFECT) into the pixel stage of each
# backend built here, for both composition spaces: DXBC (fxc ps_5_0) for D3D11 and SPIR-V (DXC ps_6_0) for Vulkan,
# encoded and linear, and with WEFT_SHADERS_METAL the encoded pass in MSL for Metal (cmake/WeftMetal.cmake); always
# optimized, warnings as errors. Sources that declare resources or uniform globals are refused
# (cmake/check_effect.cmake): an effect reads its input and the textures the shared shader declares, nothing else.
#
# weft_add_effect(<target> <name> <source.hlsl>
#                 PARAMETERS <count> | PARAMETERS <parameter>:<type>[=<initial>]...
#                 TEXTURES <count> | TEXTURES <texture>...
#                 [ANIMATED] [BACKDROP <margin>])
# Adds the program to <target>, declared in the generated header "effects/<name>.hpp":
#   const std::shared_ptr<const weft::paint::EffectProgram>& weft::effects::<name>();
# <name> is a C++ identifier. PARAMETERS (float4 each, at most 16) and TEXTURES (at most 4) are what the function reads,
# by count for a program that only display lists draw, or by name for CSS (paint::EffectParameter): CSS sets parameter
# <parameter> with the custom property --<parameter>, a value of <type> (number, vec2, vec3, vec4 or color), or
# <initial> when none is set, CSS text with commas for spaces (7.2,2.8; quote one with a #: "tint:color=#9ca3af"). A
# host registers the named textures with the program. ANIMATED: the function reads the time. BACKDROP: the function
# reads what lies behind its element (docs/effects.md), from a copy that reaches <margin> CSS px (0 to 256) past its
# box.
function(weft_add_effect target name source)
    cmake_parse_arguments(PARSE_ARGV 3 arg "ANIMATED" "BACKDROP" "PARAMETERS;TEXTURES")
    if(NOT name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "weft_add_effect: ${name} is not an identifier")
    endif()
    # The manifest: counts, and the named parameters and textures as C++ initializers.
    set(named_parameters "")
    set(named_textures "")
    if(arg_PARAMETERS MATCHES "^[0-9]+$")
        set(parameter_count ${arg_PARAMETERS})
    else()
        list(LENGTH arg_PARAMETERS parameter_count)
        foreach(parameter IN LISTS arg_PARAMETERS)
            if(NOT parameter MATCHES "^([A-Za-z_][A-Za-z0-9_-]*):(number|vec2|vec3|vec4|color)(=(.*))?$")
                message(FATAL_ERROR "weft_add_effect: ${name} parameter ${parameter} is not <name>:<type>[=<initial>]")
            endif()
            set(type_number Number)
            set(type_vec2 Vector2)
            set(type_vec3 Vector3)
            set(type_vec4 Vector4)
            set(type_color Color)
            set(type "${type_${CMAKE_MATCH_2}}")
            set(parameter_name "${CMAKE_MATCH_1}")
            string(REPLACE "," " " initial "${CMAKE_MATCH_4}")
            string(REPLACE "\\" "\\\\" initial "${initial}")
            string(REPLACE "\"" "\\\"" initial "${initial}")
            string(APPEND named_parameters "                {\"${parameter_name}\", "
                "weft::paint::EffectParameter::Type::${type}, \"${initial}\"},\n")
        endforeach()
    endif()
    if(arg_TEXTURES MATCHES "^[0-9]+$")
        set(texture_count ${arg_TEXTURES})
    else()
        list(LENGTH arg_TEXTURES texture_count)
        foreach(texture IN LISTS arg_TEXTURES)
            if(NOT texture MATCHES "^[A-Za-z_][A-Za-z0-9_-]*$")
                message(FATAL_ERROR "weft_add_effect: ${name} texture ${texture} is not a name")
            endif()
            string(APPEND named_textures "\"${texture}\", ")
        endforeach()
    endif()
    if(parameter_count GREATER 16 OR texture_count GREATER 4)
        message(FATAL_ERROR "weft_add_effect: ${name} reads at most 16 parameters and 4 textures")
    endif()
    set(manifest "")
    if(named_parameters)
        string(APPEND manifest "            p->named_parameters = {\n${named_parameters}            };\n")
    endif()
    if(named_textures)
        string(APPEND manifest "            p->named_textures = {${named_textures}};\n")
    endif()
    if(arg_ANIMATED)
        string(APPEND manifest "            p->animated = true;\n")
    endif()
    if(DEFINED arg_BACKDROP)
        if(NOT arg_BACKDROP MATCHES "^[0-9]+(\\.[0-9]+)?$" OR arg_BACKDROP GREATER 256)
            message(FATAL_ERROR "weft_add_effect: ${name} BACKDROP takes a margin from 0 to 256 CSS px")
        endif()
        string(APPEND manifest "            p->backdrop = true;\n"
            "            p->backdrop_margin = float(${arg_BACKDROP});\n")
    endif()
    get_filename_component(source "${source}" ABSOLUTE)
    set(scene "${PROJECT_SOURCE_DIR}/src/render/shaders/scene.hlsl")
    set(directory "${CMAKE_BINARY_DIR}/generated/effects")
    set(combined "${directory}/${name}.hlsl")
    file(GENERATE OUTPUT "${combined}" CONTENT "#define WEFT_EFFECT 1\n#include \"${scene}\"\n#include \"${source}\"\n")
    set(checked "${directory}/${name}.checked")
    add_custom_command(OUTPUT "${checked}"
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${source}" "-DSTAMP=${checked}"
            -P "${PROJECT_SOURCE_DIR}/cmake/check_effect.cmake"
        DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/cmake/check_effect.cmake"
        COMMENT "Checking effect ${name}"
        VERBATIM)
    set(outputs "${checked}")
    set(tables "")
    set(fills "")
    foreach(space encoded linear)
        set(fxc_defines)
        set(dxc_defines)
        if(space STREQUAL "linear")
            set(fxc_defines /D SPACE_LINEAR=1)
            set(dxc_defines -D SPACE_LINEAR=1)
        endif()
        set(depends "${combined}" "${scene}" "${source}" "${checked}" "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake")
        if(WEFT_FXC_EXECUTABLE)
            set(bytecode "${directory}/${name}.${space}.ps.cso")
            set(output "${directory}/${name}.${space}.ps.inc")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${WEFT_FXC_EXECUTABLE}" /nologo /T ps_5_0 /E ps_main /Ges /WX /O3 ${fxc_defines}
                    /Fo "${bytecode}" "${combined}"
                COMMAND "${CMAKE_COMMAND}" "-DINPUT=${bytecode}" "-DOUTPUT=${output}" -DBINARY=ON
                    -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
                DEPENDS ${depends}
                COMMENT "Compiling effect ${name} (D3D11, ${space})"
                VERBATIM)
            list(APPEND outputs "${output}")
            string(APPEND tables "        constexpr unsigned char d3d11_${space}[] = {\n"
                "#include \"effects/${name}.${space}.ps.inc\"\n        };\n")
            string(APPEND fills "                p->d3d11_${space}.assign(std::begin(d3d11_${space}), "
                "std::end(d3d11_${space}));\n")
        endif()
        if(WEFT_RENDER_VULKAN)
            set(spirv "${directory}/${name}.${space}.ps.spv")
            set(output "${directory}/${name}.${space}.ps.spv.inc")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${WEFT_DXC_EXECUTABLE}" -spirv -T ps_6_0 -E ps_main -O3 -WX -fspv-target-env=vulkan1.1
                    ${WEFT_SPIRV_SHIFTS} ${dxc_defines} -Fo "${spirv}" "${combined}"
                COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spirv}" "-DOUTPUT=${output}" -DBINARY=ON
                    -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
                DEPENDS ${depends}
                COMMENT "Compiling effect ${name} (SPIR-V, ${space})"
                VERBATIM)
            list(APPEND outputs "${output}")
            string(APPEND tables "        constexpr unsigned char spirv_${space}[] = {\n"
                "#include \"effects/${name}.${space}.ps.spv.inc\"\n        };\n")
            string(APPEND fills "                p->spirv_${space}.assign(std::begin(spirv_${space}), "
                "std::end(spirv_${space}));\n")
        endif()
        # Metal draws View frames alone, in the encoded composition.
        if(WEFT_SHADERS_METAL AND space STREQUAL "encoded")
            set(spirv "${directory}/${name}.encoded.ps.metal.spv")
            set(msl "${directory}/${name}.encoded.ps.metal")
            set(output "${directory}/${name}.encoded.ps.metal.inc")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${WEFT_DXC_EXECUTABLE}" -spirv -T ps_6_0 -E ps_main -O3 -WX -fspv-target-env=vulkan1.1
                    ${WEFT_METAL_SHIFTS} ${dxc_defines} -Fo "${spirv}" "${combined}"
                COMMAND "${WEFT_SPIRV_CROSS_EXECUTABLE}" "${spirv}" ${WEFT_METAL_CROSS_FLAGS} --output "${msl}"
                COMMAND "${CMAKE_COMMAND}" "-DINPUT=${msl}" "-DOUTPUT=${output}"
                    -P "${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake"
                DEPENDS ${depends}
                COMMENT "Translating effect ${name} to MSL (encoded)"
                VERBATIM)
            list(APPEND outputs "${output}")
            string(APPEND tables "        constexpr unsigned char msl_encoded[] = {\n"
                "#include \"effects/${name}.encoded.ps.metal.inc\"\n        };\n")
            string(APPEND fills "                p->msl_encoded.assign(reinterpret_cast<const char*>(msl_encoded), "
                "sizeof msl_encoded - 1);\n")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${directory}/${name}.hpp" CONTENT "// Generated by cmake/WeftEffects.cmake from ${source}.
#pragma once

#include \"paint/display_list.hpp\"

#include <memory>

namespace weft::effects {
    // The UI shader effect ${name}: ${parameter_count} parameters, ${texture_count} textures.
    const std::shared_ptr<const paint::EffectProgram>& ${name}();
} // namespace weft::effects
")
    file(GENERATE OUTPUT "${directory}/${name}.cpp" CONTENT "// Generated by cmake/WeftEffects.cmake from ${source}.
#include \"effects/${name}.hpp\"

#include <iterator>

namespace weft::effects {
    namespace {
${tables}    } // namespace

    const std::shared_ptr<const paint::EffectProgram>& ${name}() {
        static const std::shared_ptr<const paint::EffectProgram> program = [] {
            auto p = std::make_shared<paint::EffectProgram>();
            p->name = \"${name}\";
            p->parameters = ${parameter_count};
            p->textures = ${texture_count};
${manifest}${fills}            return p;
        }();
        return program;
    }
} // namespace weft::effects
")
    target_sources(${target} PRIVATE ${outputs} "${directory}/${name}.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
endfunction()
