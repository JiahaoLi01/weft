#ifndef WEFT_RENDER_BACKEND_VULKAN_H
#define WEFT_RENDER_BACKEND_VULKAN_H
#include "weft/render_backend.h"

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
#endif
