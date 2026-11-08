#include "scene/scene_background.hpp"

#include "base/css_scalar.hpp"
#include "constants/common.hpp"
#include "constants/scene.hpp"
#include "scene/scene_values.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace weft {
    namespace {
        using Fit = paint::TextureFit;

        [[noreturn]] void reject(const std::string& what) {
            throw std::invalid_argument(what + " outside scene profile");
        }

        std::vector<std::string> words(std::string_view s) {
            return css::split(s, ' ').parts;
        }

        // A length or percentage: px or a percentage's share; calc() of a percentage and a length.
        std::optional<BackgroundLength> length(std::string_view value) {
            const std::string text = css::trim(value);
            if (text.starts_with("calc(") && text.ends_with(')')) {
                const auto body = css::trim(text.substr(5, text.size() - 6));
                // CSS requires whitespace around the operator.
                auto at = body.find(" + ");
                float sign = 1;
                if (at == body.npos) {
                    at = body.find(" - ");
                    sign = -1;
                }
                const auto a = at == body.npos ? std::nullopt : length(std::string_view(body).substr(0, at));
                const auto b = at == body.npos ? std::nullopt : length(std::string_view(body).substr(at + 3));
                if (!a || !b) {
                    return std::nullopt;
                }
                return BackgroundLength{a->px + sign * b->px, a->share + sign * b->share};
            }
            const auto [digits, unit] = css::split_unit(text, {.px = true, .percent = true});
            const auto n = css::parse_number<float>(std::string(digits),
                                                    static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n || digits.empty() || (unit == css::Unit::None && *n != 0) ||
                (unit != css::Unit::None && unit != css::Unit::Px && unit != css::Unit::Percent)) {
                return std::nullopt;
            }
            return unit == css::Unit::Percent ? BackgroundLength{0, *n / static_cast<float>(constants::percent_scale)}
                                              : BackgroundLength{*n, 0};
        }

        BackgroundBox box(std::string_view value, std::string_view property) {
            const auto v = css::trim(value);
            if (v == "border-box") {
                return BackgroundBox::Border;
            }
            if (v == "padding-box") {
                return BackgroundBox::Padding;
            }
            if (v == "content-box") {
                return BackgroundBox::Content;
            }
            reject(std::string(property) + " " + std::string(v));
        }

        Fit::Repeat repeat_style(std::string_view v) {
            return v == "repeat"      ? Fit::Repeat::Repeat
                   : v == "no-repeat" ? Fit::Repeat::NoRepeat
                   : v == "space"     ? Fit::Repeat::Space
                   : v == "round"     ? Fit::Repeat::Round
                                      : (reject("background-repeat " + std::string(v)), Fit::Repeat::Repeat);
        }

        // background-position (CSS Backgrounds 3, <bg-position>): one value (the other axis centered), two (a
        // horizontal then a vertical one, or two keywords in either order), or three or four of edge keywords each
        // with an optional offset from it.
        std::array<BackgroundLength, 2> position(std::string_view value) {
            const auto w = words(value);
            const auto invalid = [&] {
                reject("background-position " + std::string(value));
            };
            const auto horizontal = [](const std::string& k) {
                return k == "left" || k == "right";
            };
            const auto vertical = [](const std::string& k) {
                return k == "top" || k == "bottom";
            };
            const auto keyword = [&](const std::string& k) {
                return horizontal(k) || vertical(k) || k == "center";
            };
            // A keyword's share of the room: left and top 0, center a half, right and bottom 1.
            const auto share = [](const std::string& k) {
                return k == "left" || k == "top" ? 0.f : k == "center" ? .5f : 1.f;
            };
            std::array<BackgroundLength, 2> out{BackgroundLength{0, .5f}, BackgroundLength{0, .5f}};
            if (w.empty() || w.size() > 4) {
                invalid();
            }
            if (w.size() <= 2) {
                // Which axis each value is for: a keyword of one axis puts the other on the other one.
                std::array<int, 2> axis{0, 1};
                if (w.size() == 1) {
                    axis[0] = vertical(w[0]) ? 1 : 0;
                } else if (keyword(w[0]) && keyword(w[1])) {
                    if (vertical(w[0]) || horizontal(w[1])) {
                        axis = {1, 0};
                    }
                    if (axis[0] == axis[1] || (horizontal(w[0]) && horizontal(w[1])) ||
                        (vertical(w[0]) && vertical(w[1]))) {
                        invalid();
                    }
                } else if (vertical(w[0]) || horizontal(w[1])) {
                    invalid();
                }
                for (std::size_t i = 0; i < w.size(); ++i) {
                    if (keyword(w[i])) {
                        out[std::size_t(axis[i])] = {0, share(w[i])};
                    } else if (const auto l = length(w[i])) {
                        out[std::size_t(axis[i])] = *l;
                    } else {
                        invalid();
                    }
                }
                return out;
            }
            // Edge keywords, each with an optional offset towards the box's inside; center without one.
            std::array<bool, 2> set{};
            for (std::size_t i = 0; i < w.size();) {
                const auto& k = w[i];
                if (!keyword(k)) {
                    invalid();
                }
                std::optional<BackgroundLength> offset;
                if (i + 1 < w.size() && !keyword(w[i + 1])) {
                    offset = length(w[i + 1]);
                    if (!offset || k == "center") {
                        invalid();
                    }
                }
                const std::size_t a = vertical(k) ? 1 : horizontal(k) ? 0 : set[0] ? 1 : 0;
                if (set[a]) {
                    invalid();
                }
                set[a] = true;
                const float edge = share(k);
                const float sign = edge == 1 ? -1.f : 1.f;
                out[a] = {offset ? sign * offset->px : 0, edge + (offset ? sign * offset->share : 0)};
                i += offset ? 2 : 1;
            }
            return out;
        }

        std::array<Fit::Repeat, 2> repeat(std::string_view value) {
            const auto w = words(value);
            if (w.size() == 1 && w[0] == "repeat-x") {
                return {Fit::Repeat::Repeat, Fit::Repeat::NoRepeat};
            }
            if (w.size() == 1 && w[0] == "repeat-y") {
                return {Fit::Repeat::NoRepeat, Fit::Repeat::Repeat};
            }
            if (w.empty() || w.size() > 2) {
                reject("background-repeat " + std::string(value));
            }
            return {repeat_style(w[0]), repeat_style(w.back())};
        }
    } // namespace

    BackgroundLayout scene_background_layout(const StyleValues& style) {
        BackgroundLayout out;
        if (const auto a = css::trim(style.text(PropertyId::BackgroundAttachment, "scroll")); a != "scroll") {
            reject("background-attachment " + a);
        }
        out.origin = box(style.text(PropertyId::BackgroundOrigin, "padding-box"), "background-origin");
        out.clip = box(style.text(PropertyId::BackgroundClip, "border-box"), "background-clip");
        const auto size = css::trim(style.text(PropertyId::BackgroundSize, "auto"));
        if (size == "cover" || size == "contain") {
            out.scale = size == "cover" ? Fit::Scale::Cover : Fit::Scale::Contain;
        } else {
            const auto w = words(size);
            if (w.empty() || w.size() > 2) {
                reject("background-size " + size);
            }
            for (std::size_t a = 0; a < 2; ++a) {
                const auto& v = w[std::min(a, w.size() - 1)];
                if (a == 1 && w.size() == 1) {
                    break;
                }
                if (v == "auto") {
                    continue;
                }
                const auto l = length(v);
                if (!l || l->px < 0 || l->share < 0) {
                    reject("background-size " + size);
                }
                out.size[a] = l;
            }
        }
        out.position = position(style.text(PropertyId::BackgroundPosition, "0% 0%"));
        out.repeat = repeat(style.text(PropertyId::BackgroundRepeat, "repeat"));
        return out;
    }

    BackgroundLayout scene_object_layout(const StyleValues& style, std::array<float, 2> natural,
                                         std::array<float, 2> content) {
        BackgroundLayout out;
        out.origin = out.clip = BackgroundBox::Content;
        out.repeat = {Fit::Repeat::NoRepeat, Fit::Repeat::NoRepeat};
        auto fit = css::trim(style.text(PropertyId::ObjectFit, "fill"));
        if (fit == "scale-down") {
            fit = natural[0] <= content[0] && natural[1] <= content[1] ? "none" : "contain";
        }
        if (fit == "fill") {
            out.size = {BackgroundLength{0, 1}, BackgroundLength{0, 1}};
        } else if (fit == "contain" || fit == "cover") {
            out.scale = fit == "cover" ? Fit::Scale::Cover : Fit::Scale::Contain;
        } else if (fit != "none") {
            reject("object-fit " + std::string(fit));
        }
        out.position = position(style.text(PropertyId::ObjectPosition, "50% 50%"));
        return out;
    }

    bool scene_image_pixelated(const StyleValues& style) {
        const auto rendering = css::trim(style.text(PropertyId::ImageRendering, "auto"));
        return rendering == "pixelated" || rendering == "crisp-edges";
    }

    BackgroundRect scene_background_box(const StyleValues& style, BackgroundBox which, float w, float h) {
        BackgroundRect out;
        if (which == BackgroundBox::Border) {
            const auto r = scene_values::corner_radii(style, w, h);
            out.rect = {0, 0, w, h};
            out.radius_x = out.radius_y = r;
            return out;
        }
        const auto padding = scene_values::padding_box(style, w, h);
        out.rect = padding.rect;
        out.radius_x = padding.radius_x;
        out.radius_y = padding.radius_y;
        if (which == BackgroundBox::Padding) {
            return out;
        }
        // The content box: the padding box less its padding, its corners less it too.
        std::array<float, 4> p{};
        unsigned k{};
        for (const auto id :
             {PropertyId::PaddingTop, PropertyId::PaddingRight, PropertyId::PaddingBottom, PropertyId::PaddingLeft}) {
            const auto v = css::trim(style.text(id, "0"));
            const auto l = length(v);
            if (!l || l->share != 0) {
                reject("background content-box with padding " + v);
            }
            p[k++] = std::max(0.f, l->px);
        }
        out.rect = {out.rect[0] + p[3], out.rect[1] + p[0], std::max(0.f, out.rect[2] - p[1] - p[3]),
                    std::max(0.f, out.rect[3] - p[0] - p[2])};
        const std::array<float, 4> horizontal{p[3], p[1], p[1], p[3]};
        const std::array<float, 4> vertical{p[0], p[0], p[2], p[2]};
        for (unsigned i = 0; i < 4; ++i) {
            out.radius_x[i] = std::max(0.f, out.radius_x[i] - horizontal[i]);
            out.radius_y[i] = std::max(0.f, out.radius_y[i] - vertical[i]);
            if (out.radius_x[i] <= 0 || out.radius_y[i] <= 0) {
                out.radius_x[i] = out.radius_y[i] = 0;
            }
        }
        return out;
    }

    paint::TextureFit scene_texture_fit(const BackgroundLayout& layout, const BackgroundRect& painting,
                                        const BackgroundRect& positioning) {
        Fit fit;
        fit.area = {positioning.rect[0] - painting.rect[0], positioning.rect[1] - painting.rect[1], positioning.rect[2],
                    positioning.rect[3]};
        fit.scale = layout.scale;
        for (std::size_t a = 0; a < 2; ++a) {
            if (const auto& s = layout.size[a]) {
                fit.size[a] = std::max(0.f, s->px + s->share * fit.area[2 + a]);
            }
            fit.fraction[a] = layout.position[a].share;
            fit.offset[a] = layout.position[a].px;
            fit.repeat[a] = layout.repeat[a];
        }
        return fit;
    }

    std::array<TextureTiles, 2> scene_texture_tiles(const paint::TextureFit& fit, float natural_width,
                                                    float natural_height) {
        const std::array<float, 2> natural{natural_width, natural_height};
        std::array<float, 2> tile{};
        if (fit.scale != Fit::Scale::Lengths) {
            const float sx = fit.area[2] / natural[0];
            const float sy = fit.area[3] / natural[1];
            const float k = fit.scale == Fit::Scale::Cover ? std::max(sx, sy) : std::min(sx, sy);
            tile = {natural[0] * k, natural[1] * k};
        } else {
            tile = fit.size;
            if (tile[0] < 0 && tile[1] < 0) {
                tile = natural;
            } else if (tile[0] < 0) {
                tile[0] = tile[1] * natural[0] / natural[1];
            } else if (tile[1] < 0) {
                tile[1] = tile[0] * natural[1] / natural[0];
            }
        }
        // round: as many whole tiles as fit best, scaled to fill the area; the other axis keeps the texture's ratio
        // when its size is auto.
        const std::array<bool, 2> round{fit.repeat[0] == Fit::Repeat::Round, fit.repeat[1] == Fit::Repeat::Round};
        const auto rounded = tile;
        for (std::size_t a = 0; a < 2; ++a) {
            if (round[a] && rounded[a] > 0) {
                tile[a] = fit.area[2 + a] / std::max(1.f, std::floor(fit.area[2 + a] / rounded[a] + .5f));
            }
        }
        for (std::size_t a = 0; a < 2; ++a) {
            const std::size_t b = 1 - a;
            if (round[a] && !round[b] && fit.scale == Fit::Scale::Lengths && fit.size[b] < 0 && rounded[a] > 0) {
                tile[b] = rounded[b] * tile[a] / rounded[a];
            }
        }
        std::array<TextureTiles, 2> out{};
        for (std::size_t a = 0; a < 2; ++a) {
            auto& t = out[a];
            t.tile = tile[a];
            if (!(t.tile > 0)) {
                t.tile = 0;
                continue;
            }
            const float area = fit.area[2 + a];
            t.start = fit.area[a] + (area - t.tile) * fit.fraction[a] + fit.offset[a];
            switch (fit.repeat[a]) {
                case Fit::Repeat::Repeat:
                case Fit::Repeat::Round:
                    t.period = t.tile;
                    break;
                case Fit::Repeat::NoRepeat:
                    break;
                case Fit::Repeat::Space: {
                    // Whole tiles spread over the area, the first and the last at its edges; one placed by the
                    // position when two do not fit.
                    const float n = std::floor(area / t.tile);
                    if (n >= 2) {
                        t.start = fit.area[a];
                        t.period = t.tile + (area - n * t.tile) / (n - 1);
                    }
                    break;
                }
            }
        }
        return out;
    }

    TexturePoint scene_texture_point(const TextureTiles& tiles, float x) {
        if (!(tiles.tile > 0)) {
            return {};
        }
        float d = x - tiles.start;
        if (tiles.period > 0) {
            d -= std::floor(d / tiles.period) * tiles.period;
        }
        return {d >= 0 && d < tiles.tile, d / tiles.tile};
    }

    std::optional<BackgroundLength> scene_length(std::string_view value) {
        return length(value);
    }

    std::array<BackgroundLength, 2> scene_position(std::string_view value) {
        return position(value);
    }
} // namespace weft
