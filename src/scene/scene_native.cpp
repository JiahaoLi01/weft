#include "scene/scene_native.hpp"

#include "constants/common.hpp"
#include "render/shadow_image.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_text_image.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace weft {
    namespace {
        namespace limits = constants::render::scene;
        using Kind = paint::Item::Kind;
    } // namespace

    std::size_t scene_scratch_bytes(const ScenePrepared& prepared, bool plane) {
        auto bytes = std::size_t(prepared.width) * prepared.height * constants::rgba8_bytes_per_pixel *
                     (prepared.pass.group_depth + (plane ? 1 : 0));
        for (const auto& nested : prepared.planes) {
            if (nested) {
                bytes += scene_scratch_bytes(*nested, true);
            }
        }
        return bytes;
    }

    ScenePreparer::ScenePreparer(SceneText& text, SceneNativeWork& work, std::uint32_t max_image_dimension,
                                 Reference reference)
        : text_(text),
          work_(work),
          max_image_dimension_(max_image_dimension),
          reference_(std::move(reference)),
          geometry_(text) {
    }

    std::unique_ptr<ScenePrepared> ScenePreparer::prepare(const SceneFrame& frame, float scale, unsigned width,
                                                          unsigned height, std::optional<std::array<float, 4>> crop,
                                                          unsigned view_index,
                                                          const std::vector<NodeIdentity>& layers) {
        const auto fallback = [&](const std::string& reason) {
            ++work_.fallbacks;
            ++work_.reasons[reason];
            return nullptr;
        };
        auto compiled = scene_display_list(frame, layers, &text_);
        if (!compiled.fallback.empty()) {
            return fallback(compiled.fallback);
        }
        if (const auto valid = paint::check(compiled.list, limits::max_group_depth); !valid) {
            return fallback(valid.error().message);
        }
        try {
            return prepare(compiled.list, scale, width, height, crop, view_index);
        } catch (const std::invalid_argument& e) {
            return fallback(e.what());
        }
    }

    ScenePreparer::Built ScenePreparer::image(const paint::Item& item, const paint::Raster& raster, float scale,
                                              unsigned view_index) {
        const auto& n = *raster.source;
        std::array<std::uint64_t, 4> id{view_index, n.identity.view_epoch, n.identity.slot, n.identity.generation};
        auto it = images_.find(id);
        if (it != images_.end() && scene_paint_source_equal(it->second.key, n) && it->second.scale == scale) {
            if (item.kind == Kind::Layer) {
                ++work_.layer_reuses;
            } else {
                ++work_.text_reuses;
            }
            return {it->second.text, it->second.layer};
        }
        if (item.kind == Kind::Layer) {
            bool supported = true;
            auto inspect = [&](auto&& self, const SceneNode& node) -> void {
                if (!node.space.empty() && node.space != constants::xhtml_namespace_uri) {
                    supported = false;
                }
                const auto* t = node.style.find(PropertyId::Transform);
                if (t && t->text().find("perspective(") != std::string::npos) {
                    supported = false;
                }
                if (!node.text.empty()) {
                    auto m = text_.measure(node.text, node.style);
                    if (m.width > node.box[2] + limits::text_fit_tolerance ||
                        m.height > node.box[3] + limits::text_fit_tolerance) {
                        supported = false;
                    }
                }
                for (const auto& child : node.children) {
                    self(self, child);
                }
            };
            inspect(inspect, n);
            const auto* overflow = n.style.find(PropertyId::Overflow);
            if (!overflow || overflow->text() != "hidden") {
                SceneFrame bounds;
                bounds.root = n;
                for (const auto& e : geometry_.geometry(bounds)) {
                    if (e.rect[0] < -limits::layer_bounds_tolerance || e.rect[1] < -limits::layer_bounds_tolerance ||
                        e.rect[0] + e.rect[2] > n.box[2] + limits::layer_bounds_tolerance ||
                        e.rect[1] + e.rect[3] > n.box[3] + limits::layer_bounds_tolerance) {
                        supported = false;
                    }
                }
            }
            if (!supported) {
                throw std::invalid_argument("explicit native layer bounds reference");
            }
        }
        auto metrics = text_.measure(n.text, n.style);
        const auto* overflow = n.style.find(PropertyId::Overflow);
        if (item.kind == Kind::Text && (!overflow || overflow->text() != "hidden") &&
            (metrics.width > n.box[2] + limits::text_fit_tolerance ||
             metrics.height > n.box[3] + limits::text_fit_tolerance)) {
            throw std::invalid_argument("text overflow reference");
        }
        // The node image_margin px in from the edges of a transparent frame of its size and margins: an explicit
        // layer's content, which a backend draws into the layer's image, and what the reference raster draws of text
        // whose color glyphs are images. Other text is a portable image.
        const auto framed = [&] {
            SceneFrame frame;
            frame.root.tag = "div";
            frame.root.identity = {UINT32_MAX, 1, n.identity.view_epoch};
            frame.root.box = {0, 0, n.box[2] + limits::image_margins, n.box[3] + limits::image_margins};
            frame.root.style = {{"background-color", "transparent"}};
            auto child = n;
            child.box[0] = child.box[1] = limits::image_margin;
            frame.root.children.push_back(std::move(child));
            return frame;
        };
        Entry entry;
        entry.key = n;
        entry.scale = scale;
        if (item.kind == Kind::Layer) {
            const auto frame = framed();
            const auto w = unsigned(std::ceil(frame.root.box[2] * scale));
            const auto h = unsigned(std::ceil(frame.root.box[3] * scale));
            entry.bytes = std::size_t(w) * h * constants::rgba8_bytes_per_pixel;
            if (w > max_image_dimension_ || h > max_image_dimension_ ||
                entry.bytes > limits::image_cache_budget_bytes) {
                throw std::invalid_argument("layer image byte bound");
            }
            const auto listed = scene_display_list(frame, {}, &text_);
            if (!listed.fallback.empty()) {
                throw std::invalid_argument(listed.fallback);
            }
            if (const auto valid = paint::check(listed.list, limits::max_group_depth); !valid) {
                throw std::invalid_argument(valid.error().message);
            }
            auto layer = std::make_shared<SceneLayerImage>();
            layer->id = ++next_id_;
            layer->width = w;
            layer->height = h;
            layer->content = prepare(listed.list, scale, w, h, std::nullopt, view_index);
            entry.layer = std::move(layer);
            ++work_.layer_builds;
        } else {
            SceneBitmap bitmap;
            auto text = scene_text_image(text_, n, scale, limits::image_margin);
            if (!text.color_glyphs) {
                bitmap = std::move(text.bitmap);
                ++work_.text_portable_builds;
            } else if (reference_) {
                bitmap = reference_(framed(), scale);
                ++work_.reference_builds;
            } else {
                throw std::invalid_argument("color glyph image reference");
            }
            entry.bytes = bitmap.rgba.size();
            if (entry.bytes > limits::image_cache_budget_bytes) {
                throw std::invalid_argument("text source byte bound");
            }
            // Straight RGBA to premultiplied BGRA.
            for (std::size_t i = 0; i < bitmap.rgba.size(); i += 4) {
                std::swap(bitmap.rgba[i], bitmap.rgba[i + 2]);
                for (unsigned c = 0; c < 3; ++c) {
                    bitmap.rgba[i + c] = std::uint8_t(
                        (unsigned(bitmap.rgba[i + c]) * bitmap.rgba[i + 3] + constants::color_channel_max / 2) /
                        constants::color_channel_max);
                }
            }
            auto image = std::make_shared<SceneImage>();
            image->id = ++next_id_;
            image->width = bitmap.width;
            image->height = bitmap.height;
            image->bgra = std::move(bitmap.rgba);
            entry.text = std::move(image);
            ++work_.text_builds;
        }
        const auto bytes = entry.bytes;
        if (images_.size() >= limits::image_cache_max_entries ||
            image_bytes_ + bytes > limits::image_cache_budget_bytes) {
            images_.clear();
            image_bytes_ = 0;
            ++work_.image_evictions;
        }
        it = images_.find(id);
        if (it != images_.end()) {
            image_bytes_ -= it->second.bytes;
            images_.erase(it);
        }
        image_bytes_ += bytes;
        Built built{entry.text, entry.layer};
        images_.emplace(id, std::move(entry));
        return built;
    }

    std::shared_ptr<const SceneImage> ScenePreparer::shadow(const paint::Item& item, const paint::Shadow& s,
                                                            float scale) {
        const std::array<float, 15> key{s.x,        s.y,        s.blur,     s.spread,    s.color[0],
                                        s.color[1], s.color[2], s.color[3], s.radii[0],  s.radii[1],
                                        s.radii[2], s.radii[3], item.width, item.height, scale};
        if (auto it = shadows_.find(key); it != shadows_.end()) {
            ++work_.shadow_reuses;
            return it->second;
        }
        auto image = render::shadow_image(s, item.width, item.height, scale);
        if (!image) {
            throw std::invalid_argument(image.error().message);
        }
        // Premultiplied RGBA to BGRA.
        auto& pixels = image->rgba;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
        }
        const auto bytes = pixels.size();
        if (bytes > limits::image_cache_budget_bytes) {
            throw std::invalid_argument("shadow image byte bound");
        }
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = image->geometry.width;
        result->height = image->geometry.height;
        result->bgra = std::move(pixels);
        ++work_.shadow_builds;
        if (shadows_.size() >= limits::image_cache_max_entries ||
            shadow_bytes_ + bytes > limits::image_cache_budget_bytes) {
            shadows_.clear();
            shadow_bytes_ = 0;
            ++work_.image_evictions;
        }
        shadow_bytes_ += bytes;
        shadows_.emplace(key, result);
        return result;
    }

    std::shared_ptr<const SceneImage> ScenePreparer::path(const paint::Item& item, const paint::PathPaint& p,
                                                          float scale, const std::array<std::int32_t, 4>& mask) {
        paint::Affine device = item.transform;
        for (auto& v : device) {
            v *= scale;
        }
        // The key: every value the coverage depends on, as bytes.
        std::string key;
        const auto add = [&](const void* data, std::size_t size) {
            key.append(static_cast<const char*>(data), size);
        };
        const auto& path = *p.path;
        const auto verbs = path.verbs.size();
        add(&verbs, sizeof verbs);
        add(path.verbs.data(), path.verbs.size() * sizeof(path.verbs[0]));
        add(path.points.data(), path.points.size() * sizeof(path.points[0]));
        add(&path.evenodd, sizeof path.evenodd);
        add(device.data(), sizeof device);
        add(mask.data(), sizeof mask);
        const bool stroked = p.stroke.has_value();
        add(&stroked, sizeof stroked);
        if (stroked) {
            const auto& s = *p.stroke;
            add(&s.width, sizeof s.width);
            add(&s.cap, sizeof s.cap);
            add(&s.join, sizeof s.join);
            add(&s.miter_limit, sizeof s.miter_limit);
            add(&s.dash_offset, sizeof s.dash_offset);
            add(s.dashes.data(), s.dashes.size() * sizeof(s.dashes[0]));
        }
        if (auto it = paths_.find(key); it != paths_.end()) {
            ++work_.path_reuses;
            return it->second;
        }
        auto coverage = vector_.rasterize(p, device, mask);
        if (!coverage) {
            throw std::invalid_argument(coverage.error().message);
        }
        // White coverage, premultiplied BGRA: the record tints it.
        std::vector<std::uint8_t> pixels(coverage->coverage.size() * 4);
        for (std::size_t i = 0; i < coverage->coverage.size(); ++i) {
            std::fill_n(pixels.data() + i * 4, 4, coverage->coverage[i]);
        }
        const auto bytes = pixels.size();
        if (bytes > limits::image_cache_budget_bytes) {
            throw std::invalid_argument("path image byte bound");
        }
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = coverage->width;
        result->height = coverage->height;
        result->bgra = std::move(pixels);
        ++work_.path_builds;
        if (paths_.size() >= limits::image_cache_max_entries ||
            path_bytes_ + bytes > limits::image_cache_budget_bytes) {
            paths_.clear();
            path_bytes_ = 0;
            ++work_.image_evictions;
        }
        path_bytes_ += bytes;
        paths_.emplace(std::move(key), result);
        return result;
    }

    std::unique_ptr<ScenePrepared> ScenePreparer::prepare(const paint::DisplayList& list, float scale, unsigned width,
                                                          unsigned height, std::optional<std::array<float, 4>> crop,
                                                          unsigned view_index) {
        // Scene passes sample no asset textures and run no effects yet.
        if (std::any_of(list.items.begin(), list.items.end(), [](const paint::Item& item) {
                return item.kind == Kind::Image || item.kind == Kind::Effect;
            })) {
            throw std::invalid_argument("scene renderer draws no image or effect items");
        }
        auto planned = render::plan_scene_pass(list, scale, width, height, crop);
        if (!planned) {
            throw std::invalid_argument(planned.error().message);
        }
        auto out = std::make_unique<ScenePrepared>();
        out->pass = std::move(*planned);
        out->width = width;
        out->height = height;
        out->images.resize(list.items.size());
        out->planes.resize(list.items.size());
        out->layers.resize(list.items.size());
        for (std::size_t i = 0; i < list.items.size(); ++i) {
            const auto& item = list.items[i];
            if (item.kind == Kind::Text || item.kind == Kind::Layer) {
                auto built = image(item, list.rasters[item.payload], scale, view_index);
                out->images[i] = std::move(built.text);
                out->layers[i] = std::move(built.layer);
            } else if (item.kind == Kind::Shadow) {
                out->images[i] = shadow(item, list.shadows[item.payload], scale);
            } else if (item.kind == Kind::Path) {
                const auto& mask = out->pass.masks[i];
                if (mask[2] > mask[0] && mask[3] > mask[1]) {
                    out->images[i] = path(item, list.paths[item.payload], scale, mask);
                }
            } else if (item.kind == Kind::Plane) {
                // The content flat in its own image, image_margin px in, texel per device pixel.
                const auto& plane = list.planes[item.payload];
                const auto pixels = render::plane_pixels(plane, scale);
                if (pixels[0] > max_image_dimension_ || pixels[1] > max_image_dimension_) {
                    throw std::invalid_argument("plane image bound");
                }
                paint::DisplayList content;
                const float margin = limits::image_margin;
                if (const auto shifted = paint::append(content, *plane.content, {1, 0, 0, 1, margin, margin});
                    !shifted) {
                    throw std::invalid_argument(shifted.error().message);
                }
                out->planes[i] = prepare(content, scale, pixels[0], pixels[1], std::nullopt, view_index);
            }
        }
        return out;
    }
} // namespace weft
