#import "render/backends/metal/scene_renderer.hpp"

#import "render/backends/metal/scene_internal.hpp"

#include "constants/common.hpp"
#include "render/scene_pass.hpp"
#include "scene/scene_schedule.hpp"

#include <algorithm>
#include <cstring>
#include <variant>

// The Metal scene renderer's record(): a prepared pass run as its schedule says (scene/scene_schedule.hpp).
namespace weft::render::metal {
    using namespace detail;

    void SceneRenderer::record(const ScenePrepared& prepared, id<MTLTexture> out, bool readable, float scale,
                               const SceneAnchorFrame* anchors, std::uint64_t serial, const paint::Color* clear,
                               std::span<const SceneTexture> host_textures, const ScenePatchHistory* history) {
        const auto& pass = prepared.pass;
        // What the pass draws, in order; a backdrop of a target the pass may not read is transparent. Groups that read
        // no target draw ahead into images of their own regions, and on Apple GPUs blending groups read the target
        // where they draw: on Apple's tiled GPUs a target that ends and resumes for a group writes all of it out and
        // reads it back.
        const auto schedule =
            schedule_scene_pass(prepared, host_textures, clear, readable, {true, blend_pipeline_ != nil});
        // Nested passes first, each ending before this one begins: plane contents, and layers and filtered images not
        // drawn before. The slot leases every image and layer the pass samples.
        std::vector<id<MTLTexture>> views(prepared.images.size());
        for (std::size_t i = 0; i < prepared.images.size(); ++i) {
            if (prepared.planes[i]) {
                const auto plane = target(prepared.planes[i]->width, prepared.planes[i]->height);
                record(*prepared.planes[i], plane.texture, true, scale, nullptr, 0, nullptr, host_textures);
                views[i] = plane.texture;
                ++plane_passes;
            } else if (prepared.layers[i]) {
                views[i] = layer(prepared.layers[i], scale);
                slot_->leases.push_back(prepared.layers[i]);
            } else if (prepared.filters[i]) {
                views[i] = filter(prepared.filters[i]);
                slot_->leases.push_back(prepared.filters[i]);
            } else if (const auto& drawn = prepared.images[i]) {
                views[i] = drawn->atlas ? page({drawn->format, drawn->atlas->page}).texture
                                        : textures_.at(drawn->id).image.texture;
                slot_->leases.push_back(drawn);
            }
        }
        // The host texture each texture item draws.
        for (std::size_t i = 0; i < schedule.textures.size(); ++i) {
            if (const auto* texture = schedule.textures[i]) {
                views[i] = host_texture(texture->view);
            }
        }
        texture_draws += schedule.bound_textures;
        unbound_textures += schedule.unbound_textures;
        // The records of host textures whose rows start at the bottom sample them flipped.
        const SceneRecord* record_data = pass.records.data();
        std::vector<SceneRecord> flipped;
        if (const auto flips = scene_texture_flips(prepared, host_textures);
            std::find(flips.begin(), flips.end(), true) != flips.end()) {
            flipped = pass.records;
            for (std::size_t i = 0; i < flipped.size(); ++i) {
                if (prepared.textures[i] && flips[prepared.textures[i] - 1]) {
                    flip_rows(flipped[i]);
                }
            }
            record_data = flipped.data();
        }
        // The pass's records and rounded clips. A kept pass's stay in the slot's own buffers while the slot draws that
        // pass.
        id<MTLBuffer> records;
        id<MTLBuffer> clips = blank_buffer_;
        if (serial) {
            const auto put = [&](Buffer& kept, const void* data, std::size_t bytes) {
                if (kept.size < std::max<std::size_t>(bytes, 1)) {
                    kept = new_buffer(bytes);
                    ++slot_objects;
                }
                std::memcpy(kept.mapped, data, bytes);
            };
            // A slot holding an earlier version of the kept pass writes only the records and rounded clips patched
            // since (none of them draws a host texture, so none is flipped).
            ScenePatch since;
            if (slot_->kept != serial && slot_->kept && history && history->since(slot_->kept, since) &&
                slot_->kept_records.size >= std::max<std::size_t>(pass.records.size() * sizeof(SceneRecord), 1) &&
                slot_->kept_clips.size >= std::max<std::size_t>(pass.clips.size() * sizeof(SceneClip), 1)) {
                for (const auto& [b, e] : since.records) {
                    std::memcpy(static_cast<SceneRecord*>(slot_->kept_records.mapped) + b, pass.records.data() + b,
                                (e - b) * sizeof(SceneRecord));
                }
                for (const auto& [b, e] : since.clips) {
                    std::memcpy(static_cast<SceneClip*>(slot_->kept_clips.mapped) + b, pass.clips.data() + b,
                                (e - b) * sizeof(SceneClip));
                }
                slot_->kept = serial;
                ++partial_uploads;
            } else if (slot_->kept != serial) {
                put(slot_->kept_records, record_data, pass.records.size() * sizeof(SceneRecord));
                put(slot_->kept_clips, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
                slot_->kept = serial;
            }
            records = slot_->kept_records.buffer;
            if (!pass.clips.empty()) {
                clips = slot_->kept_clips.buffer;
            }
        } else {
            auto& record_buffer = buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(SceneRecord));
            records = record_buffer.buffer;
            std::memcpy(record_buffer.mapped, record_data, pass.records.size() * sizeof(SceneRecord));
            if (!pass.clips.empty()) {
                auto& clip_buffer = buffer(pass.clips.size() * sizeof(SceneClip));
                clips = clip_buffer.buffer;
                std::memcpy(clip_buffer.mapped, pass.clips.data(), pass.clips.size() * sizeof(SceneClip));
            }
        }
        // The record each draw position takes, and the frame's anchors.
        const auto* order_values = &in_order_;
        if (anchors && !anchors->order.empty()) {
            ensure(anchors->order.size() == pass.records.size(), "a record order of the pass's records");
            order_values = &anchors->order;
        } else if (in_order_.size() < pass.records.size()) {
            in_order_.resize(pass.records.size());
            for (std::size_t i = 0; i < in_order_.size(); ++i) {
                in_order_[i] = std::uint32_t(i);
            }
        }
        auto& order_buffer = buffer(std::max<std::size_t>(pass.records.size(), 1) * sizeof(std::uint32_t));
        id<MTLBuffer> order = order_buffer.buffer;
        std::memcpy(order_buffer.mapped, order_values->data(), pass.records.size() * sizeof(std::uint32_t));
        id<MTLBuffer> anchor_values = blank_buffer_;
        if (anchors && !anchors->anchors.empty()) {
            ensure(anchors->anchors.size() == prepared.anchors.size(), "the anchors of the pass's anchored elements");
            auto& anchor_buffer = buffer(anchors->anchors.size() * sizeof(anchors->anchors[0]));
            anchor_values = anchor_buffer.buffer;
            std::memcpy(anchor_buffer.mapped, anchors->anchors.data(),
                        anchors->anchors.size() * sizeof(anchors->anchors[0]));
        } else {
            ensure(prepared.anchors.empty(), "the anchors of the pass's anchored elements");
        }
        // Metal's instance_id counts the draw's base instance, as Vulkan's instance index does: the vertex stage adds
        // none (View.baseInstance 0). The View's target size and origin are the target's being drawn into; its sampled
        // image, the group a composite samples.
        std::array<float, weft::constants::render::scene::view_constant_floats> constants{
            float(prepared.width), float(prepared.height), scale, 0};
        // The groups' images: one of the pass's size per depth, then each hoisted group's, and where each lies among
        // the pass's device px (origin, then size).
        std::vector<Image> groups;
        std::vector<std::array<float, 4>> placed;
        for (std::size_t depth = 0; depth < schedule.groups; ++depth) {
            groups.push_back(target(prepared.width, prepared.height));
            placed.push_back({0, 0, float(prepared.width), float(prepared.height)});
        }
        for (const auto& h : schedule.hoisted) {
            groups.push_back(target(h.size[0], h.size[1]));
            placed.push_back({float(h.origin[0]), float(h.origin[1]), float(h.size[0]), float(h.size[1])});
        }
        hoisted_groups += schedule.hoisted.size();
        // Each backdrop's chain of textures: its copy, then one target per filter pass; the backdrop draws the last.
        std::vector<std::vector<Image>> chains;
        for (const auto& chain : schedule.chains) {
            auto& images = chains.emplace_back();
            for (const auto& [w, h] : chain.images) {
                images.push_back(target(w, h));
            }
        }

        // The texture of a target of the schedule: the pass's own, a group's, or a chain's.
        const auto texture_of = [&](const SceneSchedule::Target& t) -> id<MTLTexture> {
            switch (t.kind) {
                case SceneSchedule::Target::Kind::Group:
                    return groups.at(t.index).texture;
                case SceneSchedule::Target::Kind::Chain:
                    return chains.at(t.index).at(t.image).texture;
                case SceneSchedule::Target::Kind::Out:
                    break;
            }
            return out;
        };
        const auto set_view = [&](id<MTLRenderCommandEncoder> encoder) {
            [encoder setVertexBytes:constants.data() length:sizeof(constants) atIndex:limits::buffer_view];
            [encoder setFragmentBytes:constants.data() length:sizeof(constants) atIndex:limits::buffer_view];
        };
        // The scene pipeline and what every draw of the pass binds, on each new encoder: the View of a target of
        // `place` (origin, size) among the pass's device px.
        const auto bind = [&](id<MTLRenderCommandEncoder> encoder, const std::array<float, 4>& place) {
            [encoder setRenderPipelineState:scene_pipeline_];
            constants[0] = place[2];
            constants[1] = place[3];
            constants[4] = place[0];
            constants[5] = place[1];
            set_view(encoder);
            [encoder setVertexBuffer:records offset:0 atIndex:limits::buffer_records];
            [encoder setFragmentBuffer:records offset:0 atIndex:limits::buffer_records];
            [encoder setFragmentBuffer:clips offset:0 atIndex:limits::buffer_clips];
            [encoder setVertexBuffer:order offset:0 atIndex:limits::buffer_order];
            [encoder setVertexBuffer:anchor_values offset:0 atIndex:limits::buffer_anchors];
            [encoder setFragmentBuffer:anchor_values offset:0 atIndex:limits::buffer_anchors];
            [encoder setFragmentSamplerState:clamp_sampler_ atIndex:limits::sampler_clamp];
            // The repeating sampler, which host textures and effects take (scene.hlsl, linearRepeat and effectRepeat).
            [encoder setFragmentSamplerState:repeat_sampler_ atIndex:limits::sampler_repeat];
            [encoder setFragmentTexture:blank_texture_ atIndex:limits::texture_image];
            for (std::size_t t = 0; t < std::size_t(weft::constants::max_effect_textures); ++t) {
                [encoder setFragmentTexture:blank_texture_ atIndex:NSUInteger(limits::texture_effects) + t];
            }
        };
        id<MTLRenderCommandEncoder> encoder = nil;
        try {
            for (const auto& step : schedule.commands) {
                if (const auto* b = std::get_if<SceneSchedule::Begin>(&step)) {
                    ensure(encoder == nil, "one target at a time");
                    if (b->target.kind == SceneSchedule::Target::Kind::Chain) {
                        // A backdrop chain's texture, cleared over all of it (transparent unless given).
                        const auto& image = chains.at(b->target.index).at(b->target.image);
                        encoder = begin(image.texture, image.width, image.height, b->clear.value_or(paint::Color{}));
                        encoder.label = @"weft backdrop chain";
                    } else {
                        const bool group = b->target.kind == SceneSchedule::Target::Kind::Group;
                        const auto place = group ? placed.at(b->target.index)
                                                 : std::array<float, 4>{0, 0, float(prepared.width),
                                                                        float(prepared.height)};
                        encoder = begin(texture_of(b->target), std::uint32_t(place[2]), std::uint32_t(place[3]),
                                        b->clear);
                        encoder.label = !group ? @"weft out"
                                        : b->target.index < schedule.groups ? @"weft group"
                                                                            : @"weft hoisted group";
                        bind(encoder, place);
                    }
                } else if (std::get_if<SceneSchedule::End>(&step)) {
                    // Metal orders the later reads of what the encoder drew: ending is all a sampled target needs.
                    ensure(encoder != nil, "an end of a target begun");
                    [encoder endEncoding];
                    encoder = nil;
                } else if (const auto* draw = std::get_if<SceneSchedule::Draw>(&step)) {
                    ensure(encoder != nil, "a draw inside a target");
                    id<MTLTexture> sampled = blank_texture_;
                    switch (draw->image.kind) {
                        case SceneSchedule::Image::Kind::Blank:
                            break;
                        case SceneSchedule::Image::Kind::Item:
                            sampled = views.at(draw->image.index);
                            ensure(sampled != nil, "an image item's texture");
                            break;
                        case SceneSchedule::Image::Kind::Page:
                            sampled = page(prepared.pages.at(draw->image.index)).texture;
                            break;
                        case SceneSchedule::Image::Kind::Group: {
                            // A group's composite: its image, and where that lies among the pass's device px.
                            sampled = groups.at(draw->image.index).texture;
                            const auto& place = placed.at(draw->image.index);
                            std::copy(place.begin(), place.end(), constants.begin() + 8);
                            set_view(encoder);
                            break;
                        }
                        case SceneSchedule::Image::Kind::Backdrop:
                            sampled = chains.at(draw->image.index).back().texture;
                            break;
                    }
                    [encoder setFragmentTexture:sampled atIndex:limits::texture_image];
                    id<MTLRenderPipelineState> pipeline = nil;
                    switch (draw->pipeline) {
                        case SceneSchedule::Pipeline::Scene:
                            break;
                        case SceneSchedule::Pipeline::Under:
                            pipeline = under_pipeline_;
                            break;
                        case SceneSchedule::Pipeline::Particles:
                            pipeline = particle_pipeline_;
                            break;
                        case SceneSchedule::Pipeline::Effect:
                            pipeline = effect_pipeline(prepared.effects.at(draw->source).program);
                            break;
                        case SceneSchedule::Pipeline::Blend:
                            pipeline = blend_pipeline_;
                            break;
                    }
                    if (draw->extra == SceneSchedule::Extra::Effect) {
                        // An effect's textures, in the program's order.
                        const auto& effect = prepared.effects.at(draw->source);
                        constexpr auto slots = std::size_t(weft::constants::max_effect_textures);
                        ensure(effect.textures.size() <= slots, "effect textures");
                        for (std::size_t t = 0; t < slots; ++t) {
                            id<MTLTexture> bound = t < effect.textures.size()
                                                       ? textures_.at(effect.textures[t]->id).image.texture
                                                       : blank_texture_;
                            [encoder setFragmentTexture:bound atIndex:limits::texture_effects + int(t)];
                        }
                        for (const auto& texture : effect.textures) {
                            slot_->leases.push_back(texture);
                        }
                    } else if (draw->extra == SceneSchedule::Extra::Mask) {
                        // The image of the item's mask where an effect's first texture goes (scene.hlsl, maskImage).
                        const auto& image = prepared.mask_images.at(draw->source);
                        ensure(image != nullptr, "a mask's image");
                        [encoder setFragmentTexture:textures_.at(image->id).image.texture
                                            atIndex:limits::texture_effects];
                        slot_->leases.push_back(image);
                    } else if (draw->extra == SceneSchedule::Extra::Sprite) {
                        // The emission's sprite where an effect's first texture goes.
                        id<MTLTexture> sprite = blank_texture_;
                        if (const auto& image = prepared.sprites.at(draw->source)) {
                            sprite = textures_.at(image->id).image.texture;
                            slot_->leases.push_back(image);
                        }
                        [encoder setFragmentTexture:sprite atIndex:limits::texture_effects];
                    }
                    if (draw->backdrop) {
                        // A blending group's backdrop where an effect's second texture goes (scene.hlsl,
                        // blendBackdrop).
                        [encoder setFragmentTexture:chains.at(*draw->backdrop).back().texture
                                            atIndex:limits::texture_effects + 1];
                    }
                    if (pipeline) {
                        [encoder setRenderPipelineState:pipeline];
                    }
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                                vertexStart:0
                                vertexCount:draw->vertices
                              instanceCount:draw->count
                               baseInstance:draw->first];
                    ++draws;
                    if (pipeline) {
                        [encoder setRenderPipelineState:scene_pipeline_];
                    }
                } else if (const auto* copy = std::get_if<SceneSchedule::Copy>(&step)) {
                    // The region of the target the backdrop reads, into its chain's first texture.
                    ensure(encoder == nil, "a copy outside a target");
                    const auto& to = chains.at(copy->chain).front();
                    id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
                    ensure(blit != nil, "a blit command encoder");
                    blit.label = @"weft backdrop copy";
                    [blit copyFromTexture:texture_of(copy->from)
                               sourceSlice:0
                               sourceLevel:0
                              sourceOrigin:MTLOriginMake(NSUInteger(copy->region[0]), NSUInteger(copy->region[1]), 0)
                                sourceSize:MTLSizeMake(to.width, to.height, 1)
                                 toTexture:to.texture
                          destinationSlice:0
                          destinationLevel:0
                         destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [blit endEncoding];
                } else if (const auto* f = std::get_if<SceneSchedule::Filter>(&step)) {
                    // A pass of the chain: its filter over all of its source into the next texture, cleared first.
                    ensure(encoder == nil, "a filter outside a target");
                    const auto& over = chains.at(f->chain).at(f->pass + 1);
                    const auto& filtering = schedule.chains.at(f->chain).passes.at(f->pass);
                    id<MTLRenderCommandEncoder> pass_encoder =
                        begin(over.texture, over.width, over.height, paint::Color{});
                    pass_encoder.label = @"weft backdrop filter";
                    [pass_encoder setRenderPipelineState:backdrop_pipelines_.at(std::size_t(filtering.first))];
                    [pass_encoder setFragmentBytes:&filtering.second
                                            length:sizeof(BackdropConstants)
                                           atIndex:limits::backdrop_buffer_constants];
                    [pass_encoder setFragmentTexture:chains.at(f->chain).at(f->pass).texture
                                             atIndex:limits::backdrop_texture_source];
                    [pass_encoder setFragmentSamplerState:mirror_sampler_ atIndex:limits::backdrop_sampler_mirror];
                    [pass_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    [pass_encoder endEncoding];
                }
            }
            ensure(encoder == nil, "the schedule ends with its target ended");
        } catch (...) {
            // The host's command buffer cannot be committed with an encoder open: end it before the failure leaves.
            if (encoder) {
                [encoder endEncoding];
            }
            throw;
        }
        backdrop_passes += schedule.backdrops;
        backdrop_copies += schedule.copies;
        if (const auto unreadable = schedule.unreadable_backdrops + schedule.unreadable_copies) {
            reasons["backdrop of an unreadable target"] += unreadable;
        }
    }
} // namespace weft::render::metal
