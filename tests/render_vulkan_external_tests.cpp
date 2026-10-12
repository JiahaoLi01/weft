#include "render/backends/vulkan/backend.hpp"
#include "render/backends/vulkan/pipeline.hpp"
#include "render/paint_list.hpp"
#include "render/srgb.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Host textures on the Vulkan adapter of the renderer C ABI (--adapter <name> picks the device). A host image of each
// linear format it offers, and one whose rows start at the bottom, written by the host's commands earlier in the
// frame's own submission, draws in a display list frame and in a quad frame as exactly its texels; a released lease
// retires only once the frame that samples it completed, and its view imports again afterwards. Payloads of another
// device or malformed, a second import of a view and imports past capacity are refused and transfer nothing. No
// validation layer warning or error, with synchronization validation. Exit 77 skips without a Vulkan device.
namespace {
    using weft::testing::require;
    namespace vk = weft::render::vulkan;
    namespace limits = weft::constants::render::vulkan;
    namespace paint = weft::paint;
    using Linear = std::array<float, 4>;
    using Pixel = std::array<std::uint8_t, 4>;

    constexpr int skipped = 77;
    constexpr unsigned width = 64;
    constexpr unsigned height = 48;
    constexpr unsigned texture_width = 20;
    constexpr unsigned texture_height = 14;

    void ok(weft_render_result result, const char* what) {
        if (result != WEFT_RENDER_OK) {
            throw std::runtime_error(std::string(what) + " result=" + std::to_string(result));
        }
    }

    float half_to_float(std::uint16_t h) {
        const unsigned exponent = (h >> 10) & 0x1f;
        const unsigned mantissa = h & 0x3ff;
        const float value =
            exponent == 0 ? std::ldexp(float(mantissa), -24) : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
        return h >> 15 ? -value : value;
    }

