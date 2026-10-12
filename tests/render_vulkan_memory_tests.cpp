#include "render/backends/vulkan/device.hpp"
#include "render/backends/vulkan/memory.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

// Device memory that images share.
// --heap: random allocations and frees of a RangeHeap stay aligned, and with its free ranges tile the heap, no two free
// ranges adjacent; a heap refuses only what no free range fits, and freeing a free range throws.
// --device [--adapter <name>]: images of random sizes, made and destroyed in random order (small ones in shared blocks,
// large ones with memory of their own), each keep their own pixels; they take few allocations, and once all are
// destroyed the device keeps one empty block. Skipped (77) without a device.
namespace {
    using weft::testing::require;
    namespace vk = weft::render::vulkan;
    constexpr int skipped = 77;

    // The free ranges of heap and the allocated ranges live (sizes by offset) tile the heap, and no two free ranges
    // are adjacent.
    void require_tiling(const vk::RangeHeap& heap, const std::map<std::uint64_t, std::uint64_t>& live) {
        const auto& free = heap.free_ranges();
        auto f = free.begin();
        auto l = live.begin();
        std::uint64_t at = 0;
        bool previous_free = false;
        while (f != free.end() || l != live.end()) {
            const bool take_free = l == live.end() || (f != free.end() && f->first < l->first);
            const auto [start, length] = take_free ? *f : *l;
            require(start == at && length > 0, "free and allocated ranges tile the heap");
            require(!(take_free && previous_free), "adjacent free ranges are joined");
            previous_free = take_free;
            at = start + length;
            if (take_free) {
                ++f;
            } else {
                ++l;
            }
        }
        require(at == heap.size(), "the ranges reach the heap's end");
    }

    void heap_contract() {
        {
            vk::RangeHeap heap(4096);
            require(heap.allocate(4096, 1) == 0 && !heap.allocate(1, 1), "an exact fit, then a full heap");
            heap.free(0, 4096);
            require(heap.allocate(100, 1) == 0 && heap.allocate(100, 256) == 256, "an aligned range after padding");
            std::map<std::uint64_t, std::uint64_t> live{{0, 100}, {256, 100}};
            require_tiling(heap, live);
            require(heap.allocate(156, 1) == 100, "the padding fits a range of its size");
            live.emplace(100, 156);
            require_tiling(heap, live);
            require(heap.allocate(3740, 4) == 356 && !heap.allocate(1, 1), "the rest fits exactly");
            heap.free(0, 100);
            bool threw = false;
            try {
                heap.free(0, 100);
            } catch (const std::exception&) {
                threw = true;
            }
            require(threw, "freeing a free range throws");
        }
        std::mt19937_64 random(20261012);
        for (int round = 0; round < 6; ++round) {
            constexpr std::uint64_t size = 1u << 20;
            vk::RangeHeap heap(size);
            std::map<std::uint64_t, std::uint64_t> live;
            std::uint64_t live_bytes = 0;
            const auto free_one = [&] {
                auto it = live.begin();
                std::advance(it, std::ptrdiff_t(random() % live.size()));
                heap.free(it->first, it->second);
                live_bytes -= it->second;
                live.erase(it);
            };
            for (int step = 0; step < 3000; ++step) {
                if (live.empty() || random() % 100 < 55) {
                    const auto bytes = 1 + random() % (round % 2 ? 4096 : 65536);
                    const auto alignment = std::uint64_t(1) << (random() % 13);
                    const auto offset = heap.allocate(bytes, alignment);
                    if (!offset) {
                        for (const auto& [start, length] : heap.free_ranges()) {
                            const auto aligned = (start + alignment - 1) / alignment * alignment;
                            require(aligned >= start + length || start + length - aligned < bytes,
                                    "a refusal only when no free range fits");
                        }
                        continue;
                    }
                    require(*offset % alignment == 0 && live.emplace(*offset, bytes).second, "an aligned new range");
                    live_bytes += bytes;
                } else {
                    free_one();
                }
                require(heap.used() == live_bytes, "used bytes");
                require_tiling(heap, live);
            }
            while (!live.empty()) {
                free_one();
            }
            require(heap.used() == 0 && heap.free_ranges().size() == 1 && heap.free_ranges().begin()->second == size,
                    "one free range once every range is freed");
        }
        std::cout << "PASS range heap: exact fits, padding, refusals, random allocations tile the heap\n";
    }

    // The color image i is cleared to: its index in red and green.
    std::array<std::uint8_t, 4> color(std::size_t i) {
        return {std::uint8_t(i & 255), std::uint8_t((i >> 8) & 255), std::uint8_t(0x5a ^ (i * 7 & 255)), 255};
    }

