#pragma once

#include "render/backends/d3d12/scene_renderer.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

// The native D3D12 scene pass on WARP over a transparent width x height target, read back as encoded premultiplied
// BGRA rows, as the D3D11 pass (d3d11_scene.hpp) and vulkan::SceneRenderer::draw read theirs: for the renderer tests
// that compare the passes. Each draw records one command list, executes it, waits, and fails on an error the debug
// layer reported when it is installed. The pass keeps the D2D reference raster for color glyph images, which needs COM
// on the thread.
namespace weft::testing {
    class D3D12Scene {
    public:
        template <class T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        // The pass on a WARP device; none without WARP (of feature level 11).
        static std::unique_ptr<D3D12Scene> open(unsigned width, unsigned height, SceneCacheBounds bounds = {}) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
            }
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> warp;
            ComPtr<ID3D12Device> device;
            if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
                FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                return nullptr;
            }
            return std::unique_ptr<D3D12Scene>(new D3D12Scene(device, width, height, bounds, debug != nullptr));
        }

        D3D12Scene(const D3D12Scene&) = delete;
        D3D12Scene& operator=(const D3D12Scene&) = delete;

        ~D3D12Scene() {
            if (event_) {
                CloseHandle(event_);
            }
        }

        // textures: the host textures -weft-texture(name) backgrounds show (their views render::d3d12::View pointers
        // of this device).
        std::vector<std::uint8_t> draw(const SceneFrame& frame, float scale = 1,
                                       const SceneAnchorTable* anchors = nullptr,
                                       std::span<const SceneTexture> textures = {}) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width_;
            desc.Height = height_;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ComPtr<ID3D12Resource> target;
            require(SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                               IID_PPV_ARGS(&target))),
                    "D3D12 scene target");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes{};
            device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> readback;
            require(SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&readback))),
                    "D3D12 readback");
            require(SUCCEEDED(allocator_->Reset()) && SUCCEEDED(list_->Reset(allocator_.Get(), nullptr)),
                    "D3D12 command list");
            if (!scene->encode(slot, frame, list_.Get(), {target.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, width_, height_},
                               scale, 0, anchors, {}, textures)) {
                list_->Close();
                std::string why;
                for (const auto& [reason, count] : scene->reasons) {
                    why += " " + reason;
                }
                require(false, "native D3D12 pass:" + why);
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list_->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            from.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            to.PlacedFootprint = footprint;
            list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            require(SUCCEEDED(list_->Close()), "close the D3D12 command list");
            ID3D12CommandList* lists[]{list_.Get()};
            queue_->ExecuteCommandLists(1, lists);
            scene->submitted(slot);
            require(SUCCEEDED(queue_->Signal(fence_.Get(), ++value_)) &&
                        SUCCEEDED(fence_->SetEventOnCompletion(value_, event_)),
                    "D3D12 fence");
            WaitForSingleObject(event_, INFINITE);
            scene->retire(slot);
            check_messages();
            void* mapped{};
            require(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "map the D3D12 readback");
            std::vector<std::uint8_t> out(std::size_t(width_) * height_ * 4);
            for (unsigned y = 0; y < height_; ++y) {
                std::memcpy(out.data() + std::size_t(y) * width_ * 4,
                            static_cast<const std::uint8_t*>(mapped) + std::size_t(y) * footprint.Footprint.RowPitch,
                            std::size_t(width_) * 4);
            }
            readback->Unmap(0, nullptr);
            return out;
        }

        // Fails on any error the debug layer reported since the last check.
        void check_messages() {
            if (!messages_) {
                return;
            }
            std::string errors;
            for (UINT64 i = 0; i < messages_->GetNumStoredMessages(); ++i) {
                SIZE_T size{};
                messages_->GetMessage(i, nullptr, &size);
                std::vector<std::byte> message(size);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(message.data());
                messages_->GetMessage(i, m, &size);
                if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR || m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                    errors.append(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0);
                    errors += '\n';
                }
            }
            messages_->ClearStoredMessages();
            require(errors.empty(), "D3D12 debug layer errors:\n" + errors);
        }

        // Whether the debug layer checks the commands.
        bool debugged() const {
            return messages_ != nullptr;
        }

    private:
        D3D12Scene(const ComPtr<ID3D12Device>& device, unsigned width, unsigned height, SceneCacheBounds bounds,
                   bool debug)
            : device_(device),
              width_(width),
              height_(height),
              gpu_(std::make_unique<render::d3d12::Device>(device.Get())) {
            if (debug) {
                device_.As(&messages_);
            }
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            require(SUCCEEDED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_))) &&
                        SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                  IID_PPV_ARGS(&allocator_))) &&
                        SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
                                                             nullptr, IID_PPV_ARGS(&list_))) &&
                        SUCCEEDED(list_->Close()) &&
                        SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))),
                    "the D3D12 queue, command list and fence");
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            scene = std::make_unique<render::d3d12::SceneRenderer>(*gpu_, thread_scene_text(), bounds);
        }

        ComPtr<ID3D12Device> device_;
        ComPtr<ID3D12InfoQueue> messages_;
        ComPtr<ID3D12CommandQueue> queue_;
        ComPtr<ID3D12CommandAllocator> allocator_;
        ComPtr<ID3D12GraphicsCommandList> list_;
        ComPtr<ID3D12Fence> fence_;
        HANDLE event_{};
        std::uint64_t value_{};
        unsigned width_{};
        unsigned height_{};
        std::unique_ptr<render::d3d12::Device> gpu_;

    public:
        // Destroyed before the device it draws with, its slot first.
        std::unique_ptr<render::d3d12::SceneRenderer> scene;
        render::d3d12::SceneRenderer::Slot slot;
    };
} // namespace weft::testing
