#include "base/diagnostic.hpp"
#include "effects/effect_css.hpp"
#include "effects/effect_parameters.hpp"
#include "render/srgb.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <cmath>
#include <iostream>
#include <string>

// UI shader effects that CSS attaches in scenes: the manifest weft_add_effect writes, registration and its refusals,
// parameter values by type, -weft-effect() backgrounds with the parameters of custom properties (initial, set,
// inherited, refused), the display list's background color, effect and borders, the frame's time, and frames that keep
// coming while an effect reads the time.
namespace {
    using weft::testing::require;
    using Type = weft::paint::EffectParameter::Type;
    using Kind = weft::paint::Item::Kind;

    bool near(float a, float b) {
        return std::abs(a - b) < 1e-5f;
    }

    bool near(const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return near(a[0], b[0]) && near(a[1], b[1]) && near(a[2], b[2]) && near(a[3], b[3]);
    }

    // The diagnostic code and message of what throws, or none.
    template <class F>
    std::pair<weft::Diag, std::string> diagnostic(F&& f) {
        try {
            f();
        } catch (const weft::DiagnosticError& e) {
            return {e.code(), e.what()};
        }
        return {weft::Diag{}, {}};
    }

    std::shared_ptr<const weft::paint::Asset> mask() {
        return std::make_shared<const weft::paint::Asset>(
            weft::paint::Asset{16, 16, weft::paint::Encoding::Coverage, std::vector<std::uint8_t>(256, 128)});
    }
} // namespace

