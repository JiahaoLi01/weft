# A release library has no developer tools in it, and a dev library does: looked for in the file, not asked of it.
# cmake -DBINARY=<library> -DEXPECT=dev|release -P devtools_binary_check.cmake
foreach(mark "Protocol-Version" "WEFT_DEVTOOLS_LISTEN" "WEFT_DEVTOOLS_SOCKET" "Overlay.setInspectMode"
        "Weft.getCompatibilityReport" "Weft.setPage" "CSS.setStyleSheetText")
    file(STRINGS "${BINARY}" found REGEX "${mark}" LIMIT_COUNT 1)
    if(EXPECT STREQUAL "dev" AND NOT found)
        message(FATAL_ERROR "${BINARY} is a dev build without \"${mark}\"")
    elseif(EXPECT STREQUAL "release" AND found)
        message(FATAL_ERROR "${BINARY} is a release build with \"${mark}\"")
    endif()
endforeach()
