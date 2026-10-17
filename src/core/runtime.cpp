#include <weft/weft.h>
#include <base/generational_slots.hpp>
#include <base/owner_thread.hpp>
#include <bridge/bridge_value.hpp>
#include <constants/bridge.hpp>
#include <constants/core.hpp>
#include <constants/view.hpp>
#include <scene/scene_text.hpp>
#include <view/view.hpp>
#include <view/view_abi.hpp>
#if defined(_WIN32)
#include <platform/win32/system_fonts.hpp>
#elif defined(__ANDROID__)
#include <platform/android/system_fonts.hpp>
#endif

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    std::atomic<uint64_t> next_runtime_id{1};

    // A sync engine function of the C ABI, kept until the View exists.
    struct HostFunction {
        std::string name;
        weft_function_fn fn{};
        void* user{};
    };

    struct ViewEntry {
        weft_view_info info{};
        // What other threads reach; the View, once a page loaded.
        std::shared_ptr<weft::ViewShared> shared;
        std::unique_ptr<weft::View> view;
        std::vector<HostFunction> functions;
        // The host's reader of the files pages name; none until it sets one.
        weft_resource_fn reader{};
        void* reader_user{};
        // A host callback (a message drain or a sync function) runs: owner operations on the View wait.
        bool busy{};
    };

    // Marks an entry busy while a host callback runs.
    struct Busy {
        ViewEntry& entry;

        explicit Busy(ViewEntry& e) : entry(e) {
            entry.busy = true;
        }

        ~Busy() {
            entry.busy = false;
        }

        Busy(const Busy&) = delete;
        Busy& operator=(const Busy&) = delete;
    };

    static_assert(sizeof(weft_anchor) == sizeof(weft::SceneAnchor) &&
                  offsetof(weft_anchor, key) == offsetof(weft::SceneAnchor, key) &&
                  offsetof(weft_anchor, x) == offsetof(weft::SceneAnchor, x) &&
                  offsetof(weft_anchor, y) == offsetof(weft::SceneAnchor, y) &&
                  offsetof(weft_anchor, scale) == offsetof(weft::SceneAnchor, scale) &&
                  offsetof(weft_anchor, depth) == offsetof(weft::SceneAnchor, depth) &&
                  offsetof(weft_anchor, flags) == offsetof(weft::SceneAnchor, flags) &&
                  offsetof(weft_anchor, reserved) == offsetof(weft::SceneAnchor, reserved));
    static_assert(WEFT_ANCHOR_HIDDEN == weft::scene_anchor_flags::hidden &&
                  WEFT_ANCHOR_BEHIND == weft::scene_anchor_flags::behind &&
                  WEFT_ANCHOR_OFFSCREEN == weft::scene_anchor_flags::offscreen);
} // namespace

struct weft_runtime {
    weft::OwnerThread owner;
    uint64_t id{};
    uint32_t live_views{};
    // View handles carry slot + 1, so a zeroed handle is never valid.
    weft::GenerationalSlots<ViewEntry> views;
    // Each slot's generation and shared part, for the calls of other threads.
    std::mutex shared_mutex;
    std::vector<std::pair<uint32_t, std::shared_ptr<weft::ViewShared>>> shared;
    // The fonts the host lists until the first page loads; then frozen, with the owner's SceneText.
    bool system_fonts{};
    weft::FontCatalog catalog;
    std::shared_ptr<const weft::FontCatalog> fonts;
    std::unique_ptr<weft::SceneText> text;
};

struct weft_value_writer {
    std::vector<std::uint8_t> value;
};

struct weft_resource_writer {
    std::vector<std::uint8_t> bytes;
    bool written{};
};

namespace {
    weft_result validate_owner(weft_runtime* runtime) noexcept {
        if (!runtime) {
            return WEFT_INVALID_ARGUMENT;
        }
        return runtime->owner.is_current() ? WEFT_OK : WEFT_WRONG_THREAD;
    }

    weft_result find_entry(weft_runtime* runtime, weft_view_handle handle, ViewEntry*& out) noexcept {
        out = nullptr;
        if (const auto error = validate_owner(runtime); error != WEFT_OK) {
            return error;
        }
        if (handle.runtime_id != runtime->id || handle.slot == 0) {
            return WEFT_STALE_HANDLE;
        }
        out = runtime->views.find(handle.slot - 1, handle.generation);
        return out ? WEFT_OK : WEFT_STALE_HANDLE;
    }

