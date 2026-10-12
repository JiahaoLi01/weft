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
} weft_render_vulkan_texture;
#endif
