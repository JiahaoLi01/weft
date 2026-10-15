#include "render/backends/d3d12/device.hpp"

#include "constants/render.hpp"

#include <cstdio>
#include <string>

namespace weft::render::d3d12 {
    namespace limits = constants::render::d3d12;

    void check(HRESULT result, std::string_view what) {
        if (FAILED(result)) {
            char code[16];
            std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(result));
            throw std::runtime_error(std::string(what) + " failed: " + code);
        }
    }

    void ensure(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string(message));
        }
    }

    Device::Device(ID3D12Device* d) : device(d) {
        ensure(device != nullptr, "a D3D12 device");
        srv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    Device::~Device() = default;

    Image Device::image(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, bool target,
                        D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        Image out;
        out.format = format;
        out.width = width;
        out.height = height;
        out.state = state;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, target ? &clear : nullptr,
                                              IID_PPV_ARGS(&out.resource)),
              "CreateCommittedResource texture");
        if (target) {
            out.rtv_index = render_target(out.resource.Get(), format, out.rtv);
        }
        return out;
    }

    Buffer Device::buffer(std::uint64_t size) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Buffer out;
        out.size = size;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&out.resource)),
              "CreateCommittedResource buffer");
        // Written by the CPU only: no range is read.
        const D3D12_RANGE none{0, 0};
        check(out.resource->Map(0, &none, &out.mapped), "Map upload buffer");
        out.address = out.resource->GetGPUVirtualAddress();
        return out;
    }

    DescriptorHeap Device::heap(std::uint32_t capacity) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = capacity;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        DescriptorHeap out;
        out.capacity = capacity;
        check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out.heap)), "CreateDescriptorHeap shader resources");
        return out;
    }

    std::uint32_t Device::render_target(ID3D12Resource* resource, DXGI_FORMAT format,
                                        D3D12_CPU_DESCRIPTOR_HANDLE& out) {
        if (free_rtvs_.empty()) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = std::uint32_t(limits::render_target_heap_size);
            ComPtr<ID3D12DescriptorHeap> heap;
            check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap render targets");
            const auto base = std::uint32_t(rtv_heaps_.size()) * desc.NumDescriptors;
            rtv_heaps_.push_back(std::move(heap));
            for (std::uint32_t i = desc.NumDescriptors; i-- > 0;) {
                free_rtvs_.push_back(base + i);
            }
        }
        const auto index = free_rtvs_.back();
        free_rtvs_.pop_back();
        const auto per_heap = std::uint32_t(limits::render_target_heap_size);
        out = rtv_heaps_[index / per_heap]->GetCPUDescriptorHandleForHeapStart();
        out.ptr += std::size_t(index % per_heap) * rtv_increment;
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, out);
        return index;
    }

    void Device::release_render_target(std::uint32_t index) noexcept {
        if (index != UINT32_MAX) {
            free_rtvs_.push_back(index);
        }
    }

    void Device::destroy(Image& image) noexcept {
        release_render_target(image.rtv_index);
        image = {};
    }

    bool Device::filters(DXGI_FORMAT format) const {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, {}, {}};
        return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
               (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
    }

    D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        return b;
    }

    void transition(ID3D12GraphicsCommandList* command, Image& image, D3D12_RESOURCE_STATES state) {
        if (image.state == state) {
            return;
        }
        const auto b = barrier(image.resource.Get(), image.state, state);
        command->ResourceBarrier(1, &b);
        image.state = state;
    }

    namespace {
        ComPtr<ID3D12RootSignature> serialize(const Device& d, const D3D12_ROOT_SIGNATURE_DESC& desc,
                                              std::string_view what) {
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> error;
            const auto result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
            if (FAILED(result)) {
                std::string message(what);
                if (error) {
                    message += ": ";
                    message.append(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
                }
                check(result, message);
            }
            ComPtr<ID3D12RootSignature> root;
            check(
                d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
                what);
            return root;
        }

        D3D12_STATIC_SAMPLER_DESC sampler(UINT shader_register, D3D12_TEXTURE_ADDRESS_MODE address) {
            D3D12_STATIC_SAMPLER_DESC s{};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW = address;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ShaderRegister = shader_register;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            return s;
        }
    } // namespace

    ComPtr<ID3D12RootSignature> create_scene_root(const Device& d) {
        // The records (t0), the rounded clips (t2), the order and the anchors (t7, t8) in one table.
        const D3D12_DESCRIPTOR_RANGE buffers[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 1},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 7, 0, 2}};
        // The image (t1), then an effect's textures or a sprite (t3-t6), in another.
        const D3D12_DESCRIPTOR_RANGE images[]{
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, std::uint32_t(limits::table_descriptors) - 1, 3, 0, 1}};
        D3D12_ROOT_PARAMETER parameters[3]{};
        auto& view = parameters[limits::root_view];
        view.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        view.Constants.ShaderRegister = 0;
        view.Constants.Num32BitValues = std::uint32_t(limits::view_constant_count);
        view.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        auto& buffer_table = parameters[limits::root_buffers];
        buffer_table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        buffer_table.DescriptorTable.NumDescriptorRanges = UINT(std::size(buffers));
        buffer_table.DescriptorTable.pDescriptorRanges = buffers;
        buffer_table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        auto& image_table = parameters[limits::root_images];
        image_table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        image_table.DescriptorTable.NumDescriptorRanges = UINT(std::size(images));
        image_table.DescriptorTable.pDescriptorRanges = images;
        image_table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // The clamping sampler (linearClamp, s0) and the repeating one (effectRepeat and linearRepeat, s1).
        const D3D12_STATIC_SAMPLER_DESC samplers[]{sampler(0, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
                                                   sampler(1, D3D12_TEXTURE_ADDRESS_MODE_WRAP)};
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = UINT(std::size(parameters));
        desc.pParameters = parameters;
        desc.NumStaticSamplers = UINT(std::size(samplers));
        desc.pStaticSamplers = samplers;
        return serialize(d, desc, "the scene root signature");
    }

    ComPtr<ID3D12RootSignature> create_backdrop_root(const Device& d) {
        const D3D12_DESCRIPTOR_RANGE source{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        auto& constants = parameters[limits::backdrop_root_constants];
        constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        constants.Descriptor.ShaderRegister = 0;
        constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        auto& table = parameters[limits::backdrop_root_source];
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.DescriptorTable.NumDescriptorRanges = 1;
        table.DescriptorTable.pDescriptorRanges = &source;
        table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const auto mirror = sampler(0, D3D12_TEXTURE_ADDRESS_MODE_MIRROR);
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = UINT(std::size(parameters));
        desc.pParameters = parameters;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &mirror;
        return serialize(d, desc, "the backdrop root signature");
    }

    ComPtr<ID3D12PipelineState> create_pipeline(const Device& d, ID3D12RootSignature* root, const void* vertex,
                                                std::size_t vertex_size, const void* pixel, std::size_t pixel_size,
                                                DXGI_FORMAT format, bool under) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.VS = {vertex, vertex_size};
        desc.PS = {pixel, pixel_size};
        auto& target = desc.BlendState.RenderTarget[0];
        target.BlendEnable = TRUE;
        if (under) {
            // Destination over, premultiplied: what the target holds stays over what is drawn.
            target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_INV_DEST_ALPHA;
            target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ONE;
        } else {
            target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        }
        target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = format;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> out;
        check(d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out)), "CreateGraphicsPipelineState");
        return out;
    }
} // namespace weft::render::d3d12
