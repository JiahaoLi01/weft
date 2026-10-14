#include "scene/scene_native.hpp"

#include "constants/common.hpp"
#include "render/shadow_image.hpp"
#include "render/srgb.hpp"
#include "scene/scene_paint.hpp"
#include "scene/scene_text_image.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace weft {
    namespace {
        namespace limits = constants::render::scene;
        using Kind = paint::Item::Kind;

        // A node's paint source as bytes, fields length-prefixed, its text first (it tells most sources apart).
        struct SourceKey {
            std::string out;

            void bytes(const void* data, std::size_t size) {
                out.append(static_cast<const char*>(data), size);
            }

            void count(std::size_t n) {
                const auto v = std::uint64_t(n);
                bytes(&v, sizeof v);
            }

            void text(std::string_view s) {
                count(s.size());
                out.append(s);
            }

            // A spelling of its own, which no other key has: for what equals nothing.
            void unique() {
                static std::atomic<std::uint64_t> next{};
                const std::uint64_t marker[2]{~std::uint64_t{}, next.fetch_add(1, std::memory_order_relaxed)};
                bytes(marker, sizeof marker);
            }

            // A number as == compares it: one spelling of zero, and a unique one for NaN.
            void number(double v) {
                if (std::isnan(v)) {
                    unique();
                    return;
                }
                if (v == 0) {
                    v = 0;
                }
                bytes(&v, sizeof v);
            }

            template <std::size_t N>
            void numbers(const std::array<float, N>& values) {
                for (const auto v : values) {
                    number(v);
                }
            }

            void paint(const PaintValue& v) {
                const auto kind = std::uint8_t(v.kind);
                bytes(&kind, sizeof kind);
                number(v.scalar.absolute);
                number(v.scalar.relative);
                for (const auto c : v.color) {
                    number(c);
                }
                count(v.operations.size());
                for (const auto& op : v.operations) {
                    const auto op_kind = std::uint8_t(op.kind);
                    bytes(&op_kind, sizeof op_kind);
                    count(op.count);
                    for (const auto& arg : op.args) {
                        number(arg.absolute);
                        number(arg.relative);
                    }
                }
            }

            void node(const SceneNode& n) {
                text(n.text);
                text(n.tag);
                text(n.space);
                count(n.style.size());
                for (const auto& [id, value] : n.style) {
                    count(std::size_t(id));
                    text(value.text());
                }
                count(n.attributes.size());
                for (const auto& [name, value] : n.attributes) {
                    text(name);
                    text(value);
                }
                numbers(n.box);
                count(n.lines.size());
                for (const auto& line : n.lines) {
                    text(line.text);
                    numbers(line.rect);
                    number(line.baseline);
                }
                count(n.fragments.size());
                for (const auto& fragment : n.fragments) {
                    numbers(fragment.rect);
                    const std::uint8_t ends[2]{fragment.first, fragment.last};
                    bytes(ends, sizeof ends);
                }
                count(n.paint ? 1 : 0);
                if (n.paint) {
                    for (const auto& slot : n.paint->values) {
                        count(slot ? 1 : 0);
                        if (slot) {
                            paint(*slot);
                        }
                    }
                }
                // Effects and particles are not keyed: a node that shows them shares its source with none.
                if (n.effect || !n.particles.empty()) {
                    unique();
                }
                count(n.children.size());
                for (const auto& child : n.children) {
                    node(child);
                }
            }
        };

        // splitmix64's finalizer over the digest so far and the next word.
        std::uint64_t mix(std::uint64_t h, std::uint64_t word) {
            std::uint64_t z = h ^ word;
            z += 0x9e3779b97f4a7c15ull;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            return z ^ (z >> 31);
        }

        std::uint64_t floats_digest(std::uint64_t h, const float* values, std::size_t n) {
            h = mix(h, n);
            for (std::size_t i = 0; i < n; ++i) {
                h = mix(h, std::bit_cast<std::uint32_t>(values[i]));
            }
            return h;
        }

        // The records of images on atlas pages sample their pages, and each run of steps next to each other in item order
        // that are boxes or images on one page (or paths with no image, which draw nothing) draws in one Atlas step.
        void atlas_batches(ScenePrepared& p) {
            using Step = render::ScenePass::Step;
            auto& pass = p.pass;
            for (std::size_t i = 0; i < p.images.size(); ++i) {
                if (const auto& image = p.images[i]; image && image->atlas) {
                    const auto size = float(scene_atlas_page_size(image->format));
                    pass.records[i].uv = {float(image->atlas->x) / size, float(image->atlas->y) / size,
                                          float(image->width) / size, float(image->height) / size};
                }
            }
            std::vector<render::ScenePass::Batch> merged;
            merged.reserve(pass.batches.size());
            // The open run: whether there is one, the steps it joined, and its page once an image on one joined it.
            bool open{};
            std::size_t joined{};
            std::optional<SceneAtlasPage> page;
            const auto close = [&] {
                if (open && page) {
                    auto& run = merged.back();
                    run.step = Step::Atlas;
                    const auto found = std::find(p.pages.begin(), p.pages.end(), *page);
                    run.source = std::uint32_t(found - p.pages.begin());
                    if (found == p.pages.end()) {
                        p.pages.push_back(*page);
                    }
                } else if (open && joined > 1) {
                    merged.back().step = Step::Boxes;
                }
                open = false;
                joined = 0;
                page.reset();
            };
            for (const auto& batch : pass.batches) {
                const bool image = batch.step == Step::Image && !p.planes[batch.first] && !p.layers[batch.first];
                const auto* drawn = image ? p.images[batch.first].get() : nullptr;
                if (!(batch.step == Step::Boxes || (image && (!drawn || drawn->atlas)))) {
                    close();
                    merged.push_back(batch);
                    continue;
                }
                std::optional<SceneAtlasPage> on;
                if (drawn) {
                    on = SceneAtlasPage{drawn->format, drawn->atlas->page};
                }
                if (open &&
                    (merged.back().first + merged.back().count != batch.first || (on && page && *on != *page))) {
                    close();
                }
                if (open) {
                    merged.back().count += batch.count;
                } else {
                    merged.push_back(batch);
                    open = true;
                }
                ++joined;
                if (on) {
                    page = on;
                }
            }
            close();
            pass.batches = std::move(merged);
        }
    } // namespace

    std::uint32_t scene_atlas_page_size(SceneImage::Format format) {
        return std::uint32_t(format == SceneImage::Format::Data ? limits::atlas_page_size
                                                                : limits::atlas_color_page_size);
    }

    std::vector<std::uint8_t> scene_atlas_pixels(const SceneImage& image) {
        constexpr auto border = std::uint32_t(limits::atlas_border);
        const auto texel = scene_image_texel_bytes(image.format);
        const auto row = std::size_t(image.width) * texel;
        const auto w = std::size_t(image.width + 2 * border) * texel;
        std::vector<std::uint8_t> out(w * (image.height + 2 * border));
        for (std::uint32_t y = 0; y < image.height + 2 * border; ++y) {
            // The nearest row of the image, and on it the nearest texels beyond its ends.
            const auto* from = image.pixels.data() + std::min(std::max(y, border) - border, image.height - 1) * row;
            auto* to = out.data() + y * w;
            for (std::uint32_t x = 0; x < border; ++x) {
                std::memcpy(to + x * texel, from, texel);
                std::memcpy(to + border * texel + row + x * texel, from + row - texel, texel);
            }
            std::memcpy(to + border * texel, from, row);
        }
        return out;
    }

    std::string scene_paint_source_key(const SceneNode& node) {
        SourceKey key;
        key.node(node);
        return std::move(key.out);
    }

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
          texts_(bounds.entries, bounds.bytes),
          layers_(bounds.entries, bounds.bytes),
          shadows_(bounds.entries, bounds.bytes),
          paths_(bounds.entries, bounds.bytes),
          effect_textures_(bounds.entries, bounds.bytes),
          coverage_atlas_(std::uint32_t(limits::atlas_page_size), bounds.atlas_pages),
          color_atlas_(std::uint32_t(limits::atlas_color_page_size), bounds.atlas_pages),
          atlas_(bounds.atlas_pages > 0) {
    }

    void ScenePreparer::place(const std::shared_ptr<SceneImage>& image) {
        constexpr auto border = std::uint32_t(limits::atlas_border);
        const bool coverage = image->format == SceneImage::Format::Data;
        auto& atlas = coverage ? coverage_atlas_ : color_atlas_;
        const auto width = image->width + 2 * border;
        const auto height = image->height + 2 * border;
        if (!atlas_ || width > atlas.page_size() / 2 || height > std::uint32_t(limits::atlas_max_height)) {
            return;
        }
        const auto region = atlas.allocate(width, height);
        if (!region) {
            ++work_.atlas_refusals;
            return;
        }
        image->atlas = SceneAtlasPlace{region->page, region->x + border, region->y + border};
        placed_.push_back({image, coverage, *region});
        ++work_.atlas_placements;
    }

    std::unique_ptr<ScenePrepared> ScenePreparer::prepare(const SceneFrame& frame, float scale, unsigned width,
                                                          unsigned height, std::optional<std::array<float, 4>> crop,
                                                          unsigned view_index, const std::vector<NodeIdentity>& layers,
                                                          const SceneAnchorTable* anchors) {
        const auto fallback = [&](const std::string& reason) {
            ++work_.fallbacks;
            ++work_.reasons[reason];
            return nullptr;
        };
        // The atlas regions of images that are gone.
        std::erase_if(placed_, [this](const Placed& placed) {
            if (!placed.image.expired()) {
                return false;
            }
            (placed.coverage ? coverage_atlas_ : color_atlas_).release(placed.region);
            return true;
        });
        // What this pass uses stays in the caches until the next pass.
        svg_.begin_pass(view_index);
        texts_.begin_pass();
        layers_.begin_pass();
        shadows_.begin_pass();
        paths_.begin_pass();
        effect_textures_.begin_pass();
        // The digests of paths that are gone, once there are as many again as after the last sweep.
        if (digests_.size() > 2 * digests_swept_ + std::size_t(limits::image_cache_max_entries)) {
            std::erase_if(digests_, [](const auto& entry) {
                return entry.second.path.expired();
            });
            digests_swept_ = digests_.size();
        }
        const auto svg_builds = svg_.builds();
        const auto svg_reuses = svg_.reuses();
        auto& anchored = anchored_[view_index];
        auto compiled = scene_display_list(frame, layers, &text_, scale, &svg_, {anchors, &anchored});
        work_.svg_value_builds += svg_.builds() - svg_builds;
        work_.svg_value_reuses += svg_.reuses() - svg_reuses;
        if (!compiled.fallback.empty()) {
            return fallback(compiled.fallback);
        }
        anchored = std::move(compiled.placed);
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
        const float margin = limits::image_margin + raster.overhang;
        // A layer by its node; text by its source alone, so that the same text of another node shares the image.
        std::array<std::uint64_t, 5> id{view_index, n.identity.view_epoch, n.identity.slot, n.identity.generation,
                                        raster.part};
        std::string source;
        if (item.kind == Kind::Layer) {
            if (const auto* cached = layers_.find(id);
                cached && scene_paint_source_equal(cached->key, n) && cached->scale == scale) {
                ++work_.layer_reuses;
                return {nullptr, cached->image};
            }
        } else {
            source = scene_paint_source_key(n);
            source.append(reinterpret_cast<const char*>(&scale), sizeof scale);
            source.append(reinterpret_cast<const char*>(&margin), sizeof margin);
            if (const auto* cached = texts_.find(source)) {
                ++work_.text_reuses;
                return {*cached, nullptr};
            }
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
            frame.root.box = {0, 0, n.box[2] + 2 * margin, n.box[3] + 2 * margin};
            frame.root.style = {{"background-color", "transparent"}};
            auto child = n;
            child.box[0] = child.box[1] = margin;
            frame.root.children.push_back(std::move(child));
            return frame;
        };
        if (item.kind == Kind::Layer) {
            Layer entry;
            entry.key = n;
            entry.scale = scale;
            const auto frame = framed();
            const auto w = unsigned(std::ceil(frame.root.box[2] * scale));
            const auto h = unsigned(std::ceil(frame.root.box[3] * scale));
            const auto bytes = std::size_t(w) * h * constants::rgba8_bytes_per_pixel;
            if (w > max_image_dimension_ || h > max_image_dimension_ || bytes > limits::image_max_bytes) {
                throw std::invalid_argument("layer image byte bound");
            }
            const auto listed = scene_display_list(frame, {}, &text_, scale, &svg_);
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
            entry.image = layer;
            ++work_.layer_builds;
            const auto evictions = layers_.evictions();
            layers_.put(id, std::move(entry), bytes);
            work_.image_evictions += layers_.evictions() - evictions;
            return {nullptr, std::move(layer)};
        }
        {
            SceneBitmap bitmap;
            auto text = scene_text_image(text_, n, scale, margin);
            if (!text.color_glyphs) {
                bitmap = std::move(text.bitmap);
                ++work_.text_portable_builds;
            } else if (reference_) {
                bitmap = reference_(framed(), scale);
                ++work_.reference_builds;
            } else {
                throw std::invalid_argument("color glyph image reference");
            }
            if (bitmap.rgba.size() > limits::image_max_bytes) {
                throw std::invalid_argument("text source byte bound");
            }
            auto image = std::make_shared<SceneImage>();
            image->id = ++next_id_;
            image->width = bitmap.width;
            image->height = bitmap.height;
            if (raster.mask) {
                // A mask is white text, whose premultiplied channels all equal its alpha: the coverage its record
                // tints, a byte per texel.
                image->format = SceneImage::Format::Data;
                image->pixels.resize(bitmap.rgba.size() / constants::rgba8_bytes_per_pixel);
                for (std::size_t i = 0; i < image->pixels.size(); ++i) {
                    image->pixels[i] = bitmap.rgba[i * constants::rgba8_bytes_per_pixel + 3];
                }
            } else {
                // Straight RGBA to premultiplied BGRA.
                for (std::size_t i = 0; i < bitmap.rgba.size(); i += 4) {
                    std::swap(bitmap.rgba[i], bitmap.rgba[i + 2]);
                    for (unsigned c = 0; c < 3; ++c) {
                        bitmap.rgba[i + c] = std::uint8_t(
                            (unsigned(bitmap.rgba[i + c]) * bitmap.rgba[i + 3] + constants::color_channel_max / 2) /
                            constants::color_channel_max);
                    }
                }
                image->pixels = std::move(bitmap.rgba);
            }
            const auto bytes = image->pixels.size();
            ++work_.text_builds;
            place(image);
            const auto evictions = texts_.evictions();
            texts_.put(source, image, bytes);
            work_.image_evictions += texts_.evictions() - evictions;
            return {std::move(image), nullptr};
        }
    }

    std::uint64_t ScenePreparer::digest(const std::shared_ptr<const paint::Path>& path) {
        // A live object at the same address is this one.
        auto& known = digests_[path.get()];
        if (!known.path.expired()) {
            return known.value;
        }
        const auto& p = *path;
        std::uint64_t h = mix(0, p.verbs.size());
        for (const auto verb : p.verbs) {
            h = mix(h, std::uint64_t(verb));
        }
        h = floats_digest(h, p.points.empty() ? nullptr : p.points.front().data(), p.points.size() * 2);
        known.path = path;
        known.value = mix(h, p.evenodd);
        return known.value;
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
        place(result);
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
        // The key: every value the coverage depends on, the path by its digest.
        const auto& path = *p.path;
        const auto bits = [](float v) {
            return std::bit_cast<std::uint32_t>(v);
        };
        const auto d = digest(p.path);
        std::array<std::uint32_t, 21> key{};
        key[0] = std::uint32_t(d);
        key[1] = std::uint32_t(d >> 32);
        key[2] = std::uint32_t(path.verbs.size());
        key[3] = std::uint32_t(path.points.size());
        const bool stroked = p.stroke.has_value();
        key[4] = std::uint32_t(path.evenodd) | std::uint32_t(stroked) << 1;
        for (unsigned i = 0; i < device.size(); ++i) {
            key[5 + i] = bits(device[i]);
        }
        for (unsigned i = 0; i < mask.size(); ++i) {
            key[11 + i] = std::uint32_t(mask[i]);
        }
        if (stroked) {
            const auto& s = *p.stroke;
            key[4] |= std::uint32_t(s.cap) << 2 | std::uint32_t(s.join) << 4;
            key[15] = bits(s.width);
            key[16] = bits(s.miter_limit);
            key[17] = bits(s.dash_offset);
            const auto dashes = floats_digest(0, s.dashes.data(), s.dashes.size());
            key[18] = std::uint32_t(dashes);
            key[19] = std::uint32_t(dashes >> 32);
            key[20] = std::uint32_t(s.dashes.size());
        }
        static const std::vector<float> solid;
        const auto& dashes = stroked ? p.stroke->dashes : solid;
        if (auto* cached = paths_.find(key)) {
            // The same path object, or one of the same verbs and points (a digest may collide).
            if (cached->path != p.path && cached->path->verbs == path.verbs && cached->path->points == path.points &&
                cached->path->evenodd == path.evenodd) {
                cached->path = p.path;
            }
            if (cached->path == p.path && cached->dashes == dashes) {
                ++work_.path_reuses;
                return cached->image;
            }
        }
        auto coverage = vector_.rasterize(p, device, mask);
        if (!coverage) {
            throw std::invalid_argument(coverage.error().message);
        }
        // The coverage as it is, a byte per texel: the record tints it.
        const auto bytes = coverage->coverage.size();
        if (bytes > limits::image_max_bytes) {
            throw std::invalid_argument("path image byte bound");
        }
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = coverage->width;
        result->height = coverage->height;
        result->format = SceneImage::Format::Data;
        result->pixels = std::move(coverage->coverage);
        ++work_.path_builds;
        place(result);
        const auto evictions = paths_.evictions();
        paths_.put(key, {p.path, dashes, result}, bytes);
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
        for (const auto& particles : list.particles) {
            const auto& sprite = particles.emitter.sprite;
            out->sprites.push_back(sprite ? effect_texture(list.assets.at(*sprite)) : nullptr);
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
        atlas_batches(*out);
        return out;
    }
} // namespace weft
