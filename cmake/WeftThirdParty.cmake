# Vendored, unmodified and pinned sources (tools/verify_dependencies.py); no FetchContent or network discovery.

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/quickjs-ng/source" third_party/quickjs-ng EXCLUDE_FROM_ALL)

# QuickJS-NG's Unicode tables without the engine: the case mappings of CSS text-transform (weft::text). Its utilities
# (cutils.h) are header-only; the same unmodified source as the engine's.
add_library(weft_unicode_case STATIC "${PROJECT_SOURCE_DIR}/third_party/quickjs-ng/source/libunicode.c")
target_include_directories(weft_unicode_case SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/quickjs-ng/source")

add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/yoga/source/yoga" third_party/yoga EXCLUDE_FROM_ALL)

foreach(feature ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${feature} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/freetype/source" third_party/freetype EXCLUDE_FROM_ALL)

set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/harfbuzz/source" third_party/harfbuzz EXCLUDE_FROM_ALL)

if(MSVC)
    foreach(dependency yogacore freetype harfbuzz)
        target_compile_options(${dependency} PRIVATE /utf-8)
    endforeach()
endif()

# Vulkan C headers only; the Vulkan backend opens the system loader at run time.
add_library(weft_vulkan_headers INTERFACE)
target_include_directories(weft_vulkan_headers SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/third_party/vulkan-headers/source/include")

# libunibreak ships no CMake project; build its line, grapheme and word break sources directly.
set(WEFT_UNIBREAK_SRC "${PROJECT_SOURCE_DIR}/third_party/libunibreak/source/src")
add_library(weft_unibreak STATIC
    "${WEFT_UNIBREAK_SRC}/unibreakbase.c"
    "${WEFT_UNIBREAK_SRC}/unibreakdef.c"
    "${WEFT_UNIBREAK_SRC}/linebreak.c"
    "${WEFT_UNIBREAK_SRC}/linebreakdata.c"
    "${WEFT_UNIBREAK_SRC}/linebreakdef.c"
    "${WEFT_UNIBREAK_SRC}/eastasianwidthdef.c"
    "${WEFT_UNIBREAK_SRC}/emojidef.c"
    "${WEFT_UNIBREAK_SRC}/graphemebreak.c"
    "${WEFT_UNIBREAK_SRC}/wordbreak.c")
target_include_directories(weft_unibreak SYSTEM PUBLIC "${WEFT_UNIBREAK_SRC}")

# lodepng: the PNG decoder of CSS url() background images (bytes the host reads); decoder only, no file I/O, no
# ancillary chunks, the C interface.
add_library(weft_lodepng STATIC "${PROJECT_SOURCE_DIR}/third_party/lodepng/source/lodepng.cpp")
target_include_directories(weft_lodepng SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/lodepng/source")
target_compile_definitions(weft_lodepng PUBLIC
    LODEPNG_NO_COMPILE_ENCODER
    LODEPNG_NO_COMPILE_DISK
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    LODEPNG_NO_COMPILE_CPP)
