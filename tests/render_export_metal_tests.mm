// The export texture extension on Metal (include/weft/render_export_texture.h, docs/ingame-ui.md), through the C ABI
// on this Mac's GPU: a View's page drawn into export images of RGBA16F and sRGB RGBA8 holds linear premultiplied color
// where the page's boxes are (an opaque one, a translucent one, and transparency), its lower levels filled; the ring
// hands out its images in turn, one waits for the sampling it was released with, and every image goes once released.
#import <Metal/Metal.h>

#include "weft/render_backend_metal.h"
#include "weft/render_export_texture.h"
#include "weft/weft.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    void require(bool condition, const std::string& what) {
        if (!condition) {
            throw std::runtime_error(what);
        }
    }

    void expect(weft_render_result result, weft_render_result wanted, const std::string& what) {
        require(result == wanted, what + ": result " + std::to_string(int(result)));
    }

    // sRGB transfer function, as export_resolve.hlsl.
    float to_linear(float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    float to_encoded(float c) {
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
    }

    // What the page shows on a 64 x 32 View: an opaque orange box over the left half, a translucent grey one over the
    // top right quarter, nothing below it.
    constexpr const char* stylesheet =
        "#root{position:relative;width:100%;height:100%}"
        ".a{position:absolute;left:0;top:0;width:32px;height:32px;background-color:rgb(255,128,0)}"
        ".b{position:absolute;left:32px;top:0;width:32px;height:16px;background-color:rgba(128,128,128,0.5)}";
    constexpr const char* script = "for (const c of ['a', 'b']) {"
                                   "  const d = document.createElement('div');"
                                   "  d.className = c;"
                                   "  document.getElementById('root').appendChild(d);"
                                   "}";

    struct Pixel {
        float r, g, b, a;
    };

    // The linear premultiplied colors the page's areas hold: CSS colors are sRGB, composited over transparent.
    Pixel orange() {
        return {1, to_linear(128 / 255.f), 0, 1};
    }

    Pixel grey() {
        const float c = to_linear(128 / 255.f) * 0.5f;
        return {c, c, c, 0.5f};
    }

    bool near(const Pixel& a, const Pixel& b, float tolerance) {
        return std::fabs(a.r - b.r) <= tolerance && std::fabs(a.g - b.g) <= tolerance &&
               std::fabs(a.b - b.b) <= tolerance && std::fabs(a.a - b.a) <= tolerance;
    }

    std::string text(const Pixel& p) {
        char out[96];
        std::snprintf(out, sizeof out, "(%.4f %.4f %.4f %.4f)", p.r, p.g, p.b, p.a);
        return out;
    }

    // A level of a texture read back through a shared buffer: its rows, tightly packed.
    std::vector<std::uint8_t> read(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture,
                                   NSUInteger level, NSUInteger bytes_per_pixel) {
        const NSUInteger width = std::max<NSUInteger>(1, texture.width >> level);
        const NSUInteger height = std::max<NSUInteger>(1, texture.height >> level);
        const NSUInteger row = width * bytes_per_pixel;
        id<MTLBuffer> buffer = [device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:row * height];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        const auto* bytes = static_cast<const std::uint8_t*>(buffer.contents);
        return {bytes, bytes + row * height};
    }

    // Half floats as Apple's CPUs read them.
    Pixel half_pixel(const std::vector<std::uint8_t>& rows, NSUInteger width, NSUInteger x, NSUInteger y) {
        __fp16 h[4];
        std::memcpy(h, rows.data() + (y * width + x) * 8, sizeof h);
        return {float(h[0]), float(h[1]), float(h[2]), float(h[3])};
    }

    // An sRGB RGBA8 texel as sampling returns it: color decoded, alpha as it is.
    Pixel srgb_pixel(const std::vector<std::uint8_t>& rows, NSUInteger width, NSUInteger x, NSUInteger y) {
        const auto* p = rows.data() + (y * width + x) * 4;
        return {to_linear(p[0] / 255.f), to_linear(p[1] / 255.f), to_linear(p[2] / 255.f), p[3] / 255.f};
    }

    id<MTLTexture> texture_of(const weft_render_export_image& image) {
        return (__bridge id<MTLTexture>)reinterpret_cast<void*>(static_cast<std::uintptr_t>(image.native_texture));
    }

    struct Gpu {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue;
        weft_render_metal* backend{};
        weft_renderer* renderer{};
        weft_render_identity identity{};
        std::uint64_t serial{};

        Gpu() {
            require(device != nil, "a Metal device");
            queue = [device newCommandQueue];
            const weft_render_metal_desc desc{sizeof(desc), WEFT_RENDER_ABI_VERSION, (__bridge void*)device, 1, 0};
            weft_render_backend table{};
            expect(weft_render_metal_create(&desc, &backend, &table), WEFT_RENDER_OK, "the Metal backend");
            expect(weft_renderer_create(&table, nullptr, &renderer), WEFT_RENDER_OK, "a renderer");
            weft_render_export_backend exporter{};
            expect(weft_render_metal_export(backend, &exporter), WEFT_RENDER_OK, "the export table");
            expect(weft_renderer_install_export_backend(renderer, &exporter), WEFT_RENDER_OK, "the export extension");
            weft_render_info info{};
            info.struct_size = sizeof(info);
            expect(weft_renderer_get_info(renderer, &info), WEFT_RENDER_OK, "renderer info");
            identity = info.identity;
        }

        weft_render_handle create_export(std::uint32_t format, std::uint32_t images) {
            const weft_render_export_desc desc{
                sizeof(desc), WEFT_RENDER_EXPORT_ABI_VERSION, identity, 64, 32, format, 0, images, 0};
            weft_render_handle out{};
            expect(weft_renderer_create_export(renderer, &desc, &out), WEFT_RENDER_OK, "an export");
            return out;
        }

        // Encodes prepared into the export in a command buffer of its own, commits it and reports its submission;
        // waits for it when asked, reporting its completion.
        weft_render_result draw(weft_render_handle prepared, weft_render_handle panel, std::uint32_t format,
                                weft_render_export_image& image, bool wait = true) {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            const weft_render_metal_pass native{
                sizeof(native), WEFT_RENDER_ABI_VERSION, (__bridge void*)command, nullptr, 0, 0, {0, 0, 0, 0}};
            const weft_render_pass pass{
                sizeof(pass), WEFT_RENDER_ABI_VERSION, identity, 64, 32, format, 1, &native, sizeof(native)};
            weft_render_handle use{};
            image = {};
            image.struct_size = sizeof(image);
            const auto v = weft_renderer_encode_export(renderer, prepared, panel, &pass, &use, &image);
            if (v != WEFT_RENDER_OK) {
                return v;
            }
            [command commit];
            expect(weft_renderer_notify_submitted(renderer, use, 1, ++serial), WEFT_RENDER_OK, "a submission");
            if (wait) {
                [command waitUntilCompleted];
                require(command.status == MTLCommandBufferStatusCompleted, "the export's commands ran");
                expect(weft_renderer_notify_completed(renderer, 1, 1, serial), WEFT_RENDER_OK, "its completion");
            }
            return v;
        }
    };

    void LinearPremultipliedLevels(Gpu& gpu, weft_render_handle prepared) {
        // RGBA16F: the values as the resolve wrote them.
        const auto panel = gpu.create_export(WEFT_RENDER_TARGET_LINEAR_RGBA16F, 2);
        weft_render_export_image first{};
        expect(gpu.draw(prepared, panel, WEFT_RENDER_TARGET_LINEAR_RGBA16F, first), WEFT_RENDER_OK, "a 16F export");
        id<MTLTexture> texture = texture_of(first);
        require(first.version == 1 && first.mip_levels == 7 && texture.mipmapLevelCount == 7 &&
                    texture.pixelFormat == MTLPixelFormatRGBA16Float && texture.width == 64,
                "the 16F image");
        const auto top = read(gpu.device, gpu.queue, texture, 0, 8);
        const Pixel left = half_pixel(top, 64, 8, 20);
        const Pixel translucent = half_pixel(top, 64, 48, 8);
        const Pixel clear = half_pixel(top, 64, 48, 24);
        std::cout << "16F level 0: orange " << text(left) << ", grey " << text(translucent) << ", clear "
                  << text(clear) << '\n';
        // Half precision and the scene pass's 8-bit target.
        require(near(left, orange(), 0.004f), "the opaque box, linear: " + text(left) + " for " + text(orange()));
        require(near(translucent, grey(), 0.004f),
                "the translucent box, linear premultiplied: " + text(translucent) + " for " + text(grey()));
        require(near(clear, {0, 0, 0, 0}, 0), "transparency: " + text(clear));
        // Level 1 (32 x 16) inside the boxes holds their color; the last level is the mean of the page.
        const auto one = read(gpu.device, gpu.queue, texture, 1, 8);
        require(near(half_pixel(one, 32, 4, 10), orange(), 0.004f), "level 1 inside the orange box");
        const auto last = read(gpu.device, gpu.queue, texture, 6, 8);
        const Pixel mean = half_pixel(last, 1, 0, 0);
        const Pixel o = orange();
        const Pixel g = grey();
        const Pixel wanted{o.r / 2 + g.r / 4, o.g / 2 + g.g / 4, o.b / 2 + g.b / 4, o.a / 2 + g.a / 4};
        std::cout << "16F level 6: " << text(mean) << " for the mean " << text(wanted) << '\n';
        require(near(mean, wanted, 0.01f), "the last level, the page's mean: " + text(mean) + " for " + text(wanted));

        // sRGB RGBA8: sampling decodes the same colors.
        const auto srgb = gpu.create_export(WEFT_RENDER_TARGET_SRGB_RGBA8, 1);
        weft_render_export_image encoded{};
        expect(gpu.draw(prepared, srgb, WEFT_RENDER_TARGET_SRGB_RGBA8, encoded), WEFT_RENDER_OK, "an sRGB export");
        require(texture_of(encoded).pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "the sRGB image");
        const auto bytes = read(gpu.device, gpu.queue, texture_of(encoded), 0, 4);
        const Pixel sampled = srgb_pixel(bytes, 64, 48, 8);
        // One step of the 8-bit encoding at the grey's level.
        const float step = to_linear(to_encoded(grey().r) + 1 / 255.f) - grey().r;
        std::cout << "sRGB level 0: orange " << text(srgb_pixel(bytes, 64, 8, 20)) << ", grey " << text(sampled)
                  << '\n';
        require(near(srgb_pixel(bytes, 64, 8, 20), orange(), 0.004f), "the sRGB image's orange");
        require(near(sampled, grey(), step + 0.004f), "the sRGB image's grey: " + text(sampled));
        expect(weft_renderer_release_export_image(gpu.renderer, encoded.image, 1, gpu.serial), WEFT_RENDER_OK,
               "releasing the sRGB image");
        expect(weft_renderer_destroy_export(gpu.renderer, srgb), WEFT_RENDER_OK, "the sRGB export goes");

        // The ring: the second image next, still drawing, then none free.
        weft_render_export_image second{};
        expect(gpu.draw(prepared, panel, WEFT_RENDER_TARGET_LINEAR_RGBA16F, second, false), WEFT_RENDER_OK,
               "the second image");
        require(second.version == 2 && texture_of(second) != texture, "another image of the ring");
        weft_render_export_image none{};
        expect(gpu.draw(prepared, panel, WEFT_RENDER_TARGET_LINEAR_RGBA16F, none), WEFT_RENDER_BUSY,
               "a ring of two, both leased");
        // The host samples the first in its next submission and releases it with that serial: busy until it completed.
        const auto sampling = gpu.serial + 1;
        expect(weft_renderer_release_export_image(gpu.renderer, first.image, 1, sampling), WEFT_RENDER_OK,
               "releasing the first");
        expect(gpu.draw(prepared, panel, WEFT_RENDER_TARGET_LINEAR_RGBA16F, none), WEFT_RENDER_BUSY,
               "the first's sampling pending");
        id<MTLCommandBuffer> host = [gpu.queue commandBuffer];
        [host commit];
        [host waitUntilCompleted];
        gpu.serial = sampling;
        expect(weft_renderer_notify_completed(gpu.renderer, 1, 1, sampling), WEFT_RENDER_OK, "the host's frame");
        weft_render_export_image third{};
        expect(gpu.draw(prepared, panel, WEFT_RENDER_TARGET_LINEAR_RGBA16F, third), WEFT_RENDER_OK, "the first again");
        require(third.version == 3 && texture_of(third) == texture, "the first image, drawn again");
        expect(weft_renderer_release_export_image(gpu.renderer, second.image, 1, 0), WEFT_RENDER_OK,
               "releasing the second");
        expect(weft_renderer_release_export_image(gpu.renderer, third.image, 1, 0), WEFT_RENDER_OK,
               "releasing the third");
        expect(weft_renderer_destroy_export(gpu.renderer, panel), WEFT_RENDER_OK, "the 16F export goes");
    }
} // namespace

