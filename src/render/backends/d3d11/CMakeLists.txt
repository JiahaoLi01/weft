# D3D11 reference renderer, its C ABI adapter, and the synchronous and two-frame sample hosts.
weft_add_library(render_d3d11
    SOURCES renderer.cpp backend.cpp two_frame_pipeline.cpp
    PUBLIC weft::render d3d11
    PRIVATE dxgi)
weft_add_hlsl(weft_render_d3d11 quad quad.hlsl)
