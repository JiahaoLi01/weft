#include "caseelephant/render_external_texture.h"
#include "caseelephant/base/generational_slots.hpp"
#include "caseelephant/base/owner_thread.hpp"
#include "caseelephant/constants/render.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
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
        ce_render_external_texture description{};
        uint64_t native{};
        uint64_t bytes{};
        uint32_t pins{};
        bool caller{};
    };

    using Slots = caseelephant::GenerationalSlots<Slot>;
    using TextureSlots = caseelephant::GenerationalSlots<TextureSlot>;

    bool unit(float x) {
        return std::isfinite(x) && x >= 0 && x <= 1;
    }

    bool color(const float* rgba) {
        return std::all_of(rgba, rgba + 4, unit);
    }
} // namespace

struct ce_renderer {
    caseelephant::OwnerThread owner;
    ce_render_backend backend{};
    ce_renderer_desc limits{};
    ce_render_identity identity{};
    // Fixed capacity allocated at creation.
    Slots prepared;
    Slots uses;
    ce_render_external_backend external{};
    TextureSlots textures;
    std::vector<ce_render_external_asset> external_scratch;
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
    ce_render_result owner(ce_renderer* r, bool producing = false) {
        if (!r) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (!r->owner.is_current()) {
            return CE_RENDER_WRONG_THREAD;
        }
        if (r->in_callback) {
            return CE_RENDER_BUSY;
        }
        if (producing && r->draining) {
            return CE_RENDER_SHUTTING_DOWN;
        }
        if (producing && !r->device_available) {
            return CE_RENDER_DEVICE_UNAVAILABLE;
        }
        return CE_RENDER_OK;
    }

    struct Callback {
        ce_renderer& r;

        Callback(ce_renderer& x) : r(x) {
            r.in_callback = true;
        }

        ~Callback() {
            r.in_callback = false;
        }
    };

    void collect_texture(ce_renderer& r, std::size_t index) {
        auto& t = r.textures[index];
        if (!r.textures.occupied(index) || t.caller || t.pins) {
            return;
        }
        {
            Callback guard(r);
            r.external.release_texture(r.external.user_data, t.native);
            if (t.description.retired) {
                t.description.retired(t.description.retirement_user_data,
                                      r.device_available ? CE_RENDER_EXTERNAL_RETIRED : CE_RENDER_EXTERNAL_ABANDONED);
            }
        }
        r.texture_bytes -= t.bytes;
        --r.live_textures;
        t.native = 0;
        t.description = {};
        t.bytes = 0;
        r.textures.vacate(index);
    }

    void release_textures(ce_renderer& r, Slot& s) {
        for (auto i : s.textures) {
            --r.textures[i].pins;
            collect_texture(r, i);
        }
        s.textures.clear();
    }

    ce_render_result texture_handle(ce_renderer& r, ce_render_handle h, TextureSlot*& out) {
        if (h.renderer_id != r.identity.renderer_id) {
            return CE_RENDER_WRONG_BACKEND;
        }
        if (h.device_epoch != r.identity.device_epoch) {
            return CE_RENDER_WRONG_EPOCH;
        }
        if (h.kind != texture_kind || h.reserved) {
            return CE_RENDER_STALE_HANDLE;
        }
        auto* t = r.textures.find(h.slot, h.generation);
        if (!t || !t->caller) {
            return CE_RENDER_STALE_HANDLE;
        }
        out = t;
        return CE_RENDER_OK;
    }

    ce_render_result handle(ce_renderer& r, ce_render_handle h, uint32_t kind, Slot*& out) {
        if (h.renderer_id != r.identity.renderer_id) {
            return CE_RENDER_WRONG_BACKEND;
        }
        if (h.device_epoch != r.identity.device_epoch) {
            return CE_RENDER_WRONG_EPOCH;
        }
        auto& slots = kind == prepared_kind ? r.prepared : r.uses;
        if (h.kind != kind || h.reserved) {
            return CE_RENDER_STALE_HANDLE;
        }
        out = slots.find(h.slot, h.generation);
        return out ? CE_RENDER_OK : CE_RENDER_STALE_HANDLE;
    }

