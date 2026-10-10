#ifndef CASEELEPHANT_RENDER_EXTERNAL_TEXTURE_H
#define CASEELEPHANT_RENDER_EXTERNAL_TEXTURE_H
#include "weft/render_backend.h"
#ifdef __cplusplus
#define WEFT_EXTERNAL_NOEXCEPT noexcept
extern "C" {
#else
#define WEFT_EXTERNAL_NOEXCEPT
#endif

/* Optional v1 extension; existing renderer/frame/backend layouts are unchanged.
 * All calls and callbacks run on the renderer owner, without reentry. */
#define WEFT_RENDER_EXTERNAL_ABI_VERSION 1u
#define WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED 0u
#define WEFT_RENDER_EXTERNAL_TOP_LEFT 0u
#define WEFT_RENDER_EXTERNAL_BOTTOM_LEFT 1u
#define WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED 0u
#define WEFT_RENDER_EXTERNAL_RETIRED 0u
#define WEFT_RENDER_EXTERNAL_ABANDONED 1u
typedef void (*weft_render_texture_retired)(void* user_data, uint32_t reason) WEFT_EXTERNAL_NOEXCEPT;

typedef struct weft_render_external_texture {
    uint32_t struct_size;
    uint32_t abi_version;
    weft_render_identity identity;
    /* immutable nonzero identity */
    uint64_t content_id;
    uint64_t content_version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t encoding;
    uint32_t origin;
    uint32_t synchronization;
    uint32_t reserved;
    uint32_t reserved2;
    const void* native_texture; /* typed backend schema; borrowed during import */
    uint64_t native_texture_size;
    void* retirement_user_data; /* borrowed until retired callback */
    weft_render_texture_retired retired;
} weft_render_external_texture;

typedef struct weft_render_external_asset {
    weft_render_external_texture description; /* native pointer fields cleared */
    uint64_t native_texture;                  /* adapter-owned import token, not a pointer */
} weft_render_external_asset;

typedef struct weft_render_external_frame {
    uint32_t struct_size;
    uint32_t abi_version;
    weft_render_frame frame;
    uint32_t external_count;
    uint32_t reserved;
    /* draw asset indices: CPU assets first, then these complete dependencies. */
    const weft_render_handle* external_textures; /* call-scoped caller leases */
} weft_render_external_frame;

typedef struct weft_render_external_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    /* bits of weft_render_target_format; linear only */
    uint32_t formats;
    uint32_t reserved;
    uint32_t max_textures;
    uint32_t reserved2;
    uint64_t max_texture_bytes; /* bounded logical payload, excludes driver overhead */
    void* user_data;            /* borrowed until renderer destroy */
    /* Success owns a strong native reference; failure returns zero and rolls back.
     * Validate device, format, dimensions and synchronization; reject importing
     * the same mutable native resource twice while any prior import is alive. */
    weft_render_result (*import_texture)(void*, const weft_render_external_texture*, uint64_t*) WEFT_EXTERNAL_NOEXCEPT;
    void (*release_texture)(void*, uint64_t) WEFT_EXTERNAL_NOEXCEPT;
    /* Inputs are call-scoped. Success owns complete draws/CPU data and external
     * metadata. Main backend encode/release/submitted/completed handle the frame. */
    weft_render_result (*prepare)(void*, const weft_render_frame*, const weft_render_external_asset*, uint32_t,
                                  uint64_t*) WEFT_EXTERNAL_NOEXCEPT;
} weft_render_external_backend;

typedef struct weft_render_external_info {
    uint32_t struct_size;
    uint32_t caller_leases;
    uint32_t live_textures;
    uint32_t reserved;
    uint64_t texture_bytes;
} weft_render_external_info;

/* Install once while quiescent. Allocates all dependency/use tracking capacity. */
weft_render_result weft_renderer_install_external_backend(weft_renderer*,
                                                          const weft_render_external_backend*) WEFT_EXTERNAL_NOEXCEPT;
/* Producer writes must be queued BEFORE every consuming submission on the same
 * device/queue. Freeze CPU/GPU contents from successful import until retirement.
 * Releasing the caller lease invalidates its handle immediately, but prepared,
 * encoded and submitted uses survive. Callback fires exactly once after all
 * pins vanish; no callback/ownership on failed import. It must not allocate,
 * throw or reenter. Abandoned reason requires host proof of no device access.
 * Queued producer-only work is the host's responsibility even without a UI use. */
weft_render_result weft_renderer_import_texture(weft_renderer*, const weft_render_external_texture*,
                                                weft_render_handle*) WEFT_EXTERNAL_NOEXCEPT;
weft_render_result weft_renderer_release_texture(weft_renderer*, weft_render_handle) WEFT_EXTERNAL_NOEXCEPT;
weft_render_result weft_renderer_prepare_external(weft_renderer*, const weft_render_external_frame*,
                                                  weft_render_handle*) WEFT_EXTERNAL_NOEXCEPT;
weft_render_result weft_renderer_get_external_info(weft_renderer*, weft_render_external_info*) WEFT_EXTERNAL_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_EXTERNAL_NOEXCEPT
#endif
