#pragma once

#include "react_window_host.hpp"

#include "render/backends/vulkan/backend.hpp"
#include "render/backends/vulkan/swapchain.hpp"
#include "render/paint_list.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

namespace probes {
    namespace vk = weft::render::vulkan;

    // Vulkan sample host of a window, the counterpart of WindowPresenter with TwoFramePipeline: UI frames go through
    // the renderer C ABI into an offscreen target of the render size, which is copied letterboxed into the acquired
    // swapchain image and presented. Two frames in flight, each with its command buffer, fence and acquire semaphore;
    // a frame's completion reaches the renderer once its fence signals. A frame that presents the last UI again
    // (compose) is host work on the same queue, with a serial of its own.
    class VulkanWindowPresenter {
    public:
        VulkanWindowPresenter(vk::Device& device, HWND hwnd, std::uint32_t render_width, std::uint32_t render_height,
                              bool vsync);
        ~VulkanWindowPresenter();
        VulkanWindowPresenter(const VulkanWindowPresenter&) = delete;
        VulkanWindowPresenter& operator=(const VulkanWindowPresenter&) = delete;

        // Draws a UI frame of list and presents it; waits only while both slots are in flight.
        void submit(const std::shared_ptr<const weft::paint::DisplayList>& list) {
            frame(&list);
        }

        // Presents the last UI frame again.
        void compose() {
            frame(nullptr);
        }

        // Reports the frames that completed, oldest first, without waiting.
        void poll();
        // Waits for every frame in flight.
        void drain();
        // Swapchain images of the client size, after every frame in flight completed.
        void resize(std::uint32_t w, std::uint32_t h);
        // The last frame presented with capture set, as RGBA, after drain().
        std::vector<Pixel> pixels() const;
        std::size_t in_flight() const;

        vk::Device& gpu;
        vk::Renderer renderer;
        vk::Backend backend;
        vk::Swapchain swapchain;
        std::uint32_t width{};
        std::uint32_t height{};
        bool capture{};
        bool last_visible{};
        unsigned presents{};
        unsigned resizes{};
        std::size_t submissions{};
        std::size_t completions{};
        std::size_t peak_in_flight{};
        std::size_t capacity_waits{};
        std::uint64_t serial{};
        // The last frame's owner times in milliseconds: preparing the list, recording the UI and the copy, submitting
        // and presenting, and waiting for a free slot.
        double prepare_ms{};
        double encode_ms{};
        double present_ms{};
        double wait_ms{};
        // The last resize: waiting for the frames in flight and creating the swapchain, in milliseconds.
        double resize_ms{};
        // Called as each UI frame completes, with its serial and the GPU time of its UI pass in milliseconds.
        std::function<void(std::uint64_t, double)> completed_timing;

    private:
        struct Slot {
            VkCommandBuffer command{};
            VkFence fence{};
            VkSemaphore acquired{};
            // Serial of the frame in flight, 0 when the slot is free, and whether it drew a UI frame.
            std::uint64_t serial{};
            bool ui{};
        };

        void frame(const std::shared_ptr<const weft::paint::DisplayList>* list);
        void retire(Slot& slot);
        void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags written, VkAccessFlags read, VkPipelineStageFlags before,
                     VkPipelineStageFlags after) const;
        void destroy_rendered();

