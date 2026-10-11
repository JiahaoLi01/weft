#pragma once

// The Vulkan API without prototypes: the backend opens the system's loader at run time and loads every function it
// calls by name, so it links no import library and runs where the loader comes with the driver.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <string_view>

// The instance, debug and device functions the backend calls. Each list expands F(name) once per function.
#define WEFT_VULKAN_INSTANCE_FUNCTIONS(F)                                                                              \
    F(vkDestroyInstance)                                                                                               \
    F(vkEnumeratePhysicalDevices)                                                                                      \
    F(vkGetPhysicalDeviceProperties)                                                                                   \
    F(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    F(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    F(vkGetPhysicalDeviceFormatProperties)                                                                             \
    F(vkCreateDevice)                                                                                                  \
    F(vkGetDeviceProcAddr)

#define WEFT_VULKAN_DEBUG_FUNCTIONS(F)                                                                                 \
    F(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    F(vkDestroyDebugUtilsMessengerEXT)

#define WEFT_VULKAN_DEVICE_FUNCTIONS(F)                                                                                \
    F(vkDestroyDevice)                                                                                                 \
    F(vkGetDeviceQueue)                                                                                                \
    F(vkDeviceWaitIdle)                                                                                                \
    F(vkQueueSubmit)                                                                                                   \
    F(vkCreateCommandPool)                                                                                             \
    F(vkDestroyCommandPool)                                                                                            \
    F(vkAllocateCommandBuffers)                                                                                        \
    F(vkFreeCommandBuffers)                                                                                            \
    F(vkResetCommandBuffer)                                                                                            \
    F(vkBeginCommandBuffer)                                                                                            \
    F(vkEndCommandBuffer)                                                                                              \
    F(vkCreateFence)                                                                                                   \
    F(vkDestroyFence)                                                                                                  \
    F(vkWaitForFences)                                                                                                 \
    F(vkResetFences)                                                                                                   \
    F(vkCreateBuffer)                                                                                                  \
    F(vkDestroyBuffer)                                                                                                 \
    F(vkGetBufferMemoryRequirements)                                                                                   \
    F(vkBindBufferMemory)                                                                                              \
    F(vkCreateImage)                                                                                                   \
    F(vkDestroyImage)                                                                                                  \
    F(vkGetImageMemoryRequirements)                                                                                    \
    F(vkBindImageMemory)                                                                                               \
    F(vkAllocateMemory)                                                                                                \
    F(vkFreeMemory)                                                                                                    \
    F(vkMapMemory)                                                                                                     \
    F(vkUnmapMemory)                                                                                                   \
    F(vkCreateImageView)                                                                                               \
    F(vkDestroyImageView)                                                                                              \
    F(vkCreateSampler)                                                                                                 \
    F(vkDestroySampler)                                                                                                \
    F(vkCreateShaderModule)                                                                                            \
    F(vkDestroyShaderModule)                                                                                           \
    F(vkCreateDescriptorSetLayout)                                                                                     \
    F(vkDestroyDescriptorSetLayout)                                                                                    \
    F(vkCreatePipelineLayout)                                                                                          \
    F(vkDestroyPipelineLayout)                                                                                         \
    F(vkCreateDescriptorPool)                                                                                          \
    F(vkDestroyDescriptorPool)                                                                                         \
    F(vkAllocateDescriptorSets)                                                                                        \
    F(vkUpdateDescriptorSets)                                                                                          \
    F(vkCreateRenderPass)                                                                                              \
    F(vkDestroyRenderPass)                                                                                             \
    F(vkCreateFramebuffer)                                                                                             \
    F(vkDestroyFramebuffer)                                                                                            \
    F(vkCreateGraphicsPipelines)                                                                                       \
    F(vkDestroyPipeline)                                                                                               \
    F(vkCmdPipelineBarrier)                                                                                            \
    F(vkCmdCopyBufferToImage)                                                                                          \
    F(vkCmdCopyImageToBuffer)                                                                                          \
    F(vkCmdClearColorImage)                                                                                            \
    F(vkCmdBeginRenderPass)                                                                                            \
    F(vkCmdEndRenderPass)                                                                                              \
    F(vkCmdBindPipeline)                                                                                               \
    F(vkCmdBindDescriptorSets)                                                                                         \
    F(vkCmdSetViewport)                                                                                                \
    F(vkCmdSetScissor)                                                                                                 \
    F(vkCmdDraw)

namespace weft::render::vulkan {
    // Throws std::runtime_error("<operation> VkResult=<n>") unless result is VK_SUCCESS.
    void check(VkResult result, const char* operation);
    // Throws std::runtime_error(message) unless condition holds.
    void ensure(bool condition, std::string_view message);

#define WEFT_VULKAN_MEMBER(name) PFN_##name name{};

    struct InstanceFunctions {
        WEFT_VULKAN_INSTANCE_FUNCTIONS(WEFT_VULKAN_MEMBER)
        // Present when the debug utils extension is enabled.
        WEFT_VULKAN_DEBUG_FUNCTIONS(WEFT_VULKAN_MEMBER)
    };

    struct DeviceFunctions {
        WEFT_VULKAN_DEVICE_FUNCTIONS(WEFT_VULKAN_MEMBER)
    };

#undef WEFT_VULKAN_MEMBER
} // namespace weft::render::vulkan
