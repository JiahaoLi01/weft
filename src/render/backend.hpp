#pragma once

#include "weft/render_backend.h"
#include "weft/render_external_texture.h"

#include <concepts>
#include <cstdint>

// Static polymorphism inside, one function table at the C ABI. A backend is a concrete C++ type that
// satisfies RenderBackend; make_c_backend generates the noexcept thunks the renderer core
// (render_backend.cpp) calls through weft_render_backend. No virtual dispatch and no std::function.
//
// Error policy of the generated thunks:
//   - prepare and encode return weft_render_result. Out handles are zeroed before the call; an exception
//     escaping the backend becomes WEFT_RENDER_BACKEND_ERROR with the out handle zeroed again (complete
//     rollback is the backend's duty, as for any error result).
//   - release_prepared, discard_encoded, submitted, completed and abandon cannot fail by contract: the thunk
//     is noexcept, so an exception terminates instead of corrupting GPU lifetime state.
//   - shutdown returns WEFT_RENDER_BACKEND_ERROR for an exception.
namespace weft::render {
    template <class B>
    concept RenderBackend = requires(B& backend, const weft_render_frame& frame, const weft_render_pass& pass,
                                     std::uint64_t handle, std::uint64_t serial, std::uint64_t& out) {
        {
            backend.prepare(frame, out)
        } -> std::same_as<weft_render_result>;
        {
            backend.release_prepared(handle)
        } -> std::same_as<void>;
        {
            backend.encode(handle, pass, out)
        } -> std::same_as<weft_render_result>;
        {
            backend.discard_encoded(handle)
        } -> std::same_as<void>;
        {
            backend.submitted(handle, serial)
        } -> std::same_as<void>;
        {
            backend.completed(serial)
        } -> std::same_as<void>;
        {
            backend.abandon()
        } -> std::same_as<void>;
        {
            backend.shutdown()
        } -> std::same_as<weft_render_result>;
    };

    // Optional host texture extension (render_external_texture.h).
    template <class B>
    concept ExternalTextureBackend =
        RenderBackend<B> && requires(B& backend, const weft_render_external_texture& texture,
                                     const weft_render_frame& frame, const weft_render_external_asset* assets,
                                     std::uint32_t count, std::uint64_t handle, std::uint64_t& out) {
            {
                backend.import_texture(texture, out)
            } -> std::same_as<weft_render_result>;
            {
                backend.release_texture(handle)
            } -> std::same_as<void>;
            {
                backend.prepare_external(frame, assets, count, out)
            } -> std::same_as<weft_render_result>;
        };

    // What a backend reports about its device: weft_render_backend's api, device_epoch, queue and capabilities.
    struct BackendDescription {
        std::uint32_t api{};
        std::uint64_t device_epoch{};
        std::uint64_t queue{};
        weft_render_capabilities capabilities{};
    };

    // Formats, texture count and byte budget of the external texture extension.
    struct ExternalTextureLimits {
        std::uint32_t formats{};
        std::uint32_t max_textures{};
        std::uint64_t max_texture_bytes{};
    };

    namespace detail {
        template <RenderBackend B>
        struct BackendThunks {
            static B& self(void* user_data) noexcept {
                return *static_cast<B*>(user_data);
            }

            static weft_render_result prepare(void* user_data, const weft_render_frame* frame,
                                              std::uint64_t* native_frame) noexcept {
                *native_frame = 0;
                try {
                    return self(user_data).prepare(*frame, *native_frame);
                } catch (...) {
                    *native_frame = 0;
                    return WEFT_RENDER_BACKEND_ERROR;
                }
            }

            static void release_prepared(void* user_data, std::uint64_t native_frame) noexcept {
                self(user_data).release_prepared(native_frame);
            }

            static weft_render_result encode(void* user_data, std::uint64_t native_frame, const weft_render_pass* pass,
                                             std::uint64_t* native_use) noexcept {
                *native_use = 0;
                try {
                    return self(user_data).encode(native_frame, *pass, *native_use);
                } catch (...) {
                    *native_use = 0;
                    return WEFT_RENDER_BACKEND_ERROR;
                }
            }

            static void discard_encoded(void* user_data, std::uint64_t native_use) noexcept {
                self(user_data).discard_encoded(native_use);
            }

            static void submitted(void* user_data, std::uint64_t native_use, std::uint64_t serial) noexcept {
                self(user_data).submitted(native_use, serial);
            }

            static void completed(void* user_data, std::uint64_t serial) noexcept {
                self(user_data).completed(serial);
            }

            static void abandon(void* user_data) noexcept {
                self(user_data).abandon();
            }

            static weft_render_result shutdown(void* user_data) noexcept {
                try {
                    return self(user_data).shutdown();
                } catch (...) {
                    return WEFT_RENDER_BACKEND_ERROR;
                }
            }
        };

        template <ExternalTextureBackend B>
        struct ExternalThunks {
            static weft_render_result import_texture(void* user_data, const weft_render_external_texture* texture,
                                                     std::uint64_t* native_texture) noexcept {
                *native_texture = 0;
                try {
                    return BackendThunks<B>::self(user_data).import_texture(*texture, *native_texture);
                } catch (...) {
                    *native_texture = 0;
                    return WEFT_RENDER_BACKEND_ERROR;
                }
            }

            static void release_texture(void* user_data, std::uint64_t native_texture) noexcept {
                BackendThunks<B>::self(user_data).release_texture(native_texture);
            }

            static weft_render_result prepare(void* user_data, const weft_render_frame* frame,
                                              const weft_render_external_asset* assets, std::uint32_t count,
                                              std::uint64_t* native_frame) noexcept {
                *native_frame = 0;
                try {
                    return BackendThunks<B>::self(user_data).prepare_external(*frame, assets, count, *native_frame);
                } catch (...) {
                    *native_frame = 0;
                    return WEFT_RENDER_BACKEND_ERROR;
                }
            }
        };
    } // namespace detail

    // The C function table for backend; it borrows backend until weft_renderer_destroy succeeds.
    template <RenderBackend B>
    weft_render_backend make_c_backend(B& backend, const BackendDescription& description) noexcept {
        using Thunks = detail::BackendThunks<B>;
        return {sizeof(weft_render_backend),
                WEFT_RENDER_ABI_VERSION,
                description.api,
                0,
                description.device_epoch,
                description.queue,
                description.capabilities,
                &backend,
                Thunks::prepare,
                Thunks::release_prepared,
                Thunks::encode,
                Thunks::discard_encoded,
                Thunks::submitted,
                Thunks::completed,
                Thunks::abandon,
                Thunks::shutdown};
    }

    // The external texture extension table for backend, for weft_renderer_install_external_backend.
    template <ExternalTextureBackend B>
    weft_render_external_backend make_c_external_backend(B& backend, const ExternalTextureLimits& limits) noexcept {
        using Thunks = detail::ExternalThunks<B>;
        return {sizeof(weft_render_external_backend),
                WEFT_RENDER_EXTERNAL_ABI_VERSION,
                limits.formats,
                0,
                limits.max_textures,
                0,
                limits.max_texture_bytes,
                &backend,
                Thunks::import_texture,
                Thunks::release_texture,
                Thunks::prepare};
    }
} // namespace weft::render
