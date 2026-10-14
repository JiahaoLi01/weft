#pragma once

#include "constants/render.hpp"
#include "render/backend.hpp"
#include "render/backends/d3d11/renderer.hpp"
#include "weft/render_backend.h"
#include "weft/render_backend_d3d11.h"
#include "weft/render_external_texture.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// D3D11 adapter of the renderer C ABI. The core ABI has no COM or D3D11 dependency: the host creates the
// device, deferred context and render target view, and owns submission and completion. Quad frames and display list
// frames both prepare a display list.
namespace weft::render::d3d11 {
    // Throws std::runtime_error("<operation> result=<n>") unless result is WEFT_RENDER_OK.
    void render_result(weft_render_result result, const char* operation);

    // A host texture of the external texture extension on device, checked as its D3D11 schema says
    // (weft_render_d3d11_texture: a view of the one mip of a single-sample Texture2D of the description's size and
    // linear format, default or immutable usage, no CPU access and no misc flags): its view and the resource it views.
    weft_render_result check_external_texture(const weft_render_external_texture& description, ID3D11Device* device,
                                              ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D11Resource>& resource);

    class Backend {
    public:
        Renderer& gpu;
        weft_renderer* renderer{};
        std::optional<PreparedFrame> prepared;
        std::array<std::optional<UseTokenId>, weft::constants::render::d3d11::frames_in_flight> uses;
        // Makes the next encode fail after its uploads, for rollback tests.
        bool inject_recording_failure{};

        struct ImportedTexture {
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11Resource> resource;
        };

        std::array<ImportedTexture, weft::constants::render::d3d11::max_imported_textures> imports;
        unsigned imported_count{};
        unsigned released_imports{};

        explicit Backend(Renderer& device);
        ~Backend();
        Backend(const Backend&) = delete;
        Backend& operator=(const Backend&) = delete;

        // Installs the external texture extension (render_external_texture.h).
        void enable_external(unsigned capacity = weft::constants::render::d3d11::external_texture_capacity,
                             std::uint64_t bytes = weft::constants::render::d3d11::external_texture_max_bytes);

        // A pass that records into context and target; commands receives the finished command list.
        weft_render_pass pass(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, void** commands,
                              weft_render_d3d11_pass& payload);

        // RenderBackend: called by the renderer core through the make_c_backend table.
        weft_render_result prepare(const weft_render_frame& frame, std::uint64_t& out);
        void release_prepared(std::uint64_t native);
        weft_render_result encode(std::uint64_t native, const weft_render_pass& render_pass, std::uint64_t& out);
        void discard_encoded(std::uint64_t native);
        void submitted(std::uint64_t native, std::uint64_t serial);
        void completed(std::uint64_t serial);
        void abandon();
        weft_render_result shutdown();

        // ExternalTextureBackend.
        weft_render_result import_texture(const weft_render_external_texture& description, std::uint64_t& out);
        void release_texture(std::uint64_t native);
        weft_render_result prepare_external(const weft_render_frame& frame, const weft_render_external_asset* textures,
                                            std::uint32_t count, std::uint64_t& out);

        // DisplayListBackend.
        weft_render_result prepare_list(const weft_paint_list& list, const weft_render_external_asset* textures,
                                        std::uint32_t count, std::uint64_t& out);

    private:
        // The imported views of a frame's external textures; STALE_HANDLE for one that is no longer imported.
        weft_render_result external_views(const weft_render_external_asset* textures, std::uint32_t count,
                                          std::vector<ExternalGpuAsset>& out);
        // Prepares make()'s frame with the external textures; a failure undoes every CPU lease it created.
        template <class Make>
        weft_render_result prepare_with(std::vector<ExternalGpuAsset> external, std::uint64_t& out, Make&& make);
        weft_render_result prepare_quads(const weft_render_frame& frame, std::vector<ExternalGpuAsset> external,
                                         std::uint64_t& out);
    };

    static_assert(ExternalTextureBackend<Backend> && DisplayListBackend<Backend>);

    // C ABI view of a FramePacket; the packet must outlive it. A quad packet is a weft_render_frame: with a renderer
    // the asset ids are its stable asset identities (version 1); otherwise asset i has id i + 1 and the given version.
    // A list packet is a weft_render_list_frame of its list.
    struct PacketView {
        std::vector<weft_render_asset> assets;
        std::vector<weft_render_draw> draws;
        weft_render_frame frame{};
        weft_paint_list list{};

        PacketView(const FramePacket& packet, uint64_t version, Renderer* cache = nullptr);
        PacketView(const PacketView&) = delete;
        PacketView& operator=(const PacketView&) = delete;

        // Prepares the frame through the entry point of its kind, with the external textures it draws.
        weft_render_result prepare(weft_renderer* renderer, weft_render_handle& out,
                                   const std::vector<weft_render_handle>& external = {}) const;
    };
} // namespace weft::render::d3d11
