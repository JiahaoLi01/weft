#ifndef CASEELEPHANT_RENDER_EXTERNAL_TEXTURE_H
#define CASEELEPHANT_RENDER_EXTERNAL_TEXTURE_H
#include "caseelephant/render_backend.h"
#ifdef __cplusplus
#define CE_EXTERNAL_NOEXCEPT noexcept
extern "C" {
#else
#define CE_EXTERNAL_NOEXCEPT
#endif

/* Optional v1 extension; existing renderer/frame/backend layouts are unchanged.
 * All calls and callbacks run on the renderer owner, without reentry. */
#define CE_RENDER_EXTERNAL_ABI_VERSION 1u
#define CE_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED 0u
#define CE_RENDER_EXTERNAL_TOP_LEFT 0u
#define CE_RENDER_EXTERNAL_BOTTOM_LEFT 1u
#define CE_RENDER_EXTERNAL_SAME_QUEUE_ORDERED 0u
#define CE_RENDER_EXTERNAL_RETIRED 0u
#define CE_RENDER_EXTERNAL_ABANDONED 1u
typedef void (*ce_render_texture_retired)(void* user_data, uint32_t reason) CE_EXTERNAL_NOEXCEPT;
typedef struct ce_render_external_texture {
    uint32_t struct_size, abi_version;
    ce_render_identity identity;
    uint64_t content_id, content_version; /* immutable nonzero identity */
    uint32_t width, height, format, encoding;
    uint32_t origin, synchronization, reserved, reserved2;
    const void* native_texture; /* typed backend schema; borrowed during import */
    uint64_t native_texture_size;
    void* retirement_user_data; /* borrowed until retired callback */
    ce_render_texture_retired retired;
} ce_render_external_texture;
typedef struct ce_render_external_asset {
    ce_render_external_texture description; /* native pointer fields cleared */
    uint64_t native_texture; /* adapter-owned import token, not a pointer */
} ce_render_external_asset;
typedef struct ce_render_external_frame {
    uint32_t struct_size, abi_version;
    ce_render_frame frame;
    uint32_t external_count, reserved;
    /* draw asset indices: CPU assets first, then these complete dependencies. */
    const ce_render_handle* external_textures; /* call-scoped caller leases */
} ce_render_external_frame;
typedef struct ce_render_external_backend {
    uint32_t struct_size, abi_version;
    uint32_t formats, reserved; /* bits of ce_render_target_format; linear only */
    uint32_t max_textures, reserved2;
    uint64_t max_texture_bytes; /* bounded logical payload, excludes driver overhead */
    void* user_data; /* borrowed until renderer destroy */
    /* Success owns a strong native reference; failure returns zero and rolls back.
     * Validate device, format, dimensions and synchronization; reject importing
     * the same mutable native resource twice while any prior import is alive. */
    ce_render_result (*import_texture)(void*, const ce_render_external_texture*, uint64_t*) CE_EXTERNAL_NOEXCEPT;
    void (*release_texture)(void*, uint64_t) CE_EXTERNAL_NOEXCEPT;
    /* Inputs are call-scoped. Success owns complete draws/CPU data and external
     * metadata. Main backend encode/release/submitted/completed handle the frame. */
    ce_render_result (*prepare)(void*, const ce_render_frame*, const ce_render_external_asset*, uint32_t, uint64_t*) CE_EXTERNAL_NOEXCEPT;
} ce_render_external_backend;
typedef struct ce_render_external_info {
    uint32_t struct_size, caller_leases, live_textures, reserved;
    uint64_t texture_bytes;
} ce_render_external_info;

/* Install once while quiescent. Allocates all dependency/use tracking capacity. */
ce_render_result ce_renderer_install_external_backend(ce_renderer*, const ce_render_external_backend*) CE_EXTERNAL_NOEXCEPT;
/* Producer writes must be queued BEFORE every consuming submission on the same
 * device/queue. Freeze CPU/GPU contents from successful import until retirement.
 * Releasing the caller lease invalidates its handle immediately, but prepared,
 * encoded and submitted uses survive. Callback fires exactly once after all
 * pins vanish; no callback/ownership on failed import. It must not allocate,
 * throw or reenter. Abandoned reason requires host proof of no device access.
 * Queued producer-only work is the host's responsibility even without a UI use. */
ce_render_result ce_renderer_import_texture(ce_renderer*, const ce_render_external_texture*, ce_render_handle*) CE_EXTERNAL_NOEXCEPT;
ce_render_result ce_renderer_release_texture(ce_renderer*, ce_render_handle) CE_EXTERNAL_NOEXCEPT;
ce_render_result ce_renderer_prepare_external(ce_renderer*, const ce_render_external_frame*, ce_render_handle*) CE_EXTERNAL_NOEXCEPT;
ce_render_result ce_renderer_get_external_info(ce_renderer*, ce_render_external_info*) CE_EXTERNAL_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef CE_EXTERNAL_NOEXCEPT
#endif