int main() {
    @autoreleasepool {
        try {
            weft_runtime* runtime{};
            require(weft_runtime_create(nullptr, &runtime) == WEFT_OK, "a runtime");
            const weft_view_desc view_desc{sizeof(view_desc), 0, 64, 32, 1, 0};
            weft_view_handle view{};
            require(weft_view_create(runtime, &view_desc, &view) == WEFT_OK, "a View");
            const weft_script scripts[]{{"page.js", 7, script, std::strlen(script)}};
            const weft_page_desc page{sizeof(page), 0,       scripts, 1, 0, stylesheet, std::strlen(stylesheet),
                                      nullptr,      0,       nullptr, 0, nullptr, 0};
            require(weft_view_load(runtime, view, &page) == WEFT_OK, "the page");
            weft_view_frame* frame{};
            require(weft_view_acquire_frame(runtime, view, &frame) == WEFT_OK && frame, "a frame");
            {
                Gpu gpu;
                const weft_render_scene_frame scene{sizeof(scene), WEFT_RENDER_ABI_VERSION, frame, 0, 0, nullptr};
                weft_render_handle prepared{};
                expect(weft_renderer_prepare_scene(gpu.renderer, &scene, &prepared), WEFT_RENDER_OK, "the frame");
                LinearPremultipliedLevels(gpu, prepared);
                expect(weft_renderer_release_prepared(gpu.renderer, prepared), WEFT_RENDER_OK, "the prepared frame");
                expect(weft_renderer_notify_completed(gpu.renderer, 1, 1, gpu.serial + 1), WEFT_RENDER_OK,
                       "everything completed");
                weft_render_export_info info{};
                info.struct_size = sizeof(info);
                expect(weft_renderer_get_export_info(gpu.renderer, &info), WEFT_RENDER_OK, "export info");
                require(!info.images && !info.leases && !info.image_bytes, "every image destroyed");
                expect(weft_renderer_destroy(gpu.renderer), WEFT_RENDER_OK, "the renderer goes");
                weft_render_metal_destroy(gpu.backend);
            }
            weft_view_frame_release(frame);
            weft_view_destroy(runtime, view);
            weft_runtime_destroy(runtime);
            std::cout << "PASS render export metal: linear premultiplied levels of RGBA16F and sRGB exports, the ring\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "FAIL: " << e.what() << '\n';
            return 1;
        }
    }
}
