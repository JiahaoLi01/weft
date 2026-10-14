#include "render/backends/vulkan/backend.hpp"

#include "render/frame_input.hpp"
#include "render/quad_list.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace weft::render::vulkan {
    namespace {
        namespace limits = weft::constants::render::vulkan;

        void render_result(weft_render_result result, const char* operation) {
            if (result != WEFT_RENDER_OK) {
                throw std::runtime_error(std::string(operation) + " result=" + std::to_string(result));
            }
        }

        // The formats the adapter draws, in their weft_render_target_format order.
        constexpr VkFormat formats[]{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                     VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_UNDEFINED, VK_FORMAT_R16G16B16A16_SFLOAT};

        void log_failure(const char* operation) noexcept {
            try {
                throw;
            } catch (const std::exception& error) {
                try {
                    std::cerr << "Vulkan " << operation << ": " << error.what() << '\n';
                } catch (...) {
                }
            } catch (...) {
            }
        }
    } // namespace

    VkFormat target_format(std::uint32_t format) {
        return format < std::size(formats) ? formats[format] : VK_FORMAT_UNDEFINED;
    }

    Backend::Backend(Renderer& device) : gpu(device) {
        const auto table = make_c_backend(*this, {WEFT_RENDER_API_VULKAN,
                                                  std::uint64_t(limits::device_epoch),
                                                  std::uint64_t(limits::queue),
                                                  {sizeof(weft_render_capabilities), limits::features, target_formats(),
                                                   limits::max_surface_dimension, limits::max_surface_dimension, 0}});
        const weft_renderer_desc desc{sizeof(weft_renderer_desc),
                                      WEFT_RENDER_ABI_VERSION,
                                      limits::max_prepared_frames,
                                      limits::frames_in_flight,
                                      limits::frames_in_flight,
                                      weft::constants::render::default_max_draws,
                                      weft::constants::render::default_max_assets,
                                      0,
                                      limits::max_frame_asset_bytes};
        render_result(weft_renderer_create(&table, &desc, &renderer), "create Vulkan backend interface");
    }

    Backend::~Backend() {
        if (weft_renderer_destroy(renderer) != WEFT_RENDER_OK) {
            std::terminate();
        }
    }

    std::uint32_t Backend::target_formats() const {
        std::uint32_t out{};
        for (std::uint32_t i = 0; i < std::size(formats); ++i) {
            if (formats[i] != VK_FORMAT_UNDEFINED && gpu.gpu.blends(formats[i])) {
                out |= 1u << i;
            }
        }
        return out;
    }

    weft_render_pass Backend::pass(VkCommandBuffer command, VkImageView target_view, weft_render_target_format format,
                                   weft_render_vulkan_pass& payload) const {
        weft_render_info info{};
        info.struct_size = sizeof(info);
        render_result(weft_renderer_get_info(renderer, &info), "backend info");
        payload = {sizeof(payload),
                   WEFT_RENDER_ABI_VERSION,
                   abi_handle(gpu.gpu.device),
                   abi_handle(command),
                   abi_handle(target_view),
                   1,
                   0,
                   {0, 0, 0, 0}};
        return {sizeof(weft_render_pass),
                WEFT_RENDER_ABI_VERSION,
                info.identity,
                gpu.width,
                gpu.height,
                std::uint32_t(format),
                1,
                &payload,
                sizeof(payload)};
    }

    std::uint32_t Backend::external_formats() const {
        std::uint32_t out = (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8) | (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F);
        if (gpu.gpu.filters(VK_FORMAT_R32G32B32A32_SFLOAT)) {
            out |= 1u << WEFT_RENDER_TARGET_LINEAR_RGBA32F;
        }
        return out;
    }

    void Backend::enable_external(unsigned capacity, std::uint64_t bytes) {
        imports_.assign(capacity, VK_NULL_HANDLE);
        const auto table = make_c_external_backend(*this, {external_formats(), capacity, bytes});
        render_result(weft_renderer_install_external_backend(renderer, &table), "install external texture extension");
    }

    weft_render_result check_external_texture(const weft_render_external_texture& description, VkDevice device,
                                              VkImageView& view) {
        if (description.native_texture_size < sizeof(weft_render_vulkan_texture)) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_vulkan_texture*>(description.native_texture);
        if (p.struct_size < sizeof(p) || p.abi_version != WEFT_RENDER_EXTERNAL_ABI_VERSION || !p.device ||
            !p.image_view) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (native_handle<VkDevice>(p.device) != device) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        view = native_handle<VkImageView>(p.image_view);
        return WEFT_RENDER_OK;
    }

    weft_render_result Backend::import_texture(const weft_render_external_texture& description, std::uint64_t& out) {
        out = 0;
        VkImageView view{};
        if (const auto v = check_external_texture(description, gpu.gpu.device, view); v != WEFT_RENDER_OK) {
            return v;
        }
        if (std::find(imports_.begin(), imports_.end(), view) != imports_.end()) {
            return WEFT_RENDER_INVALID_STATE;
        }
        const auto free = std::find(imports_.begin(), imports_.end(), VkImageView{VK_NULL_HANDLE});
        if (free == imports_.end()) {
            return WEFT_RENDER_BUSY;
        }
        *free = view;
        out = std::uint64_t(free - imports_.begin()) + 1;
        ++imported;
        return WEFT_RENDER_OK;
    }

    void Backend::release_texture(std::uint64_t native) {
        if (!native || native > imports_.size() || !imports_[native - 1]) {
            std::terminate();
        }
        imports_[native - 1] = VK_NULL_HANDLE;
        ++released;
    }

    weft_render_result Backend::external_views(const weft_render_external_asset* textures, std::uint32_t count,
                                               std::vector<ExternalView>& out) const {
        out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto token = textures[i].native_texture;
            if (!token || token > imports_.size() || !imports_[token - 1]) {
                return WEFT_RENDER_STALE_HANDLE;
            }
            out.push_back({imports_[token - 1], textures[i].description.origin == WEFT_RENDER_EXTERNAL_BOTTOM_LEFT});
        }
        return WEFT_RENDER_OK;
    }

    template <class Make>
    weft_render_result Backend::prepare_with(std::uint64_t& out, Make&& make) {
        out = 0;
        if (prepared) {
            return WEFT_RENDER_BUSY;
        }
        try {
            prepared = make();
        } catch (...) {
            // Renderer::prepare destroyed what it created; no lease is left.
            log_failure("prepare");
            return WEFT_RENDER_BACKEND_ERROR;
        }
        out = limits::prepared_frame_handle;
        return WEFT_RENDER_OK;
    }

    weft_render_result Backend::prepare(const weft_render_frame& frame, std::uint64_t& out) {
        return prepare_with(out, [&] {
            const auto packet = frame_packet(frame, [](std::uint32_t) {
                return true;
            });
            return gpu.prepare(std::make_shared<const paint::DisplayList>(quad_list(packet, 0, gpu.width, gpu.height)));
        });
    }

    weft_render_result Backend::prepare_list(const weft_paint_list& list, const weft_render_external_asset* textures,
                                             std::uint32_t count, std::uint64_t& out) {
        out = 0;
        if (!list.list) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        std::vector<ExternalView> external;
        try {
            if (const auto result = external_views(textures, count, external); result != WEFT_RENDER_OK) {
                return result;
            }
        } catch (...) {
            return WEFT_RENDER_OUT_OF_MEMORY;
        }
        return prepare_with(out, [&] {
            return gpu.prepare(list.list, std::move(external));
        });
    }

    weft_render_result Backend::prepare_external(const weft_render_frame& frame,
                                                 const weft_render_external_asset* textures, std::uint32_t count,
                                                 std::uint64_t& out) {
        out = 0;
        std::vector<ExternalView> external;
        try {
            if (const auto result = external_views(textures, count, external); result != WEFT_RENDER_OK) {
                return result;
            }
        } catch (...) {
            return WEFT_RENDER_OUT_OF_MEMORY;
        }
        return prepare_with(out, [&] {
            const auto packet = frame_packet(frame, [](std::uint32_t) {
                return true;
            });
            return gpu.prepare(
                std::make_shared<const paint::DisplayList>(quad_list(packet, count, gpu.width, gpu.height)),
                std::move(external));
        });
    }

    void Backend::release_prepared(std::uint64_t native) {
        if (native != limits::prepared_frame_handle || !prepared) {
            std::terminate();
        }
        try {
            gpu.release(*prepared);
        } catch (...) {
            std::terminate();
        }
        prepared.reset();
    }

    weft_render_result Backend::encode(std::uint64_t native, const weft_render_pass& render_pass, std::uint64_t& out) {
        out = 0;
        // A pass from before target_image ends where it starts.
        constexpr auto v1 = offsetof(weft_render_vulkan_pass, target_image);
        if (native != limits::prepared_frame_handle || !prepared || render_pass.native_pass_size < v1) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        const auto& p = *static_cast<const weft_render_vulkan_pass*>(render_pass.native_pass);
        if (p.struct_size < v1 || p.abi_version != WEFT_RENDER_ABI_VERSION || p.reserved || p.clear_target > 1 ||
            !p.device || !p.command_buffer || !p.target_view) {
            return WEFT_RENDER_INVALID_ARGUMENT;
        }
        if (p.clear_target) {
            for (float c : p.clear_linear_premultiplied) {
                if (!std::isfinite(c) || c < 0 || c > 1) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
            for (std::size_t i = 0; i < 3; ++i) {
                if (p.clear_linear_premultiplied[i] > p.clear_linear_premultiplied[3]) {
                    return WEFT_RENDER_INVALID_ARGUMENT;
                }
            }
        }
        if (native_handle<VkDevice>(p.device) != gpu.gpu.device) {
            return WEFT_RENDER_WRONG_BACKEND;
        }
        const auto format = target_format(render_pass.target_format);
        if (render_pass.width != gpu.width || render_pass.height != gpu.height || format == VK_FORMAT_UNDEFINED ||
            !(target_formats() & (1u << render_pass.target_format))) {
            return WEFT_RENDER_UNSUPPORTED_FEATURE;
        }
        for (std::size_t i = 0; i < uses.size(); ++i) {
            if (!uses[i]) {
                const paint::Color clear{p.clear_linear_premultiplied[0], p.clear_linear_premultiplied[1],
                                         p.clear_linear_premultiplied[2], p.clear_linear_premultiplied[3]};
                try {
                    uses[i] =
                        gpu.encode(*prepared, native_handle<VkCommandBuffer>(p.command_buffer),
                                   {native_handle<VkImageView>(p.target_view), format}, p.clear_target != 0, clear);
                } catch (const Busy&) {
                    // Another recording first writes an atlas page of the frame; nothing was recorded.
                    return WEFT_RENDER_BUSY;
                } catch (...) {
                    // Renderer::encode recorded nothing and discarded its use.
                    log_failure("encode");
                    return WEFT_RENDER_BACKEND_ERROR;
                }
                out = i + 1;
                return WEFT_RENDER_OK;
            }
        }
        return WEFT_RENDER_BUSY;
    }

    void Backend::discard_encoded(std::uint64_t native) {
        if (!native || native > uses.size()) {
            std::terminate();
        }
        auto& use = uses[native - 1];
        if (!use || gpu.tracker.DiscardEncoded(use->token) != LifetimeResult::Ok) {
            std::terminate();
        }
        // The host destroyed or reset the commands: nothing uses the frame's resources.
        use.reset();
        gpu.collect();
    }

    void Backend::submitted(std::uint64_t native, std::uint64_t serial) {
        if (!native || native > uses.size()) {
            std::terminate();
        }
        auto& use = uses[native - 1];
        if (!use ||
            gpu.tracker.NotifySubmitted(use->token, std::uint64_t(limits::queue), serial) != LifetimeResult::Ok) {
            std::terminate();
        }
        gpu.collect();
    }

    void Backend::completed(std::uint64_t serial) {
        if (gpu.tracker.NotifyCompletedSerial(std::uint64_t(limits::device_epoch), std::uint64_t(limits::queue),
                                              serial) != LifetimeResult::Ok) {
            std::terminate();
        }
        for (auto& use : uses) {
            UseTokenInfo info;
            if (use && gpu.tracker.InspectUseToken(use->token, info) == LifetimeResult::StaleHandle) {
                use.reset();
            }
        }
        gpu.collect();
    }

    void Backend::abandon() {
        if (gpu.tracker.AbandonDeviceEpoch(std::uint64_t(limits::device_epoch),
                                           DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess) !=
            LifetimeResult::Ok) {
            std::terminate();
        }
        // The host guarantees that the device no longer accesses anything the uses held.
        for (auto& use : uses) {
            use.reset();
        }
        gpu.collect();
    }

    weft_render_result Backend::shutdown() {
        try {
            gpu.drain();
        } catch (...) {
            log_failure("shutdown");
            return WEFT_RENDER_BACKEND_ERROR;
        }
        const auto result = gpu.tracker.Shutdown();
        return result == LifetimeResult::Ok     ? WEFT_RENDER_OK
               : result == LifetimeResult::Busy ? WEFT_RENDER_BUSY
                                                : WEFT_RENDER_BACKEND_ERROR;
    }
} // namespace weft::render::vulkan
