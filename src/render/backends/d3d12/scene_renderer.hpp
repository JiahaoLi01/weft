#pragma once

#include "render/backends/d3d12/device.hpp"
#include "scene/scene.hpp"
#include "scene/scene_native.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

// Native D3D12 pass of a scene: the passes a ScenePreparer prepares (scene/scene_native.hpp), drawn by the shared
// scene shader's encoded pass (src/render/shaders/scene.hlsl without SPACE_LINEAR, the DXBC the D3D11 pass runs) as
// instanced box records, transparent groups in scratch textures, perspective planes and explicit layers whose content
// a nested pass draws into a texture of its own, the text, shadow and path images the preparer builds, uploaded once
// each (small ones into the region the preparer gives them on an atlas page), and UI shader effects in their encoded
// pass with their textures. Its own targets hold encoded premultiplied BGRA (B8G8R8A8_UNORM), as the D3D11 and Vulkan
// scene targets do; the host's target is BGRA8 or RGBA8.
//
// encode() records a frame into the host's command list with what it uses in a slot: the host executes the commands,
// reports the submission (submitted()), and retires the slot once they completed, or discards a slot whose commands it
// never submits. Slots lease the images and layers they sample, so a texture of the cache goes only after every frame
// that sampled it completed. What a recording uploads or draws into the cache is pending until its submission: another
// recording that samples it throws Busy. Recordings are submitted in the order they were recorded. Every texture of
// the renderer's is shader-readable between recordings, so a recording that is never submitted leaves no state behind.
//
// A view's pass is kept while its frames keep their revision (SceneFrame::revision): such a frame records the kept pass
// again with the frame's anchors (ScenePreparer::place) and their record order, its records in the slot's own buffers
// since the slot first drew that pass.
//
// Owner thread of the SceneText.
namespace weft::render::d3d12 {
    // A texture a pass samples: its resource, the format its shader resource view reads it as and its levels.
    struct View {
        ID3D12Resource* resource{};
        DXGI_FORMAT format{};
        std::uint32_t levels = 1;

        bool operator==(const View&) const = default;
    };

    class SceneRenderer : public SceneNativeWork {
    public:
        // A host color target: a single-sample texture of at least width x height device px, of format (its view
        // format: B8G8R8A8_UNORM, R8G8B8A8_UNORM or R10G10B10A2_UNORM), in D3D12_RESOURCE_STATE_RENDER_TARGET when the
        // commands execute,
        // where they leave it.
        struct Target {
            ID3D12Resource* resource{};
            DXGI_FORMAT format{};
            std::uint32_t width{};
            std::uint32_t height{};
        };

        // What one recorded frame uses: its upload buffers, scratch textures, descriptor heaps and the target's render
        // target view, leases of the images and layers it samples, references of the host resources it reads or
        // draws, and the cache entries it uploads or draws. Its buffers, scratch textures and heaps serve its later
        // recordings; the slot releases them with itself, after its commands completed.
        struct Slot {
            enum class State : std::uint8_t {
                Idle,
                Recorded,
                Submitted
            };

            Slot() = default;
            Slot(const Slot&) = delete;
            Slot& operator=(const Slot&) = delete;
            ~Slot();

            State state{};
            // Numbers the slot's recording among the renderer's.
            std::uint64_t recording{};
            // Both stable while the recording takes more: its passes keep the buffers they write views of, and follow
            // the states of the textures they hold.
            std::deque<Buffer> buffers;
            std::deque<Image> images;
            std::vector<DescriptorHeap> heaps;
            std::uint32_t target_rtv = UINT32_MAX;
            std::vector<std::shared_ptr<const void>> leases;
            std::vector<ComPtr<ID3D12Resource>> held;
            std::vector<std::uint64_t> created;
            // What the slot's earlier recordings used, for its next ones.
            std::vector<Buffer> spare_buffers;
            std::vector<Image> spare_images;
            std::vector<DescriptorHeap> spare_heaps;
            // The device of the slot's objects, once it recorded.
            Device* device{};
            // The serial of the kept pass whose records and rounded clips kept_records and kept_clips hold (0: none).
            std::uint64_t kept{};
            Buffer kept_records;
            Buffer kept_clips;
        };

