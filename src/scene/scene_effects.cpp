#include "scene/scene_effects.hpp"

#include "base/css_scalar.hpp"
#include "base/diagnostic.hpp"
#include "render/srgb.hpp"
#include "style/style.hpp"

#include <utility>

namespace weft {
    namespace {
        using Type = paint::EffectParameter::Type;

        const char* type_name(Type type) {
            switch (type) {
                case Type::Number:
                    return "number";
                case Type::Vector2:
                    return "vec2";
                case Type::Vector3:
                    return "vec3";
                case Type::Vector4:
                    return "vec4";
                case Type::Color:
                    return "color";
            }
            return "value";
        }
    } // namespace

    std::optional<std::array<float, 4>> scene_effect_value(Type type, std::string_view text) {
        std::vector<std::string> parts;
        for (std::size_t i = 0; i < text.size();) {
            if (css::ecma_space(text[i])) {
                ++i;
                continue;
            }
            auto end = i;
            while (end < text.size() && !css::ecma_space(text[end])) {
                ++end;
            }
            parts.emplace_back(text.substr(i, end - i));
            i = end;
        }
        if (type == Type::Color) {
            if (parts.empty()) {
                return std::nullopt;
            }
            try {
                const auto c = parse_css_color(css::expand_color_shorthand(css::trim(text)));
                return std::array<float, 4>{render::srgb_to_linear(c[0]), render::srgb_to_linear(c[1]),
                                            render::srgb_to_linear(c[2]), c[3]};
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
        const std::size_t count = type == Type::Number ? 1 : type == Type::Vector2 ? 2 : type == Type::Vector3 ? 3 : 4;
        if (parts.size() != count) {
            return std::nullopt;
        }
        std::array<float, 4> out{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = css::parse_number<float>(parts[i]);
            if (!value) {
                return std::nullopt;
            }
            out[i] = *value;
        }
        return out;
    }

    Status SceneEffects::add(std::string name, std::shared_ptr<const paint::EffectProgram> program,
                             std::vector<std::shared_ptr<const paint::Asset>> textures) {
        const auto refuse = [&](const std::string& why) {
            return make_diagnostic(Diag::SceneProfile, "effect " + name + ": " + why);
        };
        if (effects_.contains(name)) {
            return refuse("registered twice");
        }
        if (!program || program->named_parameters.size() != program->parameters ||
            program->named_textures.size() != program->textures) {
            return refuse("a program without a manifest for CSS");
        }
        if (textures.size() != program->textures) {
            return refuse("the textures its manifest names");
        }
        for (const auto& texture : textures) {
            if (!texture || texture->encoding == paint::Encoding::SignedDistance) {
                return refuse("a texture neither coverage nor color");
            }
        }
        auto initial = std::make_shared<SceneEffect>();
        initial->program = program;
        initial->textures = std::move(textures);
        for (const auto& parameter : program->named_parameters) {
            // No initial value: zeros.
            const auto value = parameter.initial.empty() ? std::array<float, 4>{}
                                                         : scene_effect_value(parameter.type, parameter.initial);
            if (!value) {
                return refuse("the initial value of --" + parameter.name + " is not a " + type_name(parameter.type));
            }
            initial->parameters.push_back(*value);
        }
        effects_.emplace(std::move(name), Entry{std::move(initial)});
        return {};
    }

    std::shared_ptr<const SceneEffect>
    SceneEffects::resolve(std::string_view name,
                          const std::map<std::string, std::optional<std::string>>& custom) const {
        const auto found = effects_.find(name);
        if (found == effects_.end()) {
            throw_diagnostic(Diag::SceneProfile, "unknown effect " + std::string(name));
        }
        const auto& initial = found->second.initial;
        const auto& named = initial->program->named_parameters;
        std::shared_ptr<SceneEffect> set;
        for (std::size_t i = 0; i < named.size(); ++i) {
            // An absent or guaranteed-invalid property leaves the initial value.
            const auto property = custom.find("--" + named[i].name);
            if (property == custom.end() || !property->second) {
                continue;
            }
            const auto value = scene_effect_value(named[i].type, *property->second);
            if (!value) {
                throw_diagnostic(Diag::SceneProfile, "effect " + std::string(name) + ": --" + named[i].name +
                                                         " is not a " + type_name(named[i].type));
            }
            if (!set) {
                set = std::make_shared<SceneEffect>(*initial);
            }
            set->parameters[i] = *value;
        }
        return set ? set : initial;
    }
} // namespace weft
