#ifndef WEFT_RENDER_BACKEND_METAL_H
#define WEFT_RENDER_BACKEND_METAL_H
#include "weft/render_backend.h"
#include "weft/render_export_texture.h"
#include "weft/render_external_texture.h"

/* Native payload schema v1 of the Metal backend (WEFT_RENDER_API_METAL). Only the Metal adapter casts these pointers,
 * so the core ABI needs no Metal header: an Objective-C object travels as a pointer bridged without a transfer of
 * ownership ((__bridge void*)object under ARC).
 *
 * Implemented on Apple platforms (weft::scene_metal, src/render/backends/metal): the shaders' MSL and their bindings
 * are fixed and checked on every build that translates them, and on macOS the native pass draws View frames
 * (docs/render-backend-api.md, "Metal"; docs/status.md). It draws into LINEAR_BGRA8 targets only, as the Vulkan
 * backend does.
 *
 * The host passes a command buffer of the backend's device, not committed, with no command encoder open. The adapter
 * encodes the frame's uploads, copies, clears and draws into it in render and blit command encoders of its own, each
 * ended before encode returns; it does not commit, wait or add handlers. A failed encode encodes nothing. Encode
 * returns WEFT_RENDER_BUSY while another recording that is not submitted yet uploads or draws something this frame
 * samples: submit or discard that recording, then encode again. Encoders the host makes after it start from Metal's
 * default state, as every encoder does.
 *
 * The target is a 2D texture of one mip level and slice, single-sample, at least the pass's width x height (the pass
 * draws at its top-left), with MTLTextureUsageRenderTarget, of MTLPixelFormatBGRA8Unorm (target_format
 * WEFT_RENDER_TARGET_LINEAR_BGRA8), MTLPixelFormatRGBA8Unorm (WEFT_RENDER_TARGET_LINEAR_RGBA8) or
 * MTLPixelFormatRGB10A2Unorm (WEFT_RENDER_TARGET_LINEAR_RGB10A2). The host's earlier writes to it come first: Metal
 * orders them for a tracked texture on the same queue, and the host's fence or event does otherwise. A View frame's
 * backdrop filters at the target's level read what the target holds beneath them: the adapter copies from it, so such
 * a target is not framebufferOnly (a CAMetalLayer's drawable needs framebufferOnly = NO). Preserve existing target
 * contents by default. The adapter keeps no reference to the target: the host keeps it alive until the command buffer
 * completes. */
typedef struct weft_render_metal_pass {
    uint32_t struct_size;
    uint32_t abi_version;
    void* command_buffer; /* id<MTLCommandBuffer>, not committed, no encoder open */
    void* target;         /* id<MTLTexture> */
    /* 0 preserves host contents; 1 clears */
    uint32_t clear_target;
    uint32_t reserved;
    float clear_linear_premultiplied[4];
} weft_render_metal_pass;

/* A host texture of the external texture extension (render_external_texture.h), borrowed during import; the adapter
 * retains it until the lease retirement callback. A 2D texture of the backend's device, single-sample, with one slice
 * and at least the description's mip levels, of the description's width x height, with MTLTextureUsageShaderRead, its
 * pixel format the description's (WEFT_RENDER_TARGET_LINEAR_RGBA8: MTLPixelFormatRGBA8Unorm, SRGB_RGBA8:
 * MTLPixelFormatRGBA8Unorm_sRGB, LINEAR_BGRA8: MTLPixelFormatBGRA8Unorm, SRGB_BGRA8: MTLPixelFormatBGRA8Unorm_sRGB,
 * LINEAR_RGBA16F: MTLPixelFormatRGBA16Float, LINEAR_RGBA32F: MTLPixelFormatRGBA32Float where the device filters it),
 * holding linear premultiplied RGBA (after an sRGB format's decode). The host's writes come before the command buffers
 * of every frame that samples it, as for the target; those commands only read it. One import per texture at a time. */
