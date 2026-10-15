#ifndef WEFT_RENDER_BACKEND_VULKAN_H
#define WEFT_RENDER_BACKEND_VULKAN_H
#include "weft/render_backend.h"
#include "weft/render_external_texture.h"

/* Native payload schema v1. Only the Vulkan adapter casts these handles, so
 * the core ABI needs no Vulkan header. A handle travels as uint64_t: a
 * dispatchable one (VkDevice, VkCommandBuffer) and, on 64-bit platforms, a
 * non-dispatchable one (VkImageView) as (uint64_t)(uintptr_t)handle; on
 * 32-bit platforms non-dispatchable handles are already uint64_t.
 *
 * The host begins a primary command buffer on the backend device's graphics
 * queue family and passes it recording, outside a render pass. The adapter
 * records the frame's uploads, render passes and isolated groups into it; it
 * does not end, submit, present or wait. A failed encode records nothing.
 * Encode returns WEFT_RENDER_BUSY while another recording that is not
 * submitted yet first writes a glyph atlas page that this frame samples
 * (that first write discards the page): submit or discard that recording,
 * then encode again.
 * Bound pipeline, descriptor sets, viewport and scissor are undefined after
 * the commands; the host's other state is untouched.
 *
 * The target is a view (2D, one mip level and layer) of a single-sample color
 * image of the pass width x height and target_format, with color attachment
 * usage. It is in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL when the commands
 * execute, the host's earlier writes made available to color attachment
 * access; the commands leave it in that layout, their writes done at
 * VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT. The host's barrier follows
 * before it reads, samples, copies or presents the target. Preserve existing
 * target contents by default. */
typedef struct weft_render_vulkan_pass {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t device;         /* VkDevice of the backend */
    uint64_t command_buffer; /* VkCommandBuffer, recording */
    uint64_t target_view;    /* VkImageView */
    /* 0 preserves host contents; 1 clears */
    uint32_t clear_target;
    uint32_t reserved;
    float clear_linear_premultiplied[4];
    /* Optional (0, or a struct_size that ends before it): the target view's VkImage, with transfer source usage, which
     * a View frame's backdrop filters at the target's level read; without it they see nothing beneath them. */
    uint64_t target_image;
} weft_render_vulkan_pass;

/* A host texture of the external texture extension (render_external_texture.h), borrowed during import; the host
 * keeps the view and its image alive and unchanged until the lease retirement callback. Vulkan cannot report a view's
 * properties, so the host vouches for them: a 2D view of one mip level and layer of a single-sample image of the
 * description's width x height with sampled usage, in the format of the description's linear format
 * (WEFT_RENDER_TARGET_LINEAR_RGBA8: VK_FORMAT_R8G8B8A8_UNORM, LINEAR_RGBA16F: VK_FORMAT_R16G16B16A16_SFLOAT,
 * LINEAR_RGBA32F: VK_FORMAT_R32G32B32A32_SFLOAT where the device filters it linearly), holding linear premultiplied
 * RGBA. One import per view at a time. The commands that write the image are submitted on the backend device's
 * queue before every frame that samples it and leave the image in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, their
 * writes made visible to VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT reads (VK_ACCESS_SHADER_READ_BIT); the frame's
 * commands only read it. */
typedef struct weft_render_vulkan_texture {
    uint32_t struct_size;
    uint32_t abi_version; /* WEFT_RENDER_EXTERNAL_ABI_VERSION */
    uint64_t device;      /* VkDevice of the backend */
    uint64_t image_view;  /* VkImageView */
    /* The VkImage of image_view, or 0 (the first version of the structure ends before it): with it, a pass whose
     * weft_render_vulkan_pass::target_image is a texture its frame binds is refused (WEFT_RENDER_INVALID_STATE), as
     * the D3D11 backend refuses one; without it, the host keeps them apart. */
    uint64_t image;
} weft_render_vulkan_texture;

#ifdef __cplusplus
#define WEFT_RENDER_VULKAN_NOEXCEPT noexcept
extern "C" {
#else
#define WEFT_RENDER_VULKAN_NOEXCEPT
#endif

/* The built-in Vulkan backend of View frames (WEFT_RENDER_FEATURE_SCENE) on the host's device: the instance (Vulkan
 * 1.1 or later), its physical device, the device and a queue of a graphics family, and the vkGetInstanceProcAddr of
 * the host's loader, through which the backend loads what it calls; it allocates its own memory on the device and
 * never destroys the host's objects. A pass records a View's frame into the host's command buffer as
 * weft_render_vulkan_pass says, over what the target holds (clearing it first when asked): the target is a
 * VK_FORMAT_B8G8R8A8_UNORM view (target_format WEFT_RENDER_TARGET_LINEAR_BGRA8) in
 * VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. A recording's resources stay until the host reports its submission
 * complete. One backend draws the Views of one runtime. */
typedef struct weft_render_vulkan weft_render_vulkan;

typedef struct weft_render_vulkan_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t instance;        /* VkInstance */
    uint64_t physical_device; /* VkPhysicalDevice */
    uint64_t device;          /* VkDevice */
    uint64_t queue;           /* VkQueue the host submits the passes on */
    uint32_t queue_family;
    uint32_t reserved;
    void* get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr */
    uint64_t queue_identity;      /* nonzero: the host's name of the queue in its submissions */
} weft_render_vulkan_desc;

/* The backend and its table for weft_renderer_create; the table borrows the backend. */
weft_render_result weft_render_vulkan_create(const weft_render_vulkan_desc* desc, weft_render_vulkan** out_backend,
                                             weft_render_backend* out_table) WEFT_RENDER_VULKAN_NOEXCEPT;
/* The backend's external texture extension (render_external_texture.h) for weft_renderer_install_external_backend,
 * before the renderer's first frame: host textures a View's page shows by name (weft_render_scene_frame::textures),
 * each a weft_render_vulkan_texture of the backend's device (32 at once, 256 MiB), one import per view at a time, frozen
 * or live; linear RGBA8 and RGBA16F, and RGBA32F where the device filters it linearly. The table borrows the backend; its
 * external quad frames are WEFT_RENDER_UNSUPPORTED_FEATURE. */
weft_render_result weft_render_vulkan_external(weft_render_vulkan* backend,
                                               weft_render_external_backend* out_table) WEFT_RENDER_VULKAN_NOEXCEPT;
/* After weft_renderer_destroy succeeded. */
void weft_render_vulkan_destroy(weft_render_vulkan* backend) WEFT_RENDER_VULKAN_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#undef WEFT_RENDER_VULKAN_NOEXCEPT
#endif
