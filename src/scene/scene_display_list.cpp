#include "scene/scene_display_list.hpp"
#include "scene/scene_background.hpp"
#include "scene/scene_border_image.hpp"
#include "scene/scene_gradient.hpp"
#include "scene/scene_geometry.hpp"
#include "scene/scene_path.hpp"
#include "scene/scene_border.hpp"
#include "scene/scene_filter.hpp"
#include "scene/scene_inline.hpp"
#include "scene/scene_text.hpp"
#include "scene/scene_transform.hpp"
#include "scene/scene_values.hpp"
#include "base/css_scalar.hpp"
#include "constants/render.hpp"
#include "constants/scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace weft {
    namespace {
        using Matrix = std::array<float, 6>;

        // How many plane contents are being listed on this thread: a plane's content may hold planes, to a bound. And
        // how many filter contents.
        thread_local unsigned plane_nesting = 0;
        thread_local unsigned filter_nesting = 0;

        // Whether a node has a filter.
        bool filtered(const SceneNode& n) {
            const auto* v = n.style.find(PropertyId::Filter);
            return v && !css::trim(v->text()).empty() && css::trim(v->text()) != "none";
        }

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
        // its transform-origin and placed at its box (at the origin for an anchored element, which its anchor places).
        Matrix matrix(const SceneNode& n, bool at_origin = false) {
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
            const auto m =
                SceneAffine::translation(-o[0], -o[1]) * scene_affine(t->operations, n.box[2], n.box[3]) *
                SceneAffine::translation(o[0] + (at_origin ? 0 : n.box[0]), o[1] + (at_origin ? 0 : n.box[1]));
            return {float(m.m11), float(m.m12), float(m.m21), float(m.m22), float(m.dx), float(m.dy)};
        }

    } // namespace

    SceneDisplayCache::SceneDisplayCache(std::size_t max_entries, std::size_t max_bytes)
        : entries_(max_entries, max_bytes) {
    }

    const SceneSvgValues* SceneDisplayCache::find(const SceneNode& e) {
        const auto* entry = entries_.find({view_, e.identity.view_epoch, e.identity.slot, e.identity.generation});
        if (!entry || entry->tag != e.tag || entry->space != e.space || entry->text != e.text ||
            entry->attributes != e.attributes || !(entry->style == e.style)) {
            return nullptr;
        }
        ++reuses_;
        return &entry->values;
    }

    const SceneSvgValues& SceneDisplayCache::put(const SceneNode& e, SceneSvgValues values) {
        ++builds_;
        // What the entry holds: its own values' text and the paths.
        std::size_t bytes = sizeof(Entry) + e.tag.size() + e.space.size() + e.text.size();
        for (const auto& [name, value] : e.attributes) {
            bytes += name.size() + value.size();
        }
        for (const auto& [id, value] : e.style) {
            bytes += value.text().size();
        }
        for (const auto* path : {values.path.get(), values.outline.get()}) {
            if (path) {
                bytes += path->verbs.size() * sizeof(path->verbs[0]) + path->points.size() * sizeof(path->points[0]);
            }
        }
        return entries_
            .put({view_, e.identity.view_epoch, e.identity.slot, e.identity.generation},
                 Entry{e.tag, e.space, e.text, e.attributes, e.style, std::move(values)}, bytes)
            .values;
    }

    std::array<std::uint32_t, SceneListSpan::Parts> scene_list_sizes(const SceneDisplayList& out) {
        const auto& l = out.list;
        return {
            std::uint32_t(l.items.size()),     std::uint32_t(l.clips.size()),     std::uint32_t(l.boxes.size()),
            std::uint32_t(l.images.size()),    std::uint32_t(l.rasters.size()),   std::uint32_t(l.shadows.size()),
            std::uint32_t(l.paths.size()),     std::uint32_t(l.planes.size()),    std::uint32_t(l.effects.size()),
            std::uint32_t(l.particles.size()), std::uint32_t(l.backdrops.size()), std::uint32_t(l.path_shadows.size()),
            std::uint32_t(l.filters.size()),   std::uint32_t(out.anchors.size()), std::uint32_t(out.runs.size())};
    }

    SceneDisplayList scene_display_list(const SceneFrame& f, const std::vector<NodeIdentity>& layers, SceneText* text,
                                        float scale, SceneDisplayCache* cache, const SceneAnchorInput& anchors,
                                        const SceneListBuild& build) {
        SceneDisplayList out;
        auto& list = out.list;
        // Anchored elements lie in the root's box, clipped to it alone; deferred, at their origins, unclipped but by
        // their own clips.
        const std::array<double, 2> view{f.root.box[2], f.root.box[3]};
        const std::array<float, 4> view_clip{0, 0, f.root.box[2], f.root.box[3]};
        constexpr auto far_away = float(constants::scene::max_numeric_magnitude);
        const std::array<float, 4> unclipped{-far_away, -far_away, far_away, far_away};
        // Whether a descendant of n is anchored.
        const auto anchored_below = [](auto&& self, const SceneNode& n) -> bool {
            return std::any_of(n.children.begin(), n.children.end(), [&](const SceneNode& child) {
                return child.anchoring || self(self, child);
            });
        };
        // Whether a node has a backdrop filter.
        const auto filters = [](const SceneNode& n) {
            const auto* v = n.style.find(PropertyId::BackdropFilter);
            return v && !css::trim(v->text()).empty() && css::trim(v->text()) != "none";
        };
        // The host texture a node's background shows (-weft-texture(name)): its name, or none.
        const auto texture_of = [](const SceneNode& n) {
            auto name = scene_values::texture_name(scene_values::view(n.style, PropertyId::BackgroundImage, "none"));
            return name ? std::move(*name) : std::string{};
        };
        // What url() filters name: the given resolver, or the frame's own elements.
        std::optional<SceneFilterResolve> own_filters;
        if (!build.filters) {
            own_filters = scene_filter_resolver(f.root);
        }
        const SceneFilterResolve& resolve = build.filters ? *build.filters : *own_filters;
        // Whether what a subtree draws follows the frame's time or a host's texture beyond its own values: an effect
        // that reads the time, or a host texture.
        const auto follows_frame = [&](auto&& self, const SceneNode& n) -> bool {
            return (n.effect && n.effect->program->animated) || !texture_of(n).empty() ||
                   std::any_of(n.children.begin(), n.children.end(), [&](const SceneNode& child) {
                       return self(self, child);
                   });
        };
        // Whether a subtree shows an effect, particles, a backdrop filter, a host texture or a url() image: such a
        // node draws in the pass, never as a cached layer image, so that they follow the frame's time, their own
        // values, what lies behind them and what the host's texture holds at the frame (an image's texture is the
        // pass's, as an effect's).
        const auto shows_effect = [&](auto&& self, const SceneNode& n) -> bool {
            return n.effect || !n.particles.empty() || filters(n) || !texture_of(n).empty() || n.background_image ||
                   std::any_of(n.children.begin(), n.children.end(), [&](const SceneNode& child) {
                       return self(self, child);
                   });
        };
        // Whether a descendant of n has a backdrop filter.
        const auto filtered_below = [&](auto&& self, const SceneNode& n) -> bool {
            return std::any_of(n.children.begin(), n.children.end(), [&](const SceneNode& child) {
                return filters(child) || self(self, child);
            });
        };
        // The index of a program or an asset in the list, added the first time.
        const auto index_of = [](auto& leases, const auto& lease) {
            const auto found = std::find(leases.begin(), leases.end(), lease);
            if (found != leases.end()) {
                return std::uint32_t(found - leases.begin());
            }
            leases.push_back(lease);
            return std::uint32_t(leases.size() - 1);
        };
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
            // A chain with one more rounded clip: an axis-aligned rectangle whose corners a and b are its local
            // corners (0, 0) and (w, h) under m, with local corner radii, top-left first. The chain is the enclosing
            // clips, then this one, as one run.
            const auto add_rounded = [&](Chain chain, const Matrix& m, std::array<float, 2> a, std::array<float, 2> b,
                                         const std::array<float, 4>& radius_x, const std::array<float, 4>& radius_y) {
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
                    rounded.radius_x[world] = radius_x[corner] * std::abs(m[0]);
                    rounded.radius_y[world] = radius_y[corner] * std::abs(m[3]);
                }
                const std::vector<RoundedClip> enclosing(list.clips.begin() + chain.begin,
                                                         list.clips.begin() + chain.begin + chain.count);
                const Chain out{std::uint32_t(list.clips.size()), chain.count + 1};
                list.clips.insert(list.clips.end(), enclosing.begin(), enclosing.end());
                list.clips.push_back(rounded);
                return out;
            };
            const auto axis_aligned = [](const Matrix& m) {
                return std::abs(m[1]) <= constants::scene::axis_aligned_epsilon &&
                       std::abs(m[2]) <= constants::scene::axis_aligned_epsilon;
            };
            // A chain with one more rounded clip in a box's own space under m (local rect and radii): its map from
            // target px is the inverse of m. Used where m rotates or skews, so no target-space rectangle can hold it.
            const auto add_local = [&](Chain chain, const Matrix& m, const std::array<float, 4>& rect,
                                       const std::array<float, 4>& radius_x, const std::array<float, 4>& radius_y) {
                RoundedClip local;
                local.rect = rect;
                local.radius_x = radius_x;
                local.radius_y = radius_y;
                const float det = m[0] * m[3] - m[1] * m[2];
                local.to_local = {m[3] / det,
                                  -m[1] / det,
                                  -m[2] / det,
                                  m[0] / det,
                                  (m[2] * m[5] - m[3] * m[4]) / det,
                                  (m[1] * m[4] - m[0] * m[5]) / det};
                const std::vector<RoundedClip> enclosing(list.clips.begin() + chain.begin,
                                                         list.clips.begin() + chain.begin + chain.count);
                const Chain out{std::uint32_t(list.clips.size()), chain.count + 1};
                list.clips.insert(list.clips.end(), enclosing.begin(), enclosing.end());
                list.clips.push_back(local);
                return out;
            };
            // The target-space bounding rectangle of a local rectangle under m, cut to clip.
            const auto bound = [](std::array<float, 4> clip, const Matrix& m, const std::array<float, 4>& rect) {
                float left = std::numeric_limits<float>::infinity();
                float top = left;
                float right = -left;
                float bottom = -left;
                for (const auto& [x, y] :
                     std::array<std::array<float, 2>, 4>{{{rect[0], rect[1]},
                                                          {rect[0] + rect[2], rect[1]},
                                                          {rect[0], rect[1] + rect[3]},
                                                          {rect[0] + rect[2], rect[1] + rect[3]}}}) {
                    const auto p = point(m, x, y);
                    left = std::min(left, p[0]);
                    top = std::min(top, p[1]);
                    right = std::max(right, p[0]);
                    bottom = std::max(bottom, p[1]);
                }
                return std::array<float, 4>{std::max(clip[0], left), std::max(clip[1], top), std::min(clip[2], right),
                                            std::min(clip[3], bottom)};
            };
            const auto intersect = [](std::array<float, 4> clip, std::array<float, 2> a, std::array<float, 2> b) {
                return std::array<float, 4>{
                    std::max(clip[0], std::min(a[0], b[0])), std::max(clip[1], std::min(a[1], b[1])),
                    std::min(clip[2], std::max(a[0], b[0])), std::min(clip[3], std::max(a[1], b[1]))};
            };
            // SVG content under m, its user space to target px, as the reference raster draws it: an opacity group,
            // a clip-path of one user-space rectangle, the element's text, fill and stroke, then its children.
            auto svg = [&](auto&& self, const SceneNode& c, Matrix parent, std::array<float, 4> clip, Chain chain,
                           const SceneNode& root) -> void {
                if (get(c.style, PropertyId::Display) == "none" || c.tag == "defs" || c.tag == "clipPath") {
                    return;
                }
                if (list.items.size() > constants::scene::max_display_list_items) {
                    throw std::invalid_argument("native command bound");
                }
                if (filtered(c)) {
                    throw std::invalid_argument("filter on svg content");
                }
                static constexpr std::array<std::string_view, 11> drawn{
                    "circle", "ellipse", "g", "line", "path", "polygon", "polyline", "rect", "svg", "text", "#text"};
                if (std::find(drawn.begin(), drawn.end(), c.tag) == drawn.end()) {
                    throw std::invalid_argument("SVG element " + c.tag);
                }
                // The element's own values: kept ones while they are the same, else taken here in the order the
                // reference raster takes them (a singular element is skipped before its opacity, text and path).
                const SceneSvgValues* kept = cache ? cache->find(c) : nullptr;
                SceneSvgValues taken;
                if (!kept) {
                    const auto t = c.attributes.contains("transform") ? c.attributes.at("transform")
                                                                      : get(c.style, PropertyId::Transform);
                    const auto local = scene_affine(parse_scene_transform(t, true).value(), 0, 0);
                    taken.local = {float(local.m11), float(local.m12), float(local.m21),
                                   float(local.m22), float(local.dx),  float(local.dy)};
                }
                const auto m = multiply(parent, kept ? kept->local : taken.local);
                if (std::abs(m[0] * m[3] - m[1] * m[2]) < constants::scene::singular_determinant_epsilon) {
                    return;
                }
                if (!kept) {
                    taken.opacity = std::clamp(len(get(c.style, PropertyId::Opacity, "1")), 0.f, 1.f);
                }
                const float opacity = kept ? kept->opacity : taken.opacity;
                // clip-path="url(#id)" of one user-space rectangle, as in the reference raster.
                auto inner_clip = clip;
                auto inner_chain = chain;
                const auto clip_path = c.attributes.find("clip-path");
                if (clip_path != c.attributes.end() && !clip_path->second.empty() && clip_path->second != "none") {
                    const auto& v = clip_path->second;
                    if (!v.starts_with("url(#") || !v.ends_with(')')) {
                        throw std::invalid_argument("local clip-path required");
                    }
                    const auto id = v.substr(5, v.size() - 6);
                    auto find = [&](auto&& search, const SceneNode& n) -> const SceneNode* {
                        if (n.attributes.contains("id") && n.attributes.at("id") == id) {
                            return &n;
                        }
                        for (const auto& child : n.children) {
                            if (auto* p = search(search, child)) {
                                return p;
                            }
                        }
                        return nullptr;
                    };
                    const auto* definition = find(find, root);
                    if (!definition || definition->tag != "clipPath" || definition->children.size() != 1 ||
                        definition->children[0].tag != "rect" ||
                        scene_values::get(definition->attributes, "clipPathUnits", "userSpaceOnUse") !=
                            "userSpaceOnUse") {
                        throw std::invalid_argument("clipPath requires one user-space rect");
                    }
                    const auto& r = definition->children[0].attributes;
                    const auto v_of = [&](const char* k, float def = 0) {
                        const auto i = r.find(k);
                        return i == r.end() ? def : len(i->second);
                    };
                    const float x = v_of("x");
                    const float y = v_of("y");
                    const float w = v_of("width");
                    const float h = v_of("height");
                    // Corner radii as the reference's rounded rectangle takes them: one for both, at most half a side.
                    const float rx = std::clamp(v_of("rx"), 0.f, std::max(w, 0.f) / 2);
                    const float ry = std::clamp(v_of("ry", v_of("rx")), 0.f, std::max(h, 0.f) / 2);
                    const std::array<float, 4> radius_x{rx, rx, rx, rx};
                    const std::array<float, 4> radius_y{ry, ry, ry, ry};
                    if (axis_aligned(m)) {
                        const auto a = point(m, x, y);
                        const auto b = point(m, x + w, y + h);
                        inner_clip = intersect(clip, a, b);
                        if (rx > 0 && ry > 0) {
                            inner_chain = add_rounded(chain, m, a, b, radius_x, radius_y);
                        }
                    } else {
                        inner_clip = bound(clip, m, {x, y, std::max(w, 0.f), std::max(h, 0.f)});
                        inner_chain = add_local(chain, m, {x, y, std::max(w, 0.f), std::max(h, 0.f)},
                                                rx > 0 && ry > 0 ? radius_x : std::array<float, 4>{},
                                                rx > 0 && ry > 0 ? radius_y : std::array<float, 4>{});
                    }
                }
                const bool has_text = c.tag == "text" && !c.text.empty();
                if (has_text && !text) {
                    throw std::invalid_argument("SVG text reference");
                }
                if (has_text && !scene_values::text_shadows(c.style).empty()) {
                    throw std::invalid_argument("text-shadow on svg text");
                }
                if (!kept) {
                    if (has_text) {
                        // Placed as the reference raster places it: x and y with dy, text-anchor, and the baseline
                        // (or the middle, for a central dominant-baseline) on y.
                        const auto metrics = text->measure(c.text, c.style);
                        const float size = len(get(c.style, PropertyId::FontSize, constants::default_font_size));
                        float x = c.attributes.contains("x") ? len(c.attributes.at("x")) : 0;
                        float y = c.attributes.contains("y") ? len(c.attributes.at("y")) : 0;
                        if (c.attributes.contains("dy")) {
                            y += len(c.attributes.at("dy"), size);
                        }
                        const auto anchor = get(c.style, PropertyId::TextAnchor, "start");
                        if (anchor == "middle") {
                            x -= metrics.width / 2;
                        } else if (anchor == "end") {
                            x -= metrics.width;
                        }
                        y -= get(c.style, PropertyId::DominantBaseline) == "central" ? metrics.height / 2
                                                                                     : metrics.baseline;
                        taken.text_at = {x, y};
                        taken.text_metrics = metrics;
                        taken.text_fill = color(get(c.style, PropertyId::Fill, "#000000"), c.style);
                        // Glyph outlines as a path, drawn under the whole transform like any SVG shape; text with
                        // color glyphs stays an image.
                        if (taken.text_fill[3] > 0) {
                            taken.outline = text->outline(c.text, c.style);
                        }
                    }
                    if (auto path = scene_svg_path(c)) {
                        const auto fill = get(c.style, PropertyId::Fill, constants::default_color);
                        if (fill != "none" && c.tag != "line") {
                            taken.fill = color(fill, c.style);
                            taken.fill[3] *= std::clamp(len(get(c.style, PropertyId::FillOpacity, "1")), 0.f, 1.f);
                        }
                        const auto stroke = get(c.style, PropertyId::Stroke, "none");
                        if (stroke != "none") {
                            taken.stroke = color(stroke, c.style);
                            taken.stroke[3] *= std::clamp(len(get(c.style, PropertyId::StrokeOpacity, "1")), 0.f, 1.f);
                            taken.stroke_style = scene_svg_stroke(c.style);
                        }
                        taken.path = std::move(path);
                        taken.shape = scene_svg_shape(c);
                    }
                    kept = cache ? &cache->put(c, std::move(taken)) : &taken;
                }
                const auto& v = *kept;
                std::vector<paint::Item> own;
                const auto base = [&] {
                    paint::Item i;
                    i.transform = m;
                    i.clip = inner_clip;
                    i.clips_begin = inner_chain.begin;
                    i.clips_count = inner_chain.count;
                    return i;
                };
                if (has_text) {
                    const auto& metrics = v.text_metrics;
                    const auto& fill = v.text_fill;
                    const float x = v.text_at[0];
                    const float y = v.text_at[1];
                    if (v.outline) {
                        auto item = base();
                        item.kind = paint::Item::Kind::Path;
                        item.transform = multiply(m, {1, 0, 0, 1, x, y});
                        item.payload = std::uint32_t(list.paths.size());
                        list.paths.push_back({v.outline, fill, std::nullopt});
                        own.push_back(item);
                    } else if (fill[3] > 0 && metrics.width > 0 && metrics.height > 0) {
                        auto source = std::make_shared<SceneNode>();
                        source->identity = c.identity;
                        source->tag = c.tag;
                        source->text = c.text;
                        source->style = c.style;
                        source->style.erase(PropertyId::Transform);
                        source->style.erase(PropertyId::Opacity);
                        source->box = {0, 0, metrics.width, metrics.height};
                        paint::Raster raster;
                        raster.mask = std::all_of(c.text.begin(), c.text.end(), [](unsigned char ch) {
                            return ch >= constants::scene::printable_ascii_first &&
                                   ch < constants::scene::printable_ascii_end;
                        });
                        if (raster.mask) {
                            raster.tint = fill;
                            source->style[PropertyId::Color] = "#ffffff";
                        } else {
                            char value[96];
                            std::snprintf(value, sizeof value, "rgba(%d, %d, %d, %.9g)",
                                          int(std::lround(fill[0] * constants::color_channel_max)),
                                          int(std::lround(fill[1] * constants::color_channel_max)),
                                          int(std::lround(fill[2] * constants::color_channel_max)), double(fill[3]));
                            source->style[PropertyId::Color] = value;
                        }
                        raster.source = std::move(source);
                        auto item = base();
                        item.kind = paint::Item::Kind::Text;
                        item.transform = multiply(m, {1, 0, 0, 1, x, y});
                        item.width = metrics.width;
                        item.height = metrics.height;
                        item.payload = std::uint32_t(list.rasters.size());
                        list.rasters.push_back(std::move(raster));
                        own.push_back(item);
                    }
                }
                // A shape the box shader draws as it is, as a box in the element's user units: no coverage to
                // rasterize, and boxes draw in batches.
                const auto add_box = [&](const Matrix& local, float width, float height, const paint::Box& value) {
                    auto item = base();
                    item.kind = paint::Item::Kind::Box;
                    item.transform = multiply(m, local);
                    item.width = width;
                    item.height = height;
                    item.payload = std::uint32_t(list.boxes.size());
                    list.boxes.push_back(value);
                    own.push_back(item);
                };
                using ShapeKind = SceneSvgShape::Kind;
                // A rectangle with circular corners (or square ones), or a circle.
                const auto fill_box = [&] {
                    if (!v.shape || v.shape->rx != v.shape->ry) {
                        return false;
                    }
                    const auto& s = *v.shape;
                    paint::Box value;
                    value.background = v.fill;
                    value.radii.fill(float(s.rx));
                    if (s.kind == ShapeKind::Rect) {
                        add_box({1, 0, 0, 1, float(s.x), float(s.y)}, float(s.w), float(s.h), value);
                        return true;
                    }
                    if (s.kind == ShapeKind::Ellipse) {
                        add_box({1, 0, 0, 1, float(s.x - s.rx), float(s.y - s.rx)}, float(2 * s.rx), float(2 * s.rx),
                                value);
                        return true;
                    }
                    return false;
                };
                // A solid stroke centered on such an outline: a border box half the stroke out, its corners the
                // outline's grown by as much (square corners meet in a miter or a round join; a bevel cuts them). A
                // line's stroke: a box along it, its caps butt, square or round.
                const auto stroke_box = [&] {
                    const auto& style = v.stroke_style;
                    if (!v.shape || !style.dashes.empty()) {
                        return false;
                    }
                    const auto& s = *v.shape;
                    const float half = style.width / 2;
                    paint::Box value;
                    if (s.kind == ShapeKind::Line) {
                        const double dx = s.w - s.x;
                        const double dy = s.h - s.y;
                        const double length = std::hypot(dx, dy);
                        if (!(length > 0)) {
                            return false;
                        }
                        const float extend = style.cap == paint::Stroke::Cap::Butt ? 0.f : half;
                        if (style.cap == paint::Stroke::Cap::Round) {
                            value.radii.fill(half);
                        }
                        value.background = v.stroke;
                        // A level or upright line is a box of the user space, which covers each pixel by its share
                        // as a path's coverage does (a turned box antialiases by distance).
                        if (dx == 0 || dy == 0) {
                            const float left = float(std::min(s.x, s.w));
                            const float top = float(std::min(s.y, s.h));
                            if (dy == 0) {
                                add_box({1, 0, 0, 1, left - extend, top - half}, float(std::abs(dx)) + 2 * extend,
                                        style.width, value);
                            } else {
                                add_box({1, 0, 0, 1, left - half, top - extend}, style.width,
                                        float(std::abs(dy)) + 2 * extend, value);
                            }
                            return true;
                        }
                        // Along the line from its start, across it centered on it.
                        const Matrix along{float(dx / length), float(dy / length), float(-dy / length),
                                           float(dx / length), float(s.x),         float(s.y)};
                        add_box(multiply(along, {1, 0, 0, 1, -extend, -half}), float(length) + 2 * extend, style.width,
                                value);
                        return true;
                    }
                    if (s.rx != s.ry || (s.kind == ShapeKind::Rect && s.rx == 0 &&
                                         (style.join == paint::Stroke::Join::Bevel ||
                                          (style.join == paint::Stroke::Join::Miter &&
                                           style.miter_limit < std::numbers::sqrt2_v<float>)))) {
                        return false;
                    }
                    value.widths.fill(style.width);
                    value.edges.fill(v.stroke);
                    const bool square = s.kind == ShapeKind::Rect && s.rx == 0;
                    value.radii.fill(square ? (style.join == paint::Stroke::Join::Round ? half : 0.f)
                                            : float(s.rx) + half);
                    if (s.kind == ShapeKind::Rect) {
                        add_box({1, 0, 0, 1, float(s.x) - half, float(s.y) - half}, float(s.w) + style.width,
                                float(s.h) + style.width, value);
                    } else {
                        add_box({1, 0, 0, 1, float(s.x - s.rx) - half, float(s.y - s.rx) - half},
                                float(2 * s.rx) + style.width, float(2 * s.rx) + style.width, value);
                    }
                    return true;
                };
                if (v.path) {
                    if (v.fill[3] > 0 && !fill_box()) {
                        auto item = base();
                        item.kind = paint::Item::Kind::Path;
                        item.payload = std::uint32_t(list.paths.size());
                        list.paths.push_back({v.path, v.fill, std::nullopt});
                        own.push_back(item);
                    }
                    if (v.stroke[3] > 0 && v.stroke_style.width > 0 && !stroke_box()) {
                        auto item = base();
                        item.kind = paint::Item::Kind::Path;
                        item.payload = std::uint32_t(list.paths.size());
                        list.paths.push_back({v.path, v.stroke, v.stroke_style});
                        own.push_back(item);
                    }
                }
                // An opacity group, unless one item stands alone and takes the opacity itself. The group's composite
                // is not clipped by the element's own clip-path, as in the reference.
                const bool group = opacity < 1 && (!c.children.empty() || own.size() > 1);
                if (group) {
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
                } else if (opacity < 1 && own.size() == 1) {
                    own[0].opacity = opacity;
                }
                list.items.insert(list.items.end(), own.begin(), own.end());
                for (const auto& child : c.children) {
                    self(self, child, m, inner_clip, inner_chain, root);
                }
                if (group) {
                    paint::Item g;
                    g.kind = paint::Item::Kind::EndGroup;
                    g.opacity = opacity;
                    g.clip = clip;
                    g.clips_begin = chain.begin;
                    g.clips_count = chain.count;
                    list.items.push_back(g);
                    --groups;
                }
            };
            // Deferred: the anchored element child's visit added at index before (1 + it), and the end of its items;
            // 0 when it added none.
            const auto anchor_of = [&](std::size_t before, const SceneNode& child) -> std::uint32_t {
                if (out.anchors.size() <= before || out.anchors[before].element != child.identity) {
                    return 0;
                }
                out.anchors[before].end = std::uint32_t(list.items.size());
                return std::uint32_t(before + 1);
            };

            // The span of a visit of a frame's own node: its context as it starts, its ranges as it ends.
            struct Spanned {
                SceneListSpans* spans{};
                const SceneNode* node{};
                const SceneDisplayList& out;
                SceneListSpan span;

                ~Spanned() {
                    if (spans) {
                        span.end = scene_list_sizes(out);
                        spans->insert_or_assign(node, span);
                    }
                }
            };

            // framed: n is a node of the frame, not a piece of one (a fragment of an inline box, its content without
            // its box).
            auto visit = [&](auto&& self, const SceneNode& n, Matrix parent, std::array<float, 4> clip, Chain chain,
                             bool framed) -> void {
                Spanned spanned{framed ? build.spans : nullptr, &n, out, {}};
                if (spanned.spans) {
                    spanned.span = {parent, clip, chain.begin, chain.count, groups, scene_list_sizes(out), {}};
                }
                // A box of no area paints nothing of its own; what overflows it still shows (scene_shows_overflow).
                const bool empty = n.box[2] <= 0 || n.box[3] <= 0;
                if (get(n.style, PropertyId::Display) == "none" || (empty && !scene_shows_overflow(n))) {
                    return;
                }
                // visibility: hidden paints nothing of its own either, its text included; its children paint by their
                // own visibility. bare: the node paints nothing of its own (no area, or hidden).
                const bool invisible = scene_values::invisible(n.style);
                const bool bare = empty || invisible;
                if (list.items.size() > constants::scene::max_display_list_items) {
                    throw std::invalid_argument("native command bound");
                }
                // An anchored element: its anchor places it in the root's box, beyond its ancestors' transforms and
                // clips; hidden without one.
                if (n.anchoring) {
                    if (!n.fragments.empty()) {
                        throw std::invalid_argument("anchored inline box");
                    }
                    if (plane_nesting ||
                        get(n.style, PropertyId::Transform, "none").find("perspective(") != std::string::npos) {
                        throw std::invalid_argument("anchored element in a perspective plane");
                    }
                    const auto local = matrix(n, true);
                    if (anchors.deferred) {
                        // At its origin, for the pass to move; its items' range ends where its caller finds it.
                        out.anchors.push_back({n.identity,
                                               *n.anchoring,
                                               {n.box[2], n.box[3]},
                                               local,
                                               std::uint32_t(list.items.size()),
                                               std::uint32_t(list.items.size())});
                        parent = {1, 0, 0, 1, 0, 0};
                        clip = unclipped;
                    } else {
                        const auto* anchor = scene_anchor_resolve(anchors, n.identity, *n.anchoring, &out.placed);
                        const auto map = scene_anchor_map(*n.anchoring, anchor, {n.box[2], n.box[3]}, view,
                                                          {local[0], local[1], local[2], local[3], local[4], local[5]});
                        if (!map) {
                            return;
                        }
                        parent = {float((*map)[0]), float((*map)[1]), float((*map)[2]),
                                  float((*map)[3]), float((*map)[4]), float((*map)[5])};
                        clip = view_clip;
                    }
                    chain = {};
                }
                // filter: an svg element's one drop shadow of the shapes it paints is a path shadow (below); any other
                // filter draws the element and its subtree as a filtered image (below), an inline element whole.
                const bool path_shadow =
                    n.space == constants::svg_namespace_uri && n.tag == "svg" && scene_path_shadowed(n);
                std::vector<paint::FilterOp> filter_ops;
                if (!path_shadow && filtered(n)) {
                    auto parsed = scene_filter(get(n.style, PropertyId::Filter, "none"), n.style, resolve);
                    if (!parsed) {
                        throw std::invalid_argument(parsed.error().message);
                    }
                    filter_ops = std::move(*parsed);
                }
                // An inline element in lines: its box a fragment at a time, then its text and children.
                if (!n.fragments.empty() && filter_ops.empty()) {
                    for (const auto& box : scene_fragment_boxes(n)) {
                        self(self, box, parent, clip, chain, false);
                    }
                    self(self, scene_without_box(n), parent, clip, chain, false);
                    return;
                }
                // A perspective plane, as the reference raster draws it: the subtree flat in the node's own px, then
                // projected through the node's plane.
                if (get(n.style, PropertyId::Transform, "none").find("perspective(") != std::string::npos) {
                    if (plane_nesting >= constants::max_plane_depth) {
                        throw std::invalid_argument("plane nesting bound");
                    }
                    // An edge-on plane paints nothing, as in the reference raster.
                    const auto perspective =
                        scene_perspective(parse_scene_transform(get(n.style, PropertyId::Transform, "none")).value());
                    if (perspective &&
                        std::abs(std::cos(perspective->radians)) < constants::scene::edge_on_plane_epsilon) {
                        return;
                    }
                    SceneFrame content;
                    content.root = n;
                    content.root.box[0] = content.root.box[1] = 0;
                    content.root.style.erase(PropertyId::Transform);
                    if (content.root.paint) {
                        content.root.paint->values[constants::scene::paint_slot_transform].reset();
                    }
                    struct Nesting {
                        Nesting() {
                            ++plane_nesting;
                        }

                        ~Nesting() {
                            --plane_nesting;
                        }
                    };
                    SceneDisplayList nested;
                    {
                        Nesting guard;
                        nested = scene_display_list(content, {}, text, scale, cache);
                    }
                    if (!nested.fallback.empty()) {
                        throw std::invalid_argument(nested.fallback);
                    }
                    const auto plane =
                        affine_plane(SceneAffine{parent[0], parent[1], parent[2], parent[3], parent[4], parent[5]}) *
                        scene_plane(n);
                    // The plane lies in front of the viewer at every corner.
                    for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{
                             {{0, 0}, {n.box[2], 0}, {0, n.box[3]}, {n.box[2], n.box[3]}}}) {
                        if (!(plane.m[6] * x + plane.m[7] * y + plane.m[8] > constants::plane_min_depth)) {
                            throw std::invalid_argument("plane behind the viewer");
                        }
                    }
                    paint::Plane value;
                    value.content = std::make_shared<const paint::DisplayList>(std::move(nested.list));
                    value.width = n.box[2];
                    value.height = n.box[3];
                    for (unsigned k = 0; k < value.transform.size(); ++k) {
                        value.transform[k] = float(plane.m[k]);
                    }
                    paint::Item item;
                    item.kind = paint::Item::Kind::Plane;
                    item.clip = clip;
                    item.clips_begin = chain.begin;
                    item.clips_count = chain.count;
                    item.width = n.box[2];
                    item.height = n.box[3];
                    item.payload = std::uint32_t(list.planes.size());
                    list.planes.push_back(std::move(value));
                    list.items.push_back(item);
                    return;
                }
                // An svg element is a box; the SVG elements inside it are drawn in its viewport (svg below).
                const bool svg_root = n.space == constants::svg_namespace_uri && n.tag == "svg";
                if (!n.space.empty() && n.space != constants::xhtml_namespace_uri && !svg_root) {
                    throw std::invalid_argument("SVG reference");
                }
                const auto dropped = path_shadow ? scene_values::drop_shadow(n.style) : std::nullopt;
                paint::Item item;
                item.transform = multiply(parent, matrix(n, bool(n.anchoring)));
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
                if (auto g = bare ? std::nullopt : scene_gradient(n.style, item.width, item.height)) {
                    box.gradient = std::move(*g);
                }
                // Solid sides are the box's; dotted and dashed ones are Chrome's shapes, drawn apart. A dotted or
                // dashed side keeps its width in the box, transparent, so the solid sides beside it are mitered at
                // the corners as Chrome draws them over its dots.
                std::array<SceneBorderStyle, 4> styles{};
                std::array<float, 4> dashed_widths{};
                std::array<std::array<float, 4>, 4> dash_colors{};
                bool bordered{};
                k = 0;
                for (const auto* edge : {"top", "right", "bottom", "left"}) {
                    auto prefix = std::string("border-") + edge;
                    if (!scene_border_style(get(n.style, (prefix + "-style").c_str(), "solid"), styles[k])) {
                        throw std::invalid_argument("border style");
                    }
                    const float width = len(get(n.style, (prefix + "-width").c_str(), "0"));
                    const bool stroked = styles[k] == SceneBorderStyle::Dotted || styles[k] == SceneBorderStyle::Dashed;
                    box.widths[k] = styles[k] == SceneBorderStyle::None ? 0 : width;
                    dashed_widths[k] = stroked ? width : 0;
                    bordered = bordered || (styles[k] == SceneBorderStyle::Solid && width > 0);
                    const auto edge_color = paint_color(constants::scene::paint_slot_border_color + k,
                                                        (prefix + "-color").c_str(), "currentcolor");
                    dash_colors[k] = edge_color;
                    box.edges[k] = stroked ? std::array<float, 4>{} : edge_color;
                    ++k;
                }
                bordered = bordered && !bare;
                // border-image: its pieces instead of the border styles (scene_border_image.hpp); the border widths
                // still give the background its boxes.
                std::vector<SceneBorderImagePiece> border_pieces;
                if (!bare && n.border_image && n.border_image->width && n.border_image->height) {
                    border_pieces = scene_border_image(n.style, item.width, item.height, float(n.border_image->width),
                                                       float(n.border_image->height));
                }
                if (!border_pieces.empty()) {
                    box.widths = {};
                    box.edges = {};
                    bordered = false;
                    dashed_widths = {};
                }
                const bool dashed = !bare && std::any_of(dashed_widths.begin(), dashed_widths.end(), [](float w) {
                    return w > 0;
                });
                if (dashed && std::any_of(box.radii.begin(), box.radii.end(), [](float r) {
                        return r > 0;
                    })) {
                    throw std::invalid_argument("dotted or dashed border with rounded corners");
                }
                float opacity = std::clamp(len(get(n.style, PropertyId::Opacity, "1")), 0.f, 1.f);
                if (n.paint && n.paint->values[constants::scene::paint_slot_opacity]) {
                    opacity = std::clamp(float(n.paint->values[constants::scene::paint_slot_opacity]->scalar.absolute +
                                               n.paint->values[constants::scene::paint_slot_opacity]->scalar.relative),
                                         0.f, 1.f);
                }
                // filter (Filter Effects 1): the element and its subtree drawn alone into an image, through the
                // filter's functions in order, then drawn with the element's transform, opacity and clips. The image
                // covers the content's ink and what the functions spread, on the device pixels of the element's own
                // space; it is keyed by the element without its transform and opacity, so that they change without
                // drawing it again (paint::Filter).
                if (!filter_ops.empty()) {
                    if (filters(n)) {
                        throw std::invalid_argument("backdrop-filter and filter on one element");
                    }
                    auto content = scene_filter_content(n, f.time_ms, text, scale, cache, &resolve);
                    if (!content.listed.fallback.empty()) {
                        throw std::invalid_argument(content.listed.fallback);
                    }
                    const auto region = scene_filter_region(content.listed.list, filter_ops, scale);
                    const auto& px = region.pixels;
                    if (px[2] <= px[0] || px[3] <= px[1]) {
                        return;
                    }
                    const float left = float(px[0]) / region.resolution;
                    const float top = float(px[1]) / region.resolution;
                    auto moved = std::make_shared<paint::DisplayList>();
                    if (const auto appended = paint::append(*moved, content.listed.list, {1, 0, 0, 1, -left, -top});
                        !appended) {
                        throw std::invalid_argument(appended.error().message);
                    }
                    paint::Filter value;
                    value.content = std::move(moved);
                    value.ops = std::move(filter_ops);
                    value.pixels = {std::uint32_t(px[2] - px[0]), std::uint32_t(px[3] - px[1])};
                    value.resolution = region.resolution;
                    if (!follows_frame(follows_frame, n)) {
                        value.source = std::move(content.source);
                    }
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Filter;
                    shape.transform = multiply(item.transform, {1, 0, 0, 1, left, top});
                    shape.width = float(px[2] - px[0]) / region.resolution;
                    shape.height = float(px[3] - px[1]) / region.resolution;
                    shape.opacity = opacity;
                    shape.payload = std::uint32_t(list.filters.size());
                    list.filters.push_back(std::move(value));
                    list.items.push_back(shape);
                    return;
                }
                // A layer image holds its subtree in place: an anchored descendant moves on its own, so such a node
                // draws in the pass.
                if (!bare && std::find(layers.begin(), layers.end(), n.identity) != layers.end() &&
                    !shows_effect(shows_effect, n) && !anchored_below(anchored_below, n)) {
                    auto source = std::make_shared<SceneNode>(n);
                    source->box[0] = source->box[1] = 0;
                    source->anchoring.reset();
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
                // The element's first item: an effect that reads its backdrop reads what the target held before it.
                const auto first_item = std::uint32_t(list.items.size());
                // A background image laid out as CSS lays one out: a host texture or a url() image.
                const auto host_texture = texture_of(n);
                const bool fitted = !bare && (!host_texture.empty() || n.background_image);
                const bool painted =
                    !bare && (box.background[3] > 0 || box.gradient.kind != paint::GradientKind::None || bordered ||
                              dashed || n.effect || fitted || !border_pieces.empty());
                // Shadows of the border box with its corners as specified, as the reference raster draws them.
                const auto shadows =
                    bare ? std::vector<scene_values::BoxShadow>{}
                         : scene_values::box_shadows(n.style, item.width, item.height,
                                                     scene_values::specified_radii(n.style, item.width, item.height));
                // backdrop-filter: what lies behind the box, filtered, under everything the element paints. An element
                // whose opacity or a descendant's backdrop needs a group of its own draws it at the group's end, beneath
                // what the group holds (a descendant's backdrop holds the element's own painting alone).
                const auto filtered = scene_backdrop_filter(get(n.style, PropertyId::BackdropFilter, "none"));
                if (!filtered) {
                    throw std::invalid_argument(filtered.error().message);
                }
                const auto backdrop = bare ? std::remove_cvref_t<decltype(*filtered)>{} : *filtered;
                const bool backdrop_group = backdrop && (opacity < 1 || filtered_below(filtered_below, n));
                const auto add_backdrop = [&](bool under) {
                    paint::Backdrop b;
                    b.blur = backdrop->blur;
                    b.pre = backdrop->pre;
                    b.post = backdrop->post;
                    b.radii = box.radii;
                    b.under = under;
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Backdrop;
                    shape.opacity = 1;
                    shape.payload = std::uint32_t(list.backdrops.size());
                    list.backdrops.push_back(b);
                    list.items.push_back(shape);
                };
                const bool isolated =
                    backdrop_group || (opacity < 1 && (!n.children.empty() || (!n.text.empty() && painted) ||
                                                       (!shadows.empty() && (painted || !n.text.empty()))));
                if (!isolated) {
                    item.opacity = opacity;
                }
                // A group's composite takes the element's clips, which its items take anyway; anchored descendants
                // lie beyond them, so a group holding one composites over the root's box.
                const bool group_anchored = isolated && anchored_below(anchored_below, n);
                const auto group_clip = group_anchored ? view_clip : clip;
                const auto group_chain = group_anchored ? Chain{} : chain;
                if (isolated) {
                    if (++groups > constants::scene::max_opacity_group_depth) {
                        throw std::invalid_argument("opacity depth bound");
                    }
                    paint::Item g;
                    g.kind = paint::Item::Kind::BeginGroup;
                    g.opacity = opacity;
                    g.clip = group_clip;
                    g.clips_begin = group_chain.begin;
                    g.clips_count = group_chain.count;
                    list.items.push_back(g);
                }
                if (backdrop && !backdrop_group) {
                    add_backdrop(false);
                }
                // Outer shadows lie under the box, bottom first; inset ones over its background and under its border.
                const auto add_shadows = [&](bool inset) {
                    for (const auto& [shadow, rect] : shadows) {
                        if (shadow.inset != inset) {
                            continue;
                        }
                        auto s = item;
                        s.kind = paint::Item::Kind::Shadow;
                        s.transform = multiply(item.transform, {1, 0, 0, 1, rect[0], rect[1]});
                        s.width = rect[2];
                        s.height = rect[3];
                        s.payload = std::uint32_t(list.shadows.size());
                        list.shadows.push_back(shadow);
                        list.items.push_back(s);
                    }
                };
                const bool inset = std::any_of(shadows.begin(), shadows.end(), [](const auto& s) {
                    return s.shadow.inset;
                });
                add_shadows(false);
                const auto add_box = [&](const paint::Box& value) {
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Box;
                    shape.payload = std::uint32_t(list.boxes.size());
                    list.boxes.push_back(value);
                    list.items.push_back(shape);
                };
                // The background under the borders, in background-clip's box (the background painting area): the
                // border box with its corners, or a padding or content box of its own inside its inner corners (a
                // rounded clip).
                const auto clip_box = scene_values::view(n.style, PropertyId::BackgroundClip, "border-box");
                const auto add_under = [&](paint::Box under) {
                    under.widths = {};
                    if (clip_box == "border-box" || clip_box.empty()) {
                        add_box(under);
                        return;
                    }
                    const auto area = scene_background_box(
                        n.style, clip_box == "content-box" ? BackgroundBox::Content : BackgroundBox::Padding,
                        item.width, item.height);
                    const auto& r = area.rect;
                    if (r[2] <= 0 || r[3] <= 0) {
                        return;
                    }
                    under.radii = {};
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Box;
                    shape.transform = multiply(m, {1, 0, 0, 1, r[0], r[1]});
                    shape.width = r[2];
                    shape.height = r[3];
                    if (std::any_of(area.radius_x.begin(), area.radius_x.end(), [](float c) {
                            return c > 0;
                        })) {
                        const auto corners = axis_aligned(m) ? add_rounded(chain, m, point(m, r[0], r[1]),
                                                                           point(m, r[0] + r[2], r[1] + r[3]),
                                                                           area.radius_x, area.radius_y)
                                                             : add_local(chain, m, r, area.radius_x, area.radius_y);
                        shape.clips_begin = corners.begin;
                        shape.clips_count = corners.count;
                    }
                    shape.payload = std::uint32_t(list.boxes.size());
                    list.boxes.push_back(under);
                    list.items.push_back(shape);
                };
                // Dotted and dashed sides, laid out on the pass's device pixels, before the solid sides as Chrome
                // paints them.
                const auto add_dashes = [&] {
                    const std::array<double, 4> device =
                        axis_aligned(m) ? std::array<double, 4>{double(m[0]) * scale, double(m[3]) * scale,
                                                                double(m[4]) * scale, double(m[5]) * scale}
                                        : std::array<double, 4>{scale, scale, 0, 0};
                    for (unsigned side = 0; side < 4; ++side) {
                        for (const auto& dash : scene_dashed_side({item.width, item.height}, side, dashed_widths[side],
                                                                  styles[side], device)) {
                            paint::Box value;
                            value.background = dash_colors[side];
                            if (dash.round) {
                                value.radii.fill(dash.rect[2] / 2);
                            }
                            auto shape = item;
                            shape.kind = paint::Item::Kind::Box;
                            shape.transform = multiply(item.transform, {1, 0, 0, 1, dash.rect[0], dash.rect[1]});
                            shape.width = dash.rect[2];
                            shape.height = dash.rect[3];
                            shape.payload = std::uint32_t(list.boxes.size());
                            list.boxes.push_back(value);
                            list.items.push_back(shape);
                        }
                    }
                };
                const bool clipped = clip_box != "border-box" && !clip_box.empty() && box.background[3] > 0;
                if (painted && !n.effect && !fitted && !dashed && !inset && !clipped) {
                    add_box(box);
                } else if (painted && !n.effect && !fitted) {
                    if (box.background[3] > 0 || box.gradient.kind != paint::GradientKind::None) {
                        add_under(box);
                    }
                    add_shadows(true);
                    add_dashes();
                    if (bordered) {
                        auto over = box;
                        over.background = {};
                        over.gradient = {};
                        add_box(over);
                    }
                } else if (inset && !n.effect && !fitted) {
                    // Nothing else painted: the inset shadows alone.
                    add_shadows(true);
                } else if (n.effect && !bare) {
                    // As CSS paints a background image: over the background color and under the borders, inside the
                    // border box's corners. The effect's time is the frame's.
                    if (box.background[3] > 0) {
                        add_under(box);
                    }
                    paint::Effect effect;
                    effect.program = index_of(list.programs, n.effect->program);
                    effect.time = float(f.time_ms / constants::milliseconds_per_second);
                    effect.parameters = n.effect->parameters;
                    for (const auto& texture : n.effect->textures) {
                        effect.textures.push_back(index_of(list.assets, texture));
                    }
                    effect.radii = box.radii;
                    if (n.effect->program->backdrop) {
                        effect.backdrop_at = first_item;
                    }
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Effect;
                    // As Chrome draws an image (or a canvas's content): into the rect snapped to device pixels, each
                    // edge to the nearest, unless the box rotates, skews or mirrors.
                    if (axis_aligned(m) && m[0] > 0 && m[3] > 0) {
                        const auto snap = [&](float px) {
                            return std::round(px * scale) / scale;
                        };
                        const float left = snap(m[4]);
                        const float top = snap(m[5]);
                        shape.transform = {m[0], 0, 0, m[3], left, top};
                        shape.width = (snap(m[4] + m[0] * item.width) - left) / m[0];
                        shape.height = (snap(m[5] + m[3] * item.height) - top) / m[3];
                    }
                    shape.payload = std::uint32_t(list.effects.size());
                    list.effects.push_back(std::move(effect));
                    list.items.push_back(shape);
                    add_shadows(true);
                    add_dashes();
                    if (bordered) {
                        auto over = box;
                        over.background = {};
                        add_box(over);
                    }
                } else if (fitted) {
                    // A host texture or an image, as CSS paints a background image: over the background color and
                    // under the borders, over background-clip's box inside its corners (a rounded clip), laid out in
                    // background-origin's box as the fit says (scene_background.hpp), the box snapped to device
                    // pixels as Chrome draws an image, unless it rotates, skews or mirrors.
                    if (box.background[3] > 0) {
                        add_under(box);
                    }
                    const auto layout = scene_background_layout(n.style);
                    const auto painting = scene_background_box(n.style, layout.clip, item.width, item.height);
                    paint::Image image;
                    if (n.background_image) {
                        image.asset = index_of(list.assets, n.background_image);
                        image.fitted = true;
                    } else {
                        image.texture = index_of(list.textures, host_texture) + 1;
                    }
                    image.fit = scene_texture_fit(
                        layout, painting, scene_background_box(n.style, layout.origin, item.width, item.height));
                    const auto& r = painting.rect;
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Image;
                    shape.transform = multiply(m, {1, 0, 0, 1, r[0], r[1]});
                    shape.width = r[2];
                    shape.height = r[3];
                    if (std::any_of(painting.radius_x.begin(), painting.radius_x.end(), [](float c) {
                            return c > 0;
                        })) {
                        const auto corners =
                            axis_aligned(m)
                                ? add_rounded(chain, m, point(m, r[0], r[1]), point(m, r[0] + r[2], r[1] + r[3]),
                                              painting.radius_x, painting.radius_y)
                                : add_local(chain, m, r, painting.radius_x, painting.radius_y);
                        shape.clips_begin = corners.begin;
                        shape.clips_count = corners.count;
                    }
                    if (axis_aligned(m) && m[0] > 0 && m[3] > 0) {
                        const auto snap = [&](float px) {
                            return std::round(px * scale) / scale;
                        };
                        const float left = snap(m[4] + m[0] * r[0]);
                        const float top = snap(m[5] + m[3] * r[1]);
                        shape.transform = {m[0], 0, 0, m[3], left, top};
                        shape.width = (snap(m[4] + m[0] * (r[0] + r[2])) - left) / m[0];
                        shape.height = (snap(m[5] + m[3] * (r[1] + r[3])) - top) / m[3];
                    }
                    if (shape.width > 0 && shape.height > 0) {
                        shape.payload = std::uint32_t(list.images.size());
                        list.images.push_back(image);
                        list.items.push_back(shape);
                    }
                    add_shadows(true);
                    add_dashes();
                    if (bordered) {
                        auto over = box;
                        over.background = {};
                        over.gradient = {};
                        add_box(over);
                    }
                }
                // The border image's pieces over the background and inset shadows, where the border styles would be:
                // each a slice of the image (its uv) stretched over its place, laid out as a fitted image of one tile
                // the piece's size and snapped to device pixels as an image is (neighbouring pieces share their edges).
                if (!border_pieces.empty()) {
                    const float iw = float(n.border_image->width);
                    const float ih = float(n.border_image->height);
                    const auto asset = index_of(list.assets, n.border_image);
                    const bool snapped = axis_aligned(m) && m[0] > 0 && m[3] > 0;
                    const auto snap = [&](float px) {
                        return std::round(px * scale) / scale;
                    };
                    for (const auto& piece : border_pieces) {
                        auto shape = item;
                        shape.kind = paint::Item::Kind::Image;
                        shape.transform = multiply(m, {1, 0, 0, 1, piece.target[0], piece.target[1]});
                        shape.width = piece.target[2];
                        shape.height = piece.target[3];
                        if (snapped) {
                            const float left = snap(m[4] + m[0] * piece.target[0]);
                            const float top = snap(m[5] + m[3] * piece.target[1]);
                            shape.transform = {m[0], 0, 0, m[3], left, top};
                            shape.width = (snap(m[4] + m[0] * (piece.target[0] + piece.target[2])) - left) / m[0];
                            shape.height = (snap(m[5] + m[3] * (piece.target[1] + piece.target[3])) - top) / m[3];
                        }
                        if (shape.width <= 0 || shape.height <= 0) {
                            continue;
                        }
                        paint::Image image;
                        image.asset = asset;
                        image.fitted = true;
                        image.uv = {piece.source[0] / iw, piece.source[1] / ih, piece.source[2] / iw,
                                    piece.source[3] / ih};
                        image.fit.area = {0, 0, shape.width, shape.height};
                        image.fit.scale = paint::TextureFit::Scale::Lengths;
                        image.fit.size = {shape.width, shape.height};
                        image.fit.repeat = {paint::TextureFit::Repeat::NoRepeat, paint::TextureFit::Repeat::NoRepeat};
                        shape.payload = std::uint32_t(list.images.size());
                        list.images.push_back(image);
                        list.items.push_back(shape);
                    }
                }
                // The element's particles over its background and borders, under its content, unclipped by its own
                // overflow; the emitter's area in the box's px, a sprite an asset of the list.
                static const std::remove_cvref_t<decltype(n.particles)> no_particles;
                for (const auto& emission : bare ? no_particles : n.particles) {
                    paint::Particles particles{emission.emitter->emitter, emission.emission};
                    particles.emitter.area = scene_emitter_area(particles.emitter, item.width, item.height);
                    particles.emitter.sprite.reset();
                    if (emission.emitter->sprite) {
                        particles.emitter.sprite = index_of(list.assets, emission.emitter->sprite);
                    }
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Particles;
                    shape.payload = std::uint32_t(list.particles.size());
                    list.particles.push_back(std::move(particles));
                    list.items.push_back(shape);
                }
                if (svg_root) {
                    // An svg viewport clips to the rounded border box, as the reference raster clips it; under a
                    // rotation or skew, in the box's own space.
                    const bool rounded = std::any_of(box.radii.begin(), box.radii.end(), [](float r) {
                        return r > 0;
                    });
                    if (axis_aligned(m)) {
                        const auto a = point(m, 0, 0);
                        const auto b = point(m, item.width, item.height);
                        clip = intersect(clip, a, b);
                        if (rounded) {
                            chain = add_rounded(chain, m, a, b, box.radii, box.radii);
                        }
                    } else {
                        clip = bound(clip, m, {0, 0, item.width, item.height});
                        chain = add_local(chain, m, {0, 0, item.width, item.height}, box.radii, box.radii);
                    }
                } else if (get(n.style, PropertyId::Overflow) == "hidden" ||
                           get(n.style, PropertyId::Overflow) == "auto") {
                    // Content clips to the padding box with its inner corners; under a rotation or skew, in the box's
                    // own space, and the target-space clip keeps only its bounding rectangle.
                    const auto padding = scene_values::padding_box(n.style, item.width, item.height);
                    if (axis_aligned(m)) {
                        const auto a = point(m, padding.rect[0], padding.rect[1]);
                        const auto b = point(m, padding.rect[0] + padding.rect[2], padding.rect[1] + padding.rect[3]);
                        clip = intersect(clip, a, b);
                        if (scene_values::is_rounded(padding)) {
                            chain = add_rounded(chain, m, a, b, padding.radius_x, padding.radius_y);
                        }
                    } else {
                        clip = bound(clip, m, padding.rect);
                        chain = add_local(chain, m, padding.rect, padding.radius_x, padding.radius_y);
                    }
                }
                // Text a line at a time, each a single-line text node over its line box. Text without lines (a frame
                // that no solver laid out) is one line in the node's box.
                // Its text shadows, bottom first, under each line of its text.
                const auto text_shadows = scene_values::text_shadows(n.style);
                const auto add_text = [&](std::shared_ptr<SceneNode> source, const Matrix& at, float width,
                                          float height, std::uint32_t part) {
                    auto t = item;
                    t.kind = paint::Item::Kind::Text;
                    t.transform = at;
                    t.width = width;
                    t.height = height;
                    t.clip = clip;
                    t.clips_begin = chain.begin;
                    t.clips_count = chain.count;
                    source->box[0] = source->box[1] = 0;
                    paint::Raster raster;
                    raster.part = part;
                    const auto style = source->style.text(PropertyId::FontStyle, "normal");
                    if (style == "italic" || style.starts_with("oblique")) {
                        raster.overhang =
                            std::ceil(len(get(source->style, PropertyId::FontSize, constants::default_font_size)) *
                                      constants::scene::italic_overhang_em);
                    }
                    raster.mask = std::all_of(source->text.begin(), source->text.end(), [](unsigned char c) {
                        return c >= constants::scene::printable_ascii_first &&
                               c < constants::scene::printable_ascii_end;
                    });
                    if (raster.mask) {
                        raster.tint =
                            paint_color(constants::scene::paint_slot_color, "color", constants::default_color);
                        source->style[PropertyId::Color] = "#ffffff";
                        source->paint.reset();
                    }
                    raster.source = std::move(source);
                    // Each shadow: the line's coverage (white text: color glyphs keep only their alpha) offset,
                    // blurred and tinted, with room around it for the blur.
                    std::shared_ptr<const SceneNode> white = raster.mask ? raster.source : nullptr;
                    for (const auto& layer : text_shadows) {
                        if (!white) {
                            auto copy = std::make_shared<SceneNode>(*raster.source);
                            copy->style[PropertyId::Color] = "#ffffff";
                            copy->paint.reset();
                            white = std::move(copy);
                        }
                        paint::Raster shadow;
                        shadow.source = white;
                        shadow.mask = true;
                        shadow.tint = layer.color;
                        shadow.part = part;
                        shadow.blur = scene_values::shadow_sigma(layer.blur);
                        shadow.overhang =
                            raster.overhang + std::ceil(constants::scene::shadow_kernel_sigmas * shadow.blur);
                        auto s = t;
                        s.transform = multiply(at, {1, 0, 0, 1, layer.x, layer.y});
                        s.payload = std::uint32_t(list.rasters.size());
                        list.rasters.push_back(std::move(shadow));
                        list.items.push_back(s);
                    }
                    t.payload = std::uint32_t(list.rasters.size());
                    list.rasters.push_back(std::move(raster));
                    list.items.push_back(t);
                };
                // text-decoration (scene_values::text_decoration_bands): underlines and overlines under each line's
                // text, line-throughs over it, clipped as the text is.
                const auto decoration_text = get(n.style, PropertyId::TextDecorationColor, "currentcolor");
                const auto decoration =
                    decoration_text == "currentcolor" || decoration_text == "currentColor"
                        ? paint_color(constants::scene::paint_slot_color, "color", constants::default_color)
                        : color(decoration_text, n.style);
                const auto add_band = [&](const Matrix& at, const std::array<float, 4>& band) {
                    paint::Box value;
                    value.background = decoration;
                    auto shape = item;
                    shape.kind = paint::Item::Kind::Box;
                    shape.transform = multiply(at, {1, 0, 0, 1, band[0], band[1]});
                    shape.width = band[2];
                    shape.height = band[3];
                    shape.clip = clip;
                    shape.clips_begin = chain.begin;
                    shape.clips_count = chain.count;
                    shape.payload = std::uint32_t(list.boxes.size());
                    list.boxes.push_back(value);
                    list.items.push_back(shape);
                };
                for (std::size_t line = 0; !invisible && line < n.lines.size(); ++line) {
                    // A line that text-overflow hides draws nothing.
                    if (n.lines[line].text.empty()) {
                        continue;
                    }
                    auto source = std::make_shared<SceneNode>(scene_text_line(n, line));
                    const auto at = source->box;
                    // The bands lie on the line's rect, the text image on its whole px around it.
                    const auto& r = n.lines[line].rect;
                    const auto lined = multiply(item.transform, {1, 0, 0, 1, r[0], r[1]});
                    const auto bands = scene_values::text_decoration_bands(n.style, r[2], n.lines[line].baseline);
                    for (const auto& band : bands.under) {
                        add_band(lined, band);
                    }
                    add_text(std::move(source), multiply(item.transform, {1, 0, 0, 1, at[0], at[1]}), at[2], at[3],
                             std::uint32_t(line));
                    for (const auto& band : bands.over) {
                        add_band(lined, band);
                    }
                }
                if (!invisible && n.lines.empty() && !n.text.empty()) {
                    auto source = own(n);
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
                    if (n.paint) {
                        source->paint.emplace();
                        source->paint->values[constants::scene::paint_slot_color] =
                            n.paint->values[constants::scene::paint_slot_color];
                    }
                    add_text(std::move(source), item.transform, item.width, item.height, 0);
                }
                if (svg_root) {
                    const auto v = scene_svg_viewport(n);
                    const auto user = multiply(item.transform, {v.sx, 0, 0, v.sy, v.tx, v.ty});
                    // Its drop shadow under the paths it paints, its box room for the offset and the blur; what it
                    // shadows lies in its viewport, offset.
                    const auto shadowed = list.items.size();
                    if (dropped) {
                        if (painted || !shadows.empty()) {
                            throw std::invalid_argument("drop-shadow of an svg element with a box");
                        }
                        const float e = std::ceil(constants::scene::shadow_kernel_sigmas * dropped->sigma);
                        auto s = item;
                        s.kind = paint::Item::Kind::PathShadow;
                        s.transform = multiply(item.transform, {1, 0, 0, 1, dropped->x - e, dropped->y - e});
                        s.width = item.width + 2 * e;
                        s.height = item.height + 2 * e;
                        s.payload = std::uint32_t(list.path_shadows.size());
                        paint::PathShadow value;
                        value.color = dropped->color;
                        value.x = dropped->x;
                        value.y = dropped->y;
                        value.blur = dropped->sigma;
                        value.content = {e, e, item.width, item.height};
                        list.path_shadows.push_back(value);
                        list.items.push_back(s);
                    }
                    for (const auto& c : n.children) {
                        svg(svg, c, user, clip, chain, n);
                    }
                    if (dropped) {
                        auto& value = list.path_shadows[list.items[shadowed].payload];
                        value.first = std::uint32_t(shadowed + 1);
                        value.count = std::uint32_t(list.items.size() - shadowed - 1);
                        if (!std::all_of(list.items.begin() + std::ptrdiff_t(value.first), list.items.end(),
                                         [&](const paint::Item& p) {
                                             if (p.kind == paint::Item::Kind::Path) {
                                                 return true;
                                             }
                                             if (p.kind != paint::Item::Kind::Box) {
                                                 return false;
                                             }
                                             const auto& b = list.boxes[p.payload];
                                             return b.gradient.kind == paint::GradientKind::None &&
                                                    std::all_of(b.widths.begin(), b.widths.end(),
                                                                [&](float w) {
                                                                    return w == b.widths[0];
                                                                }) &&
                                                    std::all_of(b.edges.begin(), b.edges.end(),
                                                                [&](const paint::Color& c) {
                                                                    return c == b.edges[0];
                                                                });
                                         })) {
                            throw std::invalid_argument("drop-shadow of svg content other than shapes");
                        }
                    }
                }
                // In order-modified document order, stably by z-index.
                auto order = svg_root ? std::vector<std::size_t>{} : scene_child_order(n);
                const auto z = [&](std::size_t i) {
                    auto v = get(n.children[i].style, PropertyId::ZIndex, "0");
                    return v == "auto" ? 0 : number(v);
                };
                std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                    return z(a) < z(b);
                });
                if (std::any_of(n.children.begin(), n.children.end(), [](const SceneNode& c) {
                        return bool(c.anchoring);
                    })) {
                    scene_anchor_order(order, z, [&](std::size_t i) -> std::optional<float> {
                        const auto& c = n.children[i];
                        const auto* a = c.anchoring ? scene_anchor_resolve(anchors, c.identity, *c.anchoring) : nullptr;
                        return a ? std::optional<float>(a->depth) : std::nullopt;
                    });
                }
                // Deferred: each child's items and its anchored element (anchor_of), in drawing order.
                std::vector<SceneListRun::Member> members;
                for (auto i : order) {
                    const auto first = std::uint32_t(list.items.size());
                    const auto before = out.anchors.size();
                    self(self, n.children[i], item.transform, clip, chain, true);
                    if (anchors.deferred) {
                        members.push_back({first, std::uint32_t(list.items.size()), anchor_of(before, n.children[i]),
                                           std::uint32_t(i)});
                    }
                }
                // Runs of one z-index with two anchored members or more, which the pass orders each frame.
                for (std::size_t begin = 0; begin < members.size();) {
                    auto end = begin + 1;
                    while (end < members.size() && z(order[end]) == z(order[begin])) {
                        ++end;
                    }
                    if (std::count_if(members.begin() + std::ptrdiff_t(begin), members.begin() + std::ptrdiff_t(end),
                                      [](const SceneListRun::Member& m) {
                                          return m.anchor != 0;
                                      }) > 1) {
                        auto& run = out.runs.emplace_back();
                        run.members.assign(members.begin() + std::ptrdiff_t(begin),
                                           members.begin() + std::ptrdiff_t(end));
                    }
                    begin = end;
                }
                // outline: a ring outside the border box (scene_values::outline) over the element's content, in its
                // group but outside its own overflow clip (the item's clips are its parent's).
                if (!invisible) {
                    if (const auto ring = scene_values::outline(n.style, item.width, item.height)) {
                        paint::Box value;
                        value.widths.fill(ring->width);
                        value.edges.fill(color(get(n.style, PropertyId::OutlineColor, "currentcolor"), n.style));
                        value.radii = ring->radii;
                        auto shape = item;
                        shape.kind = paint::Item::Kind::Box;
                        shape.transform = multiply(item.transform, {1, 0, 0, 1, ring->rect[0], ring->rect[1]});
                        shape.width = ring->rect[2];
                        shape.height = ring->rect[3];
                        shape.payload = std::uint32_t(list.boxes.size());
                        list.boxes.push_back(value);
                        list.items.push_back(shape);
                    }
                }
                if (backdrop_group) {
                    add_backdrop(true);
                }
                if (isolated) {
                    paint::Item g;
                    g.kind = paint::Item::Kind::EndGroup;
                    g.opacity = opacity;
                    g.clip = group_clip;
                    g.clips_begin = group_chain.begin;
                    g.clips_count = group_chain.count;
                    list.items.push_back(g);
                    --groups;
                }
            };
            if (build.part) {
                // The part in its element's context: the rounded clips it inherits first.
                const auto& span = *build.span;
                list.clips.assign(build.whole->clips.begin() + span.chain_begin,
                                  build.whole->clips.begin() + span.chain_begin + span.chain_count);
                groups = span.groups;
                visit(visit, *build.part, span.parent, span.clip, Chain{0, span.chain_count}, true);
            } else {
                visit(visit, f.root, {1, 0, 0, 1, 0, 0},
                      build.unclipped ? unclipped : std::array<float, 4>{0, 0, f.root.box[2], f.root.box[3]}, Chain{},
                      true);
            }
            if (list.items.size() > constants::scene::max_display_list_items) {
                throw std::invalid_argument("native command bound");
            }
            scene_anchor_sort(out.placed);
            if (anchors.deferred) {
                (void)anchor_of(0, build.part ? *build.part : f.root);
                // Items take the anchor of the innermost anchored element around them (outer ones come first). A
                // group composites over the root's box, unmoved: its items hold the clips. Backdrops, particles,
                // planes and effects place themselves on the target, so their anchors cannot wait.
                out.deferred = true;
                // A part's items count anchors as the whole list does; the part's items outside its own anchored
                // elements lie in the enclosing one (none at 0), whose groups composite over the root's box.
                const auto first_anchor = build.part ? build.span->begin[SceneListSpan::Anchors] : 0u;
                if (build.part && build.enclosing) {
                    for (auto& item : list.items) {
                        using Kind = paint::Item::Kind;
                        if (item.kind == Kind::BeginGroup || item.kind == Kind::EndGroup) {
                            item.clip = view_clip;
                            item.clips_begin = item.clips_count = 0;
                            continue;
                        }
                        if (item.kind == Kind::Backdrop || item.kind == Kind::Particles || item.kind == Kind::Plane ||
                            item.kind == Kind::Effect) {
                            out.deferred = false;
                        }
                        item.anchor = build.enclosing;
                    }
                }
                for (std::size_t k = 0; k < out.anchors.size(); ++k) {
                    for (auto i = out.anchors[k].first; i < out.anchors[k].end; ++i) {
                        auto& item = list.items[i];
                        using Kind = paint::Item::Kind;
                        if (item.kind == Kind::BeginGroup || item.kind == Kind::EndGroup) {
                            item.clip = view_clip;
                            item.clips_begin = item.clips_count = 0;
                            continue;
                        }
                        if (item.kind == Kind::Backdrop || item.kind == Kind::Particles || item.kind == Kind::Plane ||
                            item.kind == Kind::Effect) {
                            out.deferred = false;
                        }
                        item.anchor = std::uint32_t(first_anchor + k + 1);
                    }
                }
                if (!out.deferred) {
                    list = {};
                    out.anchors.clear();
                    out.runs.clear();
                }
            }
        } catch (const std::invalid_argument& e) {
            list = {};
            out.placed.clear();
            out.fallback = e.what();
        } catch (const std::out_of_range& e) {
            list = {};
            out.placed.clear();
            out.fallback = e.what();
        }
        return out;
    }

    SceneFilterContent scene_filter_content(const SceneNode& element, double time_ms, SceneText* text, float scale,
                                            SceneDisplayCache* cache, const SceneFilterResolve* filters) {
        SceneFilterContent out;
        // Anchored descendants move on their own and particles fly past any bounds: neither lies in an image.
        const auto refused = [](auto&& self, const SceneNode& n, bool root) -> const char* {
            if (!root && n.anchoring) {
                return "anchored element in a filtered element";
            }
            if (!n.particles.empty()) {
                return "particles in a filtered element";
            }
            for (const auto& child : n.children) {
                if (const auto* why = self(self, child, false)) {
                    return why;
                }
            }
            return nullptr;
        };
        if (const auto* why = refused(refused, element, true)) {
            out.listed.fallback = why;
            return out;
        }
        if (filter_nesting >= constants::max_filter_depth) {
            out.listed.fallback = "filter nesting bound";
            return out;
        }
        auto source = std::make_shared<SceneNode>(element);
        source->box[0] = source->box[1] = 0;
        source->anchoring.reset();
        for (const auto id : {PropertyId::Transform, PropertyId::Opacity, PropertyId::Filter}) {
            source->style.erase(id);
        }
        // Its computed values are authoritative: the authored style holds the transform and opacity too.
        source->attributes.erase("style");
        if (source->paint) {
            source->paint->values[constants::scene::paint_slot_transform].reset();
            source->paint->values[constants::scene::paint_slot_opacity].reset();
        }
        SceneFrame frame;
        frame.root = *source;
        frame.time_ms = time_ms;

        struct Nesting {
            Nesting() {
                ++filter_nesting;
            }

            ~Nesting() {
                --filter_nesting;
            }
        } guard;

        SceneListBuild build;
        build.unclipped = true;
        build.filters = filters;
        out.listed = scene_display_list(frame, {}, text, scale, cache, {}, build);
        out.source = std::move(source);
        return out;
    }

    namespace {
        // Where an item draws in its list's target px: the bounding box of its drawn rectangle (a text image's margins,
        // a shadow's blur and offset, a path's stroke, a plane's projected image) under its map; none for groups.
        std::optional<std::array<double, 4>> drawn(const paint::DisplayList& list, const paint::Item& item) {
            using Kind = paint::Item::Kind;
            std::array<double, 4> local{0, 0, item.width, item.height};
            std::vector<std::array<double, 2>> corners;
            switch (item.kind) {
                case Kind::BeginGroup:
                case Kind::EndGroup:
                    return std::nullopt;
                case Kind::Text:
                case Kind::Layer: {
                    const double m = constants::render::scene::image_margin + list.rasters[item.payload].overhang;
                    local = {-m, -m, item.width + m, item.height + m};
                    break;
                }
                case Kind::Shadow: {
                    const auto& s = list.shadows[item.payload];
                    if (!s.inset) {
                        const double e = constants::scene::shadow_kernel_sigmas * double(s.blur) / 2 +
                                         std::abs(double(s.spread)) + constants::scene::shadow_guard_px;
                        local = {s.x - e, s.y - e, item.width + s.x + e, item.height + s.y + e};
                    }
                    break;
                }
                case Kind::Path: {
                    const auto& p = list.paths[item.payload];
                    if (p.path->points.empty()) {
                        return std::nullopt;
                    }
                    local = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
                    for (const auto& [x, y] : p.path->points) {
                        local = {std::min(local[0], double(x)), std::min(local[1], double(y)),
                                 std::max(local[2], double(x)), std::max(local[3], double(y))};
                    }
                    // A stroke reaches half its width past the path, a miter join its limit times that, a square cap
                    // the half diagonal.
                    if (p.stroke) {
                        const double e = p.stroke->width / 2 * std::max<double>(p.stroke->miter_limit, 2);
                        local = {local[0] - e, local[1] - e, local[2] + e, local[3] + e};
                    }
                    break;
                }
                case Kind::Plane: {
                    // The content's image, image_margin px past the plane, through its homography.
                    const auto& plane = list.planes[item.payload];
                    const auto& h = plane.transform;
                    const double m = constants::render::scene::image_margin;
                    std::array<double, 4> out{
                        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
                    for (const auto& [x, y] :
                         std::array<std::array<double, 2>, 4>{{{-m, -m},
                                                               {plane.width + m, -m},
                                                               {-m, plane.height + m},
                                                               {plane.width + m, plane.height + m}}}) {
                        const double w = h[6] * x + h[7] * y + h[8];
                        if (!(w > constants::plane_min_depth)) {
                            return std::array<double, 4>{
                                -constants::scene::max_numeric_magnitude, -constants::scene::max_numeric_magnitude,
                                constants::scene::max_numeric_magnitude, constants::scene::max_numeric_magnitude};
                        }
                        const double px = (h[0] * x + h[1] * y + h[2]) / w;
                        const double py = (h[3] * x + h[4] * y + h[5]) / w;
                        out = {std::min(out[0], px), std::min(out[1], py), std::max(out[2], px), std::max(out[3], py)};
                    }
                    return out;
                }
                default:
                    break;
            }
            const auto& m = item.transform;
            std::array<double, 4> out{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity()};
            for (const auto& [x, y] : std::array<std::array<double, 2>, 4>{
                     {{local[0], local[1]}, {local[2], local[1]}, {local[0], local[3]}, {local[2], local[3]}}}) {
                const double px = m[0] * x + m[2] * y + m[4];
                const double py = m[1] * x + m[3] * y + m[5];
                out = {std::min(out[0], px), std::min(out[1], py), std::max(out[2], px), std::max(out[3], py)};
            }
            return out;
        }
    } // namespace

    SceneFilterRegion scene_filter_region(const paint::DisplayList& content, const std::vector<paint::FilterOp>& ops,
                                          float scale) {
        constexpr double inf = std::numeric_limits<double>::infinity();
        std::array<double, 4> ink{inf, inf, -inf, -inf};
        for (const auto& item : content.items) {
            const auto rect = drawn(content, item);
            if (!rect) {
                continue;
            }
            const std::array<double, 4> clipped{
                std::max((*rect)[0], double(item.clip[0])), std::max((*rect)[1], double(item.clip[1])),
                std::min((*rect)[2], double(item.clip[2])), std::min((*rect)[3], double(item.clip[3]))};
            if (clipped[2] <= clipped[0] || clipped[3] <= clipped[1]) {
                continue;
            }
            ink = {std::min(ink[0], clipped[0]), std::min(ink[1], clipped[1]), std::max(ink[2], clipped[2]),
                   std::max(ink[3], clipped[3])};
        }
        SceneFilterRegion out{{}, scale};
        if (!(ink[2] > ink[0]) || !(ink[3] > ink[1])) {
            return out;
        }
        // What each function spreads and offsets, in order: a drop shadow on every side, so that its offset copy reads
        // nothing past the image (its passes mirror addresses there).
        for (const auto& op : ops) {
            const double spread = constants::scene::filter_kernel_sigmas * double(op.blur);
            const double x = op.kind == paint::FilterOp::Kind::DropShadow ? std::abs(double(op.x)) : 0;
            const double y = op.kind == paint::FilterOp::Kind::DropShadow ? std::abs(double(op.y)) : 0;
            if (op.kind != paint::FilterOp::Kind::Matrix) {
                ink = {ink[0] - spread - x, ink[1] - spread - y, ink[2] + spread + x, ink[3] + spread + y};
            }
        }
        // At most filter_image_max_pixels device pixels, the guard included.
        constexpr double guard = constants::scene::filter_guard_px;
        double resolution = scale;
        for (int tries = 0; tries < constants::scene::shadow_scale_tries; ++tries) {
            const double w = std::ceil((ink[2] - ink[0]) * resolution) + 2 * guard + 1;
            const double h = std::ceil((ink[3] - ink[1]) * resolution) + 2 * guard + 1;
            if (w * h <= constants::scene::filter_image_max_pixels) {
                break;
            }
            resolution *=
                std::sqrt(constants::scene::filter_image_max_pixels / (w * h)) * constants::scene::filter_scale_margin;
        }
        const auto edge = [](double v, bool up) {
            const double d = up ? std::ceil(v) : std::floor(v);
            return std::int32_t(std::clamp(d, -double(1 << 30), double(1 << 30)));
        };
        out.resolution = float(resolution);
        out.pixels = {edge(ink[0] * resolution - guard, false), edge(ink[1] * resolution - guard, false),
                      edge(ink[2] * resolution + guard, true), edge(ink[3] * resolution + guard, true)};
        return out;
    }
} // namespace weft
