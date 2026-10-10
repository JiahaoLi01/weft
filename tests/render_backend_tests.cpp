#include "caseelephant/render_external_texture.h"
#include "caseelephant/constants/common.hpp"
#include "support/check.hpp"
#include "test_constants/render.hpp"
#include <array>
#include <atomic>
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
#define CHECK(v) CE_REQUIRE(v)
#define EXPECT(v, result) CHECK((v) == CE_RENDER_##result)

    struct Scope {
        uint64_t schema{caseelephant::test_constants::render::fake_pass_schema};
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
        ce_renderer* renderer{};
        unsigned preparations{};
        unsigned encodes{};
        unsigned retirements{};
        unsigned discards{};
        bool partial_failure{};
        bool atomic_failure{};
        bool prepare_failure{};
        bool reentrancy{};

        static ce_render_result prepare(void* ptr, const ce_render_frame* f, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            *out = 0;
            ++s.preparations;
            if (s.prepare_failure) {
                return CE_RENDER_BACKEND_ERROR;
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
                        return CE_RENDER_OK;
                    }
                }
                return CE_RENDER_BUSY;
            } catch (...) {
                return CE_RENDER_OUT_OF_MEMORY;
            }
        }

        static void release(void* ptr, uint64_t id) noexcept {
            static_cast<FakeBackend*>(ptr)->frames[id - 1].reset();
        }

        static ce_render_result encode(void* ptr, uint64_t frame, const ce_render_pass* p, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            *out = 0;
            ++s.encodes;
            if (p->native_pass_size != sizeof(Scope)) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            auto& scope = *static_cast<Scope*>(const_cast<void*>(p->native_pass));
            if (scope.schema != caseelephant::test_constants::render::fake_pass_schema || scope.commands) {
                return CE_RENDER_INVALID_ARGUMENT;
            }
            ce_render_info info{};
            info.struct_size = sizeof(info);
            s.reentrancy = ce_renderer_get_info(s.renderer, &info) == CE_RENDER_BUSY;
            if (s.atomic_failure) {
                return CE_RENDER_BACKEND_ERROR;
            }
            for (size_t i = 0; i < s.uses.size(); ++i) {
                if (!s.uses[i].resource) {
                    s.uses[i] = {s.frames[frame - 1], &scope, 0};
                    scope.commands = true;
                    *out = i + 1;
                    return s.partial_failure ? CE_RENDER_BACKEND_ERROR : CE_RENDER_OK;
                }
            }
            return CE_RENDER_BUSY;
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

        static ce_render_result shutdown(void* ptr) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            for (const auto& f : s.frames) {
                if (f) {
                    return CE_RENDER_BUSY;
                }
            }
            for (const auto& u : s.uses) {
                if (u.resource) {
                    return CE_RENDER_BUSY;
                }
            }
            return CE_RENDER_OK;
        }

        static void abandon(void* ptr) noexcept {
            auto& s = *static_cast<FakeBackend*>(ptr);
            for (auto& use : s.uses) {
                use = {};
            }
        }

        ce_render_backend table(uint32_t api = CE_RENDER_API_METAL) {
            return {sizeof(ce_render_backend),
                    CE_RENDER_ABI_VERSION,
                    api,
                    0,
                    7,
                    11,
                    {sizeof(ce_render_capabilities), CE_RENDER_FEATURE_SOLID | CE_RENDER_FEATURE_COVERAGE,
                     (1u << CE_RENDER_TARGET_SRGB_RGBA8) | (1u << CE_RENDER_TARGET_SRGB_BGRA8), 64, 128, 0},
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
        ce_render_asset asset{1, 1, 1, 1, CE_RENDER_COVERAGE, 0, &pixel, 1, 0, 0};
        ce_render_draw draw{0, 0, 16, 16, {1, 1, 1, 1}, 0, 1, {0, 0, 1, 1}, 0, {0, 0, 0, 0}};
        ce_render_frame view{sizeof(ce_render_frame), CE_RENDER_ABI_VERSION, &draw, 1, 1, &asset};
    };

    ce_render_info info(ce_renderer* r) {
        ce_render_info out{};
        out.struct_size = sizeof(out);
        EXPECT(ce_renderer_get_info(r, &out), OK);
        return out;
    }

    ce_render_pass pass(ce_renderer* r, Scope& scope) {
        return {sizeof(ce_render_pass),
                CE_RENDER_ABI_VERSION,
                info(r).identity,
                64,
                64,
                CE_RENDER_TARGET_SRGB_RGBA8,
                1,
                &scope,
                sizeof(scope)};
    }

    ce_renderer* create(FakeBackend& b, uint32_t api = CE_RENDER_API_METAL) {
        const auto table = b.table(api);
        ce_renderer* r{};
        EXPECT(ce_renderer_create(&table, nullptr, &r), OK);
        b.renderer = r;
        return r;
    }

    void IndependentBackendsAndSnapshotOwnership() {
        FakeBackend a;
        FakeBackend b;
        auto* metal = create(a);
        auto* custom = create(b, CE_RENDER_API_CUSTOM_BEGIN + 77);
        Frame f;
        ce_render_handle prepared{};
        EXPECT(ce_renderer_prepare(metal, &f.view, &prepared), OK);
        f.pixel = 0;
        Scope scope;
        auto p = pass(metal, scope);
        ce_render_handle use{};
        EXPECT(ce_renderer_encode(custom, prepared, &p, &use), WRONG_BACKEND);
        CHECK(!use.renderer_id && !b.encodes);
        p.identity = info(custom).identity;
        EXPECT(ce_renderer_encode(metal, prepared, &p, &use), WRONG_BACKEND);
        CHECK(!a.encodes);
        p = pass(metal, scope);
        ++p.identity.device_epoch;
        EXPECT(ce_renderer_encode(metal, prepared, &p, &use), WRONG_EPOCH);
        p = pass(metal, scope);
        EXPECT(ce_renderer_encode(metal, prepared, &p, &use), OK);
        CHECK(a.reentrancy);
        EXPECT(ce_renderer_release_prepared(metal, prepared), OK);
        CHECK(!a.frames[0] && (*a.uses[0].resource)[0] == 128);
        EXPECT(ce_renderer_release_prepared(metal, prepared), STALE_HANDLE);
        EXPECT(ce_renderer_notify_submitted(metal, use, 99, 4), INVALID_ARGUMENT);
        EXPECT(ce_renderer_notify_submitted(metal, use, 11, 0), INVALID_SERIAL);
        CHECK(info(metal).encoded_uses == 1);
        scope.submitted = true;
        deny_allocation = true;
        const auto submit = ce_renderer_notify_submitted(metal, use, 11, 4);
        const auto complete = ce_renderer_notify_completed(metal, 7, 11, 5);
        deny_allocation = false;
        EXPECT(submit, OK);
        EXPECT(complete, OK);
        CHECK(a.retirements == 1 && info(metal).completed_serial == 5);
        EXPECT(ce_renderer_discard_encoded(metal, use), STALE_HANDLE);
        EXPECT(ce_renderer_notify_completed(metal, 6, 11, 6), WRONG_EPOCH);
        EXPECT(ce_renderer_notify_completed(metal, 7, 11, 4), INVALID_SERIAL);
        EXPECT(ce_renderer_destroy(metal), OK);
        EXPECT(ce_renderer_destroy(custom), OK);
    }

    void ValidationBeforeNativeRecording() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        ce_render_handle h{};
        f.asset.encoding = CE_RENDER_SIGNED_DISTANCE;
        f.asset.sdf_spread = 8;
        EXPECT(ce_renderer_prepare(r, &f.view, &h), UNSUPPORTED_FEATURE);
        CHECK(!b.preparations && !h.renderer_id);
        f.asset.encoding = CE_RENDER_COVERAGE;
        f.asset.sdf_spread = 0;
        f.asset.byte_count = UINT64_MAX;
        EXPECT(ce_renderer_prepare(r, &f.view, &h), INVALID_ARGUMENT);
        f.asset.byte_count = 1;
        f.draw.asset_index = 2;
        EXPECT(ce_renderer_prepare(r, &f.view, &h), INVALID_ARGUMENT);
        f.draw.asset_index = 0;
        f.draw.opacity = 2;
        EXPECT(ce_renderer_prepare(r, &f.view, &h), INVALID_ARGUMENT);
        f.draw.opacity = 1;
        EXPECT(ce_renderer_prepare(r, &f.view, &h), OK);
        Scope scope;
        auto p = pass(r, scope);
        ce_render_handle use{};
        p.sample_count = 4;
        EXPECT(ce_renderer_encode(r, h, &p, &use), UNSUPPORTED_FEATURE);
        p.sample_count = 1;
        p.target_format = 99;
        EXPECT(ce_renderer_encode(r, h, &p, &use), UNSUPPORTED_FEATURE);
        p.target_format = 0;
        p.native_pass_size = 1;
        EXPECT(ce_renderer_encode(r, h, &p, &use), INVALID_ARGUMENT);
        CHECK(!use.renderer_id && !scope.commands);
        p.native_pass_size = sizeof(scope);
        scope.schema = 0;
        EXPECT(ce_renderer_encode(r, h, &p, &use), INVALID_ARGUMENT);
        scope.schema = caseelephant::test_constants::render::fake_pass_schema;
        ce_render_result cross_thread = CE_RENDER_OK;
        std::thread other([&] {
            cross_thread = ce_renderer_encode(r, h, &p, &use);
        });
        other.join();
        EXPECT(cross_thread, WRONG_THREAD);
        p.target_format = CE_RENDER_TARGET_SRGB_BGRA8;
        EXPECT(ce_renderer_encode(r, h, &p, &use), OK);
        scope.cancelled = true;
        EXPECT(ce_renderer_discard_encoded(r, use), OK);
        EXPECT(ce_renderer_release_prepared(r, h), OK);
        EXPECT(ce_renderer_destroy(r), OK);
    }

    void PartialRecordingFailureAndDrain() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        ce_render_handle h{};
        EXPECT(ce_renderer_prepare(r, &f.view, &h), OK);
        Scope scope;
        auto p = pass(r, scope);
        ce_render_handle use{};
        b.atomic_failure = true;
        EXPECT(ce_renderer_encode(r, h, &p, &use), BACKEND_ERROR);
        CHECK(!use.renderer_id && !scope.commands);
        b.atomic_failure = false;
        b.partial_failure = true;
        EXPECT(ce_renderer_encode(r, h, &p, &use), BACKEND_ERROR);
        CHECK(use.renderer_id && scope.commands && info(r).encoded_uses == 1);
        EXPECT(ce_renderer_destroy(r), BUSY);
        CHECK(info(r).draining);
        ce_render_handle rejected{};
        EXPECT(ce_renderer_prepare(r, &f.view, &rejected), SHUTTING_DOWN);
        EXPECT(ce_renderer_release_prepared(r, h), OK);
        scope.cancelled = true;
        EXPECT(ce_renderer_discard_encoded(r, use), OK);
        CHECK(b.discards == 1);
        EXPECT(ce_renderer_destroy(r), OK);
        // Partial commands that the host actually submitted must still retire.
        FakeBackend submitted;
        submitted.partial_failure = true;
        r = create(submitted);
        EXPECT(ce_renderer_prepare(r, &f.view, &h), OK);
        Scope live;
        p = pass(r, live);
        EXPECT(ce_renderer_encode(r, h, &p, &use), BACKEND_ERROR);
        live.submitted = true;
        EXPECT(ce_renderer_notify_submitted(r, use, 11, 10), OK);
        EXPECT(ce_renderer_discard_encoded(r, use), INVALID_STATE);
        EXPECT(ce_renderer_release_prepared(r, h), OK);
        EXPECT(ce_renderer_destroy(r), BUSY);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 10), OK);
        CHECK(submitted.retirements == 1);
        EXPECT(ce_renderer_destroy(r), OK);
    }

    void CapacityGenerationsAndSharedSubmission() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        ce_render_handle frame{};
        ce_render_handle second{};
        ce_render_handle overflow{};
        EXPECT(ce_renderer_prepare(r, &f.view, &frame), OK);
        EXPECT(ce_renderer_prepare(r, &f.view, &second), OK);
        EXPECT(ce_renderer_prepare(r, &f.view, &overflow), BUSY);
        CHECK(b.preparations == 2);
        EXPECT(ce_renderer_release_prepared(r, second), OK);
        EXPECT(ce_renderer_prepare(r, &f.view, &second), OK);
        CHECK(second.generation == 2);
        auto old = second;
        --old.generation;
        EXPECT(ce_renderer_release_prepared(r, old), STALE_HANDLE);
        std::array<Scope, 3> scopes;
        std::array<ce_render_handle, 3> uses{};
        for (size_t i = 0; i < 2; ++i) {
            auto p = pass(r, scopes[i]);
            EXPECT(ce_renderer_encode(r, frame, &p, &uses[i]), OK);
        }
        auto p = pass(r, scopes[2]);
        EXPECT(ce_renderer_encode(r, frame, &p, &uses[2]), BUSY);
        CHECK(!scopes[2].commands);
        for (size_t i = 0; i < 2; ++i) {
            scopes[i].submitted = true;
            EXPECT(ce_renderer_notify_submitted(r, uses[i], 11, 20), OK);
        }
        EXPECT(ce_renderer_encode(r, frame, &p, &uses[2]), OK);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 21), OK);
        CHECK(b.retirements == 2 && info(r).encoded_uses == 1);
        scopes[2].submitted = true;
        EXPECT(ce_renderer_notify_submitted(r, uses[2], 11, 20), INVALID_SERIAL);
        EXPECT(ce_renderer_notify_submitted(r, uses[2], 11, 22), OK);
        EXPECT(ce_renderer_release_prepared(r, frame), OK);
        EXPECT(ce_renderer_release_prepared(r, second), OK);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 22), OK);
        EXPECT(ce_renderer_destroy(r), OK);
    }

    void AbandonRequiresHostProofAndCancelledScopes() {
        FakeBackend b;
        auto* r = create(b);
        Frame f;
        ce_render_handle frame{};
        ce_render_handle use{};
        EXPECT(ce_renderer_prepare(r, &f.view, &frame), OK);
        Scope scope;
        auto p = pass(r, scope);
        EXPECT(ce_renderer_encode(r, frame, &p, &use), OK);
        EXPECT(ce_renderer_abandon_device(r, 7, 0), HOST_CONFIRMATION_REQUIRED);
        EXPECT(ce_renderer_abandon_device(r, 7, 1), BUSY);
        scope.submitted = true;
        EXPECT(ce_renderer_notify_submitted(r, use, 11, 2), OK);
        EXPECT(ce_renderer_abandon_device(r, 8, 1), WRONG_EPOCH);
        EXPECT(ce_renderer_abandon_device(r, 7, 1), OK);
        CHECK(!info(r).device_available && !info(r).submitted_uses);
        ce_render_handle next{};
        EXPECT(ce_renderer_prepare(r, &f.view, &next), DEVICE_UNAVAILABLE);
        EXPECT(ce_renderer_encode(r, frame, &p, &next), DEVICE_UNAVAILABLE);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 2), DEVICE_UNAVAILABLE);
        EXPECT(ce_renderer_release_prepared(r, frame), OK);
        EXPECT(ce_renderer_destroy(r), OK);
    }

    void InFlightCapacityReservedBeforeNativeCommands() {
        FakeBackend b;
        const auto table = b.table();
        ce_renderer* r{};
        const ce_renderer_desc limits{sizeof(ce_renderer_desc), CE_RENDER_ABI_VERSION, 2, 2, 2, 8, 2, 0, 16};
        EXPECT(ce_renderer_create(&table, &limits, &r), OK);
        b.renderer = r;
        Frame f;
        ce_render_handle frame{};
        EXPECT(ce_renderer_prepare(r, &f.view, &frame), OK);
        std::array<Scope, 3> scopes;
        std::array<ce_render_handle, 3> uses{};
        for (size_t i = 0; i < 2; ++i) {
            auto p = pass(r, scopes[i]);
            EXPECT(ce_renderer_encode(r, frame, &p, &uses[i]), OK);
            scopes[i].submitted = true;
            EXPECT(ce_renderer_notify_submitted(r, uses[i], 11, 5), OK);
        }
        const auto before = b.encodes;
        auto p = pass(r, scopes[2]);
        EXPECT(ce_renderer_encode(r, frame, &p, &uses[2]), BUSY);
        CHECK(b.encodes == before && !scopes[2].commands);
        EXPECT(ce_renderer_release_prepared(r, frame), OK);
        EXPECT(ce_renderer_destroy(r), BUSY);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 5), OK);
        CHECK(b.retirements == 2);
        EXPECT(ce_renderer_destroy(r), OK);
    }

    struct FakeExternal {
        FakeBackend& backend;
        std::array<bool, 3> native{};
        unsigned imports{};
        unsigned releases{};
        unsigned snapshots{};
        bool fail_import{};
        bool fail_prepare{};

        static ce_render_result import_texture(void* p, const ce_render_external_texture*, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeExternal*>(p);
            *out = 0;
            if (s.fail_import) {
                return CE_RENDER_BACKEND_ERROR;
            }
            for (size_t i = 0; i < s.native.size(); ++i) {
                if (!s.native[i]) {
                    s.native[i] = true;
                    *out = i + 1;
                    ++s.imports;
                    return CE_RENDER_OK;
                }
            }
            return CE_RENDER_BUSY;
        }

        static void release_texture(void* p, uint64_t id) noexcept {
            auto& s = *static_cast<FakeExternal*>(p);
            if (!s.native[id - 1]) {
                std::terminate();
            }
            s.native[id - 1] = false;
            ++s.releases;
        }

        static ce_render_result prepare(void* p, const ce_render_frame* f, const ce_render_external_asset* assets,
                                        uint32_t count, uint64_t* out) noexcept {
            auto& s = *static_cast<FakeExternal*>(p);
            *out = 0;
            if (s.fail_prepare) {
                return CE_RENDER_BACKEND_ERROR;
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

        ce_render_external_backend table() {
            return {sizeof(ce_render_external_backend),
                    1,
                    1u << CE_RENDER_TARGET_LINEAR_RGBA32F,
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
        ce_renderer* renderer{};
        unsigned calls{};
        unsigned reason{};
        bool reentry_rejected{};

        static void callback(void* p, uint32_t reason) noexcept {
            auto& s = *static_cast<Retirement*>(p);
            ++s.calls;
            s.reason = reason;
            ce_render_external_info data{};
            data.struct_size = sizeof(data);
            s.reentry_rejected = ce_renderer_get_external_info(s.renderer, &data) == CE_RENDER_BUSY;
        }
    };

    ce_render_external_texture external_description(ce_renderer* r, Retirement& retired, uint64_t version = 1) {
        static int native_schema;
        return {sizeof(ce_render_external_texture),
                1,
                info(r).identity,
                99,
                version,
                2,
                2,
                CE_RENDER_TARGET_LINEAR_RGBA32F,
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

    ce_render_external_frame external_frame(Frame& f, const ce_render_handle* textures, uint32_t count = 1) {
        f.draw.asset_index = 1;
        return {sizeof(ce_render_external_frame), 1, f.view, count, 0, textures};
    }

    ce_render_external_info external_info(ce_renderer* r) {
        ce_render_external_info out{};
        out.struct_size = sizeof(out);
        EXPECT(ce_renderer_get_external_info(r, &out), OK);
        return out;
    }

    void ExternalValidationAndIndependentPins() {
        FakeBackend b;
        auto* r = create(b);
        FakeExternal ext{b};
        Retirement retired{r};
        auto d = external_description(r, retired);
        ce_render_handle texture{};
        ce_render_handle second{};
        ce_render_handle failed{};
        EXPECT(ce_renderer_import_texture(r, &d, &texture), UNSUPPORTED_FEATURE);
        auto table = ext.table();
        auto bad = table;
        bad.formats = 1u << CE_RENDER_TARGET_SRGB_RGBA8;
        EXPECT(ce_renderer_install_external_backend(r, &bad), INVALID_ARGUMENT);
        deny_allocation = true;
        EXPECT(ce_renderer_install_external_backend(r, &table), OUT_OF_MEMORY);
        deny_allocation = false;
        EXPECT(ce_renderer_install_external_backend(r, &table), OK);
        EXPECT(ce_renderer_install_external_backend(r, &table), INVALID_STATE);
        auto invalid = d;
        ++invalid.identity.device_epoch;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), WRONG_EPOCH);
        invalid = d;
        ++invalid.identity.renderer_id;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), WRONG_BACKEND);
        invalid = d;
        ++invalid.identity.queue;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), INVALID_ARGUMENT);
        invalid = d;
        invalid.synchronization = 1;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.encoding = 1;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.width = 65;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), UNSUPPORTED_FEATURE);
        invalid = d;
        invalid.width = 0;
        EXPECT(ce_renderer_import_texture(r, &invalid, &failed), INVALID_ARGUMENT);
        ext.fail_import = true;
        EXPECT(ce_renderer_import_texture(r, &d, &failed), BACKEND_ERROR);
        ext.fail_import = false;
        CHECK(!retired.calls && !external_info(r).live_textures);
        EXPECT(ce_renderer_import_texture(r, &d, &texture), OK);
        EXPECT(ce_renderer_import_texture(r, &d, &failed), INVALID_STATE);
        d.content_version = 2;
        EXPECT(ce_renderer_import_texture(r, &d, &second), OK);
        d.content_version = 3;
        EXPECT(ce_renderer_import_texture(r, &d, &failed), BUSY);
        CHECK(external_info(r).texture_bytes == 128);
        EXPECT(ce_renderer_release_texture(r, second), OK);
        CHECK(retired.calls == 1);
        Frame f;
        auto input = external_frame(f, &texture);
        ce_render_handle frame{};
        ce_render_handle use{};
        ce_render_handle another{};
        auto wrong = texture;
        ++wrong.device_epoch;
        input.external_textures = &wrong;
        EXPECT(ce_renderer_prepare_external(r, &input, &frame), WRONG_EPOCH);
        input.external_textures = &texture;
        f.draw.sdf_stroke = 1;
        EXPECT(ce_renderer_prepare_external(r, &input, &frame), INVALID_ARGUMENT);
        f.draw.sdf_stroke = 0;
        ext.fail_prepare = true;
        EXPECT(ce_renderer_prepare_external(r, &input, &frame), BACKEND_ERROR);
        ext.fail_prepare = false;
        EXPECT(ce_renderer_prepare_external(r, &input, &frame), OK);
        EXPECT(ce_renderer_release_texture(r, texture), OK);
        EXPECT(ce_renderer_release_texture(r, texture), STALE_HANDLE);
        CHECK(retired.calls == 1 && external_info(r).caller_leases == 0 && external_info(r).live_textures == 1);
        EXPECT(ce_renderer_prepare_external(r, &input, &another), STALE_HANDLE);
        std::array<Scope, 2> scopes;
        auto p = pass(r, scopes[0]);
        EXPECT(ce_renderer_encode(r, frame, &p, &use), OK);
        p = pass(r, scopes[1]);
        EXPECT(ce_renderer_encode(r, frame, &p, &another), OK);
        EXPECT(ce_renderer_release_prepared(r, frame), OK);
        CHECK(retired.calls == 1);
        EXPECT(ce_renderer_notify_submitted(r, use, 12, 1), INVALID_ARGUMENT);
        scopes[0].submitted = true;
        scopes[1].submitted = true;
        deny_allocation = true;
        EXPECT(ce_renderer_notify_submitted(r, use, 11, 1), OK);
        EXPECT(ce_renderer_notify_submitted(r, another, 11, 2), OK);
        EXPECT(ce_renderer_notify_completed(r, 8, 11, 2), WRONG_EPOCH);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 1), OK);
        CHECK(retired.calls == 1);
        EXPECT(ce_renderer_notify_completed(r, 7, 11, 2), OK);
        deny_allocation = false;
        CHECK(retired.calls == 2 && retired.reason == CE_RENDER_EXTERNAL_RETIRED && retired.reentry_rejected &&
              ext.releases == 2);
        CHECK(!external_info(r).texture_bytes);
        EXPECT(ce_renderer_destroy(r), OK);
    }

    void ExternalMutationFailureAndAbandon() {
        FakeBackend b;
        auto* r = create(b);
        FakeExternal ext{b};
        const auto table = ext.table();
        EXPECT(ce_renderer_install_external_backend(r, &table), OK);
        Retirement retired{r};
        Frame f;
        uint64_t serial{};
        uint32_t random = caseelephant::test_constants::render::external_mutation_seed;
        for (unsigned i = 0; i < caseelephant::test_constants::render::external_mutation_iterations; ++i) {
            auto d = external_description(r, retired, i + 1);
            ce_render_handle texture{};
            ce_render_handle frame{};
            ce_render_handle use{};
            EXPECT(ce_renderer_import_texture(r, &d, &texture), OK);
            auto input = external_frame(f, &texture);
            EXPECT(ce_renderer_prepare_external(r, &input, &frame), OK);
            random = random * caseelephant::constants::lcg_multiplier + caseelephant::constants::lcg_increment;
            const auto mode = random % 4;
            Scope scope;
            auto p = pass(r, scope);
            b.atomic_failure = mode == 0;
            b.partial_failure = mode == 1;
            EXPECT(ce_renderer_release_texture(r, texture), OK);
            CHECK(ce_renderer_encode(r, frame, &p, &use) == (mode < 2 ? CE_RENDER_BACKEND_ERROR : CE_RENDER_OK));
            EXPECT(ce_renderer_release_prepared(r, frame), OK);
            if (mode == 0) {
                CHECK(retired.calls == i + 1 && !use.renderer_id);
            } else {
                CHECK(retired.calls == i);
                if (mode < 3) {
                    scope.cancelled = true;
                    EXPECT(ce_renderer_discard_encoded(r, use), OK);
                } else {
                    scope.submitted = true;
                    EXPECT(ce_renderer_notify_submitted(r, use, 11, ++serial), OK);
                    deny_allocation = true;
                    EXPECT(ce_renderer_notify_completed(r, 7, 11, serial), OK);
                    deny_allocation = false;
                }
            }
            CHECK(retired.calls == i + 1 && !external_info(r).live_textures);
        }
        b.atomic_failure = false;
        b.partial_failure = false;
        auto d =
            external_description(r, retired, caseelephant::test_constants::render::external_mutation_iterations + 1);
        ce_render_handle texture{};
        ce_render_handle frame{};
        ce_render_handle use{};
        EXPECT(ce_renderer_import_texture(r, &d, &texture), OK);
        auto input = external_frame(f, &texture);
        EXPECT(ce_renderer_prepare_external(r, &input, &frame), OK);
        Scope scope;
        auto p = pass(r, scope);
        EXPECT(ce_renderer_encode(r, frame, &p, &use), OK);
        EXPECT(ce_renderer_release_texture(r, texture), OK);
        scope.submitted = true;
        EXPECT(ce_renderer_notify_submitted(r, use, 11, ++serial), OK);
        EXPECT(ce_renderer_abandon_device(r, 7, 0), HOST_CONFIRMATION_REQUIRED);
        EXPECT(ce_renderer_abandon_device(r, 7, 1), OK);
        CHECK(retired.calls == 512);
        EXPECT(ce_renderer_release_prepared(r, frame), OK);
        CHECK(retired.calls == 513 && retired.reason == CE_RENDER_EXTERNAL_ABANDONED);
        EXPECT(ce_renderer_destroy(r), OK);
        FakeBackend another;
        auto* next = create(another);
        FakeExternal next_ext{another};
        const auto next_table = next_ext.table();
        EXPECT(ce_renderer_install_external_backend(next, &next_table), OK);
        Retirement next_retired{next};
        d = external_description(next, next_retired);
        EXPECT(ce_renderer_import_texture(next, &d, &texture), OK);
        ce_render_result result{};
        std::thread worker([&] {
            result = ce_renderer_release_texture(next, texture);
        });
        worker.join();
        CHECK(result == CE_RENDER_WRONG_THREAD);
        EXPECT(ce_renderer_destroy(next), BUSY);
        ce_render_handle denied{};
        EXPECT(ce_renderer_import_texture(next, &d, &denied), SHUTTING_DOWN);
        EXPECT(ce_renderer_release_texture(next, texture), OK);
        CHECK(next_retired.calls == 1);
        EXPECT(ce_renderer_destroy(next), OK);
    }
} // namespace

int main() {
    try {
        IndependentBackendsAndSnapshotOwnership();
        ValidationBeforeNativeRecording();
        PartialRecordingFailureAndDrain();
        CapacityGenerationsAndSharedSubmission();
        AbandonRequiresHostProofAndCancelledScopes();
        InFlightCapacityReservedBeforeNativeCommands();
        ExternalValidationAndIndependentPins();
        ExternalMutationFailureAndAbandon();
        std::cout
            << "PASS backend ABI: native scope isolation, snapshot/pins, partial failure, no-allocation submission, drain and generations\n";
        return 0;
    } catch (const std::exception& e) {
        deny_allocation = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
