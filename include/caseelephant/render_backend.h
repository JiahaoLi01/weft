#ifndef CASEELEPHANT_RENDER_BACKEND_H
#define CASEELEPHANT_RENDER_BACKEND_H
#include <stdint.h>

#ifdef __cplusplus
#define CE_RENDER_NOEXCEPT noexcept
extern "C" {
#else
#define CE_RENDER_NOEXCEPT
#endif

/* Experimental v1 C ABI. No graphics headers, STL, window or VM types.
 * Every renderer operation requires its creating owner thread. Callbacks must
 * not throw, reenter this renderer, submit, present or wait for GPU work. */
#define CE_RENDER_ABI_VERSION 1u
#define CE_RENDER_SOLID_ASSET UINT32_MAX
#define CE_RENDER_API_D3D11 1u
#define CE_RENDER_API_VULKAN 2u
#define CE_RENDER_API_METAL 3u
/* Additional API IDs >= 1024 can be registered without changing the core. */
#define CE_RENDER_API_CUSTOM_BEGIN 1024u

typedef struct ce_renderer ce_renderer;

typedef enum ce_render_result {
    CE_RENDER_OK = 0,
    CE_RENDER_INVALID_ARGUMENT,
    CE_RENDER_UNSUPPORTED_ABI,
    CE_RENDER_UNSUPPORTED_FEATURE,
    CE_RENDER_WRONG_THREAD,
    CE_RENDER_STALE_HANDLE,
    CE_RENDER_WRONG_BACKEND,
    CE_RENDER_WRONG_EPOCH,
    CE_RENDER_BUSY,
    CE_RENDER_INVALID_STATE,
    CE_RENDER_INVALID_SERIAL,
    CE_RENDER_BACKEND_ERROR,
    CE_RENDER_OUT_OF_MEMORY,
    CE_RENDER_SHUTTING_DOWN,
    CE_RENDER_DEVICE_UNAVAILABLE,
    CE_RENDER_HOST_CONFIRMATION_REQUIRED
} ce_render_result;

enum {
    CE_RENDER_FEATURE_SOLID = 1u,
    CE_RENDER_FEATURE_IMAGE = 2u,
    CE_RENDER_FEATURE_COVERAGE = 4u,
    CE_RENDER_FEATURE_SDF = 8u
};

typedef enum ce_render_encoding {
    CE_RENDER_SRGB_STRAIGHT = 0,
    CE_RENDER_COVERAGE = 1,
    CE_RENDER_SIGNED_DISTANCE = 2
} ce_render_encoding;

typedef enum ce_render_target_format {
    CE_RENDER_TARGET_SRGB_RGBA8 = 0,
    CE_RENDER_TARGET_LINEAR_RGBA8 = 1,
    CE_RENDER_TARGET_LINEAR_RGBA32F = 2,
    CE_RENDER_TARGET_SRGB_BGRA8 = 3,
    CE_RENDER_TARGET_LINEAR_BGRA8 = 4,
    CE_RENDER_TARGET_LINEAR_RGBA16F = 5
} ce_render_target_format;

typedef struct ce_render_asset {
    uint64_t asset_id, asset_version; /* immutable content identity, nonzero */
    uint32_t width;
    uint32_t height;
    uint32_t encoding;
    uint32_t reserved;
    const uint8_t* bytes;
    uint64_t byte_count;
    float sdf_spread;
    uint32_t reserved2;
} ce_render_asset;

typedef struct ce_render_draw {
    float x;
    float y;
    float width;
    float height;
    float srgb[4];        /* straight color; internal blending is linear premultiplied */
    uint32_t asset_index; /* SOLID_ASSET or index in this frame's complete assets */
    float opacity;
    float uv[4]; /* normalized origin + extent */
    float sdf_stroke;
    float outline_srgb[4];
} ce_render_draw;

typedef struct ce_render_frame {
    uint32_t struct_size;
    uint32_t abi_version;
    const ce_render_draw* draws;
    uint32_t draw_count;
    uint32_t asset_count;
    const ce_render_asset* assets;
} ce_render_frame;

typedef struct ce_render_capabilities {
    uint32_t struct_size;
    uint32_t features;
    uint32_t target_formats; /* bit (1u << ce_render_target_format) */
    uint32_t max_texture_size;
    uint32_t max_target_size;
    uint32_t reserved;
} ce_render_capabilities;

typedef struct ce_render_identity {
    uint64_t renderer_id;
    uint64_t device_epoch;
    uint64_t queue;
    uint32_t api;
    uint32_t reserved;
} ce_render_identity;

typedef struct ce_render_handle {
    uint64_t renderer_id;
    uint64_t device_epoch;
    uint32_t slot;
    uint32_t generation;
    uint32_t kind;
    uint32_t reserved;
} ce_render_handle;

typedef struct ce_render_pass {
    uint32_t struct_size;
    uint32_t abi_version;
    ce_render_identity identity;
    uint32_t width;
    uint32_t height;
    uint32_t target_format;
    uint32_t sample_count;
    const void* native_pass; /* backend-specific, borrowed only during encode */
    uint64_t native_pass_size;
} ce_render_pass;

typedef struct ce_render_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t api;
    uint32_t reserved;
    uint64_t device_epoch;
    uint64_t queue;
    ce_render_capabilities capabilities;
    void* user_data; /* borrowed until successful ce_renderer_destroy */
    /* Frame pointers are call-scoped. Success must own a complete snapshot.
     * Failure must return native_frame=0 and undo preparation ownership. */
    ce_render_result (*prepare)(void*, const ce_render_frame*, uint64_t* native_frame) CE_RENDER_NOEXCEPT;
    void (*release_prepared)(void*, uint64_t native_frame) CE_RENDER_NOEXCEPT;
    /* Acquire resource pins BEFORE recording. An error may return a nonzero
     * native_use if partial commands survive. In that case out_use is VALID:
     * host must cancel those commands then discard, or acknowledge an actual
     * submission. Zero native_use on error promises complete rollback. */
    ce_render_result (*encode)(void*, uint64_t native_frame, const ce_render_pass*,
                               uint64_t* native_use) CE_RENDER_NOEXCEPT;
    void (*discard_encoded)(void*, uint64_t native_use) CE_RENDER_NOEXCEPT;
    /* Valid submission/completion callbacks cannot fail or allocate. Backend
     * must reserve its own tracking capacity before it records GPU commands. */
    void (*submitted)(void*, uint64_t native_use, uint64_t serial) CE_RENDER_NOEXCEPT;
    void (*completed)(void*, uint64_t serial) CE_RENDER_NOEXCEPT;
    /* Only after host guarantees the old device can never access resources. */
    void (*abandon)(void*) CE_RENDER_NOEXCEPT;
    ce_render_result (*shutdown)(void*) CE_RENDER_NOEXCEPT;
} ce_render_backend;

