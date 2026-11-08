#include "scene/scene_mask.hpp"

#include "base/css_scalar.hpp"
#include "paint/rounded_clip.hpp"
#include "scene/scene_background.hpp"
#include "scene/scene_gradient.hpp"
#include "scene/scene_values.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace weft {
    namespace {
        [[noreturn]] void reject(const std::string& what) {
            throw std::invalid_argument(what + " outside scene profile");
        }

        std::vector<std::string> list(const StyleValues& style, PropertyId id, const char* initial) {
            auto items = css::split(style.text(id, initial), ',').parts;
            if (items.empty()) {
                items.emplace_back(initial);
            }
            return items;
        }

        std::vector<std::string> words(std::string_view s) {
            return css::split(s, ' ').parts;
        }

        // A box of the element as mask-origin and mask-clip name one: x, y, width, height in its px.
        std::array<float, 4> box(const StyleValues& style, const std::string& name, float w, float h,
                                 std::string_view property) {
            BackgroundBox which = BackgroundBox::Border;
            if (name == "padding-box") {
                which = BackgroundBox::Padding;
            } else if (name == "content-box" || name == "fill-box") {
                which = BackgroundBox::Content;
            } else if (name != "border-box" && name != "stroke-box" && name != "view-box") {
                reject(std::string(property) + " " + name);
            }
            return scene_background_box(style, which, w, h).rect;
        }

        // A tile's side: a length or percentage of the area's side, or auto (a gradient has no size of its own: the
        // area's).
        float side(const std::string& v, float area) {
            if (v == "auto") {
                return area;
            }
            const auto l = scene_length(v);
            if (!l) {
                reject("mask-size " + v);
            }
            return l->px + l->share * area;
        }
    } // namespace

    std::string scene_mask_value(const StyleValues& style) {
        auto v = css::trim(style.text(PropertyId::MaskImage, "none"));
        return v.empty() ? std::string("none") : v;
    }

    std::optional<std::string> scene_mask_url(const StyleValues& style) {
        for (const auto& image : list(style, PropertyId::MaskImage, "none")) {
            if (auto url = scene_values::image_url(image)) {
                return url;
            }
        }
        return std::nullopt;
    }

    std::optional<SceneMask> scene_mask(const StyleValues& style, float width, float height,
                                        const paint::Asset* image_asset) {
        const auto images = list(style, PropertyId::MaskImage, "none");
        if (std::all_of(images.begin(), images.end(), [](const std::string& i) {
                return i == "none";
            })) {
            return std::nullopt;
        }
        if (images.size() > max_clip_mask_layers) {
            reject("mask of more than " + std::to_string(max_clip_mask_layers) + " layers");
        }
        if (std::count_if(images.begin(), images.end(), [](const std::string& i) {
                return i.starts_with("url(");
            }) > 1) {
            reject("mask of more than one image layer");
        }
        const auto positions = list(style, PropertyId::MaskPosition, "0% 0%");
        const auto sizes = list(style, PropertyId::MaskSize, "auto");
        const auto repeats = list(style, PropertyId::MaskRepeat, "repeat");
        const auto origins = list(style, PropertyId::MaskOrigin, "border-box");
        const auto clips = list(style, PropertyId::MaskClip, "border-box");
        const auto composites = list(style, PropertyId::MaskComposite, "add");
        const auto modes = list(style, PropertyId::MaskMode, "match-source");
        SceneMask out;
        out.clips_hits = true;
        for (std::size_t i = 0; i < images.size(); ++i) {
            const auto at = [&](const std::vector<std::string>& values) {
                return values[i % values.size()];
            };
            SceneMaskLayer layer;
            const auto& image = images[i];
            // The painting area, outside which the layer is transparent.
            if (const auto c = at(clips); c == "no-clip") {
                out.clips_hits = false;
            } else {
                layer.clip = box(style, c, width, height, "mask-clip");
            }
            const auto op = at(composites);
            layer.composite = op == "add"         ? SceneMaskLayer::Composite::Add
                              : op == "subtract"  ? SceneMaskLayer::Composite::Subtract
                              : op == "intersect" ? SceneMaskLayer::Composite::Intersect
                              : op == "exclude"   ? SceneMaskLayer::Composite::Exclude
                                                  : (reject("mask-composite " + op), SceneMaskLayer::Composite::Add);
            const auto mode = at(modes);
            if (mode != "alpha" && mode != "luminance" && mode != "match-source") {
                reject("mask-mode " + mode);
            }
            // Its tile: mask-size over the positioning area (mask-origin's box), at mask-position, repeated as
            // mask-repeat says. An image has its natural size and ratio; a gradient has neither (it covers the area,
            // and fits it).
            const bool url = image.starts_with("url(");
            const bool natural = url && image_asset && image_asset->width && image_asset->height;
            const float iw = natural ? float(image_asset->width) : 0.f;
            const float ih = natural ? float(image_asset->height) : 0.f;
            const auto area = box(style, at(origins), width, height, "mask-origin");
            auto size = words(at(sizes));
            float tw = area[2];
            float th = area[3];
            if (size.size() == 1 && (size[0] == "cover" || size[0] == "contain")) {
                if (natural) {
                    const float k = size[0] == "cover" ? std::max(area[2] / iw, area[3] / ih)
                                                       : std::min(area[2] / iw, area[3] / ih);
                    tw = iw * k;
                    th = ih * k;
                }
            } else if (!size.empty() && size.size() <= 2) {
                const auto& sx = size[0];
                const auto& sy = size.size() > 1 ? size[1] : std::string("auto");
                if (natural && sx == "auto" && sy == "auto") {
                    tw = iw;
                    th = ih;
                } else if (natural && sy == "auto") {
                    tw = side(sx, area[2]);
                    th = tw * ih / iw;
                } else if (natural && sx == "auto") {
                    th = side(sy, area[3]);
                    tw = th * iw / ih;
                } else {
                    tw = side(sx, area[2]);
                    th = side(sy, area[3]);
                }
            } else {
                reject("mask-size " + at(sizes));
            }
            const auto repeat = words(at(repeats));
            const auto repeats_on = [&](const std::string& r) {
                if (r == "repeat") {
                    return true;
                }
                if (r == "no-repeat") {
                    return false;
                }
                reject("mask-repeat " + r);
            };
            if (repeat.size() == 1 && repeat[0] == "repeat-x") {
                layer.repeat = {true, false};
            } else if (repeat.size() == 1 && repeat[0] == "repeat-y") {
                layer.repeat = {false, true};
            } else if (repeat.size() == 1 || repeat.size() == 2) {
                layer.repeat = {repeats_on(repeat[0]), repeats_on(repeat.back())};
            } else {
                reject("mask-repeat " + at(repeats));
            }
            const auto position = scene_position(at(positions));
            const float tx = area[0] + position[0].px + position[0].share * (area[2] - tw);
            const float ty = area[1] + position[1].px + position[1].share * (area[3] - th);
            if (url) {
                // The element's mask image, or a transparent layer without one.
                layer.image = natural && tw > 0 && th > 0;
                layer.luminance = mode == "luminance";
                layer.tile = {tw, th, tx, ty};
            } else if (image != "none" && image.find("gradient(") == std::string::npos) {
                reject("mask-image " + image);
            } else if (image != "none" && tw > 0 && th > 0) {
                layer.gradient = scene_gradient_image(image, style, {tw, th, tx, ty});
                for (const auto& stop : layer.gradient->stops) {
                    const auto& c = stop.color;
                    layer.values.push_back(mode == "luminance" ? (.2126f * c[0] + .7152f * c[1] + .0722f * c[2]) * c[3]
                                                               : c[3]);
                }
            }
            out.layers.push_back(std::move(layer));
        }
        return out;
    }

    std::shared_ptr<const std::vector<std::array<float, 2>>> scene_mask_data(const SceneMask& mask) {
        std::vector<float> f;
        for (const auto& layer : mask.layers) {
            std::array<float, clip_mask_layer_floats> v{};
            if (layer.image) {
                // Kind 4, an image: whether it is luminance where a gradient says whether it repeats.
                v[0] = 4;
                v[1] = layer.luminance ? 1.f : 0.f;
                std::copy(layer.tile.begin(), layer.tile.end(), v.begin() + 4);
            } else if (const auto& g = layer.gradient) {
                v[0] = float(static_cast<int>(g->kind));
                v[1] = g->repeating ? 1.f : 0.f;
                v[2] = g->angle;
                v[3] = float(g->stops.size());
                std::copy(g->tile.begin(), g->tile.end(), v.begin() + 4);
                std::copy(g->shape.begin(), g->shape.end(), v.begin() + 8);
                for (std::size_t k = 0; k < g->stops.size() && k < 8; ++k) {
                    v[20 + k] = g->stops[k].offset;
                    v[28 + k] = layer.values[k];
                }
            }
            v[12] = layer.repeat[0] ? 1.f : 0.f;
            v[13] = layer.repeat[1] ? 1.f : 0.f;
            v[14] = float(static_cast<int>(layer.composite));
            if (layer.clip) {
                v[15] = 1;
                std::copy(layer.clip->begin(), layer.clip->end(), v.begin() + 16);
            }
            f.insert(f.end(), v.begin(), v.end());
        }
        auto out = std::make_shared<std::vector<std::array<float, 2>>>();
        for (std::size_t k = 0; k + 1 < f.size(); k += 2) {
            out->push_back({f[k], f[k + 1]});
        }
        return out;
    }
} // namespace weft