    // An owner operation on a View that no host callback of it is in.
    weft_result find_idle(weft_runtime* runtime, weft_view_handle handle, ViewEntry*& out) noexcept {
        const auto error = find_entry(runtime, handle, out);
        return error != WEFT_OK ? error : out->busy ? WEFT_BUSY : WEFT_OK;
    }

    // The shared part of a View for a call of any thread; null when the handle is stale.
    std::shared_ptr<weft::ViewShared> find_shared(weft_runtime* runtime, weft_view_handle handle) {
        if (!runtime || handle.runtime_id != runtime->id || handle.slot == 0) {
            return nullptr;
        }
        const std::lock_guard lock(runtime->shared_mutex);
        const auto index = std::size_t(handle.slot - 1);
        if (index >= runtime->shared.size() || runtime->shared[index].first != handle.generation) {
            return nullptr;
        }
        return runtime->shared[index].second;
    }

    bool valid_view_desc(const weft_view_desc& desc) noexcept {
        return desc.struct_size >= sizeof(weft_view_desc) && desc.reserved == 0 && desc.reserved2 == 0 &&
               std::isfinite(desc.logical_width) && desc.logical_width > 0 && std::isfinite(desc.logical_height) &&
               desc.logical_height > 0 && std::isfinite(desc.device_scale) && desc.device_scale > 0;
    }

    // Text the host passes: null only when empty.
    bool text_arg(const char* data, size_t size, std::string_view& out) noexcept {
        if (!data && size) {
            return false;
        }
        out = size ? std::string_view(data, size) : std::string_view{};
        return true;
    }

    // What a View's C++ code threw, as a result.
    weft_result failure(const std::exception& e) noexcept {
        if (dynamic_cast<const std::bad_alloc*>(&e)) {
            return WEFT_OUT_OF_MEMORY;
        }
        if (dynamic_cast<const weft::ViewError*>(&e)) {
            return std::string_view(e.what()).starts_with("WEFT_VIEW_FAULTED") ? WEFT_FAULTED : WEFT_SCRIPT_ERROR;
        }
        if (dynamic_cast<const std::invalid_argument*>(&e)) {
            return WEFT_INVALID_ARGUMENT;
        }
        // A sync function or layout entered the View.
        const std::string_view what = e.what();
        if (what.starts_with("WEFT_ENGINE_REENTRY") || what.starts_with("WEFT_LAYOUT_REENTRY")) {
            return WEFT_BUSY;
        }
        return WEFT_INTERNAL_ERROR;
    }

    template <class Action>
    weft_result guarded(Action&& action) noexcept {
        try {
            return action();
        } catch (const std::exception& e) {
            return failure(e);
        } catch (...) {
            return WEFT_INTERNAL_ERROR;
        }
    }

    weft_result channel_result(weft::bridge::ChannelStatus status) noexcept {
        using weft::bridge::ChannelStatus;
        switch (status) {
            case ChannelStatus::Ok:
                return WEFT_OK;
            case ChannelStatus::Invalid:
                return WEFT_INVALID_ARGUMENT;
            case ChannelStatus::Full:
                return WEFT_CAPACITY_EXCEEDED;
            case ChannelStatus::Stale:
                return WEFT_STALE_HANDLE;
        }
        return WEFT_INTERNAL_ERROR;
    }

    // The runtime's fonts, frozen with the owner's SceneText at the first page.
    void freeze_fonts(weft_runtime& runtime) {
        if (runtime.text) {
            return;
        }
        weft::FontCatalog catalog;
        if (runtime.system_fonts) {
#if defined(_WIN32)
            catalog = weft::platform::win32::system_font_catalog();
#elif defined(__ANDROID__)
            catalog = weft::platform::android::system_font_catalog();
#endif
        }
        catalog.faces.insert(catalog.faces.end(), runtime.catalog.faces.begin(), runtime.catalog.faces.end());
        for (const auto& [generic, family] : runtime.catalog.generic_families) {
            std::erase_if(catalog.generic_families, [&](const auto& g) {
                return g.first == generic;
            });
            catalog.generic_families.emplace_back(generic, family);
        }
        catalog.fallback_families.insert(catalog.fallback_families.end(), runtime.catalog.fallback_families.begin(),
                                         runtime.catalog.fallback_families.end());
        auto fonts = std::make_shared<const weft::FontCatalog>(std::move(catalog));
        runtime.text = std::make_unique<weft::SceneText>(*fonts);
        runtime.fonts = std::move(fonts);
    }

