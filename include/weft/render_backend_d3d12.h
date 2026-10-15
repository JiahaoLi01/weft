#ifndef WEFT_RENDER_BACKEND_D3D12_H
#define WEFT_RENDER_BACKEND_D3D12_H
#include "weft/render_backend.h"
#include "weft/render_external_texture.h"

/* Native payload schema v1. Only the D3D12 adapter casts these pointers, so the core ABI needs no D3D12 header.
 *
 * The host passes a graphics command list of the backend's device, open (recording), with no render pass begun. The
 * adapter records the frame's uploads, barriers of its own resources, copies, clears and draws into it; it does not
 * close, execute, signal or wait. A failed encode records nothing.
 * Encode returns WEFT_RENDER_BUSY while another recording that is not submitted yet uploads or draws something this
 * frame samples: submit or discard that recording, then encode again.
 * The command list's descriptor heaps, root signature, pipeline state, render targets, viewports, scissor rectangles
 * and primitive topology are undefined after the commands; the host sets its own again (a host that tracks that state
 * treats it as lost). The host's resources keep their states.
 *
 * The target is a single-sample 2D texture of one mip level and array slice, at least the pass's width x height
 * (the pass draws at its top-left), of DXGI_FORMAT_B8G8R8A8_UNORM (target_format WEFT_RENDER_TARGET_LINEAR_BGRA8) or
 * DXGI_FORMAT_R8G8B8A8_UNORM (WEFT_RENDER_TARGET_LINEAR_RGBA8), or of the typeless format of either; the adapter makes
 * its own render target view of it with that format. It is in D3D12_RESOURCE_STATE_RENDER_TARGET when the commands
 * execute, the host's earlier writes done, and the commands leave it there. A View frame's backdrop filters at the
 * target's level read what the target holds beneath them (the adapter copies from it, returning it to the render
 * target state). Preserve existing target contents by default. */
typedef struct weft_render_d3d12_pass {
    uint32_t struct_size;
    uint32_t abi_version;
    void* command_list; /* ID3D12GraphicsCommandList, recording */
    void* target;       /* ID3D12Resource */
    /* 0 preserves host contents; 1 clears */
    uint32_t clear_target;
    uint32_t reserved;
    float clear_linear_premultiplied[4];
} weft_render_d3d12_pass;

/* A host texture of the external texture extension (render_external_texture.h), borrowed during import; the adapter
 * holds a reference until the lease retirement callback. A single-sample 2D texture of the backend's device with one
 * array slice and at least the description's mip levels, of the description's width x height, that a shader may read
 * (no D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE), its resource format the view format of the description's format or
 * the typeless format of it (WEFT_RENDER_TARGET_LINEAR_RGBA8: DXGI_FORMAT_R8G8B8A8_UNORM, SRGB_RGBA8:
 * DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, LINEAR_BGRA8: DXGI_FORMAT_B8G8R8A8_UNORM, SRGB_BGRA8:
 * DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, LINEAR_RGBA16F: DXGI_FORMAT_R16G16B16A16_FLOAT, LINEAR_RGBA32F:
 * DXGI_FORMAT_R32G32B32A32_FLOAT where the device filters it), holding linear premultiplied RGBA (after an sRGB
 * view's decode). The adapter makes its own shader resource views of it. The host's writes are done and the texture
 * is in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE (or a state that includes it) when the commands of every frame
 * that samples it execute; those commands only read it. One import per resource at a time. */
typedef struct weft_render_d3d12_texture {
    uint32_t struct_size;
    uint32_t abi_version; /* WEFT_RENDER_EXTERNAL_ABI_VERSION */
    void* resource;       /* ID3D12Resource */
    uint64_t reserved;
} weft_render_d3d12_texture;

#ifdef __cplusplus
#define WEFT_RENDER_D3D12_NOEXCEPT noexcept
extern "C" {
#else
#define WEFT_RENDER_D3D12_NOEXCEPT
#endif

/* The built-in D3D12 backend of View frames (WEFT_RENDER_FEATURE_SCENE) on the host's device: a pass records a View's
 * frame into the host's command list as weft_render_d3d12_pass says, over what the target holds (clearing it first
 * when asked). It allocates its own resources and descriptor heaps on the device and never releases the host's
 * objects beyond the references it took. A recording's resources stay until the host reports its submission
 * complete. The renderer's owner thread runs COM (the backend initializes it when the thread has not). One backend
 * draws the Views of one runtime. */
typedef struct weft_render_d3d12 weft_render_d3d12;

typedef struct weft_render_d3d12_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    void* device;   /* ID3D12Device, referenced while the backend lives */
    uint64_t queue; /* nonzero: the host's name of the command queue that executes the passes, in its submissions */
    uint64_t reserved;
} weft_render_d3d12_desc;

/* The backend and its table for weft_renderer_create; the table borrows the backend. */
weft_render_result weft_render_d3d12_create(const weft_render_d3d12_desc* desc, weft_render_d3d12** out_backend,
                                            weft_render_backend* out_table) WEFT_RENDER_D3D12_NOEXCEPT;
/* The backend's external texture extension (render_external_texture.h) for weft_renderer_install_external_backend,
 * before the renderer's first frame: host textures a View's page shows by name (weft_render_scene_frame::textures),
 * each a weft_render_d3d12_texture of the backend's device (32 at once, 256 MiB), one import per resource at a time,
 * frozen or live; linear and sRGB RGBA8 and BGRA8, RGBA16F, and RGBA32F where the device filters it. A pass does not
 * draw into a texture its frame binds (WEFT_RENDER_INVALID_STATE). The table borrows the backend; its external quad
 * frames are WEFT_RENDER_UNSUPPORTED_FEATURE. */
weft_render_result weft_render_d3d12_external(weft_render_d3d12* backend,
                                              weft_render_external_backend* out_table) WEFT_RENDER_D3D12_NOEXCEPT;
/* After weft_renderer_destroy succeeded. */
void weft_render_d3d12_destroy(weft_render_d3d12* backend) WEFT_RENDER_D3D12_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_RENDER_D3D12_NOEXCEPT
#endif
