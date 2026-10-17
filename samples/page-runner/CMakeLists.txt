# A game UI page run without the game, through the public headers only (include/weft): a timeline of engine events,
# view models, anchors and pointer input, the page's diagnostics and messages printed, frames drawn on D3D11 into PNGs.
# How a port of a game's UI onto weft finds what the page needs before the engine runs it.
add_executable(weft_page_runner page_runner.cpp)
weft_project_options(weft_page_runner)
target_link_libraries(weft_page_runner PRIVATE weft::core weft::view weft::scene_d3d11 d3d11 dxgi)
if(MSVC)
    # The View's owner thread: 8 MiB of stack (include/weft/weft.h).
    target_link_options(weft_page_runner PRIVATE /STACK:8388608)
endif()
