#include "render/backend.hpp"
#include "support/check.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

// RenderBackend concept and the generated C tables: what satisfies the concept, how make_c_backend fills
// weft_render_backend, the thunks' error policy, and a full prepare/encode/submit/complete/destroy cycle of a
// concept backend through the renderer core.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)
#define EXPECT(v, result) CHECK((v) == WEFT_RENDER_##result)

    constexpr std::uint64_t epoch = 7;
    constexpr std::uint64_t queue = 11;

    // One prepared frame and one encoded use at a time; counts every callback.
    struct CountingBackend {
        unsigned prepares{};
        unsigned releases{};
        unsigned encodes{};
        unsigned discards{};
        unsigned submits{};
        unsigned completions{};
        unsigned abandons{};
        unsigned shutdowns{};
        bool frame_live{};
        bool use_live{};
        std::uint64_t use_serial{};
        std::uint64_t completed_serial{};
        bool throw_prepare{};
        bool throw_encode{};
        bool throw_shutdown{};

        weft_render_result prepare(const weft_render_frame& frame, std::uint64_t& out) {
            ++prepares;
            if (throw_prepare) {
                out = 5; // the thunk must not leak a handle on an exception
                throw std::runtime_error("prepare failed");
            }
            if (frame_live || frame.draw_count != 1) {
                return WEFT_RENDER_BUSY;
            }
            frame_live = true;
            out = 1;
            return WEFT_RENDER_OK;
        }

        void release_prepared(std::uint64_t native) {
            CHECK(native == 1 && frame_live);
            frame_live = false;
            ++releases;
        }

        weft_render_result encode(std::uint64_t native, const weft_render_pass& pass, std::uint64_t& out) {
            ++encodes;
            if (throw_encode) {
                out = 9;
                throw std::runtime_error("encode failed");
            }
            if (native != 1 || pass.width != 32 || use_live) {
                return WEFT_RENDER_INVALID_ARGUMENT;
            }
            use_live = true;
            out = 3;
            return WEFT_RENDER_OK;
        }

        void discard_encoded(std::uint64_t native) {
            CHECK(native == 3 && use_live);
            use_live = false;
            ++discards;
        }

        void submitted(std::uint64_t native, std::uint64_t serial) {
            CHECK(native == 3 && use_live);
            use_serial = serial;
            ++submits;
        }

        void completed(std::uint64_t serial) {
            completed_serial = serial;
            if (use_live && use_serial && use_serial <= serial) {
                use_live = false;
            }
            ++completions;
        }

        void abandon() {
            use_live = false;
            ++abandons;
        }

        weft_render_result shutdown() {
            ++shutdowns;
            if (throw_shutdown) {
                throw std::runtime_error("shutdown failed");
            }
            return frame_live || use_live ? WEFT_RENDER_BUSY : WEFT_RENDER_OK;
        }
    };

    struct ExternalBackend : CountingBackend {
        unsigned imports{};
        unsigned texture_releases{};
        bool throw_import{};

        weft_render_result import_texture(const weft_render_external_texture&, std::uint64_t& out) {
            ++imports;
            if (throw_import) {
                out = 4;
                throw std::runtime_error("import failed");
            }
            out = 1;
            return WEFT_RENDER_OK;
        }

        void release_texture(std::uint64_t) {
            ++texture_releases;
        }

        weft_render_result prepare_external(const weft_render_frame& frame, const weft_render_external_asset*,
                                            std::uint32_t, std::uint64_t& out) {
            return prepare(frame, out);
        }
    };

    // Lacks shutdown.
    struct Incomplete {
        weft_render_result prepare(const weft_render_frame&, std::uint64_t&);
        void release_prepared(std::uint64_t);
        weft_render_result encode(std::uint64_t, const weft_render_pass&, std::uint64_t&);
        void discard_encoded(std::uint64_t);
        void submitted(std::uint64_t, std::uint64_t);
        void completed(std::uint64_t);
        void abandon();
    };

    // prepare reports success as bool instead of weft_render_result.
    struct WrongResult : CountingBackend {
        bool prepare(const weft_render_frame&, std::uint64_t&);
    };

    static_assert(weft::render::RenderBackend<CountingBackend>);
    static_assert(!weft::render::ExternalTextureBackend<CountingBackend>);
    static_assert(weft::render::ExternalTextureBackend<ExternalBackend>);
    static_assert(!weft::render::RenderBackend<Incomplete>);
    static_assert(!weft::render::RenderBackend<WrongResult>);

    weft::render::BackendDescription description() {
        return {WEFT_RENDER_API_CUSTOM_BEGIN + 5,
                epoch,
                queue,
                {sizeof(weft_render_capabilities), WEFT_RENDER_FEATURE_SOLID, 1u << WEFT_RENDER_TARGET_SRGB_RGBA8, 64,
                 64, 0}};
    }

    struct Frame {
        weft_render_draw draw{0, 0, 16, 16, {1, 1, 1, 1}, WEFT_RENDER_SOLID_ASSET, 1, {0, 0, 1, 1}, 0, {0, 0, 0, 0}};
        weft_render_frame view{sizeof(weft_render_frame), WEFT_RENDER_ABI_VERSION, &draw, 1, 0, nullptr};
    };

    weft_render_pass pass(weft_renderer* r, int& payload) {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        EXPECT(weft_renderer_get_info(r, &info), OK);
        return {sizeof(weft_render_pass),
                WEFT_RENDER_ABI_VERSION,
                info.identity,
                32,
                32,
                WEFT_RENDER_TARGET_SRGB_RGBA8,
                1,
                &payload,
                sizeof(payload)};
    }

    void TableCarriesDescriptionAndBackend() {
        CountingBackend backend;
        const auto table = weft::render::make_c_backend(backend, description());
        CHECK(table.struct_size == sizeof(weft_render_backend) && table.abi_version == WEFT_RENDER_ABI_VERSION);
        CHECK(table.api == WEFT_RENDER_API_CUSTOM_BEGIN + 5 && !table.reserved);
        CHECK(table.device_epoch == epoch && table.queue == queue);
        CHECK(table.capabilities.features == WEFT_RENDER_FEATURE_SOLID && table.capabilities.max_target_size == 64);
        CHECK(table.user_data == &backend);
        CHECK(table.prepare && table.release_prepared && table.encode && table.discard_encoded && table.submitted &&
              table.completed && table.abandon && table.shutdown);
    }

    void ThunksTurnExceptionsIntoErrors() {
        CountingBackend backend;
        const auto table = weft::render::make_c_backend(backend, description());
        Frame frame;
        std::uint64_t handle = 99;
        backend.throw_prepare = true;
        EXPECT(table.prepare(table.user_data, &frame.view, &handle), BACKEND_ERROR);
        CHECK(handle == 0 && backend.prepares == 1);
        backend.throw_prepare = false;
        EXPECT(table.prepare(table.user_data, &frame.view, &handle), OK);
        CHECK(handle == 1);
        int payload{};
        const weft_render_pass p{sizeof(weft_render_pass), WEFT_RENDER_ABI_VERSION, {}, 32, 32, 0, 1, &payload,
                                 sizeof(payload)};
        std::uint64_t use = 99;
        backend.throw_encode = true;
        EXPECT(table.encode(table.user_data, handle, &p, &use), BACKEND_ERROR);
        CHECK(use == 0);
        backend.throw_encode = false;
        table.release_prepared(table.user_data, handle);
        backend.throw_shutdown = true;
        EXPECT(table.shutdown(table.user_data), BACKEND_ERROR);
        backend.throw_shutdown = false;
        EXPECT(table.shutdown(table.user_data), OK);
        CHECK(backend.releases == 1 && backend.shutdowns == 2);
    }

    void FrameCycleThroughTheCore() {
        CountingBackend backend;
        const auto table = weft::render::make_c_backend(backend, description());
        weft_renderer* r{};
        EXPECT(weft_renderer_create(&table, nullptr, &r), OK);
        Frame frame;
        weft_render_handle prepared{};
        EXPECT(weft_renderer_prepare(r, &frame.view, &prepared), OK);
        int payload{};
        auto p = pass(r, payload);
        weft_render_handle use{};
        // A throwing encode reaches the core as a complete rollback: no use, frame still prepared.
        backend.throw_encode = true;
        EXPECT(weft_renderer_encode(r, prepared, &p, &use), BACKEND_ERROR);
        CHECK(!use.renderer_id && !backend.use_live);
        backend.throw_encode = false;
        EXPECT(weft_renderer_encode(r, prepared, &p, &use), OK);
        EXPECT(weft_renderer_release_prepared(r, prepared), OK);
        EXPECT(weft_renderer_notify_submitted(r, use, queue, 4), OK);
        EXPECT(weft_renderer_notify_completed(r, epoch, queue, 5), OK);
        CHECK(backend.submits == 1 && backend.completed_serial == 5 && !backend.use_live);
        // Cancel path: encode, then discard before any submission.
        EXPECT(weft_renderer_prepare(r, &frame.view, &prepared), OK);
        EXPECT(weft_renderer_encode(r, prepared, &p, &use), OK);
        EXPECT(weft_renderer_discard_encoded(r, use), OK);
        EXPECT(weft_renderer_release_prepared(r, prepared), OK);
        CHECK(backend.discards == 1 && backend.releases == 2);
        EXPECT(weft_renderer_destroy(r), OK);
        CHECK(backend.shutdowns == 1 && !backend.frame_live && !backend.use_live);
    }

    void ExternalTableAndPolicy() {
        ExternalBackend backend;
        const auto table =
            weft::render::make_c_external_backend(backend, {1u << WEFT_RENDER_TARGET_LINEAR_RGBA8, 4, 1024});
        CHECK(table.struct_size == sizeof(weft_render_external_backend) &&
              table.abi_version == WEFT_RENDER_EXTERNAL_ABI_VERSION);
        CHECK(table.formats == 1u << WEFT_RENDER_TARGET_LINEAR_RGBA8 && table.max_textures == 4 &&
              table.max_texture_bytes == 1024 && !table.reserved && !table.reserved2);
        CHECK(table.user_data == &backend && table.import_texture && table.release_texture && table.prepare);
        weft_render_external_texture texture{};
        std::uint64_t native = 99;
        backend.throw_import = true;
        EXPECT(table.import_texture(table.user_data, &texture, &native), BACKEND_ERROR);
        CHECK(native == 0 && backend.imports == 1);
        backend.throw_import = false;
        EXPECT(table.import_texture(table.user_data, &texture, &native), OK);
        table.release_texture(table.user_data, native);
        CHECK(backend.texture_releases == 1);
        // The core accepts the generated extension table.
        const auto main = weft::render::make_c_backend(backend, description());
        weft_renderer* r{};
        EXPECT(weft_renderer_create(&main, nullptr, &r), OK);
        EXPECT(weft_renderer_install_external_backend(r, &table), OK);
        EXPECT(weft_renderer_destroy(r), OK);
    }
} // namespace

int main() {
    try {
        TableCarriesDescriptionAndBackend();
        ThunksTurnExceptionsIntoErrors();
        FrameCycleThroughTheCore();
        ExternalTableAndPolicy();
        std::cout << "PASS render backend concept: tables, thunk error policy, core frame cycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
