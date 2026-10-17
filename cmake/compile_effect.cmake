# Compiles a UI shader effect for a host that registers it at run time (weft_runtime_add_effect): the HLSL pixel
# function (docs/architecture.md, "UI 着色器效果") with weft's shell, to DXBC (ps_5_0, for D3D11 and D3D12) and, with the
# DXC of the Vulkan SDK, to SPIR-V (ps_6_0, for Vulkan), each for the scene pass's encoded and the document pass's
# linear composition space, as cmake/WeftEffects.cmake compiles the effects weft's own targets embed. The source is
# checked first (cmake/check_effect.cmake).
#
#   cmake -DSOURCE=<effect.hlsl> -DOUTPUT=<dir>/<name> [-DFXC=<fxc.exe>] [-DDXC=<dxc.exe>] -P compile_effect.cmake
#
# writes <name>.encoded.dxbc and <name>.linear.dxbc, and with a DXC (given, or found in the Vulkan SDK)
# <name>.encoded.spv and <name>.linear.spv. Without FXC it looks in the newest Windows 10 SDK. A host may ship this file
# with check_effect.cmake and the shell scene.hlsl beside it, without weft's source tree.
cmake_minimum_required(VERSION 3.25)
if(NOT SOURCE OR NOT OUTPUT)
    message(FATAL_ERROR "compile_effect.cmake requires -DSOURCE=<effect.hlsl> and -DOUTPUT=<dir>/<name>")
endif()
get_filename_component(SOURCE "${SOURCE}" ABSOLUTE)
get_filename_component(OUTPUT "${OUTPUT}" ABSOLUTE)
get_filename_component(directory "${OUTPUT}" DIRECTORY)
get_filename_component(weft "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# Beside this file when a host ships the three, else in weft's source tree.
set(shell "${CMAKE_CURRENT_LIST_DIR}/scene.hlsl")
set(check "${CMAKE_CURRENT_LIST_DIR}/check_effect.cmake")
if(NOT EXISTS "${shell}")
    set(shell "${weft}/src/render/shaders/scene.hlsl")
endif()
if(NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "compile_effect.cmake: no ${SOURCE}")
endif()

if(NOT FXC)
    file(GLOB kits "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/fxc.exe")
    list(SORT kits COMPARE NATURAL ORDER DESCENDING)
    list(GET kits 0 FXC)
    if(NOT FXC OR NOT EXISTS "${FXC}")
        message(FATAL_ERROR "compile_effect.cmake: no fxc; give -DFXC=<fxc.exe> of a Windows 10 SDK")
    endif()
endif()
if(NOT DXC)
    file(GLOB sdks "$ENV{VULKAN_SDK}/Bin/dxc.exe" "C:/VulkanSDK/*/Bin/dxc.exe")
    if(sdks)
        list(SORT sdks COMPARE NATURAL ORDER DESCENDING)
        list(GET sdks 0 DXC)
    endif()
endif()

file(MAKE_DIRECTORY "${directory}")
set(stamp "${OUTPUT}.checked")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${SOURCE}" "-DSTAMP=${stamp}"
    -P "${check}" RESULT_VARIABLE checked)
file(REMOVE "${stamp}")
if(NOT checked EQUAL 0)
    message(FATAL_ERROR "compile_effect.cmake: ${SOURCE} is no effect source")
endif()
set(combined "${OUTPUT}.combined.hlsl")
file(WRITE "${combined}" "#define WEFT_EFFECT 1\n#include \"${shell}\"\n#include \"${SOURCE}\"\n")

foreach(space encoded linear)
    set(fxc_defines)
    set(dxc_defines)
    if(space STREQUAL "linear")
        set(fxc_defines /D SPACE_LINEAR=1)
        set(dxc_defines -D SPACE_LINEAR=1)
    endif()
    execute_process(COMMAND "${FXC}" /nologo /T ps_5_0 /E ps_main /Ges /WX /O3 ${fxc_defines}
            /Fo "${OUTPUT}.${space}.dxbc" "${combined}"
        RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE log)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "compile_effect.cmake: fxc (${space}) failed:\n${log}")
    endif()
    if(DXC)
        # The register shifts of the Vulkan backend's descriptor set (cmake/WeftSpirv.cmake).
        execute_process(COMMAND "${DXC}" -spirv -T ps_6_0 -E ps_main -O3 -WX -fspv-target-env=vulkan1.1
                -fvk-b-shift 0 0 -fvk-t-shift 10 0 -fvk-s-shift 20 0 ${dxc_defines}
                -Fo "${OUTPUT}.${space}.spv" "${combined}"
            RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE log)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "compile_effect.cmake: dxc (${space}) failed:\n${log}")
        endif()
    endif()
endforeach()
file(REMOVE "${combined}")
if(DXC)
    message(STATUS "compile_effect.cmake: ${OUTPUT}.{encoded,linear}.{dxbc,spv}")
else()
    message(STATUS "compile_effect.cmake: ${OUTPUT}.{encoded,linear}.dxbc (no DXC with SPIR-V: no Vulkan bytecode)")
endif()
