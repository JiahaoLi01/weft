// A game engine's side of Weft through the public C headers only (include/weft): a runtime and a View that runs the
// HUD page of examples/engine-host; the engine's units move every frame and the engine writes their screen positions
// as anchors; their health goes to the view model "health", damage to hud.damage events; the engine checks what is
// under the pointer (a page element with a pointer cursor takes the click, anything else would go to the game) and
// clicks the end-turn button, then answers the page's call; and the View's frames are drawn over the game's picture,
// on D3D11 (the device's immediate context) or Vulkan (the engine's command buffer), submitted and completed by the
// engine. The last frame is written as a BMP.
//
//   weft_engine_host [--d3d11 | --vulkan] [--warp] [--frames N] [--page <dir>] [--out <file.bmp>]
//
// Exits 0 when the page answered as it should, 77 without the requested device, 1 otherwise.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <weft/render_backend.h>
#include <weft/render_backend_d3d11.h>
#include <weft/render_backend_vulkan.h>
#include <weft/weft.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    constexpr unsigned width = 1280;
    constexpr unsigned height = 720;
    constexpr unsigned units = 12;
    constexpr int skipped = 77;

    struct Failure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    void check(bool ok, const std::string& what) {
        if (!ok) {
            throw Failure(what);
        }
    }

    void check(weft_result result, const char* what) {
        if (result != WEFT_OK) {
            throw Failure(std::string(what) + ": " + weft_result_string(result));
        }
    }

    void check(weft_render_result result, const char* what) {
        if (result != WEFT_RENDER_OK) {
            throw Failure(std::string(what) + ": render result " + std::to_string(result));
        }
    }

    // Frame times in ms: the median, the 99th percentile, the slowest, and the first frame's (its start-up work).
    std::string times(std::vector<double> ms) {
        const double first = ms.front();
        std::sort(ms.begin(), ms.end());
        const auto at = [&](double q) {
            return ms[std::min(ms.size() - 1, std::size_t(q * double(ms.size() - 1) + 0.5))];
        };
        char text[128];
        std::snprintf(text, sizeof text, "p50 %.3f / p99 %.3f / max %.3f ms (first frame %.3f ms)", at(0.5), at(0.99),
                      ms.back(), first);
        return text;
    }

    std::string read(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        check(bool(in), "cannot read " + path);
        return {std::istreambuf_iterator<char>(in), {}};
    }

    // The game's picture under the UI at frame n: a color as the back buffer holds it (sRGB-encoded, opaque).
    std::array<float, 4> picture(unsigned n) {
        const float t = float(n) / 60;
        return {0.15f + 0.1f * std::sin(t), 0.22f, 0.3f + 0.1f * std::cos(t * 0.7f), 1};
    }

    // Where unit i stands at frame n, in the View's px: a circle of its own.
    std::array<float, 2> place(unsigned i, unsigned n) {
        const float t = float(n) / 60;
        const float cx = 120 + float((i * 197) % (width - 300));
        const float cy = 120 + float((i * 113) % (height - 300));
        const float r = 30 + float(i % 4) * 12;
        const float a = t * (0.8f + float(i % 3) * 0.3f) + float(i);
        return {cx + r * std::cos(a), cy + r * std::sin(a)};
    }

    // The engine: its game state and the View it drives.
    struct Engine {
        weft_runtime* runtime{};
        weft_view_handle view{};
        bool ready{};
        unsigned turn = 1;
        unsigned answered{};
        unsigned diagnostics{};
        std::vector<double> advance_ms;
        std::vector<float> health = std::vector<float>(units, 1);

        explicit Engine(const std::string& page) {
            check(weft_runtime_create(nullptr, &runtime), "runtime");
            const weft_view_desc desc{sizeof(desc), 0, float(width), float(height), 1, 0};
            check(weft_view_create(runtime, &desc, &view), "view");
            // A sync function the page calls in the middle of its script: the engine's localization.
            check(weft_view_register_function(
                      runtime, view, "loc.get", 7,
                      [](void*, const void*, size_t, weft_value_writer* result) {
                          static const char title[] = "\"Battle of Weft\"";
                          return weft_value_writer_set(result, WEFT_VALUE_JSON, title, sizeof title - 1);
                      },
                      nullptr),
                  "register loc.get");
            const auto script = read(page + "/hud.js");
            const auto css = read(page + "/hud.css");
            const auto keyframes = read(page + "/hud-keyframes.css");
            const weft_script s{"hud.js", 6, script.data(), script.size()};
            const weft_page_desc desc_page{
                sizeof(desc_page), 0, &s, 1, 0, css.data(), css.size(), keyframes.data(), keyframes.size(), nullptr, 0};
            check(weft_view_load(runtime, view, &desc_page), "load the HUD");
            for (unsigned i = 0; i < units; ++i) {
                const auto spawn =
                    "{\"id\":" + std::to_string(i + 1) + ",\"name\":\"Unit " + std::to_string(i + 1) + "\"}";
                check(weft_view_emit(runtime, view, "hud.spawn", 9, WEFT_VALUE_JSON, spawn.data(), spawn.size(), 0),
                      "spawn");
            }
        }

        ~Engine() {
            weft_view_destroy(runtime, view);
            weft_runtime_destroy(runtime);
        }

        // One frame of the game: units move (anchors), now and then one takes damage (model and event), and at frame
        // 30 the player clicks the end-turn button; then the View advances and the engine answers the page.
        void frame(unsigned n) {
            std::vector<weft_anchor> anchors(units);
            for (unsigned i = 0; i < units; ++i) {
                const auto [x, y] = place(i, n);
                anchors[i] = {i + 1, x, y, 1, y, 0, 0};
            }
            check(weft_view_write_anchors(runtime, view, anchors.data(), anchors.size()), "anchors");
            if (n % 15 == 7) {
                const unsigned i = (n / 15) % units;
                health[i] = health[i] > 0.2f ? health[i] - 0.15f : 1;
                const auto fields = "{\"" + std::to_string(i + 1) + "\":" + std::to_string(health[i]) + "}";
                check(weft_view_set_model(runtime, view, "health", 6, WEFT_VALUE_JSON, fields.data(), fields.size()),
                      "health");
                const auto damage = "{\"id\":" + std::to_string(i + 1) + ",\"amount\":15}";
                check(weft_view_emit(runtime, view, "hud.damage", 10, WEFT_VALUE_JSON, damage.data(), damage.size(), 0),
                      "damage");
            }
            if (n == 30) {
                click(1200, 680);
            }
            const auto start = std::chrono::steady_clock::now();
            check(weft_view_advance(runtime, view, n / 60.0), "advance");
            advance_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            check(weft_view_drain_messages(runtime, view, &Engine::message, this), "messages");
            check(weft_view_drain_diagnostics(
                      runtime, view,
                      [](void* user, weft_view_handle, const weft_diagnostic* d) {
                          std::fprintf(stderr, "page: %.*s\n", int(d->message_size), d->message);
                          ++static_cast<Engine*>(user)->diagnostics;
                      },
                      this, nullptr),
                  "diagnostics");
        }

        // The pointer goes to the page only over something on it that shows a pointer cursor.
        void click(float x, float y) {
            weft_hit hit{};
            hit.struct_size = sizeof(hit);
            check(weft_view_hit_test(runtime, view, x, y, &hit), "hit test");
            check(hit.element && std::string(hit.cursor) == "pointer", "the end-turn button under the pointer");
            weft_hit empty{};
            empty.struct_size = sizeof(empty);
            check(weft_view_hit_test(runtime, view, 5, 5, &empty), "hit test");
            check(!empty.element, "nothing of the page in the corner: a click there is the game's");
            for (const auto kind : {WEFT_POINTER_MOVE, WEFT_POINTER_DOWN, WEFT_POINTER_UP}) {
                const weft_pointer_event e{sizeof(e), uint32_t(kind), x, y, kind == WEFT_POINTER_DOWN ? 1u : 0u, 0, 0,
                                           0};
                check(weft_view_pointer(runtime, view, &e, nullptr), "pointer");
            }
        }

        static void message(void* user, weft_view_handle view, const weft_message* m) {
            auto& engine = *static_cast<Engine*>(user);
            const std::string name(m->name, m->name_size);
            if (m->kind == WEFT_MESSAGE_SEND && name == "hud.ready") {
                engine.ready = true;
            } else if (m->kind == WEFT_MESSAGE_CALL && name == "hud.endTurn") {
                // The engine answers at once here; it could answer later, from any thread.
                ++engine.turn;
                const auto answer = "{\"turn\":" + std::to_string(engine.turn) + "}";
                check(weft_view_complete_call(engine.runtime, view, m->call_id, 1, WEFT_VALUE_JSON, answer.data(),
                                              answer.size()),
                      "complete the call");
                ++engine.answered;
            }
        }
    };

    weft_render_handle prepare(weft_renderer* renderer, const weft_view_frame* frame) {
        const weft_render_scene_frame f{sizeof(f), WEFT_RENDER_ABI_VERSION, frame, 0, 0};
        weft_render_handle prepared{};
        check(weft_renderer_prepare_scene(renderer, &f, &prepared), "prepare the View's frame");
        return prepared;
    }

    // A 32-bit BMP of BGRA rows, top down.
    void write_bmp(const std::string& path, const std::vector<std::uint8_t>& bgra) {
        if (const auto folder = std::filesystem::path(path).parent_path(); !folder.empty()) {
            std::filesystem::create_directories(folder);
        }
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = LONG(width);
        info.biHeight = -LONG(height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = DWORD(file.bfOffBits + bgra.size());
        std::ofstream out(path, std::ios::binary);
        check(bool(out), "cannot write " + path);
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info), sizeof(info));
        out.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
    }

    // D3D11: the engine's device and back buffer; the View's frames drawn on the immediate context over the game.
    int run_d3d11(Engine& engine, unsigned frames, bool warp, const std::string& out) {
        ID3D11Device* device{};
        ID3D11DeviceContext* context{};
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &device, nullptr,
                                     &context))) {
            std::fprintf(stderr, "no D3D11 device\n");
            return skipped;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* back{};
        ID3D11RenderTargetView* rtv{};
        check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &back)) &&
                  SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &rtv)),
              "back buffer");
        constexpr std::uint64_t queue = 1;
        const weft_render_d3d11_desc backend_desc{sizeof(backend_desc), WEFT_RENDER_ABI_VERSION, device, queue, 0};
        weft_render_d3d11* backend{};
        weft_render_backend table{};
        check(weft_render_d3d11_create(&backend_desc, &backend, &table), "the D3D11 backend");
        weft_renderer* renderer{};
        check(weft_renderer_create(&table, nullptr, &renderer), "renderer");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        check(weft_renderer_get_info(renderer, &info), "renderer info");
        // The engine's completion marker after each frame's work.
        D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
        ID3D11Query* done{};
        check(SUCCEEDED(device->CreateQuery(&query_desc, &done)), "query");
        std::vector<double> encode_ms;
        for (unsigned n = 0; n < frames; ++n) {
            engine.frame(n);
            const auto game = picture(n);
            context->ClearRenderTargetView(rtv, game.data());
            weft_view_frame* frame{};
            check(weft_view_acquire_frame(engine.runtime, engine.view, &frame), "acquire the View's frame");
            const auto prepared = prepare(renderer, frame);
            weft_view_frame_release(frame);
            weft_render_d3d11_pass payload{sizeof(payload), WEFT_RENDER_ABI_VERSION, nullptr, rtv, nullptr, 0, 0, {}};
            const weft_render_pass pass{
                sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, width, height, WEFT_RENDER_TARGET_LINEAR_BGRA8, 1,
                &payload,     sizeof(payload)};
            weft_render_handle use{};
            const auto start = std::chrono::steady_clock::now();
            check(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
            encode_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            check(weft_renderer_release_prepared(renderer, prepared), "release");
            context->End(done);
            context->Flush();
            check(weft_renderer_notify_submitted(renderer, use, queue, n + 1), "submitted");
            while (context->GetData(done, nullptr, 0, 0) == S_FALSE) {
                Sleep(0);
            }
            check(weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, n + 1), "completed");
        }
        // The last frame back to the CPU.
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging{};
        check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "staging");
        context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), "map");
        std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4);
        for (unsigned y = 0; y < height; ++y) {
            std::memcpy(pixels.data() + std::size_t(y) * width * 4,
                        static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, width * 4);
        }
        context->Unmap(staging, 0);
        if (!out.empty()) {
            write_bmp(out, pixels);
        }
        check(weft_renderer_destroy(renderer), "destroy the renderer");
        weft_render_d3d11_destroy(backend);
        staging->Release();
        done->Release();
        rtv->Release();
        back->Release();
        context->Release();
        device->Release();
        std::printf("D3D11%s: %u frames, encode %s\n", warp ? " (WARP)" : "", frames, times(encode_ms).c_str());
        return 0;
    }

    // Vulkan: the engine's instance, device, back buffer and command buffer, through the system loader.
    struct Vulkan {
        HMODULE library{};
        PFN_vkGetInstanceProcAddr get{};
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice device{};
        VkQueue queue{};
        std::uint32_t family{};
#define WEFT_SAMPLE_FUNCTIONS(F)                                                                                       \
    F(vkDestroyInstance)                                                                                               \
    F(vkEnumeratePhysicalDevices)                                                                                      \
    F(vkGetPhysicalDeviceProperties)                                                                                   \
    F(vkGetPhysicalDeviceQueueFamilyProperties) F(vkGetPhysicalDeviceMemoryProperties) F(vkCreateDevice)               \
        F(vkGetDeviceProcAddr)
#define WEFT_SAMPLE_DEVICE_FUNCTIONS(F)                                                                                \
    F(vkDestroyDevice)                                                                                                 \
    F(vkGetDeviceQueue)                                                                                                \
    F(vkCreateImage)                                                                                                   \
    F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) F(vkBindImageMemory)         \
        F(vkCreateImageView) F(vkDestroyImageView) F(vkCreateBuffer) F(vkDestroyBuffer)                                \
            F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory) F(vkMapMemory) F(vkCreateCommandPool)               \
                F(vkDestroyCommandPool) F(vkAllocateCommandBuffers) F(vkBeginCommandBuffer) F(vkEndCommandBuffer)      \
                    F(vkResetCommandBuffer) F(vkCmdPipelineBarrier) F(vkCmdClearColorImage) F(vkCmdCopyImageToBuffer)  \
                        F(vkQueueSubmit) F(vkCreateFence) F(vkDestroyFence) F(vkWaitForFences) F(vkResetFences)        \
                            F(vkDeviceWaitIdle)
