#include "scene/scene_display_list.hpp"
#include "scene/scene_transform.hpp"
#include "scene/scene_values.hpp"
#include "base/css_scalar.hpp"
#include "constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace weft {
    namespace {
        using Matrix = std::array<float, 6>;
        using Style = SceneStyle;

        using css::trim;

        std::string get(const Style& s, const char* k, const char* d = "") {
            return css::style_value(s, k, d);
        }

        std::string get(const Style& s, PropertyId k, const char* d = "") {
            return std::string(s.text(k, d));
        }

        // The message is the fallback reason the native paint compiler reports.
        float number(std::string_view s) {
            const auto n =
                css::parse_number<float>(std::string(s), static_cast<float>(constants::scene::max_numeric_magnitude));
            if (!n) {
                throw std::invalid_argument("number");
            }
            return *n;
        }

        // Unitless numbers are accepted as px; em resolves against ref.
        float len(std::string_view s, float ref = 1) {
            const auto [digits, unit] = css::split_unit(s, {.px = true, .percent = true, .em = true});
            const float n = number(digits);
            if (unit == css::Unit::Percent) {
                return n * ref / constants::percent_scale;
            }
            return unit == css::Unit::Em ? n * ref : n;
        }

        std::vector<std::string> split(std::string_view s, char c = ' ') {
            return css::split(s, c, {.quotes = false}).parts;
        }

        std::array<float, 4> color(std::string s, const Style& style) {
            if (s == "currentcolor" || s == "currentColor") {
                s = get(style, PropertyId::Color, constants::default_color);
            }
            if (s == "transparent" || s == "none") {
                return {};
            }
            return parse_css_color(css::expand_color_shorthand(s));
        }

        Matrix multiply(Matrix a, Matrix b) {
            return {a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],        a[0] * b[2] + a[2] * b[3],
                    a[1] * b[2] + a[3] * b[3], a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]};
        }

        std::array<float, 2> point(Matrix m, float x, float y) {
            return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
        }

        // The node's compiled transform (an animation sample keeps full precision there), else its property, about
        // its transform-origin and placed at its box.
        Matrix matrix(const SceneNode& n) {
            const std::optional<PaintValue>* compiled =
                n.paint ? &n.paint->values[constants::scene::paint_slot_transform] : nullptr;
            std::optional<PaintValue> parsed;
            if (!compiled || !*compiled) {
                parsed = parse_scene_paint_value(get(n.style, PropertyId::Transform, "none"));
                compiled = &parsed;
            }
            const auto& t = *compiled;
            if (!t || t->kind != PaintValue::Transform) {
                throw std::invalid_argument("transform grammar");
            }
            if (std::any_of(t->operations.begin(), t->operations.end(), [](const auto& op) {
                    return op.kind == PaintOp::Perspective || op.kind == PaintOp::RotateY;
                })) {
                throw std::invalid_argument("projective transform");
            }
            const auto o = scene_transform_origin(n.style, n.box[2], n.box[3]);
            const auto m = SceneAffine::translation(-o[0], -o[1]) * scene_affine(t->operations, n.box[2], n.box[3]) *
                           SceneAffine::translation(o[0] + n.box[0], o[1] + n.box[1]);
            return {float(m.m11), float(m.m12), float(m.m21), float(m.m22), float(m.dx), float(m.dy)};
        }

        void gradient(paint::Gradient& g, float width, float height, const Style& s) {
            auto image = get(s, PropertyId::BackgroundImage, "none");
            if (image == "none") {
                return;
            }
            auto b = image.find('(');
            auto e = image.rfind(')');
            if (b == image.npos || e == image.npos) {
                throw std::invalid_argument("gradient grammar");
            }
            auto kind = image.substr(0, b);
            g.kind = kind == "linear-gradient"   ? paint::GradientKind::Linear
                     : kind == "radial-gradient" ? paint::GradientKind::Radial
                     : kind == "conic-gradient"  ? paint::GradientKind::Conic
                                                 : paint::GradientKind::None;
            if (g.kind == paint::GradientKind::None) {
                throw std::invalid_argument("background image");
            }
            auto a = split(image.substr(b + 1, e - b - 1), ',');
            if (a.empty()) {
                throw std::invalid_argument("gradient args");
            }
            g.angle = g.kind == paint::GradientKind::Linear
                          ? static_cast<float>(constants::scene::default_linear_gradient_angle)
                          : 0.f;
            if (g.kind == paint::GradientKind::Linear && a[0].ends_with("deg")) {
                g.angle = number(a[0].substr(0, a[0].size() - 3));
                a.erase(a.begin());
            }
            if (g.kind == paint::GradientKind::Radial && a[0] == "circle") {
                a.erase(a.begin());
            }
            if (g.kind == paint::GradientKind::Conic && a[0].starts_with("from ")) {
                g.angle = number(a[0].substr(5, a[0].size() - 8));
                a.erase(a.begin());
            }
            auto& tile = g.tile;
            tile[0] = width;
            tile[1] = height;
            auto size = split(get(s, PropertyId::BackgroundSize, "100% 100%"));
            if (size.size() == 2) {
                tile[0] = len(size[0], width);
                tile[1] = len(size[1], height);
            }
            if (tile[0] <= 0 || tile[1] <= 0) {
                throw std::invalid_argument("gradient tile");
            }
            // A length stop is a fraction of the gradient line in the tile, as the shader measures it: a linear
            // gradient's extent along its angle, a radial gradient's half diagonal. A conic gradient takes no
            // lengths.
            const float radians = g.angle * std::numbers::pi_v<float> / constants::degrees_per_half_turn;
            const float line = g.kind == paint::GradientKind::Radial ? std::hypot(tile[0] / 2, tile[1] / 2)
                               : g.kind == paint::GradientKind::Linear
                                   ? std::abs(tile[0] * std::sin(radians)) + std::abs(tile[1] * std::cos(radians))
                                   : 0.f;
            std::vector<bool> specified;
            for (auto text : a) {
                auto v = split(text);
                auto c = color(v.at(0), s);
                if (v.size() == 1) {
                    g.stops.push_back({0, c});
                    specified.push_back(false);
                } else {
                    for (std::size_t i = 1; i < v.size(); ++i) {
                        float offset = len(v[i]);
                        if (!v[i].ends_with('%')) {
                            if (line <= 0) {
                                throw std::invalid_argument("conic gradient length stop");
                            }
                            offset /= line;
                        }
                        g.stops.push_back({offset, c});
                        specified.push_back(true);
                    }
                }
            }
            if (g.stops.size() < 2 || g.stops.size() > constants::scene::max_gradient_stops) {
                throw std::invalid_argument("gradient stop bound");
            }
            if (!specified[0]) {
                g.stops[0].offset = 0;
            }
            if (!specified.back()) {
                g.stops.back().offset = 1;
            }
            specified[0] = specified.back() = true;
            float last = g.stops[0].offset;
            for (std::size_t i = 0; i < g.stops.size(); ++i) {
                if (specified[i]) {
                    g.stops[i].offset = std::max(last, g.stops[i].offset);
                    last = g.stops[i].offset;
                }
            }
            for (std::size_t i = 1; i < g.stops.size() - 1;) {
                if (specified[i]) {
                    ++i;
                    continue;
                }
                auto j = i;
                while (!specified[j]) {
                    ++j;
                }
                for (auto k = i; k < j; ++k) {
                    g.stops[k].offset = g.stops[i - 1].offset + (g.stops[j].offset - g.stops[i - 1].offset) *
                                                                    float(k - i + 1) / float(j - i + 1);
                }
                i = j;
            }
            auto pos = split(get(s, PropertyId::BackgroundPosition, "0% 0%"));
            if (!pos.empty()) {
                tile[2] = len(pos[0], width - tile[0]);
            }
            if (pos.size() > 1) {
                tile[3] = len(pos[1], height - tile[1]);
            }
        }
    } // namespace

    SceneDisplayList scene_display_list(const SceneFrame& f, const std::vector<NodeIdentity>& layers) {
        SceneDisplayList out;
        auto& list = out.list;
        // A node's own values as a raster source, without its subtree.
        const auto own = [](const SceneNode& n) {
            auto source = std::make_shared<SceneNode>();
            source->identity = n.identity;
            source->tag = n.tag;
            source->space = n.space;
            source->text = n.text;
            source->attributes = n.attributes;
            source->style = n.style;
            source->box = n.box;
            source->paint = n.paint;
            return source;
        };

        // The rounded clips of a subtree: a run of list.clips, outermost first.
        struct Chain {
            std::uint32_t begin{};
            std::uint32_t count{};
        };

        try {
            unsigned groups{};
            auto visit = [&](auto&& self, const SceneNode& n, Matrix parent, std::array<float, 4> clip,
                             Chain chain) -> void {
                if (get(n.style, PropertyId::Display) == "none" || n.box[2] <= 0 || n.box[3] <= 0) {
                    return;
                }
                if (list.items.size() > constants::scene::max_display_list_items) {
                    throw std::invalid_argument("native command bound");
                }
                if ((!n.space.empty() && n.space != constants::xhtml_namespace_uri)) {
                    throw std::invalid_argument("SVG reference");
                }
                paint::Item item;
                item.transform = multiply(parent, matrix(n));
                item.clip = clip;
                item.clips_begin = chain.begin;
                item.clips_count = chain.count;
                item.width = n.box[2];
                item.height = n.box[3];
                const auto& m = item.transform;
                float determinant = m[0] * m[3] - m[1] * m[2];
                if (std::abs(determinant) < constants::scene::singular_determinant_epsilon) {
                    return;
                }
                if (std::hypot(m[0], m[1]) + std::hypot(m[2], m[3]) > constants::scene::max_geometry_transform_scale) {
                    throw std::invalid_argument("transform scale bound");
                }
                paint::Box box;
                box.radii = scene_values::corner_radii(n.style, item.width, item.height);
                unsigned k{};
                auto paint_color = [&](unsigned i, const char* key, const char* def) {
                    if (n.paint && n.paint->values[i] && n.paint->values[i]->kind == PaintValue::Color) {
                        std::array<float, 4> c{};
                        for (unsigned j = 0; j < 4; ++j) {
                            c[j] = float(n.paint->values[i]->color[j]);
                        }
                        return c;
                    }
                    return color(get(n.style, key, def), n.style);
                };
                box.background =
                    paint_color(constants::scene::paint_slot_background_color, "background-color", "transparent");
                gradient(box.gradient, item.width, item.height, n.style);
                k = 0;
                for (const auto* edge : {"top", "right", "bottom", "left"}) {
                    auto prefix = std::string("border-") + edge;
                    if (get(n.style, (prefix + "-style").c_str(), "solid") != "solid" &&
                        len(get(n.style, (prefix + "-width").c_str(), "0")) > 0) {
                        throw std::invalid_argument("border style");
                    }
                    box.widths[k] = len(get(n.style, (prefix + "-width").c_str(), "0"));
                    box.edges[k] = paint_color(constants::scene::paint_slot_border_color + k,
                                               (prefix + "-color").c_str(), "currentcolor");
                    ++k;
                }
                float opacity = std::clamp(len(get(n.style, PropertyId::Opacity, "1")), 0.f, 1.f);
                if (n.paint && n.paint->values[constants::scene::paint_slot_opacity]) {
                    opacity = std::clamp(float(n.paint->values[constants::scene::paint_slot_opacity]->scalar.absolute +
                                               n.paint->values[constants::scene::paint_slot_opacity]->scalar.relative),
                                         0.f, 1.f);
                }
                if (std::find(layers.begin(), layers.end(), n.identity) != layers.end()) {
                    auto source = std::make_shared<SceneNode>(n);
                    source->box[0] = source->box[1] = 0;
                    source->style.erase(PropertyId::Transform);
                    source->style.erase(PropertyId::Opacity);
                    source->attributes.erase("style");
                    source->paint.reset();
                    item.kind = paint::Item::Kind::Layer;
                    item.opacity = opacity;
                    item.payload = std::uint32_t(list.rasters.size());
                    list.rasters.push_back({std::move(source)});
                    list.items.push_back(item);
                    return;
                }
                const bool painted = box.background[3] > 0 || box.gradient.kind != paint::GradientKind::None ||
                                     std::any_of(box.widths.begin(), box.widths.end(), [](float w) {
                                         return w > 0;
                                     });
                // Shadows of the border box with its corners as specified, as the reference raster draws them.
                const auto shadows = scene_values::box_shadows(
                    n.style, item.width, item.height, scene_values::specified_radii(n.style, item.width, item.height));
                const bool isolated = opacity < 1 && (!n.children.empty() || (!n.text.empty() && painted) ||
                                                      (!shadows.empty() && (painted || !n.text.empty())));
                if (!isolated) {
                    item.opacity = opacity;
                }
                if (isolated) {
                    if (++groups > constants::scene::max_opacity_group_depth) {
                        throw std::invalid_argument("opacity depth bound");
                    }
                    paint::Item g;
                    g.kind = paint::Item::Kind::BeginGroup;
                    g.opacity = opacity;
                    g.clip = clip;
                    g.clips_begin = chain.begin;
                    g.clips_count = chain.count;
                    list.items.push_back(g);
                }
                // Shadows lie under the box, bottom first.
                for (const auto& shadow : shadows) {
                    auto s = item;
                    s.kind = paint::Item::Kind::Shadow;
                    s.payload = std::uint32_t(list.shadows.size());
                    list.shadows.push_back(shadow);
                    list.items.push_back(s);
                }
                if (painted) {
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Box;
                    shape.payload = std::uint32_t(list.boxes.size());
                    list.boxes.push_back(box);
                    list.items.push_back(shape);
                }
                if (get(n.style, PropertyId::Overflow) == "hidden" || get(n.style, PropertyId::Overflow) == "auto") {
                    // Content clips to the padding box with its inner corners. A rotated or skewed box has no
                    // axis-aligned clip.
                    if (std::abs(m[1]) > constants::scene::axis_aligned_epsilon ||
                        std::abs(m[2]) > constants::scene::axis_aligned_epsilon) {
                        throw std::invalid_argument("rounded/transformed clip reference");
                    }
                    const auto padding = scene_values::padding_box(n.style, item.width, item.height);
                    auto a = point(m, padding.rect[0], padding.rect[1]);
                    auto b = point(m, padding.rect[0] + padding.rect[2], padding.rect[1] + padding.rect[3]);
                    clip = {std::max(clip[0], std::min(a[0], b[0])), std::max(clip[1], std::min(a[1], b[1])),
                            std::min(clip[2], std::max(a[0], b[0])), std::min(clip[3], std::max(a[1], b[1]))};
                    if (scene_values::is_rounded(padding)) {
                        RoundedClip rounded;
                        rounded.rect = {std::min(a[0], b[0]), std::min(a[1], b[1]), std::abs(b[0] - a[0]),
                                        std::abs(b[1] - a[1])};
                        // Corners TL, TR, BR, BL of the box land where a mirroring transform puts them.
                        const bool mirror_x = m[0] < 0;
                        const bool mirror_y = m[3] < 0;
                        for (unsigned corner = 0; corner < 4; ++corner) {
                            const bool right = corner == 1 || corner == 2;
                            const bool bottom = corner >= 2;
                            const bool world_right = right != mirror_x;
                            const bool world_bottom = bottom != mirror_y;
                            const unsigned world = world_bottom ? (world_right ? 2u : 3u) : (world_right ? 1u : 0u);
                            rounded.radius_x[world] = padding.radius_x[corner] * std::abs(m[0]);
                            rounded.radius_y[world] = padding.radius_y[corner] * std::abs(m[3]);
                        }
                        // The subtree's chain: the enclosing clips, then this one, as one run.
                        const std::vector<RoundedClip> enclosing(list.clips.begin() + chain.begin,
                                                                 list.clips.begin() + chain.begin + chain.count);
                        chain = {std::uint32_t(list.clips.size()), chain.count + 1};
                        list.clips.insert(list.clips.end(), enclosing.begin(), enclosing.end());
                        list.clips.push_back(rounded);
                    }
                }
                if (!n.text.empty()) {
                    auto t = item;
                    t.kind = paint::Item::Kind::Text;
                    t.clip = clip;
                    t.clips_begin = chain.begin;
                    t.clips_count = chain.count;
                    auto source = own(n);
                    source->box[0] = source->box[1] = 0;
                    source->style.erase(PropertyId::Transform);
                    source->style.erase(PropertyId::Opacity);
                    // Computed source values below are authoritative. Authored style
                    // still contains composited transform/opacity; retaining it in the
                    // image key rerasterizes static Unicode/emoji on every motion tick.
                    source->attributes.erase("style");
                    source->style[PropertyId::BackgroundColor] = "transparent";
                    source->style[PropertyId::BackgroundImage] = "none";
                    for (const auto* edge : {"top", "right", "bottom", "left"}) {
                        source->style[std::string("border-") + edge + "-color"] = "transparent";
                    }
                    source->paint.reset();
                    paint::Raster raster;
                    raster.mask = std::all_of(n.text.begin(), n.text.end(), [](unsigned char c) {
                        return c >= constants::scene::printable_ascii_first &&
                               c < constants::scene::printable_ascii_end;
                    });
                    if (raster.mask) {
                        raster.tint =
                            paint_color(constants::scene::paint_slot_color, "color", constants::default_color);
                        source->style[PropertyId::Color] = "#ffffff";
                    } else if (n.paint) {
                        source->paint.emplace();
                        source->paint->values[constants::scene::paint_slot_color] =
                            n.paint->values[constants::scene::paint_slot_color];
                    }
                    raster.source = std::move(source);
                    t.payload = std::uint32_t(list.rasters.size());
                    list.rasters.push_back(std::move(raster));
                    list.items.push_back(t);
                }
                std::vector<std::size_t> order;
                for (std::size_t i = 0; i < n.children.size(); ++i) {
                    order.push_back(i);
                }
                std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                    auto z = [&](auto i) {
                        auto v = get(n.children[i].style, PropertyId::ZIndex, "0");
                        return v == "auto" ? 0 : number(v);
                    };
                    return z(a) < z(b);
                });
                for (auto i : order) {
                    self(self, n.children[i], item.transform, clip, chain);
                }
                if (isolated) {
                    paint::Item g;
                    g.kind = paint::Item::Kind::EndGroup;
                    g.opacity = opacity;
                    g.clip = item.clip;
                    g.clips_begin = item.clips_begin;
                    g.clips_count = item.clips_count;
                    list.items.push_back(g);
                    --groups;
                }
            };
            visit(visit, f.root, {1, 0, 0, 1, 0, 0}, {0, 0, f.root.box[2], f.root.box[3]}, Chain{});
            if (list.items.size() > constants::scene::max_display_list_items) {
                throw std::invalid_argument("native command bound");
            }
        } catch (const std::invalid_argument& e) {
            list = {};
            out.fallback = e.what();
        } catch (const std::out_of_range& e) {
            list = {};
            out.fallback = e.what();
        }
        return out;
    }
} // namespace weft
