# The developer tools (docs/devtools.md). Built only with WEFT_DEVTOOLS: a release build does not compile this directory.
weft_add_library(devtools SOURCES server.cpp json.cpp agent.cpp PUBLIC weft::base weft::view PRIVATE weft::bridge)
target_compile_definitions(weft_devtools PRIVATE WEFT_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(weft_devtools PRIVATE ws2_32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(weft_devtools PRIVATE Threads::Threads)
