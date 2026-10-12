#include "render/backends/vulkan/device.hpp"

#include "constants/render.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace weft::render::vulkan {
    namespace {
        constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
        // The surface extension of the platform's windows.
#ifdef _WIN32
        constexpr const char* platform_surface = "VK_KHR_win32_surface";
#elif defined(__ANDROID__)
        constexpr const char* platform_surface = "VK_KHR_android_surface";
#else
        constexpr const char* platform_surface = nullptr;
#endif
        // vkGetPhysicalDeviceWin32PresentationSupportKHR, without the Win32 header.
        using PresentationSupport = VkBool32(VKAPI_PTR*)(VkPhysicalDevice, std::uint32_t);

        // The loader that comes with the driver.
        void* open_loader() {
#ifdef _WIN32
            return LoadLibraryW(L"vulkan-1.dll");
#else
            if (void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)) {
                return library;
            }
            return dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
        }

        void close_loader(void* library) {
            if (!library) {
                return;
            }
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(library));
#else
            dlclose(library);
#endif
        }

        PFN_vkGetInstanceProcAddr loader_entry(void* library) {
#ifdef _WIN32
            return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), "vkGetInstanceProcAddr")));
#else
            return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
        }

        bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
            for (const auto& e : extensions) {
                if (std::strcmp(e.extensionName, name) == 0) {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    void check(VkResult result, const char* operation) {
        if (result != VK_SUCCESS) {
            std::ostringstream message;
            message << operation << " VkResult=" << int(result);
            throw std::runtime_error(message.str());
        }
    }

    void ensure(bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(std::string(message));
        }
    }

    std::unique_ptr<Device> Device::open(const DeviceOptions& options, std::string& unavailable) {
        std::unique_ptr<Device> d(new Device);
        d->library_ = open_loader();
        if (!d->library_) {
            unavailable = "no Vulkan loader";
            return nullptr;
        }
        const auto get = loader_entry(d->library_);
        if (!get) {
            unavailable = "the Vulkan loader exports no vkGetInstanceProcAddr";
            return nullptr;
        }
        const auto enumerate_layers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            get(nullptr, "vkEnumerateInstanceLayerProperties"));
        const auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            get(nullptr, "vkEnumerateInstanceExtensionProperties"));
        const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
        if (!enumerate_layers || !enumerate_extensions || !create_instance) {
            unavailable = "the Vulkan loader lacks its global functions";
            return nullptr;
        }

        std::vector<const char*> layers;
        std::vector<const char*> extensions;
        bool synchronization{};
        if (options.presentation) {
            std::uint32_t count{};
            check(enumerate_extensions(nullptr, &count, nullptr), "vkEnumerateInstanceExtensionProperties");
            std::vector<VkExtensionProperties> provided(count);
            check(enumerate_extensions(nullptr, &count, provided.data()), "vkEnumerateInstanceExtensionProperties");
            if (!platform_surface || !has_extension(provided, VK_KHR_SURFACE_EXTENSION_NAME) ||
                !has_extension(provided, platform_surface)) {
                unavailable = "no Vulkan surface extensions for this platform's windows";
                return nullptr;
            }
            extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            extensions.push_back(platform_surface);
        }
        if (options.validation) {
            std::uint32_t count{};
            check(enumerate_layers(&count, nullptr), "vkEnumerateInstanceLayerProperties");
            std::vector<VkLayerProperties> available(count);
            check(enumerate_layers(&count, available.data()), "vkEnumerateInstanceLayerProperties");
            for (const auto& layer : available) {
                if (std::strcmp(layer.layerName, validation_layer) == 0) {
                    layers.push_back(validation_layer);
                }
            }
            if (!layers.empty()) {
                check(enumerate_extensions(validation_layer, &count, nullptr),
                      "vkEnumerateInstanceExtensionProperties");
                std::vector<VkExtensionProperties> provided(count);
                check(enumerate_extensions(validation_layer, &count, provided.data()),
                      "vkEnumerateInstanceExtensionProperties");
                ensure(has_extension(provided, VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
                       "the validation layer provides no debug utils messenger");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                synchronization =
                    options.synchronization && has_extension(provided, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                if (synchronization) {
                    extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                }
            }
        }

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "Weft";
        application.pEngineName = "Weft";
        application.apiVersion = VK_API_VERSION_1_1;
        // The messenger also reports while the instance is created and destroyed.
        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messenger.pfnUserCallback = report;
        messenger.pUserData = d.get();
        const VkValidationFeatureEnableEXT enabled[]{VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
        VkValidationFeaturesEXT features{};
        features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        features.enabledValidationFeatureCount = std::uint32_t(std::size(enabled));
        features.pEnabledValidationFeatures = enabled;
        features.pNext = &messenger;
        VkInstanceCreateInfo instance{};
        instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance.pNext = synchronization ? static_cast<const void*>(&features) : layers.empty() ? nullptr : &messenger;
        instance.pApplicationInfo = &application;
        instance.enabledLayerCount = std::uint32_t(layers.size());
        instance.ppEnabledLayerNames = layers.data();
        instance.enabledExtensionCount = std::uint32_t(extensions.size());
        instance.ppEnabledExtensionNames = extensions.data();
        const auto created = create_instance(&instance, nullptr, &d->instance);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER) {
            unavailable = "no Vulkan 1.1 driver";
            return nullptr;
        }
        check(created, "vkCreateInstance");
        d->validation = !layers.empty();
        d->synchronization = synchronization;
        d->presents = options.presentation;
        d->get_instance_proc = get;

#define WEFT_VULKAN_LOAD(name)                                                                                         \
    d->vki.name = reinterpret_cast<PFN_##name>(get(d->instance, #name));                                               \
    ensure(d->vki.name != nullptr, #name);
        WEFT_VULKAN_INSTANCE_FUNCTIONS(WEFT_VULKAN_LOAD)
        if (d->validation) {
            WEFT_VULKAN_DEBUG_FUNCTIONS(WEFT_VULKAN_LOAD)
            check(d->vki.vkCreateDebugUtilsMessengerEXT(d->instance, &messenger, nullptr, &d->messenger_),
                  "vkCreateDebugUtilsMessengerEXT");
        }
        if (d->presents) {
            WEFT_VULKAN_SURFACE_FUNCTIONS(WEFT_VULKAN_LOAD)
        }
#undef WEFT_VULKAN_LOAD
#ifdef _WIN32
        const auto win32_presents = d->presents ? reinterpret_cast<PresentationSupport>(get(
                                                      d->instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR"))
                                                : nullptr;
        ensure(!d->presents || win32_presents, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
#endif
        // A graphics queue family of physical, which presents when the device must; nothing when it has none.
        auto graphics_family = [&](VkPhysicalDevice physical) -> std::optional<std::uint32_t> {
            std::uint32_t families{};
            d->vki.vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            d->vki.vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queues.data());
            if (d->presents) {
                std::uint32_t count{};
                check(d->vki.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr),
                      "vkEnumerateDeviceExtensionProperties");
                std::vector<VkExtensionProperties> provided(count);
                check(d->vki.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, provided.data()),
                      "vkEnumerateDeviceExtensionProperties");
                if (!has_extension(provided, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                    return std::nullopt;
                }
            }
            for (std::uint32_t i = 0; i < families; ++i) {
                if (!(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    continue;
                }
#ifdef _WIN32
                if (d->presents && !win32_presents(physical, i)) {
                    continue;
                }
#endif
                return i;
            }
            return std::nullopt;
        };

        std::uint32_t count{};
        check(d->vki.vkEnumeratePhysicalDevices(d->instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> physicals(count);
        check(d->vki.vkEnumeratePhysicalDevices(d->instance, &count, physicals.data()), "vkEnumeratePhysicalDevices");
        // The named adapter, else the first discrete GPU, else the first device; each with a graphics queue.
        int best = -1;
        int best_rank = -1;
        for (std::size_t i = 0; i < physicals.size(); ++i) {
            VkPhysicalDeviceProperties properties{};
            d->vki.vkGetPhysicalDeviceProperties(physicals[i], &properties);
            if (!graphics_family(physicals[i]) || properties.apiVersion < VK_API_VERSION_1_1) {
                continue;
            }
            const bool named = !options.adapter.empty() &&
                               std::string(properties.deviceName).find(options.adapter) != std::string::npos;
            const bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            const int rank = named ? 2 : discrete && options.adapter.empty() ? 1 : 0;
            if (rank > best_rank) {
                best = int(i);
                best_rank = rank;
            }
        }
        if (best < 0 || (!options.adapter.empty() && best_rank < 2)) {
            unavailable = options.adapter.empty()
                              ? std::string(d->presents ? "no Vulkan 1.1 device with a graphics queue that presents"
                                                        : "no Vulkan 1.1 device with a graphics queue")
                              : "no Vulkan device named " + options.adapter;
            return nullptr;
        }
        d->physical = physicals[std::size_t(best)];
        VkPhysicalDeviceProperties properties{};
        d->vki.vkGetPhysicalDeviceProperties(d->physical, &properties);
        d->name = properties.deviceName;
        d->api_version = properties.apiVersion;
        d->driver_version = properties.driverVersion;
        d->discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        d->vki.vkGetPhysicalDeviceMemoryProperties(d->physical, &d->memory);
        d->queue_family = *graphics_family(d->physical);
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = d->queue_family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device{};
        device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        device.enabledExtensionCount = d->presents ? 1u : 0u;
        device.ppEnabledExtensionNames = &swapchain;
        check(d->vki.vkCreateDevice(d->physical, &device, nullptr, &d->device), "vkCreateDevice");

#define WEFT_VULKAN_LOAD(name)                                                                                         \
    d->vkd.name = reinterpret_cast<PFN_##name>(d->vki.vkGetDeviceProcAddr(d->device, #name));                          \
    ensure(d->vkd.name != nullptr, #name);
        WEFT_VULKAN_DEVICE_FUNCTIONS(WEFT_VULKAN_LOAD)
        if (d->presents) {
            WEFT_VULKAN_SWAPCHAIN_FUNCTIONS(WEFT_VULKAN_LOAD)
        }
#undef WEFT_VULKAN_LOAD
        d->vkd.vkGetDeviceQueue(d->device, d->queue_family, 0, &d->queue);
        return d;
    }

    Device::~Device() {
        if (device) {
            vkd.vkDeviceWaitIdle(device);
            for (const auto& block : blocks_) {
                vkd.vkFreeMemory(device, block->memory, nullptr);
            }
            vkd.vkDestroyDevice(device, nullptr);
        }
        if (messenger_) {
            vki.vkDestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
        }
        if (instance) {
            vki.vkDestroyInstance(instance, nullptr);
        }
        close_loader(library_);
    }

    std::uint32_t Device::memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
        throw std::runtime_error("no Vulkan memory type with the required properties");
    }

    void Device::wait(VkFence fence) const {
        check(vkd.vkWaitForFences(device, 1, &fence, VK_TRUE, weft::constants::render::vulkan::fence_timeout_ns),
              "vkWaitForFences");
    }

    Image Device::image(VkFormat format, std::uint32_t w, std::uint32_t h, VkImageUsageFlags usage) const {
        Image out;
        out.format = format;
        out.width = w;
        out.height = h;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        try {
            check(vkd.vkCreateImage(device, &info, nullptr, &out.image), "vkCreateImage");
            VkMemoryRequirements needs{};
            vkd.vkGetImageMemoryRequirements(device, out.image, &needs);
            const auto type = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (needs.size <= weft::constants::render::vulkan::block_image_max_bytes) {
                place(out, needs, type);
            } else {
                VkMemoryAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocate.allocationSize = needs.size;
                allocate.memoryTypeIndex = type;
                check(vkd.vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory image");
                ++memory_allocations_;
            }
            check(vkd.vkBindImageMemory(device, out.image, out.memory, out.memory_offset), "vkBindImageMemory");
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = out.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkd.vkCreateImageView(device, &view, nullptr, &out.view), "vkCreateImageView");
        } catch (...) {
            destroy(out);
            throw;
        }
        return out;
    }

    Buffer Device::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped) const {
        Buffer out;
        out.size = size;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        try {
            check(vkd.vkCreateBuffer(device, &info, nullptr, &out.buffer), "vkCreateBuffer");
            VkMemoryRequirements needs{};
            vkd.vkGetBufferMemoryRequirements(device, out.buffer, &needs);
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.allocationSize = needs.size;
            allocate.memoryTypeIndex =
                memory_type(needs.memoryTypeBits,
                            mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkd.vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory buffer");
            ++memory_allocations_;
            check(vkd.vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory");
            if (mapped) {
                check(vkd.vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped), "vkMapMemory");
            }
        } catch (...) {
            destroy(out);
            throw;
        }
        return out;
    }

    void Device::destroy(Image& image) const noexcept {
        if (image.framebuffer) {
            vkd.vkDestroyFramebuffer(device, image.framebuffer, nullptr);
        }
        if (image.view) {
            vkd.vkDestroyImageView(device, image.view, nullptr);
        }
        if (image.image) {
            vkd.vkDestroyImage(device, image.image, nullptr);
        }
        if (image.memory) {
            if (image.memory_size) {
                release(image);
            } else {
                vkd.vkFreeMemory(device, image.memory, nullptr);
            }
        }
        image = {};
    }

    void Device::place(Image& image, const VkMemoryRequirements& needs, std::uint32_t type) const {
        const std::lock_guard lock(blocks_mutex_);
        for (const auto& block : blocks_) {
            if (block->type != type) {
                continue;
            }
            if (const auto offset = block->ranges.allocate(needs.size, needs.alignment)) {
                image.memory = block->memory;
                image.memory_offset = *offset;
                image.memory_size = needs.size;
                return;
            }
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = weft::constants::render::vulkan::image_block_bytes;
        allocate.memoryTypeIndex = type;
        VkDeviceMemory block{};
        check(vkd.vkAllocateMemory(device, &allocate, nullptr, &block), "vkAllocateMemory image block");
        ++memory_allocations_;
        try {
            blocks_.push_back(std::make_unique<ImageBlock>(block, type, allocate.allocationSize));
        } catch (...) {
            vkd.vkFreeMemory(device, block, nullptr);
            throw;
        }
        // A small image fits an empty block at any alignment: offset 0 is aligned.
        const auto offset = blocks_.back()->ranges.allocate(needs.size, needs.alignment);
        ensure(offset.has_value(), "a small image in an empty block");
        image.memory = block;
        image.memory_offset = *offset;
        image.memory_size = needs.size;
    }

    void Device::release(const Image& image) const noexcept {
        const std::lock_guard lock(blocks_mutex_);
        const auto block = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& b) {
            return b->memory == image.memory;
        });
        ensure(block != blocks_.end(), "an image range of one of the device's blocks");
        auto& ranges = (*block)->ranges;
        ranges.free(image.memory_offset, image.memory_size);
        if (ranges.used()) {
            return;
        }
        const auto type = (*block)->type;
        const bool other_empty = std::any_of(blocks_.begin(), blocks_.end(), [&](const auto& b) {
            return b.get() != block->get() && b->type == type && !b->ranges.used();
        });
        if (other_empty) {
            vkd.vkFreeMemory(device, (*block)->memory, nullptr);
            blocks_.erase(block);
        }
    }

    std::size_t Device::image_blocks() const {
        const std::lock_guard lock(blocks_mutex_);
        return blocks_.size();
    }

    void Device::destroy(Buffer& buffer) const noexcept {
        if (buffer.mapped) {
            vkd.vkUnmapMemory(device, buffer.memory);
        }
        if (buffer.buffer) {
            vkd.vkDestroyBuffer(device, buffer.buffer, nullptr);
        }
        if (buffer.memory) {
            vkd.vkFreeMemory(device, buffer.memory, nullptr);
        }
        buffer = {};
    }

    bool Device::blends(VkFormat format) const {
        VkFormatProperties properties{};
        vki.vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
        constexpr VkFormatFeatureFlags needed =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        return (properties.optimalTilingFeatures & needed) == needed;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL Device::report(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto& d = *static_cast<Device*>(user);
        try {
            if (!d.validation_messages && data && data->pMessage) {
                d.first_validation_message = data->pMessage;
            }
        } catch (...) {
        }
        ++d.validation_messages;
        return VK_FALSE;
    }
} // namespace weft::render::vulkan
