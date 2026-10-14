#include "scene/scene_native.hpp"
#include "support/check.hpp"
#include "support/d3d11_scene.hpp"
#include "support/png.hpp"
#include "support/scene_fonts.hpp"
#ifdef WEFT_TEST_VULKAN
#include "render/backends/vulkan/pipeline.hpp"
#include "render/backends/vulkan/scene_renderer.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// Small text, shadow and path images on atlas pages (step 12.3 of docs/refactoring-plan.md). A frame drawn with atlas
// pages matches the frame drawn with a texture for each image but for the rounding of where its images are sampled, in
// far fewer draws: labels over boxes, text of one color and in colors of its own, a shadow, SVG paths small and large,
// a label in a translucent group, and a rotated label whose filtering would read its neighbour on the page past its
// border (a path its box clips, whose edge texels are opaque). Frames of other labels take the regions earlier labels
// gave back and show only what the new ones hold. Vulkan, under validation, draws the pages as D3D11, also when two
// recordings copy into one page before either is submitted, and when a recording that copied into a page is never
// submitted.
namespace {
    using weft::testing::require;

    constexpr unsigned width = 800;
    constexpr unsigned height = 600;
    constexpr float scale = 2;
    // Sampling an image where it lies on a page rounds its texture coordinates in their last bits otherwise than on a
    // texture of its own: at an edge, filtering may take its weights a step of the sampler's precision apart. A
    // neighbour on the page read past a border, or a region's earlier image left in it, differs by its whole color.
    constexpr int max_difference = 2;
    constexpr std::size_t max_differing_pixels = 2000;
    // Vulkan and D3D11 WARP filter alike within the gallery's near level (vulkan_scene_near_level).
    constexpr int max_vulkan_difference = 2;

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

    weft::SceneNode svg_node(std::uint32_t slot, const char* tag,
                             std::initializer_list<std::pair<const char*, const char*>> attributes,
                             std::initializer_list<std::pair<const char*, const char*>> style) {
        auto n = node(slot, {}, style);
        n.tag = tag;
        n.space = weft::constants::svg_namespace_uri;
        for (const auto& [k, v] : attributes) {
            n.attributes[k] = v;
        }
        return n;
    }

    // Labels of word over boxes, each fifth in colors of its own; a shadow; SVG triangles and a large curve; a label in
    // a translucent group; a rotated label, and beside it on its page a path its box clips, whose edge texels are
    // opaque.
    weft::SceneFrame frame(const std::string& word, std::uint32_t first) {
        weft::SceneFrame f;
        f.root = node(first, {0, 0, width / scale, height / scale}, {{"background-color", "#fdfcf8"}});
        auto& c = f.root.children;
        auto slot = first + 1;
        for (unsigned i = 0; i < 24; ++i) {
            const float x = 6 + float(i % 4) * 96;
            const float y = 6 + float(i / 4) * 26;
            c.push_back(node(slot++, {x, y, 88, 22}, {{"background-color", i % 3 ? "#e8e2d0" : "#c9d8f0"}}));
            const bool colored = i % 5 == 4;
            c.push_back(node(slot++, {x + 2, y + 2, 84, 18}, {{"font-size", "12px"}, {"color", "#20304a"}},
                             (colored ? "\xe2\x98\x85 " : "") + word + " " + std::to_string(i)));
        }
        c.push_back(node(slot++, {10, 170, 60, 30},
                         {{"background-color", "#3a6ea5"}, {"box-shadow", "3px 4px 6px 0px rgba(10, 20, 40, 0.6)"}}));
        auto svg = svg_node(slot++, "svg", {{"width", "200"}, {"height", "110"}}, {});
        svg.box = {90, 165, 200, 110};
        svg.children = {svg_node(slot++, "path", {{"d", "M10 10 L40 14 L22 40 Z"}}, {{"fill", "#c0392b"}}),
                        svg_node(slot++, "path", {{"d", "M50 10 L80 18 L58 44 Z"}}, {{"fill", "#16a085"}}),
                        svg_node(slot++, "path", {{"d", "M5 100 C 60 20, 140 120, 195 30"}},
                                 {{"fill", "none"}, {"stroke", "#8e44ad"}, {"stroke-width", "3"}})};
        c.push_back(std::move(svg));
        auto group = node(slot++, {300, 170, 90, 50}, {{"opacity", "0.6"}, {"background-color", "#2c3e50"}});
        group.children = {node(slot++, {4, 4, 80, 18}, {{"font-size", "12px"}, {"color", "#ffffff"}}, word + " a"),
                          node(slot++, {4, 26, 80, 18}, {{"font-size", "12px"}, {"color", "#f1c40f"}}, word + " b")};
        c.push_back(std::move(group));
        c.push_back(node(slot++, {300, 240, 84, 18},
                         {{"font-size", "12px"}, {"color", "#7f1d1d"}, {"transform", "rotate(11deg)"}}, word));
        auto clipped = node(slot++, {300, 268, 60, 18}, {{"overflow", "hidden"}});
        auto cut = svg_node(slot++, "svg", {{"width", "60"}, {"height", "18"}}, {});
        cut.box = {0, 0, 60, 18};
        cut.children = {svg_node(slot++, "path", {{"d", "M-10 -10 H70 V30 H-10 Z"}}, {{"fill", "#0b3d2e"}})};
        clipped.children = {std::move(cut)};
        c.push_back(std::move(clipped));
        return f;
    }

