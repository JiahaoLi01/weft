#include "gallery_layer_manifest.hpp"
#include "gallery_scene_session.hpp"
#include "probe_modes.hpp"
#include "react_window_host.hpp"
#include "render/backends/vulkan/scene_renderer.hpp"
#include "render/backends/vulkan/swapchain.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace probes {
    namespace {
        namespace vk = weft::render::vulkan;

        // One frame in flight: its commands, the scene slot they record with, and the scene target they copy from.
        struct WindowFrame {
            VkCommandBuffer command{};
            VkFence fence{};
            VkSemaphore acquired{};
            vk::SceneRenderer::Slot slot;
            vk::Image target;
            bool in_flight{};
        };

        // The nearest-rank p quantile of values, sorted in place; 0 without values.
        double quantile(std::vector<double>& values, double p) {
            if (values.empty()) {
                return 0;
            }
            std::sort(values.begin(), values.end());
            return values[std::max<std::size_t>(std::size_t(std::ceil(p * double(values.size()))), 1) - 1];
        }
    } // namespace

    // The gallery's 26 CSS and 8 D3 scenes on Vulkan in a window: one scene at a time, animated by the clock, drawn by
    // the native Vulkan scene pass into a target of the scene's size at the window's scale, and copied centered into
    // the swapchain image (a BGRA8 sRGB one takes the encoded bytes as they are). Two frames in flight. A click, Space or
    // the right arrow shows the next scene, the left arrow the previous one; Esc closes. The self test shows every scene
    // for a few frames and requires the last copy presented to equal what draw() draws of the same frame.
    int run_scene_vulkan_window(bool self_test, const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        vk::DeviceOptions options;
        options.presentation = true;
        options.validation = window_present_settings().validation;
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan scene window: " << unavailable << '\n';
            return weft::probe_constants::skipped_exit_code;
        }
        const auto& d = *device;
        NativeReactWindow window(true);
        require(SetWindowTextW(window.state.hwnd, L"CaseElephant - gallery scenes / native Vulkan") != 0,
                "Vulkan scene window title");
        vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
        vk::Swapchain swapchain(*device, vk::win32_surface(*device, GetModuleHandleW(nullptr), window.state.hwnd),
                                !self_test && window_present_settings().vsync);
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = d.queue_family;
        vk::check(d.vkd.vkCreateCommandPool(d.device, &pool_info, nullptr, &pool), "scene window command pool");
        std::array<WindowFrame, weft::constants::render::vulkan::frames_in_flight> frames;
        for (auto& f : frames) {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &f.command), "scene window command buffer");
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vk::check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &f.fence), "scene window fence");
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vk::check(d.vkd.vkCreateSemaphore(d.device, &semaphore, nullptr, &f.acquired), "scene window semaphore");
        }
        std::vector<VkSemaphore> rendered;
        vk::Buffer readback;
        const auto complete = [&](WindowFrame& f) {
            if (f.in_flight) {
                d.wait(f.fence);
                vk::check(d.vkd.vkResetFences(d.device, 1, &f.fence), "scene window fence reset");
                scene.retire(f.slot);
                f.in_flight = false;
            }
        };
        const auto drain = [&] {
            for (auto& f : frames) {
                complete(f);
            }
        };
        const auto resize = [&] {
            drain();
            RECT client{};
            GetClientRect(window.state.hwnd, &client);
            swapchain.resize(std::uint32_t(std::max<LONG>(client.right, 1)),
                             std::uint32_t(std::max<LONG>(client.bottom, 1)));
            for (auto s : rendered) {
                d.vkd.vkDestroySemaphore(d.device, s, nullptr);
            }
            rendered.assign(swapchain.images.size(), VK_NULL_HANDLE);
            for (auto& s : rendered) {
                VkSemaphoreCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                vk::check(d.vkd.vkCreateSemaphore(d.device, &info, nullptr, &s), "scene window present semaphore");
            }
            window.state.resize = false;
        };
        resize();
        require(swapchain.format == VK_FORMAT_B8G8R8A8_SRGB, "a BGRA8 sRGB swapchain takes encoded scene bytes");

        constexpr unsigned scene_count =
            weft::probe_constants::gallery::css_scene_count + weft::probe_constants::gallery::d3_scene_count;
        unsigned current = 0;
        std::unique_ptr<GallerySceneSession> session;
        auto shown = std::chrono::steady_clock::now();
        const auto open = [&](unsigned index) {
            drain();
            if (session) {
                session->unmount();
            }
            const bool d3 = index >= weft::probe_constants::gallery::css_scene_count;
            const auto local = d3 ? index - weft::probe_constants::gallery::css_scene_count : index;
            session = std::make_unique<GallerySceneSession>(d3, local,
                                                            d3 ? float(weft::probe_constants::gallery::d3_card_size)
                                                               : float(weft::probe_constants::gallery::css_card_width));
            shown = std::chrono::steady_clock::now();
            current = index;
            const auto name = (d3 ? "d3-" : "css-") + std::string(local < 10 ? "0" : "") + std::to_string(local);
            const auto title = "CaseElephant - " + name + " (" + std::to_string(index + 1) + "/" +
                               std::to_string(scene_count) +
                               ") / native Vulkan scene pass - click, Space or arrows switch, Esc closes";
            SetWindowTextW(window.state.hwnd, std::wstring(title.begin(), title.end()).c_str());
        };
        open(0);
        std::size_t presented{};
        std::size_t compared{};
        double slowest_ms{};
        double total_ms{};
        // The self test's frame times: the first frame of a scene (its scripts start, its text is prepared) apart from
        // the frames after it.
        std::vector<double> first_ms;
        std::vector<double> later_ms;
        unsigned scene_frames{};
        std::size_t next{};
        const auto first = std::chrono::steady_clock::now();
        while (!window.state.closed) {
            MSG key{};
            while (PeekMessageW(&key, window.state.hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)) {
                if (key.wParam == VK_ESCAPE) {
                    window.state.close_requested = true;
                } else if (key.wParam == VK_RIGHT || key.wParam == VK_SPACE) {
                    open((current + 1) % scene_count);
                    scene_frames = 0;
                } else if (key.wParam == VK_LEFT) {
                    open((current + scene_count - 1) % scene_count);
                    scene_frames = 0;
                }
            }
            window.messages();
            if (window.state.closed) {
                break;
            }
            while (!window.state.input.empty()) {
                const auto mouse = window.state.input.front();
                window.state.input.pop_front();
                if (mouse.action == weft::MouseAction::Up && !mouse.wheel) {
                    open((current + 1) % scene_count);
                    scene_frames = 0;
                }
            }
            if (window.state.resize || swapchain.stale) {
                resize();
            }
            if (!swapchain.extent.width || !swapchain.extent.height) {
                Sleep(weft::probe_constants::idle_wait_ms);
                continue;
            }
            const auto started = std::chrono::steady_clock::now();
            auto& f = frames[next++ % frames.size()];
            complete(f);
            const double elapsed = std::chrono::duration<double, std::milli>(started - shown).count();
            // The self test steps its scene on the fixed clock of the gallery's references.
            session->step(self_test ? scene_frames * weft::probe_constants::gallery::fixed_clock_step_ms : elapsed);
            const float scale = std::min(float(swapchain.extent.width) / session->width,
                                         float(swapchain.extent.height) / session->height);
            const auto target_width = std::max(1u, unsigned(session->width * scale));
            const auto target_height = std::max(1u, unsigned(session->height * scale));
            if (f.target.width != target_width || f.target.height != target_height) {
                d.destroy(f.target);
                f.target = d.image(VK_FORMAT_B8G8R8A8_UNORM, target_width, target_height,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            }
            const auto image = swapchain.acquire(f.acquired);
            if (!image) {
                continue;
            }
            const auto command = f.command;
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk::check(d.vkd.vkBeginCommandBuffer(command, &begin), "scene window begin");
            const auto layers = gallery_layer_manifest(current, session->current, true);
            require(scene.encode(f.slot, session->current, command, {f.target.view, target_width, target_height}, scale,
                                 std::nullopt, current, layers),
                    "Vulkan scene window frame fell back");
            vk::transition(d, command, f.target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            const auto swap = swapchain.images[*image];
            vk::transition(d, command, swap, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                           VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            const VkClearColorValue letterbox{{0, 0, 0, 1}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            d.vkd.vkCmdClearColorImage(command, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &letterbox, 1, &range);
            VkMemoryBarrier cleared{};
            cleared.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            cleared.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            d.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                       &cleared, 0, nullptr, 0, nullptr);
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstOffset = {std::int32_t((swapchain.extent.width - target_width) / 2),
                              std::int32_t((swapchain.extent.height - target_height) / 2), 0};
            copy.extent = {target_width, target_height, 1};
            d.vkd.vkCmdCopyImage(command, f.target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            // The self test reads the last frame of each scene back.
            const bool capture = self_test && scene_frames + 1 == weft::probe_constants::gallery::vulkan_window_frames;
            auto layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            if (capture) {
                const auto bytes = VkDeviceSize(target_width) * target_height * 4;
                if (readback.size < bytes) {
                    d.destroy(readback);
                    readback = d.buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                }
                vk::transition(d, command, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);
                layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                VkBufferImageCopy back{};
                back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                back.imageOffset = copy.dstOffset;
                back.imageExtent = copy.extent;
                d.vkd.vkCmdCopyImageToBuffer(command, swap, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                                             &back);
                VkBufferMemoryBarrier host{};
                host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                host.buffer = readback.buffer;
                host.size = VK_WHOLE_SIZE;
                d.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                           nullptr, 1, &host, 0, nullptr);
            }
            vk::transition(d, command, swap, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            vk::check(d.vkd.vkEndCommandBuffer(command), "scene window end");
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &f.acquired;
            submit.pWaitDstStageMask = &wait_stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &rendered[*image];
            vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, f.fence), "scene window submit");
            scene.submitted(f.slot);
            f.in_flight = true;
            swapchain.present(*image, rendered[*image]);
            ++presented;
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            slowest_ms = std::max(slowest_ms, ms);
            total_ms += ms;
            if (self_test) {
                (scene_frames == 0 ? first_ms : later_ms).push_back(ms);
            }
            if (capture) {
                // What the window shows is what draw() draws of the same frame.
                drain();
                std::vector<std::uint8_t> shown_bytes(std::size_t(target_width) * target_height * 4);
                std::memcpy(shown_bytes.data(), readback.mapped, shown_bytes.size());
                const auto drawn =
                    scene.draw(session->current, scale, target_width, target_height, std::nullopt, current, layers);
                require(drawn && *drawn == shown_bytes, "the window shows what draw() draws");
                ++compared;
            }
            ++scene_frames;
            if (self_test && scene_frames == weft::probe_constants::gallery::vulkan_window_frames) {
                if (current + 1 == scene_count) {
                    break;
                }
                open(current + 1);
                scene_frames = 0;
            }
        }
        drain();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count();
        if (session) {
            session->unmount();
        }
        for (auto s : rendered) {
            d.vkd.vkDestroySemaphore(d.device, s, nullptr);
        }
        for (auto& f : frames) {
            d.vkd.vkDestroySemaphore(d.device, f.acquired, nullptr);
            d.vkd.vkDestroyFence(d.device, f.fence, nullptr);
            d.destroy(f.target);
        }
        d.destroy(readback);
        d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
        std::cout << "Vulkan scene window on " << d.name << ": " << presented << " frames in " << seconds << " s, mean "
                  << (presented ? total_ms / double(presented) : 0) << " ms, slowest " << slowest_ms << " ms, "
                  << compared << " scenes compared, validation messages " << d.validation_messages << '\n';
        if (d.validation_messages) {
            std::cerr << d.first_validation_message << '\n';
        }
        if (self_test) {
            require(compared == scene_count && !scene.fallbacks && !d.validation_messages,
                    "Vulkan scene window coverage or validation");
            const auto first_p50 = quantile(first_ms, .5);
            const auto first_max = quantile(first_ms, 1);
            const auto later_p50 = quantile(later_ms, .5);
            const auto later_p99 = quantile(later_ms, .99);
            const auto later_max = quantile(later_ms, 1);
            std::cout << "Swapchain " << swapchain.extent.width << 'x' << swapchain.extent.height
                      << ", validation layer " << (d.validation ? "on" : "off") << "; a scene's first frame P50 "
                      << first_p50 << " ms, max " << first_max << " ms; later frames P50 " << later_p50 << " ms, P99 "
                      << later_p99 << " ms, max " << later_max << " ms; " << d.memory_allocations()
                      << " device memory allocations, " << d.image_blocks() << " image blocks\n";
            std::ostringstream report;
            report << "{\"format\":\"scene-vulkan-window-v3\",\"adapter\":" << json_string(d.name)
                   << ",\"swapchain_width\":" << swapchain.extent.width
                   << ",\"swapchain_height\":" << swapchain.extent.height
                   << ",\"validation_layer\":" << (d.validation ? "true" : "false") << ",\"frames\":" << presented
                   << ",\"scenes_compared\":" << compared
                   << ",\"mean_frame_ms\":" << (presented ? total_ms / double(presented) : 0)
                   << ",\"first_frame_ms\":{\"p50\":" << first_p50 << ",\"max\":" << first_max << '}'
                   << ",\"later_frame_ms\":{\"p50\":" << later_p50 << ",\"p99\":" << later_p99
                   << ",\"max\":" << later_max << "},\"memory_allocations\":" << d.memory_allocations()
                   << ",\"image_blocks\":" << d.image_blocks() << ",\"validation_messages\":" << d.validation_messages
                   << '}';
            write_text(folder / "report.json", report.str());
            std::cout << "PASS Vulkan scene window: every gallery scene presented as draw() draws it\n";
        }
        return 0;
    }
} // namespace probes
