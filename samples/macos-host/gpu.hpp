#pragma once

// The macOS host sample's Metal side (Objective-C++, ARC): the engine's device and queue, its picture of the world
// (a ground grid and the units, drawn by a pipeline of its own), the minimap texture it writes every frame and imports
// into weft once as a live texture, and the View's frames drawn over its picture by the built-in Metal backend
// (include/weft/render_backend_metal.h), committed by the engine and reported submitted and completed.
#import <Metal/Metal.h>

#include "engine.hpp"

#include <weft/render_backend.h>
#include <weft/render_backend_metal.h>
#include <weft/render_external_texture.h>

#include <array>
#include <cstring>
#include <functional>

namespace demo {
    inline void check(weft_render_result result, const char* what) {
        if (result != WEFT_RENDER_OK) {
            throw Failure(std::string(what) + ": render result " + std::to_string(result));
        }
    }

    // The world's picture: the ground (instance 0, the whole target) and each unit a disc tinted by its health.
    // Premultiplied colors as the target holds them (sRGB-encoded, as weft writes).
    constexpr const char* world_msl = R"(
#include <metal_stdlib>
using namespace metal;
struct World { float2 size; float time; float units; };
struct Unit { float2 center; float radius; float health; };
struct Out { float4 position [[position]]; float2 at; float4 color; float ground; };
vertex Out world_vs(uint v [[vertex_id]], uint i [[instance_id]], constant World& w [[buffer(0)]],
                    constant Unit* units [[buffer(1)]]) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 c = corners[v];
    Out o;
    float2 p;
    if (i == 0) {
        p = c * w.size;
        o.at = p;
        o.ground = 1;
        o.color = float4(0);
    } else {
        const Unit u = units[i - 1];
        p = u.center + (c * 2 - 1) * u.radius;
        o.at = c * 2 - 1;
        o.ground = 0;
        o.color = float4(mix(float3(0.85, 0.22, 0.16), float3(0.25, 0.78, 0.36), u.health), 1);
    }
    o.position = float4(p.x / w.size.x * 2 - 1, 1 - p.y / w.size.y * 2, 0, 1);
    return o;
}
fragment float4 world_fs(Out in [[stage_in]], constant World& w [[buffer(0)]]) {
    if (in.ground > 0.5) {
        const float2 p = in.at;
        float3 base = mix(float3(0.13, 0.20, 0.24), float3(0.07, 0.10, 0.13), p.y / w.size.y);
        const float2 cell = abs(fract(p / 64.0) - 0.5);
        base += 0.035 * step(0.47, max(cell.x, cell.y));
        const float wave = 0.5 + 0.5 * sin(w.time * 0.7 + p.x * 0.004 + p.y * 0.003);
        base += float3(0.0, 0.015, 0.02) * wave;
        return float4(base, 1);
    }
    const float d = length(in.at);
    if (d > 1) {
        discard_fragment();
    }
    const float coverage = clamp((1 - d) * 12, 0.0, 1.0);
    const float ring = smoothstep(0.72, 0.78, d) * (1 - smoothstep(0.9, 0.96, d));
    float3 color = in.color.rgb * (0.7 + 0.3 * (1 - d)) + ring * 0.25;
    return float4(color * coverage, coverage);
}
)";

    struct Gpu {
        id<MTLDevice> device;
        id<MTLCommandQueue> queue;
        id<MTLRenderPipelineState> world;
        id<MTLTexture> minimap;
        // The minimap's texels of the frames in flight, one buffer each.
        std::array<id<MTLBuffer>, 3> staging;
        weft_render_metal* backend{};
        weft_renderer* renderer{};
        weft_render_info info{};
        weft_render_handle minimap_lease{};
        std::uint64_t serial{};
        std::uint64_t completed{};
        unsigned frames{};
        static constexpr std::uint64_t queue_name = 1;

        Gpu() {
            device = MTLCreateSystemDefaultDevice();
            check(device != nil, "a Metal device");
            queue = [device newCommandQueue];
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:@(world_msl) options:nil error:&error];
            check(library != nil, "the world shader");
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [library newFunctionWithName:@"world_vs"];
            d.fragmentFunction = [library newFunctionWithName:@"world_fs"];
            auto* color = d.colorAttachments[0];
            color.pixelFormat = MTLPixelFormatBGRA8Unorm;
            color.blendingEnabled = YES;
            color.sourceRGBBlendFactor = color.sourceAlphaBlendFactor = MTLBlendFactorOne;
            color.destinationRGBBlendFactor = color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            world = [device newRenderPipelineStateWithDescriptor:d error:&error];
            check(world != nil, "the world pipeline");

            // weft's Metal backend on the engine's device, and a renderer of it.
            const weft_render_metal_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, (__bridge void*)device,
                                              queue_name, 0};
            weft_render_backend table{};
            check(weft_render_metal_create(&desc, &backend, &table), "the Metal backend");
            check(weft_renderer_create(&table, nullptr, &renderer), "renderer");
            info.struct_size = sizeof(info);
            check(weft_renderer_get_info(renderer, &info), "renderer info");
            weft_render_external_backend external{};
            check(weft_render_metal_external(backend, &external), "the external texture extension");
            check(weft_renderer_install_external_backend(renderer, &external), "install the extension");

            // The minimap: written by a copy from a staging buffer in every frame, before the UI samples it.
            MTLTextureDescriptor* map = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:map_width
                                                                                         height:map_height
                                                                                      mipmapped:NO];
            map.usage = MTLTextureUsageShaderRead;
            map.storageMode = MTLStorageModePrivate;
            minimap = [device newTextureWithDescriptor:map];
            for (auto& s : staging) {
                s = [device newBufferWithLength:map_width * map_height * 4 options:MTLResourceStorageModeShared];
            }
            const weft_render_metal_texture native{sizeof(native), WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                   (__bridge void*)minimap, 0};
            const weft_render_external_texture texture{sizeof(texture),
                                                       WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                       info.identity,
                                                       1,
                                                       1,
                                                       map_width,
                                                       map_height,
                                                       WEFT_RENDER_TARGET_LINEAR_RGBA8,
                                                       WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED,
                                                       WEFT_RENDER_EXTERNAL_TOP_LEFT,
                                                       WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE,
                                                       0,
                                                       0,
                                                       &native,
                                                       sizeof(native),
                                                       nullptr,
                                                       nullptr};
            check(weft_renderer_import_texture(renderer, &texture, &minimap_lease), "import the minimap");
        }

        ~Gpu() {
            drain();
            weft_renderer_release_texture(renderer, minimap_lease);
            weft_renderer_destroy(renderer);
            weft_render_metal_destroy(backend);
        }

        Gpu(const Gpu&) = delete;
        Gpu& operator=(const Gpu&) = delete;

        // Every committed frame completed: the renderer retires their resources.
        void drain() {
            id<MTLCommandBuffer> last = [queue commandBuffer];
            [last commit];
            [last waitUntilCompleted];
            if (serial) {
                weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue_name, serial);
                completed = serial;
            }
        }

        // The frame serial `done` completed (on the owner thread).
        void complete(std::uint64_t done) {
            if (done > completed) {
                completed = done;
                check(weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue_name, done),
                      "completed");
            }
        }

        // Encodes and commits one frame into target (width x height device px) at time t: the world's picture, the
        // minimap's new texels, and the View's frame over them. before_commit runs on the command buffer before it is
        // committed (a drawable's present); on_complete runs on Metal's thread when it completed, with its serial.
        void frame(Engine& engine, double t, id<MTLTexture> target, unsigned width, unsigned height,
                   const std::function<void(id<MTLCommandBuffer>)>& before_commit,
                   const std::function<void(std::uint64_t)>& on_complete) {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            // The world: ground and units, in the View's logical px (the target is scale times larger).
            struct WorldConstants {
                float size[2];
                float time;
                float units;
            } constants{{engine.width, engine.height}, float(t), float(demo::units)};
            struct Unit {
                float center[2];
                float radius;
                float health;
            };
            std::array<Unit, demo::units> drawn{};
            for (unsigned i = 0; i < demo::units; ++i) {
                const auto [x, y] = engine.place(i, t);
                drawn[i] = {{x, y}, 14, engine.health[i]};
            }
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            // Other pages than the HUD run over a plain backdrop.
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.06, 0.07, 0.09, 1);
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            const MTLViewport viewport{0, 0, double(width), double(height), 0, 1};
            [encoder setViewport:viewport];
            [encoder setRenderPipelineState:world];
            [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
            [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
            [encoder setVertexBytes:drawn.data() length:sizeof(drawn) atIndex:1];
            if (engine.hud) {
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:6
                          instanceCount:demo::units + 1];
            }
            [encoder endEncoding];
            // The minimap's texels for this frame, after the last frame's UI read it.
            id<MTLBuffer> texels = staging[frames++ % staging.size()];
            const auto map = engine.minimap(t);
            std::memcpy(texels.contents, map.data(), map.size());
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            [blit copyFromBuffer:texels
                       sourceOffset:0
                  sourceBytesPerRow:map_width * 4
                sourceBytesPerImage:map_width * map_height * 4
                         sourceSize:MTLSizeMake(map_width, map_height, 1)
                          toTexture:minimap
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            // The View's frame over the world, with the minimap bound to its name.
            weft_render_handle use{};
            bool encoded_ui{};
            weft_view_frame* frame{};
            if (weft_view_acquire_frame(engine.runtime, engine.view, &frame) == WEFT_OK) {
                const weft_render_scene_texture texture{"minimap", 7, 0, minimap_lease};
                const weft_render_scene_frame scene{sizeof(scene), WEFT_RENDER_ABI_VERSION, frame, 0, 1, &texture};
                weft_render_handle prepared{};
                check(weft_renderer_prepare_scene(renderer, &scene, &prepared), "prepare the View's frame");
                weft_view_frame_release(frame);
                const weft_render_metal_pass payload{sizeof(payload),
                                                     WEFT_RENDER_ABI_VERSION,
                                                     (__bridge void*)command,
                                                     (__bridge void*)target,
                                                     0,
                                                     0,
                                                     {0, 0, 0, 0}};
                const weft_render_pass render{sizeof(render), WEFT_RENDER_ABI_VERSION,
                                              info.identity,  width,
                                              height,         WEFT_RENDER_TARGET_LINEAR_BGRA8,
                                              1,              &payload,
                                              sizeof(payload)};
                const auto encoded = weft_renderer_encode(renderer, prepared, &render, &use);
                weft_renderer_release_prepared(renderer, prepared);
                if (encoded == WEFT_RENDER_OK) {
                    encoded_ui = true;
                } else {
                    // The world's picture is committed without the UI this frame.
                    std::fprintf(stderr, "weft encode: render result %d\n", int(encoded));
                    if (use.generation) {
                        weft_renderer_discard_encoded(renderer, use);
                    }
                }
            }
            if (before_commit) {
                before_commit(command);
            }
            const auto done = ++serial;
            // The block keeps its own copy of the callback: frame() returns long before the frame completes.
            const std::function<void(std::uint64_t)> finish = on_complete;
            [command addCompletedHandler:^(id<MTLCommandBuffer>) {
              finish(done);
            }];
            [command commit];
            if (encoded_ui) {
                check(weft_renderer_notify_submitted(renderer, use, queue_name, done), "submitted");
            }
        }
    };
} // namespace demo