#define WEFT_SAMPLE_DECLARE(name) PFN_##name name{};
        WEFT_SAMPLE_FUNCTIONS(WEFT_SAMPLE_DECLARE)
        WEFT_SAMPLE_DEVICE_FUNCTIONS(WEFT_SAMPLE_DECLARE)
#undef WEFT_SAMPLE_DECLARE
        VkPhysicalDeviceMemoryProperties memory{};

        bool open() {
            library = LoadLibraryA("vulkan-1.dll");
            if (!library) {
                return false;
            }
            get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr")));
            if (!get) {
                return false;
            }
            const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
            VkApplicationInfo app{};
            app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app.pApplicationName = "weft_engine_host";
            app.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            info.pApplicationInfo = &app;
            if (!create || create(&info, nullptr, &instance) != VK_SUCCESS) {
                return false;
            }
#define WEFT_SAMPLE_LOAD(name) name = reinterpret_cast<PFN_##name>(get(instance, #name));
            WEFT_SAMPLE_FUNCTIONS(WEFT_SAMPLE_LOAD)
#undef WEFT_SAMPLE_LOAD
            std::uint32_t count{};
            vkEnumeratePhysicalDevices(instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(instance, &count, devices.data());
            for (const auto candidate : devices) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                std::uint32_t families{};
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
                std::vector<VkQueueFamilyProperties> queues(families);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
                for (std::uint32_t i = 0; i < families; ++i) {
                    if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && properties.apiVersion >= VK_API_VERSION_1_1 &&
                        (!physical || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
                        physical = candidate;
                        family = i;
                        break;
                    }
                }
            }
            if (!physical) {
                return false;
            }
            vkGetPhysicalDeviceMemoryProperties(physical, &memory);
            const float priority = 1;
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;
            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;
            if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) {
                return false;
            }
