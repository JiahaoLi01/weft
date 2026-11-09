#include "weft/render_export_texture.h"
#include "weft/render_external_texture.h"
#include "constants/common.hpp"
#include "constants/render.hpp"
#include "render/paint_list.hpp"
#include "support/check.hpp"
#include "test_constants/render.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    std::atomic<bool> deny_allocation{};
} // namespace

void* operator new(std::size_t n) {
    if (deny_allocation.load()) {
        throw std::bad_alloc();
    }
    if (auto* p = std::malloc(n ? n : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {
#define CHECK(v) WEFT_REQUIRE(v)
#define EXPECT(v, result) CHECK((v) == WEFT_RENDER_##result)

    struct Scope {
        uint64_t schema{weft::test_constants::render::fake_pass_schema};
        bool commands{};
        bool cancelled{};
        bool submitted{};
    };

    struct FakeBackend {
        struct Use {
            std::shared_ptr<std::vector<uint8_t>> resource;
            Scope* scope{};
            uint64_t serial{};
        };

        std::array<std::shared_ptr<std::vector<uint8_t>>, 2> frames;
        std::array<Use, 4> uses;
        weft_renderer* renderer{};
        // The display list of the last prepared list frame and the number of list frames prepared.
        std::shared_ptr<const weft::paint::DisplayList> list;
        unsigned list_preparations{};
        unsigned preparations{};
        unsigned encodes{};
        unsigned retirements{};
        unsigned discards{};
        bool partial_failure{};
        bool atomic_failure{};
        bool prepare_failure{};
        bool reentrancy{};

        static weft_render_result prepare(void* ptr, const weft_render_frame* f, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            *out = 0;
            ++s.preparations;
            if (s.prepare_failure) {
                return WEFT_RENDER_BACKEND_ERROR;
            }
            try {
                for (size_t i = 0; i < s.frames.size(); ++i) {
                    if (!s.frames[i]) {
                        auto data = std::make_shared<std::vector<uint8_t>>();
                        if (f->asset_count) {
                            data->assign(f->assets[0].bytes, f->assets[0].bytes + f->assets[0].byte_count);
                        }
                        s.frames[i] = std::move(data);
                        *out = i + 1;
                        return WEFT_RENDER_OK;
                    }
                }
                return WEFT_RENDER_BUSY;
            } catch (...) {
                return WEFT_RENDER_OUT_OF_MEMORY;
            }
        }

        static weft_render_result prepare_list(void* ptr, const weft_paint_list* l, const weft_render_external_asset*,
                                               uint32_t, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            *out = 0;
            ++s.list_preparations;
            try {
                for (size_t i = 0; i < s.frames.size(); ++i) {
                    if (!s.frames[i]) {
                        s.frames[i] = std::make_shared<std::vector<uint8_t>>();
                        s.list = l->list;
                        *out = i + 1;
                        return WEFT_RENDER_OK;
                    }
                }
                return WEFT_RENDER_BUSY;
            } catch (...) {
                return WEFT_RENDER_OUT_OF_MEMORY;
            }
        }

        static void release(void* ptr, uint64_t id) noexcept {
            static_cast<FakeBackend*>(ptr)->frames[id - 1].reset();
        }

        static weft_render_result encode(void* ptr, uint64_t frame, const weft_render_pass* p, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            *out = 0;
            ++s.encodes;
            if (p->native_pass_size != sizeof(Scope)) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            auto& scope = *static_cast<Scope*>(const_cast<void*>(p->native_pass));
            if (scope.schema != weft::test_constants::render::fake_pass_schema || scope.commands) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            weft_render_info info{};
            info.struct_size = sizeof(info);
            s.reentrancy = weft_renderer_get_info(s.renderer, &info) == WEFT_RENDER_BUSY;
            if (s.atomic_failure) {
                return WEFT_RENDER_BACKEND_ERROR;
            }
            for (size_t i = 0; i < s.uses.size(); ++i) {
                if (!s.uses[i].resource) {
                    s.uses[i] = {s.frames[frame - 1], &scope, 0};
                    scope.commands = true;
                    *out = i + 1;
                    return s.partial_failure ? WEFT_RENDER_BACKEND_ERROR : WEFT_RENDER_OK;
                }
            }
            return WEFT_RENDER_BUSY;
        }

        static void discard(void* ptr, uint64_t id) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            auto& use = s.uses[id - 1];
            if (!use.scope->cancelled) {
                std::terminate();
            }
            use = {};
            ++s.discards;
        }

        static void submitted(void* ptr, uint64_t id, uint64_t serial) noexcept {
            auto& use = static_cast<FakeBackend*>(ptr)->uses[id - 1];
            if (!use.scope->submitted) {
                std::terminate();
            }
            use.serial = serial;
        }

        static void completed(void* ptr, uint64_t serial) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            for (auto& use : s.uses) {
                if (use.resource && use.serial && use.serial <= serial) {
                    use = {};
                    ++s.retirements;
                }
            }
        }

        static weft_render_result shutdown(void* ptr) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            for (const auto& f : s.frames) {
                if (f) {
                    return WEFT_RENDER_BUSY;
                }
            }
            for (const auto& u : s.uses) {
                if (u.resource) {
                    return WEFT_RENDER_BUSY;
                }
            }
            return WEFT_RENDER_OK;
        }

        static void abandon(void* ptr) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            for (auto& use : s.uses) {
                use = {};
            }
        }

        weft_render_backend table(uint32_t api = WEFT_RENDER_API_METAL) {
            return {sizeof(weft_render_backend),
                    WEFT_RENDER_ABI_VERSION,
                    api,
                    0,
                    7,
                    11,
                    {sizeof(weft_render_capabilities), WEFT_RENDER_FEATURE_SOLID | WEFT_RENDER_FEATURE_COVERAGE,
                     (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8), 64, 128, 0},
                    this,
                    prepare,
                    release,
                    encode,
                    discard,
                    submitted,
                    completed,
                    abandon,
                    shutdown};
        }
    };

    struct Frame {
        uint8_t pixel{128};
        weft_render_asset asset{1, 1, 1, 1, WEFT_RENDER_COVERAGE, 0, &pixel, 1, 0, 0};
        weft_render_draw draw{0, 0, 16, 16, {1, 1, 1, 1}, 0, 1, {0, 0, 1, 1}, 0, {0, 0, 0, 0}};
        weft_render_frame view{sizeof(weft_render_frame), WEFT_RENDER_ABI_VERSION, &draw, 1, 1, &asset};
    };

    weft_render_info info(weft_renderer* r) {
        weft_render_info out{};
        out.struct_size = sizeof(out);
        EXPECT(weft_renderer_get_info(r, &out), OK);
        return out;
    }

    weft_render_pass pass(weft_renderer* r, Scope& scope) {
        return {sizeof(weft_render_pass),
                WEFT_RENDER_ABI_VERSION,
                info(r).identity,
                64,
                64,
                WEFT_RENDER_TARGET_SRGB_RGBA8,
                1,
                &scope,
                sizeof(scope)};
    }

    weft_renderer* create(FakeBackend& b, uint32_t api = WEFT_RENDER_API_METAL) {
        const auto table = b.table(api);
        weft_renderer* r{};
        EXPECT(weft_renderer_create(&table, nullptr, &r), OK);
        b.renderer = r;
        return r;
    }

    void IndependentBackendsAndSnapshotOwnership() {
        FakeBackend a;
        FakeBackend b;
        auto* metal = create(a);
        auto* custom = create(b, WEFT_RENDER_API_CUSTOM_BEGIN + 77);
        Frame f;
        weft_render_handle prepared{};
        EXPECT(weft_renderer_prepare(metal, &f.view, &prepared), OK);
        f.pixel = 0;
        Scope scope;
        auto p = pass(metal, scope);
        weft_render_handle use{};
        EXPECT(weft_renderer_encode(custom, prepared, &p, &use), WRONG_BACKEND);
        CHECK(!use.renderer_id && !b.encodes);
        p.identity = info(custom).identity;
        EXPECT(weft_renderer_encode(metal, prepared, &p, &use), WRONG_BACKEND);
        CHECK(!a.encodes);
        p = pass(metal, scope);
        ++p.identity.device_epoch;
        EXPECT(weft_renderer_encode(metal, prepared, &p, &use), WRONG_EPOCH);
        p = pass(metal, scope);
        EXPECT(weft_renderer_encode(metal, prepared, &p, &use), OK);
        CHECK(a.reentrancy);
        EXPECT(weft_renderer_release_prepared(metal, prepared), OK);
        CHECK(!a.frames[0] && (*a.uses[0].resource)[0] == 128);
        EXPECT(weft_renderer_release_prepared(metal, prepared), STALE_HANDLE);
        EXPECT(weft_renderer_notify_submitted(metal, use, 99, 4), INVALID_ARGUMENT);
        EXPECT(weft_renderer_notify_submitted(metal, use, 11, 0), INVALID_SERIAL);
        CHECK(info(metal).encoded_uses == 1);
        scope.submitted = true;
        deny_allocation = true;
        const auto submit = weft_renderer_notify_submitted(metal, use, 11, 4);
        const auto complete = weft_renderer_notify_completed(metal, 7, 11, 5);
        deny_allocation = false;
        EXPECT(submit, OK);
        EXPECT(complete, OK);
        CHECK(a.retirements == 1 && info(metal).completed_serial == 5);
        EXPECT(weft_renderer_discard_encoded(metal, use), STALE_HANDLE);
        EXPECT(weft_renderer_notify_completed(metal, 6, 11, 6), WRONG_EPOCH);
        EXPECT(weft_renderer_notify_completed(metal, 7, 11, 4), INVALID_SERIAL);
        EXPECT(weft_renderer_destroy(metal), OK);
        EXPECT(weft_renderer_destroy(custom), OK);
    }

    // A display list frame reaches prepare_list only through a backend that reports lists, after the core validated
    // it; a backend table that ends before prepare_list has no lists.
    void DisplayListFrames() {
        namespace paint = weft::paint;
        FakeBackend b;
        auto table = b.table();
        weft_renderer* r{};
        table.capabilities.features |= WEFT_RENDER_FEATURE_DISPLAY_LIST;
        EXPECT(weft_renderer_create(&table, nullptr, &r), INVALID_ARGUMENT);
        table.prepare_list = FakeBackend::prepare_list;
        table.capabilities.features &= ~uint32_t(WEFT_RENDER_FEATURE_DISPLAY_LIST);
        EXPECT(weft_renderer_create(&table, nullptr, &r), INVALID_ARGUMENT);

        // A box and a coverage image of one asset.
        auto valid = [] {
            auto list = std::make_shared<paint::DisplayList>();
            list->assets.push_back(
                std::make_shared<const paint::Asset>(paint::Asset{2, 1, paint::Encoding::Coverage, {64, 128}}));
            list->boxes.emplace_back().background = {1, 0, 0, 1};
            list->images.emplace_back();
            for (auto kind : {paint::Item::Kind::Box, paint::Item::Kind::Image}) {
                paint::Item item;
                item.kind = kind;
                item.width = item.height = 4;
                item.clip = {0, 0, 64, 64};
                list->items.push_back(item);
            }
            return list;
        };
        weft_paint_list handle{valid()};
        weft_render_list_frame frame{sizeof(frame), WEFT_RENDER_ABI_VERSION, &handle, 0, 0, nullptr};
        weft_render_handle h{};
        auto old = b.table();
        old.struct_size = uint32_t(offsetof(weft_render_backend, prepare_list));
        EXPECT(weft_renderer_create(&old, nullptr, &r), OK);
        EXPECT(weft_renderer_prepare_list(r, &frame, &h), UNSUPPORTED_FEATURE);
        EXPECT(weft_renderer_destroy(r), OK);

        table.capabilities.features |= WEFT_RENDER_FEATURE_DISPLAY_LIST;
        EXPECT(weft_renderer_create(&table, nullptr, &r), OK);
        b.renderer = r;
        auto refused = [&](auto&& change, weft_render_result expected) {
            auto list = valid();
            change(*list);
            handle.list = list;
            return weft_renderer_prepare_list(r, &frame, &h) == expected && !h.renderer_id;
        };
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.items[0].opacity = 2;
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        // Textures by name are a View frame's (weft_renderer_prepare_scene).
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.textures = {"minimap"};
            },
            WEFT_RENDER_UNSUPPORTED_FEATURE));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.items[1].transform = {0, 0, 0, 0, 0, 0};
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.boxes[0].gradient.stops.resize(9);
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.images[0].asset = 1;
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.assets[0] = std::make_shared<const paint::Asset>(paint::Asset{2, 1, paint::Encoding::Coverage, {1}});
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.assets[0] = std::make_shared<const paint::Asset>(
                    paint::Asset{1, 1, paint::Encoding::SrgbStraight, {1, 2, 3, 4}});
            },
            WEFT_RENDER_UNSUPPORTED_FEATURE));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.clips.push_back({{0, 0, -1, 4}, {}, {}});
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.clips.assign(weft::constants::render::scene::max_rounded_clips + 1, {{0, 0, 4, 4}, {}, {}});
                l.items[0].clips_count = std::uint32_t(l.clips.size());
            },
            WEFT_RENDER_UNSUPPORTED_FEATURE));
        // The list's external textures are exactly the frame's, and need the extension.
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.external_textures = 1;
                l.images[0].asset = 1;
            },
            WEFT_RENDER_INVALID_ARGUMENT));
        weft_render_handle missing{};
        frame.external_count = 1;
        frame.external_textures = &missing;
        CHECK(refused(
            [](paint::DisplayList& l) {
                l.external_textures = 1;
                l.images[0].asset = 1;
            },
            WEFT_RENDER_UNSUPPORTED_FEATURE));
        frame.external_count = 0;
        frame.external_textures = nullptr;
        handle.list = nullptr;
        EXPECT(weft_renderer_prepare_list(r, &frame, &h), INVALID_ARGUMENT);
        CHECK(!b.list_preparations && !b.preparations);

        // A valid list: the backend keeps its own reference past the call.
        const auto list = valid();
        handle.list = list;
        EXPECT(weft_renderer_prepare_list(r, &frame, &h), OK);
        handle.list.reset();
        CHECK(b.list_preparations == 1 && b.list == list && list.use_count() == 2 && info(r).prepared_frames == 1);
        EXPECT(weft_renderer_release_prepared(r, h), OK);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    void ValidationBeforeNativeRecording() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        weft_render_handle h{};
        f.asset.encoding = WEFT_RENDER_SIGNED_DISTANCE;
        f.asset.sdf_spread = 8;
        EXPECT(weft_renderer_prepare(r, &f.view, &h), UNSUPPORTED_FEATURE);
        CHECK(!b.preparations && !h.renderer_id);
        f.asset.encoding = WEFT_RENDER_COVERAGE;
        f.asset.sdf_spread = 0;
        f.asset.byte_count = UINT64_MAX;
        EXPECT(weft_renderer_prepare(r, &f.view, &h), INVALID_ARGUMENT);
        f.asset.byte_count = 1;
        f.draw.asset_index = 2;
        EXPECT(weft_renderer_prepare(r, &f.view, &h), INVALID_ARGUMENT);
        f.draw.asset_index = 0;
        f.draw.opacity = 2;
        EXPECT(weft_renderer_prepare(r, &f.view, &h), INVALID_ARGUMENT);
        f.draw.opacity = 1;
        EXPECT(weft_renderer_prepare(r, &f.view, &h), OK);
        Scope scope;
        auto p = pass(r, scope);
        weft_render_handle use{};
        p.sample_count = 4;
        EXPECT(weft_renderer_encode(r, h, &p, &use), UNSUPPORTED_FEATURE);
        p.sample_count = 1;
        p.target_format = 99;
        EXPECT(weft_renderer_encode(r, h, &p, &use), UNSUPPORTED_FEATURE);
        p.target_format = 0;
        p.native_pass_size = 1;
        EXPECT(weft_renderer_encode(r, h, &p, &use), INVALID_ARGUMENT);
        CHECK(!use.renderer_id && !scope.commands);
        p.native_pass_size = sizeof(scope);
        scope.schema = 0;
        EXPECT(weft_renderer_encode(r, h, &p, &use), INVALID_ARGUMENT);
        scope.schema = weft::test_constants::render::fake_pass_schema;
        weft_render_result cross_thread = WEFT_RENDER_OK;
        std::thread other([&] {
            cross_thread = weft_renderer_encode(r, h, &p, &use);
        });
        other.join();
        EXPECT(cross_thread, WRONG_THREAD);
        p.target_format = WEFT_RENDER_TARGET_SRGB_BGRA8;
        EXPECT(weft_renderer_encode(r, h, &p, &use), OK);
        scope.cancelled = true;
        EXPECT(weft_renderer_discard_encoded(r, use), OK);
        EXPECT(weft_renderer_release_prepared(r, h), OK);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    void PartialRecordingFailureAndDrain() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        weft_render_handle h{};
        EXPECT(weft_renderer_prepare(r, &f.view, &h), OK);
        Scope scope;
        auto p = pass(r, scope);
        weft_render_handle use{};
        b.atomic_failure = true;
        EXPECT(weft_renderer_encode(r, h, &p, &use), BACKEND_ERROR);
        CHECK(!use.renderer_id && !scope.commands);
        b.atomic_failure = false;
        b.partial_failure = true;
        EXPECT(weft_renderer_encode(r, h, &p, &use), BACKEND_ERROR);
        CHECK(use.renderer_id && scope.commands && info(r).encoded_uses == 1);
        EXPECT(weft_renderer_destroy(r), BUSY);
        CHECK(info(r).draining);
        weft_render_handle rejected{};
        EXPECT(weft_renderer_prepare(r, &f.view, &rejected), SHUTTING_DOWN);
        EXPECT(weft_renderer_release_prepared(r, h), OK);
        scope.cancelled = true;
        EXPECT(weft_renderer_discard_encoded(r, use), OK);
        CHECK(b.discards == 1);
        EXPECT(weft_renderer_destroy(r), OK);
        // Partial commands that the host actually submitted must still retire.
        FakeBackend submitted;
        submitted.partial_failure = true;
        r = create(submitted);
        EXPECT(weft_renderer_prepare(r, &f.view, &h), OK);
        Scope live;
        p = pass(r, live);
        EXPECT(weft_renderer_encode(r, h, &p, &use), BACKEND_ERROR);
        live.submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 10), OK);
        EXPECT(weft_renderer_discard_encoded(r, use), INVALID_STATE);
        EXPECT(weft_renderer_release_prepared(r, h), OK);
        EXPECT(weft_renderer_destroy(r), BUSY);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 10), OK);
        CHECK(submitted.retirements == 1);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    void CapacityGenerationsAndSharedSubmission() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        weft_render_handle frame{};
        weft_render_handle second{};
        weft_render_handle overflow{};
        EXPECT(weft_renderer_prepare(r, &f.view, &frame), OK);
        EXPECT(weft_renderer_prepare(r, &f.view, &second), OK);
        EXPECT(weft_renderer_prepare(r, &f.view, &overflow), BUSY);
        CHECK(b.preparations == 2);
        EXPECT(weft_renderer_release_prepared(r, second), OK);
        EXPECT(weft_renderer_prepare(r, &f.view, &second), OK);
        CHECK(second.generation == 2);
        auto old = second;
        --old.generation;
        EXPECT(weft_renderer_release_prepared(r, old), STALE_HANDLE);
        std::array<Scope, 3> scopes;
        std::array<weft_render_handle, 3> uses{};
        for (size_t i = 0; i < 2; ++i) {
            auto p = pass(r, scopes[i]);
            EXPECT(weft_renderer_encode(r, frame, &p, &uses[i]), OK);
        }
        auto p = pass(r, scopes[2]);
        EXPECT(weft_renderer_encode(r, frame, &p, &uses[2]), BUSY);
        CHECK(!scopes[2].commands);
        for (size_t i = 0; i < 2; ++i) {
            scopes[i].submitted = true;
            EXPECT(weft_renderer_notify_submitted(r, uses[i], 11, 20), OK);
        }
        EXPECT(weft_renderer_encode(r, frame, &p, &uses[2]), OK);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 21), OK);
        CHECK(b.retirements == 2 && info(r).encoded_uses == 1);
        scopes[2].submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, uses[2], 11, 20), INVALID_SERIAL);
        EXPECT(weft_renderer_notify_submitted(r, uses[2], 11, 22), OK);
        EXPECT(weft_renderer_release_prepared(r, frame), OK);
        EXPECT(weft_renderer_release_prepared(r, second), OK);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 22), OK);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    void AbandonRequiresHostProofAndCancelledScopes() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        weft_render_handle frame{};
        weft_render_handle use{};
        EXPECT(weft_renderer_prepare(r, &f.view, &frame), OK);
        Scope scope;
        auto p = pass(r, scope);
        EXPECT(weft_renderer_encode(r, frame, &p, &use), OK);
        EXPECT(weft_renderer_abandon_device(r, 7, 0), HOST_CONFIRMATION_REQUIRED);
        EXPECT(weft_renderer_abandon_device(r, 7, 1), BUSY);
        scope.submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 2), OK);
        EXPECT(weft_renderer_abandon_device(r, 8, 1), WRONG_EPOCH);
        EXPECT(weft_renderer_abandon_device(r, 7, 1), OK);
        CHECK(!info(r).device_available && !info(r).submitted_uses);
        weft_render_handle next{};
        EXPECT(weft_renderer_prepare(r, &f.view, &next), DEVICE_UNAVAILABLE);
        EXPECT(weft_renderer_encode(r, frame, &p, &next), DEVICE_UNAVAILABLE);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 2), DEVICE_UNAVAILABLE);
        EXPECT(weft_renderer_release_prepared(r, frame), OK);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    void InFlightCapacityReservedBeforeNativeCommands() {
        FakeBackend b;
        const auto table = b.table();
        weft_renderer* r{};
        const weft_renderer_desc limits{sizeof(weft_renderer_desc), WEFT_RENDER_ABI_VERSION, 2, 2, 2, 8, 2, 0, 16};
        EXPECT(weft_renderer_create(&table, &limits, &r), OK);
        b.renderer = r;
        Frame f;
        weft_render_handle frame{};
        EXPECT(weft_renderer_prepare(r, &f.view, &frame), OK);
        std::array<Scope, 3> scopes;
        std::array<weft_render_handle, 3> uses{};
        for (size_t i = 0; i < 2; ++i) {
            auto p = pass(r, scopes[i]);
            EXPECT(weft_renderer_encode(r, frame, &p, &uses[i]), OK);
            scopes[i].submitted = true;
            EXPECT(weft_renderer_notify_submitted(r, uses[i], 11, 5), OK);
        }
        const auto before = b.encodes;
        auto p = pass(r, scopes[2]);
        EXPECT(weft_renderer_encode(r, frame, &p, &uses[2]), BUSY);
        CHECK(b.encodes == before && !scopes[2].commands);
        EXPECT(weft_renderer_release_prepared(r, frame), OK);
        EXPECT(weft_renderer_destroy(r), BUSY);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 5), OK);
        CHECK(b.retirements == 2);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    struct FakeExternal {
        FakeBackend& backend;
        std::array<bool, 3> native{};
        unsigned imports{};
        unsigned releases{};
        unsigned snapshots{};
        bool fail_import{};
        bool fail_prepare{};

        static weft_render_result import_texture(void* p, const weft_render_external_texture*, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeExternal*>(p);
            *out = 0;
            if (s.fail_import) {
                return WEFT_RENDER_BACKEND_ERROR;
            }
            for (size_t i = 0; i < s.native.size(); ++i) {
                if (!s.native[i]) {
                    s.native[i] = true;
                    *out = i + 1;
                    ++s.imports;
                    return WEFT_RENDER_OK;
                }
            }
            return WEFT_RENDER_BUSY;
        }

        static void release_texture(void* p, uint64_t id) noexcept {
            auto& s = *static_cast<FakeExternal*>(p);
            if (!s.native[id - 1]) {
                std::terminate();
            }
            s.native[id - 1] = false;
            ++s.releases;
        }

        static weft_render_result prepare(void* p, const weft_render_frame* f, const weft_render_external_asset* assets,
                                          uint32_t count, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeExternal*>(p);
            *out = 0;
            if (s.fail_prepare) {
                return WEFT_RENDER_BACKEND_ERROR;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const auto& a = assets[i];
                if (!s.native[a.native_texture - 1] || a.description.native_texture || a.description.retired ||
                    a.description.retirement_user_data) {
                    std::terminate();
                }
            }
            ++s.snapshots;
            return FakeBackend::prepare(&s.backend, f, out);
        }

        weft_render_external_backend table() {
            return {sizeof(weft_render_external_backend),
                    1,
                    1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F,
                    0,
                    3,
                    0,
                    128,
                    this,
                    import_texture,
                    release_texture,
                    prepare};
        }
    };

    struct Retirement {
        weft_renderer* renderer{};
        unsigned calls{};
        unsigned reason{};
        bool reentry_rejected{};

        static void callback(void* p, uint32_t reason) noexcept {
            auto& s = *static_cast<Retirement*>(p);
            ++s.calls;
            s.reason = reason;
            weft_render_external_info data{};
            data.struct_size = sizeof(data);
            s.reentry_rejected = weft_renderer_get_external_info(s.renderer, &data) == WEFT_RENDER_BUSY;
        }
    };

    weft_render_external_texture external_description(weft_renderer* r, Retirement& retired, uint64_t version = 1) {
        static int native_schema;
        return {sizeof(weft_render_external_texture),
                1,
                info(r).identity,
                99,
                version,
                2,
                2,
                WEFT_RENDER_TARGET_LINEAR_RGBA32F,
                0,
                0,
                0,
                0,
                0,
                &native_schema,
                sizeof(native_schema),
                &retired,
                Retirement::callback};
    }

    weft_render_external_frame external_frame(Frame& f, const weft_render_handle* textures, uint32_t count = 1) {
        f.draw.asset_index = 1;
        return {sizeof(weft_render_external_frame), 1, f.view, count, 0, textures};
    }

    weft_render_external_info external_info(weft_renderer* r) {
        weft_render_external_info out{};
        out.struct_size = sizeof(out);
        EXPECT(weft_renderer_get_external_info(r, &out), OK);
        return out;
    }

    void ExternalValidationAndIndependentPins() {
        FakeBackend b;
        auto* r = create(b);
        FakeExternal ext{b};
        Retirement retired{r};
        auto d = external_description(r, retired);
        weft_render_handle texture{};
        weft_render_handle second{};
        weft_render_handle failed{};
        EXPECT(weft_renderer_import_texture(r, &d, &texture), UNSUPPORTED_FEATURE);
        auto table = ext.table();
        auto bad = table;
        // A format no weft_render_target_format names.
        bad.formats = 1u << (WEFT_RENDER_TARGET_LINEAR_RGBA16F + 1);
        EXPECT(weft_renderer_install_external_backend(r, &bad), INVALID_ARGUMENT);
        deny_allocation = true;
        EXPECT(weft_renderer_install_external_backend(r, &table), OUT_OF_MEMORY);
        deny_allocation = false;
        EXPECT(weft_renderer_install_external_backend(r, &table), OK);
        EXPECT(weft_renderer_install_external_backend(r, &table), INVALID_STATE);
        auto invalid = d;
        ++invalid.identity.device_epoch;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), WRONG_EPOCH);
        invalid = d;
        ++invalid.identity.renderer_id;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), WRONG_BACKEND);
        invalid = d;
        ++invalid.identity.queue;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), INVALID_ARGUMENT);
        invalid = d;
        invalid.synchronization = WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE + 1;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.encoding = 1;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.width = 65;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.width = 0;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), INVALID_ARGUMENT);
        // A 2 x 2 texture has two levels.
        invalid = d;
        invalid.mip_levels = 3;
        EXPECT(weft_renderer_import_texture(r, &invalid, &failed), INVALID_ARGUMENT);
        ext.fail_import = true;
        EXPECT(weft_renderer_import_texture(r, &d, &failed), BACKEND_ERROR);
        ext.fail_import = false;
        CHECK(!retired.calls && !external_info(r).live_textures);
        EXPECT(weft_renderer_import_texture(r, &d, &texture), OK);
        EXPECT(weft_renderer_import_texture(r, &d, &failed), INVALID_STATE);
        // A live texture: its producer writes it again between the frames that sample it.
        d.content_version = 2;
        d.synchronization = WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE;
        EXPECT(weft_renderer_import_texture(r, &d, &second), OK);
        d.synchronization = WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED;
        d.content_version = 3;
        EXPECT(weft_renderer_import_texture(r, &d, &failed), BUSY);
        CHECK(external_info(r).texture_bytes == 128);
        EXPECT(weft_renderer_release_texture(r, second), OK);
        CHECK(retired.calls == 1);
        // Every level's bytes count: two levels of 2 x 2 RGBA32F (80 bytes) do not fit beside the first (64 of 128).
        d.content_version = 4;
        d.mip_levels = 2;
        EXPECT(weft_renderer_import_texture(r, &d, &failed), BUSY);
        d.mip_levels = 0;
        Frame f;
        auto input = external_frame(f, &texture);
        weft_render_handle frame{};
        weft_render_handle use{};
        weft_render_handle another{};
        auto wrong = texture;
        ++wrong.device_epoch;
        input.external_textures = &wrong;
        EXPECT(weft_renderer_prepare_external(r, &input, &frame), WRONG_EPOCH);
        input.external_textures = &texture;
        f.draw.sdf_stroke = 1;
        EXPECT(weft_renderer_prepare_external(r, &input, &frame), INVALID_ARGUMENT);
        f.draw.sdf_stroke = 0;
        ext.fail_prepare = true;
        EXPECT(weft_renderer_prepare_external(r, &input, &frame), BACKEND_ERROR);
        ext.fail_prepare = false;
        EXPECT(weft_renderer_prepare_external(r, &input, &frame), OK);
        EXPECT(weft_renderer_release_texture(r, texture), OK);
        EXPECT(weft_renderer_release_texture(r, texture), STALE_HANDLE);
        CHECK(retired.calls == 1 && external_info(r).caller_leases == 0 && external_info(r).live_textures == 1);
        EXPECT(weft_renderer_prepare_external(r, &input, &another), STALE_HANDLE);
        std::array<Scope, 2> scopes;
        auto p = pass(r, scopes[0]);
        EXPECT(weft_renderer_encode(r, frame, &p, &use), OK);
        p = pass(r, scopes[1]);
        EXPECT(weft_renderer_encode(r, frame, &p, &another), OK);
        EXPECT(weft_renderer_release_prepared(r, frame), OK);
        CHECK(retired.calls == 1);
        EXPECT(weft_renderer_notify_submitted(r, use, 12, 1), INVALID_ARGUMENT);
        scopes[0].submitted = true;
        scopes[1].submitted = true;
        deny_allocation = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 1), OK);
        EXPECT(weft_renderer_notify_submitted(r, another, 11, 2), OK);
        EXPECT(weft_renderer_notify_completed(r, 8, 11, 2), WRONG_EPOCH);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 1), OK);
        CHECK(retired.calls == 1);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 2), OK);
        deny_allocation = false;
        CHECK(retired.calls == 2 && retired.reason == WEFT_RENDER_EXTERNAL_RETIRED && retired.reentry_rejected &&
              ext.releases == 2);
        CHECK(!external_info(r).texture_bytes);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    void ExternalMutationFailureAndAbandon() {
        FakeBackend b;
        auto* r = create(b);
        FakeExternal ext{b};
        const auto table = ext.table();
        EXPECT(weft_renderer_install_external_backend(r, &table), OK);
        Retirement retired{r};
        Frame f;
        uint64_t serial{};
        uint32_t random = weft::test_constants::render::external_mutation_seed;
        for (unsigned i = 0; i < weft::test_constants::render::external_mutation_iterations; ++i) {
            auto d = external_description(r, retired, i + 1);
            weft_render_handle texture{};
            weft_render_handle frame{};
            weft_render_handle use{};
            EXPECT(weft_renderer_import_texture(r, &d, &texture), OK);
            auto input = external_frame(f, &texture);
            EXPECT(weft_renderer_prepare_external(r, &input, &frame), OK);
            random = random * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            const auto mode = random % 4;
            Scope scope;
            auto p = pass(r, scope);
            b.atomic_failure = mode == 0;
            b.partial_failure = mode == 1;
            EXPECT(weft_renderer_release_texture(r, texture), OK);
            CHECK(weft_renderer_encode(r, frame, &p, &use) == (mode < 2 ? WEFT_RENDER_BACKEND_ERROR : WEFT_RENDER_OK));
            EXPECT(weft_renderer_release_prepared(r, frame), OK);
            if (mode == 0) {
                CHECK(retired.calls == i + 1 && !use.renderer_id);
            } else {
                CHECK(retired.calls == i);
                if (mode < 3) {
                    scope.cancelled = true;
                    EXPECT(weft_renderer_discard_encoded(r, use), OK);
                } else {
                    scope.submitted = true;
                    EXPECT(weft_renderer_notify_submitted(r, use, 11, ++serial), OK);
                    deny_allocation = true;
                    EXPECT(weft_renderer_notify_completed(r, 7, 11, serial), OK);
                    deny_allocation = false;
                }
            }
            CHECK(retired.calls == i + 1 && !external_info(r).live_textures);
        }
        b.atomic_failure = false;
        b.partial_failure = false;
        auto d = external_description(r, retired, weft::test_constants::render::external_mutation_iterations + 1);
        weft_render_handle texture{};
        weft_render_handle frame{};
        weft_render_handle use{};
        EXPECT(weft_renderer_import_texture(r, &d, &texture), OK);
        auto input = external_frame(f, &texture);
        EXPECT(weft_renderer_prepare_external(r, &input, &frame), OK);
        Scope scope;
        auto p = pass(r, scope);
        EXPECT(weft_renderer_encode(r, frame, &p, &use), OK);
        EXPECT(weft_renderer_release_texture(r, texture), OK);
        scope.submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, ++serial), OK);
        EXPECT(weft_renderer_abandon_device(r, 7, 0), HOST_CONFIRMATION_REQUIRED);
        EXPECT(weft_renderer_abandon_device(r, 7, 1), OK);
        CHECK(retired.calls == 512);
        EXPECT(weft_renderer_release_prepared(r, frame), OK);
        CHECK(retired.calls == 513 && retired.reason == WEFT_RENDER_EXTERNAL_ABANDONED);
        EXPECT(weft_renderer_destroy(r), OK);
        FakeBackend another;
        auto* next = create(another);
        FakeExternal next_ext{another};
        const auto next_table = next_ext.table();
        EXPECT(weft_renderer_install_external_backend(next, &next_table), OK);
        Retirement next_retired{next};
        d = external_description(next, next_retired);
        EXPECT(weft_renderer_import_texture(next, &d, &texture), OK);
        weft_render_result result{};
        std::thread worker([&] {
            result = weft_renderer_release_texture(next, texture);
        });
        worker.join();
        CHECK(result == WEFT_RENDER_WRONG_THREAD);
        EXPECT(weft_renderer_destroy(next), BUSY);
        weft_render_handle denied{};
        EXPECT(weft_renderer_import_texture(next, &d, &denied), SHUTTING_DOWN);
        EXPECT(weft_renderer_release_texture(next, texture), OK);
        CHECK(next_retired.calls == 1);
        EXPECT(weft_renderer_destroy(next), OK);
    }

    // The export extension's fake: images by token, and the host's view of them (what it may still sample) that a draw
    // must never touch. Uses are the main fake's; an export use records the image it draws.
    struct FakeExport {
        static constexpr uint64_t texture_base = 0x1000;
        FakeBackend& backend;
        std::array<bool, 8> images{};
        // The host may sample the image (leased, or released with a sampling that has not completed).
        std::array<bool, 8> sampled{};
        // The image each of the main fake's uses draws (0: none).
        std::array<uint64_t, 4> drawn{};
        std::array<uint32_t, 8> levels{};
        unsigned creates{};
        unsigned destroys{};
        unsigned draws{};
        unsigned fail_create_at{};
        uint64_t last_image{};

        static weft_render_result create_image(void* p, const weft_render_export_desc* d, uint64_t* native,
                                               uint64_t* texture) noexcept {
            auto& s = *static_cast<FakeExport*>(p);
            *native = 0;
            *texture = 0;
            if (s.fail_create_at && s.creates + 1 == s.fail_create_at) {
                s.fail_create_at = 0;
                return WEFT_RENDER_OUT_OF_MEMORY;
            }
            for (size_t i = 0; i < s.images.size(); ++i) {
                if (!s.images[i]) {
                    s.images[i] = true;
                    s.levels[i] = d->mip_levels;
                    *native = i + 1;
                    *texture = texture_base + i + 1;
                    ++s.creates;
                    return WEFT_RENDER_OK;
                }
            }
            return WEFT_RENDER_BUSY;
        }

        // An image is destroyed only when nothing samples or draws it.
        bool drawing(uint64_t image) const {
            for (size_t k = 0; k < drawn.size(); ++k) {
                if (drawn[k] == image && backend.uses[k].resource) {
                    return true;
                }
            }
            return false;
        }

        static void destroy_image(void* p, uint64_t id) noexcept {
            auto& s = *static_cast<FakeExport*>(p);
            if (!s.images[id - 1] || s.sampled[id - 1] || s.drawing(id)) {
                std::terminate();
            }
            s.images[id - 1] = false;
            ++s.destroys;
        }

        static weft_render_result encode(void* p, uint64_t frame, uint64_t image, const weft_render_pass* pass,
                                         uint64_t* out) noexcept {
            auto& s = *static_cast<FakeExport*>(p);
            *out = 0;
            if (!s.images[image - 1] || s.sampled[image - 1] || s.drawing(image)) {
                std::terminate();
            }
            ++s.draws;
            s.last_image = image;
            const auto v = FakeBackend::encode(&s.backend, frame, pass, out);
            if (*out) {
                s.drawn[*out - 1] = image;
            }
            return v;
        }

        weft_render_export_backend table(uint32_t max_images = 6, uint64_t bytes = 1u << 20) {
            return {sizeof(weft_render_export_backend),
                    WEFT_RENDER_EXPORT_ABI_VERSION,
                    (1u << WEFT_RENDER_TARGET_SRGB_RGBA8) | (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F),
                    max_images,
                    bytes,
                    this,
                    create_image,
                    destroy_image,
                    encode};
        }
    };

    weft_render_export_desc export_desc(weft_renderer* r, uint32_t images = 2) {
        return {sizeof(weft_render_export_desc),
                WEFT_RENDER_EXPORT_ABI_VERSION,
                info(r).identity,
                64,
                32,
                WEFT_RENDER_TARGET_SRGB_RGBA8,
                0,
                images,
                0};
    }

    weft_render_pass export_pass(weft_renderer* r, Scope& scope) {
        auto p = pass(r, scope);
        p.width = 64;
        p.height = 32;
        return p;
    }

    weft_render_export_info export_info(weft_renderer* r) {
        weft_render_export_info out{};
        out.struct_size = sizeof(out);
        EXPECT(weft_renderer_get_export_info(r, &out), OK);
        return out;
    }

    weft_render_export_image empty_image() {
        weft_render_export_image out{};
        out.struct_size = sizeof(out);
        return out;
    }

    // The fake's token of an image the host leases.
    uint64_t token(const weft_render_export_image& image) {
        return image.native_texture - FakeExport::texture_base;
    }

    void ExportValidationRingAndLeases() {
        FakeBackend b;
        auto* r = create(b);
        FakeExport ex{b};
        auto d = export_desc(r);
        weft_render_handle panel{};
        weft_render_export_info data{};
        data.struct_size = sizeof(data);
        EXPECT(weft_renderer_create_export(r, &d, &panel), UNSUPPORTED_FEATURE);
        EXPECT(weft_renderer_get_export_info(r, &data), UNSUPPORTED_FEATURE);
        // 64 KiB: two images of the 64 x 32 RGBA8 chain fit, one of a 128 x 128 RGBA16F chain does not.
        auto table = ex.table(6, 1u << 16);
        auto bad = table;
        bad.formats = 1u << (WEFT_RENDER_TARGET_LINEAR_RGB10A2 + 1);
        EXPECT(weft_renderer_install_export_backend(r, &bad), INVALID_ARGUMENT);
        bad = table;
        bad.max_images = 0;
        EXPECT(weft_renderer_install_export_backend(r, &bad), INVALID_ARGUMENT);
        bad = table;
        bad.encode = nullptr;
        EXPECT(weft_renderer_install_export_backend(r, &bad), INVALID_ARGUMENT);
        bad = table;
        bad.abi_version = 2;
        EXPECT(weft_renderer_install_export_backend(r, &bad), UNSUPPORTED_ABI);
        // Only while quiescent.
        Frame f;
        weft_render_handle prepared{};
        EXPECT(weft_renderer_prepare(r, &f.view, &prepared), OK);
        EXPECT(weft_renderer_install_export_backend(r, &table), INVALID_STATE);
        EXPECT(weft_renderer_release_prepared(r, prepared), OK);
        deny_allocation = true;
        EXPECT(weft_renderer_install_export_backend(r, &table), OUT_OF_MEMORY);
        deny_allocation = false;
        EXPECT(weft_renderer_install_export_backend(r, &table), OK);
        EXPECT(weft_renderer_install_export_backend(r, &table), INVALID_STATE);

        auto invalid = d;
        ++invalid.identity.device_epoch;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), WRONG_EPOCH);
        invalid = d;
        ++invalid.identity.renderer_id;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), WRONG_BACKEND);
        invalid = d;
        ++invalid.identity.queue;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), INVALID_ARGUMENT);
        invalid = d;
        invalid.images = 0;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), INVALID_ARGUMENT);
        invalid.images = WEFT_RENDER_EXPORT_MAX_IMAGES + 1;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), INVALID_ARGUMENT);
        // 64 x 32 has seven levels.
        invalid = d;
        invalid.mip_levels = 8;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), INVALID_ARGUMENT);
        invalid = d;
        invalid.width = 129;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.format = WEFT_RENDER_TARGET_LINEAR_RGBA32F;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), UNSUPPORTED_FEATURE);
        // More images than the backend has left, and more bytes.
        invalid = d;
        invalid.images = 7;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), BUSY);
        invalid = d;
        invalid.width = invalid.height = 128;
        invalid.format = WEFT_RENDER_TARGET_LINEAR_RGBA16F;
        invalid.images = 1;
        EXPECT(weft_renderer_create_export(r, &invalid, &panel), BUSY);
        CHECK(!ex.creates);
        // A failed image rolls back the ones made before it.
        ex.fail_create_at = 2;
        EXPECT(weft_renderer_create_export(r, &d, &panel), OUT_OF_MEMORY);
        CHECK(ex.creates == 1 && ex.destroys == 1 && !panel.renderer_id && !export_info(r).images);
        ex.creates = ex.destroys = 0;

        EXPECT(weft_renderer_create_export(r, &d, &panel), OK);
        data = export_info(r);
        // Two images of a full chain of 64 x 32 RGBA8: seven levels, 2048 + 512 + 128 + 32 + 8 + 2 + 1 texels.
        CHECK(data.exports == 1 && data.images == 2 && !data.leases && data.image_bytes == 2 * 4 * 2731 &&
              ex.levels[0] == 7 && ex.levels[1] == 7);

        EXPECT(weft_renderer_prepare(r, &f.view, &prepared), OK);
        std::array<Scope, 4> scopes;
        weft_render_handle use{};
        auto image = empty_image();
        auto p = export_pass(r, scopes[0]);
        p.width = 63;
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), INVALID_ARGUMENT);
        p = export_pass(r, scopes[0]);
        p.target_format = WEFT_RENDER_TARGET_LINEAR_RGBA16F;
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), INVALID_ARGUMENT);
        p = export_pass(r, scopes[0]);
        image.struct_size = 8;
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), INVALID_ARGUMENT);
        image = empty_image();
        EXPECT(weft_renderer_encode_export(r, prepared, prepared, &p, &use, &image), STALE_HANDLE);
        CHECK(!ex.draws);

        // Two images: each leased in turn, then none is free.
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), OK);
        const auto first = image;
        CHECK(first.version == 1 && first.width == 64 && first.height == 32 && first.mip_levels == 7 &&
              first.format == WEFT_RENDER_TARGET_SRGB_RGBA8 && first.image.kind && first.native_texture);
        ex.sampled[token(first) - 1] = true;
        scopes[0].submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 1), OK);
        p = export_pass(r, scopes[1]);
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), OK);
        const auto second = image;
        CHECK(second.version == 2 && token(second) != token(first) && export_info(r).leases == 2);
        ex.sampled[token(second) - 1] = true;
        scopes[1].submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 2), OK);
        p = export_pass(r, scopes[2]);
        const auto draws = ex.draws;
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), BUSY);
        CHECK(ex.draws == draws && !scopes[2].commands && !use.renderer_id && !image.image.kind);

        // The first goes back once the host's last sampling of it (serial 3) completed.
        EXPECT(weft_renderer_release_export_image(r, first.image, 12, 3), INVALID_ARGUMENT);
        deny_allocation = true;
        EXPECT(weft_renderer_release_export_image(r, first.image, 11, 3), OK);
        deny_allocation = false;
        EXPECT(weft_renderer_release_export_image(r, first.image, 11, 3), STALE_HANDLE);
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), BUSY);
        deny_allocation = true;
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 2), OK);
        deny_allocation = false;
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), BUSY);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 3), OK);
        ex.sampled[token(first) - 1] = false;
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), OK);
        const auto third = image;
        CHECK(third.version == 3 && token(third) == token(first) && third.image.slot == first.image.slot &&
              third.image.generation != first.image.generation);
        ex.sampled[token(third) - 1] = true;
        scopes[2].submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 4), OK);
        // A lease of an earlier occupancy of the image is stale.
        EXPECT(weft_renderer_release_export_image(r, first.image, 11, 0), STALE_HANDLE);

        // A cancelled recording: the image is not leased, and free again once discarded.
        EXPECT(weft_renderer_release_export_image(r, second.image, 11, 0), OK);
        ex.sampled[token(second) - 1] = false;
        b.partial_failure = true;
        weft_render_handle partial{};
        auto none = empty_image();
        p = export_pass(r, scopes[3]);
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &partial, &none), BACKEND_ERROR);
        b.partial_failure = false;
        CHECK(partial.renderer_id && !none.image.kind && export_info(r).leases == 1);
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &none), BUSY);
        scopes[3].cancelled = true;
        EXPECT(weft_renderer_discard_encoded(r, partial), OK);
        b.atomic_failure = true;
        Scope atomic;
        p = export_pass(r, atomic);
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &none), BACKEND_ERROR);
        b.atomic_failure = false;
        CHECK(!use.renderer_id && !none.image.kind);

        // Destroying the export invalidates its handle at once; the leased image stays until released and its
        // sampling completed, the drawing one until its use retires.
        Scope last;
        p = export_pass(r, last);
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), OK);
        ex.sampled[token(image) - 1] = true;
        CHECK(image.version == 4);
        EXPECT(weft_renderer_release_prepared(r, prepared), OK);
        EXPECT(weft_renderer_destroy_export(r, panel), OK);
        EXPECT(weft_renderer_destroy_export(r, panel), STALE_HANDLE);
        CHECK(export_info(r).exports == 0 && export_info(r).images == 2 && !ex.destroys);
        EXPECT(weft_renderer_destroy(r), BUSY);
        EXPECT(weft_renderer_release_export_image(r, third.image, 11, 6), OK);
        EXPECT(weft_renderer_release_export_image(r, image.image, 11, 0), OK);
        ex.sampled[token(image) - 1] = false;
        CHECK(!ex.destroys);
        last.submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 5), OK);
        deny_allocation = true;
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 5), OK);
        deny_allocation = false;
        CHECK(ex.destroys == 1 && export_info(r).images == 1);
        ex.sampled[token(third) - 1] = false;
        EXPECT(weft_renderer_notify_completed(r, 7, 11, 6), OK);
        CHECK(ex.destroys == 2 && !export_info(r).images && !export_info(r).image_bytes);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    // After the host vouches the device is gone, released images go at once whatever sampling they were released
    // with; producing calls report the device unavailable.
    void ExportAbandon() {
        FakeBackend b;
        auto* r = create(b);
        FakeExport ex{b};
        const auto table = ex.table();
        EXPECT(weft_renderer_install_export_backend(r, &table), OK);
        auto d = export_desc(r, 3);
        weft_render_handle panel{};
        EXPECT(weft_renderer_create_export(r, &d, &panel), OK);
        Frame f;
        weft_render_handle prepared{};
        EXPECT(weft_renderer_prepare(r, &f.view, &prepared), OK);
        Scope scope;
        auto p = export_pass(r, scope);
        weft_render_handle use{};
        auto image = empty_image();
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), OK);
        scope.submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, 1), OK);
        EXPECT(weft_renderer_abandon_device(r, 7, WEFT_RENDER_HOST_CONFIRMED_NO_FURTHER_ACCESS), OK);
        Scope again;
        p = export_pass(r, again);
        auto none = empty_image();
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &none), DEVICE_UNAVAILABLE);
        weft_render_handle another{};
        EXPECT(weft_renderer_create_export(r, &d, &another), DEVICE_UNAVAILABLE);
        EXPECT(weft_renderer_destroy_export(r, panel), OK);
        CHECK(ex.destroys == 2);
        EXPECT(weft_renderer_release_export_image(r, image.image, 11, 99), OK);
        CHECK(ex.destroys == 3 && !export_info(r).images);
        EXPECT(weft_renderer_release_prepared(r, prepared), OK);
        EXPECT(weft_renderer_destroy(r), OK);
    }

    // Random encodes (some failing), releases with past, present and future samplings, completions and discards over
    // one ring, against a model of which images are free: the core draws only free images (the fake terminates
    // otherwise), the least recently drawn of them, reports BUSY exactly when none is, and destroys every image once.
    void ExportMutation() {
        namespace tc = weft::test_constants::render;
        FakeBackend b;
        auto* r = create(b);
        FakeExport ex{b};
        const auto table = ex.table();
        EXPECT(weft_renderer_install_export_backend(r, &table), OK);
        const auto d = export_desc(r, 3);
        weft_render_handle panel{};
        EXPECT(weft_renderer_create_export(r, &d, &panel), OK);
        Frame f;
        weft_render_handle prepared{};
        EXPECT(weft_renderer_prepare(r, &f.view, &prepared), OK);

        struct Held {
            weft_render_export_image image;
        };

        std::vector<Held> held;
        // By token: the serial of the host's last sampling (0: none), the use drawing it (and its serial), the version
        // that drew it last.
        std::array<uint64_t, 9> sampling{};
        std::array<uint64_t, 9> drawing{};
        std::array<uint64_t, 9> version{};
        std::array<Scope, 4> scopes;
        uint64_t serial{};
        uint64_t completed{};
        uint64_t encodes{};
        uint32_t random = tc::export_mutation_seed;
        auto next = [&] {
            random = random * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
            return random >> 8;
        };
        auto leased = [&](uint64_t t) {
            return std::any_of(held.begin(), held.end(), [&](const Held& h) {
                return token(h.image) == t;
            });
        };
        auto free_in_model = [&](uint64_t t) {
            return !leased(t) && !drawing[t] && sampling[t] <= completed;
        };
        held.reserve(4);
        for (int step = 0; step < tc::export_mutation_steps; ++step) {
            const auto op = next() % 3;
            if (op == 0) {
                // The free image drawn least recently, if any.
                std::optional<uint64_t> expected;
                for (uint64_t t = 1; t <= 3; ++t) {
                    if (free_in_model(t) && (!expected || version[t] < version[*expected])) {
                        expected = t;
                    }
                }
                const auto mode = next() % 4;
                b.atomic_failure = mode == 0;
                b.partial_failure = mode == 1;
                auto& scope = scopes[step % scopes.size()];
                scope = {};
                auto p = export_pass(r, scope);
                weft_render_handle use{};
                auto image = empty_image();
                const auto draws = ex.draws;
                const auto v = weft_renderer_encode_export(r, prepared, panel, &p, &use, &image);
                b.atomic_failure = false;
                b.partial_failure = false;
                if (!expected) {
                    CHECK(v == WEFT_RENDER_BUSY && ex.draws == draws);
                    continue;
                }
                CHECK(ex.draws == draws + 1 && ex.last_image == *expected);
                if (mode == 0) {
                    CHECK(v == WEFT_RENDER_BACKEND_ERROR && !use.renderer_id);
                    continue;
                }
                if (mode == 1) {
                    CHECK(v == WEFT_RENDER_BACKEND_ERROR && use.renderer_id && !image.image.kind);
                    scope.cancelled = true;
                    deny_allocation = true;
                    EXPECT(weft_renderer_discard_encoded(r, use), OK);
                    deny_allocation = false;
                    continue;
                }
                EXPECT(v, OK);
                CHECK(token(image) == *expected && image.version == ++encodes);
                version[*expected] = image.version;
                held.push_back({image});
                ex.sampled[*expected - 1] = true;
                if (mode == 2) {
                    scope.cancelled = true;
                    EXPECT(weft_renderer_discard_encoded(r, use), OK);
                } else {
                    scope.submitted = true;
                    EXPECT(weft_renderer_notify_submitted(r, use, 11, ++serial), OK);
                    drawing[*expected] = serial;
                }
            } else if (op == 1 && !held.empty()) {
                // Release a lease with no sampling, one already made, or one the host makes later.
                const auto k = next() % held.size();
                const auto t = token(held[k].image);
                const auto kind = next() % 3;
                const uint64_t last = kind == 0 ? 0 : kind == 1 ? serial : serial + 1 + next() % 3;
                deny_allocation = true;
                EXPECT(weft_renderer_release_export_image(r, held[k].image.image, 11, last), OK);
                deny_allocation = false;
                EXPECT(weft_renderer_release_export_image(r, held[k].image.image, 11, last), STALE_HANDLE);
                sampling[t] = last;
                held.erase(held.begin() + std::ptrdiff_t(k));
            } else if (op == 2) {
                // Completion up to a serial the host has submitted or will submit.
                const auto upto = completed + next() % 3;
                if (!upto) {
                    continue;
                }
                serial = std::max(serial, upto);
                deny_allocation = true;
                EXPECT(weft_renderer_notify_completed(r, 7, 11, upto), OK);
                deny_allocation = false;
                completed = upto;
                for (uint64_t t = 1; t <= 3; ++t) {
                    if (drawing[t] && drawing[t] <= completed) {
                        drawing[t] = 0;
                    }
                }
            }
            for (uint64_t t = 1; t <= 3; ++t) {
                ex.sampled[t - 1] = leased(t) || sampling[t] > completed;
            }
            CHECK(export_info(r).leases == held.size() && ex.creates == 3 && !ex.destroys);
        }
        CHECK(encodes > uint64_t(tc::export_mutation_steps) / 10);
        // A last image drawing and leased when the export goes: every sampling completed, one lease released.
        serial = std::max(serial, *std::max_element(sampling.begin(), sampling.end()));
        EXPECT(weft_renderer_notify_completed(r, 7, 11, serial), OK);
        if (held.size() == 3) {
            EXPECT(weft_renderer_release_export_image(r, held.back().image.image, 11, 0), OK);
            held.pop_back();
        }
        std::fill(ex.sampled.begin(), ex.sampled.end(), false);
        for (const auto& h : held) {
            ex.sampled[token(h.image) - 1] = true;
        }
        Scope last;
        auto p = export_pass(r, last);
        weft_render_handle use{};
        auto image = empty_image();
        EXPECT(weft_renderer_encode_export(r, prepared, panel, &p, &use, &image), OK);
        held.push_back({image});
        last.submitted = true;
        EXPECT(weft_renderer_notify_submitted(r, use, 11, ++serial), OK);
        EXPECT(weft_renderer_release_prepared(r, prepared), OK);
        EXPECT(weft_renderer_destroy_export(r, panel), OK);
        for (const auto& h : held) {
            EXPECT(weft_renderer_release_export_image(r, h.image.image, 11, serial + 1), OK);
        }
        std::fill(ex.sampled.begin(), ex.sampled.end(), false);
        EXPECT(weft_renderer_destroy(r), BUSY);
        EXPECT(weft_renderer_notify_completed(r, 7, 11, serial + 1), OK);
        CHECK(ex.destroys == 3);
        EXPECT(weft_renderer_destroy(r), OK);
    }
} // namespace

int main() {
    try {
        IndependentBackendsAndSnapshotOwnership();
        DisplayListFrames();
        ValidationBeforeNativeRecording();
        PartialRecordingFailureAndDrain();
        CapacityGenerationsAndSharedSubmission();
        AbandonRequiresHostProofAndCancelledScopes();
        InFlightCapacityReservedBeforeNativeCommands();
        ExternalValidationAndIndependentPins();
        ExternalMutationFailureAndAbandon();
        ExportValidationRingAndLeases();
        ExportAbandon();
        ExportMutation();
        std::cout
            << "PASS backend ABI: native scope isolation, snapshot/pins, display list frames, partial failure, no-allocation submission, drain and generations, export rings and leases\n";
        return 0;
    } catch (const std::exception& e) {
        deny_allocation = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
