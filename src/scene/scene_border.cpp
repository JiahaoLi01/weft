#include "scene/scene_border.hpp"

#include <algorithm>
#include <cmath>

namespace weft {
    namespace {
        // Dash and gap of a stroke, phase 0; none draws the line whole.
        struct Pattern {
            bool none{};
            float dash{};
            float gap{};
            bool round{};
        };

        // StyledStrokeData::SelectBestDashGap for an open path: the gap nearest the nominal one with which a whole
        // number of dashes start and end the path.
        float best_gap(float length, float dash, float gap) {
            const float available = length + gap;
            const float min_dashes = std::floor(available / (dash + gap));
            const float max_dashes = min_dashes + 1;
            const float min_gap = (length - min_dashes * dash) / (min_dashes - 1);
            const float max_gap = (length - max_dashes * dash) / (max_dashes - 1);
            return max_gap <= 0 || std::abs(min_gap - gap) < std::abs(max_gap - gap) ? min_gap : max_gap;
        }

        // StyledStrokeData's dash effect for a line of `length` device px drawn `width` px thick.
        Pattern pattern(float length, int width, SceneBorderStyle style) {
            const bool dashed = style == SceneBorderStyle::Dashed;
            if (dashed || width <= 3) {
                float dash = float(width);
                float gap = float(width);
                if (dashed) {
                    dash *= width >= 3 ? 2.f : 3.f;
                    gap *= width >= 3 ? 1.f : 2.f;
                }
                if (length <= dash * 2) {
                    return {true};
                }
                // Exactly two dashes, proportionally sized.
                const float two = 2 * dash + gap;
                if (length <= two) {
                    const float m = length / two;
                    return {false, dash * m, gap * m};
                }
                if (dashed) {
                    gap = best_gap(length, dash, gap);
                }
                return {false, dash, gap};
            }
            const float per_dot = 2.f * float(width);
            if (length < per_dot) {
                return {false, 0, per_dot, true};
            }
            // The epsilon keeps a whole dot at the end of the line. A line with room for only one gap has none that
            // fits (its gap is infinite): Skia then draws it whole, with its round caps.
            constexpr float epsilon = 1e-2f;
            const float gap = best_gap(length, float(width), float(width));
            if (!std::isfinite(gap)) {
                return {true, 0, 0, true};
            }
            return {false, 0, gap + float(width) - epsilon, true};
        }
    } // namespace

    bool scene_border_style(std::string_view value, SceneBorderStyle& out) {
        if (value == "solid") {
            out = SceneBorderStyle::Solid;
        } else if (value == "dotted") {
            out = SceneBorderStyle::Dotted;
        } else if (value == "dashed") {
            out = SceneBorderStyle::Dashed;
        } else if (value == "none" || value == "hidden") {
            out = SceneBorderStyle::None;
        } else {
            return false;
        }
        return true;
    }