#define WEFT_SAMPLE_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
            WEFT_SAMPLE_DEVICE_FUNCTIONS(WEFT_SAMPLE_LOAD)
#undef WEFT_SAMPLE_LOAD
            vkGetDeviceQueue(device, family, 0, &queue);
            return true;
        }

        ~Vulkan() {
            if (device) {
                vkDestroyDevice(device, nullptr);
            }
            if (instance) {
                vkDestroyInstance(instance, nullptr);
            }
            if (library) {
                FreeLibrary(library);
            }
        }

        std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                    return i;
                }
            }
            throw Failure("no memory type");
        }

        void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags written, VkAccessFlags read, VkPipelineStageFlags before,
                     VkPipelineStageFlags after) const {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = written;
            b.dstAccessMask = read;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(command, before, after, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
    };

    std::uint64_t u64(const void* handle) {
        return std::uint64_t(reinterpret_cast<std::uintptr_t>(handle));
    }

    int run_vulkan(Engine& engine, unsigned frames, const std::string& out) {
        Vulkan vk;
        if (!vk.open()) {
            std::fprintf(stderr, "no Vulkan 1.1 device with a graphics queue\n");
            return skipped;
        }
        // The back buffer (B8G8R8A8_UNORM, drawn into, cleared, read back) and the readback buffer.
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage back{};
        check(vk.vkCreateImage(vk.device, &image_info, nullptr, &back) == VK_SUCCESS, "back buffer");
        VkMemoryRequirements needs{};
        vk.vkGetImageMemoryRequirements(vk.device, back, &needs);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk.memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory back_memory{};
        check(vk.vkAllocateMemory(vk.device, &allocate, nullptr, &back_memory) == VK_SUCCESS &&
                  vk.vkBindImageMemory(vk.device, back, back_memory, 0) == VK_SUCCESS,
              "back buffer memory");
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = back;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView back_view{};
        check(vk.vkCreateImageView(vk.device, &view_info, nullptr, &back_view) == VK_SUCCESS, "back buffer view");
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = VkDeviceSize(width) * height * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer readback{};
        check(vk.vkCreateBuffer(vk.device, &buffer_info, nullptr, &readback) == VK_SUCCESS, "readback");
        vk.vkGetBufferMemoryRequirements(vk.device, readback, &needs);
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = vk.memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory readback_memory{};
        void* mapped{};
        check(vk.vkAllocateMemory(vk.device, &allocate, nullptr, &readback_memory) == VK_SUCCESS &&
                  vk.vkBindBufferMemory(vk.device, readback, readback_memory, 0) == VK_SUCCESS &&
                  vk.vkMapMemory(vk.device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS,
              "readback memory");
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = vk.family;
        VkCommandPool pool{};
        check(vk.vkCreateCommandPool(vk.device, &pool_info, nullptr, &pool) == VK_SUCCESS, "command pool");
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer command{};
        check(vk.vkAllocateCommandBuffers(vk.device, &command_info, &command) == VK_SUCCESS, "command buffer");
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence{};
        check(vk.vkCreateFence(vk.device, &fence_info, nullptr, &fence) == VK_SUCCESS, "fence");

        constexpr std::uint64_t queue = 1;
        const weft_render_vulkan_desc backend_desc{sizeof(backend_desc),
                                                   WEFT_RENDER_ABI_VERSION,
                                                   u64(vk.instance),
                                                   u64(vk.physical),
                                                   u64(vk.device),
                                                   u64(vk.queue),
                                                   vk.family,
                                                   0,
                                                   reinterpret_cast<void*>(vk.get),
                                                   queue};
        weft_render_vulkan* backend{};
        weft_render_backend table{};
        check(weft_render_vulkan_create(&backend_desc, &backend, &table), "the Vulkan backend");
        weft_renderer* renderer{};
        check(weft_renderer_create(&table, nullptr, &renderer), "renderer");
        weft_render_info info{};
        info.struct_size = sizeof(info);
        check(weft_renderer_get_info(renderer, &info), "renderer info");
        std::vector<double> encode_ms;
        for (unsigned n = 0; n < frames; ++n) {
            engine.frame(n);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vk.vkBeginCommandBuffer(command, &begin) == VK_SUCCESS, "begin");
            // The game's picture, then the UI over it as a color attachment.
            vk.barrier(command, back, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const auto game = picture(n);
            const VkClearColorValue clear{{game[0], game[1], game[2], game[3]}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdClearColorImage(command, back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            vk.barrier(command, back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            weft_view_frame* frame{};
            check(weft_view_acquire_frame(engine.runtime, engine.view, &frame), "acquire the View's frame");
            const auto prepared = prepare(renderer, frame);
            weft_view_frame_release(frame);
            weft_render_vulkan_pass payload{
                sizeof(payload), WEFT_RENDER_ABI_VERSION, u64(vk.device), u64(command), u64(back_view), 0, 0, {},
                u64(back)};
            const weft_render_pass pass{
                sizeof(pass), WEFT_RENDER_ABI_VERSION, info.identity, width, height, WEFT_RENDER_TARGET_LINEAR_BGRA8, 1,
                &payload,     sizeof(payload)};
            weft_render_handle use{};
            const auto start = std::chrono::steady_clock::now();
            check(weft_renderer_encode(renderer, prepared, &pass, &use), "encode");
            encode_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            check(weft_renderer_release_prepared(renderer, prepared), "release");
            // The last frame back to the CPU.
            vk.barrier(command, back, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vk.vkCmdCopyImageToBuffer(command, back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
            check(vk.vkEndCommandBuffer(command) == VK_SUCCESS, "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            check(vk.vkQueueSubmit(vk.queue, 1, &submit, fence) == VK_SUCCESS, "submit");
            check(weft_renderer_notify_submitted(renderer, use, queue, n + 1), "submitted");
            check(vk.vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS, "wait");
            check(vk.vkResetFences(vk.device, 1, &fence) == VK_SUCCESS, "reset fence");
            check(weft_renderer_notify_completed(renderer, info.identity.device_epoch, queue, n + 1), "completed");
        }
        if (!out.empty()) {
            const auto* bytes = static_cast<const std::uint8_t*>(mapped);
            write_bmp(out, std::vector<std::uint8_t>(bytes, bytes + std::size_t(width) * height * 4));
        }
        check(weft_renderer_destroy(renderer), "destroy the renderer");
        weft_render_vulkan_destroy(backend);
        vk.vkDeviceWaitIdle(vk.device);
        vk.vkDestroyFence(vk.device, fence, nullptr);
        vk.vkDestroyCommandPool(vk.device, pool, nullptr);
        vk.vkDestroyBuffer(vk.device, readback, nullptr);
        vk.vkFreeMemory(vk.device, readback_memory, nullptr);
        vk.vkDestroyImageView(vk.device, back_view, nullptr);
        vk.vkDestroyImage(vk.device, back, nullptr);
        vk.vkFreeMemory(vk.device, back_memory, nullptr);
        std::printf("Vulkan: %u frames, encode %s\n", frames, times(encode_ms).c_str());
        return 0;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        bool vulkan{};
        bool warp{};
        unsigned frames = 120;
        std::string page = WEFT_SAMPLE_PAGE;
        std::string out;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--vulkan") {
                vulkan = true;
            } else if (a == "--d3d11") {
                vulkan = false;
            } else if (a == "--warp") {
                warp = true;
            } else if (a == "--frames" && i + 1 < argc) {
                frames = unsigned(std::stoul(argv[++i]));
            } else if (a == "--page" && i + 1 < argc) {
                page = argv[++i];
            } else if (a == "--out" && i + 1 < argc) {
                out = argv[++i];
            } else {
                std::fprintf(stderr, "usage: weft_engine_host [--d3d11 | --vulkan] [--warp] [--frames N] [--page dir] "
                                     "[--out file.bmp]\n");
                return 2;
            }
        }
        check(frames > 30, "more than 30 frames (the click is at frame 30)");
        Engine engine(page);
        const int result = vulkan ? run_vulkan(engine, frames, out) : run_d3d11(engine, frames, warp, out);
        if (result != 0) {
            return result;
        }
        check(engine.ready, "the page's hud.ready message");
        check(engine.answered == 1 && engine.turn == 2, "one end-turn call, answered");
        check(engine.diagnostics == 0, "no page diagnostics");
        std::printf("PASS engine host: the page ready, %u call answered; View advance %s\n", engine.answered,
                    times(engine.advance_ms).c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "engine host failed: %s\n", e.what());
        return 1;
    }
}
