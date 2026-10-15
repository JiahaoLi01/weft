#include "style/background_layer.hpp"

#include <algorithm>
#include <array>

namespace weft {
    namespace {
        using Kind = BackgroundComponent::Kind;

        bool one_of(std::string_view name, std::initializer_list<std::string_view> names) {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        bool repeat_keyword(const BackgroundComponent& c) {
            return c.kind == Kind::Ident &&
                   one_of(c.name, {"repeat", "repeat-x", "repeat-y", "no-repeat", "space", "round"});
        }

        // repeat-x and repeat-y stand alone; the others take a second value for the vertical axis.
        bool two_axis_repeat(const BackgroundComponent& c) {
            return repeat_keyword(c) && c.name != "repeat-x" && c.name != "repeat-y";
        }

        bool length(const BackgroundComponent& c) {
            return c.kind == Kind::Length ||
                   (c.kind == Kind::Function && one_of(c.name, {"calc", "min", "max", "clamp"}));
        }

        bool position_component(const BackgroundComponent& c) {
            return length(c) || (c.kind == Kind::Ident && one_of(c.name, {"left", "right", "top", "bottom", "center"}));
        }

        bool size_component(const BackgroundComponent& c) {
            return length(c) || (c.kind == Kind::Ident && one_of(c.name, {"auto", "cover", "contain"}));
        }

        bool image_component(const BackgroundComponent& c) {
            return (c.kind == Kind::Ident && c.name == "none") ||
                   (c.kind == Kind::Function &&
                    (c.name.ends_with("gradient") ||
                     one_of(c.name, {"-weft-effect", "-weft-texture", "url", "image", "image-set", "cross-fade"})));
        }
    } // namespace

    std::optional<std::vector<BackgroundPart>> background_parts(std::span<const BackgroundComponent> components) {
        std::vector<BackgroundPart> parts(components.size());
        std::array<bool, 10> seen{};
        unsigned boxes{};
        std::size_t last_box{};
        const auto first = [&](BackgroundPart part) {
            const bool fresh = !seen[std::size_t(part)];
            seen[std::size_t(part)] = true;
            return fresh;
        };
        for (std::size_t i = 0; i < components.size();) {
            const auto& c = components[i];
            if (position_component(c)) {
                if (!first(BackgroundPart::Position)) {
                    return std::nullopt;
                }
                std::size_t j = i;
                for (; j < components.size() && j - i < 4 && position_component(components[j]); ++j) {
                    parts[j] = BackgroundPart::Position;
                }
                if (j < components.size() && components[j].kind == Kind::Slash) {
                    parts[j++] = BackgroundPart::Slash;
                    const std::size_t size = j;
                    for (; j < components.size() && j - size < 2 && size_component(components[j]); ++j) {
                        parts[j] = BackgroundPart::Size;
                    }
                    // cover and contain stand alone.
                    const auto alone = [&](std::size_t k) {
                        return components[k].kind == Kind::Ident && components[k].name != "auto";
                    };
                    if (j == size || (j - size > 1 && (alone(size) || alone(size + 1))) ||
                        !first(BackgroundPart::Size)) {
                        return std::nullopt;
                    }
                }
                i = j;
            } else if (repeat_keyword(c)) {
                if (!first(BackgroundPart::Repeat)) {
                    return std::nullopt;
                }
                parts[i] = BackgroundPart::Repeat;
                if (two_axis_repeat(c) && i + 1 < components.size() && two_axis_repeat(components[i + 1])) {
                    parts[++i] = BackgroundPart::Repeat;
                }
                ++i;
            } else if (c.kind == Kind::Ident && one_of(c.name, {"scroll", "fixed", "local"})) {
                if (!first(BackgroundPart::Attachment)) {
                    return std::nullopt;
                }
                parts[i++] = BackgroundPart::Attachment;
            } else if (c.kind == Kind::Ident && one_of(c.name, {"border-box", "padding-box", "content-box"})) {
                if (++boxes > 2) {
                    return std::nullopt;
                }
                parts[i] = boxes == 1 ? BackgroundPart::OriginAndClip : BackgroundPart::Clip;
                if (boxes == 2) {
                    parts[last_box] = BackgroundPart::Origin;
                }
                last_box = i++;
            } else if (image_component(c)) {
                if (!first(BackgroundPart::Image)) {
                    return std::nullopt;
                }
                parts[i++] = BackgroundPart::Image;
            } else if (c.kind == Kind::Ident || c.kind == Kind::Hash || c.kind == Kind::Function) {
                // A color: a name, a hash or a color function; its own property tells whether it is one.
                if (!first(BackgroundPart::Color)) {
                    return std::nullopt;
                }
                parts[i++] = BackgroundPart::Color;
            } else {
                return std::nullopt;
            }
        }
        return parts;
    }
} // namespace weft
