#pragma once

// The engine side of the in-game sample on Metal (Objective-C++, ARC): its camera, its world (a sky, a grid floor lit
// by a turning core, and the panels), drawn 4x multisampled into an sRGB target in linear light, and weft's renderer
// with the export texture extension (include/weft/render_export_texture.h): each panel's page is drawn into an image of
// its export only when the View published a frame that draws differently, in a command buffer of its own committed
// before the world's, and the world samples the image the panel holds with mips and anisotropy. The panel lets go of
// the image it held with the serial of the last frame that sampled it.
#import <Metal/Metal.h>

#include "ui.hpp"

#include <weft/render_backend.h>
#include <weft/render_backend_metal.h>
#include <weft/render_export_texture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

namespace ingame {
    inline void check(weft_render_result result, const std::string& what) {
        if (result != WEFT_RENDER_OK) {
            throw Failure(what + ": render result " + std::to_string(result));
        }
    }

    // A camera orbiting a point: yaw about +y (0 looks along -z), pitch above the horizon, distance (m).
    struct Camera {
        simd_float3 target = simd_make_float3(0, 1.5f, 0);
        float yaw{};
        float pitch = 0.1f;
        float distance = 6.2f;
        float fov = 0.9f;

        simd_float3 eye() const {
            return target + simd_make_float3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                             std::cos(pitch) * std::cos(yaw)) *
                                distance;
        }

        // Its basis: right, up and forward.
        std::array<simd_float3, 3> basis() const {
            const simd_float3 forward = simd_normalize(target - eye());
            const simd_float3 right = simd_normalize(simd_cross(forward, simd_make_float3(0, 1, 0)));
            return {right, simd_cross(right, forward), forward};
        }

        // Clip space of Metal (y up, z from 0 at near to 1 at far) from the world, right-handed.
        simd_float4x4 view_projection(float aspect) const {
            const auto [s, u, f] = basis();
            const simd_float3 e = eye();
            const simd_float4x4 view = simd_matrix(simd_make_float4(s.x, u.x, -f.x, 0), simd_make_float4(s.y, u.y, -f.y, 0),
                                                   simd_make_float4(s.z, u.z, -f.z, 0),
                                                   simd_make_float4(-simd_dot(s, e), -simd_dot(u, e), simd_dot(f, e), 1));
            const float near = 0.05f;
            const float far = 200.f;
            const float y = 1 / std::tan(fov / 2);
            const float z = far / (near - far);
            const simd_float4x4 projection =
                simd_matrix(simd_make_float4(y / aspect, 0, 0, 0), simd_make_float4(0, y, 0, 0),
                            simd_make_float4(0, 0, z, -1), simd_make_float4(0, 0, z * near, 0));
            return simd_mul(projection, view);
        }

        // The ray through a point of the target, x and y from 0 to 1 from its top left.
        simd_float3 ray(float x, float y, float aspect) const {
            const auto [s, u, f] = basis();
            const float t = std::tan(fov / 2);
            return simd_normalize(f + s * ((x * 2 - 1) * t * aspect) + u * ((1 - y * 2) * t));
        }

        // Where a world point lands on the target, x and y from 0 to 1 from its top left.
        simd_float2 project(simd_float3 p, float aspect) const {
            const simd_float4 c = simd_mul(view_projection(aspect), simd_make_float4(p, 1));
            return simd_make_float2((c.x / c.w + 1) / 2, (1 - c.y / c.w) / 2);
        }
    };

    // Linear light throughout; the target's sRGB format encodes it. Constants of a frame, a panel and the core.
    constexpr const char* world_msl = R"(
#include <metal_stdlib>
using namespace metal;
struct Frame { float4x4 view_projection; float4 eye; float4 fog; float4 core; float4 sky; };
struct Place { float4 center; float4 right; float4 up; };

float fogged(float3 world, constant Frame& f) {
    return 1 - exp(-length(world - f.eye.xyz) * 0.035);
}

