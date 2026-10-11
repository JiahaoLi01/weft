#include "render/backends/vulkan/backend.hpp"
#include "render/paint_list.hpp"
#include "render/quad_list.hpp"
#include "support/check.hpp"
#include "support/render_lists.hpp"

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

// The Vulkan adapter of the renderer C ABI on this machine's Vulkan device (--adapter <name> picks one), driven as a
// host drives it: the host's command buffers and targets, its submissions and completion serials. Rejected payloads
// and failed recordings record nothing; a discarded recording uploads again; two frames stay in flight and each
// retires only with its own completion; preserved, BGRA and float targets; quad frames. No validation layer warning or
// error, with synchronization validation. Exit 77 skips without a Vulkan loader or device.
namespace {
#define CHECK(v) WEFT_REQUIRE(v)

    using namespace weft::testing;
    namespace vk = weft::render::vulkan;
    namespace limits = weft::constants::render::vulkan;
    using weft::render::LifetimeResult;
    using weft::render::ResourceInfo;
    using weft::render::ResourcePhase;

    // Exit code that makes CTest report the test as skipped.
    constexpr int skipped = 77;
    constexpr unsigned width = 96;
    constexpr unsigned height = 64;
    constexpr std::uint64_t queue = limits::queue;
    constexpr std::uint64_t epoch = limits::device_epoch;

    void ok(weft_render_result result, const char* what) {
        if (result != WEFT_RENDER_OK) {
            throw std::runtime_error(std::string(what) + " result=" + std::to_string(result));
        }
    }

    float half_to_float(std::uint16_t h) {
        const unsigned exponent = (h >> 10) & 0x1f;
        const unsigned mantissa = h & 0x3ff;
        const float value = exponent == 0    ? std::ldexp(float(mantissa), -24)
                            : exponent == 31 ? (mantissa ? NAN : INFINITY)
                                             : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
        return h >> 15 ? -value : value;
    }

    // The host: one command buffer and fence per frame slot, color targets with their readback buffers.
    class Host {
    public:
        struct Target {
            vk::Image image;
            vk::Buffer readback;
            std::size_t texel_bytes{};
        };

        Host(vk::Device& device, unsigned slots) : d_(device), commands_(slots), fences_(slots) {
            VkCommandPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = d_.queue_family;
            vk::check(d_.vkd.vkCreateCommandPool(d_.device, &pool, nullptr, &pool_), "host command pool");
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = pool_;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = slots;
            vk::check(d_.vkd.vkAllocateCommandBuffers(d_.device, &allocate, commands_.data()), "host command buffers");
            for (auto& fence : fences_) {
                VkFenceCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                vk::check(d_.vkd.vkCreateFence(d_.device, &info, nullptr, &fence), "host fence");
            }
        }

        ~Host() {
            d_.vkd.vkDeviceWaitIdle(d_.device);
            for (auto& t : targets_) {
                d_.destroy(t->image);
                d_.destroy(t->readback);
            }
            for (auto fence : fences_) {
                d_.vkd.vkDestroyFence(d_.device, fence, nullptr);
            }
            d_.vkd.vkDestroyCommandPool(d_.device, pool_, nullptr);
        }

        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;