    // The C function of an engine function, with the View busy while it runs.
    weft::EngineFunction engine_function(ViewEntry& entry, HostFunction function) {
        return [&entry, function](std::span<const std::uint8_t> argument) {
            weft_value_writer writer;
            weft_result result{};
            {
                const Busy busy(entry);
                result = function.fn(function.user, argument.data(), argument.size(), &writer);
            }
            if (result != WEFT_OK) {
                throw std::runtime_error("WEFT_ENGINE_FUNCTION: " + function.name + " returned " +
                                         weft_result_string(result));
            }
            return std::move(writer.value);
        };
    }

    // The View's reader of the files its pages name: the host's, while it has one.
    weft::ViewResourceReader resource_reader(ViewEntry& entry) {
        if (!entry.reader) {
            return {};
        }
        return [&entry](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            weft_resource_writer file;
            weft_result result{};
            {
                const Busy busy(entry);
                result = entry.reader(entry.reader_user, path.data(), path.size(), &file);
            }
            if (result != WEFT_OK || !file.written) {
                return std::nullopt;
            }
            return std::move(file.bytes);
        };
    }

    // Fonts can change until the runtime's first page.
    weft_result font_owner(weft_runtime* runtime) noexcept {
        if (const auto error = validate_owner(runtime); error != WEFT_OK) {
            return error;
        }
        return runtime->text ? WEFT_BUSY : WEFT_OK;
    }
} // namespace

extern "C" weft_result weft_runtime_create(const weft_runtime_desc* desc, weft_runtime** out_runtime) {
    if (!out_runtime) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_runtime = nullptr;
    uint32_t max_views = weft::constants::core::default_max_views;
    uint32_t flags = WEFT_RUNTIME_SYSTEM_FONTS;
    if (desc) {
        if (desc->struct_size < sizeof(weft_runtime_desc)) {
            return WEFT_INVALID_ARGUMENT;
        }
        if (desc->abi_version != WEFT_ABI_VERSION) {
            return WEFT_UNSUPPORTED_ABI;
        }
        if ((desc->flags & ~WEFT_RUNTIME_SYSTEM_FONTS) || desc->max_views == 0 ||
            desc->max_views > weft::constants::core::hard_max_views) {
            return WEFT_INVALID_ARGUMENT;
        }
        max_views = desc->max_views;
        flags = desc->flags;
    }
    try {
        auto runtime = std::make_unique<weft_runtime>();
        auto id = next_runtime_id.load(std::memory_order_relaxed);
        for (;;) {
            if (id == std::numeric_limits<uint64_t>::max()) {
                return WEFT_CAPACITY_EXCEEDED;
            }
            if (next_runtime_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
                break;
            }
        }
        runtime->id = id;
        runtime->views = weft::GenerationalSlots<ViewEntry>(max_views);
        runtime->shared.resize(max_views);
        runtime->system_fonts = flags & WEFT_RUNTIME_SYSTEM_FONTS;
        *out_runtime = runtime.release();
        return WEFT_OK;
    } catch (const std::bad_alloc&) {
        return WEFT_OUT_OF_MEMORY;
    } catch (...) {
        return WEFT_INTERNAL_ERROR;
    }
}

extern "C" weft_result weft_runtime_destroy(weft_runtime* runtime) {
    if (const auto error = validate_owner(runtime); error != WEFT_OK) {
        return error;
    }
    if (runtime->live_views != 0) {
        return WEFT_BUSY;
    }
    delete runtime;
    return WEFT_OK;
}

