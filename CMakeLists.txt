cmake_minimum_required(VERSION 3.25)
project(Weft VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(WEFT_BUILD_TESTS "Build Weft behavior tests" ON)
option(WEFT_BUILD_PROBES "Build architecture probes" ON)
option(WEFT_BUILD_SAMPLES "Build host samples (Windows, where the engine host also needs the Vulkan backend; macOS and iOS with the Metal backend)" ON)
# The two builds of the engine (docs/devtools.md): OFF is what a game ships, with no developer tools code at all; ON adds
# them, off until asked for. The public headers and exports are the same in both.
option(WEFT_DEVTOOLS "Build the developer tools into the engine" OFF)
# The engine as one shared library (weft.dll, libweft.dylib), for hosts that take a binary. Only macOS has been built.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(weft_shared_default ON)
else()
    set(weft_shared_default OFF)
endif()
option(WEFT_BUILD_SHARED "Build the engine as one shared library" ${weft_shared_default})

if(APPLE)
    # The Metal backend and the Apple host samples are Objective-C++ (src/render/backends/metal, samples/macos-host,
    # samples/ios-host).
    enable_language(OBJCXX)
    # Newer clang (LLVM 23) sends class messages through objc_msgSendClass$ stubs that only newer linkers make;
    # without them every linker links the code.
    include(CheckOBJCXXCompilerFlag)
    check_objcxx_compiler_flag(-fno-objc-msgsend-class-selector-stubs WEFT_OBJC_CLASS_STUBS_FLAG)
    if(WEFT_OBJC_CLASS_STUBS_FLAG)
        add_compile_options($<$<COMPILE_LANGUAGE:OBJCXX>:-fno-objc-msgsend-class-selector-stubs>)
    endif()
    # Clang contracts a * b + c into fused multiply-adds by default on arm64 (-ffp-contract=on); MSVC on x64, where the
    # layout, geometry and pixel references were produced, rounds each operation. Keep that rounding in every target,
    # the vendored libraries' too (Yoga's flex resolution: 60 - 40 must stay 20, not 19.9999981).
    add_compile_options(-ffp-contract=off)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(WeftTargets)
include(WeftThirdParty)
include(WeftShaders)
include(WeftSpirv)
include(WeftMetal)
include(WeftEffects)
include(WeftExports)

if(WEFT_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()

add_subdirectory(src)
if(WEFT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(WEFT_BUILD_PROBES)
    add_subdirectory(probes)
endif()
if(WEFT_BUILD_SAMPLES AND WIN32)
    add_subdirectory(samples)
elseif(WEFT_BUILD_SAMPLES AND APPLE AND TARGET weft_scene_metal)
    # The Metal host samples: a window on macOS (samples/macos-host, and the in-game UI of samples/macos-ingame), an
    # app on iOS (samples/ios-host).
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        add_subdirectory(samples/ios-host)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_subdirectory(samples/macos-host)
        # In-game UI: pages on world-space panels through the export texture extension.
        add_subdirectory(samples/macos-ingame)
    endif()
endif()
