#pragma once

#include "dom/text_field.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Text fields in a scene (dom/text_field.hpp, SceneNode::field): what a field's node shows of its state, and where its
// value's offsets lie on its lines.
namespace weft {
    // The text a field shows: its placeholder while its value is empty (placeholder set to true), else its value, a
    // password's characters as bullets (U+2022, as Chrome's -webkit-text-security: disc draws them).
    std::string scene_field_text(const TextFieldState& f, bool& placeholder);
    // An offset of the value as an offset of the text the field shows, and back.
    std::uint32_t scene_field_shown(const TextFieldState& f, std::uint32_t value_offset);
    std::uint32_t scene_field_value(const TextFieldState& f, std::uint32_t shown_offset);

    // The caret stops of a line of a field's text: for each character boundary of its text (byte offsets from start
    // to start + text size), its x from the line box's left, from the glyphs of its shaped run (x and advance by UTF-8
    // cluster; a cluster of several characters splits its advance evenly).
    std::vector<std::pair<std::uint32_t, float>> scene_field_stops(const SceneTextRun& run, std::string_view text,
                                                                   std::uint32_t start);

    // Where a shown offset lies on a field's lines: the line (the last whose text starts at or before it) and the x
    // (node coordinates) of its stop, the nearest one at or before it. None without lines.
    struct SceneFieldCaret {
        std::size_t line{};
        float x{};
    };
    std::optional<SceneFieldCaret> scene_field_caret(const SceneNode& n, std::uint32_t shown_offset);
    // The shown offset of the stop nearest a point (node coordinates) on the line nearest it vertically.
    std::uint32_t scene_field_offset_at(const SceneNode& n, float x, float y);
} // namespace weft