extern "C" weft_result weft_runtime_get_info(weft_runtime* runtime, weft_runtime_info* out_info) {
    if (const auto error = validate_owner(runtime); error != WEFT_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(weft_runtime_info)) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_info = {sizeof(weft_runtime_info), WEFT_ABI_VERSION, runtime->id, runtime->live_views,
                 static_cast<uint32_t>(runtime->views.size())};
    return WEFT_OK;
}

extern "C" weft_result weft_runtime_add_font(weft_runtime* runtime, const weft_font_face* face) {
    if (const auto error = font_owner(runtime); error != WEFT_OK) {
        return error;
    }
    std::string_view family;
    std::string_view path;
    if (!face || face->struct_size < sizeof(weft_font_face) || !text_arg(face->family, face->family_size, family) ||
        !text_arg(face->path, face->path_size, path) || family.empty() || path.empty() || face->weight < 1 ||
        face->weight > 1000 || face->italic > 1 || !weft::bridge::utf8(family) || !weft::bridge::utf8(path)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        weft::FontFaceEntry entry;
        entry.family = std::string(family);
        entry.weight = std::uint16_t(face->weight);
        entry.italic = face->italic != 0;
        entry.source.path = std::filesystem::path(std::u8string(path.begin(), path.end()));
        entry.source.face_index = face->face_index;
        runtime->catalog.faces.push_back(std::move(entry));
        return WEFT_OK;
    });
}

extern "C" weft_result weft_runtime_set_generic_family(weft_runtime* runtime, const char* generic, size_t generic_size,
                                                       const char* family, size_t family_size) {
    if (const auto error = font_owner(runtime); error != WEFT_OK) {
        return error;
    }
    std::string_view keyword;
    std::string_view name;
    if (!text_arg(generic, generic_size, keyword) || !text_arg(family, family_size, name) || name.empty() ||
        !weft::bridge::utf8(name) ||
        (keyword != "serif" && keyword != "sans-serif" && keyword != "monospace" && keyword != "system-ui" &&
         keyword != "cursive" && keyword != "fantasy")) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        auto& generics = runtime->catalog.generic_families;
        std::erase_if(generics, [&](const auto& g) {
            return g.first == keyword;
        });
        generics.emplace_back(std::string(keyword), std::string(name));
        return WEFT_OK;
    });
}

