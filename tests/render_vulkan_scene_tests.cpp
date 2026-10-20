#include "render/backends/vulkan/scene_renderer.hpp"
#include "support/check.hpp"
#include "support/native_scene.hpp"
#include "support/scene_fonts.hpp"

#include <cstring>
#include <iostream>
#include <string>

// The Vulkan scene pass recorded into a host's command buffers (--adapter <name> picks the device): three frames in
// flight from three slots, submitted and retired by the host, draw what draw() draws, and each slot's later recordings
// reuse what its first one made; a recording that samples what an unsubmitted one uploads is Busy; a discarded
// recording's uploads leave the cache. A page's frames from an incremental solver, recorded three in flight, patch
// the kept pass (each slot then holds the records of an earlier version and writes what the patches since changed) and
// draw what a renderer that prepares every frame anew draws. Skipped (77) without a device.
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

    // Seeded draws.
    std::uint32_t draw(std::uint32_t& seed) {
        seed = seed * weft::constants::lcg_multiplier + weft::constants::lcg_increment;
        return seed >> 8;
    }

    // A panel of boxes and text whose boxes move, recolor and fade frame by frame, as an incremental solver gives its
    // frames (with revisions, so that a renderer keeps and patches its pass), recorded three in flight from three
    // slots, each slot retired before it records again: every frame draws what a renderer that prepares each frame anew
    // draws. Returns how many recordings patched the kept pass.
    std::size_t patched_in_flight(vk::Device& d) {
        auto& text = weft::testing::thread_scene_text();
        constexpr std::uint32_t width = 320;
        constexpr std::uint32_t height = 192;
        weft::testing::NativeScene s;
        s.set(*s.root, "style", "position:relative;width:320px;height:192px;background-color:#102030");
        std::vector<weft::dom::Node*> boxes;
        for (int i = 0; i < 12; ++i) {
            auto& box = s.html("div");
            s.set(box, "style",
                  "position:absolute;left:" + std::to_string(10 + (i % 6) * 50) +
                      "px;top:" + std::to_string(10 + (i / 6) * 90) +
                      "px;width:40px;height:30px;"
                      "background-color:#d4a843;border-radius:6px");
            s.tree.insert(*s.root, box, nullptr);
            auto& label = s.tree.create(weft::constants::text_node_type, "#text", "", s.document->identity);
            label.data = "B" + std::to_string(i);
            s.tree.insert(box, label, nullptr);
            boxes.push_back(&box);
        }
        weft::SceneSolver solver("", "", text);
        weft::SceneFrame frame;
        vk::SceneRenderer patching(d, text);
        vk::SceneRenderer fresh(d, text);
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
        std::vector<std::vector<std::uint8_t>> expected;
        const auto complete = [&](Host& h) {
            if (!h.in_flight) {
                return;
            }
            d.wait(h.fence);
            vk::check(d.vkd.vkResetFences(d.device, 1, &h.fence), "vkResetFences");
            patching.retire(h.slot);
            h.in_flight = false;
            require(std::memcmp(h.readback.mapped, expected[h.frame].data(), expected[h.frame].size()) == 0,
                    "a patched frame in flight draws what a fresh pass draws, frame " + std::to_string(h.frame));
        };
        std::uint32_t seed = 0x9a7;
        constexpr unsigned frames = 60;
        for (unsigned i = 0; i < frames; ++i) {
            auto& box = *boxes[draw(seed) % boxes.size()];
            const auto kind = draw(seed) % 3;
            const auto left = 10 + int(draw(seed) % 260);
            s.set(box, "style",
                  "position:absolute;left:" + std::to_string(left) +
                      "px;top:40px;width:40px;height:30px;border-radius:6px;background-color:" +
                      (kind ? "#d4a843" : "#4080c0") + (kind == 2 ? ";opacity:0.5" : ""));
            solver.update(s.tree.snapshot(*s.root), frame, width, height, i * 16.0);
            fresh.forget(0);
            auto pixels = fresh.draw(frame, 1, width, height, std::nullopt, 0, {});
            require(pixels.has_value(), "a fresh draw()");
            expected.push_back(std::move(*pixels));
            auto& h = hosts[i % hosts.size()];
            complete(h);
            VkCommandBufferBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk::check(d.vkd.vkBeginCommandBuffer(h.command, &info), "vkBeginCommandBuffer");
            h.frame = i;
            require(patching.encode(h.slot, frame, h.command, {h.image.view, width, height}, 1, std::nullopt, 0, {}),
                    "a patched recording");
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
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &h.command;
            vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, h.fence), "vkQueueSubmit");
            patching.submitted(h.slot);
            h.in_flight = true;
        }
        for (auto& h : hosts) {
            complete(h);
        }
        for (auto& h : hosts) {
            patching.retire(h.slot);
            d.vkd.vkDestroyFence(d.device, h.fence, nullptr);
            d.destroy(h.readback);
            d.destroy(h.image);
        }
        d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
        return patching.kept_patches;
    }

    // A recording whose scratch passes the bytes of spares a slot keeps of what it leaves (two nested groups over a
    // target past 4K, each an image of the target's size) keeps what it drew with all the same: the slot's later
    // recordings of the frame make no scratch images. Returns the objects the first recording made.
    std::size_t large_groups_kept(vk::Device& d) {
        constexpr std::uint32_t width = 4096;
        constexpr std::uint32_t height = 2304;
        static_assert(std::size_t(width) * height * 4 * 2 >
                      std::size_t(weft::constants::render::vulkan::scene_slot_spare_image_bytes));
        vk::SceneRenderer scene(d, weft::testing::thread_scene_text());
        weft::SceneFrame f;
        f.root = node(1, {0, 0, float(width), float(height)}, {{"background-color", "#102030"}});
        auto outer = node(2, {0, 0, float(width), float(height)}, {{"opacity", "0.8"}, {"background-color", "#405060"}});
        auto inner = node(3, {100, 100, 800, 600}, {{"opacity", "0.6"}, {"background-color", "#c02060"}});
        inner.children = {node(4, {10, 10, 100, 60}, {{"background-color", "#ffffff"}})};
        outer.children = {inner, node(5, {1000, 100, 200, 100}, {{"background-color", "#20a060"}})};
        f.root.children = {outer};
        VkCommandPool pool{};
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = d.queue_family;
        vk::check(d.vkd.vkCreateCommandPool(d.device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        Host h;
        h.image = d.image(VK_FORMAT_B8G8R8A8_UNORM, width, height, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, &h.command), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vk::check(d.vkd.vkCreateFence(d.device, &fence, nullptr, &h.fence), "vkCreateFence");
        std::size_t first{};
        for (int k = 0; k < 3; ++k) {
            VkCommandBufferBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk::check(d.vkd.vkBeginCommandBuffer(h.command, &info), "vkBeginCommandBuffer");
            require(scene.encode(h.slot, f, h.command, {h.image.view, width, height}, 1, std::nullopt, 0, {}),
                    "a recording of nested groups at 4K");
            vk::check(d.vkd.vkEndCommandBuffer(h.command), "vkEndCommandBuffer");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &h.command;
            vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, h.fence), "vkQueueSubmit");
            scene.submitted(h.slot);
            d.wait(h.fence);
            vk::check(d.vkd.vkResetFences(d.device, 1, &h.fence), "vkResetFences");
            scene.retire(h.slot);
            if (k == 0) {
                first = scene.slot_objects;
            }
        }
        require(first > 0 && scene.slot_objects == first,
                "groups past the spare bytes are kept for the slot's next recordings: " +
                    std::to_string(scene.slot_objects - first) + " objects made again");
        d.vkd.vkDestroyFence(d.device, h.fence, nullptr);
        d.destroy(h.image);
        d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
        return first;
    }
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
        const auto patched = patched_in_flight(*device);
        require(patched > 30, "most recordings patch the kept pass: " + std::to_string(patched));
        (void)large_groups_kept(*device);
        if (d.validation_messages) {
            std::cerr << d.first_validation_message << '\n';
        }
        for (const auto& [reason, count] : scene.reasons) {
            std::cerr << "fallback " << reason << ": " << count << '\n';
        }
        require(!d.validation_messages && !scene.fallbacks, "validation and fallbacks");
        std::cout << "PASS Vulkan scene recording on " << d.name << ": " << compared
                  << " frames in flight equal to draw(), later recordings of a slot made nothing, Busy, discard; "
                  << patched << " patched recordings in flight as fresh passes; groups at 4K kept\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
