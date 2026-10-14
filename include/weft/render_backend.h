#ifndef WEFT_RENDER_BACKEND_H
#define WEFT_RENDER_BACKEND_H
#include <stdint.h>

#ifdef __cplusplus
#define WEFT_RENDER_NOEXCEPT noexcept
extern "C" {
#else
#define WEFT_RENDER_NOEXCEPT
#endif

/* Experimental v1 C ABI. No graphics headers, STL, window or VM types.
 * Every renderer operation requires its creating owner thread. Callbacks must
 * not throw, reenter this renderer, submit, present or wait for GPU work. */
#define WEFT_RENDER_ABI_VERSION 1u
#define WEFT_RENDER_SOLID_ASSET UINT32_MAX
#define WEFT_RENDER_API_D3D11 1u
#define WEFT_RENDER_API_VULKAN 2u
#define WEFT_RENDER_API_METAL 3u
/* Additional API IDs >= 1024 can be registered without changing the core. */
#define WEFT_RENDER_API_CUSTOM_BEGIN 1024u

typedef struct weft_renderer weft_renderer;
/* Defined by render_external_texture.h. */
struct weft_render_external_asset;
/* A View's published frame (weft.h). */
struct weft_view_frame;

typedef enum weft_render_result {
    WEFT_RENDER_OK = 0,
    WEFT_RENDER_INVALID_ARGUMENT,
    WEFT_RENDER_UNSUPPORTED_ABI,
    WEFT_RENDER_UNSUPPORTED_FEATURE,
    WEFT_RENDER_WRONG_THREAD,
    WEFT_RENDER_STALE_HANDLE,
    WEFT_RENDER_WRONG_BACKEND,
    WEFT_RENDER_WRONG_EPOCH,
    WEFT_RENDER_BUSY,
    WEFT_RENDER_INVALID_STATE,
    WEFT_RENDER_INVALID_SERIAL,
    WEFT_RENDER_BACKEND_ERROR,
    WEFT_RENDER_OUT_OF_MEMORY,
    WEFT_RENDER_SHUTTING_DOWN,
    WEFT_RENDER_DEVICE_UNAVAILABLE,
    WEFT_RENDER_HOST_CONFIRMATION_REQUIRED
} weft_render_result;

enum {
    WEFT_RENDER_FEATURE_SOLID = 1u,
    WEFT_RENDER_FEATURE_IMAGE = 2u,
    WEFT_RENDER_FEATURE_COVERAGE = 4u,
    WEFT_RENDER_FEATURE_SDF = 8u,
    /* weft_renderer_prepare_list through weft_render_backend::prepare_list */
    WEFT_RENDER_FEATURE_DISPLAY_LIST = 16u,
    /* weft_renderer_prepare_scene through weft_render_backend::prepare_scene */
    WEFT_RENDER_FEATURE_SCENE = 32u
};

typedef enum weft_render_encoding {
    WEFT_RENDER_SRGB_STRAIGHT = 0,
    WEFT_RENDER_COVERAGE = 1,
    WEFT_RENDER_SIGNED_DISTANCE = 2
} weft_render_encoding;

typedef enum weft_render_target_format {
    WEFT_RENDER_TARGET_SRGB_RGBA8 = 0,
    WEFT_RENDER_TARGET_LINEAR_RGBA8 = 1,
    WEFT_RENDER_TARGET_LINEAR_RGBA32F = 2,
    WEFT_RENDER_TARGET_SRGB_BGRA8 = 3,
    WEFT_RENDER_TARGET_LINEAR_BGRA8 = 4,
    WEFT_RENDER_TARGET_LINEAR_RGBA16F = 5
} weft_render_target_format;

typedef struct weft_render_asset {
    /* immutable content identity, nonzero */
    uint64_t asset_id;
    uint64_t asset_version;
    uint32_t width;
    uint32_t height;
    uint32_t encoding;
    uint32_t reserved;
    const uint8_t* bytes;
    uint64_t byte_count;
    float sdf_spread;
    uint32_t reserved2;
} weft_render_asset;

typedef struct weft_render_draw {
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
} weft_render_draw;

typedef struct weft_render_frame {
    uint32_t struct_size;
    uint32_t abi_version;
    const weft_render_draw* draws;
    uint32_t draw_count;
    uint32_t asset_count;
    const weft_render_asset* assets;
} weft_render_frame;

/* An immutable display list of the Weft runtime: boxes, images, rounded clips and isolated groups in paint
 * order, with its own asset leases. Opaque to hosts, which pass it through; built-in backends read it. */
typedef struct weft_paint_list weft_paint_list;

typedef struct weft_render_capabilities {
    uint32_t struct_size;
    uint32_t features;
    uint32_t target_formats; /* bit (1u << weft_render_target_format) */
    uint32_t max_texture_size;
    uint32_t max_target_size;
    uint32_t reserved;
} weft_render_capabilities;

typedef struct weft_render_identity {
    uint64_t renderer_id;
    uint64_t device_epoch;
    uint64_t queue;
    uint32_t api;
    uint32_t reserved;
} weft_render_identity;

typedef struct weft_render_handle {
    uint64_t renderer_id;
    uint64_t device_epoch;
    uint32_t slot;
    uint32_t generation;
    uint32_t kind;
    uint32_t reserved;
} weft_render_handle;

typedef struct weft_render_list_frame {
    uint32_t struct_size;
    uint32_t abi_version;
    const weft_paint_list* list; /* call-scoped; a successful prepare keeps its own reference */
    uint32_t external_count;     /* the list's external textures (render_external_texture.h), in order */
    uint32_t reserved;
    const weft_render_handle* external_textures; /* call-scoped caller leases */
} weft_render_list_frame;

/* A name of a View's page bound to a host texture for one frame: an element whose background-image is
 * -weft-texture(name) shows the texture stretched over its border box, inside its rounded corners and under its
 * borders. The texture is an import of the external texture extension (render_external_texture.h); its linear
 * premultiplied colors are composited as the page's own. */
typedef struct weft_render_scene_texture {
    const char* name; /* name_size bytes of letters, digits, '-' and '_', at most 64; not terminated */
    uint32_t name_size;
    uint32_t reserved;
    weft_render_handle texture; /* a caller lease of the renderer's (weft_renderer_import_texture) */
} weft_render_scene_texture;

/* A View's frame to draw: the View's page at scale device px per CSS px (0: the View's device scale) from the
 * target's top-left, its anchored elements where the View's anchors are when the frame is encoded. A pass of it draws
 * over what the target holds (clearing it first only when the pass asks), sRGB-encoded and premultiplied, composited in
 * that encoding as a browser composites a page: the target is a UNORM (not sRGB) view. textures bind the names of the
 * page's -weft-texture(name) backgrounds, each name once (a name left unbound draws nothing); the prepared frame pins
 * their leases until its uses complete, as an external frame does. A struct_size that ends before textures is the
 * first version: no textures. */
typedef struct weft_render_scene_frame {
    uint32_t struct_size;
    uint32_t abi_version;
    const struct weft_view_frame* frame; /* call-scoped; a successful prepare keeps its own reference */
    float scale;
    uint32_t texture_count;
    const weft_render_scene_texture* textures; /* call-scoped */
} weft_render_scene_frame;

typedef struct weft_render_pass {
    uint32_t struct_size;
    uint32_t abi_version;
    weft_render_identity identity;
    uint32_t width;
    uint32_t height;
    uint32_t target_format;
    uint32_t sample_count;
    const void* native_pass; /* backend-specific, borrowed only during encode */
    uint64_t native_pass_size;
} weft_render_pass;

typedef struct weft_render_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t api;
    uint32_t reserved;
    uint64_t device_epoch;
    uint64_t queue;
    weft_render_capabilities capabilities;
    void* user_data; /* borrowed until successful weft_renderer_destroy */
    /* Frame pointers are call-scoped. Success must own a complete snapshot.
     * Failure must return native_frame=0 and undo preparation ownership. */
    weft_render_result (*prepare)(void*, const weft_render_frame*, uint64_t* native_frame) WEFT_RENDER_NOEXCEPT;
    void (*release_prepared)(void*, uint64_t native_frame) WEFT_RENDER_NOEXCEPT;
    /* Acquire resource pins BEFORE recording. An error may return a nonzero
     * native_use if partial commands survive. In that case out_use is VALID:
     * host must cancel those commands then discard, or acknowledge an actual
     * submission. Zero native_use on error promises complete rollback. */
    weft_render_result (*encode)(void*, uint64_t native_frame, const weft_render_pass*,
                                 uint64_t* native_use) WEFT_RENDER_NOEXCEPT;
    void (*discard_encoded)(void*, uint64_t native_use) WEFT_RENDER_NOEXCEPT;
    /* Valid submission/completion callbacks cannot fail or allocate. Backend
     * must reserve its own tracking capacity before it records GPU commands. */
    void (*submitted)(void*, uint64_t native_use, uint64_t serial) WEFT_RENDER_NOEXCEPT;
    void (*completed)(void*, uint64_t serial) WEFT_RENDER_NOEXCEPT;
    /* Only after host guarantees the old device can never access resources. */
    void (*abandon)(void*) WEFT_RENDER_NOEXCEPT;
    weft_render_result (*shutdown)(void*) WEFT_RENDER_NOEXCEPT;
    /* Required with WEFT_RENDER_FEATURE_DISPLAY_LIST, null otherwise; a struct_size that ends before it means
     * null. Prepares a display list frame as prepare does: inputs are call-scoped, success owns a complete
     * snapshot (its own list reference and the external texture metadata). */
    weft_render_result (*prepare_list)(void*, const weft_paint_list*, const struct weft_render_external_asset*,
                                       uint32_t, uint64_t* native_frame) WEFT_RENDER_NOEXCEPT;
    /* Required with WEFT_RENDER_FEATURE_SCENE, null otherwise; a struct_size that ends before it means null. Prepares a
     * View's frame as prepare does: the frame and its textures' metadata (the frame's textures in order) are
     * call-scoped, success keeps its own reference to the frame and the metadata it needs. */
    weft_render_result (*prepare_scene)(void*, const weft_render_scene_frame*, const struct weft_render_external_asset*,
                                        uint32_t, uint64_t* native_frame) WEFT_RENDER_NOEXCEPT;
} weft_render_backend;

