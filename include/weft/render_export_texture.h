#ifndef WEFT_RENDER_EXPORT_TEXTURE_H
#define WEFT_RENDER_EXPORT_TEXTURE_H
#include "weft/render_backend.h"
#ifdef __cplusplus
#define WEFT_EXPORT_NOEXCEPT noexcept
extern "C" {
#else
#define WEFT_EXPORT_NOEXCEPT
#endif

/* Optional v1 extension, the reverse of the external texture extension (render_external_texture.h): the renderer draws
 * a View's frame into a texture of its own and lends it to the host, which samples it in its world (an in-game UI
 * panel; docs/ingame-ui.md). Existing renderer/frame/backend layouts are unchanged. All calls and callbacks run on the
 * renderer owner, without reentry.
 *
 * An export is a ring of images of one size, format and mip chain, all made when the export is. Encoding a prepared
 * frame into an export draws it into a free image, converts it to linear premultiplied color and fills its lower
 * levels, and leases the image to the host. The host samples it on the renderer's device and queue after the
 * submission of that use, and releases the lease with the serial of its last submission that samples the image; the
 * image is free again once that serial completed and the use that drew it retired. */
#define WEFT_RENDER_EXPORT_ABI_VERSION 1u
/* Images of one export at most. */
#define WEFT_RENDER_EXPORT_MAX_IMAGES 8u

typedef struct weft_render_export_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    weft_render_identity identity; /* the renderer's */
    uint32_t width;                /* device px */
    uint32_t height;
    /* A weft_render_target_format the backend's extension lists: what sampling the image through a view of that
     * format returns is linear premultiplied color (an sRGB format's after its decode). */
    uint32_t format;
    /* The levels each image holds, its most detailed first: 0 a full chain, 1 one level, else at most a full chain. */
    uint32_t mip_levels;
    /* The ring: 1 to WEFT_RENDER_EXPORT_MAX_IMAGES. One suits a host that samples an image only in the command buffer
     * that draws it and releases it there; hosts with frames in flight use one more than they hold at once. */
    uint32_t images;
    uint32_t reserved;
} weft_render_export_desc;

/* An image of an export as the host samples it. Row 0 is the page's top (a top-left origin). */
typedef struct weft_render_export_image {
    uint32_t struct_size;
    uint32_t abi_version;
    weft_render_handle image; /* the host's lease */
    uint64_t version;         /* the export's successful encodes, 1 for its first */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t mip_levels; /* the levels the image holds, all filled */
    /* The backend's texture object (Metal: an id<MTLTexture> as a pointer bridged without a transfer of ownership),
     * valid while the lease lives; its schema is the backend header's. */
    uint64_t native_texture;
} weft_render_export_image;

typedef struct weft_render_export_backend {
    uint32_t struct_size;
    uint32_t abi_version;
    /* bits of weft_render_target_format the backend draws exports in */
    uint32_t formats;
    uint32_t max_images;      /* images of all exports at once */
    uint64_t max_image_bytes; /* their logical bytes, every level, excluding driver overhead */
    void* user_data;          /* borrowed until renderer destroy */
    /* One image of desc (validated, its mip_levels the count of levels), all its levels. Success owns it and returns
     * its token and texture object; failure returns zeros and rolls back. */
    weft_render_result (*create_image)(void*, const weft_render_export_desc*, uint64_t* native_image,
                                       uint64_t* native_texture) WEFT_EXPORT_NOEXCEPT;
    /* Once no lease, use or pending sampling holds the image (or the device is abandoned). */
    void (*destroy_image)(void*, uint64_t native_image) WEFT_EXPORT_NOEXCEPT;
    /* Records native_frame into native_image as the main backend's encode records a pass: cleared to transparent
     * first, then converted to linear premultiplied and its lower levels filled. The pass's size and format are the
     * image's; its native_pass is the backend's schema. The returned use is the main backend's (discard_encoded,
     * submitted, completed, abandon), under encode's error contract. */
    weft_render_result (*encode)(void*, uint64_t native_frame, uint64_t native_image, const weft_render_pass*,
                                 uint64_t* native_use) WEFT_EXPORT_NOEXCEPT;
} weft_render_export_backend;

typedef struct weft_render_export_info {
    uint32_t struct_size;
    uint32_t formats; /* the backend's */
    uint32_t exports; /* whose handles are live */
    uint32_t images;  /* live, of live and destroyed exports */
    uint32_t leases;  /* images the host leases */
    uint32_t max_images;
    uint64_t image_bytes;
    uint64_t max_image_bytes;
} weft_render_export_info;

/* Install once while quiescent. Allocates all export, image and lease tracking capacity. */
weft_render_result weft_renderer_install_export_backend(weft_renderer*,
                                                        const weft_render_export_backend*) WEFT_EXPORT_NOEXCEPT;
/* Makes every image of the ring. BUSY when the images or their bytes exceed what the backend has left. */
weft_render_result weft_renderer_create_export(weft_renderer*, const weft_render_export_desc*,
                                               weft_render_handle* out_export) WEFT_EXPORT_NOEXCEPT;
/* Invalidates the handle at once. Leased images stay until released; the others go once the uses that draw them
 * retired and the samplings they were released with completed. */
weft_render_result weft_renderer_destroy_export(weft_renderer*, weft_render_handle export_handle) WEFT_EXPORT_NOEXCEPT;
/* Encodes prepared into a free image of the export, under weft_renderer_encode's contract for the use: the pass's
 * width, height and target_format are the export's. On success out_image leases the image, which holds the frame once
 * the use's commands ran: the host's commands that sample it follow them on the renderer's queue. BUSY, encoding
 * nothing, when every image is leased, drawn by a use that has not retired, or sampled by a submission that has not
 * completed. */
weft_render_result weft_renderer_encode_export(weft_renderer*, weft_render_handle prepared,
                                               weft_render_handle export_handle, const weft_render_pass*,
                                               weft_render_handle* out_use,
                                               weft_render_export_image* out_image) WEFT_EXPORT_NOEXCEPT;
/* Ends the lease: the handle is invalid at once. serial: the host's last submission on queue that samples the image
 * (0: none); the image is free again once it completed. It may be a submission the host has not made yet, which it
 * then makes and reports complete. */
weft_render_result weft_renderer_release_export_image(weft_renderer*, weft_render_handle image, uint64_t queue,
                                                      uint64_t serial) WEFT_EXPORT_NOEXCEPT;
weft_render_result weft_renderer_get_export_info(weft_renderer*, weft_render_export_info*) WEFT_EXPORT_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_EXPORT_NOEXCEPT
#endif