    int device_contract(const std::string& adapter) {
        vk::DeviceOptions options;
        options.adapter = adapter;
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan device memory: " << unavailable << '\n';
            return skipped;
        }
        const auto& d = *device;
        // Sampled, as the images of renderers are: every image has a view.
        constexpr VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        const auto allocations = d.memory_allocations();
        std::mt19937 random(20261012);
        std::vector<vk::Image> images;
        std::size_t own{};
        std::size_t peak_blocks{};
        const auto make = [&] {
            const bool large = random() % 64 == 0;
            const auto w = large ? 760 + random() % 64 : 1 + random() % 192;
            const auto h = large ? 760 + random() % 64 : 1 + random() % 192;
            images.push_back(d.image(VK_FORMAT_B8G8R8A8_UNORM, w, h, usage));
            require(large == !images.back().memory_size, "small images share blocks, large ones have their own");
            own += large;
            peak_blocks = std::max(peak_blocks, d.image_blocks());
        };
        const auto drop = [&] {
            const auto k = std::ptrdiff_t(random() % images.size());
            d.destroy(images[std::size_t(k)]);
            images.erase(images.begin() + k);
        };
        for (int i = 0; i < 300; ++i) {
            make();
        }
        for (int i = 0; i < 150; ++i) {
            drop();
        }
        for (int i = 0; i < 200; ++i) {
            make();
        }

        // Each image cleared to its own color, then all read back: no clear reaches another image.
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = d.queue_family;
        vk::check(d.vkd.vkCreateCommandPool(d.device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBuffer command{};
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &command), "vkAllocateCommandBuffers");
        VkFence fence{};
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk::check(d.vkd.vkCreateFence(d.device, &fence_info, nullptr, &fence), "vkCreateFence");
        std::vector<VkDeviceSize> offsets;
        VkDeviceSize total{};
        for (const auto& image : images) {
            offsets.push_back(total);
            total += VkDeviceSize(image.width) * image.height * 4;
        }
        auto readback = d.buffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk::check(d.vkd.vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (std::size_t i = 0; i < images.size(); ++i) {
            vk::transition(d, command, images[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            const auto c = color(i);
            // Red, green, blue and alpha; B8G8R8A8 stores them as blue, green, red, alpha bytes.
            const VkClearColorValue clear{{c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, c[3] / 255.f}};
            d.vkd.vkCmdClearColorImage(command, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                       &range);
        }
        for (std::size_t i = 0; i < images.size(); ++i) {
            vk::transition(d, command, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offsets[i];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {images[i].width, images[i].height, 1};
            d.vkd.vkCmdCopyImageToBuffer(command, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         readback.buffer, 1, &copy);
        }
        VkBufferMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.buffer;
        host.size = VK_WHOLE_SIZE;
        d.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                                   1, &host, 0, nullptr);
        vk::check(d.vkd.vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence), "vkQueueSubmit");
        d.wait(fence);
        for (std::size_t i = 0; i < images.size(); ++i) {
            const auto c = color(i);
            const std::array<std::uint8_t, 4> bgra{c[2], c[1], c[0], c[3]};
            const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped) + offsets[i];
            for (std::size_t p = 0; p < std::size_t(images[i].width) * images[i].height; ++p) {
                require(std::memcmp(bytes + p * 4, bgra.data(), 4) == 0, "every image keeps its own pixels");
            }
        }
        const auto made = d.memory_allocations() - allocations;
        const auto blocks = d.image_blocks();
        while (!images.empty()) {
            drop();
        }
        require(d.image_blocks() == 1, "one empty block kept once every image is destroyed");
        d.destroy(readback);
        d.vkd.vkDestroyFence(d.device, fence, nullptr);
        d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
        // Allocations: the readback buffer, the large images' memory, and a few blocks for the 500 small images.
        constexpr std::size_t max_blocks_made = 16;
        require(made <= 1 + own + max_blocks_made, "images take few allocations");
        if (d.validation_messages) {
            std::cerr << d.first_validation_message << '\n';
        }
        require(!d.validation_messages, "validation");
        std::cout << "PASS Vulkan device memory on " << d.name << ": " << total / 4 << " pixels of " << offsets.size()
                  << " images kept apart, " << made << " allocations (" << own << " large images, " << blocks
                  << " blocks at the end, " << peak_blocks << " at the peak)\n";
        return 0;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        std::string adapter;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--adapter") {
                adapter = argv[i + 1];
            }
        }
        if (mode == "--heap") {
            heap_contract();
            return 0;
        }
        require(mode == "--device", "usage: weft_render_vulkan_memory_tests --heap | --device [--adapter <name>]");
        return device_contract(adapter);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