extern "C" weft_result weft_runtime_add_fallback_family(weft_runtime* runtime, const char* family, size_t family_size) {
    if (const auto error = font_owner(runtime); error != WEFT_OK) {
        return error;
    }
    std::string_view name;
    if (!text_arg(family, family_size, name) || name.empty() || !weft::bridge::utf8(name)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        runtime->catalog.fallback_families.emplace_back(name);
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_create(weft_runtime* runtime, const weft_view_desc* desc, weft_view_handle* out_view) {
    if (!out_view) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_view = {};
    if (const auto error = validate_owner(runtime); error != WEFT_OK) {
        return error;
    }
    const weft_view_desc defaults{sizeof(weft_view_desc),
                                  0,
                                  weft::constants::core::default_view_logical_width,
                                  weft::constants::core::default_view_logical_height,
                                  1,
                                  0};
    if (!desc) {
        desc = &defaults;
    }
    if (desc->struct_size < sizeof(weft_view_desc) || !valid_view_desc(*desc)) {
        return WEFT_INVALID_ARGUMENT;
    }
    const auto index = runtime->views.first_free();
    if (index == runtime->views.size()) {
        return WEFT_CAPACITY_EXCEEDED;
    }
    return guarded([&] {
        auto shared = std::make_shared<weft::ViewShared>(std::make_shared<weft::bridge::EngineChannel>());
        auto& entry = runtime->views[index];
        entry = {};
        entry.info = {
            sizeof(weft_view_info), 0, desc->logical_width, desc->logical_height, desc->device_scale, 0, 0, 0};
        entry.shared = shared;
        const auto generation = runtime->views.occupy(index);
        {
            const std::lock_guard lock(runtime->shared_mutex);
            runtime->shared[index] = {generation, std::move(shared)};
        }
        ++runtime->live_views;
        *out_view = {runtime->id, static_cast<uint32_t>(index + 1), generation};
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_destroy(weft_runtime* runtime, weft_view_handle view) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    {
        const std::lock_guard lock(runtime->shared_mutex);
        runtime->shared[view.slot - 1] = {};
    }
    // The View goes with its VM; frames and shared parts others hold stay until they let go.
    *entry = {};
    runtime->views.vacate(view.slot - 1);
    --runtime->live_views;
    return WEFT_OK;
}

extern "C" weft_result weft_view_get_info(weft_runtime* runtime, weft_view_handle view, weft_view_info* out_info) {
    ViewEntry* entry{};
    if (const auto error = find_entry(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    if (!out_info || out_info->struct_size < sizeof(weft_view_info)) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_info = entry->info;
    return WEFT_OK;
}

namespace {
    // Validates and records a monotonic host time.
    weft_result record_time(weft_view_info& info, double time_seconds) noexcept {
        if (!std::isfinite(time_seconds) || time_seconds < 0) {
            return WEFT_INVALID_ARGUMENT;
        }
        if (info.tick_count != 0 && time_seconds < info.last_time_seconds) {
            return WEFT_TIME_REGRESSION;
        }
        if (info.tick_count == std::numeric_limits<uint64_t>::max()) {
            return WEFT_CAPACITY_EXCEEDED;
        }
        ++info.tick_count;
        info.last_time_seconds = time_seconds;
        return WEFT_OK;
    }
} // namespace

extern "C" weft_result weft_view_record_tick(weft_runtime* runtime, weft_view_handle view, double time_seconds) {
    ViewEntry* entry{};
    if (const auto error = find_entry(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    return record_time(entry->info, time_seconds);
}

extern "C" weft_result weft_view_load(weft_runtime* runtime, weft_view_handle view, const weft_page_desc* page) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    // A page of an earlier ABI ends before base.
    const bool based = page && page->struct_size >= sizeof(weft_page_desc);
    if (!page || (!based && page->struct_size != offsetof(weft_page_desc, base)) ||
        (page->flags & ~(WEFT_PAGE_CRPG_BRIDGE | WEFT_PAGE_FORGIVING_CSS)) || page->reserved ||
        (!page->scripts && page->script_count)) {
        return WEFT_INVALID_ARGUMENT;
    }
    weft::ViewPage content;
    std::string_view text;
    for (uint32_t i = 0; i < page->script_count; ++i) {
        const auto& script = page->scripts[i];
        std::string_view name;
        if (!text_arg(script.name, script.name_size, name) || !text_arg(script.source, script.source_size, text)) {
            return WEFT_INVALID_ARGUMENT;
        }
    }
    std::string_view stylesheet;
    std::string_view keyframes;
    std::string_view root;
    std::string_view base;
    if (!text_arg(page->stylesheet, page->stylesheet_size, stylesheet) ||
        !text_arg(page->keyframes, page->keyframes_size, keyframes) || !text_arg(page->root, page->root_size, root) ||
        (based && !text_arg(page->base, page->base_size, base))) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        for (uint32_t i = 0; i < page->script_count; ++i) {
            const auto& script = page->scripts[i];
            content.scripts.push_back(
                {std::string(script.name, script.name_size), std::string(script.source, script.source_size)});
        }
        content.stylesheet = stylesheet;
        content.keyframes = keyframes;
        content.root = root.empty() ? std::string("#root") : std::string(root);
        content.base = base;
        content.crpg_bridge = page->flags & WEFT_PAGE_CRPG_BRIDGE;
        content.forgiving_css = page->flags & WEFT_PAGE_FORGIVING_CSS;
        if (!entry->view) {
            freeze_fonts(*runtime);
            entry->view =
                std::make_unique<weft::View>(runtime->fonts, *runtime->text, entry->info.logical_width,
                                             entry->info.logical_height, entry->info.device_scale, entry->shared);
            for (auto& function : entry->functions) {
                entry->view->install_function(function.name, engine_function(*entry, function));
            }
            entry->view->set_resource_reader(resource_reader(*entry));
        }
        entry->view->load(content);
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_advance(weft_runtime* runtime, weft_view_handle view, double time_seconds) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    auto info = entry->info;
    if (const auto error = record_time(info, time_seconds); error != WEFT_OK) {
        return error;
    }
    return guarded([&] {
        if (entry->view) {
            entry->view->advance(std::max(time_seconds * 1000, entry->view->time()));
        }
        entry->info = info;
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_resize(weft_runtime* runtime, weft_view_handle view, float logical_width,
                                        float logical_height, float device_scale) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    const weft_view_desc desc{sizeof(weft_view_desc), 0, logical_width, logical_height, device_scale, 0};
    if (!valid_view_desc(desc)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        if (entry->view) {
            entry->view->resize(logical_width, logical_height, device_scale);
        }
        entry->info.logical_width = logical_width;
        entry->info.logical_height = logical_height;
        entry->info.device_scale = device_scale;
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_pointer(weft_runtime* runtime, weft_view_handle view, const weft_pointer_event* event,
                                         uint32_t* out_prevented) {
    if (out_prevented) {
        *out_prevented = 0;
    }
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    if (!event || event->struct_size < sizeof(weft_pointer_event) || event->kind > WEFT_POINTER_LEAVE ||
        event->reserved) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        if (entry->view) {
            const bool prevented = entry->view->pointer(
                {weft::PointerKind(event->kind), event->x, event->y, event->buttons, event->wheel_x, event->wheel_y});
            if (out_prevented) {
                *out_prevented = prevented;
            }
        }
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_hit_test(weft_runtime* runtime, weft_view_handle view, float x, float y,
                                          weft_hit* out_hit) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    if (!out_hit || out_hit->struct_size < sizeof(weft_hit)) {
        return WEFT_INVALID_ARGUMENT;
    }
    out_hit->element = 0;
    std::memcpy(out_hit->cursor, "auto", 5);
    return guarded([&] {
        if (entry->view) {
            const auto hit = entry->view->hit(x, y);
            out_hit->element = hit.element.generation != 0;
            const auto size = std::min(hit.cursor.size(), sizeof(out_hit->cursor) - 1);
            std::memcpy(out_hit->cursor, hit.cursor.data(), size);
            out_hit->cursor[size] = 0;
        }
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_evaluate(weft_runtime* runtime, weft_view_handle view, const char* source,
                                          size_t source_size, weft_text_fn fn, void* user) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    if ((!source && source_size) || !fn) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        if (!entry->view || !entry->view->loaded()) {
            return WEFT_BUSY;
        }
        const auto [ok, text] = entry->view->evaluate(std::string_view(source ? source : "", source_size));
        fn(user, ok ? 1u : 0u, text.data(), text.size());
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_acquire_frame(weft_runtime* runtime, weft_view_handle view,
                                               weft_view_frame** out_frame) {
    if (!out_frame) {
        return WEFT_INVALID_ARGUMENT;
    }
    *out_frame = nullptr;
    if (!runtime) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        const auto shared = find_shared(runtime, view);
        if (!shared) {
            return WEFT_STALE_HANDLE;
        }
        auto frame = shared->frame();
        if (!frame) {
            return WEFT_BUSY;
        }
        *out_frame = new weft_view_frame{std::move(frame)};
        return WEFT_OK;
    });
}

extern "C" void weft_view_frame_release(weft_view_frame* frame) {
    delete frame;
}

extern "C" weft_result weft_view_frame_get_info(const weft_view_frame* frame, weft_view_frame_info* out_info) {
    if (!frame || !out_info || out_info->struct_size < sizeof(weft_view_frame_info)) {
        return WEFT_INVALID_ARGUMENT;
    }
    const auto& f = *frame->frame;
    *out_info = {sizeof(weft_view_frame_info),
                 f.scene->animated,
                 f.scene->revision,
                 f.scene->time_ms / 1000,
                 f.width,
                 f.height,
                 f.scale,
                 0};
    return WEFT_OK;
}

extern "C" weft_result weft_view_write_anchors(weft_runtime* runtime, weft_view_handle view, const weft_anchor* anchors,
                                               size_t count) {
    if (!runtime || (!anchors && count)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        const auto shared = find_shared(runtime, view);
        if (!shared) {
            return WEFT_STALE_HANDLE;
        }
        const auto* first = reinterpret_cast<const weft::SceneAnchor*>(anchors);
        switch (shared->write_anchors({first, count})) {
            case weft::SceneAnchorWrite::Written:
                return WEFT_OK;
            case weft::SceneAnchorWrite::Invalid:
                return WEFT_INVALID_ARGUMENT;
            case weft::SceneAnchorWrite::TooMany:
                return WEFT_CAPACITY_EXCEEDED;
        }
        return WEFT_INTERNAL_ERROR;
    });
}

namespace {
    // A value the host passes, in its format: MessagePack checked by the channel, JSON converted by it.
    bool value_arg(uint32_t format, const void* payload, size_t size) noexcept {
        return (format == WEFT_VALUE_MSGPACK || format == WEFT_VALUE_JSON) && (payload || !size);
    }

    std::span<const std::uint8_t> bytes_of(const void* payload, size_t size) noexcept {
        return {static_cast<const std::uint8_t*>(payload), size};
    }

    std::string_view text_of(const void* payload, size_t size) noexcept {
        return {static_cast<const char*>(payload), size};
    }
} // namespace

extern "C" weft_result weft_view_emit(weft_runtime* runtime, weft_view_handle view, const char* name, size_t name_size,
                                      uint32_t format, const void* payload, size_t payload_size, uint32_t flags) {
    std::string_view event;
    if (!runtime || !text_arg(name, name_size, event) || !value_arg(format, payload, payload_size) ||
        (flags & ~WEFT_EMIT_LATEST)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        const auto shared = find_shared(runtime, view);
        if (!shared) {
            return WEFT_STALE_HANDLE;
        }
        const bool latest = flags & WEFT_EMIT_LATEST;
        auto& channel = *shared->engine();
        return channel_result(format == WEFT_VALUE_MSGPACK
                                  ? channel.emit(event, bytes_of(payload, payload_size), latest)
                                  : channel.emit_json(event, text_of(payload, payload_size), latest));
    });
}

extern "C" weft_result weft_view_complete_call(weft_runtime* runtime, weft_view_handle view, uint64_t call_id,
                                               uint32_t succeeded, uint32_t format, const void* payload,
                                               size_t payload_size) {
    if (!runtime || succeeded > 1 || !value_arg(format, payload, payload_size)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        const auto shared = find_shared(runtime, view);
        if (!shared) {
            return WEFT_STALE_HANDLE;
        }
        auto& channel = *shared->engine();
        return channel_result(format == WEFT_VALUE_MSGPACK
                                  ? channel.complete(call_id, succeeded, bytes_of(payload, payload_size))
                                  : channel.complete_json(call_id, succeeded, text_of(payload, payload_size)));
    });
}

extern "C" weft_result weft_view_set_model(weft_runtime* runtime, weft_view_handle view, const char* model,
                                           size_t model_size, uint32_t format, const void* fields, size_t fields_size) {
    std::string_view name;
    if (!runtime || !text_arg(model, model_size, name) || !value_arg(format, fields, fields_size)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        const auto shared = find_shared(runtime, view);
        if (!shared) {
            return WEFT_STALE_HANDLE;
        }
        auto& channel = *shared->engine();
        return channel_result(format == WEFT_VALUE_MSGPACK
                                  ? channel.set_model(name, bytes_of(fields, fields_size))
                                  : channel.set_model_json(name, text_of(fields, fields_size)));
    });
}

extern "C" weft_result weft_view_drain_messages(weft_runtime* runtime, weft_view_handle view, weft_message_fn fn,
                                                void* user) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    if (!fn) {
        return WEFT_INVALID_ARGUMENT;
    }
    if (!entry->view) {
        return WEFT_OK;
    }
    return guarded([&] {
        const auto messages = entry->view->take_messages();
        const Busy busy(*entry);
        for (const auto& m : messages) {
            const weft_message message{sizeof(weft_message),
                                       m.call ? WEFT_MESSAGE_CALL : WEFT_MESSAGE_SEND,
                                       m.call,
                                       m.name.data(),
                                       m.name.size(),
                                       m.value.data(),
                                       m.value.size()};
            fn(user, view, &message);
        }
        return WEFT_OK;
    });
}

extern "C" weft_result weft_value_writer_set(weft_value_writer* writer, uint32_t format, const void* value,
                                             size_t size) {
    if (!writer || !value_arg(format, value, size)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        if (!size) {
            writer->value.clear();
            return WEFT_OK;
        }
        if (format == WEFT_VALUE_JSON) {
            writer->value = weft::bridge::value_from_json(text_of(value, size));
            return WEFT_OK;
        }
        const auto bytes = bytes_of(value, size);
        if (!weft::bridge::valid_value(bytes)) {
            return WEFT_INVALID_ARGUMENT;
        }
        writer->value.assign(bytes.begin(), bytes.end());
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_register_function(weft_runtime* runtime, weft_view_handle view, const char* name,
                                                   size_t name_size, weft_function_fn fn, void* user) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    std::string_view function;
    if (!fn || !text_arg(name, name_size, function) || function.empty() ||
        function.size() > weft::constants::bridge::max_name_bytes || !weft::bridge::utf8(function)) {
        return WEFT_INVALID_ARGUMENT;
    }
    return guarded([&] {
        HostFunction host{std::string(function), fn, user};
        std::erase_if(entry->functions, [&](const HostFunction& f) {
            return f.name == host.name;
        });
        entry->functions.push_back(host);
        if (entry->view) {
            entry->view->install_function(host.name, engine_function(*entry, host));
        }
        return WEFT_OK;
    });
}

extern "C" weft_result weft_resource_writer_set(weft_resource_writer* writer, const void* bytes, size_t size) {
    if (!writer || (!bytes && size)) {
        return WEFT_INVALID_ARGUMENT;
    }
    if (size > weft::constants::view::max_resource_bytes) {
        return WEFT_CAPACITY_EXCEEDED;
    }
    return guarded([&] {
        const auto* first = static_cast<const std::uint8_t*>(bytes);
        writer->bytes.assign(first, first + size);
        writer->written = true;
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_set_resource_reader(weft_runtime* runtime, weft_view_handle view, weft_resource_fn fn,
                                                     void* user) {
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    return guarded([&] {
        entry->reader = fn;
        entry->reader_user = fn ? user : nullptr;
        if (entry->view) {
            entry->view->set_resource_reader(resource_reader(*entry));
        }
        return WEFT_OK;
    });
}

extern "C" weft_result weft_view_drain_diagnostics(weft_runtime* runtime, weft_view_handle view, weft_diagnostic_fn fn,
                                                   void* user, uint32_t* out_dropped) {
    if (out_dropped) {
        *out_dropped = 0;
    }
    ViewEntry* entry{};
    if (const auto error = find_idle(runtime, view, entry); error != WEFT_OK) {
        return error;
    }
    if (!fn) {
        return WEFT_INVALID_ARGUMENT;
    }
    if (!entry->view) {
        return WEFT_OK;
    }
    return guarded([&] {
        std::size_t dropped{};
        const auto diagnostics = entry->view->take_diagnostics(&dropped);
        if (out_dropped) {
            *out_dropped = uint32_t(std::min<std::size_t>(dropped, std::numeric_limits<uint32_t>::max()));
        }
        const Busy busy(*entry);
        for (const auto& d : diagnostics) {
            const weft_diagnostic diagnostic{sizeof(weft_diagnostic), uint32_t(d.kind) + 1, d.message.data(),
                                             d.message.size()};
            fn(user, view, &diagnostic);
        }
        return WEFT_OK;
    });
}

extern "C" const char* weft_result_string(weft_result result) {
    switch (result) {
        case WEFT_OK:
            return "ok";
        case WEFT_INVALID_ARGUMENT:
            return "invalid argument";
        case WEFT_UNSUPPORTED_ABI:
            return "unsupported ABI";
        case WEFT_OUT_OF_MEMORY:
            return "out of memory";
        case WEFT_WRONG_THREAD:
            return "wrong owner thread";
        case WEFT_BUSY:
            return "resources still live";
        case WEFT_STALE_HANDLE:
            return "stale handle";
        case WEFT_CAPACITY_EXCEEDED:
            return "capacity exceeded";
        case WEFT_TIME_REGRESSION:
            return "host time regressed";
        case WEFT_INTERNAL_ERROR:
            return "internal error";
        case WEFT_SCRIPT_ERROR:
            return "page script error";
        case WEFT_FAULTED:
            return "view faulted";
    }
    return "unknown result";
}
