#pragma once

#include "render/backends/vulkan/memory.hpp"
#include "render/backends/vulkan/vulkan.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// One Vulkan device of the system's loader: an instance, with the validation layer when it is installed, one physical
// device and one graphics queue; or a host's (Device::adopt). Owner thread of the renderers that use it.
namespace weft::render::vulkan {
    struct DeviceOptions {
        // The physical device whose name contains this; otherwise the first discrete GPU, else the first device.
        std::string adapter;
        // Enables VK_LAYER_KHRONOS_validation, which reports through validation_messages, when it is installed.
        bool validation = true;
        // With the validation layer, also checks hazards between the device's accesses (synchronization validation).
        bool synchronization = true;
        // Presents to window surfaces: the platform's surface instance extensions, the swapchain device extension, and
        // a graphics queue that presents (on Windows, to Win32 windows).
        bool presentation = false;
    };

    // A device-local 2D image of one mip level and layer, with its memory and a view of it. A framebuffer only for the
    // renderer's own targets. A small image's memory is the range [memory_offset, memory_offset + memory_size) of a
    // block that small images share; a larger image's (memory_size 0) is its own.
    struct Image {
        VkImage image{};
        VkDeviceMemory memory{};
        VkDeviceSize memory_offset{};
        VkDeviceSize memory_size{};
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

    // A host's Vulkan objects: its instance (Vulkan 1.1 or later), the physical device and the device it made on it, a
    // queue of a graphics family of that device, and the loader's entry the host uses.
    struct HostDevice {
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkQueue queue{};
        std::uint32_t queue_family{};
        PFN_vkGetInstanceProcAddr get_instance_proc{};
    };

    class Device {
    public:
        // The device, or nullptr with the reason when this system has no Vulkan loader or no device with a graphics
        // queue. Failures after that throw std::runtime_error.
        static std::unique_ptr<Device> open(const DeviceOptions& options, std::string& unavailable);
        // The host's device: Weft loads the functions it needs through the host's entry and allocates its own memory
        // on the device; destroying the Device frees only that memory, never the host's device or instance, and does
        // not wait for the device (the host has seen Weft's work complete). Throws std::invalid_argument for a missing
        // object, std::runtime_error for a device older than Vulkan 1.1 or a function it lacks.
        static std::unique_ptr<Device> adopt(const HostDevice& host);
        ~Device();
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        // A memory type among bits that has flags; throws when there is none.
        std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
        // Waits (bounded) for fence.
        void wait(VkFence fence) const;
        // A new image or buffer; a failure throws and leaves nothing behind. Images of at most block_image_max_bytes
        // share blocks of device memory: allocating and freeing device memory costs far more than making an image.
        // An image of levels mip levels (its most detailed first), its view holding them all.
        Image image(VkFormat format, std::uint32_t width, std::uint32_t height, VkImageUsageFlags usage,
                    std::uint32_t levels = 1) const;
        Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped) const;
        // Destroys what exists of image or buffer and empties it. The device must no longer use it.
        void destroy(Image& image) const noexcept;
        void destroy(Buffer& buffer) const noexcept;
        // Whether images of format can be color attachments that blend.
        bool blends(VkFormat format) const;
        // Whether images of format can be sampled with linear filtering.
        bool filters(VkFormat format) const;
        // Blocks of device memory that small images share now, and the device memory allocations made so far (blocks,
        // and the memory of larger images and of buffers).
        std::size_t image_blocks() const;

        std::size_t memory_allocations() const noexcept {
            return memory_allocations_;
        }

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
        // The validation layer is enabled, and with it synchronization validation; the device presents.
        bool validation{};
        bool synchronization{};
        bool presents{};
        // The loader's entry, for the functions of platform extensions.
        PFN_vkGetInstanceProcAddr get_instance_proc{};
        // Validation warnings and errors so far, and the first of them.
        std::size_t validation_messages{};
        std::string first_validation_message;

    private:
        // A block of device-local memory of one memory type that small images share. Only images of optimal tiling
        // share blocks, so no linear resource needs bufferImageGranularity apart from them.
        struct ImageBlock {
            ImageBlock(VkDeviceMemory m, std::uint32_t t, VkDeviceSize size) : memory(m), type(t), ranges(size) {
            }

            VkDeviceMemory memory;
            std::uint32_t type;
            RangeHeap ranges;
        };

        Device() = default;

        // Gives image a range of a block of memory type `type` that fits needs: a new block when none has room.
        void place(Image& image, const VkMemoryRequirements& needs, std::uint32_t type) const;
        // Frees image's range of its block. A block left empty goes unless it is the only empty one of its type.
        void release(const Image& image) const noexcept;

        static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

        void* library_{};
        VkDebugUtilsMessengerEXT messenger_{};
        // The device and instance are the host's.
        bool adopted_{};
        mutable std::mutex blocks_mutex_;
        mutable std::vector<std::unique_ptr<ImageBlock>> blocks_;
        mutable std::atomic<std::size_t> memory_allocations_{};
    };
} // namespace weft::render::vulkan
