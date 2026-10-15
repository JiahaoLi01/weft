#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

// What the D3D12 renderer of the shared scene shader (src/render/shaders/scene.hlsl) records with: a host's device, its
// textures and upload buffers, the render target views of its own targets, the root signatures of the scene and
// backdrop passes (constants::render::d3d12) and their pipeline states. Owner thread of the renderer that uses it.
namespace weft::render::d3d12 {
    using Microsoft::WRL::ComPtr;

    // A recording would sample what another recording that is not submitted yet uploads or draws.
    struct Busy : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Throws std::runtime_error naming what failed.
    void check(HRESULT result, std::string_view what);
    void ensure(bool condition, std::string_view message);

    // A 2D texture of one mip level and array slice in the default heap, the state its commands leave it in as the
    // recordings that use it are recorded, and a render target view when it is a target of the backend's (rtv_index
    // into the device's render target views).
    struct Image {
        ComPtr<ID3D12Resource> resource;
        DXGI_FORMAT format{};
        std::uint32_t width{};
        std::uint32_t height{};
        D3D12_RESOURCE_STATES state{};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        std::uint32_t rtv_index = UINT32_MAX;
    };

    // A buffer of the upload heap, mapped for as long as it lives: records, constants and staged texels.
    struct Buffer {
        ComPtr<ID3D12Resource> resource;
        void* mapped{};
        std::uint64_t size{};
        D3D12_GPU_VIRTUAL_ADDRESS address{};
    };

    // A shader-visible heap of shader resource views a recording allocates from, front to back.
    struct DescriptorHeap {
        ComPtr<ID3D12DescriptorHeap> heap;
        std::uint32_t capacity{};
        std::uint32_t used{};
    };

    class Device {
    public:
        explicit Device(ID3D12Device* device);
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        ~Device();

        ComPtr<ID3D12Device> device;
        std::uint32_t srv_increment{};
        std::uint32_t rtv_increment{};

        // A texture of format, created in state; with target, one the backend renders into, with a render target
        // view.
        Image image(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, bool target,
                    D3D12_RESOURCE_STATES state);
        // An upload buffer of at least size bytes.
        Buffer buffer(std::uint64_t size);
        // A shader-visible heap of capacity shader resource views.
        DescriptorHeap heap(std::uint32_t capacity);
        // A render target view of resource's top level in format, and its index to give back.
        std::uint32_t render_target(ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE& out);
        void release_render_target(std::uint32_t index) noexcept;
        void destroy(Image& image) noexcept;

        // Whether the device filters textures of format linearly.
        bool filters(DXGI_FORMAT format) const;

    private:
        std::vector<ComPtr<ID3D12DescriptorHeap>> rtv_heaps_;
        std::vector<std::uint32_t> free_rtvs_;
    };

    // Moves image to state with a barrier on command, when it is not there.
    void transition(ID3D12GraphicsCommandList* command, Image& image, D3D12_RESOURCE_STATES state);
    // A barrier of a resource the backend does not track (the host's target) from one state to another.
    D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    // The scene pass's root signature (constants::render::d3d12: the View constants, the buffer table, the image
    // table, and the clamping and repeating samplers s0, s1) and the backdrop passes' (their constants, the
    // source table and the mirroring sampler s0).
    ComPtr<ID3D12RootSignature> create_scene_root(const Device& d);
    ComPtr<ID3D12RootSignature> create_backdrop_root(const Device& d);
    // A pipeline of a vertex and pixel shader (DXBC) drawing triangle lists into one target of format, without depth,
    // with premultiplied source-over (or with under, premultiplied destination-over; with replace, no blending).
    ComPtr<ID3D12PipelineState> create_pipeline(const Device& d, ID3D12RootSignature* root, const void* vertex,
                                                std::size_t vertex_size, const void* pixel, std::size_t pixel_size,
                                                DXGI_FORMAT format, bool under = false);
} // namespace weft::render::d3d12
