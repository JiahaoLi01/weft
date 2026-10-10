#pragma once
#include "caseelephant/constants/scene.hpp"
#include <array>
#include <map>
#include <optional>
#include <string>
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

    const std::array<const char*, constants::scene::paint_property_count>& scene_paint_properties();
    std::optional<PaintValue> parse_scene_paint_value(const std::string&);
    PaintValue interpolate_scene_paint_value(const PaintValue&, const PaintValue&, double);
    ScenePaintValues compile_scene_paint_values(const std::map<std::string, std::string>&);
} // namespace weft
