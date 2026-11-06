# C ABI runtime behind include/weft/weft.h: Runtimes and their Views (src/view); owner_executor.hpp is header-only.
weft_add_library(core SOURCES runtime.cpp PUBLIC weft::base PRIVATE weft::view)
if(WIN32)
    target_link_libraries(weft_core PRIVATE weft::platform_win32)
elseif(ANDROID)
    target_link_libraries(weft_core PRIVATE weft::platform_android)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(weft_core PRIVATE weft::platform_macos)
endif()