        Target& target(VkFormat format, std::size_t texel_bytes) {
            auto t = std::make_unique<Target>();
            t->texel_bytes = texel_bytes;
            t->image = d_.image(format, width, height,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            t->readback = d_.buffer(VkDeviceSize(width) * height * texel_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            targets_.push_back(std::move(t));
            return *targets_.back();
        }

        VkCommandBuffer begin(unsigned slot) {
            VkCommandBufferBeginInfo info{};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk::check(d_.vkd.vkBeginCommandBuffer(commands_[slot], &info), "host begin");
            return commands_[slot];
        }

        // Cancels the slot's recorded commands.
        void reset(unsigned slot) {
            vk::check(d_.vkd.vkResetCommandBuffer(commands_[slot], 0), "host reset");
        }

        // Before the UI: the target as a color attachment, first cleared to host_clear when there is one.
        void prologue(VkCommandBuffer command, Target& t, const VkClearColorValue* host_clear) {
            if (host_clear) {
                barrier(command, t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                d_.vkd.vkCmdClearColorImage(command, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, host_clear, 1,
                                            &range);
                barrier(command, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            } else {
                barrier(command, t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
        }

        // After the UI: the target copied into its readback buffer for the host to read.
        void epilogue(VkCommandBuffer command, Target& t) {
            barrier(command, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            d_.vkd.vkCmdCopyImageToBuffer(command, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          t.readback.buffer, 1, &copy);
            VkBufferMemoryBarrier host{};
            host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = t.readback.buffer;
            host.size = VK_WHOLE_SIZE;
            d_.vkd.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                        nullptr, 1, &host, 0, nullptr);
        }

        void submit(unsigned slot) {
            vk::check(d_.vkd.vkEndCommandBuffer(commands_[slot]), "host end");
            VkSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            info.commandBufferCount = 1;
            info.pCommandBuffers = &commands_[slot];
            vk::check(d_.vkd.vkQueueSubmit(d_.queue, 1, &info, fences_[slot]), "host submit");
        }

        void wait(unsigned slot) {
            d_.wait(fences_[slot]);
            vk::check(d_.vkd.vkResetFences(d_.device, 1, &fences_[slot]), "host fence reset");
        }

        // The pixels of an 8-bit target as RGBA, swizzled when bgra.
        std::vector<Pixel> pixels(const Target& t, bool bgra = false) const {
            std::vector<Pixel> out(std::size_t(width) * height);
            std::memcpy(out.data(), t.readback.mapped, out.size() * sizeof(Pixel));
            if (bgra) {
                for (auto& p : out) {
                    std::swap(p[0], p[2]);
                }
            }
            return out;
        }

        // The texels of a 16-bit float target.
        std::vector<std::array<float, 4>> floats(const Target& t) const {
            std::vector<std::array<float, 4>> out(std::size_t(width) * height);
            const auto* halves = static_cast<const std::uint16_t*>(t.readback.mapped);
            for (std::size_t i = 0; i < out.size(); ++i) {
                for (unsigned c = 0; c < 4; ++c) {
                    out[i][c] = half_to_float(halves[i * 4 + c]);
                }
            }
            return out;
        }

    private:
        void barrier(VkCommandBuffer command, Target& t, VkImageLayout from, VkImageLayout to, VkAccessFlags written,
                     VkAccessFlags read, VkPipelineStageFlags before, VkPipelineStageFlags after) {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = written;
            b.dstAccessMask = read;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t.image.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            d_.vkd.vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        vk::Device& d_;
        VkCommandPool pool_{};
        std::vector<VkCommandBuffer> commands_;
        std::vector<VkFence> fences_;
        std::vector<std::unique_ptr<Target>> targets_;
    };

    weft_paint_list shared(paint::DisplayList list) {
        return {std::make_shared<const paint::DisplayList>(std::move(list))};
    }

    // Every texture of the prepared frame is still only created: no upload was submitted or is pinned by a use.
    bool uploads_pending(vk::Backend& backend) {
        for (const auto* a : backend.prepared->assets) {
            ResourceInfo info;
            if (backend.gpu.tracker.InspectResource(a->id, info) != LifetimeResult::Ok ||
                info.phase != ResourcePhase::Created || info.encoded_pins || info.submitted_pins) {
                return false;
            }
        }
        return true;
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
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--no-synchronization") {
                options.synchronization = false;
            }
        }
        std::string unavailable;
        const auto device = vk::Device::open(options, unavailable);
        if (!device) {
            std::cout << "SKIP Vulkan: " << unavailable << '\n';
            return skipped;
        }
        std::cout << "Vulkan device: " << device->name << ", validation layer "
                  << (device->validation ? device->synchronization ? "on with synchronization validation" : "on"
                                         : "not installed")
                  << '\n';

        // The renderer's own path draws the references, on a renderer of its own.
        vk::Renderer reference(*device, width, height);
        const auto page_expected = reference.draw(page_list());
        const auto groups_expected = reference.draw(groups(true));
        const auto glyphs_expected = reference.draw(glyph_run());

        vk::Renderer gpu(*device, width, height);
        vk::Backend backend(gpu);
        auto* r = backend.renderer;
        Host host(*device, limits::frames_in_flight);
        // A failed check exits at once: the backend may still hold frames, which only a drained renderer may drop.
        try {
            weft_render_info info{};
            info.struct_size = sizeof(info);
            ok(weft_renderer_get_info(r, &info), "info");
            const auto formats = backend.target_formats();
            CHECK(info.identity.api == WEFT_RENDER_API_VULKAN && info.identity.queue == queue &&
                  info.identity.device_epoch == epoch);
            CHECK(info.capabilities.features == unsigned(limits::features) &&
                  info.capabilities.target_formats == formats);
            // Vulkan requires blending on all three; linear 8-bit and 32-bit float targets are not offered.
            CHECK(formats == ((1u << WEFT_RENDER_TARGET_SRGB_RGBA8) | (1u << WEFT_RENDER_TARGET_SRGB_BGRA8) |
                              (1u << WEFT_RENDER_TARGET_LINEAR_RGBA16F)));

            auto prepare = [&](const weft_paint_list& list) {
                const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &list, 0, 0, nullptr};
                weft_render_handle prepared{};
                ok(weft_renderer_prepare_list(r, &input, &prepared), "prepare list");
                return prepared;
            };
            const auto page = shared(page_list());
            const auto page_assets = page.list->assets.size();
            std::uint64_t serial{};
            auto& target = host.target(VK_FORMAT_R8G8B8A8_SRGB, 4);

            // Rejected payloads record nothing and create nothing.
            auto prepared = prepare(page);
            auto command = host.begin(0);
            weft_render_vulkan_pass payload{};
            auto pass = backend.pass(command, target.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
            const auto recordings = gpu.recordings;
            const auto draws = gpu.draws;
            const auto frames = gpu.frames_created;
            weft_render_handle use{};
            auto expect = [&](weft_render_pass p, weft_render_result result, const char* what) {
                const auto actual = weft_renderer_encode(r, prepared, &p, &use);
                if (actual != result || use.renderer_id) {
                    std::cerr << what << ": result " << actual << ", expected " << result << '\n';
                    CHECK(false);
                }
            };
            {
                const auto valid = payload;
                auto p = pass;
                p.native_pass_size = sizeof(payload) - 1;
                expect(p, WEFT_RENDER_INVALID_ARGUMENT, "short payload");
                payload.struct_size = 8;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "payload struct size");
                payload = valid;
                payload.abi_version = WEFT_RENDER_ABI_VERSION + 1;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "payload ABI version");
                payload = valid;
                payload.command_buffer = 0;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "no command buffer");
                payload = valid;
                payload.target_view = 0;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "no target");
                payload = valid;
                payload.reserved = 1;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "reserved field");
                payload = valid;
                payload.clear_target = 2;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "clear flag");
                payload = valid;
                payload.clear_linear_premultiplied[0] = 1;
                payload.clear_linear_premultiplied[3] = .5f;
                expect(pass, WEFT_RENDER_INVALID_ARGUMENT, "clear color not premultiplied");
                payload = valid;
                payload.device += 64;
                expect(pass, WEFT_RENDER_WRONG_BACKEND, "another device");
                payload = valid;
                p = pass;
                p.width = width + 1;
                expect(p, WEFT_RENDER_UNSUPPORTED_FEATURE, "target size");
                p = pass;
                p.target_format = WEFT_RENDER_TARGET_LINEAR_RGBA8;
                expect(p, WEFT_RENDER_UNSUPPORTED_FEATURE, "target format");
                p = pass;
                p.sample_count = 4;
                expect(p, WEFT_RENDER_UNSUPPORTED_FEATURE, "multisampled target");
            }
            CHECK(gpu.recordings == recordings && gpu.draws == draws && gpu.frames_created == frames);

