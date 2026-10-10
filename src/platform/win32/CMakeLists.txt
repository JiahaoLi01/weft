# Windows platform services. Font discovery lists installed faces for the portable font catalog.
weft_add_library(platform_win32
    SOURCES system_fonts.cpp
    PUBLIC weft::text
    PRIVATE dwrite)