    std::vector<SceneDash> scene_dashed_side(std::array<float, 2> size, unsigned side, float width,
                                             SceneBorderStyle style, const std::array<double, 4>& device) {
        std::vector<SceneDash> out;
        if (width <= 0 || (style != SceneBorderStyle::Dotted && style != SceneBorderStyle::Dashed)) {
            return out;
        }
        const bool horizontal = side == 0 || side == 2;
        // The box in device px, snapped as Chrome snaps the border box (ToPixelSnappedRect).
        const double x0 = device[2];
        const double y0 = device[3];
        const double x1 = size[0] * device[0] + device[2];
        const double y1 = size[1] * device[1] + device[3];
        const int left = int(std::lround(std::min(x0, x1)));
        const int top = int(std::lround(std::min(y0, y1)));
        const int right = int(std::lround(std::max(x0, x1)));
        const int bottom = int(std::lround(std::max(y0, y1)));
        // Border widths are whole device px, at least one.
        const double scale = std::abs(horizontal ? device[1] : device[0]);
        const int w = std::max(1, int(std::floor(width * scale + 1e-4)));
        // The side's rect, then DrawDashedOrDottedBoxSide's line along its middle.
        int sx1 = left;
        int sy1 = top;
        int sx2 = right;
        int sy2 = bottom;
        switch (side) {
            case 0:
                sy2 = top + w;
                break;
            case 1:
                sx1 = right - w;
                break;
            case 2:
                sy1 = bottom - w;
                break;
            default:
                sx2 = left + w;
                break;
        }
        const int length = horizontal ? sx2 - sx1 : sy2 - sy1;
        if (length <= 0) {
            return out;
        }
        // Device shapes: rects, round dots.
        std::vector<SceneDash> shapes;
        // The line from p1 to p2 along one axis (a along, c across).
        float a1 = float(horizontal ? sx1 : sy1);
        float a2 = float(horizontal ? sx2 : sy2);
        float c = float(horizontal ? sy1 + w / 2 : sx1 + w / 2);
        const auto rect = [&](float along0, float along1, float across0, float across1, bool round) {
            const float lo = std::min(along0, along1);
            const float hi = std::max(along0, along1);
            shapes.push_back({horizontal ? std::array<float, 4>{lo, across0, hi - lo, across1 - across0}
                                         : std::array<float, 4>{across0, lo, across1 - across0, hi - lo},
                              round});
        };
        const auto p = pattern(float(length), w, style);
        if (style == SceneBorderStyle::Dotted && w <= 3) {
            // EnforceDotsAtEndpoints: whole dots at the ends where the pattern would leave a gap or a part of one.
            const int mod4 = length % 4;
            const int mod6 = length % 6;
            bool start_dot{};
            int start_growth{};
            int start_offset{};
            bool end_dot{};
            int end_growth{};
            if ((w == 1 && length % 2 == 0) || (w == 3 && mod6 == 0)) {
                start_dot = true;
                start_growth = 1;
                start_offset = 1;
            }
            if ((w == 2 && (mod4 == 0 || mod4 == 1)) || (w == 3 && (mod6 == 1 || mod6 == 2))) {
                start_dot = true;
                start_offset = -1;
            }
            if ((w == 2 && mod4 == 0) || (w == 3 && mod6 == 1)) {
                end_dot = true;
            }
            if ((w == 2 && mod4 == 3) || (w == 3 && (mod6 == 4 || mod6 == 5))) {
                start_dot = true;
                start_offset = 1;
            }
            if (w == 3 && mod6 == 5) {
                end_dot = true;
            } else if (w == 3 && mod6 == 0) {
                end_dot = true;
                end_growth = 1;
            }
            const float across0 = c - float(w / 2);
            const float across1 = c + float(w - w / 2);
            if (start_dot) {
                rect(a1, a1 + float(w + start_growth), across0, across1, false);
                a1 += float(2 * w + start_offset);
            }
            if (end_dot) {
                rect(a2 - float(w + end_growth), a2, across0, across1, false);
                a2 -= float(w + end_growth + 1);
            }
        } else if (style == SceneBorderStyle::Dotted) {
            // Round dots: zero-length dashes with round caps, the line moved in by half a width at each end.
            a1 += float(w) / 2;
            a2 -= float(w) / 2;
        }
        // An odd width centres the stroke on the pixel rows (or columns) it covers.
        if (w % 2) {
            c += .5f;
        }
        const float half = float(w) / 2;
        const float direction = a2 >= a1 ? 1.f : -1.f;
        const float span = std::abs(a2 - a1);
        if (p.none) {
            rect(a1, a2, c - half, c + half, false);
            if (p.round) {
                rect(a1 - half, a1 + half, c - half, c + half, true);
                rect(a2 - half, a2 + half, c - half, c + half, true);
            }
        } else if (p.round) {
            for (float t = 0; t <= span + 1e-3f; t += p.gap) {
                const float at = a1 + direction * t;
                rect(at - half, at + half, c - half, c + half, true);
                if (p.gap <= 0) {
                    break;
                }
            }
        } else if (p.dash > 0) {
            for (float t = 0; t < span; t += p.dash + p.gap) {
                const float end = std::min(span, t + p.dash);
                rect(a1 + direction * t, a1 + direction * end, c - half, c + half, false);
            }
        }
        // Back to the box's coordinates.
        for (const auto& s : shapes) {
            const double rx0 = (s.rect[0] - device[2]) / device[0];
            const double rx1 = (s.rect[0] + s.rect[2] - device[2]) / device[0];
            const double ry0 = (s.rect[1] - device[3]) / device[1];
            const double ry1 = (s.rect[1] + s.rect[3] - device[3]) / device[1];
            out.push_back({{float(std::min(rx0, rx1)), float(std::min(ry0, ry1)), float(std::abs(rx1 - rx0)),
                            float(std::abs(ry1 - ry0))},
                           s.round});
        }
        return out;
    }
} // namespace weft