    struct Difference {
        int worst{};
        std::size_t pixels{};
    };

    // Channel differences between two encoded premultiplied BGRA frames.
    Difference difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
        Difference d;
        for (std::size_t i = 0; i < a.size(); i += 4) {
            int worst{};
            for (unsigned c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(int(a[i + c]) - int(b[i + c])));
            }
            d.worst = std::max(d.worst, worst);
            d.pixels += worst > 0;
        }
        return d;
    }

    void save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bgra) {
        std::vector<std::uint8_t> rgba(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            rgba[i] = bgra[i + 2];
            rgba[i + 1] = bgra[i + 1];
            rgba[i + 2] = bgra[i];
            rgba[i + 3] = bgra[i + 3];
        }
        weft::testing::write_png(path, width, height, rgba);
    }

    // A frame drawn with atlas pages against one drawn with a texture for each image.
    void alike(const std::string& what, const std::vector<std::uint8_t>& pages, const std::vector<std::uint8_t>& own,
               const std::filesystem::path& folder) {
        const auto d = difference(pages, own);
        std::printf("%-44s worst %d, %zu px differ\n", what.c_str(), d.worst, d.pixels);
        if (d.worst > max_difference || d.pixels > max_differing_pixels) {
            save(folder / ("unlike-" + what + "-pages.png"), pages);
            save(folder / ("unlike-" + what + "-own.png"), own);
            require(false, what + ": atlas pages unlike textures of their own");
        }
    }
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: weft_render_scene_atlas_tests <artifact folder>");
        const std::filesystem::path folder = argv[1];
        std::filesystem::create_directories(folder);
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM init");
        auto& text = weft::testing::thread_scene_text();
        // Caches of four entries a kind: the images of a frame go once the next frame's pass is drawn, and their
        // regions come back for the frame after.
        weft::testing::D3D11Scene pages{width, height, weft::SceneCacheBounds{4, std::size_t(1) << 30}};
        weft::testing::D3D11Scene own{width, height, weft::SceneCacheBounds{4, std::size_t(1) << 30, 0}};
        const auto first = frame("Alpha", 100);
        const auto drawn = pages.draw(first, scale);
        const auto expected = own.draw(first, scale);
        save(folder / "pages.png", drawn);
        alike("labels, shadow, paths, group, rotated", drawn, expected, folder);
        require(pages.scene.atlas_placements >= 30 && !pages.scene.atlas_refusals && !own.scene.atlas_placements,
                "images on pages: " + std::to_string(pages.scene.atlas_placements));
        require(pages.scene.draws * 3 <= own.scene.draws, "draws: " + std::to_string(pages.scene.draws) +
                                                              " with pages, " + std::to_string(own.scene.draws) +
                                                              " without");
        // Frames of other words: each takes the regions the frame before the last gave back.
        for (const auto* word : {"Beta", "Gamma", "Delta", "Epsilon"}) {
            const auto next = frame(word, 100);
            alike(std::string("regions given back, ") + word, pages.draw(next, scale), own.draw(next, scale), folder);
        }
        pages.gpu.check_debug_messages();
        own.gpu.check_debug_messages();
        std::string vulkan = "skipped";
