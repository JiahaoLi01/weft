#pragma once

#include "base/result.hpp"
#include "paint/display_list.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// UI shader effects that CSS attaches with background-image: -weft-effect(name) (docs/architecture.md, "UI 着色器效果、
// 粒子与背景拷贝"): the effects a host registers by name, and the effect an element shows, with the parameters its
// custom properties give.
//
// Owner thread of the scene. A resolved effect is an immutable value that frames carry to the render thread.
namespace weft {
    // An element's -weft-effect() background: the registered program, its textures, and each parameter of the
    // program's manifest as the function reads it.
    struct SceneEffect {
        std::shared_ptr<const paint::EffectProgram> program;
        std::vector<std::shared_ptr<const paint::Asset>> textures;
        std::vector<std::array<float, 4>> parameters;

        friend bool operator==(const SceneEffect&, const SceneEffect&) = default;
    };

    // A parameter value of type from CSS text (paint::EffectParameter): numbers in x to w, absent ones 0; a color as
    // straight linear RGB and alpha. Nothing when the text is not a value of type.
    std::optional<std::array<float, 4>> scene_effect_value(paint::EffectParameter::Type type, std::string_view text);

    class SceneEffects {
    public:
        // Registers program under name with the textures it samples, in the order of its manifest: coverage assets read
        // as data, sRGB images as color. The program names its parameters and textures, and each parameter's initial
        // value is of its type. Diag::SceneProfile otherwise, or for a name already registered.
        Status add(std::string name, std::shared_ptr<const paint::EffectProgram> program,
                   std::vector<std::shared_ptr<const paint::Asset>> textures);

        // The effect -weft-effect(name) shows on an element whose computed custom properties are custom (names with
        // their leading "--"): each parameter from --<parameter>, or its initial value. Throws DiagnosticError
        // (Diag::SceneProfile) for a name nobody registered or a value that is not of its parameter's type.
        std::shared_ptr<const SceneEffect>
        resolve(std::string_view name, const std::map<std::string, std::optional<std::string>>& custom) const;

    private:
        struct Entry {
            // With every parameter at its initial value.
            std::shared_ptr<const SceneEffect> initial;
        };

        std::map<std::string, Entry, std::less<>> effects_;
    };
} // namespace weft
