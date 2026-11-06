# macOS platform services, which iOS shares. Font discovery lists the system faces for the portable font catalog:
# CoreText lists them, FreeType finds their collection indices and names.
weft_add_library(platform_macos
    SOURCES system_fonts.cpp
    PUBLIC weft::text
    PRIVATE freetype "-framework CoreText" "-framework CoreFoundation")
