#include "weft/render_external_texture.h"
#include "base/generational_slots.hpp"
#include "base/owner_thread.hpp"
#include "constants/render.hpp"
#include "constants/scene.hpp"
#include "render/paint_list.hpp"
#include "render/rounded_clip.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    std::atomic<uint64_t> next_id{1};
    constexpr uint32_t prepared_kind = 1;
    constexpr uint32_t use_kind = 2;
    constexpr uint32_t texture_kind = 3;

    // Prepared frame or encoded use. Occupancy and generation live in the slot table; the
    // texture list keeps its reserved capacity across occupancies.
    struct Slot {
        uint64_t native{};
        uint64_t serial{};
        bool submitted{};
        std::vector<uint32_t> textures;
    };

    // Imported external texture; occupied while the backend still holds the native texture.
    struct TextureSlot {
        weft_render_external_texture description{};
        uint64_t native{};
        uint64_t bytes{};
        uint32_t pins{};
        bool caller{};
    };

    using Slots = weft::GenerationalSlots<Slot>;
    using TextureSlots = weft::GenerationalSlots<TextureSlot>;

    bool unit(float x) {
        return std::isfinite(x) && x >= 0 && x <= 1;
    }

    bool color(const float* rgba) {
        return std::all_of(rgba, rgba + 4, unit);
    }

    bool finite(const float* values, std::size_t count) {
        return std::all_of(values, values + count, [](float v) {
            return std::isfinite(v);
        });
    }

    // A backend table that ends before prepare_list has none.
    constexpr std::size_t backend_without_lists = offsetof(weft_render_backend, prepare_list);
    // A View frame description that ends before textures binds none (its first version).
    constexpr std::size_t scene_frame_without_textures = offsetof(weft_render_scene_frame, textures);

    // A texture name of a View's page: letters, digits, '-' and '_', at most max_texture_name_bytes.
    bool texture_name(const char* name, uint32_t size) {
        return name && size && size <= uint32_t(weft::constants::scene::max_texture_name_bytes) &&
               std::all_of(name, name + size, [](char ch) {
                   return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                          ch == '-' || ch == '_';
               });
    }
} // namespace

struct weft_renderer {
    weft::OwnerThread owner;
    weft_render_backend backend{};
    weft_renderer_desc limits{};
    weft_render_identity identity{};
    // Fixed capacity allocated at creation.
    Slots prepared;
    Slots uses;
    weft_render_external_backend external{};
    TextureSlots textures;
    std::vector<weft_render_external_asset> external_scratch;
    uint32_t caller_leases{};
    uint32_t live_textures{};
    uint64_t texture_bytes{};
    uint32_t prepared_count{};
    uint32_t encoded_count{};
    uint32_t submitted_count{};
    uint64_t last_submitted{};
    uint64_t completed{};
    bool draining{};
    bool in_callback{};
    bool device_available{true};
};

namespace {
    weft_render_result owner(weft_renderer* r, bool producing = false) {
        if (!r) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (!r->owner.is_current()) {
            return WEFT_RENDER_WRONG_THREAD;
        }
        if (r->in_callback) {
            return WEFT_RENDER_BUSY;
        }
        if (producing && r->draining) {
            return WEFT_RENDER_SHUTTING_DOWN;
        }
        if (producing && !r->device_available) {
            return WEFT_RENDER_DEVICE_UNAVAILABLE;
        }
        return WEFT_RENDER_OK;
    }

    struct Callback {
        weft_renderer& r;

        Callback(weft_renderer& x) : r(x) {
            r.in_callback = true;
        }

        ~Callback() {
            r.in_callback = false;
        }
    };

    void collect_texture(weft_renderer& r, std::size_t index) {
        auto& t = r.textures[index];
        if (!r.textures.occupied(index) || t.caller || t.pins) {
            return;
        }
        {
            Callback guard(r);
            r.external.release_texture(r.external.user_data, t.native);
            if (t.description.retired) {
                t.description.retired(t.description.retirement_user_data, r.device_available
                                                                              ? WEFT_RENDER_EXTERNAL_RETIRED
                                                                              : WEFT_RENDER_EXTERNAL_ABANDONED);
            }
        }
        r.texture_bytes -= t.bytes;
        --r.live_textures;
        t.native = 0;
        t.description = {};
        t.bytes = 0;
        r.textures.vacate(index);
    }

    void release_textures(weft_renderer& r, Slot& s) {
        for (auto i : s.textures) {
            --r.textures[i].pins;
            collect_texture(r, i);
        }
        s.textures.clear();
    }

    weft_render_result texture_handle(weft_renderer& r, weft_render_handle h, TextureSlot*& out) {
        if (h.renderer_id != r.identity.renderer_id) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        if (h.device_epoch != r.identity.device_epoch) {
            return WEFT_RENDER_WRONG_EPOCH;
        }
        if (h.kind != texture_kind || h.reserved) {
            return WEFT_RENDER_STALE_HANDLE;
        }
        auto* t = r.textures.find(h.slot, h.generation);
        if (!t || !t->caller) {
            return WEFT_RENDER_STALE_HANDLE;
        }
        out = t;
        return WEFT_RENDER_OK;
    }

