# Android platform services. Font discovery lists the system faces for the portable font catalog (API level 29).
weft_add_library(platform_android
    SOURCES system_fonts.cpp
    PUBLIC weft::text
    PRIVATE android)