struct SkyOut { float4 position [[position]]; float y; };
vertex SkyOut sky_vs(uint v [[vertex_id]]) {
    const float2 p = float2((v << 1) & 2, v & 2) * 2 - 1;
    SkyOut o;
    o.position = float4(p, 0, 1);
    o.y = p.y;
    return o;
}
fragment float4 sky_fs(SkyOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    // Elevation of the view direction: the camera's pitch and the point's height in the field of view.
    const float elevation = f.sky.x + atan(in.y * f.sky.y);
    const float3 zenith = float3(0.004, 0.008, 0.022);
    return float4(mix(f.fog.rgb, zenith, saturate(elevation * 1.8)), 1);
}

struct GroundOut { float4 position [[position]]; float3 world; };
vertex GroundOut ground_vs(uint v [[vertex_id]], constant Frame& f [[buffer(0)]]) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 q = (corners[v] * 2 - 1) * 60;
    GroundOut o;
    o.world = float3(q.x, 0, q.y);
    o.position = f.view_projection * float4(o.world, 1);
    return o;
}
fragment float4 ground_fs(GroundOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    const float2 p = in.world.xz;
    const float2 minor = abs(fract(p - 0.5) - 0.5) / fwidth(p);
    const float2 major = abs(fract(p / 4 - 0.5) - 0.5) / fwidth(p / 4);
    const float a = 1 - min(min(minor.x, minor.y), 1.0);
    const float b = 1 - min(min(major.x, major.y), 1.0);
    float3 color = float3(0.006, 0.009, 0.013) + a * float3(0.010, 0.030, 0.045) + b * float3(0.02, 0.07, 0.10);
    // The core lights the floor around it.
    const float d = length(p);
    color += f.core.rgb * (0.22 / (1 + d * d * 0.9));
    return float4(mix(color, f.fog.rgb, fogged(in.world, f)), 1);
}

struct CoreOut { float4 position [[position]]; float3 normal; float3 world; };
vertex CoreOut core_vs(uint v [[vertex_id]], constant Frame& f [[buffer(0)]], constant float4x4& model [[buffer(1)]]) {
    // A cube: six faces of two triangles, each face's corners from its normal and two tangents.
    const float3 normals[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1),
                               float3(0, 0, -1)};
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1)};
    const float3 n = normals[v / 6];
    const float3 t = abs(n.y) > 0.5 ? float3(1, 0, 0) : float3(0, 1, 0);
    const float3 b = cross(n, t);
    const float2 c = corners[v % 6];
    const float3 local = n + t * c.x + b * c.y;
    CoreOut o;
    const float4 world = model * float4(local, 1);
    o.world = world.xyz;
    o.normal = normalize((model * float4(n, 0)).xyz);
    o.position = f.view_projection * world;
    return o;
}
fragment float4 core_fs(CoreOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    const float3 view = normalize(f.eye.xyz - in.world);
    const float rim = pow(1 - saturate(dot(in.normal, view)), 2.0);
    const float light = saturate(dot(in.normal, normalize(float3(0.4, 0.8, 0.5))));
    const float3 color = f.core.rgb * (0.3 + 0.4 * light) + rim * f.core.rgb * 0.9 + 0.01 * light;
    return float4(mix(color, f.fog.rgb, fogged(in.world, f)), 1);
}

