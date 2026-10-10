#ifndef CASEELEPHANT_RENDER_BACKEND_D3D11_H
#define CASEELEPHANT_RENDER_BACKEND_D3D11_H
#include "caseelephant/render_backend.h"
#include "caseelephant/render_external_texture.h"

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
 * contents stay frozen until the lease retirement callback. */
typedef struct weft_render_d3d11_texture {
    uint32_t struct_size;
    uint32_t abi_version;
    void* shader_resource_view;
    uint64_t reserved;
} weft_render_d3d11_texture;
#endif