        // bounds are the preparer's image caches' and atlases' (SceneCacheBounds).
        SceneRenderer(Device& device, SceneText& text, SceneCacheBounds bounds = {});
        ~SceneRenderer();
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // Records frame's pass over target (scale device px per CSS px, the anchors placing the anchored elements as
        // ScenePreparer::prepare says) into command, a graphics command list recording, with what it uses in slot: an
        // idle slot, or one whose submitted commands completed (it retires first). False, with the reason counted and
        // nothing recorded, when a native pass cannot draw the frame. Throws Busy, recording nothing, while the frame
        // samples what another recording uploads or draws and that is not submitted yet. start: whether the pass
        // replaces what the target holds or draws over it (SceneTargetStart). textures: the host textures of the
        // page's -weft-texture(name) backgrounds, each SceneTexture::view a View* the caller keeps while the slot
        // lives, in a state pixel shaders read when the commands execute; an unbound name draws nothing.
        bool encode(Slot& slot, const SceneFrame& frame, ID3D12GraphicsCommandList* command, const Target& target,
                    float scale, unsigned view_index, const SceneAnchorTable* anchors = nullptr,
                    const SceneTargetStart& start = {}, std::span<const SceneTexture> textures = {});
        // The slot's recorded commands were submitted: what they upload or draw is no longer pending.
        void submitted(Slot& slot);
        // The slot's recorded commands will never be submitted: what they would upload or draw leaves the cache, and the
        // slot is idle again.
        void discard(Slot& slot) noexcept;
        // The slot's submitted commands completed: its resources and leases go, and the slot is idle again.
        void retire(Slot& slot) noexcept;

        Device& gpu;
        // Images uploaded, recordings made, and buffers, scratch textures and heaps made for slots (which their later
        // recordings reuse).
        std::size_t uploads{};
        std::size_t recordings{};
        std::size_t slot_objects{};

        // Drops what the renderer and its preparer keep for a view: its kept pass and its anchored elements' last
        // anchors. The view's slots are the caller's to retire.
        void forget(unsigned view_index) {
            kept_.erase(view_index);
            preparer_.forget(view_index);
        }

        // The views the renderer keeps a pass for, until it forgets them.
        std::size_t kept_views() const {
            return kept_.size();
        }

    private:
        // The texture of a preparer's image or layer, kept while that lives (none for an image on an atlas page, whose
        // page holds it); pending while the recording that uploads or draws it is not submitted.
        struct Texture {
            std::weak_ptr<const void> source;
            Image image;
            std::uint64_t pending{};
        };

        // The pipelines that draw into targets of one format: the scene's records with source-over, with
        // destination-over (a backdrop beneath its group's content), the particles, and each effect program's encoded
        // pass. An effect program's lease keeps it alive, so its address names it alone.
        struct Pipelines {
            ComPtr<ID3D12PipelineState> scene;
            ComPtr<ID3D12PipelineState> under;
            ComPtr<ID3D12PipelineState> particles;
            std::unordered_map<const paint::EffectProgram*,
                               std::pair<std::shared_ptr<const paint::EffectProgram>, ComPtr<ID3D12PipelineState>>>
                effects;
        };

        SceneRaster reference_;
        ScenePreparer preparer_;
        ComPtr<ID3D12RootSignature> root_;
        ComPtr<ID3D12RootSignature> backdrop_root_;
        std::map<DXGI_FORMAT, Pipelines> pipelines_;
        // The area downsample, the Gaussian pass and the color matrix, into BGRA8 targets.
        std::array<ComPtr<ID3D12PipelineState>, 3> backdrop_pipelines_;
        // Bound where a draw samples no image, and where a pass has no rounded clips or anchors.
        Image blank_image_;
        Buffer blank_buffer_;
        std::unordered_map<std::uint64_t, Texture> textures_;
        // The preparer's atlas pages by format and page number.
        std::map<std::array<std::uint32_t, 2>, Image> pages_;
        std::uint64_t next_recording_{};
        // The command list and slot of the recording in progress, and the shader-visible heap it allocates from.
        ID3D12GraphicsCommandList* command_{};
        Slot* slot_{};
        DescriptorHeap* heap_{};

