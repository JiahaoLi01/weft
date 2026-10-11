#pragma once

#include "render/backends/vulkan/vulkan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// One Vulkan device of the system's loader: an instance, with the validation layer when it is installed, one physical
// device and one graphics queue. Owner thread of the renderers that use it.
namespace weft::render::vulkan {
    struct DeviceOptions {
        // The physical device whose name contains this; otherwise the first discrete GPU, else the first device.
        std::string adapter;
        // Enables VK_LAYER_KHRONOS_validation, which reports through validation_messages, when it is installed.
        bool validation = true;
        // With the validation layer, also checks hazards between the device's accesses (synchronization validation).
        bool synchronization = true;
    };

    // A device-local 2D image of one mip level and layer, with its memory and a view of it. A framebuffer only for the
    // renderer's own targets.
    struct Image {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        VkFormat format{};
        std::uint32_t width{};
        std::uint32_t height{};
    };

    // A buffer with its memory; a mapped one is host visible and coherent and stays mapped.
    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        VkDeviceSize size{};
    };

    class Device {
    public:
        // The device, or nullptr with the reason when this system has no Vulkan loader or no device with a graphics
        // queue. Failures after that throw std::runtime_error.
        static std::unique_ptr<Device> open(const DeviceOptions& options, std::string& unavailable);
        ~Device();
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        // A memory type among bits that has flags; throws when there is none.
        std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
        // Waits (bounded) for fence.
        void wait(VkFence fence) const;
        // A new image or buffer; a failure throws and leaves nothing behind.
        Image image(VkFormat format, std::uint32_t width, std::uint32_t height, VkImageUsageFlags usage) const;
        Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped) const;
        // Destroys what exists of image or buffer and empties it. The device must no longer use it.
        void destroy(Image& image) const noexcept;
        void destroy(Buffer& buffer) const noexcept;
        // Whether images of format can be color attachments that blend.
        bool blends(VkFormat format) const;

        InstanceFunctions vki;
        DeviceFunctions vkd;
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkQueue queue{};
        std::uint32_t queue_family{};
        VkPhysicalDeviceMemoryProperties memory{};
        std::string name;
        std::uint32_t api_version{};
        std::uint32_t driver_version{};
        bool discrete{};
        // The validation layer is enabled, and with it synchronization validation.
        bool validation{};
        bool synchronization{};
        // Validation warnings and errors so far, and the first of them.
        std::size_t validation_messages{};
        std::string first_validation_message;

    private:
        Device() = default;

        static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

        void* library_{};
        VkDebugUtilsMessengerEXT messenger_{};
    };
} // namespace weft::render::vulkan
