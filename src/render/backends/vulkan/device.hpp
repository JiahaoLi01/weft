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
        // The validation layer is enabled.
        bool validation{};
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