typedef struct ce_renderer_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_prepared_frames;
    uint32_t max_encoded_uses;
    uint32_t max_in_flight_uses;
    uint32_t max_draws;
    uint32_t max_assets;
    uint32_t reserved;
    uint64_t max_frame_asset_bytes;
} ce_renderer_desc;

typedef struct ce_render_info {
    uint32_t struct_size;
    uint32_t draining;
    ce_render_identity identity;
    ce_render_capabilities capabilities;
    uint32_t prepared_frames;
    uint32_t encoded_uses;
    uint32_t submitted_uses;
    uint32_t device_available;
    uint64_t last_submitted_serial;
    uint64_t completed_serial;
} ce_render_info;

ce_render_result ce_renderer_create(const ce_render_backend*, const ce_renderer_desc*,
                                    ce_renderer**) CE_RENDER_NOEXCEPT;
/* Starts drain; Busy keeps the renderer, handles and callback user_data alive. */
ce_render_result ce_renderer_destroy(ce_renderer*) CE_RENDER_NOEXCEPT;
ce_render_result ce_renderer_get_info(ce_renderer*, ce_render_info*) CE_RENDER_NOEXCEPT;
ce_render_result ce_renderer_prepare(ce_renderer*, const ce_render_frame*, ce_render_handle*) CE_RENDER_NOEXCEPT;
ce_render_result ce_renderer_release_prepared(ce_renderer*, ce_render_handle) CE_RENDER_NOEXCEPT;
ce_render_result ce_renderer_encode(ce_renderer*, ce_render_handle, const ce_render_pass*,
                                    ce_render_handle*) CE_RENDER_NOEXCEPT;
/* Host must first destroy/cancel ALL native commands using this token. */
ce_render_result ce_renderer_discard_encoded(ce_renderer*, ce_render_handle) CE_RENDER_NOEXCEPT;
/* Call only after actual GPU submission. Incorrect notifications retain pins. */
ce_render_result ce_renderer_notify_submitted(ce_renderer*, ce_render_handle, uint64_t queue,
                                              uint64_t serial) CE_RENDER_NOEXCEPT;
/* Host supplies truthful completion, including non-UI work on the same queue. */
ce_render_result ce_renderer_notify_completed(ce_renderer*, uint64_t device_epoch, uint64_t queue,
                                              uint64_t serial) CE_RENDER_NOEXCEPT;
/* Cancel all unsubmitted native commands first. Surface loss/pause alone does
 * not authorize abandoning GPU resources. Release prepared frames normally;
 * create a new backend/renderer for a new device epoch. */
#define CE_RENDER_HOST_CONFIRMED_NO_FURTHER_ACCESS 1u
ce_render_result ce_renderer_abandon_device(ce_renderer*, uint64_t device_epoch,
                                            uint32_t confirmation) CE_RENDER_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef CE_RENDER_NOEXCEPT
#endif