        VkCommandPool pool_{};
        std::array<Slot, weft::constants::render::vulkan::frames_in_flight> slots_;
        std::size_t next_{};
        // One per swapchain image: its presentation waits on it.
        std::vector<VkSemaphore> rendered_;
        vk::Image ui_;
        bool ui_drawn_{};
        vk::Buffer readback_;
        bool captured_{};
        VkQueryPool timestamps_{};
        double tick_ns_{};
        std::uint64_t tick_mask_{};
    };

    inline VulkanWindowPresenter::VulkanWindowPresenter(vk::Device& device, HWND hwnd, std::uint32_t render_width,
                                                        std::uint32_t render_height, bool vsync)
        : gpu(device),
          renderer(device, render_width, render_height),
          backend(renderer),
          swapchain(device, vk::win32_surface(device, GetModuleHandleW(nullptr), hwnd), vsync) {
        const auto& d = gpu;
        renderer.cache_assets = true;
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = d.queue_family;
        vk::check(d.vkd.vkCreateCommandPool(d.device, &pool, nullptr, &pool_), "window command pool");
        for (auto& slot : slots_) {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool_;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &slot.command), "window command buffer");
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vk::check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &slot.fence), "window frame fence");
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vk::check(d.vkd.vkCreateSemaphore(d.device, &semaphore, nullptr, &slot.acquired), "acquire semaphore");
        }
        ui_ = d.image(VK_FORMAT_R8G8B8A8_SRGB, render_width, render_height,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkPhysicalDeviceProperties properties{};
        d.vki.vkGetPhysicalDeviceProperties(d.physical, &properties);
        std::uint32_t families{};
        d.vki.vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        d.vki.vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &families, queues.data());
        const auto bits = queues.at(d.queue_family).timestampValidBits;
        if (bits) {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = std::uint32_t(2 * slots_.size());
            vk::check(d.vkd.vkCreateQueryPool(d.device, &query, nullptr, &timestamps_), "frame timestamps");
            tick_ns_ = properties.limits.timestampPeriod;
            tick_mask_ = bits >= 64 ? ~std::uint64_t{} : (std::uint64_t{1} << bits) - 1;
        }
    }

    inline VulkanWindowPresenter::~VulkanWindowPresenter() {
        const auto& d = gpu;
        try {
            drain();
        } catch (...) {
            std::terminate();
        }
        d.vkd.vkDeviceWaitIdle(d.device);
        destroy_rendered();
        for (auto& slot : slots_) {
            d.vkd.vkDestroySemaphore(d.device, slot.acquired, nullptr);
            d.vkd.vkDestroyFence(d.device, slot.fence, nullptr);
        }
        if (timestamps_) {
            d.vkd.vkDestroyQueryPool(d.device, timestamps_, nullptr);
        }
        d.vkd.vkDestroyCommandPool(d.device, pool_, nullptr);
        d.destroy(readback_);
        d.destroy(ui_);
    }

    inline void VulkanWindowPresenter::destroy_rendered() {
        for (auto semaphore : rendered_) {
            gpu.vkd.vkDestroySemaphore(gpu.device, semaphore, nullptr);
        }
        rendered_.clear();
    }

    inline std::size_t VulkanWindowPresenter::in_flight() const {
        std::size_t count{};
        for (const auto& slot : slots_) {
            count += slot.serial != 0;
        }
        return count;
    }

    inline void VulkanWindowPresenter::barrier(VkCommandBuffer command, VkImage image, VkImageLayout from,
                                               VkImageLayout to, VkAccessFlags written, VkAccessFlags read,
                                               VkPipelineStageFlags before, VkPipelineStageFlags after) const {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = written;
        b.dstAccessMask = read;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        gpu.vkd.vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    inline void VulkanWindowPresenter::retire(Slot& slot) {
        const auto& d = gpu;
        d.wait(slot.fence);
        vk::check(d.vkd.vkResetFences(d.device, 1, &slot.fence), "reset frame fence");
        double gpu_ms{};
        const auto index = std::size_t(&slot - slots_.data());
        if (slot.ui && timestamps_) {
            std::array<std::uint64_t, 2> ticks{};
            vk::check(d.vkd.vkGetQueryPoolResults(d.device, timestamps_, std::uint32_t(2 * index), 2, sizeof(ticks),
                                                  ticks.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT),
                      "frame timestamps");
            gpu_ms = double((ticks[1] - ticks[0]) & tick_mask_) * tick_ns_ / 1e6;
        }
        render_result(weft_renderer_notify_completed(backend.renderer, weft::constants::render::vulkan::device_epoch,
                                                     weft::constants::render::vulkan::queue, slot.serial),
                      "Vulkan frame completion");
        ++completions;
        if (slot.ui && completed_timing) {
            completed_timing(slot.serial, gpu_ms);
        }
        slot.serial = 0;
        slot.ui = false;
    }

    inline void VulkanWindowPresenter::poll() {
        for (;;) {
            Slot* oldest{};
            for (auto& slot : slots_) {
                if (slot.serial && (!oldest || slot.serial < oldest->serial)) {
                    oldest = &slot;
                }
            }
            if (!oldest || gpu.vkd.vkGetFenceStatus(gpu.device, oldest->fence) != VK_SUCCESS) {
                return;
            }
            retire(*oldest);
        }
    }

    inline void VulkanWindowPresenter::drain() {
        for (;;) {
            Slot* oldest{};
            for (auto& slot : slots_) {
                if (slot.serial && (!oldest || slot.serial < oldest->serial)) {
                    oldest = &slot;
                }
            }
            if (!oldest) {
                return;
            }
            retire(*oldest);
        }
    }

    inline void VulkanWindowPresenter::resize(std::uint32_t w, std::uint32_t h) {
        const auto& d = gpu;
        const auto started = std::chrono::steady_clock::now();
        drain();
        d.vkd.vkDeviceWaitIdle(d.device);
        destroy_rendered();
        swapchain.resize(w, h);
        width = swapchain.extent.width;
        height = swapchain.extent.height;
        for (std::size_t i = 0; i < swapchain.images.size(); ++i) {
            VkSemaphoreCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore semaphore{};
            vk::check(d.vkd.vkCreateSemaphore(d.device, &info, nullptr, &semaphore), "present semaphore");
            rendered_.push_back(semaphore);
        }
        // The readback of captured frames is created by the first capture at this size.
        d.destroy(readback_);
        captured_ = false;
        ++resizes;
        resize_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }

    inline void VulkanWindowPresenter::frame(const std::shared_ptr<const weft::paint::DisplayList>* list) {
        using clock = std::chrono::steady_clock;
        const auto since = [](clock::time_point start) {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count();
        };
        const auto& d = gpu;
        auto& slot = slots_[next_];
        const auto waiting = clock::now();
        if (slot.serial) {
            ++capacity_waits;
            retire(slot);
        }
        wait_ms = since(waiting);
        if (swapchain.stale) {
            resize(width, height);
        }
        auto image = swapchain.acquire(slot.acquired);
        if (!image) {
            resize(width, height);
            image = swapchain.acquire(slot.acquired);
        }
        if (!image) {
            // Minimized: nothing to present into.
            last_visible = false;
            return;
        }
        const auto preparing = clock::now();
        weft_render_handle prepared{};
        if (list) {
            const weft_paint_list handle{*list};
            const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &handle, 0, 0, nullptr};
            render_result(weft_renderer_prepare_list(backend.renderer, &input, &prepared), "Vulkan window prepare");
        }
        prepare_ms = since(preparing);
        const auto recording = clock::now();
        const auto command = slot.command;
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk::check(d.vkd.vkBeginCommandBuffer(command, &begin), "window frame begin");
        weft_render_handle use{};
        if (list) {
            const auto query = std::uint32_t(2 * next_);
            if (timestamps_) {
                d.vkd.vkCmdResetQueryPool(command, timestamps_, query, 2);
                d.vkd.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_, query);
            }
            // The previous frame's copy read the UI target.
            barrier(command, ui_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            weft_render_vulkan_pass payload{};
            const auto pass = backend.pass(command, ui_.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
            // The copy does not blend: the UI starts on opaque black, as the D3D11 window composites it over black.
            payload.clear_linear_premultiplied[3] = 1;
            const auto encoded = weft_renderer_encode(backend.renderer, prepared, &pass, &use);
            if (encoded != WEFT_RENDER_OK) {
                d.vkd.vkResetCommandBuffer(command, 0);
                weft_renderer_release_prepared(backend.renderer, prepared);
                render_result(encoded, "Vulkan window encode");
            }
            render_result(weft_renderer_release_prepared(backend.renderer, prepared), "Vulkan window release");
            if (timestamps_) {
                d.vkd.vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, query + 1);
            }
            barrier(command, ui_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            ui_drawn_ = true;
        }
        require(ui_drawn_, "a composition presents an earlier UI frame");
        // The UI letterboxed into the swapchain image: black, then the scaled UI.
        const auto target = swapchain.images.at(*image);
        barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue black{{0, 0, 0, 1}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        d.vkd.vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        const auto viewport =
            weft::Viewport::fit(float(width), float(height), float(renderer.width), float(renderer.height));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {std::int32_t(renderer.width), std::int32_t(renderer.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {std::int32_t(std::lround(viewport.x)), std::int32_t(std::lround(viewport.y)), 0};
        blit.dstOffsets[1] = {std::int32_t(std::lround(viewport.x + viewport.width)),
                              std::int32_t(std::lround(viewport.y + viewport.height)), 1};
        d.vkd.vkCmdBlitImage(command, ui_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        if (capture) {
            if (!readback_.buffer) {
                readback_ =
                    d.buffer(VkDeviceSize(width) * height * sizeof(Pixel), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            }
            barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            d.vkd.vkCmdCopyImageToBuffer(command, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1,
                                         &copy);
            VkBufferMemoryBarrier host{};
            host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = readback_.buffer;
            host.size = VK_WHOLE_SIZE;
            d.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                       nullptr, 1, &host, 0, nullptr);
            barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else {
            barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        vk::check(d.vkd.vkEndCommandBuffer(command), "window frame end");
        encode_ms = since(recording);
        const auto presenting = clock::now();
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &slot.acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered_.at(*image);
        vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, slot.fence), "window frame submit");
        slot.serial = ++serial;
        slot.ui = list != nullptr;
        if (list) {
            render_result(
                weft_renderer_notify_submitted(backend.renderer, use, weft::constants::render::vulkan::queue, serial),
                "Vulkan window submission");
        }
        ++submissions;
        peak_in_flight = std::max(peak_in_flight, in_flight());
        // The readback keeps the last captured frame.
        captured_ = captured_ || capture;
        swapchain.present(*image, rendered_.at(*image));
        last_visible = true;
        ++presents;
        present_ms = since(presenting);
        next_ = (next_ + 1) % slots_.size();
    }

    inline std::vector<Pixel> VulkanWindowPresenter::pixels() const {
        require(!in_flight() && captured_, "readback after a captured frame completed (swapchain " +
                                               std::to_string(width) + "x" + std::to_string(height) + ")");
        std::vector<Pixel> out(std::size_t(width) * height);
        std::memcpy(out.data(), readback_.mapped, out.size() * sizeof(Pixel));
        if (swapchain.format == VK_FORMAT_B8G8R8A8_SRGB) {
            for (auto& p : out) {
                std::swap(p[0], p[2]);
            }
        }
        return out;
    }
} // namespace probes