int main() {
    try {
        // The manifest of tests/effects/css.hlsl, as tests/CMakeLists.txt declares it.
        const auto& css = weft::effects::effect_css();
        require(css->parameters == 3 && css->textures == 1 && css->animated && css->named_parameters.size() == 3 &&
                    css->named_parameters[0].name == "tint" && css->named_parameters[0].type == Type::Color &&
                    css->named_parameters[0].initial == "#336699" && css->named_parameters[1].name == "amount" &&
                    css->named_parameters[1].type == Type::Number && css->named_parameters[1].initial == "0.5" &&
                    css->named_parameters[2].name == "offset" && css->named_parameters[2].type == Type::Vector2 &&
                    css->named_parameters[2].initial == "0 0" &&
                    css->named_textures == std::vector<std::string>{"mask"},
                "effect manifest");

        // Values by type: exactly the numbers a type takes, colors as straight linear RGB.
        using weft::scene_effect_value;
        const auto grey = weft::render::srgb_to_linear(128 / 255.f);
        require(near(*scene_effect_value(Type::Number, " 0.25 "), {.25f, 0, 0, 0}) &&
                    near(*scene_effect_value(Type::Vector2, "1 -2"), {1, -2, 0, 0}) &&
                    near(*scene_effect_value(Type::Vector4, "1 2 3 4"), {1, 2, 3, 4}) &&
                    near(*scene_effect_value(Type::Color, "#808080"), {grey, grey, grey, 1}) &&
                    near(*scene_effect_value(Type::Color, "rgba(0, 0, 255, 0.5)"), {0, 0, 1, .5f}) &&
                    near(*scene_effect_value(Type::Color, "#f00"), {1, 0, 0, 1}),
                "effect values");
        require(!scene_effect_value(Type::Number, "red") && !scene_effect_value(Type::Number, "1 2") &&
                    !scene_effect_value(Type::Vector3, "1 2") && !scene_effect_value(Type::Vector2, "1px 2") &&
                    !scene_effect_value(Type::Color, "1") && !scene_effect_value(Type::Number, ""),
                "values of another type");

        // Registration: the program's manifest, its textures, and each initial value of its type.
        weft::SceneEffects effects;
        require(bool(effects.add("glow", css, {mask()})), "register an effect");
        require(!effects.add("glow", css, {mask()}), "a name registered twice");
        require(!effects.add("bare", css, {}), "too few textures");
        require(!effects.add("distance", css,
                             {std::make_shared<const weft::paint::Asset>(
                                 weft::paint::Asset{2, 2, weft::paint::Encoding::SignedDistance, {1, 2, 3, 4}, 4})}),
                "a signed distance texture");
        require(!effects.add("anonymous", weft::effects::effect_parameters(), {}), "a program without a manifest");
        auto bad = std::make_shared<weft::paint::EffectProgram>(*css);
        bad->named_parameters[1].initial = "much";
        require(!effects.add("bad", bad, {mask()}), "an initial value of another type");
        auto still = std::make_shared<weft::paint::EffectProgram>(*css);
        still->animated = false;
        require(bool(effects.add("still", still, {mask()})), "register an effect that ignores the time");

        // Resolution: initial values unless a custom property sets one; an unknown name or a value of another type is a
        // scene profile diagnostic.
        const auto initial = effects.resolve("glow", {});
        require(initial->parameters.size() == 3 &&
                    near(initial->parameters[0],
                         {weft::render::srgb_to_linear(0x33 / 255.f), weft::render::srgb_to_linear(0x66 / 255.f),
                          weft::render::srgb_to_linear(0x99 / 255.f), 1}) &&
                    near(initial->parameters[1], {.5f, 0, 0, 0}) && effects.resolve("glow", {}) == initial,
                "initial values, shared");
        const auto set = effects.resolve("glow", {{"--amount", "0.75"}, {"--offset", std::nullopt}, {"--other", "x"}});
        require(set != initial && near(set->parameters[1], {.75f, 0, 0, 0}) &&
                    set->parameters[2] == initial->parameters[2],
                "a set value; an invalid one keeps the initial value");
        const auto unknown = diagnostic([&] {
            effects.resolve("nope", {});
        });
        const auto mistyped = diagnostic([&] {
            effects.resolve("glow", {{"--amount", "red"}});
        });
        require(unknown.first == weft::Diag::SceneProfile &&
                    unknown.second.find("unknown effect nope") != std::string::npos &&
                    mistyped.first == weft::Diag::SceneProfile &&
                    mistyped.second.find("--amount is not a number") != std::string::npos,
                "effect diagnostics");

        // A scene: the effect with the custom properties of the element and its ancestors, between the element's
        // background color and its borders, at the frame's time, inside its corners.
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:200px;height:120px;--tint:#ff0000");
        auto& card = s.html("div");
        s.set(card, "style",
              "position:absolute;left:10px;top:20px;width:100px;height:60px;background-color:#202020;border:2px solid "
              "#ffffff;border-radius:8px;background-image:-weft-effect(glow);--amount:0.75");
        s.tree.insert(*s.root, card, nullptr);
        auto& twin = s.html("div");
        s.set(twin, "style",
              "position:absolute;left:120px;top:20px;width:40px;height:40px;background:-weft-effect(glow)");
        s.tree.insert(*s.root, twin, nullptr);
        auto& scene_text = weft::testing::thread_scene_text();
        weft::SceneSolver solver("", "", scene_text);
        const auto without = diagnostic([&] {
            solver.frame(s.tree.snapshot(*s.root), 200, 120, 0);
        });
        require(without.first == weft::Diag::SceneProfile &&
                    without.second.find("-weft-effect(glow) without registered effects") != std::string::npos,
                "an effect without registered effects");
        solver.reset();
        solver.set_effects(&effects);
        auto frame = solver.frame(s.tree.snapshot(*s.root), 200, 120, 1500);
        const auto& node = frame.root.children[0];
        require(node.effect && node.effect->program == css && near(node.effect->parameters[0], {1, 0, 0, 1}) &&
                    near(node.effect->parameters[1], {.75f, 0, 0, 0}) && frame.root.children[1].effect &&
                    frame.animated,
                "an element's effect, its parameters inherited and set, and a frame that keeps coming");
        auto listed = weft::scene_display_list(frame, {}, &weft::testing::thread_scene_text());
        require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 4)), "a well-formed list");
        const auto& list = listed.list;
        std::vector<Kind> kinds;
        for (const auto& item : list.items) {
            kinds.push_back(item.kind);
        }
        // The root, then the card's background color, effect and borders, then the twin's effect alone.
        require(kinds == std::vector<Kind>{Kind::Effect, Kind::Box, Kind::Effect, Kind::Box, Kind::Effect} ||
                    kinds == std::vector<Kind>{Kind::Box, Kind::Effect, Kind::Box, Kind::Effect},
                "background color, effect, borders");
        const auto first = std::size_t(kinds.size() == 5 ? 1 : 0);
        const auto& under = list.boxes[list.items[first].payload];
        const auto& effect = list.effects[list.items[first + 1].payload];
        const auto& over = list.boxes[list.items[first + 2].payload];
        require(under.background[3] > 0 && under.widths == std::array<float, 4>{} && over.background[3] == 0 &&
                    over.widths == std::array<float, 4>{2, 2, 2, 2} &&
                    effect.radii == std::array<float, 4>{8, 8, 8, 8} && near(effect.time, 1.5f) &&
                    effect.parameters == node.effect->parameters && list.items[first + 1].width == node.box[2] &&
                    list.items[first + 1].height == node.box[3],
                "the card's layers");
        require(list.programs.size() == 1 && list.assets.size() == 1 &&
                    list.effects.back().textures == std::vector<std::uint32_t>{0} && list.effects.back().program == 0,
                "two elements share the program and the texture");
        // An explicit layer that shows an effect draws in the pass, never as a cached image.
        const auto layered = weft::scene_display_list(frame, {node.identity}, &weft::testing::thread_scene_text());
        require(std::none_of(layered.list.items.begin(), layered.list.items.end(),
                             [](const weft::paint::Item& item) {
                                 return item.kind == Kind::Layer;
                             }) &&
                    layered.list.effects.size() == 2,
                "a layer with an effect draws in the pass");
        // As Chrome draws an image or a canvas's content, an effect fills its box's rect snapped to device pixels, each
        // edge to the nearest; a rotated box's is not snapped.
        s.set(card, "style",
              "position:absolute;left:10.375px;top:20.625px;width:100.5px;height:60.25px;"
              "background-image:-weft-effect(glow)");
        s.set(twin, "style",
              "position:absolute;left:120.5px;top:20.5px;width:40px;height:40px;transform:rotate(30deg);"
              "background-image:-weft-effect(glow)");
        const auto fractional = solver.frame(s.tree.snapshot(*s.root), 200, 120, 1600);
        const auto effect_items = [&](float scale) {
            std::vector<weft::paint::Item> items;
            for (const auto& item :
                 weft::scene_display_list(fractional, {}, &weft::testing::thread_scene_text(), scale).list.items) {
                if (item.kind == Kind::Effect) {
                    items.push_back(item);
                }
            }
            require(items.size() == 2, "the card's and the twin's effects");
            return items;
        };
        const auto at1 = effect_items(1);
        const auto at2 = effect_items(2);
        require(at1[0].transform == weft::paint::Affine{1, 0, 0, 1, 10, 21} && at1[0].width == 101 &&
                    at1[0].height == 60,
                "an effect snapped to pixels");
        require(at2[0].transform == weft::paint::Affine{1, 0, 0, 1, 10.5f, 20.5f} && at2[0].width == 100.5f &&
                    at2[0].height == 60.5f,
                "an effect snapped to device pixels");
        require(at1[1].transform == at2[1].transform && at1[1].width == 40 && at1[1].height == 40 &&
                    at1[1].transform[1] != 0,
                "a rotated effect not snapped");

        // An effect that ignores the time lets the frames stop; a custom property that script sets repaints without a
        // new layout, and the effect takes the value.
        const auto still_card = [&](const char* amount) {
            s.set(card, "style",
                  std::string("position:absolute;left:10px;top:20px;width:100px;height:60px;background-image:-weft-"
                              "effect(still);--amount:") +
                      amount);
        };
        s.set(twin, "style", "position:absolute;left:120px;top:20px;width:40px;height:40px");
        still_card("0.75");
        // The retained frame of a host, which update() changes in place.
        weft::SceneSolver quiet("div:hover{--amount:1}", "", scene_text);
        quiet.set_effects(&effects);
        weft::SceneFrame retained;
        quiet.update(s.tree.snapshot(*s.root), retained, 200, 120, 0);
        quiet.update(s.tree.snapshot(*s.root), retained, 200, 120, 16);
        require(!retained.animated && !quiet.work().paint_changed && retained.root.children[0].effect,
                "an effect that ignores the time");
        still_card("0.25");
        quiet.update(s.tree.snapshot(*s.root), retained, 200, 120, 32);
        require(near(retained.root.children[0].effect->parameters[1], {.25f, 0, 0, 0}) && quiet.work().paint_changed &&
                    !quiet.work().geometry_changed && !retained.animated,
                "a changed parameter repaints");
        // A custom property that :hover sets changes the effect alone, not the attributes or the style values: it
        // repaints too.
        s.set(card, "style",
              "position:absolute;left:10px;top:20px;width:100px;height:60px;background-image:-weft-effect(still)");
        quiet.update(s.tree.snapshot(*s.root), retained, 200, 120, 36);
        card.hovered = true;
        s.tree.changed(card);
        quiet.update(s.tree.snapshot(*s.root), retained, 200, 120, 40);
        require(near(retained.root.children[0].effect->parameters[1], {1, 0, 0, 0}) && quiet.work().paint_changed &&
                    !quiet.work().geometry_changed,
                "a parameter that :hover sets repaints");
        card.hovered = false;
        s.tree.changed(card);
        // An effect that reads the time repaints every frame, though nothing else changes.
        s.set(card, "style",
              "position:absolute;left:10px;top:20px;width:100px;height:60px;background:-weft-effect(glow)");
        quiet.update(s.tree.snapshot(*s.root), retained, 200, 120, 48);
        // A new layout makes this the solver's reference frame, from which the next one could take the card whole.
        quiet.update(s.tree.snapshot(*s.root), retained, 201, 120, 56);
        quiet.update(s.tree.snapshot(*s.root), retained, 201, 120, 64);
        require(retained.animated && quiet.work().paint_changed && !quiet.work().geometry_changed,
                "an effect that reads the time repaints");
        // Its box is damaged in every frame, though nothing else changed; an effect that ignores the time is not.
        const auto before = retained;
        quiet.update(s.tree.snapshot(*s.root), retained, 201, 120, 80);
        const auto damage = weft::scene_damage(before, retained, 1);
        require(damage.changed && !damage.full && damage.rect[0] <= 10 && damage.rect[1] <= 20 &&
                    damage.rect[2] >= 110 && damage.rect[3] >= 80,
                "the box of an effect that reads the time is damaged");
        still_card("0.25");
        quiet.update(s.tree.snapshot(*s.root), retained, 201, 120, 96);
        const auto still_before = retained;
        quiet.update(s.tree.snapshot(*s.root), retained, 201, 120, 112);
        require(!weft::scene_damage(still_before, retained, 1).changed, "an effect that ignores the time is not");

        // -weft-effect() names one registered effect.
        for (const auto& [value, expected] : std::vector<std::pair<std::string, std::string>>{
                 {"-weft-effect()", "-weft-effect() takes one effect name"},
                 {"-weft-effect(glow, still)", "-weft-effect() takes one effect name"},
                 {"-weft-effect(nope)", "unknown effect nope"}}) {
            s.set(twin, "style", "width:40px;height:40px;background-image:" + value);
            weft::SceneSolver refused("", "", scene_text);
            refused.set_effects(&effects);
            const auto d = diagnostic([&] {
                refused.frame(s.tree.snapshot(*s.root), 200, 120, 0);
            });
            require(d.first == weft::Diag::SceneProfile && d.second.find(expected) != std::string::npos,
                    "refused " + value);
        }
        std::cout << "PASS scene effects: manifest, values, registration, custom properties, layers of the display "
                     "list, time, repaint, diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
