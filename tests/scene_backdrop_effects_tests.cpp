#include "effects/effect_backdrop_shift.hpp"
#include "effects/effect_backdrop_tight.hpp"
#include "effects/effect_heat_haze.hpp"
#include "effects/effect_refraction.hpp"
#include "render/scene_pass.hpp"
#include "render/srgb.hpp"
#include "scene/scene.hpp"
#include "scene/scene_display_list.hpp"
#include "scene/scene_effects.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

// UI shader effects that read what lies behind their element (docs/effects.md): the display list's copy point (the
// element's first item, or its group's BeginGroup), the pass's copies and the runs they cut, the list check's
// refusals, the linear pass's refusal, damage; and the native passes (D3D11 on WARP at scale 1 and 2, Vulkan on this
// machine's device when built) against the backdrop drawn without the effect, moved by a known shift: read through the
// margin and not the element's own background, clamped to the copy (no margin, the target's edge), under the
// element's own opacity group (the target that encloses it), inside an ancestor's group (that group's content alone),
// along a turned box's axes; the samples (refraction and heat haze) where they move nothing or a tint lies over the
// view, and the same time drawing the same picture; an unreadable Vulkan target. Without the copies, the effects read
// nothing and every pixel check fails.
namespace {
    using weft::testing::require;
    using Kind = weft::paint::Item::Kind;
    using Step = weft::render::ScenePass::Step;
    using Pixels = std::vector<std::uint8_t>;
    using Expected = std::function<std::array<int, 4>(unsigned, unsigned)>;
    constexpr unsigned width = 64;
    constexpr unsigned height = 48;
    // The backdrop's cells, in CSS px.
    constexpr unsigned cell = 4;

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style = {}) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    std::shared_ptr<const weft::SceneEffect> effect(const std::shared_ptr<const weft::paint::EffectProgram>& program,
                                                    std::vector<std::array<float, 4>> parameters) {
        return std::make_shared<const weft::SceneEffect>(weft::SceneEffect{program, {}, std::move(parameters)});
    }

    // The shift effect over box, reading shift CSS px away (tight: with no margin).
    weft::SceneNode shifter(std::uint32_t slot, std::array<float, 4> box, std::array<float, 2> shift,
                            bool tight = false, std::initializer_list<std::pair<const char*, const char*>> style = {}) {
        auto n = node(slot, box, style);
        n.effect = effect(tight ? weft::effects::effect_backdrop_tight() : weft::effects::effect_backdrop_shift(),
                          {{shift[0], shift[1], 0, 0}});
        return n;
    }

    // The backdrop: opaque cells of distinct colors over the root, then the elements over them.
    weft::SceneFrame frame(std::vector<weft::SceneNode> over, double time_ms = 0) {
        weft::SceneFrame f;
        f.time_ms = time_ms;
        f.root = node(1, {0, 0, float(width), float(height)});
        std::uint32_t slot = 1000;
        for (unsigned y = 0; y < height / cell; ++y) {
            for (unsigned x = 0; x < width / cell; ++x) {
                char color[8];
                std::snprintf(color, sizeof(color), "#%02x%02x%02x", (x * 53 + 20) & 255, (y * 71 + 40) & 255,
                              ((x + y) * 29 + 60) & 255);
                f.root.children.push_back(node(slot++, {float(x * cell), float(y * cell), float(cell), float(cell)},
                                               {{"background-color", color}}));
            }
        }
        for (auto& n : over) {
            f.root.children.push_back(std::move(n));
        }
        return f;
    }

    // The frame with the geometry damage reads: its root's and the root's children's rects.
    weft::SceneFrame placed(weft::SceneFrame f) {
        const auto add = [&](const weft::SceneNode& n) {
            weft::ElementGeometry g;
            g.identity = n.identity;
            g.rect = {n.box[0], n.box[1], n.box[2], n.box[3]};
            f.elements.push_back(g);
        };
        add(f.root);
        for (const auto& c : f.root.children) {
            add(c);
        }
        return f;
    }

    std::array<int, 4> at(const Pixels& p, unsigned w, unsigned x, unsigned y) {
        const auto* q = p.data() + (std::size_t(y) * w + x) * 4;
        return {q[0], q[1], q[2], q[3]};
    }

    bool inside(unsigned x, unsigned y, std::array<int, 4> rect) {
        return int(x) >= rect[0] && int(y) >= rect[1] && int(x) < rect[2] && int(y) < rect[3];
    }

    // Each device pixel within levels of expected(x, y), skipping the ones it leaves out (alpha -1) and those within a
    // device px outside the effects' boxes rings names, where an effect's corners antialias (scene.hlsl); the first
    // that is not is reported. The worst difference.
    int compare(const std::string& what, const Pixels& drawn, unsigned w, unsigned h,
                const std::vector<std::array<int, 4>>& rings, const Expected& expected, int levels) {
        int worst{};
        for (unsigned y = 0; y < h; ++y) {
            for (unsigned x = 0; x < w; ++x) {
                const auto want = expected(x, y);
                if (want[3] < 0 || std::any_of(
                                       rings.begin(), rings.end(),
                                       [&](const std::array<int, 4>& r) {
                                           return !inside(x, y, r) &&
                                                  inside(x, y, {r[0] - 1, r[1] - 1, r[2] + 1, r[3] + 1});
                                       })) {
                    continue;
                }
                const auto got = at(drawn, w, x, y);
                for (unsigned c = 0; c < 4; ++c) {
                    const int d = std::abs(got[c] - want[c]);
                    worst = std::max(worst, d);
                    if (d > levels) {
                        std::cerr << what << ": pixel (" << x << ", " << y << ") is " << got[0] << ' ' << got[1] << ' '
                                  << got[2] << ' ' << got[3] << ", expected " << want[0] << ' ' << want[1] << ' '
                                  << want[2] << ' ' << want[3] << '\n';
                        for (unsigned v = y > 2 ? y - 2 : 0; v < std::min(h, y + 3); ++v) {
                            for (unsigned u = x > 2 ? x - 2 : 0; u < std::min(w, x + 3); ++u) {
                                const auto g = at(drawn, w, u, v);
                                const auto e = expected(u, v);
                                std::cerr << "  (" << u << ", " << v << ") " << g[0] << ' ' << g[1] << ' ' << g[2]
                                          << ' ' << g[3] << " / " << e[0] << ' ' << e[1] << ' ' << e[2] << ' ' << e[3];
                            }
                            std::cerr << '\n';
                        }
                        require(false, what);
                    }
                }
            }
        }
        std::cout << "  " << what << ": worst " << worst << '\n';
        return worst;
    }

    // A rect of CSS px (left, top, right, bottom) in device px at scale s.
    std::array<int, 4> device(std::array<int, 4> css, int s) {
        return {css[0] * s, css[1] * s, css[2] * s, css[3] * s};
    }

    // Encoded premultiplied BGRA of a linear premultiplied color.
    std::array<int, 4> encoded(const std::array<float, 4>& linear) {
        std::array<int, 4> out{};
        const float a = linear[3];
        for (unsigned c = 0; c < 3; ++c) {
            const float straight = a > 0 ? std::clamp(linear[c] / a, 0.f, 1.f) : 0;
            out[2 - c] = int(std::lround(weft::render::linear_to_srgb(straight) * a * 255));
        }
        out[3] = int(std::lround(a * 255));
        return out;
    }

    // The pass's plan of a frame's list at scale.
    weft::render::ScenePass plan(const weft::paint::DisplayList& list, float scale) {
        auto pass = weft::render::plan_scene_pass(list, scale, unsigned(width * scale), unsigned(height * scale));
        require(bool(pass), "a planned pass");
        return std::move(*pass);
    }

    std::size_t first_of(const weft::paint::DisplayList& list, Kind kind, std::size_t from = 0) {
        for (auto i = from; i < list.items.size(); ++i) {
            if (list.items[i].kind == kind) {
                return i;
            }
        }
        require(false, "an item of the kind");
        return 0;
    }

    // Lists, plans, refusals and damage, without a device.
    void lists() {
        // The samples' manifests take CSS: each initial value of its type.
        weft::SceneEffects registry;
        require(bool(registry.add("refraction", weft::effects::effect_refraction(), {})) &&
                    bool(registry.add("heat-haze", weft::effects::effect_heat_haze(), {})) &&
                    weft::effects::effect_refraction()->backdrop &&
                    weft::effects::effect_refraction()->backdrop_margin == 24 &&
                    weft::effects::effect_heat_haze()->backdrop && weft::effects::effect_heat_haze()->animated &&
                    !weft::effects::effect_backdrop_tight()->backdrop_margin,
                "the samples' manifests");
        auto& text = weft::testing::thread_scene_text();
        // The element's own background color lies after the copy point: the first item of the element is its box.
        const auto shifted = frame({shifter(10, {16, 12, 32, 24}, {3, 2}, false, {{"background-color", "#ff0000"}})});
        const auto listed = weft::scene_display_list(shifted, {}, &text, 1);
        require(listed.fallback.empty() && bool(weft::paint::check(listed.list, 4)), "a native list");
        const auto& list = listed.list;
        const auto effect_item = first_of(list, Kind::Effect);
        const auto& e = list.effects[list.items[effect_item].payload];
        require(e.backdrop_at + 1 == effect_item && list.items[e.backdrop_at].kind == Kind::Box,
                "the copy point: the element's background color, its first item");
        for (const float scale : {1.f, 2.f}) {
            const auto pass = plan(list, scale);
            require(pass.copies.size() == 1, "one copy");
            const auto s = int(scale);
            // The box (16, 12, 48, 36) grown by the margin of 8 px, at the scale.
            const std::array<std::int32_t, 4> region{8 * s, 4 * s, 56 * s, 44 * s};
            require(pass.copies[0].region == region, "the copy's region: the box and the margin, at the scale");
            const auto& r = pass.records[effect_item];
            require(r.uv == weft::render::SceneRecord::Vector{float(region[0]), float(region[1]),
                                                              float(region[2] - region[0]),
                                                              float(region[3] - region[1])},
                    "the record's uv: the copy's device px");
            std::size_t copy_at{};
            std::size_t effect_at{};
            for (std::size_t b = 0; b < pass.batches.size(); ++b) {
                const auto& batch = pass.batches[b];
                if (batch.step == Step::Copy) {
                    require(batch.first == e.backdrop_at && batch.source == 0, "the copy before the element");
                    copy_at = b;
                }
                if (batch.step == Step::Effect) {
                    require(batch.first == effect_item && batch.count == 1 && batch.copy == 1, "the effect's copy");
                    effect_at = b;
                }
                // No run of items drawn together crosses the copy point.
                require(batch.step == Step::Copy || batch.first >= e.backdrop_at ||
                            batch.first + batch.count <= e.backdrop_at,
                        "runs end at the copy point");
            }
            require(copy_at && copy_at < effect_at, "the copy, then the effect");
        }

        // An element that opens a group (opacity, a child): the copy point is its group's BeginGroup, which reads the
        // target that encloses the group.
        auto grouped = shifter(10, {16, 12, 32, 24}, {3, 2}, false, {{"opacity", "0.5"}});
        grouped.children = {node(11, {4, 4, 6, 6}, {{"background-color", "#ffffff"}})};
        const auto in_group = weft::scene_display_list(frame({grouped}), {}, &text, 1);
        const auto& g = in_group.list;
        const auto& ge = g.effects[g.items[first_of(g, Kind::Effect)].payload];
        require(g.items[ge.backdrop_at].kind == Kind::BeginGroup && ge.backdrop_at == first_of(g, Kind::BeginGroup),
                "the copy point: the element's group");

        // A first item that covers the target opaquely stays an item when a copy comes before it.
        weft::SceneFrame whole;
        whole.root = node(1, {0, 0, float(width), float(height)}, {{"background-color", "#204060"}});
        whole.root.effect = effect(weft::effects::effect_backdrop_shift(), {{1, 1, 0, 0}});
        const auto whole_list = weft::scene_display_list(whole, {}, &text, 1);
        const auto whole_pass = plan(whole_list.list, 1);
        require(whole_pass.clear == weft::paint::Color{} && whole_pass.batches.front().step == Step::Copy &&
                    whole_pass.batches.front().first == 0,
                "no clear folded past a copy");

        // An effect outside the target copies and draws nothing.
        const auto away = weft::scene_display_list(frame({shifter(10, {200, 100, 20, 20}, {3, 2})}), {}, &text, 1);
        const auto away_pass = plan(away.list, 1);
        require(away_pass.copies.empty() && std::none_of(away_pass.batches.begin(), away_pass.batches.end(),
                                                         [](const auto& b) {
                                                             return b.step == Step::Effect || b.step == Step::Copy;
                                                         }),
                "an effect outside the target");

        // The check: a copy point past the effect, a group that ends between it and the effect, a margin out of range.
        const auto refused = [&](weft::paint::DisplayList bad, const char* what) {
            const auto checked = weft::paint::check(bad, 4);
            require(!checked && checked.error().message.find("effect backdrop") != std::string::npos, what);
        };
        auto past = list;
        past.effects[list.items[effect_item].payload].backdrop_at = std::uint32_t(effect_item + 1);
        refused(past, "a copy point past the effect");
        // A copy point inside a group that ends before the effect.
        {
            weft::paint::DisplayList l;
            l.programs = {weft::effects::effect_backdrop_shift()};
            l.boxes = {weft::paint::Box{}};
            weft::paint::Item begin;
            begin.kind = Kind::BeginGroup;
            weft::paint::Item box;
            box.kind = Kind::Box;
            box.width = box.height = 4;
            weft::paint::Item end;
            end.kind = Kind::EndGroup;
            weft::paint::Item fx;
            fx.kind = Kind::Effect;
            fx.width = fx.height = 4;
            weft::paint::Effect value;
            value.parameters = {{0, 0, 0, 0}};
            value.backdrop_at = 1;
            l.effects = {value};
            l.items = {begin, box, end, fx};
            refused(l, "a group that ends between the copy point and the effect");
            l.effects[0].backdrop_at = 0;
            require(bool(weft::paint::check(l, 4)), "a copy point before a group that ends before the effect");
            l.effects[0].backdrop_at = 3;
            require(bool(weft::paint::check(l, 4)), "the effect's own item as its copy point");
            auto wide = std::make_shared<weft::paint::EffectProgram>(*weft::effects::effect_backdrop_shift());
            wide->backdrop_margin = 300;
            l.programs = {wide};
            refused(l, "a margin past the bound");
        }

        // The document's linear pass draws no effect that reads its backdrop.
        bool thrown{};
        try {
            weft::testing::D3D11Scene scene(8, 8);
            scene.gpu.prepare(std::make_shared<const weft::paint::DisplayList>(list));
        } catch (const std::exception&) {
            thrown = true;
        }
        require(thrown, "the linear pass refuses an effect that reads its backdrop");

        // Damage: any change repaints whole while an effect reads its backdrop; without one, only what changed.
        const auto read = placed(shifted);
        auto changed = read;
        changed.root.children.edit(0).style["background-color"] = "#00ff00";
        const auto damage = weft::scene_damage(read, changed, 1);
        require(damage.changed && damage.full, "a change repaints whole around an effect that reads its backdrop");
        const auto plain = placed(frame({node(10, {16, 12, 32, 24}, {{"background-color", "#ff0000"}})}));
        auto plain_changed = plain;
        plain_changed.root.children.edit(0).style["background-color"] = "#00ff00";
        const auto partial = weft::scene_damage(plain, plain_changed, 1);
        require(partial.changed && !partial.full, "without one, a change repaints its own rect");
        std::cout << "  lists, plans, checks and damage\n";
    }

    // The pixel checks of every pass, at scale 1 and 2: each frame's pixels, for the comparison of passes.
    std::vector<Pixels> check(const char* pass, const std::function<Pixels(const weft::SceneFrame&, float)>& draw) {
        std::vector<Pixels> out;
        for (const int s : {1, 2}) {
            const auto scale = float(s);
            const unsigned w = width * s;
            const unsigned h = height * s;
            const std::string name = std::string(pass) + " at scale " + std::to_string(s);
            const auto back = draw(frame({}), scale);
            const auto behind = [&](int x, int y) {
                return at(back, w, unsigned(std::clamp(x, 0, int(w) - 1)), unsigned(std::clamp(y, 0, int(h) - 1)));
            };
            const auto keep = [&](const Pixels& drawn) {
                out.push_back(drawn);
                return drawn;
            };

            // Through the margin, not the element's own red background: the backdrop moved by (-3, -2) CSS px.
            const auto box = device({16, 12, 48, 36}, s);
            const auto moved = keep(
                draw(frame({shifter(10, {16, 12, 32, 24}, {3, 2}, false, {{"background-color", "#ff0000"}})}), scale));
            compare(
                name + ", a shift through the margin", moved, w, h, {box},
                [&](unsigned x, unsigned y) {
                    return inside(x, y, box) ? behind(int(x) + 3 * s, int(y) + 2 * s) : behind(int(x), int(y));
                },
                1);

            // No margin: past the box, the copy's nearest texel, both ways.
            for (const float sign : {1.f, -1.f}) {
                const auto clamped =
                    keep(draw(frame({shifter(10, {16, 12, 32, 24}, {3 * sign, 2 * sign}, true)}), scale));
                compare(
                    name + (sign > 0 ? ", clamped to the box right and down" : ", clamped to the box left and up"),
                    clamped, w, h, {box},
                    [&](unsigned x, unsigned y) {
                        if (!inside(x, y, box)) {
                            return behind(int(x), int(y));
                        }
                        return behind(std::clamp(int(x) + int(3 * sign) * s, box[0], box[2] - 1),
                                      std::clamp(int(y) + int(2 * sign) * s, box[1], box[3] - 1));
                    },
                    1);
            }

            // The same function in a program whose manifest says nothing of the backdrop: no copy, a transparent read.
            auto undeclared = std::make_shared<weft::paint::EffectProgram>(*weft::effects::effect_backdrop_shift());
            undeclared->backdrop = false;
            undeclared->backdrop_margin = 0;
            auto blind = node(10, {16, 12, 32, 24});
            blind.effect = effect(undeclared, {{3, 2, 0, 0}});
            compare(
                name + ", an effect that does not read its backdrop", keep(draw(frame({blind}), scale)), w, h, {box},
                [&](unsigned x, unsigned y) {
                    return behind(int(x), int(y));
                },
                0);

            // At the target's edge: the copy ends there.
            const auto corner = device({40, 30, 64, 48}, s);
            const auto edged = keep(draw(frame({shifter(10, {40, 30, 24, 18}, {3, 2})}), scale));
            compare(
                name + ", clamped to the target's edge", edged, w, h, {corner},
                [&](unsigned x, unsigned y) {
                    return inside(x, y, corner) ? behind(int(x) + 3 * s, int(y) + 2 * s) : behind(int(x), int(y));
                },
                1);

            // Its own opacity group: the copy of the target that encloses the group, half over the backdrop, a white
            // child over it.
            auto grouped = shifter(10, {16, 12, 32, 24}, {3, 2}, false, {{"opacity", "0.5"}});
            grouped.children = {node(11, {4, 4, 6, 6}, {{"background-color", "#ffffff"}})};
            const auto child = device({20, 16, 26, 22}, s);
            const auto half = keep(draw(frame({grouped}), scale));
            compare(
                name + ", under its own opacity group", half, w, h, {box},
                [&](unsigned x, unsigned y) {
                    const auto under = behind(int(x), int(y));
                    if (!inside(x, y, box)) {
                        return under;
                    }
                    const auto over = inside(x, y, child) ? std::array<int, 4>{255, 255, 255, 255}
                                                          : behind(int(x) + 3 * s, int(y) + 2 * s);
                    std::array<int, 4> mixed{};
                    for (unsigned c = 0; c < 4; ++c) {
                        mixed[c] = int(std::lround(over[c] * .5 + under[c] * .5));
                    }
                    return mixed;
                },
                1);

            // Inside an ancestor's group: the group's content alone (its blue box), none of the backdrop beneath.
            auto parent = node(20, {8, 8, 48, 32}, {{"opacity", "0.5"}});
            parent.children = {node(21, {4, 4, 12, 12}, {{"background-color", "#2050ff"}}),
                               shifter(22, {8, 4, 24, 20}, {3, 2})};
            const auto blue_box = device({12, 12, 24, 24}, s);
            const auto reader = device({16, 12, 40, 32}, s);
            const std::array<int, 4> blue{255, 80, 32, 255};
            const auto ancestor = keep(draw(frame({parent}), scale));
            compare(
                name + ", inside an ancestor's opacity group", ancestor, w, h, {reader},
                [&](unsigned x, unsigned y) {
                    const auto under = behind(int(x), int(y));
                    const bool own = inside(x, y, blue_box);
                    const bool read = inside(x, y, reader) && inside(x + 3 * s, y + 2 * s, blue_box);
                    if (!own && !read) {
                        return under;
                    }
                    std::array<int, 4> mixed{};
                    for (unsigned c = 0; c < 4; ++c) {
                        mixed[c] = int(std::lround(blue[c] * .5 + under[c] * .5));
                    }
                    return mixed;
                },
                1);

            // Turned a quarter: the box's own axis across runs down the target.
            const auto turned_box = device({20, 8, 44, 40}, s);
            const auto turned = keep(
                draw(frame({shifter(10, {16, 12, 32, 24}, {3, 0}, false, {{"transform", "rotate(90deg)"}})}), scale));
            compare(
                name + ", along a turned box's axes", turned, w, h, {turned_box},
                [&](unsigned x, unsigned y) -> std::array<int, 4> {
                    const std::array<int, 4> inner{turned_box[0] + 1, turned_box[1] + 1, turned_box[2] - 1,
                                                   turned_box[3] - 1};
                    if (inside(x, y, inner)) {
                        return behind(int(x), int(y) + 3 * s);
                    }
                    return inside(x, y, turned_box) ? std::array<int, 4>{-1, -1, -1, -1} : behind(int(x), int(y));
                },
                1);

            // The samples: a refraction of no strength and a haze of no amplitude move nothing; a tint lies over the
            // view; a bevel bends the view near the edges alone; the same time draws the same haze, another time
            // another.
            const auto same_as_behind = [&](unsigned x, unsigned y) {
                return behind(int(x), int(y));
            };
            auto still = node(10, {16, 12, 32, 24});
            still.effect = effect(weft::effects::effect_refraction(),
                                  {{0, 0, 0, 0}, {18, 0, 0, 0}, {.2f, 0, 0, 0}, {40, 0, 0, 0}, {0, 0, 0, 0}});
            compare(name + ", refraction of no strength", keep(draw(frame({still}), scale)), w, h, {box},
                    same_as_behind, 1);
            auto tinted = node(10, {16, 12, 32, 24});
            tinted.effect = effect(weft::effects::effect_refraction(),
                                   {{0, 0, 0, 0}, {18, 0, 0, 0}, {.2f, 0, 0, 0}, {40, 0, 0, 0}, {0, 0, 1, .5f}});
            compare(
                name + ", refraction's tint over the view", keep(draw(frame({tinted}), scale)), w, h, {box},
                [&](unsigned x, unsigned y) {
                    const auto under = behind(int(x), int(y));
                    if (!inside(x, y, box)) {
                        return under;
                    }
                    std::array<float, 4> linear{};
                    for (unsigned c = 0; c < 3; ++c) {
                        linear[c] = weft::render::srgb_to_linear(float(under[2 - c]) / 255) * .5f;
                    }
                    linear[2] += .5f;
                    linear[3] = 1;
                    return encoded(linear);
                },
                1);
            auto bevelled = node(10, {16, 12, 32, 24});
            bevelled.effect = effect(weft::effects::effect_refraction(),
                                     {{6, 0, 0, 0}, {4, 0, 0, 0}, {0, 0, 0, 0}, {40, 0, 0, 0}, {0, 0, 0, 0}});
            const auto bent = keep(draw(frame({bevelled}), scale));
            const auto middle = device({20, 16, 44, 32}, s);
            compare(
                name + ", a bevel leaves the middle", bent, w, h, {box},
                [&](unsigned x, unsigned y) -> std::array<int, 4> {
                    if (inside(x, y, box) && !inside(x, y, middle)) {
                        return {-1, -1, -1, -1};
                    }
                    return behind(int(x), int(y));
                },
                1);
            bool bends{};
            for (unsigned y = unsigned(box[1]); y < unsigned(box[3]) && !bends; ++y) {
                for (unsigned x = unsigned(box[0]); x < unsigned(box[2]) && !bends; ++x) {
                    bends = !inside(x, y, middle) && at(bent, w, x, y) != behind(int(x), int(y));
                }
            }
            require(bends, name + ": a bevel bends the view near the edges");
            auto calm = node(10, {16, 12, 32, 24});
            calm.effect =
                effect(weft::effects::effect_heat_haze(), {{0, 0, 0, 0}, {28, 0, 0, 0}, {36, 0, 0, 0}, {.6f, 0, 0, 0}});
            compare(name + ", haze of no amplitude", keep(draw(frame({calm}, 1250), scale)), w, h, {box},
                    same_as_behind, 1);
            auto haze = node(10, {16, 12, 32, 24});
            haze.effect =
                effect(weft::effects::effect_heat_haze(), {{3, 0, 0, 0}, {28, 0, 0, 0}, {36, 0, 0, 0}, {.6f, 0, 0, 0}});
            const auto hazed = keep(draw(frame({haze}, 1250), scale));
            require(draw(frame({haze}, 1250), scale) == hazed, name + ": the same time draws the same haze");
            require(draw(frame({haze}, 2500), scale) != hazed && hazed != back, name + ": another time, another haze");
        }
        return out;
    }
} // namespace

