#pragma once

#include "render/scene_pass.hpp"

#include <string_view>

// The shared shaders of the Metal backend in Metal Shading Language (cmake/WeftMetal.cmake): text the device compiles
// (newLibraryWithSource, MSL 2.1 or later), with each resource at the argument table index constants::render::metal
// names. The translation and its checks run wherever the build has the tools, without Metal.
namespace weft::render::metal {
    // A program's two stages, each a source of its own (both declare the same types): the vertex function vs_main and
    // the fragment function ps_main.
    struct ShaderSource {
        std::string_view vertex;
        std::string_view fragment;
    };

    // The shared scene shader's encoded pass (src/render/shaders/scene.hlsl), whose vertex stage an effect program's
    // fragment (paint::EffectProgram::msl_encoded) runs after too.
    ShaderSource scene_shader();
    // Its pixel stage that composites a blending group over what the target holds where it draws (WEFT_BLEND_FETCH:
    // framebuffer fetch, MSL 2.3), after the scene shader's vertex stage.
    std::string_view scene_fetch_fragment();
    // Its particle pass (WEFT_PARTICLES).
    ShaderSource particle_shader();
    // A backdrop pass (src/render/shaders/backdrop.hlsl), all over the same fullscreen triangle.
    ShaderSource backdrop_shader(BackdropFilter filter);
    // The resolve of an export image (src/render/shaders/export_resolve.hlsl): a fullscreen triangle that writes the
    // scene pass's encoded premultiplied texels linear premultiplied.
    ShaderSource export_resolve_shader();
} // namespace weft::render::metal