#ifdef WEFT_TEST_VULKAN
        {
            namespace vk = weft::render::vulkan;
            std::string unavailable;
            if (const auto device = vk::Device::open({}, unavailable)) {
                const auto& d = *device;
                weft::testing::D3D11Scene reference{width, height};
                vk::SceneRenderer scene(*device, text);
                const auto against = [&](const std::string& what, const std::vector<std::uint8_t>& pixels,
                                         const weft::SceneFrame& f) {
                    const auto diff = difference(pixels, reference.draw(f, scale));
                    std::printf("Vulkan %-37s worst %d, %zu px differ\n", what.c_str(), diff.worst, diff.pixels);
                    require(diff.worst <= max_vulkan_difference, "Vulkan " + what + " unlike D3D11");
                };
                const auto one = scene.draw(first, scale, width, height, std::nullopt, 0, {});
                require(one.has_value(), "native Vulkan pass");
                against("labels, shadow, paths, group, rotated", *one, first);
                // Two recordings that copy into one page before either is submitted, the second sampling only what it
                // copies; then a recording that copies into the page and is never submitted, and one that draws its
                // frame again.
                VkCommandPoolCreateInfo pool_info{};
                pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pool_info.queueFamilyIndex = d.queue_family;
                VkCommandPool pool{};
                vk::check(d.vkd.vkCreateCommandPool(d.device, &pool_info, nullptr, &pool), "test command pool");
                std::array<VkCommandBuffer, 2> commands{};
                VkCommandBufferAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                allocate.commandPool = pool;
                allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocate.commandBufferCount = 2;
                vk::check(d.vkd.vkAllocateCommandBuffers(d.device, &allocate, commands.data()), "test commands");
                VkFenceCreateInfo fence_info{};
                fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                VkFence fence{};
                vk::check(d.vkd.vkCreateFence(d.device, &fence_info, nullptr, &fence), "test fence");
                std::array<vk::Image, 2> targets{};
                std::array<vk::Buffer, 2> readbacks{};
                for (unsigned k = 0; k < 2; ++k) {
                    targets[k] = d.image(VK_FORMAT_B8G8R8A8_UNORM, width, height,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                    readbacks[k] = d.buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                }
                std::array<vk::SceneRenderer::Slot, 2> slots;
                // Records frame f into command k's target, ending with its copy into readback k.
                const auto record = [&](unsigned k, const weft::SceneFrame& f) {
                    VkCommandBufferBeginInfo begin{};
                    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vk::check(d.vkd.vkBeginCommandBuffer(commands[k], &begin), "test begin");
                    require(scene.encode(slots[k], f, commands[k], {targets[k].view, width, height, targets[k].image},
                                         scale, std::nullopt, 0, {}),
                            "native Vulkan recording");
                    vk::transition(d, commands[k], targets[k].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy copy{};
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.imageExtent = {width, height, 1};
                    d.vkd.vkCmdCopyImageToBuffer(commands[k], targets[k].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 readbacks[k].buffer, 1, &copy);
                    VkBufferMemoryBarrier host{};
                    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    host.buffer = readbacks[k].buffer;
                    host.size = VK_WHOLE_SIZE;
                    d.vkd.vkCmdPipelineBarrier(commands[k], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                               0, 0, nullptr, 1, &host, 0, nullptr);
                    vk::check(d.vkd.vkEndCommandBuffer(commands[k]), "test end");
                };
                // Submits the commands in order, waits, and reads each target back.
                const auto run = [&](std::initializer_list<unsigned> ks) {
                    std::vector<VkCommandBuffer> submitted;
                    for (const auto k : ks) {
                        submitted.push_back(commands[k]);
                    }
                    VkSubmitInfo submit{};
                    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    submit.commandBufferCount = std::uint32_t(submitted.size());
                    submit.pCommandBuffers = submitted.data();
                    vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence), "test submit");
                    for (const auto k : ks) {
                        scene.submitted(slots[k]);
                    }
                    d.wait(fence);
                    vk::check(d.vkd.vkResetFences(d.device, 1, &fence), "test fence reset");
                    std::vector<std::vector<std::uint8_t>> out;
                    for (const auto k : ks) {
                        scene.retire(slots[k]);
                        const auto* bytes = static_cast<const std::uint8_t*>(readbacks[k].mapped);
                        out.emplace_back(bytes, bytes + std::size_t(width) * height * 4);
                    }
                    return out;
                };
                const auto zeta = frame("Zeta", 300);
                const auto eta = frame("Eta", 400);
                record(0, zeta);
                record(1, eta);
                const auto both = run({0, 1});
                against("two recordings copying into one page", both[0], zeta);
                against("the second of them", both[1], eta);
                const auto theta = frame("Theta", 500);
                record(0, theta);
                vk::check(d.vkd.vkResetCommandBuffer(commands[0], 0), "test reset");
                scene.discard(slots[0]);
                record(0, theta);
                against("again after a recording never submitted", run({0})[0], theta);
                require(d.validation_messages == 0, "Vulkan validation: " + d.first_validation_message);
                for (unsigned k = 0; k < 2; ++k) {
                    d.destroy(targets[k]);
                    d.destroy(readbacks[k]);
                }
                d.vkd.vkDestroyFence(d.device, fence, nullptr);
                d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
                vulkan = d.name;
            } else {
                std::cout << "Vulkan skipped: " << unavailable << '\n';
            }
        }
#endif
        std::cout << "PASS scene atlas pages: as textures of their own in " << pages.scene.draws << " draws instead of "
                  << own.scene.draws << ", regions given back, a rotated label apart from its neighbours; D3D11 WARP, "
                  << "Vulkan " << vulkan << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
