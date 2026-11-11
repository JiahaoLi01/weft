# The engine as one shared library: weft.dll, libweft.dylib, libweft.so. It exports the C ABI and nothing else, the same
# list in the release and dev builds (WEFT_DEVTOOLS), so that a host takes either file without being compiled again.
set(modules weft::core weft::view weft::render)
set(headers weft.h render_backend.h render_export_texture.h render_external_texture.h)
foreach(backend d3d11 d3d12 vulkan metal)
    if(TARGET weft_scene_${backend})
        list(APPEND modules weft::scene_${backend})
        list(APPEND headers render_backend_${backend}.h)
    endif()
endforeach()
weft_collect_exports(names ${headers})

add_library(weft_shared SHARED shared.cpp)
weft_project_options(weft_shared)
target_link_libraries(weft_shared PRIVATE ${modules})
set_target_properties(weft_shared PROPERTIES
    OUTPUT_NAME weft
    # Beside the programs in bin/, on every platform.
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    BUILD_WITH_INSTALL_NAME_DIR ON
    INSTALL_NAME_DIR "@rpath")

set(exports "${CMAKE_CURRENT_BINARY_DIR}/weft_exports")
if(WIN32)
    list(JOIN names "\n    " lines)
    file(GENERATE OUTPUT "${exports}.def" CONTENT "LIBRARY weft\nEXPORTS\n    ${lines}\n")
    target_link_options(weft_shared PRIVATE "/DEF:${exports}.def")
    set_property(TARGET weft_shared APPEND PROPERTY LINK_DEPENDS "${exports}.def")
elseif(APPLE)
    list(JOIN names "\n_" lines)
    file(GENERATE OUTPUT "${exports}.txt" CONTENT "_${lines}\n")
    target_link_options(weft_shared PRIVATE "LINKER:-exported_symbols_list,${exports}.txt" "LINKER:-dead_strip")
    set_property(TARGET weft_shared APPEND PROPERTY LINK_DEPENDS "${exports}.txt")
    target_link_libraries(weft_shared PRIVATE "-framework Foundation" "-framework Metal" "-framework ImageIO"
        "-framework CoreGraphics" "-framework CoreText" "-framework QuartzCore")
else()
    list(JOIN names ";\n    " lines)
    file(GENERATE OUTPUT "${exports}.map" CONTENT "{\n  global:\n    ${lines};\n  local: *;\n};\n")
    target_link_options(weft_shared PRIVATE "LINKER:--version-script=${exports}.map")
    set_property(TARGET weft_shared APPEND PROPERTY LINK_DEPENDS "${exports}.map")
    # A version script does not pull archive members in.
    foreach(name IN LISTS names)
        target_link_options(weft_shared PRIVATE "LINKER:-u,${name}")
    endforeach()
endif()
