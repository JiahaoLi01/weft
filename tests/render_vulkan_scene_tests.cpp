#include "render/backends/vulkan/scene_renderer.hpp"
#include "support/check.hpp"
#include "support/scene_fonts.hpp"

#include <cstring>
#include <iostream>
#include <string>

// The Vulkan scene pass recorded into a host's command buffers (--adapter <name> picks the device): three frames in
// flight from three slots, submitted and retired by the host, draw what draw() draws, and each slot's later recordings
// reuse what its first one made; a recording that samples what an unsubmitted one uploads is Busy; a discarded
// recording's uploads leave the cache. Skipped (77) without a device.
namespace {
    using weft::testing::require;
    namespace vk = weft::render::vulkan;
    constexpr int skipped = 77;

    weft::SceneNode node(std::uint32_t slot, std::array<float, 4> box,
                         std::initializer_list<std::pair<const char*, const char*>> style, std::string text = {}) {
        weft::SceneNode n;
        n.identity = {1, 1, slot};
        n.tag = "div";
        n.box = box;
        n.text = std::move(text);
        for (const auto& [k, v] : style) {
            n.style[k] = v;
        }
        return n;
    }

    // Frame i: a moving rounded box with a shadow over a gradient, a counter text, a plane and a layer.
    weft::SceneFrame frame(unsigned i) {
        weft::SceneFrame f;
        f.root = node(1, {0, 0, 160, 96}, {{"background-image", "linear-gradient(90deg, #203040, #405060)"}});
        auto card = node(2, {8.f + float(i % 16) * 3, 8, 50, 30},
                         {{"background-color", "#d4a843"},
                          {"border-top-left-radius", "6px"},
                          {"border-top-right-radius", "6px"},
                          {"border-bottom-right-radius", "6px"},
                          {"border-bottom-left-radius", "6px"},
                          {"box-shadow", "2px 3px 4px 0px rgba(0, 0, 0, 0.5)"}});
        auto counter =
            node(3, {8, 48, 84, 20}, {{"font-size", "14px"}, {"color", "#ffffff"}}, "Frame " + std::to_string(i));
        auto plane = node(4, {96, 10, 50, 40},
                          {{"transform", "perspective(200px) rotateY(30deg)"}, {"background-color", "#20a060"}});
        auto layer = node(5, {96, 60, 50, 28}, {{"background-color", "#c02060"}, {"opacity", "0.8"}});
        layer.children = {node(6, {6, 6, 20, 10}, {{"background-color", "#ffffff"}})};
        f.root.children = {card, counter, plane, layer};
        return f;
    }

    // A host target and what it reads back through.
    struct Host {
        vk::Image image;
        vk::Buffer readback;
        VkCommandBuffer command{};
        VkFence fence{};
        vk::SceneRenderer::Slot slot;
        bool in_flight{};
        unsigned frame{};
    };
} // namespace

