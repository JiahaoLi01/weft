#include "scene/scene_native.hpp"

#include "base/css_scalar.hpp"
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
#include <tuple>
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
                const bool image = batch.step == Step::Image && !p.planes[batch.first] && !p.layers[batch.first] &&
                                   !p.textures[batch.first];
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

        // Whether a pass may draw a run's items in another order without planning it again: they lie in one batch
        // that draws its records in any order (boxes, or boxes and images on one atlas page).
        bool reorderable(const ScenePrepared& prepared, const SceneListRun& run) {
            const auto first = run.members.front().first;
            const auto end = run.members.back().end;
            return first == end ||
                   std::any_of(prepared.pass.batches.begin(), prepared.pass.batches.end(), [&](const auto& b) {
                       return (b.step == render::ScenePass::Step::Boxes || b.step == render::ScenePass::Step::Atlas) &&
                              b.first <= first && end <= b.first + b.count;
                   });
        }
    } // namespace

    std::vector<const SceneTexture*> scene_texture_bindings(const ScenePrepared& prepared,
                                                            std::span<const SceneTexture> textures) {
        std::vector<const SceneTexture*> out(prepared.texture_names.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto found = std::find_if(textures.begin(), textures.end(), [&](const SceneTexture& t) {
                return t.name == prepared.texture_names[i];
            });
            out[i] = found == textures.end() ? nullptr : &*found;
        }
        return out;
    }

    std::vector<bool> scene_texture_flips(const ScenePrepared& prepared, std::span<const SceneTexture> textures) {
        const auto bound = scene_texture_bindings(prepared, textures);
        std::vector<bool> out(bound.size());
        for (std::size_t i = 0; i < bound.size(); ++i) {
            out[i] = bound[i] && bound[i]->bottom_left;
        }
        return out;
    }

    bool scene_planes_draw_textures(const ScenePrepared& prepared) {
        return std::any_of(prepared.planes.begin(), prepared.planes.end(), [](const auto& plane) {
            return plane && (std::any_of(plane->textures.begin(), plane->textures.end(),
                                         [](std::uint32_t t) {
                                             return t != 0;
                                         }) ||
                             scene_planes_draw_textures(*plane));
        });
    }

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
          path_shadows_(bounds.entries, bounds.bytes),
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
                                                          const SceneAnchorTable* anchors, bool defer, bool over) {
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
        path_shadows_.begin_pass();
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
        defer = defer && !crop;
        // A deferred list without layers is kept for patch, listed from a copy of the frame whose nodes its spans
        // name.
        listed_.erase(view_index);
        Listed* listed = defer && layers.empty() ? &listed_[view_index] : nullptr;
        if (listed) {
            listed->frame.root = frame.root;
            listed->frame.time_ms = frame.time_ms;
            listed->frame.animated = frame.animated;
            listed->frame.revision = frame.revision;
        }
        auto compiled = scene_display_list(listed ? listed->frame : frame, layers, &text_, scale, &svg_,
                                           {anchors, &anchored, defer}, {listed ? &listed->spans : nullptr});
        if (defer && compiled.fallback.empty() && !compiled.deferred) {
            // Anchored subtrees the pass cannot move: placed now, every frame.
            compiled = scene_display_list(frame, layers, &text_, scale, &svg_, {anchors, &anchored});
            defer = false;
            listed_.erase(view_index);
            listed = nullptr;
        }
        work_.svg_value_builds += svg_.builds() - svg_builds;
        work_.svg_value_reuses += svg_.reuses() - svg_reuses;
        if (!compiled.fallback.empty()) {
            listed_.erase(view_index);
            return fallback(compiled.fallback);
        }
        if (!defer) {
            anchored = std::move(compiled.placed);
        }
        if (const auto valid = paint::check(compiled.list, limits::max_group_depth); !valid) {
            listed_.erase(view_index);
            return fallback(valid.error().message);
        }
        try {
            auto prepared = prepare(compiled.list, scale, width, height, crop, view_index,
                                    std::uint32_t(compiled.anchors.size()), over);
            if (defer) {
                prepared->anchors = compiled.anchors;
                prepared->runs = compiled.runs;
                prepared->view = {frame.root.box[2], frame.root.box[3]};
                prepared->keepable = !scene_planes_draw_textures(*prepared);
                for (auto& run : prepared->runs) {
                    run.reorderable = reorderable(*prepared, run);
                }
            }
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
            if (listed && prepared->keepable) {
                listed->list = std::move(compiled);
                listed->scale = scale;
                listed->width = width;
                listed->height = height;
            } else {
                listed_.erase(view_index);
            }
            return prepared;
        } catch (const std::invalid_argument& e) {
            listed_.erase(view_index);
            return fallback(e.what());
        }
    }

    namespace {
        using Part = SceneListSpan::Part;

        // A node of the frame whose storage is not the listed frame's, and the node it replaces: a subtree to list
        // again (whole), or a node of the same own values on the way to one, whose span is the node's it replaces.
        struct Changed {
            const SceneNode* old{};
            const SceneNode* now{};
            bool whole{};
        };

        std::string_view z_index(const SceneNode& n) {
            return n.style.text(PropertyId::ZIndex, "0");
        }

        // A node whose visit lists its subtree as no visit of its descendants does: an svg element (its clip-paths
        // find their definitions anywhere inside it) and a perspective plane (a list of its own).
        bool atomic(const SceneNode& n) {
            return n.tag == "svg" ||
                   n.style.text(PropertyId::Transform, "none").find("perspective(") != std::string_view::npos;
        }

        // Adds the nodes below now (whose own values are old's) whose storage differs from old's to changed, each with
        // the node it replaces: subtrees to list again, and the nodes on the way to them; false when now's children are
        // not old's in their order and z-index, so that now is to be listed whole.
        bool changed_parts(const SceneNode& old, const SceneNode& now, std::vector<Changed>& changed) {
            if (old.children.size() != now.children.size()) {
                return false;
            }
            for (std::size_t k = 0; k < now.children.size(); ++k) {
                const auto a = old.children.handle(k);
                const auto b = now.children.handle(k);
                if (a == b) {
                    continue;
                }
                if (a->identity != b->identity || z_index(*a) != z_index(*b)) {
                    return false;
                }
                const auto before = changed.size();
                if (!scene_own_equal(*a, *b) || atomic(*b) || !changed_parts(*a, *b, changed)) {
                    changed.resize(before);
                    changed.push_back({a.get(), b.get(), true});
                } else {
                    changed.push_back({a.get(), b.get(), false});
                }
            }
            return true;
        }

        // What of a subtree its ancestors' visits read: its anchored elements with their anchoring, and the elements
        // with a backdrop filter.
        void ancestors_read(const SceneNode& n, std::vector<std::pair<NodeIdentity, SceneAnchoring>>& anchored,
                            std::vector<NodeIdentity>& filtered) {
            if (n.anchoring) {
                anchored.emplace_back(n.identity, *n.anchoring);
            }
            const auto filter = css::trim(n.style.text(PropertyId::BackdropFilter, "none"));
            if (!filter.empty() && filter != "none") {
                filtered.push_back(n.identity);
            }
            for (const auto& c : n.children) {
                ancestors_read(c, anchored, filtered);
            }
        }

        bool same_for_ancestors(const SceneNode& a, const SceneNode& b) {
            std::vector<std::pair<NodeIdentity, SceneAnchoring>> anchored_a;
            std::vector<std::pair<NodeIdentity, SceneAnchoring>> anchored_b;
            std::vector<NodeIdentity> filtered_a;
            std::vector<NodeIdentity> filtered_b;
            ancestors_read(a, anchored_a, filtered_a);
            ancestors_read(b, anchored_b, filtered_b);
            return anchored_a == anchored_b && filtered_a == filtered_b;
        }

        // Whether a patch can replace an item of this kind in place: what it draws needs no effect program, asset,
        // texture name, plane, backdrop or particles, whose items a pass batches or places by more than their kind.
        bool patchable(paint::Item::Kind kind) {
            using Kind = paint::Item::Kind;
            return kind == Kind::Box || kind == Kind::Text || kind == Kind::Shadow || kind == Kind::Path ||
                   kind == Kind::PathShadow || kind == Kind::BeginGroup || kind == Kind::EndGroup;
        }

        // The payload list of an item's kind (SceneListSpan::Part); Parts for groups.
        Part payload_part(paint::Item::Kind kind) {
            using Kind = paint::Item::Kind;
            switch (kind) {
                case Kind::Box:
                    return Part::Boxes;
                case Kind::Text:
                case Kind::Layer:
                    return Part::Rasters;
                case Kind::Image:
                    return Part::Images;
                case Kind::Shadow:
                    return Part::Shadows;
                case Kind::Path:
                    return Part::Paths;
                case Kind::Plane:
                    return Part::Planes;
                case Kind::Effect:
                    return Part::Effects;
                case Kind::Particles:
                    return Part::Particles;
                case Kind::Backdrop:
                    return Part::Backdrops;
                case Kind::PathShadow:
                    return Part::PathShadows;
                case Kind::BeginGroup:
                case Kind::EndGroup:
                    break;
            }
            return Part::Parts;
        }

        // How an image's item joins batches: no image (a path with nothing in its clip), on an atlas page, or a texture
        // of its own.
        std::optional<SceneAtlasPage> batch_place(const std::shared_ptr<const SceneImage>& image, bool& own) {
            own = image && !image->atlas;
            if (image && image->atlas) {
                return SceneAtlasPage{image->format, image->atlas->page};
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<ScenePatch> ScenePreparer::patch(ScenePrepared& prepared, const SceneFrame& frame,
                                                   unsigned view_index, const SceneAnchorTable* anchors) {
        using Kind = paint::Item::Kind;
        const auto refuse = [&](const char* reason) -> std::optional<ScenePatch> {
            ++work_.patch_refusals[reason];
            return std::nullopt;
        };
        const auto it = listed_.find(view_index);
        if (it == listed_.end()) {
            return refuse("no list");
        }
        auto& listed = it->second;
        auto& whole = listed.list;
        if (!whole.list.effects.empty() || prepared.width != listed.width || prepared.height != listed.height ||
            !scene_own_equal(listed.frame.root, frame.root)) {
            return refuse("another root, size or effects");
        }
        std::vector<Changed> changed;
        if (!changed_parts(listed.frame.root, frame.root, changed)) {
            return refuse("children in another order or z-index");
        }
        // Listing most of the list again a part at a time costs more than listing it whole.
        std::size_t parted{};
        for (const auto& part : changed) {
            if (part.whole) {
                if (const auto found = listed.spans.find(part.old); found != listed.spans.end()) {
                    parted += found->second.end[Part::Items] - found->second.begin[Part::Items];
                }
            }
        }
        if (parted * 2 > whole.list.items.size()) {
            return refuse("most of the list changed");
        }

        // Each part listed again, its records, masks and images: nothing changes before every part fits.
        struct Item {
            render::SceneRecord record;
            std::array<std::int32_t, 4> mask{};
            std::shared_ptr<const SceneImage> image;
        };

        struct Listing {
            // A copy: committing a part forgets the spans of the subtree it replaces.
            SceneListSpan span;
            const SceneNode* old{};
            SceneDisplayList list;
            SceneListSpans spans;
            std::vector<Item> items;
            // The pass's records of the rounded clips the part adds.
            std::vector<render::SceneClip> clips;
        };

        std::vector<Listing> listings;
        listings.reserve(changed.size());
        svg_.begin_pass(view_index);
        const float scale = listed.scale;
        const auto anchor_count = std::uint32_t(prepared.anchors.size());
        for (const auto& part : changed) {
            if (!part.whole) {
                if (!listed.spans.contains(part.old)) {
                    return refuse("a node without a span");
                }
                continue;
            }
            const auto found = listed.spans.find(part.old);
            if (found == listed.spans.end()) {
                return refuse("a node without a span");
            }
            if (!same_for_ancestors(*part.old, *part.now)) {
                return refuse("an anchoring or backdrop filter changed");
            }
            const auto& span = found->second;
            const auto first = span.begin[Part::Items];
            const auto count = span.end[Part::Items] - first;
            if (first == 0 && count) {
                return refuse("a part at the first item");
            }
            // The anchored element of the whole list the part lies in: the innermost listed before it whose items
            // hold its own.
            std::uint32_t enclosing{};
            for (std::uint32_t k = 0; k < span.begin[Part::Anchors]; ++k) {
                const auto& a = whole.anchors[k];
                if (a.first <= first && first + count <= a.end) {
                    enclosing = k + 1;
                }
            }
            auto& listing = listings.emplace_back();
            listing.span = span;
            listing.old = part.old;
            listing.list = scene_display_list(frame, {}, &text_, scale, &svg_, {anchors, &anchored_[view_index], true},
                                              {&listing.spans, part.now, &listing.span, &whole.list, enclosing});
            const auto& built = listing.list;
            if (!built.fallback.empty() || !built.deferred) {
                return refuse("a part the list cannot defer");
            }
            const auto sizes = scene_list_sizes(built);
            for (unsigned p = 0; p < Part::Parts; ++p) {
                const auto size = p == Part::Clips ? sizes[p] - span.chain_count : sizes[p];
                if (size != span.end[p] - span.begin[p]) {
                    return refuse("a part of other sizes");
                }
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& item = built.list.items[i];
                if (!patchable(item.kind) || item.kind != whole.list.items[first + i].kind) {
                    return refuse("a part of other kinds");
                }
            }
            if (!paint::check(built.list, limits::max_group_depth)) {
                return refuse("a part the checks refuse");
            }
            for (std::size_t c = span.chain_count; c < built.list.clips.size(); ++c) {
                auto planned = render::plan_scene_clip(built.list.clips[c]);
                if (!planned) {
                    return refuse("a rounded clip refused");
                }
                listing.clips.push_back(*planned);
            }
            // Records as the plan makes them, with the whole list's rounded clips; images, which must join the batches
            // the items they replace joined.
            listing.items.resize(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& item = built.list.items[i];
                auto planned = render::plan_scene_item(built.list, i, scale, listed.width, listed.height, std::nullopt,
                                                       render::Space::Encoded, anchor_count);
                if (!planned) {
                    return refuse("an item the plan refuses");
                }
                auto& out = listing.items[i];
                out.record = planned->record;
                out.mask = planned->mask;
                try {
                    if (item.kind == Kind::Text) {
                        auto made = image(item, built.list.rasters[item.payload], scale, view_index);
                        out.image = std::move(made.text);
                    } else if (item.kind == Kind::Shadow) {
                        out.image = shadow(item, built.list.shadows[item.payload], scale);
                    } else if (item.kind == Kind::Path && out.mask[2] > out.mask[0] && out.mask[3] > out.mask[1]) {
                        out.image = path(item, built.list.paths[item.payload], scale, out.mask);
                    } else if (item.kind == Kind::PathShadow && out.mask[2] > out.mask[0] &&
                               out.mask[3] > out.mask[1]) {
                        out.image = path_shadow(built.list, item, scale, out.mask);
                    }
                } catch (const std::invalid_argument&) {
                    return refuse("an image refused");
                }
                bool own_now{};
                bool own_before{};
                if (batch_place(out.image, own_now) != batch_place(prepared.images[first + i], own_before) ||
                    own_now != own_before || bool(out.image) != bool(prepared.images[first + i])) {
                    return refuse("an image in another batch");
                }
                if (out.image && out.image->atlas) {
                    const auto size = float(scene_atlas_page_size(out.image->format));
                    out.record.uv = {float(out.image->atlas->x) / size, float(out.image->atlas->y) / size,
                                     float(out.image->width) / size, float(out.image->height) / size};
                }
            }
        }
        // Every part fits: its values go where the elements they replace were.
        ScenePatch out;
        for (auto& listing : listings) {
            const auto& span = listing.span;
            auto& built = listing.list;
            const auto base = [&](Part p) {
                return span.begin[p];
            };
            const auto chain = span.chain_count;
            // A rounded clip index of the part in the whole list: the inherited run is whole's, the part's own follow
            // its span's.
            const auto clip_index = [&](std::uint32_t begin, std::uint32_t count) {
                if (!count) {
                    return begin;
                }
                return begin < chain ? begin + span.chain_begin : begin - chain + base(Part::Clips);
            };
            const auto first = base(Part::Items);
            for (std::size_t i = 0; i < built.list.items.size(); ++i) {
                auto item = built.list.items[i];
                if (const auto part = payload_part(item.kind); part != Part::Parts) {
                    item.payload += base(part);
                }
                item.clips_begin = clip_index(item.clips_begin, item.clips_count);
                whole.list.items[first + i] = item;
                auto& planned = listing.items[i];
                planned.record.clips[0] = float(item.clips_begin);
                prepared.pass.records[first + i] = planned.record;
                prepared.pass.masks[first + i] = planned.mask;
                prepared.images[first + i] = std::move(planned.image);
            }
            const auto copy = [&](auto& to, const auto& from, Part p) {
                std::copy(from.begin(), from.end(), to.begin() + std::ptrdiff_t(base(p)));
            };
            copy(whole.list.boxes, built.list.boxes, Part::Boxes);
            copy(whole.list.rasters, built.list.rasters, Part::Rasters);
            copy(whole.list.shadows, built.list.shadows, Part::Shadows);
            copy(whole.list.paths, built.list.paths, Part::Paths);
            for (auto& shadowed : built.list.path_shadows) {
                shadowed.first += first;
            }
            copy(whole.list.path_shadows, built.list.path_shadows, Part::PathShadows);
            for (std::size_t c = chain; c < built.list.clips.size(); ++c) {
                const auto at = base(Part::Clips) + std::uint32_t(c - chain);
                whole.list.clips[at] = built.list.clips[c];
                prepared.pass.clips[at] = listing.clips[c - chain];
            }
            for (std::size_t k = 0; k < built.anchors.size(); ++k) {
                auto a = built.anchors[k];
                a.first += first;
                a.end += first;
                whole.anchors[base(Part::Anchors) + k] = a;
                prepared.anchors[base(Part::Anchors) + k] = a;
            }
            for (std::size_t r = 0; r < built.runs.size(); ++r) {
                auto run = built.runs[r];
                for (auto& m : run.members) {
                    m.first += first;
                    m.end += first;
                    if (m.anchor) {
                        m.anchor += base(Part::Anchors);
                    }
                }
                // Its members may hold other items than before (in the batches the pass keeps).
                run.reorderable = reorderable(prepared, run);
                whole.runs[base(Part::Runs) + r] = run;
                prepared.runs[base(Part::Runs) + r] = std::move(run);
            }
            // The part's elements' spans, as the whole list's, in place of those of the subtree it replaced.
            const auto forget = [&](auto&& self, const SceneNode& n) -> void {
                listed.spans.erase(&n);
                for (const auto& c : n.children) {
                    self(self, c);
                }
            };
            forget(forget, *listing.old);
            for (auto& [node, s] : listing.spans) {
                s.chain_begin = clip_index(s.chain_begin, s.chain_count);
                for (unsigned p = 0; p < Part::Parts; ++p) {
                    if (p == Part::Clips) {
                        s.begin[p] = s.begin[p] - chain + base(Part::Clips);
                        s.end[p] = s.end[p] - chain + base(Part::Clips);
                    } else {
                        s.begin[p] += base(Part(p));
                        s.end[p] += base(Part(p));
                    }
                }
                listed.spans.insert_or_assign(node, s);
            }
            out.records.push_back({first, span.end[Part::Items]});
            if (span.end[Part::Clips] > span.begin[Part::Clips]) {
                out.clips.push_back({span.begin[Part::Clips], span.end[Part::Clips]});
            }
        }
        // The nodes on the way keep their spans under their new storage.
        for (const auto& part : changed) {
            if (!part.whole) {
                auto span = listed.spans.extract(part.old);
                span.key() = part.now;
                listed.spans.insert(std::move(span));
            }
        }
        listed.frame.root = frame.root;
        listed.frame.time_ms = frame.time_ms;
        listed.frame.animated = frame.animated;
        listed.frame.revision = frame.revision;
        return out;
    }

    void ScenePatchHistory::add(std::uint64_t from, std::uint64_t to, const ScenePatch& patch) {
        if (applied_.size() >= std::size_t(constants::render::scene::kept_patch_history)) {
            applied_.erase(applied_.begin());
        }
        applied_.push_back({from, to, patch});
    }

    bool ScenePatchHistory::since(std::uint64_t from, ScenePatch& out) const {
        const auto first = std::find_if(applied_.begin(), applied_.end(), [&](const Applied& a) {
            return a.from == from;
        });
        if (first == applied_.end()) {
            return false;
        }
        out = {};
        for (auto it = first; it != applied_.end(); ++it) {
            out.records.insert(out.records.end(), it->patch.records.begin(), it->patch.records.end());
            out.clips.insert(out.clips.end(), it->patch.clips.begin(), it->patch.clips.end());
        }
        return true;
    }

    bool ScenePreparer::place(const ScenePrepared& prepared, unsigned view_index, const SceneAnchorTable* anchors,
                              SceneAnchorFrame& frame) {
        // Each element's anchor, as the list resolves it, and its map; what they took, for missing: stay (kept once
        // the order is known: an anchor taken from what the last frame placed lives in that).
        auto& last = anchored_[view_index];
        std::vector<ScenePlacedAnchor> placed;
        placed.reserve(prepared.anchors.size());
        std::vector<const SceneAnchor*> taken(prepared.anchors.size());
        frame.anchors.resize(prepared.anchors.size());
        for (std::size_t k = 0; k < prepared.anchors.size(); ++k) {
            const auto& a = prepared.anchors[k];
            taken[k] = scene_anchor_resolve({anchors, &last}, a.element, a.anchoring, &placed);
            const auto& o = a.own;
            const auto map =
                scene_anchor_map(a.anchoring, taken[k], {a.box[0], a.box[1]}, {prepared.view[0], prepared.view[1]},
                                 {o[0], o[1], o[2], o[3], o[4], o[5]});
            // An element its anchor shrinks to nothing paints nothing, as a singular transform does in the list.
            const double determinant = map ? (*map)[0] * (*map)[0] * double(o[0] * o[3] - o[1] * o[2]) : 0;
            frame.anchors[k] = std::abs(determinant) < constants::scene::singular_determinant_epsilon
                                   ? std::array<float, 4>{1, 0, 0, 0}
                                   : std::array<float, 4>{float((*map)[0]), float((*map)[4]), float((*map)[5]), 1};
        }
        // Each run's anchored members by depth, far to near, in their places (scene_anchor_order); inner runs first, so
        // that an outer one moves its members' records in their new order.
        frame.order.clear();
        std::vector<std::size_t> places;
        std::vector<std::tuple<float, std::uint32_t, std::size_t>> sorted;
        std::vector<std::uint32_t> moved;
        for (const auto& run : prepared.runs) {
            places.clear();
            sorted.clear();
            for (std::size_t m = 0; m < run.members.size(); ++m) {
                const auto& member = run.members[m];
                if (member.anchor && taken[member.anchor - 1]) {
                    places.push_back(m);
                    sorted.emplace_back(taken[member.anchor - 1]->depth, member.child, m);
                }
            }
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) > std::get<0>(b)
                                                        : std::get<1>(a) < std::get<1>(b);
            });
            bool same = true;
            for (std::size_t k = 0; k < places.size(); ++k) {
                same = same && std::get<2>(sorted[k]) == places[k];
            }
            if (same) {
                continue;
            }
            if (!run.reorderable) {
                return false;
            }
            if (frame.order.empty()) {
                frame.order.resize(prepared.pass.records.size());
                for (std::size_t i = 0; i < frame.order.size(); ++i) {
                    frame.order[i] = std::uint32_t(i);
                }
            }
            moved.clear();
            std::size_t next{};
            for (std::size_t m = 0; m < run.members.size(); ++m) {
                const auto& member = next < places.size() && places[next] == m
                                         ? run.members[std::get<2>(sorted[next++])]
                                         : run.members[m];
                moved.insert(moved.end(), frame.order.begin() + member.first, frame.order.begin() + member.end);
            }
            std::copy(moved.begin(), moved.end(), frame.order.begin() + run.members.front().first);
        }
        if (!frame.order.empty()) {
            ++work_.reordered_passes;
        }
        scene_anchor_sort(placed);
        last = std::move(placed);
        return true;
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
            source.append(reinterpret_cast<const char*>(&raster.blur), sizeof raster.blur);
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
                // A text shadow: the coverage blurred, in device px.
                if (raster.blur > 0) {
                    std::vector<float> coverage(image->pixels.size());
                    for (std::size_t i = 0; i < coverage.size(); ++i) {
                        coverage[i] = image->pixels[i] / float(constants::color_channel_max);
                    }
                    coverage = render::gaussian_blur(coverage, image->width, image->height, raster.blur * scale);
                    for (std::size_t i = 0; i < coverage.size(); ++i) {
                        image->pixels[i] = std::uint8_t(
                            std::lround(std::clamp(coverage[i], 0.f, 1.f) * float(constants::color_channel_max)));
                    }
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
        const std::array<float, 16> key{s.x,        s.y,         s.blur,     s.spread,      s.color[0], s.color[1],
                                        s.color[2], s.color[3],  s.radii[0], s.radii[1],    s.radii[2], s.radii[3],
                                        item.width, item.height, scale,      float(s.inset)};
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

    namespace {
        // A w x h rectangle at (x, y) with circular corners (top-left first, clockwise, each at most half the shorter
        // side) as a closed subpath of p.
        void rounded_rect(paint::Path& p, float x, float y, float w, float h, std::array<float, 4> r) {
            using Verb = paint::Path::Verb;
            // A quarter circle's cubic control distance, as a fraction of the radius.
            constexpr float k = 0.5522847498f;
            for (auto& v : r) {
                v = std::clamp(v, 0.f, std::min(w, h) / 2);
            }
            const auto to = [&](Verb verb, std::initializer_list<std::array<float, 2>> points) {
                p.verbs.push_back(verb);
                p.points.insert(p.points.end(), points);
            };
            to(Verb::Move, {{x + r[0], y}});
            to(Verb::Line, {{x + w - r[1], y}});
            to(Verb::Cubic, {{x + w - r[1] + k * r[1], y}, {x + w, y + r[1] - k * r[1]}, {x + w, y + r[1]}});
            to(Verb::Line, {{x + w, y + h - r[2]}});
            to(Verb::Cubic,
               {{x + w, y + h - r[2] + k * r[2]}, {x + w - r[2] + k * r[2], y + h}, {x + w - r[2], y + h}});
            to(Verb::Line, {{x + r[3], y + h}});
            to(Verb::Cubic, {{x + r[3] - k * r[3], y + h}, {x, y + h - r[3] + k * r[3]}, {x, y + h - r[3]}});
            to(Verb::Line, {{x, y + r[0]}});
            to(Verb::Cubic, {{x, y + r[0] - k * r[0]}, {x + r[0] - k * r[0], y}, {x + r[0], y}});
            to(Verb::Close, {});
        }

        // What a box the box shader draws for an svg shape covers, as paths at their alpha: its background, filled in
        // its corners, and its border, the ring inside them.
        std::vector<std::pair<paint::PathPaint, float>> box_shapes(const paint::Box& b, float w, float h) {
            std::vector<std::pair<paint::PathPaint, float>> out;
            if (b.background[3] > 0) {
                auto fill = std::make_shared<paint::Path>();
                rounded_rect(*fill, 0, 0, w, h, b.radii);
                out.push_back({{std::move(fill), b.background, std::nullopt}, b.background[3]});
            }
            const float width = b.widths[0];
            if (width > 0 && b.edges[0][3] > 0) {
                auto ring = std::make_shared<paint::Path>();
                ring->evenodd = true;
                rounded_rect(*ring, 0, 0, w, h, b.radii);
                if (w > 2 * width && h > 2 * width) {
                    std::array<float, 4> inner{};
                    for (unsigned c = 0; c < 4; ++c) {
                        inner[c] = std::max(0.f, b.radii[c] - width);
                    }
                    rounded_rect(*ring, width, width, w - 2 * width, h - 2 * width, inner);
                }
                out.push_back({{std::move(ring), b.edges[0], std::nullopt}, b.edges[0][3]});
            }
            return out;
        }
    } // namespace

    std::shared_ptr<const SceneImage> ScenePreparer::path_shadow(const paint::DisplayList& list,
                                                                 const paint::Item& item, float scale,
                                                                 const std::array<std::int32_t, 4>& mask) {
        const auto& s = list.path_shadows[item.payload];
        const auto& m = item.transform;
        // The offset in device px: the element's linear map of it.
        const float dx = (m[0] * s.x + m[2] * s.y) * scale;
        const float dy = (m[1] * s.x + m[3] * s.y) * scale;
        // A blur in item px spreads by the map's mean scale.
        const float sigma = s.blur * scale * std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
        std::string key;
        const auto add = [&](const auto& v) {
            key.append(reinterpret_cast<const char*>(&v), sizeof v);
        };
        add(mask);
        add(m);
        add(scale);
        add(s.x);
        add(s.y);
        add(s.blur);
        add(s.content);
        std::vector<std::shared_ptr<const paint::Path>> paths;
        for (auto k = s.first; k < s.first + s.count; ++k) {
            const auto& path_item = list.items[k];
            if (path_item.kind == Kind::Box) {
                const auto& b = list.boxes[path_item.payload];
                add(path_item.transform);
                add(path_item.width);
                add(path_item.height);
                add(path_item.opacity);
                add(b.background[3]);
                add(b.radii);
                add(b.widths[0]);
                add(b.edges[0][3]);
                continue;
            }
            const auto& p = list.paths[path_item.payload];
            add(digest(p.path));
            add(p.path->verbs.size());
            add(p.path->points.size());
            add(p.path->evenodd);
            add(path_item.transform);
            add(path_item.opacity);
            add(p.color[3]);
            add(p.stroke.has_value());
            if (p.stroke) {
                add(p.stroke->width);
                add(p.stroke->cap);
                add(p.stroke->join);
                add(p.stroke->miter_limit);
                add(p.stroke->dash_offset);
                add(floats_digest(0, p.stroke->dashes.data(), p.stroke->dashes.size()));
                add(p.stroke->dashes.size());
            }
            paths.push_back(p.path);
        }
        if (auto* cached = path_shadows_.find(key)) {
            bool same = cached->paths.size() == paths.size();
            for (std::size_t k = 0; same && k < paths.size(); ++k) {
                same = cached->paths[k] == paths[k] ||
                       (cached->paths[k]->verbs == paths[k]->verbs && cached->paths[k]->points == paths[k]->points);
            }
            if (same) {
                ++work_.shadow_reuses;
                return cached->image;
            }
        }
        const auto w = unsigned(mask[2] - mask[0]);
        const auto h = unsigned(mask[3] - mask[1]);
        std::vector<float> alpha(std::size_t(w) * h);
        for (auto k = s.first; k < s.first + s.count; ++k) {
            const auto& path_item = list.items[k];
            paint::Affine device = path_item.transform;
            for (auto& v : device) {
                v *= scale;
            }
            device[4] += dx;
            device[5] += dy;
            const auto shapes = path_item.kind == Kind::Box
                                    ? box_shapes(list.boxes[path_item.payload], path_item.width, path_item.height)
                                    : std::vector<std::pair<paint::PathPaint, float>>{
                                          {list.paths[path_item.payload], list.paths[path_item.payload].color[3]}};
            for (const auto& [shape, shape_alpha] : shapes) {
                auto coverage = vector_.rasterize(shape, device, mask);
                if (!coverage) {
                    throw std::invalid_argument(coverage.error().message);
                }
                const float a = std::clamp(shape_alpha * path_item.opacity, 0.f, 1.f);
                for (std::size_t i = 0; i < alpha.size() && i < coverage->coverage.size(); ++i) {
                    const float c = coverage->coverage[i] / float(constants::color_channel_max) * a;
                    alpha[i] += c * (1 - alpha[i]);
                }
            }
        }
        // Nothing lies outside the content the paths were clipped to, where the shadow puts it.
        const double det = double(m[0]) * m[3] - double(m[1]) * m[2];
        if (std::abs(det) > 0) {
            for (unsigned row = 0; row < h; ++row) {
                for (unsigned col = 0; col < w; ++col) {
                    const double tx = (mask[0] + col + .5) / scale - m[4];
                    const double ty = (mask[1] + row + .5) / scale - m[5];
                    const double ix = (m[3] * tx - m[2] * ty) / det;
                    const double iy = (m[0] * ty - m[1] * tx) / det;
                    if (ix < s.content[0] || iy < s.content[1] || ix > s.content[0] + s.content[2] ||
                        iy > s.content[1] + s.content[3]) {
                        alpha[std::size_t(row) * w + col] = 0;
                    }
                }
            }
        }
        alpha = render::gaussian_blur(alpha, w, h, sigma);
        auto result = std::make_shared<SceneImage>();
        result->id = ++next_id_;
        result->width = w;
        result->height = h;
        result->format = SceneImage::Format::Data;
        result->pixels.resize(alpha.size());
        for (std::size_t i = 0; i < alpha.size(); ++i) {
            result->pixels[i] =
                std::uint8_t(std::lround(std::clamp(alpha[i], 0.f, 1.f) * float(constants::color_channel_max)));
        }
        if (result->pixels.size() > limits::image_max_bytes) {
            throw std::invalid_argument("path shadow image byte bound");
        }
        const auto bytes = result->pixels.size();
        ++work_.shadow_builds;
        place(result);
        const auto evictions = path_shadows_.evictions();
        path_shadows_.put(key, {std::move(paths), result}, bytes);
        work_.image_evictions += path_shadows_.evictions() - evictions;
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
                                                          unsigned view_index, std::uint32_t anchors, bool over) {
        // Scene passes sample no asset images, only host textures by name.
        if (std::any_of(list.items.begin(), list.items.end(), [&](const paint::Item& item) {
                return item.kind == Kind::Image && !list.images[item.payload].texture;
            })) {
            throw std::invalid_argument("scene renderer draws no asset images");
        }
        auto planned =
            render::plan_scene_pass(list, scale, width, height, crop, render::Space::Encoded, nullptr, anchors, !over);
        if (!planned) {
            throw std::invalid_argument(planned.error().message);
        }
        auto out = std::make_unique<ScenePrepared>();
        out->pass = std::move(*planned);
        out->over = over;
        out->width = width;
        out->height = height;
        out->images.resize(list.items.size());
        out->planes.resize(list.items.size());
        out->layers.resize(list.items.size());
        out->textures.resize(list.items.size());
        out->texture_names = list.textures;
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
            } else if (item.kind == Kind::Image) {
                out->textures[i] = list.images[item.payload].texture;
            } else if (item.kind == Kind::Shadow) {
                out->images[i] = shadow(item, list.shadows[item.payload], scale);
            } else if (item.kind == Kind::Path) {
                const auto& mask = out->pass.masks[i];
                if (mask[2] > mask[0] && mask[3] > mask[1]) {
                    out->images[i] = path(item, list.paths[item.payload], scale, mask);
                }
            } else if (item.kind == Kind::PathShadow) {
                const auto& mask = out->pass.masks[i];
                if (mask[2] > mask[0] && mask[3] > mask[1]) {
                    out->images[i] = path_shadow(list, item, scale, mask);
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