    ce_render_handle publish(ce_renderer& r, Slots& slots, size_t i, uint64_t native, uint32_t kind) {
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

    ce_render_result validate_frame(const ce_renderer& r, const ce_render_frame* f, uint32_t external_count = 0) {
        if (!f || f->struct_size < sizeof(*f)) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (f->abi_version != CE_RENDER_ABI_VERSION) {
            return CE_RENDER_UNSUPPORTED_ABI;
        }
        if ((f->draw_count && !f->draws) || (f->asset_count && !f->assets)) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (f->draw_count > r.limits.max_draws || f->asset_count > r.limits.max_assets ||
            external_count > r.limits.max_assets - f->asset_count) {
            return CE_RENDER_BUSY;
        }
        uint64_t total{};
        const auto& caps = r.backend.capabilities;
        for (uint32_t i = 0; i < f->asset_count; ++i) {
            const auto& a = f->assets[i];
            if (!a.asset_id || !a.asset_version || !a.width || !a.height || !a.bytes || a.reserved || a.reserved2 ||
                a.encoding > CE_RENDER_SIGNED_DISTANCE) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            if (a.width > caps.max_texture_size || a.height > caps.max_texture_size) {
                return CE_RENDER_UNSUPPORTED_FEATURE;
            }
            const uint64_t pixels = uint64_t(a.width) * a.height;
            const uint64_t channels =
                a.encoding == CE_RENDER_SRGB_STRAIGHT ? caseelephant::constants::rgba8_bytes_per_pixel : 1;
            if (pixels > UINT64_MAX / channels || a.byte_count != pixels * channels) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            if (a.byte_count > r.limits.max_frame_asset_bytes - total) {
                return CE_RENDER_BUSY;
            }
            total += a.byte_count;
            if (a.encoding == CE_RENDER_SIGNED_DISTANCE) {
                if (!std::isfinite(a.sdf_spread) || a.sdf_spread < caseelephant::constants::render::min_sdf_spread ||
                    a.sdf_spread > caseelephant::constants::render::max_sdf_spread) {
                    return CE_RENDER_INVALID_ARGUMENT;
                }
            } else if (a.sdf_spread != 0) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
        }
        for (uint32_t i = 0; i < f->draw_count; ++i) {
            const auto& d = f->draws[i];
            if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.width) || !std::isfinite(d.height) ||
                d.width < 0 || d.height < 0 || !color(d.srgb) || !color(d.outline_srgb) || !unit(d.opacity) ||
                !std::isfinite(d.sdf_stroke) || d.sdf_stroke < 0) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            for (float v : d.uv) {
                if (!unit(v)) {
                    return CE_RENDER_INVALID_ARGUMENT;
                }
            }
            if (d.uv[0] + d.uv[2] > caseelephant::constants::render::max_uv_extent ||
                d.uv[1] + d.uv[3] > caseelephant::constants::render::max_uv_extent) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            uint32_t feature = CE_RENDER_FEATURE_SOLID;
            if (d.asset_index != CE_RENDER_SOLID_ASSET) {
                if (d.asset_index >= f->asset_count + external_count) {
                    return CE_RENDER_INVALID_ARGUMENT;
                }
                if (d.asset_index >= f->asset_count) {
                    if (d.sdf_stroke != 0) {
                        return CE_RENDER_INVALID_ARGUMENT;
                    }
                    continue; /* external capability validated by the extension */
                }
                const auto& a = f->assets[d.asset_index];
                feature = a.encoding == CE_RENDER_COVERAGE          ? CE_RENDER_FEATURE_COVERAGE
                          : a.encoding == CE_RENDER_SIGNED_DISTANCE ? CE_RENDER_FEATURE_SDF
                                                                    : CE_RENDER_FEATURE_IMAGE;
                if (a.encoding == CE_RENDER_SIGNED_DISTANCE) {
                    if (d.sdf_stroke >= a.sdf_spread - 1) {
                        return CE_RENDER_INVALID_ARGUMENT;
                    }
                } else if (d.sdf_stroke != 0) {
                    return CE_RENDER_INVALID_ARGUMENT;
                }
            } else if (d.sdf_stroke != 0) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            if (!(caps.features & feature)) {
                return CE_RENDER_UNSUPPORTED_FEATURE;
            }
        }
        return CE_RENDER_OK;
    }

    ce_render_result validate_pass(const ce_renderer& r, const ce_render_pass* p) {
        if (!p || p->struct_size < sizeof(*p)) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (p->abi_version != CE_RENDER_ABI_VERSION) {
            return CE_RENDER_UNSUPPORTED_ABI;
        }
        const auto& id = r.identity;
        if (p->identity.renderer_id != id.renderer_id || p->identity.api != id.api) {
            return CE_RENDER_WRONG_BACKEND;
        }
        if (p->identity.device_epoch != id.device_epoch) {
            return CE_RENDER_WRONG_EPOCH;
        }
        if (p->identity.queue != id.queue || p->identity.reserved) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (!p->width || !p->height || !p->native_pass || !p->native_pass_size) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        const auto& c = r.backend.capabilities;
        if (p->width > c.max_target_size || p->height > c.max_target_size || p->sample_count != 1 ||
            p->target_format > CE_RENDER_TARGET_LINEAR_RGBA16F || !(c.target_formats & (1u << p->target_format))) {
            return CE_RENDER_UNSUPPORTED_FEATURE;
        }
        return CE_RENDER_OK;
    }
} // namespace