int main(int argc, char** argv) {
    try {
        vk::DeviceOptions options;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--adapter") {
                options.adapter = argv[i + 1];
            }
        }
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan scene recording: " << unavailable << '\n';
            return skipped;
        }
        const auto& d = *device;
        vk::SceneRenderer scene(*device, weft::testing::thread_scene_text());
        constexpr unsigned frames = 8;
        constexpr float scale = 2;
        constexpr std::uint32_t width = 320;
        constexpr std::uint32_t height = 192;
        const std::vector<weft::NodeIdentity> layers{{1, 1, 5}};
        std::vector<std::vector<std::uint8_t>> expected;
        for (unsigned i = 0; i < frames; ++i) {
            auto pixels = scene.draw(frame(i), scale, width, height, std::nullopt, 0, layers);
            require(pixels.has_value(), "draw()");
            expected.push_back(std::move(*pixels));
        }

        VkCommandPool pool{};
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = d.queue_family;
        vk::check(d.vkd.vkCreateCommandPool(d.device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        std::array<Host, 3> hosts;
        for (auto& h : hosts) {
            h.image = d.image(VK_FORMAT_B8G8R8A8_UNORM, width, height,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            h.readback = d.buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &h.command), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vk::check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &h.fence), "vkCreateFence");
        }
        const auto begin = [&](Host& h) {
            VkCommandBufferBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk::check(d.vkd.vkBeginCommandBuffer(h.command, &info), "vkBeginCommandBuffer");
        };
        const auto submit = [&](Host& h) {
            vk::transition(d, h.command, h.image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            d.vkd.vkCmdCopyImageToBuffer(h.command, h.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         h.readback.buffer, 1, &copy);
            VkBufferMemoryBarrier host{};
            host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = h.readback.buffer;
            host.size = VK_WHOLE_SIZE;
            d.vkd.vkCmdPipelineBarrier(h.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                       nullptr, 1, &host, 0, nullptr);
            vk::check(d.vkd.vkEndCommandBuffer(h.command), "vkEndCommandBuffer");
            VkSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            info.commandBufferCount = 1;
            info.pCommandBuffers = &h.command;
            vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &info, h.fence), "vkQueueSubmit");
            scene.submitted(h.slot);
            h.in_flight = true;
        };
        unsigned compared{};
        const auto complete = [&](Host& h) {
            if (!h.in_flight) {
                return;
            }
            d.wait(h.fence);
            vk::check(d.vkd.vkResetFences(d.device, 1, &h.fence), "vkResetFences");
            scene.retire(h.slot);
            h.in_flight = false;
            require(std::memcmp(h.readback.mapped, expected[h.frame].data(), expected[h.frame].size()) == 0,
                    "a frame in flight draws what draw() draws");
            ++compared;
        };

        // Three frames in flight: each slot is retired once its commands completed, before it records again. A frame's
        // counter text leaves the cache when the next frame prepares; the frame still running leases it. Every frame has
        // the same passes, so a slot's later recordings make no buffers, scratch images or descriptor pools.
        std::size_t primed{};
        for (unsigned i = 0; i < frames; ++i) {
            auto& h = hosts[i % hosts.size()];
            complete(h);
            begin(h);
            h.frame = i;
            require(scene.encode(h.slot, frame(i), h.command, {h.image.view, width, height}, scale, std::nullopt, 0,
                                 layers),
                    "encode");
            submit(h);
            if (i + 1 == hosts.size()) {
                primed = scene.slot_objects;
            }
        }
        for (auto& h : hosts) {
            complete(h);
        }
        require(compared == frames, "every frame in flight compared");
        require(primed > 0 && scene.slot_objects == primed, "slots reuse what their first recordings made");

        // A recording that samples what an unsubmitted one uploads is Busy and records nothing. Once that one is
        // submitted (still running) it records, and both draw alike.
        const auto finish = [&](Host& h) {
            d.wait(h.fence);
            vk::check(d.vkd.vkResetFences(d.device, 1, &h.fence), "vkResetFences");
            scene.retire(h.slot);
            h.in_flight = false;
        };
        auto fresh = frame(100);
        begin(hosts[0]);
        require(scene.encode(hosts[0].slot, fresh, hosts[0].command, {hosts[0].image.view, width, height}, scale,
                             std::nullopt, 0, layers),
                "first recording of new content");
        begin(hosts[1]);
        bool busy = false;
        try {
            (void)scene.encode(hosts[1].slot, fresh, hosts[1].command, {hosts[1].image.view, width, height}, scale,
                               std::nullopt, 0, layers);
        } catch (const vk::Busy&) {
            busy = true;
        }
        require(busy && hosts[1].slot.state == vk::SceneRenderer::Slot::State::Idle, "sampling an unsubmitted upload");
        submit(hosts[0]);
        require(scene.encode(hosts[1].slot, fresh, hosts[1].command, {hosts[1].image.view, width, height}, scale,
                             std::nullopt, 0, layers),
                "recording after the submission");
        submit(hosts[1]);
        finish(hosts[0]);
        finish(hosts[1]);
        require(std::memcmp(hosts[0].readback.mapped, hosts[1].readback.mapped, std::size_t(width) * height * 4) == 0,
                "both recordings of new content draw alike");

        // A discarded recording's uploads leave the cache: the next recording uploads them again.
        auto other = frame(200);
        const auto before = scene.uploads;
        begin(hosts[0]);
        require(scene.encode(hosts[0].slot, other, hosts[0].command, {hosts[0].image.view, width, height}, scale,
                             std::nullopt, 0, layers),
                "a recording to discard");
        const auto uploaded = scene.uploads - before;
        scene.discard(hosts[0].slot);
        vk::check(d.vkd.vkResetCommandBuffer(hosts[0].command, 0), "vkResetCommandBuffer");
        require(uploaded > 0 && hosts[0].slot.state == vk::SceneRenderer::Slot::State::Idle, "discarded recording");
        begin(hosts[1]);
        require(scene.encode(hosts[1].slot, other, hosts[1].command, {hosts[1].image.view, width, height}, scale,
                             std::nullopt, 0, layers),
                "recording after the discard");
        require(scene.uploads - before == 2 * uploaded, "a discarded recording's uploads are made again");
        submit(hosts[1]);
        finish(hosts[1]);

        d.vkd.vkDeviceWaitIdle(d.device);
        for (auto& h : hosts) {
            d.vkd.vkDestroyFence(d.device, h.fence, nullptr);
            d.destroy(h.readback);
            d.destroy(h.image);
        }
        d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
        if (d.validation_messages) {
            std::cerr << d.first_validation_message << '\n';
        }
        for (const auto& [reason, count] : scene.reasons) {
            std::cerr << "fallback " << reason << ": " << count << '\n';
        }
        require(!d.validation_messages && !scene.fallbacks, "validation and fallbacks");
        std::cout << "PASS Vulkan scene recording on " << d.name << ": " << compared
                  << " frames in flight equal to draw(), later recordings of a slot made nothing, Busy, discard\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
