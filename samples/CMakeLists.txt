# Host samples: how an engine uses Weft through the public headers (include/weft).
if(WEFT_RENDER_VULKAN)
    add_subdirectory(engine-host)
endif()
add_subdirectory(page-runner)
add_subdirectory(effects)