    // The nearest half (ties to even) of a finite non-negative float below 65504.
    std::uint16_t float_to_half(float f) {
        std::uint32_t bits{};
        std::memcpy(&bits, &f, sizeof(bits));
        const int exponent = int((bits >> 23) & 0xff) - 127 + 15;
        std::uint32_t mantissa = bits & 0x7fffff;
        if (exponent <= 0) {
            if (exponent < -10) {
                return 0;
            }
            mantissa |= 0x800000;
            const int shift = 14 - exponent;
            std::uint32_t half = mantissa >> shift;
            const std::uint32_t rest = mantissa & ((1u << shift) - 1);
            const std::uint32_t halfway = 1u << (shift - 1);
            if (rest > halfway || (rest == halfway && (half & 1))) {
                ++half;
            }
            return std::uint16_t(half);
        }
        std::uint32_t half = (std::uint32_t(exponent) << 10) | (mantissa >> 13);
        const std::uint32_t rest = mantissa & 0x1fff;
        if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) {
            ++half;
        }
        return std::uint16_t(half);
    }

    // Texel (x, y) of pattern p as linear premultiplied light: straight sRGB channels and alphas over the whole range,
    // zero alpha among them.
    Linear pattern(unsigned p, unsigned x, unsigned y) {
        const float alpha = std::uint8_t(x * y * 7 + 40 + p * 30) / 255.f;
        const std::uint8_t straight[3]{std::uint8_t(x * 37 + p * 50), std::uint8_t(y * 53 + p * 90),
                                       std::uint8_t((x + y) * 29 + p * 13)};
        Linear out{};
        for (unsigned c = 0; c < 3; ++c) {
            out[c] = weft::render::srgb_to_linear(straight[c] / 255.f) * alpha;
        }
        out[3] = alpha;
        return out;
    }

    // What an sRGB RGBA8 target stores for a linear premultiplied color.
    Pixel encode(const Linear& c) {
        Pixel out{};
        for (unsigned i = 0; i < 4; ++i) {
            const float v = std::clamp(c[i], 0.f, 1.f);
            out[i] = std::uint8_t(std::lround((i == 3 ? v : weft::render::linear_to_srgb(v)) * 255));
        }
        return out;
    }

    struct Format {
        std::uint32_t weft;
        VkFormat vulkan;
        std::size_t texel_bytes;
    };

    constexpr Format formats[]{{WEFT_RENDER_TARGET_LINEAR_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4},
                               {WEFT_RENDER_TARGET_LINEAR_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
                               {WEFT_RENDER_TARGET_LINEAR_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};

    // A host texture: its image, the staging bytes of its texels, and what a frame that draws it upright shows.
    struct HostTexture {
        Format format{};
        bool bottom_left{};
        vk::Image image;
        vk::Buffer staging;
        std::vector<Pixel> expected;
        weft_render_vulkan_texture payload{};
        weft_render_handle lease{};
        unsigned retired{};
        std::uint32_t reason{};
    };

    void on_retired(void* user_data, std::uint32_t reason) noexcept {
        auto& t = *static_cast<HostTexture*>(user_data);
        ++t.retired;
        t.reason = reason;
    }

    // Texture p in format, its rows stored bottom up when bottom_left; expected holds the upright texels as the target
    // stores them.
    HostTexture make_texture(const vk::Device& d, unsigned p, const Format& format, bool bottom_left) {
        HostTexture t;
        t.format = format;
        t.bottom_left = bottom_left;
        t.image = d.image(format.vulkan, texture_width, texture_height,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        t.staging = d.buffer(VkDeviceSize(texture_width) * texture_height * format.texel_bytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        auto* bytes = static_cast<std::uint8_t*>(t.staging.mapped);
        for (unsigned y = 0; y < texture_height; ++y) {
            for (unsigned x = 0; x < texture_width; ++x) {
                const auto v = pattern(p, x, y);
                Linear stored{};
                const auto row = bottom_left ? texture_height - 1 - y : y;
                auto* texel = bytes + (std::size_t(row) * texture_width + x) * format.texel_bytes;
                for (unsigned c = 0; c < 4; ++c) {
                    if (format.vulkan == VK_FORMAT_R8G8B8A8_UNORM) {
                        texel[c] = std::uint8_t(std::lround(v[c] * 255));
                        stored[c] = texel[c] / 255.f;
                    } else if (format.vulkan == VK_FORMAT_R16G16B16A16_SFLOAT) {
                        const auto h = float_to_half(v[c]);
                        std::memcpy(texel + c * 2, &h, 2);
                        stored[c] = half_to_float(h);
                    } else {
                        std::memcpy(texel + c * 4, &v[c], 4);
                        stored[c] = v[c];
                    }
                }
                t.expected.push_back(encode(stored));
            }
        }
        t.payload = {sizeof(t.payload), WEFT_RENDER_EXTERNAL_ABI_VERSION, vk::abi_handle(d.device),
                     vk::abi_handle(t.image.view)};
        return t;
    }

    // Records the copy of t's texels after the earlier frames' reads, leaving the image readable by fragment shaders.
    void produce(const vk::Device& d, VkCommandBuffer command, const HostTexture& t) {
        vk::transition(d, command, t.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {texture_width, texture_height, 1};
        d.vkd.vkCmdCopyBufferToImage(command, t.staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                     &copy);
        vk::transition(d, command, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    // Where texture i draws: three in the top row, the rest below.
    std::array<unsigned, 2> origin(std::size_t i) {
        return {2 + unsigned(i % 3) * (texture_width + 1), 2 + unsigned(i / 3) * (texture_height + 2)};
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
            std::cout << "SKIP Vulkan host textures: " << unavailable << '\n';
            return skipped;
        }
        const auto& d = *device;
        vk::Renderer gpu(*device, width, height);
        vk::Backend backend(gpu);
        auto* r = backend.renderer;
        // A failed check exits at once: a backend that still holds leases or frames may not be destroyed.
        try {
            // The host textures: every offered format, and linear RGBA8 bottom up.
            std::vector<std::unique_ptr<HostTexture>> textures;
            const auto offered = backend.external_formats();
            require((offered & (1u << WEFT_RENDER_TARGET_LINEAR_RGBA8)) &&
                        (offered & (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F)),
                    "linear RGBA8 and RGBA16F host textures are offered");
            for (const auto& f : formats) {
                if (offered & (1u << f.weft)) {
                    textures.push_back(
                        std::make_unique<HostTexture>(make_texture(d, unsigned(textures.size()), f, false)));
                }
            }
            textures.push_back(
                std::make_unique<HostTexture>(make_texture(d, unsigned(textures.size()), formats[0], true)));
            const auto count = std::uint32_t(textures.size());
            // Room for one texture more than the frames draw.
            backend.enable_external(count + 1, std::uint64_t(64) * weft::constants::bytes_per_mib);

            weft_render_info info{};
            info.struct_size = sizeof(info);
            ok(weft_renderer_get_info(r, &info), "info");
            std::uint64_t content = 1;
            const auto describe = [&](HostTexture& t, const weft_render_vulkan_texture& payload) {
                return weft_render_external_texture{sizeof(weft_render_external_texture),
                                                    WEFT_RENDER_EXTERNAL_ABI_VERSION,
                                                    info.identity,
                                                    content++,
                                                    1,
                                                    texture_width,
                                                    texture_height,
                                                    t.format.weft,
                                                    WEFT_RENDER_EXTERNAL_LINEAR_PREMULTIPLIED,
                                                    t.bottom_left ? WEFT_RENDER_EXTERNAL_BOTTOM_LEFT
                                                                  : WEFT_RENDER_EXTERNAL_TOP_LEFT,
                                                    WEFT_RENDER_EXTERNAL_SAME_QUEUE_ORDERED,
                                                    0,
                                                    0,
                                                    &payload,
                                                    sizeof(payload),
                                                    &t,
                                                    on_retired};
            };

            // Refused imports transfer nothing.
            {
                auto& t = *textures[0];
                auto payload = t.payload;
                weft_render_handle refused{};
                payload.device = t.payload.device + 1;
                auto description = describe(t, payload);
                require(weft_renderer_import_texture(r, &description, &refused) == WEFT_RENDER_WRONG_BACKEND,
                        "a texture of another device");
                payload = t.payload;
                payload.image_view = 0;
                description = describe(t, payload);
                require(weft_renderer_import_texture(r, &description, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                        "a payload without a view");
                payload = t.payload;
                payload.abi_version = WEFT_RENDER_EXTERNAL_ABI_VERSION + 1;
                description = describe(t, payload);
                require(weft_renderer_import_texture(r, &description, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                        "a payload of another schema");
                payload = t.payload;
                payload.struct_size = sizeof(payload) - 1;
                description = describe(t, payload);
                require(weft_renderer_import_texture(r, &description, &refused) == WEFT_RENDER_INVALID_ARGUMENT,
                        "a short payload");
                description = describe(t, t.payload);
                description.format = WEFT_RENDER_TARGET_SRGB_RGBA8;
                require(weft_renderer_import_texture(r, &description, &refused) == WEFT_RENDER_UNSUPPORTED_FEATURE,
                        "an encoded format");
                require(!refused.renderer_id && !t.retired && !backend.imported, "refused imports transfer nothing");
            }

            // Every texture imported; a second import of a view is refused, and so is an import past capacity once
            // another texture took the spare slot. An import that no frame used retires when its lease goes.
            const auto import_all = [&] {
                std::vector<weft_render_handle> leases;
                for (auto& t : textures) {
                    const auto description = describe(*t, t->payload);
                    ok(weft_renderer_import_texture(r, &description, &t->lease), "import");
                    leases.push_back(t->lease);
                }
                return leases;
            };
            auto leases = import_all();
            {
                weft_render_handle refused{};
                const auto again = describe(*textures[1], textures[1]->payload);
                require(weft_renderer_import_texture(r, &again, &refused) == WEFT_RENDER_INVALID_STATE &&
                            !refused.renderer_id,
                        "a second import of a view");
                auto extra = make_texture(d, 9, formats[0], false);
                auto past = make_texture(d, 10, formats[0], false);
                const auto spare = describe(extra, extra.payload);
                ok(weft_renderer_import_texture(r, &spare, &extra.lease), "import into the spare slot");
                const auto beyond = describe(past, past.payload);
                require(weft_renderer_import_texture(r, &beyond, &refused) == WEFT_RENDER_BUSY && !refused.renderer_id,
                        "an import past capacity");
                ok(weft_renderer_release_texture(r, extra.lease), "release the unused import");
                require(extra.retired == 1 && !past.retired, "an unused import retires with its lease");
                for (auto* t : {&extra, &past}) {
                    d.destroy(t->image);
                    d.destroy(t->staging);
                }
            }

            // The host's frame resources.
            VkCommandPool pool{};
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
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
            auto target = d.image(VK_FORMAT_R8G8B8A8_SRGB, width, height,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            auto readback = d.buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            std::uint64_t serial{};

            // One frame: the host writes its textures, then the frame draws them, and the host reads the target back. The
            // caller leases go once the frame is prepared; the textures retire with the frame's completion.
            const auto frame = [&](const char* name, auto&& prepare) {
                for (auto& t : textures) {
                    t->retired = 0;
                }
                weft_render_handle prepared{};
                ok(prepare(prepared), name);
                for (auto& t : textures) {
                    ok(weft_renderer_release_texture(r, t->lease), "release lease");
                }
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vk::check(d.vkd.vkBeginCommandBuffer(command, &begin), "begin");
                for (const auto& t : textures) {
                    produce(d, command, *t);
                }
                vk::transition(d, command, target.image, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                               VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                weft_render_vulkan_pass payload{};
                const auto pass = backend.pass(command, target.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
                weft_render_handle use{};
                ok(weft_renderer_encode(r, prepared, &pass, &use), "encode");
                ok(weft_renderer_release_prepared(r, prepared), "release prepared");
                vk::transition(d, command, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                d.vkd.vkCmdCopyImageToBuffer(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             readback.buffer, 1, &copy);
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
                vk::check(d.vkd.vkEndCommandBuffer(command), "end");
                VkSubmitInfo submit{};
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &command;
                vk::check(d.vkd.vkQueueSubmit(d.queue, 1, &submit, fence), "submit");
                ok(weft_renderer_notify_submitted(r, use, limits::queue, ++serial), "submitted");
                for (const auto& t : textures) {
                    require(!t->retired, "a released lease stays until its frame completed");
                }
                d.wait(fence);
                vk::check(d.vkd.vkResetFences(d.device, 1, &fence), "fence reset");
                ok(weft_renderer_notify_completed(r, limits::device_epoch, limits::queue, serial), "completed");
                for (const auto& t : textures) {
                    require(t->retired == 1 && t->reason == WEFT_RENDER_EXTERNAL_RETIRED,
                            "a texture retires once with its frame's completion");
                }
                // Each texture at its place, nothing elsewhere.
                std::vector<Pixel> pixels(std::size_t(width) * height);
                std::memcpy(pixels.data(), readback.mapped, pixels.size() * sizeof(Pixel));
                int worst{};
                std::size_t inside{};
                for (unsigned y = 0; y < height; ++y) {
                    for (unsigned x = 0; x < width; ++x) {
                        Pixel expected{};
                        for (std::size_t i = 0; i < textures.size(); ++i) {
                            const auto [ox, oy] = origin(i);
                            if (x >= ox && x < ox + texture_width && y >= oy && y < oy + texture_height) {
                                expected = textures[i]->expected[std::size_t(y - oy) * texture_width + (x - ox)];
                                ++inside;
                            }
                        }
                        const auto& actual = pixels[std::size_t(y) * width + x];
                        for (unsigned c = 0; c < 4; ++c) {
                            worst = std::max(worst, std::abs(int(actual[c]) - int(expected[c])));
                        }
                    }
                }
                if (worst > 1) {
                    std::cerr << name << ": a pixel differs from the host texels by " << worst << " levels\n";
                }
                require(worst <= 1 && inside == textures.size() * texture_width * texture_height,
                        "every host texture draws as its texels");
            };

            // A display list frame: one image item over each host texture at one device px per texel.
            paint::DisplayList list;
            list.external_textures = count;
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto [ox, oy] = origin(i);
                list.images.push_back({i});
                auto& item = list.items.emplace_back();
                item.kind = paint::Item::Kind::Image;
                item.transform = {1, 0, 0, 1, float(ox), float(oy)};
                item.width = float(texture_width);
                item.height = float(texture_height);
                item.clip = {0, 0, float(width), float(height)};
                item.payload = i;
            }
            const weft_paint_list shared{std::make_shared<const paint::DisplayList>(list)};
            frame("display list", [&](weft_render_handle& prepared) {
                const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &shared, count, 0,
                                                   leases.data()};
                return weft_renderer_prepare_list(r, &input, &prepared);
            });
            require(backend.imported == count + 1 && backend.released == count + 1,
                    "every import released after completion");

            // A quad frame of the same textures, imported again: their views are free once they retired.
            leases = import_all();
            std::vector<weft_render_draw> draws;
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto [ox, oy] = origin(i);
                weft_render_draw draw{};
                draw.x = float(ox);
                draw.y = float(oy);
                draw.width = float(texture_width);
                draw.height = float(texture_height);
                std::fill(std::begin(draw.srgb), std::end(draw.srgb), 1.f);
                draw.asset_index = i;
                draw.opacity = 1;
                draw.uv[2] = 1;
                draw.uv[3] = 1;
                draws.push_back(draw);
            }
            frame("quad frame", [&](weft_render_handle& prepared) {
                const weft_render_frame quads{sizeof(quads), WEFT_RENDER_ABI_VERSION, draws.data(), count, 0, nullptr};
                const weft_render_external_frame input{sizeof(input), WEFT_RENDER_EXTERNAL_ABI_VERSION, quads, count, 0,
                                                       leases.data()};
                return weft_renderer_prepare_external(r, &input, &prepared);
            });
            require(backend.imported == 2 * count + 1 && backend.released == 2 * count + 1,
                    "the second imports released too");

            d.vkd.vkDeviceWaitIdle(d.device);
            for (auto& t : textures) {
                d.destroy(t->image);
                d.destroy(t->staging);
            }
            d.destroy(target);
            d.destroy(readback);
            d.vkd.vkDestroyFence(d.device, fence, nullptr);
            d.vkd.vkDestroyCommandPool(d.device, pool, nullptr);
            if (d.validation_messages) {
                std::cerr << d.first_validation_message << '\n';
            }
            require(!d.validation_messages, "validation");
            std::cout
                << "PASS Vulkan host textures on " << d.name << ": " << count
                << " textures (linear formats, bottom-up rows) in a display list and a quad frame, leases retired "
                   "with their frames, refusals, validation clean\n";
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            std::cerr.flush();
            std::_Exit(1);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