struct PanelOut { float4 position [[position]]; float2 uv; float3 world; };
vertex PanelOut panel_vs(uint v [[vertex_id]], constant Frame& f [[buffer(0)]], constant Place& p [[buffer(1)]]) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 q = corners[v];
    PanelOut o;
    o.uv = q;
    o.world = p.center.xyz + p.right.xyz * (q.x * 2 - 1) + p.up.xyz * (1 - q.y * 2);
    o.position = f.view_projection * float4(o.world, 1);
    return o;
}
// The page's image is linear premultiplied: blended with One, OneMinusSourceAlpha. From behind, the panel's back.
fragment float4 panel_fs(PanelOut in [[stage_in]], bool front [[front_facing]], constant Frame& f [[buffer(0)]],
                         texture2d<float> page [[texture(0)]], sampler s [[sampler(0)]]) {
    const float4 color = page.sample(s, in.uv);
    if (!front) {
        return float4(0.010, 0.012, 0.016, 1) * color.a;
    }
    return float4(mix(color.rgb, f.fog.rgb * color.a, fogged(in.world, f)), color.a);
}
)";

    struct FrameConstants {
        simd_float4x4 view_projection;
        simd_float4 eye;
        simd_float4 fog;
        simd_float4 core;
        // x: the camera's pitch, y: tan of half its field of view.
        simd_float4 sky;
    };

    struct PlaceConstants {
        simd_float4 center;
        simd_float4 right;
        simd_float4 up;
    };

    // What one frame did on the CPU: pages drawn into their exports, and those that found every image of their ring
    // busy (they draw next frame).
    struct FrameReport {
        std::uint64_t serial{};
        unsigned redraws{};
        unsigned busy{};
    };

    struct World {
        static constexpr std::uint64_t queue_name = 1;
        static constexpr NSUInteger samples = 4;
        static constexpr MTLPixelFormat color_format = MTLPixelFormatBGRA8Unorm_sRGB;
        static constexpr MTLPixelFormat depth_format = MTLPixelFormatDepth32Float;
        // Images of each panel's ring: the one it shows, one the frame in flight may still sample, one to draw.
        static constexpr std::uint32_t ring = 3;

        id<MTLDevice> device;
        id<MTLCommandQueue> queue;
        id<MTLRenderPipelineState> sky;
        id<MTLRenderPipelineState> ground;
        id<MTLRenderPipelineState> core;
        id<MTLRenderPipelineState> panel;
        id<MTLDepthStencilState> depth_write;
        id<MTLDepthStencilState> depth_test;
        id<MTLDepthStencilState> depth_off;
        // Trilinear with anisotropy, and without mips (M toggles, to compare).
        id<MTLSamplerState> mipped;
        id<MTLSamplerState> unmipped;
        bool use_mips = true;
        // The multisampled color and depth of the target's size (memoryless on Apple GPUs: tile memory only).
        id<MTLTexture> msaa;
        id<MTLTexture> depth;

        weft_render_metal* backend{};
        weft_renderer* renderer{};
        weft_render_identity identity{};
        // Per panel: its export and the image it shows (image.kind 0: none yet), and the revision drawn into it.
        struct Shown {
            weft_render_handle exported{};
            weft_render_export_image image{};
            std::uint64_t revision{};
            unsigned redraws{};
            unsigned busy{};
        };
        std::vector<Shown> shown;
        std::uint64_t serial{};
        std::uint64_t completed{};

        explicit World(const Ui& ui) {
            device = MTLCreateSystemDefaultDevice();
            check(device != nil, "a Metal device");
            queue = [device newCommandQueue];
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:@(world_msl) options:nil error:&error];
            check(library != nil, std::string("the world's shaders: ") +
                                      (error ? error.localizedDescription.UTF8String : "unknown error"));
            sky = pipeline(library, @"sky", false);
            ground = pipeline(library, @"ground", false);
            core = pipeline(library, @"core", false);
            panel = pipeline(library, @"panel", true);
            depth_write = depth_state(MTLCompareFunctionLess, true);
            depth_test = depth_state(MTLCompareFunctionLess, false);
            depth_off = depth_state(MTLCompareFunctionAlways, false);
            MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
            s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
            s.sAddressMode = s.tAddressMode = MTLSamplerAddressModeClampToEdge;
            s.mipFilter = MTLSamplerMipFilterNotMipmapped;
            unmipped = [device newSamplerStateWithDescriptor:s];
            s.mipFilter = MTLSamplerMipFilterLinear;
            s.maxAnisotropy = 8;
            mipped = [device newSamplerStateWithDescriptor:s];

            // weft's Metal backend on the engine's device, a renderer of it, and its export extension.
            const weft_render_metal_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, (__bridge void*)device,
                                              queue_name, 0};
            weft_render_backend table{};
            check(weft_render_metal_create(&desc, &backend, &table), "the Metal backend");
            // Every panel may draw in the same command buffer before it is submitted: as many encoded uses as panels
            // (the default is two). The other limits are the defaults.
            const weft_renderer_desc limits{sizeof(limits), WEFT_RENDER_ABI_VERSION, 8, 8, 64, 65536, 512, 0,
                                            16u << 20};
            check(weft_renderer_create(&table, &limits, &renderer), "renderer");
            weft_render_export_backend exporter{};
            check(weft_render_metal_export(backend, &exporter), "the export extension");
            check(weft_renderer_install_export_backend(renderer, &exporter), "install the export extension");
            weft_render_info info{};
            info.struct_size = sizeof(info);
            check(weft_renderer_get_info(renderer, &info), "renderer info");
            identity = info.identity;
            // An export per panel: its page's device px, sRGB RGBA8 (linear when sampled), every mip level.
            for (const auto& p : ui.panels) {
                const weft_render_export_desc d{sizeof(d),
                                                WEFT_RENDER_EXPORT_ABI_VERSION,
                                                identity,
                                                p.pixel_width(),
                                                p.pixel_height(),
                                                WEFT_RENDER_TARGET_SRGB_RGBA8,
                                                0,
                                                ring,
                                                0};
                Shown s;
                check(weft_renderer_create_export(renderer, &d, &s.exported), std::string("the export of ") + p.name);
                shown.push_back(s);
            }
        }

        ~World() {
            // Every frame completed; then the images, the exports and the renderer go.
            id<MTLCommandBuffer> last = [queue commandBuffer];
            [last commit];
            [last waitUntilCompleted];
            if (serial) {
                weft_renderer_notify_completed(renderer, identity.device_epoch, queue_name, serial);
            }
            for (auto& s : shown) {
                if (s.image.image.kind) {
                    weft_renderer_release_export_image(renderer, s.image.image, queue_name, 0);
                }
                weft_renderer_destroy_export(renderer, s.exported);
            }
            if (const auto v = weft_renderer_destroy(renderer); v != WEFT_RENDER_OK) {
                std::fprintf(stderr, "renderer destroy: render result %d\n", int(v));
                return;
            }
            weft_render_metal_destroy(backend);
        }

        World(const World&) = delete;
        World& operator=(const World&) = delete;

        id<MTLRenderPipelineState> pipeline(id<MTLLibrary> library, NSString* name, bool blend) {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.label = name;
            d.vertexFunction = [library newFunctionWithName:[name stringByAppendingString:@"_vs"]];
            d.fragmentFunction = [library newFunctionWithName:[name stringByAppendingString:@"_fs"]];
            d.rasterSampleCount = samples;
            d.colorAttachments[0].pixelFormat = color_format;
            d.depthAttachmentPixelFormat = depth_format;
            if (blend) {
                auto* c = d.colorAttachments[0];
                c.blendingEnabled = YES;
                c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorOne;
                c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            }
            NSError* error = nil;
            id<MTLRenderPipelineState> out = [device newRenderPipelineStateWithDescriptor:d error:&error];
            check(out != nil, std::string("the pipeline ") + name.UTF8String);
            return out;
        }

        id<MTLDepthStencilState> depth_state(MTLCompareFunction compare, bool write) {
            MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = compare;
            d.depthWriteEnabled = write;
            return [device newDepthStencilStateWithDescriptor:d];
        }

        // The multisampled attachments for a target of width x height.
        void attachments(unsigned width, unsigned height) {
            if (msaa && msaa.width == width && msaa.height == height) {
                return;
            }
            const bool tiles = [device supportsFamily:MTLGPUFamilyApple7];
            const auto make = [&](MTLPixelFormat format) {
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                             width:width
                                                                                            height:height
                                                                                         mipmapped:NO];
                d.textureType = MTLTextureType2DMultisample;
                d.sampleCount = samples;
                d.usage = MTLTextureUsageRenderTarget;
                d.storageMode = tiles ? MTLStorageModeMemoryless : MTLStorageModePrivate;
                return [device newTextureWithDescriptor:d];
            };
            msaa = make(color_format);
            depth = make(depth_format);
        }

        // The serial `done` completed (on the owner thread): images released with it can be drawn again.
        void complete(std::uint64_t done) {
            if (done > completed) {
                completed = done;
                check(weft_renderer_notify_completed(renderer, identity.device_epoch, queue_name, done), "completed");
            }
        }

        // The pages whose View published a frame that draws differently, each into a free image of its export, in a
        // command buffer committed before the world's; reported submitted as frame serial n. on_ui_complete gets its
        // GPU time (ms) on Metal's thread.
        void draw_pages(Ui& ui, std::uint64_t n, FrameReport& report, const std::function<void(double)>& on_ui_complete,
                        Phases* phases) {
            id<MTLCommandBuffer> command = nil;
            std::array<weft_render_handle, 8> uses{};
            unsigned used{};
            for (std::size_t i = 0; i < ui.panels.size(); ++i) {
                auto& p = ui.panels[i];
                auto& s = shown[i];
                weft_view_frame* frame{};
                if (weft_view_acquire_frame(ui.runtime, p.view, &frame) != WEFT_OK) {
                    continue;
                }
                weft_view_frame_info info{};
                info.struct_size = sizeof(info);
                check(weft_view_frame_get_info(frame, &info), "frame info");
                // A frame of the revision the image holds draws alike: nothing to do.
                if (s.image.image.kind && info.revision && info.revision == s.revision) {
                    weft_view_frame_release(frame);
                    continue;
                }
                if (!command) {
                    command = [queue commandBuffer];
                    command.label = @"weft pages";
                }
                const auto begin = Phases::Clock::now();
                const weft_render_scene_frame scene{sizeof(scene), WEFT_RENDER_ABI_VERSION, frame, 0, 0, nullptr};
                weft_render_handle prepared{};
                const auto ready = weft_renderer_prepare_scene(renderer, &scene, &prepared);
                weft_view_frame_release(frame);
                check(ready, std::string("prepare the page of ") + p.name);
                const auto encoding = Phases::Clock::now();
                if (phases) {
                    phases->prepare[i] = std::chrono::duration<double, std::milli>(encoding - begin).count();
                }
                const weft_render_metal_pass native{sizeof(native), WEFT_RENDER_ABI_VERSION, (__bridge void*)command,
                                                    nullptr,        0,
                                                    0,              {0, 0, 0, 0}};
                const weft_render_pass pass{sizeof(pass),     WEFT_RENDER_ABI_VERSION, identity,
                                            p.pixel_width(),  p.pixel_height(),        WEFT_RENDER_TARGET_SRGB_RGBA8,
                                            1,                &native,                 sizeof(native)};
                weft_render_handle use{};
                weft_render_export_image image{};
                image.struct_size = sizeof(image);
                const auto v = weft_renderer_encode_export(renderer, prepared, s.exported, &pass, &use, &image);
                weft_renderer_release_prepared(renderer, prepared);
                if (phases) {
                    phases->encode[i] = Phases::since(encoding);
                }
                if (v == WEFT_RENDER_BUSY) {
                    // Every image is shown, in flight or not yet free: the panel shows what it has, and draws next
                    // frame.
                    ++s.busy;
                    ++report.busy;
                    continue;
                }
                if (v != WEFT_RENDER_OK) {
                    std::string why;
                    weft_renderer_last_failure(
                        renderer,
                        [](void* user, const char* text, size_t size) {
                            static_cast<std::string*>(user)->assign(text, size);
                        },
                        &why);
                    throw Failure(std::string("draw the page of ") + p.name + ": render result " +
                                  std::to_string(v) + ": " + why);
                }
                uses[used++] = use;
                // The image it showed was sampled last by the frame before this one.
                if (s.image.image.kind) {
                    check(weft_renderer_release_export_image(renderer, s.image.image, queue_name, n - 1),
                          "release the image shown");
                }
                s.image = image;
                s.revision = info.revision;
                ++s.redraws;
                ++report.redraws;
            }
            if (!command) {
                return;
            }
            const auto committing = Phases::Clock::now();
            const std::function<void(double)> finish = on_ui_complete;
            [command addCompletedHandler:^(id<MTLCommandBuffer> done) {
              finish((done.GPUEndTime - done.GPUStartTime) * 1000);
            }];
            [command commit];
            for (unsigned k = 0; k < used; ++k) {
                check(weft_renderer_notify_submitted(renderer, uses[k], queue_name, n), "submitted");
            }
            if (phases) {
                phases->pages_commit = Phases::since(committing);
            }
        }

        // One frame into target (width x height px, color_format) at time t (s): the pages that changed, then the
        // world. before_commit runs on the world's command buffer before it is committed (a drawable's present);
        // on_complete gets the serial and the world's GPU time (ms), on_ui_complete the pages' GPU time, both on
        // Metal's thread.
        FrameReport frame(Ui& ui, const Camera& camera, double t, id<MTLTexture> target, unsigned width,
                          unsigned height, const std::function<void(id<MTLCommandBuffer>)>& before_commit,
                          const std::function<void(std::uint64_t, double)>& on_complete,
                          const std::function<void(double)>& on_ui_complete, Phases* phases = nullptr) {
            FrameReport report;
            const auto n = report.serial = ++serial;
            draw_pages(ui, n, report, on_ui_complete, phases);
            const auto world_begin = Phases::Clock::now();

            attachments(width, height);
            const float aspect = float(width) / float(height);
            const simd_float3 eye = camera.eye();
            const simd_float3 glow = ui.core_color();
            const FrameConstants constants{camera.view_projection(aspect), simd_make_float4(eye, 1),
                                           simd_make_float4(0.030f, 0.046f, 0.070f, 1), simd_make_float4(glow, 1),
                                           simd_make_float4(camera.pitch, std::tan(camera.fov / 2), 0, 0)};
            id<MTLCommandBuffer> command = [queue commandBuffer];
            command.label = @"world";
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = msaa;
            pass.colorAttachments[0].resolveTexture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            pass.depthAttachment.texture = depth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = 1;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            encoder.label = @"world";
            [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:0];
            [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:0];
            // The sky over everything, then the floor and the core.
            [encoder setDepthStencilState:depth_off];
            [encoder setRenderPipelineState:sky];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder setDepthStencilState:depth_write];
            [encoder setRenderPipelineState:ground];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            const float angle = float(t) * ui.core_speed();
            const simd_float4x4 model = simd_mul(
                simd_matrix(simd_make_float4(0.42f, 0, 0, 0), simd_make_float4(0, 0.42f, 0, 0),
                            simd_make_float4(0, 0, 0.42f, 0), simd_make_float4(0, 1.25f, 0, 1)),
                simd_mul(rotation(simd_make_float3(0, 1, 0), angle),
                         rotation(simd_normalize(simd_make_float3(1, 0, 1)), 0.6f)));
            [encoder setRenderPipelineState:core];
            [encoder setVertexBytes:&model length:sizeof(model) atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];
            // The panels, farthest first: they blend over what is behind them and leave the depth as it is.
            std::vector<std::size_t> order(ui.panels.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return simd_distance(ui.panels[a].center, eye) > simd_distance(ui.panels[b].center, eye);
            });
            [encoder setDepthStencilState:depth_test];
            [encoder setRenderPipelineState:panel];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setFragmentSamplerState:use_mips ? mipped : unmipped atIndex:0];
            for (const auto i : order) {
                const auto& s = shown[i];
                if (!s.image.image.kind) {
                    continue;
                }
                const auto& p = ui.panels[i];
                const PlaceConstants place{simd_make_float4(p.center, 1), simd_make_float4(p.right, 0),
                                           simd_make_float4(p.up, 0)};
                [encoder setVertexBytes:&place length:sizeof(place) atIndex:1];
                id<MTLTexture> image = (__bridge id<MTLTexture>)reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(s.image.native_texture));
                [encoder setFragmentTexture:image atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
            }
            [encoder endEncoding];
            if (before_commit) {
                before_commit(command);
            }
            const auto committing = Phases::Clock::now();
            const std::function<void(std::uint64_t, double)> finish = on_complete;
            [command addCompletedHandler:^(id<MTLCommandBuffer> done) {
              finish(n, (done.GPUEndTime - done.GPUStartTime) * 1000);
            }];
            [command commit];
            if (phases) {
                phases->world = std::chrono::duration<double, std::milli>(committing - world_begin).count();
                phases->commit = Phases::since(committing);
            }
            return report;
        }

        static simd_float4x4 rotation(simd_float3 axis, float angle) {
            const simd_quatf q = simd_quaternion(angle, axis);
            return simd_matrix4x4(q);
        }
    };
} // namespace ingame