    weft_render_result handle(weft_renderer& r, weft_render_handle h, uint32_t kind, Slot*& out) {
        if (h.renderer_id != r.identity.renderer_id) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        if (h.device_epoch != r.identity.device_epoch) {
            return WEFT_RENDER_WRONG_EPOCH;
        }
        auto& slots = kind == prepared_kind ? r.prepared : r.uses;
        if (h.kind != kind || h.reserved) {
            return WEFT_RENDER_STALE_HANDLE;
        }
        out = slots.find(h.slot, h.generation);
        return out ? WEFT_RENDER_OK : WEFT_RENDER_STALE_HANDLE;
    }

    weft_render_handle publish(weft_renderer& r, Slots& slots, size_t i, uint64_t native, uint32_t kind) {
        const auto generation = slots.occupy(i);
        auto& s = slots[i];
        s.native = native;
        s.serial = 0;
        s.submitted = false;
        return {r.identity.renderer_id, r.identity.device_epoch, uint32_t(i), generation, kind, 0};
    }

    // Ends an occupancy whose native object the backend has already released or retired.
    void retire(Slots& slots, size_t i) {
        slots[i].native = 0;
        slots.vacate(i);
    }

    weft_render_result validate_frame(const weft_renderer& r, const weft_render_frame* f, uint32_t external_count = 0) {
        if (!f || f->struct_size < sizeof(*f)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (f->abi_version != WEFT_RENDER_ABI_VERSION) {
            return WEFT_RENDER_UNSUPPORTED_ABI;
        }
        if ((f->draw_count && !f->draws) || (f->asset_count && !f->assets)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (f->draw_count > r.limits.max_draws || f->asset_count > r.limits.max_assets ||
            external_count > r.limits.max_assets - f->asset_count) {
            return WEFT_RENDER_BUSY;
        }
        uint64_t total{};
        const auto& caps = r.backend.capabilities;
        for (uint32_t i = 0; i < f->asset_count; ++i) {
            const auto& a = f->assets[i];
            if (!a.asset_id || !a.asset_version || !a.width || !a.height || !a.bytes || a.reserved || a.reserved2 ||
                a.encoding > WEFT_RENDER_SIGNED_DISTANCE) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            if (a.width > caps.max_texture_size || a.height > caps.max_texture_size) {
                return WEFT_RENDER_UNSUPPORTED_FEATURE;
            }
            const uint64_t pixels = uint64_t(a.width) * a.height;
            const uint64_t channels =
                a.encoding == WEFT_RENDER_SRGB_STRAIGHT ? weft::constants::rgba8_bytes_per_pixel : 1;
            if (pixels > UINT64_MAX / channels || a.byte_count != pixels * channels) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            if (a.byte_count > r.limits.max_frame_asset_bytes - total) {
                return WEFT_RENDER_BUSY;
            }
            total += a.byte_count;
            if (a.encoding == WEFT_RENDER_SIGNED_DISTANCE) {
                if (!std::isfinite(a.sdf_spread) || a.sdf_spread < weft::constants::render::min_sdf_spread ||
                    a.sdf_spread > weft::constants::render::max_sdf_spread) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            } else if (a.sdf_spread != 0) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
        }
        for (uint32_t i = 0; i < f->draw_count; ++i) {
            const auto& d = f->draws[i];
            if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.width) || !std::isfinite(d.height) ||
                d.width < 0 || d.height < 0 || !color(d.srgb) || !color(d.outline_srgb) || !unit(d.opacity) ||
                !std::isfinite(d.sdf_stroke) || d.sdf_stroke < 0) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            for (float v : d.uv) {
                if (!unit(v)) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
            if (d.uv[0] + d.uv[2] > weft::constants::render::max_uv_extent ||
                d.uv[1] + d.uv[3] > weft::constants::render::max_uv_extent) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            uint32_t feature = WEFT_RENDER_FEATURE_SOLID;
            if (d.asset_index != WEFT_RENDER_SOLID_ASSET) {
                if (d.asset_index >= f->asset_count + external_count) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
                if (d.asset_index >= f->asset_count) {
                    if (d.sdf_stroke != 0) {
                        return WEFT_RENDER_INVALID_ARGUMENT;
                    }
                    continue; /* external capability validated by the extension */
                }
                const auto& a = f->assets[d.asset_index];
                feature = a.encoding == WEFT_RENDER_COVERAGE          ? WEFT_RENDER_FEATURE_COVERAGE
                          : a.encoding == WEFT_RENDER_SIGNED_DISTANCE ? WEFT_RENDER_FEATURE_SDF
                                                                      : WEFT_RENDER_FEATURE_IMAGE;
                if (a.encoding == WEFT_RENDER_SIGNED_DISTANCE) {
                    if (d.sdf_stroke >= a.sdf_spread - 1) {
                        return WEFT_RENDER_INVALID_ARGUMENT;
                    }
                } else if (d.sdf_stroke != 0) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            } else if (d.sdf_stroke != 0) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            if (!(caps.features & feature)) {
                return WEFT_RENDER_UNSUPPORTED_FEATURE;
            }
        }
        return WEFT_RENDER_OK;
    }

    // An asset of a display list: what validate_frame requires of a frame's assets.
    weft_render_result validate_asset(const weft_renderer& r, const weft::paint::Asset& a, uint64_t& total) {
        const auto& caps = r.backend.capabilities;
        const auto encoding = static_cast<uint32_t>(a.encoding);
        if (!a.width || !a.height || encoding > WEFT_RENDER_SIGNED_DISTANCE) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (a.width > caps.max_texture_size || a.height > caps.max_texture_size) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        const uint64_t channels = encoding == WEFT_RENDER_SRGB_STRAIGHT ? weft::constants::rgba8_bytes_per_pixel : 1;
        if (a.bytes.size() != uint64_t(a.width) * a.height * channels) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (a.bytes.size() > r.limits.max_frame_asset_bytes - total) {
            return WEFT_RENDER_BUSY;
        }
        total += a.bytes.size();
        if (encoding == WEFT_RENDER_SIGNED_DISTANCE) {
            if (!std::isfinite(a.spread) || a.spread < weft::constants::render::min_sdf_spread ||
                a.spread > weft::constants::render::max_sdf_spread) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
        } else if (a.spread != 0) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        return WEFT_RENDER_OK;
    }

    // A box: unit colors, finite non-negative geometry and a gradient the shared shader can draw.
    bool valid_box(const weft::paint::Box& b) {
        using weft::paint::GradientKind;
        const auto& g = b.gradient;
        bool valid = color(b.background.data()) && finite(b.radii.data(), b.radii.size()) &&
                     finite(b.widths.data(), b.widths.size()) && finite(g.tile.data(), g.tile.size()) &&
                     std::isfinite(g.angle) && g.kind <= GradientKind::Conic &&
                     g.stops.size() <= weft::constants::render::scene::max_gradient_stops &&
                     (g.kind == GradientKind::None || !g.stops.empty());
        for (unsigned i = 0; valid && i < b.radii.size(); ++i) {
            valid = b.radii[i] >= 0 && b.widths[i] >= 0 && color(b.edges[i].data());
        }
        // A radial gradient's ending shape has radii; a repeating gradient's stops span a distance and may lie outside
        // [0, 1].
        valid = valid && finite(g.shape.data(), g.shape.size()) &&
                (g.kind != GradientKind::Radial || (g.shape[2] > 0 && g.shape[3] > 0)) &&
                (!g.repeating || (g.kind != GradientKind::None && g.kind != GradientKind::Conic && g.stops.size() > 1 &&
                                  g.stops.back().offset > g.stops.front().offset));
        for (const auto& stop : g.stops) {
            valid = valid && (g.repeating ? std::isfinite(stop.offset) : unit(stop.offset)) && color(stop.color.data());
        }
        return valid;
    }

    // A display list frame: a well-formed list of boxes, images and isolated groups within the renderer's limits,
    // with valid assets and exactly external_count external textures. Scene rasters do not cross the ABI, and a
    // rounded clip chain holds at most max_rounded_clips.
    weft_render_result validate_list(const weft_renderer& r, const weft_paint_list* p, uint32_t external_count) {
        const auto& caps = r.backend.capabilities;
        if (!(caps.features & WEFT_RENDER_FEATURE_DISPLAY_LIST)) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (!p || !p->list) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& list = *p->list;
        if (!weft::paint::check(list, weft::constants::render::scene::max_group_depth) ||
            list.external_textures != external_count) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        // Named host textures are a View's (weft_renderer_prepare_scene).
        if (!list.textures.empty()) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        if (list.items.size() > r.limits.max_draws || list.assets.size() > r.limits.max_assets ||
            external_count > r.limits.max_assets - list.assets.size()) {
            return WEFT_RENDER_BUSY;
        }
        uint64_t total{};
        for (const auto& a : list.assets) {
            if (const auto v = validate_asset(r, *a, total); v != WEFT_RENDER_OK) {
                return v;
            }
        }
        for (const auto& c : list.clips) {
            try {
                (void)weft::normalize_rounded(c);
            } catch (const std::invalid_argument&) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
        }
        using Kind = weft::paint::Item::Kind;
        for (const auto& item : list.items) {
            const auto& m = item.transform;
            if (!finite(m.data(), m.size()) || !std::isfinite(item.width) || !std::isfinite(item.height) ||
                item.width < 0 || item.height < 0 || !unit(item.opacity) ||
                std::any_of(
                    item.clip.begin(), item.clip.end(),
                    [](float v) {
                        return std::isnan(v);
                    })) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            uint32_t feature = WEFT_RENDER_FEATURE_SOLID;
            // Effects, particles and backdrops come to the C ABI with feature bits of their own.
            if (item.kind == Kind::Text || item.kind == Kind::Layer || item.kind == Kind::Shadow ||
                item.kind == Kind::Path || item.kind == Kind::Plane || item.kind == Kind::Effect ||
                item.kind == Kind::Particles || item.kind == Kind::Backdrop ||
                item.clips_count > uint32_t(weft::constants::render::scene::max_rounded_clips)) {
                return WEFT_RENDER_UNSUPPORTED_FEATURE;
            }
            if (item.kind == Kind::BeginGroup || item.kind == Kind::EndGroup) {
                continue;
            }
            // Boxes and images map item space through an invertible transform.
            if (!std::isnormal(m[0] * m[3] - m[1] * m[2])) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            if (item.kind == Kind::Box) {
                if (!valid_box(list.boxes[item.payload])) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            } else {
                const auto& image = list.images[item.payload];
                if (!color(image.color.data()) || !color(image.outline.data()) ||
                    !finite(image.uv.data(), image.uv.size()) || !std::isfinite(image.stroke) || image.stroke < 0) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
                if (image.asset >= list.assets.size()) {
                    continue; /* external capability validated by the extension */
                }
                const auto& a = *list.assets[image.asset];
                feature = a.encoding == weft::paint::Encoding::Coverage         ? WEFT_RENDER_FEATURE_COVERAGE
                          : a.encoding == weft::paint::Encoding::SignedDistance ? WEFT_RENDER_FEATURE_SDF
                                                                                : WEFT_RENDER_FEATURE_IMAGE;
                if (a.encoding == weft::paint::Encoding::SignedDistance ? image.stroke >= a.spread - 1
                                                                        : image.stroke != 0) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
            if (!(caps.features & feature)) {
                return WEFT_RENDER_UNSUPPORTED_FEATURE;
            }
        }
        return WEFT_RENDER_OK;
    }

    // Pins the caller leases of a frame's external textures (handle(i), i < count) for prepared slot index and lists
    // their metadata in r.external_scratch, whose capacity covers every frame that passed validation. Fails without
    // pins.
    template <class Handle>
    weft_render_result pin_external(weft_renderer& r, std::size_t index, uint32_t count, Handle handle) {
        auto& deps = r.prepared[index].textures;
        auto& scratch = r.external_scratch;
        deps.clear();
        scratch.clear();
        for (uint32_t i = 0; i < count; ++i) {
            TextureSlot* t{};
            if (const auto v = texture_handle(r, handle(i), t); v != WEFT_RENDER_OK) {
                deps.clear();
                return v;
            }
            auto description = t->description;
            description.retired = nullptr;
            description.retirement_user_data = nullptr;
            scratch.push_back({description, t->native});
            const auto slot = handle(i).slot;
            if (std::find(deps.begin(), deps.end(), slot) == deps.end()) {
                deps.push_back(slot);
            }
        }
        for (auto i : deps) {
            ++r.textures[i].pins;
        }
        return WEFT_RENDER_OK;
    }

    weft_render_result validate_pass(const weft_renderer& r, const weft_render_pass* p) {
        if (!p || p->struct_size < sizeof(*p)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (p->abi_version != WEFT_RENDER_ABI_VERSION) {
            return WEFT_RENDER_UNSUPPORTED_ABI;
        }
        const auto& id = r.identity;
        if (p->identity.renderer_id != id.renderer_id || p->identity.api != id.api) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        if (p->identity.device_epoch != id.device_epoch) {
            return WEFT_RENDER_WRONG_EPOCH;
        }
        if (p->identity.queue != id.queue || p->identity.reserved) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (!p->width || !p->height || !p->native_pass || !p->native_pass_size) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& c = r.backend.capabilities;
        if (p->width > c.max_target_size || p->height > c.max_target_size || p->sample_count != 1 ||
            p->target_format > WEFT_RENDER_TARGET_LINEAR_RGBA16F || !(c.target_formats & (1u << p->target_format))) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        return WEFT_RENDER_OK;
    }
} // namespace

extern "C" {
weft_render_result weft_renderer_create(const weft_render_backend* b, const weft_renderer_desc* desc,
                                        weft_renderer** out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (!b || b->struct_size < backend_without_lists) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (b->abi_version != WEFT_RENDER_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    // A table from before display lists ends at shutdown.
    weft_render_backend table{};
    std::copy_n(reinterpret_cast<const unsigned char*>(b), std::min<std::size_t>(b->struct_size, sizeof(table)),
                reinterpret_cast<unsigned char*>(&table));
    table.struct_size = sizeof(table);
    const auto& c = table.capabilities;
    const bool lists = c.features & WEFT_RENDER_FEATURE_DISPLAY_LIST;
    const bool scenes = c.features & WEFT_RENDER_FEATURE_SCENE;
    if (!table.api || table.reserved || !table.device_epoch || !table.queue || c.struct_size < sizeof(c) ||
        c.reserved || !c.max_texture_size || !c.max_target_size ||
        (c.features & ~weft::constants::render::known_feature_bits) || !c.target_formats ||
        (c.target_formats & ~weft::constants::render::known_target_format_bits) || !table.prepare ||
        !table.release_prepared || !table.encode || !table.discard_encoded || !table.submitted || !table.completed ||
        !table.abandon || !table.shutdown || lists != (table.prepare_list != nullptr) ||
        scenes != (table.prepare_scene != nullptr)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    weft_renderer_desc limits{sizeof(weft_renderer_desc),
                              WEFT_RENDER_ABI_VERSION,
                              weft::constants::render::default_max_prepared_frames,
                              weft::constants::render::default_max_encoded_uses,
                              weft::constants::render::default_max_in_flight_uses,
                              weft::constants::render::default_max_draws,
                              weft::constants::render::default_max_assets,
                              0,
                              weft::constants::render::default_max_frame_asset_bytes};
    if (desc) {
        if (desc->struct_size < sizeof(*desc)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (desc->abi_version != WEFT_RENDER_ABI_VERSION) {
            return WEFT_RENDER_UNSUPPORTED_ABI;
        }
        limits = *desc;
    }
    if (limits.reserved || !limits.max_prepared_frames ||
        limits.max_prepared_frames > weft::constants::render::max_prepared_frames_limit || !limits.max_encoded_uses ||
        limits.max_encoded_uses > limits.max_in_flight_uses ||
        limits.max_in_flight_uses > weft::constants::render::max_in_flight_uses_limit || !limits.max_draws ||
        !limits.max_assets || !limits.max_frame_asset_bytes || limits.max_frame_asset_bytes > uint64_t(SIZE_MAX)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    try {
        auto* r = new weft_renderer;
        try {
            r->backend = table;
            r->limits = limits;
            r->prepared = Slots(limits.max_prepared_frames);
            r->uses = Slots(limits.max_in_flight_uses);
            uint64_t id = next_id.load(std::memory_order_relaxed);
            for (;;) {
                if (id == UINT64_MAX) {
                    delete r;
                    return WEFT_RENDER_BUSY;
                }
                if (next_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
                    break;
                }
            }
            r->identity = {id, table.device_epoch, table.queue, table.api, 0};
            *out = r;
            return WEFT_RENDER_OK;
        } catch (...) {
            delete r;
            throw;
        }
    } catch (const std::bad_alloc&) {
        return WEFT_RENDER_OUT_OF_MEMORY;
    } catch (...) {
        return WEFT_RENDER_BACKEND_ERROR;
    }
}

weft_render_result weft_renderer_destroy(weft_renderer* r) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    r->draining = true;
    if (r->prepared_count || r->encoded_count || r->submitted_count || r->live_textures) {
        return WEFT_RENDER_BUSY;
    }
    weft_render_result result;
    {
        Callback guard(*r);
        result = r->backend.shutdown(r->backend.user_data);
    }
    if (result != WEFT_RENDER_OK) {
        return result;
    }
    delete r;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_get_info(weft_renderer* r, weft_render_info* out) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!out || out->struct_size < sizeof(*out)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {sizeof(*out),      uint32_t(r->draining), r->identity,        r->backend.capabilities,
            r->prepared_count, r->encoded_count,      r->submitted_count, uint32_t(r->device_available),
            r->last_submitted, r->completed};
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_prepare(weft_renderer* r, const weft_render_frame* frame,
                                         weft_render_handle* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    if (const auto v = validate_frame(*r, frame); v != WEFT_RENDER_OK) {
        return v;
    }
    const auto i = r->prepared.first_free();
    if (i == r->prepared.size()) {
        return WEFT_RENDER_BUSY;
    }
    uint64_t native{};
    weft_render_result v;
    {
        Callback guard(*r);
        v = r->backend.prepare(r->backend.user_data, frame, &native);
    }
    if (v != WEFT_RENDER_OK) {
        return v;
    }
    if (!native) {
        return WEFT_RENDER_BACKEND_ERROR;
    }
    *out = publish(*r, r->prepared, i, native, prepared_kind);
    ++r->prepared_count;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_prepare_list(weft_renderer* r, const weft_render_list_frame* f,
                                              weft_render_handle* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!f || f->struct_size < sizeof(*f) || f->reserved || (f->external_count && !f->external_textures)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (f->abi_version != WEFT_RENDER_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    try {
        if (const auto v = validate_list(*r, f->list, f->external_count); v != WEFT_RENDER_OK) {
            return v;
        }
    } catch (const std::bad_alloc&) {
        return WEFT_RENDER_OUT_OF_MEMORY;
    } catch (...) {
        return WEFT_RENDER_BACKEND_ERROR;
    }
    if (f->external_count && !r->external.struct_size) {
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }
    if (f->external_count > r->textures.size()) {
        return WEFT_RENDER_BUSY;
    }
    const auto index = r->prepared.first_free();
    if (index == r->prepared.size()) {
        return WEFT_RENDER_BUSY;
    }
    if (const auto v = pin_external(*r, index, f->external_count,
                                    [&](uint32_t i) {
                                        return f->external_textures[i];
                                    });
        v != WEFT_RENDER_OK) {
        return v;
    }
    uint64_t native{};
    weft_render_result v;
    {
        Callback guard(*r);
        v = r->backend.prepare_list(r->backend.user_data, f->list, r->external_scratch.data(), f->external_count,
                                    &native);
    }
    if (v != WEFT_RENDER_OK || !native) {
        release_textures(*r, r->prepared[index]);
        return v == WEFT_RENDER_OK ? WEFT_RENDER_BACKEND_ERROR : v;
    }
    *out = publish(*r, r->prepared, index, native, prepared_kind);
    ++r->prepared_count;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_prepare_scene(weft_renderer* r, const weft_render_scene_frame* f,
                                               weft_render_handle* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!f || f->struct_size < scene_frame_without_textures || !f->frame || !std::isfinite(f->scale) || f->scale < 0) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    const uint32_t count = f->texture_count;
    if (count && (f->struct_size < sizeof(*f) || !f->textures)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    // Each name once, of the names a page can give.
    for (uint32_t k = 0; k < count; ++k) {
        const auto& t = f->textures[k];
        if (t.reserved || !texture_name(t.name, t.name_size)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        for (uint32_t j = 0; j < k; ++j) {
            if (f->textures[j].name_size == t.name_size &&
                std::equal(t.name, t.name + t.name_size, f->textures[j].name)) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
        }
    }
    if (f->abi_version != WEFT_RENDER_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    if (!r->backend.prepare_scene || (count && !r->external.struct_size)) {
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }
    if (count > r->external_scratch.capacity()) {
        return WEFT_RENDER_BUSY;
    }
    const auto i = r->prepared.first_free();
    if (i == r->prepared.size()) {
        return WEFT_RENDER_BUSY;
    }
    if (const auto v = pin_external(*r, i, count,
                                    [&](uint32_t k) {
                                        return f->textures[k].texture;
                                    });
        v != WEFT_RENDER_OK) {
        return v;
    }
    uint64_t native{};
    weft_render_result v;
    {
        Callback guard(*r);
        v = r->backend.prepare_scene(r->backend.user_data, f, r->external_scratch.data(), count, &native);
    }
    if (v != WEFT_RENDER_OK || !native) {
        release_textures(*r, r->prepared[i]);
        return v == WEFT_RENDER_OK ? WEFT_RENDER_BACKEND_ERROR : v;
    }
    *out = publish(*r, r->prepared, i, native, prepared_kind);
    ++r->prepared_count;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_release_prepared(weft_renderer* r, weft_render_handle h) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    Slot* s{};
    if (const auto v = handle(*r, h, prepared_kind, s); v != WEFT_RENDER_OK) {
        return v;
    }
    {
        Callback guard(*r);
        r->backend.release_prepared(r->backend.user_data, s->native);
    }
    retire(r->prepared, h.slot);
    --r->prepared_count;
    release_textures(*r, *s);
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_encode(weft_renderer* r, weft_render_handle h, const weft_render_pass* pass,
                                        weft_render_handle* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    Slot* frame{};
    if (const auto v = handle(*r, h, prepared_kind, frame); v != WEFT_RENDER_OK) {
        return v;
    }
    if (const auto v = validate_pass(*r, pass); v != WEFT_RENDER_OK) {
        return v;
    }
    if (r->encoded_count == r->limits.max_encoded_uses) {
        return WEFT_RENDER_BUSY;
    }
    const auto i = r->uses.first_free();
    if (i == r->uses.size()) {
        return WEFT_RENDER_BUSY;
    }
    auto& use = r->uses[i];
    use.textures.assign(frame->textures.begin(), frame->textures.end());
    for (auto t : use.textures) {
        ++r->textures[t].pins;
    }
    uint64_t native{};
    weft_render_result v;
    {
        Callback guard(*r);
        v = r->backend.encode(r->backend.user_data, frame->native, pass, &native);
    }
    if (native) {
        *out = publish(*r, r->uses, i, native, use_kind);
        ++r->encoded_count;
    } else {
        release_textures(*r, use);
        if (v == WEFT_RENDER_OK) {
            return WEFT_RENDER_BACKEND_ERROR;
        }
    }
    return v;
}

weft_render_result weft_renderer_discard_encoded(weft_renderer* r, weft_render_handle h) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    Slot* s{};
    if (const auto v = handle(*r, h, use_kind, s); v != WEFT_RENDER_OK) {
        return v;
    }
    if (s->submitted) {
        return WEFT_RENDER_INVALID_STATE;
    }
    {
        Callback guard(*r);
        r->backend.discard_encoded(r->backend.user_data, s->native);
    }
    retire(r->uses, h.slot);
    --r->encoded_count;
    release_textures(*r, *s);
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_notify_submitted(weft_renderer* r, weft_render_handle h, uint64_t queue,
                                                  uint64_t serial) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!r->device_available) {
        return WEFT_RENDER_DEVICE_UNAVAILABLE;
    }
    Slot* s{};
    if (const auto v = handle(*r, h, use_kind, s); v != WEFT_RENDER_OK) {
        return v;
    }
    if (s->submitted) {
        return WEFT_RENDER_INVALID_STATE;
    }
    if (queue != r->identity.queue) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (!serial || serial < r->last_submitted || serial <= r->completed) {
        return WEFT_RENDER_INVALID_SERIAL;
    }
    {
        Callback guard(*r);
        r->backend.submitted(r->backend.user_data, s->native, serial);
    }
    s->submitted = true;
    s->serial = serial;
    --r->encoded_count;
    ++r->submitted_count;
    r->last_submitted = serial;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_notify_completed(weft_renderer* r, uint64_t epoch, uint64_t queue,
                                                  uint64_t serial) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!r->device_available) {
        return WEFT_RENDER_DEVICE_UNAVAILABLE;
    }
    if (epoch != r->identity.device_epoch) {
        return WEFT_RENDER_WRONG_EPOCH;
    }
    if (queue != r->identity.queue) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (!serial || serial < r->completed) {
        return WEFT_RENDER_INVALID_SERIAL;
    }
    {
        Callback guard(*r);
        r->backend.completed(r->backend.user_data, serial);
    }
    r->completed = serial;
    for (size_t i = 0; i < r->uses.size(); ++i) {
        auto& s = r->uses[i];
        if (r->uses.occupied(i) && s.submitted && s.serial <= serial) {
            retire(r->uses, i);
            --r->submitted_count;
            release_textures(*r, s);
        }
    }
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_abandon_device(weft_renderer* r, uint64_t epoch, uint32_t confirmation) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    if (epoch != r->identity.device_epoch) {
        return WEFT_RENDER_WRONG_EPOCH;
    }
    if (confirmation != WEFT_RENDER_HOST_CONFIRMED_NO_FURTHER_ACCESS) {
        return WEFT_RENDER_HOST_CONFIRMATION_REQUIRED;
    }
    if (!r->device_available) {
        return WEFT_RENDER_DEVICE_UNAVAILABLE;
    }
    if (r->encoded_count) {
        return WEFT_RENDER_BUSY;
    }
    {
        Callback guard(*r);
        r->backend.abandon(r->backend.user_data);
    }
    r->device_available = false;
    for (size_t i = 0; i < r->uses.size(); ++i) {
        retire(r->uses, i);
        release_textures(*r, r->uses[i]);
    }
    r->submitted_count = 0;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_install_external_backend(weft_renderer* r,
                                                          const weft_render_external_backend* b) noexcept {
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!b || b->struct_size < sizeof(*b)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (b->abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    // Views whose sampling returns linear values: linear 8-bit, half and float RGBA, and 8-bit RGBA and BGRA, linear or
    // sRGB (the view decodes it).
    constexpr uint32_t formats = (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) |
                                 (1u << WEFT_RENDER_TARGET_LINEAR_BGRA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                                 (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F) | (1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F);
    if (b->reserved || b->reserved2 || !b->formats || (b->formats & ~formats) || !b->max_textures ||
        b->max_textures > weft::constants::render::max_external_textures_limit || !b->max_texture_bytes ||
        !b->import_texture || !b->release_texture || !b->prepare) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (r->external.struct_size || r->prepared_count || r->encoded_count || r->submitted_count) {
        return WEFT_RENDER_INVALID_STATE;
    }
    try {
        TextureSlots textures(b->max_textures);
        std::vector<weft_render_external_asset> scratch;
        scratch.reserve(std::min(b->max_textures, r->limits.max_assets));
        for (size_t i = 0; i < r->prepared.size(); ++i) {
            r->prepared[i].textures.reserve(scratch.capacity());
        }
        for (size_t i = 0; i < r->uses.size(); ++i) {
            r->uses[i].textures.reserve(scratch.capacity());
        }
        r->textures = std::move(textures);
        r->external_scratch = std::move(scratch);
        r->external = *b;
        return WEFT_RENDER_OK;
    } catch (const std::bad_alloc&) {
        return WEFT_RENDER_OUT_OF_MEMORY;
    } catch (...) {
        return WEFT_RENDER_BACKEND_ERROR;
    }
}

weft_render_result weft_renderer_import_texture(weft_renderer* r, const weft_render_external_texture* d,
                                                weft_render_handle* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!r->external.struct_size) {
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }
    if (!d || d->struct_size < sizeof(*d)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (d->abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    if (d->identity.renderer_id != r->identity.renderer_id || d->identity.api != r->identity.api) {
        return WEFT_RENDER_WRONG_BACKEND;
    }
    if (d->identity.device_epoch != r->identity.device_epoch) {
        return WEFT_RENDER_WRONG_EPOCH;
    }
    if (d->identity.reserved || d->identity.queue != r->identity.queue || !d->content_id || !d->content_version ||
        !d->width || !d->height || d->reserved2 || !d->native_texture || !d->native_texture_size) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    // At most a full chain: one level more than halvings of the larger side until a texel.
    const uint32_t levels = std::max(1u, d->mip_levels);
    if (levels > uint32_t(std::bit_width(std::max(d->width, d->height)))) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (d->width > r->backend.capabilities.max_texture_size || d->height > r->backend.capabilities.max_texture_size ||
        d->encoding != WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED || d->origin > WEFT_RENDER_EXTERNAL_BOTTOM_LEFT ||
        d->synchronization > WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE || d->format > WEFT_RENDER_TARGET_LINEAR_RGBA16F ||
        !(r->external.formats & (1u << d->format))) {
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }
    // The texels of every level, each half the one before on each side down to one.
    uint64_t pixels{};
    for (uint32_t level = 0; level < levels; ++level) {
        pixels += uint64_t(std::max(1u, d->width >> level)) * std::max(1u, d->height >> level);
    }
    const uint64_t bpp =
        d->format == WEFT_RENDER_TARGET_LINEAR_RGBA16F   ? weft::constants::render::rgba16f_bytes_per_pixel
        : d->format == WEFT_RENDER_TARGET_LINEAR_RGBA32F ? weft::constants::render::rgba32f_bytes_per_pixel
                                                         : weft::constants::rgba8_bytes_per_pixel;
    if (pixels > UINT64_MAX / bpp) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    const auto bytes = pixels * bpp;
    if (bytes > r->external.max_texture_bytes - r->texture_bytes) {
        return WEFT_RENDER_BUSY;
    }
    size_t index = r->textures.size();
    for (size_t i = 0; i < r->textures.size(); ++i) {
        const auto& t = r->textures[i];
        if (r->textures.occupied(i) && t.description.content_id == d->content_id &&
            t.description.content_version == d->content_version) {
            return WEFT_RENDER_INVALID_STATE;
        }
        if (r->textures.reusable(i) && index == r->textures.size()) {
            index = i;
        }
    }
    if (index == r->textures.size()) {
        return WEFT_RENDER_BUSY;
    }
    uint64_t native{};
    weft_render_result v;
    {
        Callback guard(*r);
        v = r->external.import_texture(r->external.user_data, d, &native);
    }
    if (v != WEFT_RENDER_OK) {
        return v;
    }
    if (!native) {
        return WEFT_RENDER_BACKEND_ERROR;
    }
    auto& t = r->textures[index];
    t.description = *d;
    t.description.native_texture = nullptr;
    t.description.native_texture_size = 0;
    t.native = native;
    t.bytes = bytes;
    t.caller = true;
    const auto generation = r->textures.occupy(index);
    ++r->caller_leases;
    ++r->live_textures;
    r->texture_bytes += bytes;
    *out = {r->identity.renderer_id, r->identity.device_epoch, uint32_t(index), generation, texture_kind, 0};
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_release_texture(weft_renderer* r, weft_render_handle h) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    TextureSlot* t{};
    if (const auto v = texture_handle(*r, h, t); v != WEFT_RENDER_OK) {
        return v;
    }
    t->caller = false;
    --r->caller_leases;
    collect_texture(*r, h.slot);
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_prepare_external(weft_renderer* r, const weft_render_external_frame* f,
                                                  weft_render_handle* out) noexcept {
    if (!out) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!r->external.struct_size) {
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }
    if (!f || f->struct_size < sizeof(*f) || f->reserved || (f->external_count && !f->external_textures)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (f->abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION) {
        return WEFT_RENDER_UNSUPPORTED_ABI;
    }
    if (const auto v = validate_frame(*r, &f->frame, f->external_count); v != WEFT_RENDER_OK) {
        return v;
    }
    if (f->external_count > r->textures.size()) {
        return WEFT_RENDER_BUSY;
    }
    const auto index = r->prepared.first_free();
    if (index == r->prepared.size()) {
        return WEFT_RENDER_BUSY;
    }
    if (const auto v = pin_external(*r, index, f->external_count,
                                    [&](uint32_t i) {
                                        return f->external_textures[i];
                                    });
        v != WEFT_RENDER_OK) {
        return v;
    }
    uint64_t native{};
    weft_render_result v;
    {
        Callback guard(*r);
        v = r->external.prepare(r->external.user_data, &f->frame, r->external_scratch.data(), f->external_count,
                                &native);
    }
    if (v != WEFT_RENDER_OK || !native) {
        release_textures(*r, r->prepared[index]);
        return v == WEFT_RENDER_OK ? WEFT_RENDER_BACKEND_ERROR : v;
    }
    *out = publish(*r, r->prepared, index, native, prepared_kind);
    ++r->prepared_count;
    return WEFT_RENDER_OK;
}

weft_render_result weft_renderer_get_external_info(weft_renderer* r, weft_render_external_info* out) noexcept {
    if (const auto v = owner(r); v != WEFT_RENDER_OK) {
        return v;
    }
    if (!out || out->struct_size < sizeof(*out)) {
        return WEFT_RENDER_INVALID_ARGUMENT;
    }
    if (!r->external.struct_size) {
        return WEFT_RENDER_UNSUPPORTED_FEATURE;
    }
    *out = {sizeof(*out), r->caller_leases, r->live_textures, 0, r->texture_bytes};
    return WEFT_RENDER_OK;
}
}