typedef struct weft_renderer_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_prepared_frames;
    uint32_t max_encoded_uses;
    uint32_t max_in_flight_uses;
    uint32_t max_draws;
    uint32_t max_assets;
    uint32_t reserved;
    uint64_t max_frame_asset_bytes;
} weft_renderer_desc;

typedef struct weft_render_info {
    uint32_t struct_size;
    uint32_t draining;
    weft_render_identity identity;
    weft_render_capabilities capabilities;
    uint32_t prepared_frames;
    uint32_t encoded_uses;
    uint32_t submitted_uses;
    uint32_t device_available;
    uint64_t last_submitted_serial;
    uint64_t completed_serial;
} weft_render_info;

weft_render_result weft_renderer_create(const weft_render_backend*, const weft_renderer_desc*,
                                        weft_renderer**) WEFT_RENDER_NOEXCEPT;
/* Starts drain; Busy keeps the renderer, handles and callback user_data alive. */
weft_render_result weft_renderer_destroy(weft_renderer*) WEFT_RENDER_NOEXCEPT;
weft_render_result weft_renderer_get_info(weft_renderer*, weft_render_info*) WEFT_RENDER_NOEXCEPT;
weft_render_result weft_renderer_prepare(weft_renderer*, const weft_render_frame*,
                                         weft_render_handle*) WEFT_RENDER_NOEXCEPT;
