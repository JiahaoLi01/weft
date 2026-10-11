#include "weft/render_external_texture.h"
#include "constants/common.hpp"
#include "constants/render.hpp"
#include "render/paint_list.hpp"
#include "support/check.hpp"
#include "test_constants/render.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
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
        bad.formats = 1u << WEFT_RENDER_TARGET_SRGB_RGBA8;
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
        invalid.synchronization = 1;
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
        ext.fail_import = true;
        EXPECT(weft_renderer_import_texture(r, &d, &failed), BACKEND_ERROR);
        ext.fail_import = false;
        CHECK(!retired.calls && !external_info(r).live_textures);
        EXPECT(weft_renderer_import_texture(r, &d, &texture), OK);
        EXPECT(weft_renderer_import_texture(r, &d, &failed), INVALID_STATE);
        d.content_version = 2;
        EXPECT(weft_renderer_import_texture(r, &d, &second), OK);
        d.content_version = 3;
        EXPECT(weft_renderer_import_texture(r, &d, &failed), BUSY);
        CHECK(external_info(r).texture_bytes == 128);
        EXPECT(weft_renderer_release_texture(r, second), OK);
        CHECK(retired.calls == 1);
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
        std::cout
            << "PASS backend ABI: native scope isolation, snapshot/pins, display list frames, partial failure, no-allocation submission, drain and generations\n";
        return 0;
    } catch (const std::exception& e) {
        deny_allocation = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