int main() {
    try {
        // The D3D11 pass keeps the D2D reference raster for color glyph images, which needs COM.
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        lists();
        weft::testing::D3D11Scene one(width, height);
        weft::testing::D3D11Scene two(width * 2, height * 2);
        const auto warp = check("D3D11 WARP", [&](const weft::SceneFrame& f, float scale) {
            return scale == 1 ? one.draw(f) : two.draw(f, scale);
        });
        require(one.scene.backdrop_copies > 0 && two.scene.backdrop_copies == one.scene.backdrop_copies,
                "D3D11 copies counted");
        one.gpu.check_debug_messages();
        two.gpu.check_debug_messages();
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        namespace vk = weft::render::vulkan;
        std::string unavailable;
        if (const auto device = vk::Device::open({}, unavailable)) {
            vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
            const auto drawn = check("Vulkan", [&](const weft::SceneFrame& f, float scale) {
                auto pixels =
                    scene.draw(f, scale, unsigned(width * scale), unsigned(height * scale), std::nullopt, 0, {});
                require(pixels.has_value(), "native Vulkan pass");
                return std::move(*pixels);
            });
            int worst{};
            for (std::size_t i = 0; i < drawn.size(); ++i) {
                for (std::size_t k = 0; k < drawn[i].size(); ++k) {
                    worst = std::max(worst, std::abs(int(drawn[i][k]) - int(warp[i][k])));
                }
            }
            std::cout << "Vulkan vs D3D11 WARP: worst " << worst << '\n';
            require(drawn.size() == warp.size() && worst <= 2, "Vulkan against D3D11 WARP");
            // A target the pass may not read: a transparent copy and its reason; the effect draws nothing.
            const auto f = frame({shifter(10, {16, 12, 32, 24}, {3, 2})});
            const auto before = scene.reasons["backdrop of an unreadable target"];
            const auto unread = scene.draw(f, 1, width, height, std::nullopt, 0, {}, false);
            const auto plain = scene.draw(frame({}), 1, width, height, std::nullopt, 0, {});
            require(unread && plain && *unread == *plain &&
                        scene.reasons["backdrop of an unreadable target"] == before + 1 && !scene.fallbacks,
                    "an unreadable Vulkan target: a transparent copy and its reason");
            require(device->validation_messages == 0, "Vulkan validation");
            vulkan = device->name;
        } else {
            std::cout << "Vulkan skipped: " << unavailable << '\n';
        }
#endif
        std::cout << "PASS effects that read their backdrop: lists, D3D11 WARP at scale 1 and 2, Vulkan " << vulkan
                  << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