/* A display list frame; requires WEFT_RENDER_FEATURE_DISPLAY_LIST, and the external texture extension when the
 * list draws external textures. */
weft_render_result weft_renderer_prepare_list(weft_renderer*, const weft_render_list_frame*,
                                              weft_render_handle*) WEFT_RENDER_NOEXCEPT;
/* A View's frame; requires WEFT_RENDER_FEATURE_SCENE, and the external texture extension for textures. One renderer
 * draws a View's frames: as it encodes one it takes the latest of the View's anchors. */
weft_render_result weft_renderer_prepare_scene(weft_renderer*, const weft_render_scene_frame*,
                                               weft_render_handle*) WEFT_RENDER_NOEXCEPT;
weft_render_result weft_renderer_release_prepared(weft_renderer*, weft_render_handle) WEFT_RENDER_NOEXCEPT;
weft_render_result weft_renderer_encode(weft_renderer*, weft_render_handle, const weft_render_pass*,
                                        weft_render_handle*) WEFT_RENDER_NOEXCEPT;
/* Host must first destroy/cancel ALL native commands using this token. */
weft_render_result weft_renderer_discard_encoded(weft_renderer*, weft_render_handle) WEFT_RENDER_NOEXCEPT;
/* Call only after actual GPU submission. Incorrect notifications retain pins. */
weft_render_result weft_renderer_notify_submitted(weft_renderer*, weft_render_handle, uint64_t queue,
                                                  uint64_t serial) WEFT_RENDER_NOEXCEPT;
/* Host supplies truthful completion, including non-UI work on the same queue. */
weft_render_result weft_renderer_notify_completed(weft_renderer*, uint64_t device_epoch, uint64_t queue,
                                                  uint64_t serial) WEFT_RENDER_NOEXCEPT;
/* Cancel all unsubmitted native commands first. Surface loss/pause alone does
 * not authorize abandoning GPU resources. Release prepared frames normally;
 * create a new backend/renderer for a new device epoch. */
#define WEFT_RENDER_HOST_CONFIRMED_NO_FURTHER_ACCESS 1u
weft_render_result weft_renderer_abandon_device(weft_renderer*, uint64_t device_epoch,
                                                uint32_t confirmation) WEFT_RENDER_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_RENDER_NOEXCEPT
#endif