extern "C" {
ce_render_result ce_renderer_create(const ce_render_backend* b, const ce_renderer_desc* desc,
                                    ce_renderer** out) noexcept {
    if (!out) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    *out = nullptr;
    if (!b || b->struct_size < sizeof(*b)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (b->abi_version != CE_RENDER_ABI_VERSION) {
        return CE_RENDER_UNSUPPORTED_ABI;
    }
    const auto& c = b->capabilities;
    if (!b->api || b->reserved || !b->device_epoch || !b->queue || c.struct_size < sizeof(c) || c.reserved ||
        !c.max_texture_size || !c.max_target_size ||
        (c.features & ~caseelephant::constants::render::known_feature_bits) || !c.target_formats ||
        (c.target_formats & ~caseelephant::constants::render::known_target_format_bits) || !b->prepare ||
        !b->release_prepared || !b->encode || !b->discard_encoded || !b->submitted || !b->completed || !b->abandon ||
        !b->shutdown) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    ce_renderer_desc limits{sizeof(ce_renderer_desc),
                            CE_RENDER_ABI_VERSION,
                            caseelephant::constants::render::default_max_prepared_frames,
                            caseelephant::constants::render::default_max_encoded_uses,
                            caseelephant::constants::render::default_max_in_flight_uses,
                            caseelephant::constants::render::default_max_draws,
                            caseelephant::constants::render::default_max_assets,
                            0,
                            caseelephant::constants::render::default_max_frame_asset_bytes};
    if (desc) {
        if (desc->struct_size < sizeof(*desc)) {
            return CE_RENDER_INVALID_ARGUMENT;
        }
        if (desc->abi_version != CE_RENDER_ABI_VERSION) {
            return CE_RENDER_UNSUPPORTED_ABI;
        }
        limits = *desc;
    }
    if (limits.reserved || !limits.max_prepared_frames ||
        limits.max_prepared_frames > caseelephant::constants::render::max_prepared_frames_limit ||
        !limits.max_encoded_uses || limits.max_encoded_uses > limits.max_in_flight_uses ||
        limits.max_in_flight_uses > caseelephant::constants::render::max_in_flight_uses_limit || !limits.max_draws ||
        !limits.max_assets || !limits.max_frame_asset_bytes || limits.max_frame_asset_bytes > uint64_t(SIZE_MAX)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    try {
        auto* r = new ce_renderer;
        try {
            r->backend = *b;
            r->limits = limits;
            r->prepared = Slots(limits.max_prepared_frames);
            r->uses = Slots(limits.max_in_flight_uses);
            uint64_t id = next_id.load(std::memory_order_relaxed);
            for (;;) {
                if (id == UINT64_MAX) {
                    delete r;
                    return CE_RENDER_BUSY;
                }
                if (next_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
                    break;
                }
            }
            r->identity = {id, b->device_epoch, b->queue, b->api, 0};
            *out = r;
            return CE_RENDER_OK;
        } catch (...) {
            delete r;
            throw;
        }
    } catch (const std::bad_alloc&) {
        return CE_RENDER_OUT_OF_MEMORY;
    } catch (...) {
        return CE_RENDER_BACKEND_ERROR;
    }
}

ce_render_result ce_renderer_destroy(ce_renderer* r) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    r->draining = true;
    if (r->prepared_count || r->encoded_count || r->submitted_count || r->live_textures) {
        return CE_RENDER_BUSY;
    }
    ce_render_result result;
    {
        Callback guard(*r);
        result = r->backend.shutdown(r->backend.user_data);
    }
    if (result != CE_RENDER_OK) {
        return result;
    }
    delete r;
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_get_info(ce_renderer* r, ce_render_info* out) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    if (!out || out->struct_size < sizeof(*out)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    *out = {sizeof(*out),      uint32_t(r->draining), r->identity,        r->backend.capabilities,
            r->prepared_count, r->encoded_count,      r->submitted_count, uint32_t(r->device_available),
            r->last_submitted, r->completed};
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_prepare(ce_renderer* r, const ce_render_frame* frame, ce_render_handle* out) noexcept {
    if (!out) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != CE_RENDER_OK) {
        return v;
    }
    if (const auto v = validate_frame(*r, frame); v != CE_RENDER_OK) {
        return v;
    }
    const auto i = r->prepared.first_free();
    if (i == r->prepared.size()) {
        return CE_RENDER_BUSY;
    }
    uint64_t native{};
    ce_render_result v;
    {
        Callback guard(*r);
        v = r->backend.prepare(r->backend.user_data, frame, &native);
    }
    if (v != CE_RENDER_OK) {
        return v;
    }
    if (!native) {
        return CE_RENDER_BACKEND_ERROR;
    }
    *out = publish(*r, r->prepared, i, native, prepared_kind);
    ++r->prepared_count;
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_release_prepared(ce_renderer* r, ce_render_handle h) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    Slot* s{};
    if (const auto v = handle(*r, h, prepared_kind, s); v != CE_RENDER_OK) {
        return v;
    }
    {
        Callback guard(*r);
        r->backend.release_prepared(r->backend.user_data, s->native);
    }
    retire(r->prepared, h.slot);
    --r->prepared_count;
    release_textures(*r, *s);
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_encode(ce_renderer* r, ce_render_handle h, const ce_render_pass* pass,
                                    ce_render_handle* out) noexcept {
    if (!out) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != CE_RENDER_OK) {
        return v;
    }
    Slot* frame{};
    if (const auto v = handle(*r, h, prepared_kind, frame); v != CE_RENDER_OK) {
        return v;
    }
    if (const auto v = validate_pass(*r, pass); v != CE_RENDER_OK) {
        return v;
    }
    if (r->encoded_count == r->limits.max_encoded_uses) {
        return CE_RENDER_BUSY;
    }
    const auto i = r->uses.first_free();
    if (i == r->uses.size()) {
        return CE_RENDER_BUSY;
    }
    auto& use = r->uses[i];
    use.textures.assign(frame->textures.begin(), frame->textures.end());
    for (auto t : use.textures) {
        ++r->textures[t].pins;
    }
    uint64_t native{};
    ce_render_result v;
    {
        Callback guard(*r);
        v = r->backend.encode(r->backend.user_data, frame->native, pass, &native);
    }
    if (native) {
        *out = publish(*r, r->uses, i, native, use_kind);
        ++r->encoded_count;
    } else {
        release_textures(*r, use);
        if (v == CE_RENDER_OK) {
            return CE_RENDER_BACKEND_ERROR;
        }
    }
    return v;
}

ce_render_result ce_renderer_discard_encoded(ce_renderer* r, ce_render_handle h) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    Slot* s{};
    if (const auto v = handle(*r, h, use_kind, s); v != CE_RENDER_OK) {
        return v;
    }
    if (s->submitted) {
        return CE_RENDER_INVALID_STATE;
    }
    {
        Callback guard(*r);
        r->backend.discard_encoded(r->backend.user_data, s->native);
    }
    retire(r->uses, h.slot);
    --r->encoded_count;
    release_textures(*r, *s);
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_notify_submitted(ce_renderer* r, ce_render_handle h, uint64_t queue,
                                              uint64_t serial) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    if (!r->device_available) {
        return CE_RENDER_DEVICE_UNAVAILABLE;
    }
    Slot* s{};
    if (const auto v = handle(*r, h, use_kind, s); v != CE_RENDER_OK) {
        return v;
    }
    if (s->submitted) {
        return CE_RENDER_INVALID_STATE;
    }
    if (queue != r->identity.queue) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (!serial || serial < r->last_submitted || serial <= r->completed) {
        return CE_RENDER_INVALID_SERIAL;
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
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_notify_completed(ce_renderer* r, uint64_t epoch, uint64_t queue,
                                              uint64_t serial) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    if (!r->device_available) {
        return CE_RENDER_DEVICE_UNAVAILABLE;
    }
    if (epoch != r->identity.device_epoch) {
        return CE_RENDER_WRONG_EPOCH;
    }
    if (queue != r->identity.queue) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (!serial || serial < r->completed) {
        return CE_RENDER_INVALID_SERIAL;
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
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_abandon_device(ce_renderer* r, uint64_t epoch, uint32_t confirmation) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    if (epoch != r->identity.device_epoch) {
        return CE_RENDER_WRONG_EPOCH;
    }
    if (confirmation != CE_RENDER_HOST_CONFIRMED_NO_FURTHER_ACCESS) {
        return CE_RENDER_HOST_CONFIRMATION_REQUIRED;
    }
    if (!r->device_available) {
        return CE_RENDER_DEVICE_UNAVAILABLE;
    }
    if (r->encoded_count) {
        return CE_RENDER_BUSY;
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
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_install_external_backend(ce_renderer* r, const ce_render_external_backend* b) noexcept {
    if (const auto v = owner(r, true); v != CE_RENDER_OK) {
        return v;
    }
    if (!b || b->struct_size < sizeof(*b)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (b->abi_version != CE_RENDER_EXTERNAL_ABI_VERSION) {
        return CE_RENDER_UNSUPPORTED_ABI;
    }
    constexpr uint32_t formats = (1u << CE_RENDER_TARGET_LINEAR_RGBA8) | (1u << CE_RENDER_TARGET_LINEAR_RGBA16F) |
                                 (1u << CE_RENDER_TARGET_LINEAR_RGBA32F);
    if (b->reserved || b->reserved2 || !b->formats || (b->formats & ~formats) || !b->max_textures ||
        b->max_textures > caseelephant::constants::render::max_external_textures_limit || !b->max_texture_bytes ||
        !b->import_texture || !b->release_texture || !b->prepare) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (r->external.struct_size || r->prepared_count || r->encoded_count || r->submitted_count) {
        return CE_RENDER_INVALID_STATE;
    }
    try {
        TextureSlots textures(b->max_textures);
        std::vector<ce_render_external_asset> scratch;
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
        return CE_RENDER_OK;
    } catch (const std::bad_alloc&) {
        return CE_RENDER_OUT_OF_MEMORY;
    } catch (...) {
        return CE_RENDER_BACKEND_ERROR;
    }
}

ce_render_result ce_renderer_import_texture(ce_renderer* r, const ce_render_external_texture* d,
                                            ce_render_handle* out) noexcept {
    if (!out) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != CE_RENDER_OK) {
        return v;
    }
    if (!r->external.struct_size) {
        return CE_RENDER_UNSUPPORTED_FEATURE;
    }
    if (!d || d->struct_size < sizeof(*d)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (d->abi_version != CE_RENDER_EXTERNAL_ABI_VERSION) {
        return CE_RENDER_UNSUPPORTED_ABI;
    }
    if (d->identity.renderer_id != r->identity.renderer_id || d->identity.api != r->identity.api) {
        return CE_RENDER_WRONG_BACKEND;
    }
    if (d->identity.device_epoch != r->identity.device_epoch) {
        return CE_RENDER_WRONG_EPOCH;
    }
    if (d->identity.reserved || d->identity.queue != r->identity.queue || !d->content_id || !d->content_version ||
        !d->width || !d->height || d->reserved || d->reserved2 || !d->native_texture || !d->native_texture_size) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (d->width > r->backend.capabilities.max_texture_size || d->height > r->backend.capabilities.max_texture_size ||
        d->encoding != CE_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED || d->origin > CE_RENDER_EXTERNAL_BOTTOM_LEFT ||
        d->synchronization != CE_RENDER_EXTERNAL_SAME_QUEUE_ORDERED || d->format > CE_RENDER_TARGET_LINEAR_RGBA16F ||
        !(r->external.formats & (1u << d->format))) {
        return CE_RENDER_UNSUPPORTED_FEATURE;
    }
    const uint64_t pixels = uint64_t(d->width) * d->height;
    const uint64_t bpp = d->format == CE_RENDER_TARGET_LINEAR_RGBA8 ? caseelephant::constants::rgba8_bytes_per_pixel
                         : d->format == CE_RENDER_TARGET_LINEAR_RGBA16F
                             ? caseelephant::constants::render::rgba16f_bytes_per_pixel
                             : caseelephant::constants::render::rgba32f_bytes_per_pixel;
    if (pixels > UINT64_MAX / bpp) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    const auto bytes = pixels * bpp;
    if (bytes > r->external.max_texture_bytes - r->texture_bytes) {
        return CE_RENDER_BUSY;
    }
    size_t index = r->textures.size();
    for (size_t i = 0; i < r->textures.size(); ++i) {
        const auto& t = r->textures[i];
        if (r->textures.occupied(i) && t.description.content_id == d->content_id &&
            t.description.content_version == d->content_version) {
            return CE_RENDER_INVALID_STATE;
        }
        if (r->textures.reusable(i) && index == r->textures.size()) {
            index = i;
        }
    }
    if (index == r->textures.size()) {
        return CE_RENDER_BUSY;
    }
    uint64_t native{};
    ce_render_result v;
    {
        Callback guard(*r);
        v = r->external.import_texture(r->external.user_data, d, &native);
    }
    if (v != CE_RENDER_OK) {
        return v;
    }
    if (!native) {
        return CE_RENDER_BACKEND_ERROR;
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
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_release_texture(ce_renderer* r, ce_render_handle h) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    TextureSlot* t{};
    if (const auto v = texture_handle(*r, h, t); v != CE_RENDER_OK) {
        return v;
    }
    t->caller = false;
    --r->caller_leases;
    collect_texture(*r, h.slot);
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_prepare_external(ce_renderer* r, const ce_render_external_frame* f,
                                              ce_render_handle* out) noexcept {
    if (!out) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    *out = {};
    if (const auto v = owner(r, true); v != CE_RENDER_OK) {
        return v;
    }
    if (!r->external.struct_size) {
        return CE_RENDER_UNSUPPORTED_FEATURE;
    }
    if (!f || f->struct_size < sizeof(*f) || f->reserved || (f->external_count && !f->external_textures)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (f->abi_version != CE_RENDER_EXTERNAL_ABI_VERSION) {
        return CE_RENDER_UNSUPPORTED_ABI;
    }
    if (const auto v = validate_frame(*r, &f->frame, f->external_count); v != CE_RENDER_OK) {
        return v;
    }
    if (f->external_count > r->textures.size()) {
        return CE_RENDER_BUSY;
    }
    const auto index = r->prepared.first_free();
    if (index == r->prepared.size()) {
        return CE_RENDER_BUSY;
    }
    auto& deps = r->prepared[index].textures;
    auto& scratch = r->external_scratch;
    deps.clear();
    scratch.clear();
    for (uint32_t i = 0; i < f->external_count; ++i) {
        TextureSlot* t{};
        if (const auto v = texture_handle(*r, f->external_textures[i], t); v != CE_RENDER_OK) {
            deps.clear();
            return v;
        }
        auto description = t->description;
        description.retired = nullptr;
        description.retirement_user_data = nullptr;
        scratch.push_back({description, t->native});
        const auto slot = f->external_textures[i].slot;
        if (std::find(deps.begin(), deps.end(), slot) == deps.end()) {
            deps.push_back(slot);
        }
    }
    for (auto i : deps) {
        ++r->textures[i].pins;
    }
    uint64_t native{};
    ce_render_result v;
    {
        Callback guard(*r);
        v = r->external.prepare(r->external.user_data, &f->frame, scratch.data(), f->external_count, &native);
    }
    if (v != CE_RENDER_OK || !native) {
        release_textures(*r, r->prepared[index]);
        return v == CE_RENDER_OK ? CE_RENDER_BACKEND_ERROR : v;
    }
    *out = publish(*r, r->prepared, index, native, prepared_kind);
    ++r->prepared_count;
    return CE_RENDER_OK;
}

ce_render_result ce_renderer_get_external_info(ce_renderer* r, ce_render_external_info* out) noexcept {
    if (const auto v = owner(r); v != CE_RENDER_OK) {
        return v;
    }
    if (!out || out->struct_size < sizeof(*out)) {
        return CE_RENDER_INVALID_ARGUMENT;
    }
    if (!r->external.struct_size) {
        return CE_RENDER_UNSUPPORTED_FEATURE;
    }
    *out = {sizeof(*out), r->caller_leases, r->live_textures, 0, r->texture_bytes};
    return CE_RENDER_OK;
}
}
