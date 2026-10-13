#include "scene/scene_native.hpp"

#include "constants/common.hpp"
#include "render/shadow_image.hpp"
#include "render/srgb.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_text_image.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
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
                                 Reference reference, SceneCacheBounds bounds)
        : text_(text),
          work_(work),
          max_image_dimension_(max_image_dimension),
          reference_(std::move(reference)),
          geometry_(text),
          images_(bounds.entries, bounds.bytes),
          shadows_(bounds.entries, bounds.bytes),
          paths_(bounds.entries, bounds.bytes),
          effect_textures_(bounds.entries, bounds.bytes) {
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
        // What this pass uses stays in the caches until the next pass.
        images_.begin_pass();
        shadows_.begin_pass();
        paths_.begin_pass();
        effect_textures_.begin_pass();
        auto compiled = scene_display_list(frame, layers, &text_, scale);
        if (!compiled.fallback.empty()) {
            return fallback(compiled.fallback);
        }
        if (const auto valid = paint::check(compiled.list, limits::max_group_depth); !valid) {
            return fallback(valid.error().message);
        }
        try {
            auto prepared = prepare(compiled.list, scale, width, height, crop, view_index);
            // The pass's working set: each image once.
            std::map<std::uint64_t, std::size_t> images;
            const auto count = [&](auto&& self, const ScenePrepared& p) -> void {
                for (const auto& image : p.images) {
                    if (image) {
                        images.emplace(image->id, image->pixels.size());
                    }
                }
                for (const auto& effect : p.effects) {
                    for (const auto& texture : effect.textures) {
                        images.emplace(texture->id, texture->pixels.size());
                    }
                }
                for (const auto& plane : p.planes) {
                    if (plane) {
                        self(self, *plane);
                    }
                }
                for (const auto& layer : p.layers) {
                    if (layer && layer->content) {
                        self(self, *layer->content);
                    }
                }
            };
            count(count, *prepared);
            std::size_t bytes{};
            for (const auto& [id, size] : images) {
                bytes += size;
            }
            work_.peak_pass_images = std::max(work_.peak_pass_images, images.size());
            work_.peak_pass_image_bytes = std::max(work_.peak_pass_image_bytes, bytes);
            return prepared;
        } catch (const std::invalid_argument& e) {
            return fallback(e.what());
        }
    }

    ScenePreparer::Built ScenePreparer::image(const paint::Item& item, const paint::Raster& raster, float scale,
                                              unsigned view_index) {
        const auto& n = *raster.source;
        std::array<std::uint64_t, 5> id{view_index, n.identity.view_epoch, n.identity.slot, n.identity.generation,
                                        raster.part};
        if (const auto* cached = images_.find(id);
            cached && scene_paint_source_equal(cached->key, n) && cached->scale == scale) {
            if (item.kind == Kind::Layer) {
                ++work_.layer_reuses;
            } else {
                ++work_.text_reuses;
            }
            return {cached->text, cached->layer};
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
            if (w > max_image_dimension_ || h > max_image_dimension_ || entry.bytes > limits::image_max_bytes) {
                throw std::invalid_argument("layer image byte bound");
            }
            const auto listed = scene_display_list(frame, {}, &text_, scale);
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
            if (entry.bytes > limits::image_max_bytes) {
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
            image->pixels = std::move(bitmap.rgba);
            entry.text = std::move(image);
            ++work_.text_builds;
        }
        const auto bytes = entry.bytes;
        Built built{entry.text, entry.layer};
        const auto evictions = images_.evictions();
        images_.put(id, std::move(entry), bytes);
        work_.image_evictions += images_.evictions() - evictions;
        return built;
    }

    std::shared_ptr<const SceneImage> ScenePreparer::shadow(const paint::Item& item, const paint::Shadow& s,
                                                            float scale) {
        const std::array<float, 15> key{s.x,        s.y,        s.blur,     s.spread,    s.color[0],
                                        s.color[1], s.color[2], s.color[3], s.radii[0],  s.radii[1],
                                        s.radii[2], s.radii[3], item.width, item.height, scale};
        if (const auto* cached = shadows_.find(key)) {
            ++work_.shadow_reuses;
            return *cached;
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
        if (bytes > limits::image_max_bytes) {
            throw std::invalid_argument("shadow image byte bound");
        }
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = image->geometry.width;
        result->height = image->geometry.height;
        result->pixels = std::move(pixels);
        ++work_.shadow_builds;
        const auto evictions = shadows_.evictions();
        shadows_.put(key, result, bytes);
        work_.image_evictions += shadows_.evictions() - evictions;
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
        if (const auto* cached = paths_.find(key)) {
            ++work_.path_reuses;
            return *cached;
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
        if (bytes > limits::image_max_bytes) {
            throw std::invalid_argument("path image byte bound");
        }
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = coverage->width;
        result->height = coverage->height;
        result->pixels = std::move(pixels);
        ++work_.path_builds;
        const auto evictions = paths_.evictions();
        paths_.put(key, result, bytes);
        work_.image_evictions += paths_.evictions() - evictions;
        return result;
    }

    std::shared_ptr<const SceneImage> ScenePreparer::effect_texture(const std::shared_ptr<const paint::Asset>& asset) {
        if (const auto* cached = effect_textures_.find(asset)) {
            ++work_.effect_texture_reuses;
            return *cached;
        }
        const auto& a = *asset;
        const auto pixels = std::size_t(a.width) * a.height;
        const bool color = a.encoding == paint::Encoding::SrgbStraight;
        if (!a.width || !a.height || a.encoding == paint::Encoding::SignedDistance ||
            a.bytes.size() != pixels * (color ? constants::rgba8_bytes_per_pixel : 1)) {
            throw std::invalid_argument("effect texture");
        }
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = a.width;
        result->height = a.height;
        bool opaque = true;
        for (std::size_t p = 3; color && opaque && p < a.bytes.size(); p += constants::rgba8_bytes_per_pixel) {
            opaque = a.bytes[p] == constants::color_channel_max;
        }
        if (!color || opaque) {
            result->format = color ? SceneImage::Format::Srgb : SceneImage::Format::Data;
            result->pixels = a.bytes;
        } else {
            // Linear premultiplied, as the display list renderers sample translucent images.
            std::vector<float> linear(pixels * constants::rgba8_bytes_per_pixel);
            for (std::size_t i = 0; i < pixels; ++i) {
                const auto* p = a.bytes.data() + i * constants::rgba8_bytes_per_pixel;
                const float alpha = p[3] / constants::render::color_channel_max_float;
                for (unsigned c = 0; c < 3; ++c) {
                    linear[i * 4 + c] =
                        render::srgb_to_linear(p[c] / constants::render::color_channel_max_float) * alpha;
                }
                linear[i * 4 + 3] = alpha;
            }
            result->format = SceneImage::Format::Linear;
            result->pixels.resize(linear.size() * sizeof(float));
            std::memcpy(result->pixels.data(), linear.data(), result->pixels.size());
        }
        const auto bytes = result->pixels.size();
        if (bytes > limits::image_max_bytes) {
            throw std::invalid_argument("effect texture byte bound");
        }
        ++work_.effect_texture_builds;
        const auto evictions = effect_textures_.evictions();
        effect_textures_.put(asset, result, bytes);
        work_.image_evictions += effect_textures_.evictions() - evictions;
        return result;
    }

    std::unique_ptr<ScenePrepared> ScenePreparer::prepare(const paint::DisplayList& list, float scale, unsigned width,
                                                          unsigned height, std::optional<std::array<float, 4>> crop,
                                                          unsigned view_index) {
        // Scene passes sample no asset images.
        if (std::any_of(list.items.begin(), list.items.end(), [](const paint::Item& item) {
                return item.kind == Kind::Image;
            })) {
            throw std::invalid_argument("scene renderer draws no image items");
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
        for (const auto& effect : list.effects) {
            auto& prepared = out->effects.emplace_back();
            prepared.program = list.programs.at(effect.program);
            for (const auto texture : effect.textures) {
                prepared.textures.push_back(effect_texture(list.assets.at(texture)));
            }
        }
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
