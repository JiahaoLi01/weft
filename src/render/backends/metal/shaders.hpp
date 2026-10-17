#pragma once

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

    // The passes that blur a backdrop (src/render/shaders/backdrop.hlsl), all over the same fullscreen triangle.
    enum class BackdropPass {
        Area,
        Blur,
        Matrix
    };

    // The shared scene shader's encoded pass (src/render/shaders/scene.hlsl), whose vertex stage an effect program's
    // fragment (paint::EffectProgram::msl_encoded) runs after too.
    ShaderSource scene_shader();
    // Its particle pass (WEFT_PARTICLES).
    ShaderSource particle_shader();
    ShaderSource backdrop_shader(BackdropPass pass);
} // namespace weft::render::metal