            // A recording that fails records nothing and frees what it created.
            gpu.inject_encode_failure = true;
            expect(pass, WEFT_RENDER_BACKEND_ERROR, "injected failure");
            gpu.inject_encode_failure = false;
            CHECK(gpu.recordings == recordings && gpu.draws == draws && !gpu.uploads &&
                  gpu.frames_created == frames + 1 && gpu.frames_released == gpu.frames_created &&
                  uploads_pending(backend));

            // A discarded recording: the host resets its commands, the frame's resources go at once, and the next
            // recording uploads again.
            ok(weft_renderer_encode(r, prepared, &pass, &use), "encode to discard");
            CHECK(gpu.recordings == recordings + 1 && gpu.uploads == page_assets);
            host.reset(0);
            ok(weft_renderer_discard_encoded(r, use), "discard");
            CHECK(gpu.frames_released == gpu.frames_created && uploads_pending(backend));

            // Two recordings before either is submitted each upload, so the second stays whole when the first is
            // discarded. The frame's snapshot is released before submission; nothing retires before completion.
            auto& second = host.target(VK_FORMAT_R8G8B8A8_SRGB, 4);
            command = host.begin(0);
            host.prologue(command, target, nullptr);
            pass = backend.pass(command, target.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
            weft_render_handle discarded{};
            ok(weft_renderer_encode(r, prepared, &pass, &discarded), "encode the first");
            const auto kept_command = host.begin(1);
            host.prologue(kept_command, second, nullptr);
            pass = backend.pass(kept_command, second.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
            ok(weft_renderer_encode(r, prepared, &pass, &use), "encode the second");
            CHECK(gpu.uploads == 3 * page_assets);
            host.reset(0);
            ok(weft_renderer_discard_encoded(r, discarded), "discard the first");
            host.epilogue(kept_command, second);
            ok(weft_renderer_release_prepared(r, prepared), "release before submission");
            const auto textures = gpu.native_releases;
            host.submit(1);
            ok(weft_renderer_notify_submitted(r, use, queue, ++serial), "submitted");
            CHECK(weft_renderer_discard_encoded(r, use) == WEFT_RENDER_INVALID_STATE);
            CHECK(gpu.native_releases == textures && gpu.frames_released + 1 == gpu.frames_created);
            host.wait(1);
            ok(weft_renderer_notify_completed(r, epoch, queue, serial), "completed");
            CHECK(gpu.native_releases == textures + page_assets && gpu.frames_released == gpu.frames_created);
            CHECK(host.pixels(second) == page_expected);

            // Two frames in flight on two targets; each retires with its own serial.
            const auto groups_list = shared(groups(true));
            const auto glyphs = shared(glyph_run());
            const std::uint64_t first_serial = ++serial;
            const std::uint64_t second_serial = ++serial;
            unsigned slot{};
            for (const auto* frame : {&groups_list, &glyphs}) {
                auto& t = slot ? second : target;
                prepared = prepare(*frame);
                command = host.begin(slot);
                host.prologue(command, t, nullptr);
                pass = backend.pass(command, t.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
                ok(weft_renderer_encode(r, prepared, &pass, &use), "encode in flight");
                host.epilogue(command, t);
                ok(weft_renderer_release_prepared(r, prepared), "release in flight");
                host.submit(slot);
                ok(weft_renderer_notify_submitted(r, use, queue, slot ? second_serial : first_serial),
                   "submit in flight");
                ++slot;
            }
            ok(weft_renderer_get_info(r, &info), "in flight info");
            CHECK(info.submitted_uses == 2 && gpu.frames_created - gpu.frames_released == 2);
            const auto glyph_textures = glyphs.list->assets.size();
            CHECK(gpu.native_creations - gpu.native_releases == glyph_textures);
            host.wait(0);
            ok(weft_renderer_notify_completed(r, epoch, queue, first_serial), "first completed");
            CHECK(gpu.frames_created - gpu.frames_released == 1 &&
                  gpu.native_creations - gpu.native_releases == glyph_textures);
            host.wait(1);
            ok(weft_renderer_notify_completed(r, epoch, queue, second_serial), "second completed");
            CHECK(gpu.frames_created == gpu.frames_released && gpu.native_creations == gpu.native_releases);
            CHECK(host.pixels(target) == groups_expected && host.pixels(second) == glyphs_expected);

            // One frame on two targets: the second recording reuses the first's submitted uploads, and a completion past
            // both serials retires both.
            prepared = prepare(page);
            const auto uploads = gpu.uploads;
            for (unsigned s = 0; s < 2; ++s) {
                auto& t = s ? second : target;
                command = host.begin(s);
                host.prologue(command, t, nullptr);
                pass = backend.pass(command, t.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
                ok(weft_renderer_encode(r, prepared, &pass, &use), "encode twice");
                host.epilogue(command, t);
                host.submit(s);
                ok(weft_renderer_notify_submitted(r, use, queue, ++serial), "submit twice");
            }
            CHECK(gpu.uploads == uploads + page_assets);
            ok(weft_renderer_release_prepared(r, prepared), "release after submission");
            host.wait(0);
            host.wait(1);
            // The host's queue also ran work of its own after the UI's.
            serial += 3;
            ok(weft_renderer_notify_completed(r, epoch, queue, serial), "completion past the UI");
            CHECK(gpu.frames_created == gpu.frames_released && gpu.native_creations == gpu.native_releases);
            CHECK(host.pixels(target) == page_expected && host.pixels(second) == page_expected);

            // The host's red under a half-green box, preserved and cleared: an sRGB, a BGRA and a float target.
            const auto half_green = shared(fill(width, height, {0, 1, 0, .5f}));
            auto& bgra = host.target(VK_FORMAT_B8G8R8A8_SRGB, 4);
            auto& floats = host.target(VK_FORMAT_R16G16B16A16_SFLOAT, 8);
            const VkClearColorValue red{{1, 0, 0, 1}};

            struct Case {
                Host::Target* target;
                weft_render_target_format format;
                bool preserve;
            };

            for (const auto& c :
                 {Case{&target, WEFT_RENDER_TARGET_SRGB_RGBA8, true}, Case{&bgra, WEFT_RENDER_TARGET_SRGB_BGRA8, true},
                  Case{&floats, WEFT_RENDER_TARGET_LINEAR_RGBA16F, true},
                  Case{&floats, WEFT_RENDER_TARGET_LINEAR_RGBA16F, false}}) {
                prepared = prepare(half_green);
                command = host.begin(0);
                host.prologue(command, *c.target, &red);
                pass = backend.pass(command, c.target->image.view, c.format, payload);
                payload.clear_target = c.preserve ? 0 : 1;
                ok(weft_renderer_encode(r, prepared, &pass, &use), "encode over the host's red");
                host.epilogue(command, *c.target);
                ok(weft_renderer_release_prepared(r, prepared), "release over red");
                host.submit(0);
                ok(weft_renderer_notify_submitted(r, use, queue, ++serial), "submit over red");
                host.wait(0);
                ok(weft_renderer_notify_completed(r, epoch, queue, serial), "complete over red");
                if (c.format == WEFT_RENDER_TARGET_LINEAR_RGBA16F) {
                    const std::array<float, 4> expected =
                        c.preserve ? std::array<float, 4>{.5f, .5f, 0, 1} : std::array<float, 4>{0, .5f, 0, .5f};
                    for (const auto& texel : host.floats(*c.target)) {
                        CHECK(texel == expected);
                    }
                } else {
                    // Linear .5 encodes to 187.52, so the hardware's rounding may land on either side.
                    for (const auto& pixel : host.pixels(*c.target, c.format == WEFT_RENDER_TARGET_SRGB_BGRA8)) {
                        CHECK(close_to(pixel, Pixel{188, 188, 0, 255}, 1));
                    }
                }
            }

            // A BGRA target draws the page as the RGBA one does.
            prepared = prepare(page);
            command = host.begin(0);
            host.prologue(command, bgra, nullptr);
            pass = backend.pass(command, bgra.image.view, WEFT_RENDER_TARGET_SRGB_BGRA8, payload);
            ok(weft_renderer_encode(r, prepared, &pass, &use), "encode BGRA");
            host.epilogue(command, bgra);
            ok(weft_renderer_release_prepared(r, prepared), "release BGRA");
            host.submit(0);
            ok(weft_renderer_notify_submitted(r, use, queue, ++serial), "submit BGRA");
            host.wait(0);
            ok(weft_renderer_notify_completed(r, epoch, queue, serial), "complete BGRA");
            CHECK(host.pixels(bgra, true) == page_expected);

            // A quad frame draws as its display list.
            weft::render::FramePacket quads;
            quads.assets.push_back(std::make_shared<const Asset>(
                Asset{4, 4, Encoding::Coverage, {0, 64, 128, 255, 255, 128, 64, 0, 0, 128, 255, 64, 255, 64, 128, 0}}));
            quads.draws.push_back({{0, 0, float(width), float(height)}, {.1f, .15f, .2f, 1}});
            quads.draws.push_back({{6.5f, 4, 40, 36}, {1, .9f, .3f, 1}, 0});
            quads.draws.push_back({{30, 20, 50, 30}, {.3f, .8f, .4f, .6f}});
            const auto quads_expected = reference.draw(weft::render::quad_list(quads, 0, width, height));
            {
                const auto& a = *quads.assets[0];
                const weft_render_asset asset{
                    1, 1, a.width, a.height, WEFT_RENDER_COVERAGE, 0, a.bytes.data(), a.bytes.size(), 0, 0};
                std::vector<weft_render_draw> quad_draws;
                for (const auto& d : quads.draws) {
                    weft_render_draw draw{d.rect.x,  d.rect.y,
                                          d.rect.w,  d.rect.h,
                                          {},        d.asset < 0 ? WEFT_RENDER_SOLID_ASSET : std::uint32_t(d.asset),
                                          d.opacity, {},
                                          d.stroke,  {}};
                    std::copy(d.srgb.begin(), d.srgb.end(), draw.srgb);
                    std::copy(d.uv.begin(), d.uv.end(), draw.uv);
                    quad_draws.push_back(draw);
                }
                const weft_render_frame frame{
                    sizeof(frame), WEFT_RENDER_ABI_VERSION, quad_draws.data(), std::uint32_t(quad_draws.size()), 1,
                    &asset};
                ok(weft_renderer_prepare(r, &frame, &prepared), "prepare quads");
            }
            command = host.begin(0);
            host.prologue(command, target, nullptr);
            pass = backend.pass(command, target.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, payload);
            ok(weft_renderer_encode(r, prepared, &pass, &use), "encode quads");
            host.epilogue(command, target);
            ok(weft_renderer_release_prepared(r, prepared), "release quads");
            host.submit(0);
            ok(weft_renderer_notify_submitted(r, use, queue, ++serial), "submit quads");

            // Destroying waits for the GPU: busy while the quads are in flight, and draining refuses new frames.
            CHECK(weft_renderer_destroy(r) == WEFT_RENDER_BUSY);
            {
                const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &page, 0, 0, nullptr};
                weft_render_handle refused{};
                CHECK(weft_renderer_prepare_list(r, &input, &refused) != WEFT_RENDER_OK && !refused.renderer_id);
            }
            host.wait(0);
            ok(weft_renderer_notify_completed(r, epoch, queue, serial), "complete quads");
            CHECK(host.pixels(target) == quads_expected);
            CHECK(gpu.frames_created == gpu.frames_released && gpu.native_creations == gpu.native_releases);

            // A host that lost its device confirms that the GPU no longer runs the frame; abandoning retires it at once.
            {
                vk::Renderer lost(*device, width, height);
                vk::Backend lost_backend(lost);
                auto* l = lost_backend.renderer;
                const weft_render_list_frame input{sizeof(input), WEFT_RENDER_ABI_VERSION, &page, 0, 0, nullptr};
                weft_render_handle lost_frame{};
                ok(weft_renderer_prepare_list(l, &input, &lost_frame), "prepare before the loss");
                command = host.begin(0);
                host.prologue(command, target, nullptr);
                weft_render_vulkan_pass lost_payload{};
                auto lost_pass =
                    lost_backend.pass(command, target.image.view, WEFT_RENDER_TARGET_SRGB_RGBA8, lost_payload);
                weft_render_handle lost_use{};
                ok(weft_renderer_encode(l, lost_frame, &lost_pass, &lost_use), "encode before the loss");
                host.epilogue(command, target);
                ok(weft_renderer_release_prepared(l, lost_frame), "release before the loss");
                host.submit(0);
                ok(weft_renderer_notify_submitted(l, lost_use, queue, 1), "submit before the loss");
                host.wait(0);
                CHECK(weft_renderer_abandon_device(l, epoch, 0) == WEFT_RENDER_HOST_CONFIRMATION_REQUIRED);
                CHECK(lost.frames_released + 1 == lost.frames_created &&
                      lost.native_creations - lost.native_releases == page_assets);
                ok(weft_renderer_abandon_device(l, epoch, WEFT_RENDER_HOST_CONFIRMED_NO_FURTHER_ACCESS), "abandon");
                CHECK(lost.frames_released == lost.frames_created && lost.native_creations == lost.native_releases);
            }

            if (device->validation_messages) {
                std::cerr << device->validation_messages
                          << " validation messages; first: " << device->first_validation_message << '\n';
            }
            CHECK(device->validation_messages == 0);
            std::cout << "PASS Vulkan C ABI on " << device->name << ": " << serial << " serials, " << gpu.recordings
                      << " recordings, " << gpu.uploads << " uploads, " << gpu.native_releases << " textures and "
                      << gpu.frames_released << " frames retired after completion; validation "
                      << (device->validation ? "clean" : "not installed") << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << '\n';
            std::cerr.flush();
            std::_Exit(1);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