typedef struct weft_render_metal_texture {
    uint32_t struct_size;
    uint32_t abi_version; /* WEFT_RENDER_EXTERNAL_ABI_VERSION */
    void* texture;        /* id<MTLTexture> */
    uint64_t reserved;
} weft_render_metal_texture;

/* The export texture extension (render_export_texture.h; docs/ingame-ui.md). weft_renderer_encode_export takes a
 * weft_render_metal_pass whose command_buffer is the host's as for a pass, with target NULL (the image is the target)
 * and clear_target 0: the adapter draws the frame over transparent into a scratch target of its own, then, in render
 * and blit command encoders of its own, writes the image's top level linear premultiplied and fills its lower levels
 * (blit generateMipmapsForTexture). An image's native_texture is its id<MTLTexture> (a pointer bridged without a
 * transfer of ownership): a private 2D texture of the backend's device with MTLTextureUsageShaderRead and
 * MTLTextureUsageRenderTarget, the export's width x height and levels, of MTLPixelFormatRGBA8Unorm_sRGB (format
 * WEFT_RENDER_TARGET_SRGB_RGBA8), MTLPixelFormatBGRA8Unorm_sRGB (SRGB_BGRA8) or MTLPixelFormatRGBA16Float
 * (LINEAR_RGBA16F). The host only samples it, in encoders after encode_export's in the same command buffer or in
 * command buffers committed after it to the same queue (Metal orders a tracked texture's writes and reads there), and
 * keeps no reference past the lease. */

/* The built-in Metal backend of View frames (WEFT_RENDER_FEATURE_SCENE) on the host's device: a pass encodes a View's
 * frame into the host's command buffer as weft_render_metal_pass says, over what the target holds (clearing it first
 * when asked). It makes its own buffers, textures and pipelines on the device (the shaders' MSL compiled there) and
 * releases none of the host's objects beyond what it retained. A recording's resources stay until the host reports its
 * submission complete. One backend draws the Views of one runtime. */
typedef struct weft_render_metal weft_render_metal;

typedef struct weft_render_metal_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    void* device;   /* id<MTLDevice>, retained while the backend lives */
    uint64_t queue; /* nonzero: the host's name of the command queue whose command buffers carry the passes */
    uint64_t reserved;
} weft_render_metal_desc;

#if defined(__APPLE__)
#ifdef __cplusplus
#define WEFT_RENDER_METAL_NOEXCEPT noexcept
extern "C" {
#else
#define WEFT_RENDER_METAL_NOEXCEPT
#endif

/* Apple platforms only (weft::scene_metal). The backend and its table for weft_renderer_create; the table borrows the
 * backend. */
weft_render_result weft_render_metal_create(const weft_render_metal_desc* desc, weft_render_metal** out_backend,
                                            weft_render_backend* out_table) WEFT_RENDER_METAL_NOEXCEPT;
/* The backend's external texture extension (render_external_texture.h) for weft_renderer_install_external_backend,
 * before the renderer's first frame: host textures a View's page shows by name (weft_render_scene_frame::textures),
 * each a weft_render_metal_texture of the backend's device. The table borrows the backend; its external quad frames
 * are WEFT_RENDER_UNSUPPORTED_FEATURE. */
weft_render_result weft_render_metal_external(weft_render_metal* backend,
                                              weft_render_external_backend* out_table) WEFT_RENDER_METAL_NOEXCEPT;
/* The backend's export texture extension (render_export_texture.h) for weft_renderer_install_export_backend, before the
 * renderer's first frame: images of its own (at most 32 at once, 256 MiB with every level) that a View's frames are
 * drawn into for the host to sample, as above. The table borrows the backend. */
weft_render_result weft_render_metal_export(weft_render_metal* backend,
                                            weft_render_export_backend* out_table) WEFT_RENDER_METAL_NOEXCEPT;
/* After weft_renderer_destroy succeeded. */
void weft_render_metal_destroy(weft_render_metal* backend) WEFT_RENDER_METAL_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_RENDER_METAL_NOEXCEPT
#endif
#endif
