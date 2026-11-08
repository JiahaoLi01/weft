#include "render/backends/metal/shaders.hpp"

#include <cstddef>

namespace weft::render::metal {
    namespace {
        // Each stage's MSL with a terminating 0 (cmake/WeftMetal.cmake).
        constexpr unsigned char scene_vertex[] = {
#include "shaders/scene_encoded.vs.metal.inc"
        };
        constexpr unsigned char scene_fragment[] = {
#include "shaders/scene_encoded.ps.metal.inc"
        };
        constexpr unsigned char fetch_fragment[] = {
#include "shaders/scene_fetch_encoded.ps.metal.inc"
        };
        constexpr unsigned char particle_vertex[] = {
#include "shaders/scene_particles_encoded.vs.metal.inc"
        };
        constexpr unsigned char particle_fragment[] = {
#include "shaders/scene_particles_encoded.ps.metal.inc"
        };
        // The backdrop passes share the fullscreen triangle.
        constexpr unsigned char backdrop_vertex[] = {
#include "shaders/backdrop_blur.vs.metal.inc"
        };
        constexpr unsigned char area_fragment[] = {
#include "shaders/backdrop_area.ps.metal.inc"
        };
        constexpr unsigned char blur_fragment[] = {
#include "shaders/backdrop_blur.ps.metal.inc"
        };
        constexpr unsigned char matrix_fragment[] = {
#include "shaders/backdrop_matrix.ps.metal.inc"
        };
        constexpr unsigned char resolve_vertex[] = {
#include "shaders/export_resolve.vs.metal.inc"
        };
        constexpr unsigned char resolve_fragment[] = {
#include "shaders/export_resolve.ps.metal.inc"
        };

        template <std::size_t N>
        std::string_view text(const unsigned char (&bytes)[N]) {
            return {reinterpret_cast<const char*>(bytes), N - 1};
        }
    } // namespace

    ShaderSource scene_shader() {
        return {text(scene_vertex), text(scene_fragment)};
    }

    std::string_view scene_fetch_fragment() {
        return text(fetch_fragment);
    }

    ShaderSource particle_shader() {
        return {text(particle_vertex), text(particle_fragment)};
    }

    ShaderSource backdrop_shader(BackdropFilter filter) {
        switch (filter) {
            case BackdropFilter::Area:
                return {text(backdrop_vertex), text(area_fragment)};
            case BackdropFilter::Blur:
                return {text(backdrop_vertex), text(blur_fragment)};
            case BackdropFilter::Matrix:
                break;
        }
        return {text(backdrop_vertex), text(matrix_fragment)};
    }

    ShaderSource export_resolve_shader() {
        return {text(resolve_vertex), text(resolve_fragment)};
    }
} // namespace weft::render::metal
