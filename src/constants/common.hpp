#pragma once

#include <cstdint>

// Values shared by several modules. Module-specific budgets and tuning values live in the
// sibling headers of src/constants/ (see docs/coding-style.md).
//
// Each constant has the type of the literals it replaced (a plain integer literal is int), so
// arithmetic, comparisons and generated code at every use site stay exactly as before.
namespace weft::constants {
    // Initial values used by the M0 CSS profiles and the text probes.
    inline constexpr int default_font_size_px = 16;
    inline constexpr int default_line_height_px = 24;
    inline constexpr const char* default_font_size = "16px";
    inline constexpr const char* default_line_height = "24px";
    inline constexpr const char* default_font_weight = "400";
    inline constexpr const char* default_color = "#000000";
    // Bundled probe fonts: Roboto for Latin text, then the Noto CJK fallback.
    inline constexpr const char* latin_font_family = "ProbeRoboto";
    inline constexpr const char* default_font_family = "ProbeRoboto, ProbeCjk";

    // Units.
    inline constexpr int bytes_per_kib = 1024;
    inline constexpr int bytes_per_mib = 1024 * 1024;
    inline constexpr int milliseconds_per_second = 1000;
    inline constexpr int degrees_per_half_turn = 180;
    inline constexpr int degrees_per_turn = 360;
    // 8-bit color channel maximum (sRGB byte encoding).
    inline constexpr int color_channel_max = 255;
    // CSS percentages are hundredths of the reference length.
    inline constexpr int percent_scale = 100;

    // DOM Node.nodeType values.
    inline constexpr int element_node_type = 1;
    inline constexpr int text_node_type = 3;
    inline constexpr int document_node_type = 9;
    // Unicode scalar range; invalid_codepoint_marker is one past the last code point.
    inline constexpr int max_unicode_codepoint = 0x10ffff;
    inline constexpr int invalid_codepoint_marker = 0x110000;
    // Compound selectors accepted per selector by both the DOM and the style selector engines.
    inline constexpr int max_selector_compounds = 16;
    // Bound on any logical CSS pixel magnitude: coordinates, lengths, extents and wheel deltas.
    inline constexpr int max_logical_px_magnitude = 100000;
    // Projective planes: homogeneous w and determinants below this are singular; a point that projects to infinity
    // maps this far away.
    inline constexpr double plane_singular_epsilon = 1e-10;
    inline constexpr double plane_far_point = 1e10;
    // Perspective planes of the paint IR (paint::Plane): how deep plane contents may nest, and the least depth (w) a
    // plane's corner may have, in front of the viewer.
    inline constexpr unsigned max_plane_depth = 2;
    inline constexpr double plane_min_depth = 1e-3;
    // UI shader effects of the paint IR (paint::Effect): the float4 parameters and the textures one effect may read.
    // The parameters fill the shared shader record's fields that effects leave unused.
    inline constexpr unsigned max_effect_parameters = 16;
    inline constexpr unsigned max_effect_textures = 4;
    // UI particles of the paint IR (paint::Particles): the particles one emission draws, and the frames of a sprite's
    // grid. A particle's random values hash its index modulo particle_index_period, which floats hold exactly.
    inline constexpr unsigned max_particles_per_emission = 65536;
    inline constexpr unsigned max_particle_sprite_frames = 1024;
    inline constexpr unsigned particle_index_period = 1u << 24;
    // Below this drag (per second) particles move as without drag: the drag terms' limits.
    inline constexpr float particle_drag_epsilon = 1e-4f;
    // Largest standard deviation of a backdrop filter's blur (paint::Backdrop), in item px.
    inline constexpr float max_backdrop_blur = 256;
    // Largest viewport edge accepted by the layout and scene profiles.
    inline constexpr int max_viewport_dimension_px = 4096;
    inline constexpr int rgba8_bytes_per_pixel = 4;

    // XML namespaces used by the native DOM.
    inline constexpr const char* xhtml_namespace_uri = "http://www.w3.org/1999/xhtml";
    inline constexpr const char* svg_namespace_uri = "http://www.w3.org/2000/svg";

    // Document bounds shared by style, layout and the page builders: 16,384 nodes holds the CRPG floating UI of 400
    // units (about 33 nodes each with their damage numbers; 2,048 until 13.4 held about 60).
    inline constexpr int max_document_nodes = 16384;
    inline constexpr int max_document_depth = 64;
    // Largest UTF-8 text payload accepted by text, style and layout inputs.
    inline constexpr int max_text_bytes = 65536;

    // Canvas backing limits, shared by the canvas model and its DOM binding.
    inline constexpr int max_canvas_dimension = 2048;
    inline constexpr int max_canvas_pending_commands = 65536;

    // Default logical view used by the page/flow probes, mouse routing and their tests.
    inline constexpr int default_view_width = 640;
    inline constexpr int default_view_height = 420;
    // 4K UHD target used by the visible window probes.
    inline constexpr int uhd_width = 3840;
    inline constexpr int uhd_height = 2160;

    // Deterministic test/probe randomness: Numerical Recipes 32-bit LCG.
    inline constexpr std::uint32_t lcg_multiplier = 1664525u;
    inline constexpr std::uint32_t lcg_increment = 1013904223u;
} // namespace weft::constants