        // The pass kept for a view: what its frames' revision and the pass's target were, the pass, its serial (a new
        // one for each patched version), and the anchors of the frame recorded last.
        struct Kept {
            std::uint64_t revision{};
            float scale{};
            std::uint32_t width{};
            std::uint32_t height{};
            std::shared_ptr<ScenePrepared> prepared;
            std::uint64_t serial{};
            SceneAnchorFrame frame;
            // Which of its host textures had rows from the bottom: its records sample those flipped.
            std::vector<bool> flips;
            ScenePatchHistory history;
        };

        std::unordered_map<unsigned, Kept> kept_;
        std::uint64_t serial_{};
        // Every record in its own place, for passes recorded in planned order (grown to the largest pass).
        std::vector<std::uint32_t> in_order_;

        // A target being drawn: its render target view, format and size, and the texture behind it (the host's or one
        // of the renderer's), whose state the pass follows.
        struct Surface {
            Image* image{};
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            DXGI_FORMAT format{};
            std::uint32_t width{};
            std::uint32_t height{};
        };

        Pipelines& pipelines(DXGI_FORMAT format);
        ID3D12PipelineState* effect_pipeline(DXGI_FORMAT format,
                                             const std::shared_ptr<const paint::EffectProgram>& program);
        // Throws Busy when prepared samples, from the cache, what another unsubmitted recording uploads or draws.
        void check_pending(const ScenePrepared& prepared) const;
        // Records the uploads of the images that prepared samples and that have no texture yet, its layers' contents
        // included when those are drawn here.
        void upload(const ScenePrepared& prepared);
        // Records prepared's pass into out (a texture of prepared's size or larger): its planes' and new layers'
        // contents first, each into a texture of its own. anchors are the frame's (none for a pass whose anchors are
        // not deferred); serial names a kept pass, whose records the slot keeps. clear, for a pass drawn over its
        // target: the color the target is cleared to first (none: kept). host_textures: the host textures bound to the
        // names of the pass and its planes. history, the patches of the kept pass, lets a slot holding an earlier
        // version write only what they changed.
        void record(const ScenePrepared& prepared, const Surface& out, float scale,
                    const SceneAnchorFrame* anchors = nullptr, std::uint64_t serial = 0,
                    const paint::Color* clear = nullptr, std::span<const SceneTexture> host_textures = {},
                    const ScenePatchHistory* history = nullptr);
        // An atlas page's texture, created shader-readable the first time.
        Image& page(const SceneAtlasPage& page);
        // The texture of a layer, its content recorded the first time.
        View layer(const std::shared_ptr<const SceneLayerImage>& layer, float scale);
        // A target texture of the slot's (a spare of that size and format, or new), shader-readable.
        Image& scratch(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM);
        // An upload buffer of the slot's of at least size bytes: a spare, or new with room.
        Buffer& buffer(std::uint64_t size);
        // count consecutive descriptors of the recording's shader-visible heap: the index of the first. A heap that
        // has no room is followed by a larger one, bound on the command list.
        std::uint32_t descriptors(std::uint32_t count);
        // Writes a shader resource view of view at index of the recording's heap.
        void write_view(std::uint32_t index, const View& view);
        // Writes a structured buffer view of count elements of stride bytes from the start of buffer at index.
        void write_buffer(std::uint32_t index, const Buffer& buffer, std::size_t count, std::size_t stride);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor(std::uint32_t index) const;
        // Ends the slot's recording: its buffers, scratch textures and heaps become spares (as many as the slot keeps),
        // its target view, leases, references and cache entries go.
        void release(Slot& slot) noexcept;
        void destroy_all() noexcept;
    };
} // namespace weft::render::d3d12
