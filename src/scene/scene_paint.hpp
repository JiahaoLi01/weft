#pragma once
#include "base/result.hpp"
#include "constants/scene.hpp"
#include "style/values.hpp"
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weft {
    // Owned API-neutral paint parameters. Strings remain the diagnostic/reference
    // scene representation; adapters can consume numeric samples directly.
    struct PaintNumber {
        double absolute{};
        double relative{};
        friend bool operator==(const PaintNumber&, const PaintNumber&) = default;
    };
    enum class PaintOp {
        Translate,
        TranslateX,
        TranslateY,
        Scale,
        ScaleX,
        ScaleY,
        Rotate,
        Matrix,
        Perspective,
        RotateY
    };

    struct PaintOperation {
        PaintOp kind{};
        std::array<PaintNumber, 6> args{};
        unsigned count{};
        friend bool operator==(const PaintOperation&, const PaintOperation&) = default;
    };

    struct PaintValue {
        enum Kind {
            Scalar,
            Color,
            Transform
        } kind{};

        PaintNumber scalar;
        std::array<double, 4> color{};
        std::vector<PaintOperation> operations;
        friend bool operator==(const PaintValue&, const PaintValue&) = default;
    };

    struct ScenePaintValues {
        // opacity, transform, background, text color, top/right/bottom/left colors;
        // indexed by the constants::scene::paint_slot_* values.
        std::array<std::optional<PaintValue>, constants::scene::paint_property_count> values;
    };

    // The functions of a CSS transform value (svg = false) or an SVG transform attribute (svg = true, where unitless
    // angles are degrees, rotate takes an optional center and commas may separate functions): translate[XY],
    // scale[XY], rotate, matrix, perspective and rotateY. Lengths stay unresolved; percentages are fractions in
    // relative. The one transform grammar of the scene; outside it, Diag::SceneRaster.
    Result<std::vector<PaintOperation>> parse_scene_transform(std::string_view text, bool svg = false);

    const std::array<const char*, constants::scene::paint_property_count>& scene_paint_properties();
    std::optional<PaintValue> parse_scene_paint_value(const std::string&);
    PaintValue interpolate_scene_paint_value(const PaintValue&, const PaintValue&, double);
    ScenePaintValues compile_scene_paint_values(const StyleValues&);
    // The same values, those of slots whose text `before` gave as style does taken from `last` (compiled from
    // before): an animation or a moved element parses only what changed.
    ScenePaintValues compile_scene_paint_values(const StyleValues& style, const StyleValues& before,
                                                const ScenePaintValues& last);
} // namespace weft
