#ifndef WEFT_RENDER_BACKEND_D3D11_H
#define WEFT_RENDER_BACKEND_D3D11_H
#include "weft/render_backend.h"
#include "weft/render_external_texture.h"

#ifdef __cplusplus
#define WEFT_RENDER_D3D11_NOEXCEPT noexcept
#else
#define WEFT_RENDER_D3D11_NOEXCEPT
#endif

/* Native payload schema v1. Only the D3D11 adapter casts these pointers.
 * Fresh exclusive deferred ID3D11DeviceContext and single-sample RTV, both on
 * the backend device. The adapter closes its command list; host owns the
 * returned COM reference, ExecuteCommandList and its completion query.
 * Preserve existing target contents by default. It does not submit, present
 * or wait. Other backend payloads can use different recording models. */
typedef struct weft_render_d3d11_pass {
    uint32_t struct_size;
    uint32_t abi_version;
    void* deferred_context;
    void* render_target_view;
    void** out_command_list; /* set to null on entry; transferred on success */
    /* 0 preserves host contents; 1 clears */
    uint32_t clear_target;
    uint32_t reserved;
    float clear_linear_premultiplied[4];
} weft_render_d3d11_pass;

/* Borrow a valid ID3D11ShaderResourceView during import. Adapter AddRefs it.
 * One full Texture2D mip, single sample, no arrays/shared synchronization/CPU
 * writes, on its own device. SRV is UNORM/FLOAT linear premultiplied RGBA;
 * sRGB views and encoded-premultiplied producer pixels require conversion.
 * Producer queues its writes on the host immediate context before consumption;
 * contents stay frozen until the lease retirement callback, unless imported live
 * (WEFT_RENDER_EXTERNAL_SAME_QUEUE_LIVE): then the producer writes them again on
 * that context between the frames that sample them. */
typedef struct weft_render_d3d11_texture {
    uint32_t struct_size;
    uint32_t abi_version;
    void* shader_resource_view;
    uint64_t reserved;
} weft_render_d3d11_texture;

#ifdef __cplusplus
extern "C" {
#endif

/* The built-in D3D11 backend of View frames (WEFT_RENDER_FEATURE_SCENE) on the host's device. A pass draws the frame
 * on the device's immediate context during weft_renderer_encode: its weft_render_d3d11_pass has no deferred context
 * (out_command_list stays NULL), and the renderer's owner thread encodes where the host's immediate-context work for
 * the frame belongs. The target is a UNORM RGBA8 or BGRA8 Texture2D view of the pass's size (target_format
 * WEFT_RENDER_TARGET_LINEAR_RGBA8 or _BGRA8); the pass draws over it, clearing it first when asked. The context's
 * bound state is undefined afterwards. The context orders the pass's writes after the reads before it, so the backend
 * holds nothing for completions, though the host reports its submissions and completions as for any backend. The
 * renderer's owner thread runs COM (the backend initializes it when the thread has not). One backend draws the Views
 * of one runtime. */
typedef struct weft_render_d3d11 weft_render_d3d11;

typedef struct weft_render_d3d11_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    void* device;   /* ID3D11Device, referenced while the backend lives */
    uint64_t queue; /* nonzero: the host's name of the immediate context's queue in its submissions */
    uint64_t reserved;
} weft_render_d3d11_desc;

/* The backend and its table for weft_renderer_create; the table borrows the backend. */
weft_render_result weft_render_d3d11_create(const weft_render_d3d11_desc* desc, weft_render_d3d11** out_backend,
                                            weft_render_backend* out_table) WEFT_RENDER_D3D11_NOEXCEPT;
/* The backend's external texture extension (render_external_texture.h) for weft_renderer_install_external_backend,
 * before the renderer's first frame: host textures a View's page shows by name (weft_render_scene_frame::textures),
 * each a weft_render_d3d11_texture of the backend's device (32 at once, 256 MiB), one import per resource at a time,
 * frozen or live; linear RGBA8 and RGBA16F, and RGBA32F where the device filters it. A pass does not draw into a texture
 * its frame binds (WEFT_RENDER_INVALID_STATE). The table borrows the backend; its external quad frames are
 * WEFT_RENDER_UNSUPPORTED_FEATURE. */
weft_render_result weft_render_d3d11_external(weft_render_d3d11* backend,
                                              weft_render_external_backend* out_table) WEFT_RENDER_D3D11_NOEXCEPT;
/* After weft_renderer_destroy succeeded. */
void weft_render_d3d11_destroy(weft_render_d3d11* backend) WEFT_RENDER_D3D11_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_RENDER_D3D11_NOEXCEPT
#endif
