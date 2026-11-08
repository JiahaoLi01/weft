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
if(APPLE)
    # Yoga's defaults (cmake/project-defaults.cmake) build it with -fexceptions -fno-rtti -fvisibility=hidden. Without
    # RTTI clang emits the type info of the standard exceptions its code can throw (std::exception, std::logic_error,
    # std::out_of_range) into Yoga's objects, hidden; the static link binds the whole program's catch clauses to those
    # copies, and Apple's libc++abi matches a catch clause by type info address, so catch (const std::exception&)
    # catches nothing libc++ or weft throws. With RTTI, Yoga refers to libc++abi's. Later options win over Yoga's.
    target_compile_options(yogacore PRIVATE -frtti)
endif()

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

# SheenBidi: the Unicode bidirectional algorithm (UAX #9) and script runs (UAX #24) of the scene's text; its single
# unity compilation unit, as its own CMake builds it by default.
set(WEFT_SHEENBIDI_DIR "${PROJECT_SOURCE_DIR}/third_party/sheenbidi/source")
add_library(weft_sheenbidi STATIC "${WEFT_SHEENBIDI_DIR}/Source/SheenBidi.c")
target_compile_definitions(weft_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(weft_sheenbidi SYSTEM PUBLIC "${WEFT_SHEENBIDI_DIR}/Headers")
target_include_directories(weft_sheenbidi PRIVATE "${WEFT_SHEENBIDI_DIR}/Source")
if(MSVC)
    target_compile_options(weft_sheenbidi PRIVATE /utf-8)
endif()

# lodepng: the PNG decoder of CSS url() background images (bytes the host reads); decoder only, no file I/O, no
# ancillary chunks, the C interface.
add_library(weft_lodepng STATIC "${PROJECT_SOURCE_DIR}/third_party/lodepng/source/lodepng.cpp")
target_include_directories(weft_lodepng SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/lodepng/source")
target_compile_definitions(weft_lodepng PUBLIC
    LODEPNG_NO_COMPILE_ENCODER
    LODEPNG_NO_COMPILE_DISK
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    LODEPNG_NO_COMPILE_CPP)

# libjpeg-turbo 3.1.4.1 (third_party/libjpeg-turbo/README.md): its libjpeg API library, built here because its own
# build system refuses add_subdirectory(). The sources are its CMakeLists.txt's JPEG_SOURCES without arithmetic coding
# (WITH_ARITH_DEC and WITH_ARITH_ENC off: Chrome's libjpeg-turbo has none, and draws such a JPEG as nothing); its
# configuration headers come from its own templates with the values its build would choose. No SIMD (NASM on x86; the C routines give the same pixels for the integer IDCT
# Chrome uses), no TurboJPEG API, tools or tests.
set(WEFT_JPEG_DIR "${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/source")
set(WEFT_JPEG_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/third_party/libjpeg-turbo")
set(JPEG_LIB_VERSION 62)
set(VERSION 3.1.4.1)
set(LIBJPEG_TURBO_VERSION_NUMBER 3001004)
set(COPYRIGHT_YEAR "1991-2026")
set(BUILD "weft")
set(CMAKE_PROJECT_NAME_FOR_JPEG "libjpeg-turbo")
set(D_ARITH_CODING_SUPPORTED 0)
set(C_ARITH_CODING_SUPPORTED 0)
set(WITH_SIMD 0)
set(RIGHT_SHIFT_IS_UNSIGNED 0)
set(SIZE_T ${CMAKE_SIZEOF_VOID_P})
if(MSVC)
    set(HIDDEN "")
    set(INLINE "__forceinline")
    set(THREAD_LOCAL "__declspec(thread)")
    set(HAVE_BUILTIN_CTZL 0)
    set(HAVE_INTRIN_H 1)
else()
    if(WIN32)
        set(HIDDEN "")
    else()
        set(HIDDEN "__attribute__((visibility(\"hidden\")))")
    endif()
    set(INLINE "__inline__ __attribute__((always_inline))")
    set(THREAD_LOCAL "__thread")
    # __builtin_ctzl takes an unsigned long, which is as wide as size_t except on 64-bit Windows.
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(HAVE_BUILTIN_CTZL 0)
    else()
        set(HAVE_BUILTIN_CTZL 1)
    endif()
    set(HAVE_INTRIN_H 0)
endif()
# The templates name the package by CMAKE_PROJECT_NAME, which is weft's here.
set(WEFT_SAVED_PROJECT_NAME "${CMAKE_PROJECT_NAME}")
set(CMAKE_PROJECT_NAME "libjpeg-turbo")
configure_file("${WEFT_JPEG_DIR}/src/jconfig.h.in" "${WEFT_JPEG_CONFIG}/jconfig.h")
configure_file("${WEFT_JPEG_DIR}/src/jconfigint.h.in" "${WEFT_JPEG_CONFIG}/jconfigint.h")
configure_file("${WEFT_JPEG_DIR}/src/jversion.h.in" "${WEFT_JPEG_CONFIG}/jversion.h")
set(CMAKE_PROJECT_NAME "${WEFT_SAVED_PROJECT_NAME}")
foreach(variable JPEG_LIB_VERSION VERSION LIBJPEG_TURBO_VERSION_NUMBER COPYRIGHT_YEAR BUILD D_ARITH_CODING_SUPPORTED
                 C_ARITH_CODING_SUPPORTED WITH_SIMD RIGHT_SHIFT_IS_UNSIGNED SIZE_T HIDDEN INLINE THREAD_LOCAL
                 HAVE_BUILTIN_CTZL HAVE_INTRIN_H CMAKE_PROJECT_NAME_FOR_JPEG WEFT_SAVED_PROJECT_NAME)
    unset(${variable})
endforeach()
set(WEFT_JPEG_SOURCES src/jcapimin.c
    src/wrapper/jcapistd-8.c src/wrapper/jcapistd-12.c src/wrapper/jcapistd-16.c
    src/wrapper/jccoefct-8.c src/wrapper/jccoefct-12.c
    src/wrapper/jccolor-8.c src/wrapper/jccolor-12.c src/wrapper/jccolor-16.c
    src/wrapper/jcdctmgr-8.c src/wrapper/jcdctmgr-12.c
    src/wrapper/jcdiffct-8.c src/wrapper/jcdiffct-12.c src/wrapper/jcdiffct-16.c
    src/jchuff.c src/jcicc.c src/jcinit.c src/jclhuff.c
    src/wrapper/jclossls-8.c src/wrapper/jclossls-12.c src/wrapper/jclossls-16.c
    src/wrapper/jcmainct-8.c src/wrapper/jcmainct-12.c src/wrapper/jcmainct-16.c
    src/jcmarker.c src/jcmaster.c src/jcomapi.c src/jcparam.c src/jcphuff.c
    src/wrapper/jcprepct-8.c src/wrapper/jcprepct-12.c src/wrapper/jcprepct-16.c
    src/wrapper/jcsample-8.c src/wrapper/jcsample-12.c src/wrapper/jcsample-16.c
    src/jctrans.c src/jdapimin.c
    src/wrapper/jdapistd-8.c src/wrapper/jdapistd-12.c src/wrapper/jdapistd-16.c
    src/jdatadst.c src/jdatasrc.c
    src/wrapper/jdcoefct-8.c src/wrapper/jdcoefct-12.c
    src/wrapper/jdcolor-8.c src/wrapper/jdcolor-12.c src/wrapper/jdcolor-16.c
    src/wrapper/jddctmgr-8.c src/wrapper/jddctmgr-12.c
    src/wrapper/jddiffct-8.c src/wrapper/jddiffct-12.c src/wrapper/jddiffct-16.c
    src/jdhuff.c src/jdicc.c src/jdinput.c src/jdlhuff.c
    src/wrapper/jdlossls-8.c src/wrapper/jdlossls-12.c src/wrapper/jdlossls-16.c
    src/wrapper/jdmainct-8.c src/wrapper/jdmainct-12.c src/wrapper/jdmainct-16.c
    src/jdmarker.c src/jdmaster.c
    src/wrapper/jdmerge-8.c src/wrapper/jdmerge-12.c
    src/jdphuff.c
    src/wrapper/jdpostct-8.c src/wrapper/jdpostct-12.c src/wrapper/jdpostct-16.c
    src/wrapper/jdsample-8.c src/wrapper/jdsample-12.c src/wrapper/jdsample-16.c
    src/jdtrans.c src/jerror.c src/jfdctflt.c
    src/wrapper/jfdctfst-8.c src/wrapper/jfdctfst-12.c
    src/wrapper/jfdctint-8.c src/wrapper/jfdctint-12.c
    src/wrapper/jidctflt-8.c src/wrapper/jidctflt-12.c
    src/wrapper/jidctfst-8.c src/wrapper/jidctfst-12.c
    src/wrapper/jidctint-8.c src/wrapper/jidctint-12.c
    src/wrapper/jidctred-8.c src/wrapper/jidctred-12.c
    src/jmemmgr.c src/jmemnobs.c src/jpeg_nbits.c
    src/wrapper/jquant1-8.c src/wrapper/jquant1-12.c
    src/wrapper/jquant2-8.c src/wrapper/jquant2-12.c
    src/wrapper/jutils-8.c src/wrapper/jutils-12.c src/wrapper/jutils-16.c)
list(TRANSFORM WEFT_JPEG_SOURCES PREPEND "${WEFT_JPEG_DIR}/")
add_library(weft_jpeg STATIC ${WEFT_JPEG_SOURCES})
target_include_directories(weft_jpeg SYSTEM PUBLIC "${WEFT_JPEG_DIR}/src" "${WEFT_JPEG_CONFIG}")
if(MSVC)
    target_compile_definitions(weft_jpeg PRIVATE _CRT_SECURE_NO_DEPRECATE)
endif()

# libwebp 1.6.0 (third_party/libwebp/README.md): its webp library (VP8, VP8L and alpha decoding; its encoder links in
# only what is used) and webpdemux (an animated WebP's frames, of which a page's image shows the first), its SIMD paths
# chosen at run time as upstream builds them (libwebp keeps them bit-exact with its C code), without threads; no mux,
# tools or extras.
foreach(option ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS WEBP_JS FUZZTEST)
    set(WEBP_BUILD_${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)
set(WEBP_NEAR_LOSSLESS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/libwebp/source" third_party/libwebp EXCLUDE_FROM_ALL)
# Its public headers as <webp/decode.h> and <webp/demux.h>.
target_include_directories(webpdemux SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/libwebp/source/src>")
