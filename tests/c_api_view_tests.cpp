#include <weft/weft.h>

#include "bridge/bridge_value.hpp"
#include "support/check.hpp"
#include "support/files.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The View's C ABI (include/weft/weft.h, docs/engine-communication.md section 8): struct sizes and versions, owner and
// any-thread calls, stale handles (also against concurrent destruction), pages, engine events, calls, view models,
// sync functions and their reentry, anchors, hit queries, frames that outlive their View, diagnostics and faults, and
// the resource reader of a page's url() images against its base.
namespace {
    namespace bridge = weft::bridge;
    using weft::testing::require;

    std::string source(const std::string& relative) {
        return weft::testing::read_file(std::string(WEFT_SOURCE_DIR) + "/" + relative);
    }

    struct Runtime {
        weft_runtime* runtime{};

        Runtime() {
            require(weft_runtime_create(nullptr, &runtime) == WEFT_OK, "runtime");
        }

        ~Runtime() {
            weft_runtime_destroy(runtime);
        }

        weft_view_handle view(float width = 1280, float height = 720) const {
            const weft_view_desc desc{sizeof(weft_view_desc), 0, width, height, 1, 0};
            weft_view_handle out{};
            require(weft_view_create(runtime, &desc, &out) == WEFT_OK, "view");
            return out;
        }
    };

    // A page's sources, kept alive for its descriptor.
    struct Page {
        std::vector<std::string> names;
        std::vector<std::string> sources;
        std::vector<weft_script> scripts;
        std::string stylesheet;
        std::string keyframes;
        std::string base;
        uint32_t flags{};

        weft_page_desc desc() {
            scripts.clear();
            for (std::size_t i = 0; i < sources.size(); ++i) {
                scripts.push_back({names[i].data(), names[i].size(), sources[i].data(), sources[i].size()});
            }
            return {sizeof(weft_page_desc),
                    flags,
                    scripts.data(),
                    uint32_t(scripts.size()),
                    0,
                    stylesheet.data(),
                    stylesheet.size(),
                    keyframes.data(),
                    keyframes.size(),
                    nullptr,
                    0,
                    base.data(),
                    base.size()};
        }
    };

    Page hud() {
        Page p;
        p.names = {"hud.js"};
        p.sources = {source("examples/engine-host/hud.js")};
        p.stylesheet = source("examples/engine-host/hud.css");
        p.keyframes = source("examples/engine-host/hud-keyframes.css");
        return p;
    }

    Page inline_page(std::string script, std::string stylesheet = {}) {
        Page p;
        p.names = {"inline.js"};
        p.sources = {std::move(script)};
        p.stylesheet = "#root{position:relative;width:100%;height:100%}" + stylesheet;
        return p;
    }

    weft_result load(const Runtime& r, weft_view_handle view, Page page) {
        const auto desc = page.desc();
        return weft_view_load(r.runtime, view, &desc);
    }

    struct Message {
        uint32_t kind{};
        uint64_t call{};
        std::string name;
        std::string json;
    };

    std::vector<Message> drain(const Runtime& r, weft_view_handle view) {
        std::vector<Message> out;
        require(weft_view_drain_messages(
                    r.runtime, view,
                    [](void* user, weft_view_handle, const weft_message* m) {
                        static_cast<std::vector<Message>*>(user)->push_back(
                            {m->kind, m->call_id, std::string(m->name, m->name_size),
                             bridge::value_to_json({static_cast<const std::uint8_t*>(m->payload), m->payload_size})});
                    },
                    &out) == WEFT_OK,
                "drain");
        return out;
    }

    std::vector<std::pair<uint32_t, std::string>> diagnostics(const Runtime& r, weft_view_handle view) {
        std::vector<std::pair<uint32_t, std::string>> out;
        uint32_t dropped = 9;
        require(weft_view_drain_diagnostics(
                    r.runtime, view,
                    [](void* user, weft_view_handle, const weft_diagnostic* d) {
                        static_cast<std::vector<std::pair<uint32_t, std::string>>*>(user)->emplace_back(
                            d->kind, std::string(d->message, d->message_size));
                    },
                    &out, &dropped) == WEFT_OK &&
                    dropped == 0,
                "diagnostics");
        return out;
    }

    weft_result emit(const Runtime& r, weft_view_handle view, std::string_view name, std::string_view json,
                     uint32_t flags = 0) {
        return weft_view_emit(r.runtime, view, name.data(), name.size(), WEFT_VALUE_JSON, json.data(), json.size(),
                              flags);
    }

    weft_result pointer(const Runtime& r, weft_view_handle view, uint32_t kind, float x, float y, uint32_t buttons = 0,
                        uint32_t* prevented = nullptr) {
        const weft_pointer_event e{sizeof(weft_pointer_event), kind, x, y, buttons, 0, 0, 0};
        return weft_view_pointer(r.runtime, view, &e, prevented);
    }

    weft_hit hit(const Runtime& r, weft_view_handle view, float x, float y) {
        weft_hit out{};
        out.struct_size = sizeof(out);
        require(weft_view_hit_test(r.runtime, view, x, y, &out) == WEFT_OK, "hit");
        return out;
    }

    weft_view_frame_info frame_info(const Runtime& r, weft_view_handle view) {
        weft_view_frame* frame{};
        require(weft_view_acquire_frame(r.runtime, view, &frame) == WEFT_OK && frame, "frame");
        weft_view_frame_info info{};
        info.struct_size = sizeof(info);
        require(weft_view_frame_get_info(frame, &info) == WEFT_OK, "frame info");
        weft_view_frame_release(frame);
        return info;
    }

    void sizes_and_versions() {
        weft_runtime* runtime = reinterpret_cast<weft_runtime*>(1);
        weft_runtime_desc desc{sizeof(weft_runtime_desc), WEFT_ABI_VERSION, 4, 2};
        require(weft_runtime_create(&desc, &runtime) == WEFT_INVALID_ARGUMENT && !runtime, "unknown runtime flags");
        desc = {sizeof(weft_runtime_desc) - 4, WEFT_ABI_VERSION, 4, 0};
        require(weft_runtime_create(&desc, &runtime) == WEFT_INVALID_ARGUMENT, "a short runtime descriptor");
        desc = {sizeof(weft_runtime_desc), WEFT_ABI_VERSION + 1, 4, 0};
        require(weft_runtime_create(&desc, &runtime) == WEFT_UNSUPPORTED_ABI, "another ABI version");

        Runtime r;
        const auto view = r.view();
        auto page = inline_page("1");
        auto d = page.desc();
        d.struct_size = sizeof(d) - 8;
        require(weft_view_load(r.runtime, view, &d) == WEFT_INVALID_ARGUMENT, "a short page descriptor");
        d = page.desc();
        d.flags = 16;
        require(weft_view_load(r.runtime, view, &d) == WEFT_INVALID_ARGUMENT, "unknown page flags");
        d = page.desc();
        d.reserved = 1;
        require(weft_view_load(r.runtime, view, &d) == WEFT_INVALID_ARGUMENT, "reserved page field");
        d = page.desc();
        d.scripts = nullptr;
        require(weft_view_load(r.runtime, view, &d) == WEFT_INVALID_ARGUMENT, "scripts missing");
        d = page.desc();
        page.scripts[0].source = nullptr;
        require(weft_view_load(r.runtime, view, &d) == WEFT_INVALID_ARGUMENT, "a script's text missing");
        require(weft_view_load(r.runtime, view, nullptr) == WEFT_INVALID_ARGUMENT, "no page");

        weft_pointer_event e{sizeof(weft_pointer_event) - 4, WEFT_POINTER_MOVE, 0, 0, 0, 0, 0, 0};
        require(weft_view_pointer(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a short pointer event");
        e = {sizeof(weft_pointer_event), 9, 0, 0, 0, 0, 0, 0};
        require(weft_view_pointer(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "an unknown pointer kind");
        e = {sizeof(weft_pointer_event), WEFT_POINTER_MOVE, 0, 0, 0, 1u << 7, 0, 0};
        require(weft_view_pointer(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "unknown pointer modifiers");
        weft_hit h{};
        h.struct_size = sizeof(h) - 1;
        require(weft_view_hit_test(r.runtime, view, 0, 0, &h) == WEFT_INVALID_ARGUMENT, "a short hit");
        weft_font_face face{sizeof(weft_font_face), 400, 0, 0, "Ahem", 4, "x.ttf", 5};
        face.struct_size -= 8;
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "a short font face");
        face = {sizeof(weft_font_face), 0, 0, 0, "Ahem", 4, "x.ttf", 5};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "weight 0");
        face = {sizeof(weft_font_face), 400, 2, 0, "Ahem", 4, "x.ttf", 5};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "italic 2");
        face = {sizeof(weft_font_face), 400, 0, 0, "", 0, "x.ttf", 5};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "a face without a family");
        face = {sizeof(weft_font_face), 400, 0, 0, "Ahem", 4, "x.ttf", 5, "U+30-", 5};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "a unicode-range that is none");
        face = {sizeof(weft_font_face), 400, 0, 0, "Ahem", 4, "x.ttf", 5, nullptr, 3};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "a unicode-range's text missing");
        face = {offsetof(weft_font_face, unicode_range) + 8, 400, 0, 0, "Ahem", 4, "x.ttf", 5};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "a size inside unicode_range");
        face = {sizeof(weft_font_face), 400, 0, 0, "", 0, "x.ttf", 5};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_INVALID_ARGUMENT, "no family");
        require(weft_runtime_set_generic_family(r.runtime, "math", 4, "Ahem", 4) == WEFT_INVALID_ARGUMENT,
                "an unknown generic family");
        require(weft_runtime_set_generic_family(r.runtime, "serif", 5, "Ahem", 4) == WEFT_OK &&
                    weft_runtime_add_fallback_family(r.runtime, "Ahem", 4) == WEFT_OK,
                "font families before the first page");
        const auto ahem = std::string(WEFT_SOURCE_DIR) + "/third_party/ahem/Ahem.ttf";
        face = {sizeof(weft_font_face), 400, 0, 0, "Ahem", 4, ahem.data(), ahem.size()};
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_OK, "a font file");
        face.unicode_range = "U+0-7F";
        face.unicode_range_size = 6;
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_OK, "a face with a unicode-range");
        face.struct_size = offsetof(weft_font_face, unicode_range);
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_OK, "a face of the earlier ABI");
        require(load(r, view, inline_page("1")) == WEFT_OK, "a page");
        require(weft_runtime_add_font(r.runtime, &face) == WEFT_BUSY &&
                    weft_runtime_add_fallback_family(r.runtime, "Ahem", 4) == WEFT_BUSY,
                "fonts are fixed once a page loaded");
        weft_view_frame_info info{};
        info.struct_size = sizeof(info) - 4;
        weft_view_frame* frame{};
        require(weft_view_acquire_frame(r.runtime, view, &frame) == WEFT_OK &&
                    weft_view_frame_get_info(frame, &info) == WEFT_INVALID_ARGUMENT,
                "a short frame info");
        weft_view_frame_release(frame);
        require(std::string(weft_result_string(WEFT_SCRIPT_ERROR)) == "page script error" &&
                    std::string(weft_result_string(WEFT_FAULTED)) == "view faulted",
                "the new results' names");
        weft_view_destroy(r.runtime, view);
    }

    void without_a_page() {
        Runtime r;
        const auto view = r.view();
        weft_view_frame* frame = reinterpret_cast<weft_view_frame*>(1);
        require(weft_view_acquire_frame(r.runtime, view, &frame) == WEFT_BUSY && !frame, "no frame yet");
        require(weft_view_advance(r.runtime, view, 0.5) == WEFT_OK &&
                    pointer(r, view, WEFT_POINTER_MOVE, 10, 10) == WEFT_OK,
                "advance and pointer without a page");
        const auto h = hit(r, view, 10, 10);
        require(!h.element && std::string(h.cursor) == "auto", "nothing under a point");
        require(drain(r, view).empty() && diagnostics(r, view).empty(), "nothing to drain");
        require(weft_view_advance(r.runtime, view, 0.25) == WEFT_TIME_REGRESSION &&
                    weft_view_advance(r.runtime, view, std::nan("")) == WEFT_INVALID_ARGUMENT,
                "time");
        weft_view_destroy(r.runtime, view);
    }

    struct Calls {
        const Runtime* r{};
        weft_view_handle view{};
        weft_result reentry = WEFT_OK;
        weft_result emitted = WEFT_INTERNAL_ERROR;
        unsigned count{};
    };

    weft_result loc_get(void* user, const void* argument, size_t size, weft_value_writer* result) {
        auto& calls = *static_cast<Calls*>(user);
        ++calls.count;
        // Owner operations wait; the bridge's any-thread calls work.
        calls.reentry = weft_view_advance(calls.r->runtime, calls.view, 1000);
        calls.emitted = emit(*calls.r, calls.view, "from.function", "1");
        const auto key = bridge::value_to_json({static_cast<const std::uint8_t*>(argument), size});
        const std::string answer = key == R"({"key":"hud.title"})" ? R"("Battle")" : R"("?")";
        return weft_value_writer_set(result, WEFT_VALUE_JSON, answer.data(), answer.size());
    }

    void pages_and_bridge() {
        Runtime r;
        const auto view = r.view();
        Calls calls{&r, view};
        require(weft_view_register_function(r.runtime, view, "loc.get", 7, loc_get, &calls) == WEFT_OK,
                "a function before the page");
        require(weft_view_register_function(r.runtime, view, "", 0, loc_get, &calls) == WEFT_INVALID_ARGUMENT &&
                    weft_view_register_function(r.runtime, view, "x", 1, nullptr, &calls) == WEFT_INVALID_ARGUMENT,
                "a function needs a name and a callback");
        require(load(r, view, hud()) == WEFT_OK, "the HUD");
        require(calls.count == 1 && calls.reentry == WEFT_BUSY && calls.emitted == WEFT_OK,
                "a function's owner calls wait; its emits go");
        auto messages = drain(r, view);
        require(messages.size() == 1 && messages[0].kind == WEFT_MESSAGE_SEND && messages[0].call == 0 &&
                    messages[0].name == "hud.ready" && messages[0].json == R"({"title":"Battle"})",
                "the ready message");
        const auto first = frame_info(r, view);
        require(first.revision && first.logical_width == 1280 && first.logical_height == 720 && first.device_scale == 1,
                "the first frame");
        // Events in JSON and MessagePack, latest, a model; malformed input refused.
        require(emit(r, view, "hud.spawn", R"({"id":1,"name":"Goblin"})") == WEFT_OK, "JSON event");
        bridge::ValueWriter w;
        w.map(2);
        w.string("id");
        w.integer(2);
        w.string("name");
        w.string("Orc");
        require(weft_view_emit(r.runtime, view, "hud.spawn", 9, WEFT_VALUE_MSGPACK, w.data().data(), w.data().size(),
                               0) == WEFT_OK,
                "MessagePack event");
        require(emit(r, view, "hud.spawn", "{", 0) == WEFT_INVALID_ARGUMENT &&
                    emit(r, view, "hud.spawn", "{}", 2) == WEFT_INVALID_ARGUMENT &&
                    weft_view_emit(r.runtime, view, "x", 1, 3, "1", 1, 0) == WEFT_INVALID_ARGUMENT,
                "malformed events");
        const std::string fields = R"({"1":0.25})";
        require(weft_view_set_model(r.runtime, view, "health", 6, WEFT_VALUE_JSON, fields.data(), fields.size()) ==
                    WEFT_OK,
                "a model");
        require(weft_view_set_model(r.runtime, view, "health", 6, WEFT_VALUE_JSON, "[1]", 3) == WEFT_INVALID_ARGUMENT,
                "a model that is not a map");
        require(weft_view_advance(r.runtime, view, 0.016) == WEFT_OK, "advance");
        const auto second = frame_info(r, view);
        require(second.revision != first.revision && std::abs(second.time_seconds - 0.016) < 1e-9, "a new frame");
        // The button calls the engine; a completion settles it at the next advance.
        require(pointer(r, view, WEFT_POINTER_MOVE, 1200, 680) == WEFT_OK &&
                    pointer(r, view, WEFT_POINTER_DOWN, 1200, 680, 1) == WEFT_OK &&
                    pointer(r, view, WEFT_POINTER_UP, 1200, 680, 0) == WEFT_OK,
                "a click");
        messages = drain(r, view);
        require(messages.size() == 1 && messages[0].kind == WEFT_MESSAGE_CALL && messages[0].call &&
                    messages[0].name == "hud.endTurn" && messages[0].json == R"({"turn":1})",
                "the click's call");
        const std::string answer = R"({"turn":2})";
        require(weft_view_complete_call(r.runtime, view, messages[0].call, 1, WEFT_VALUE_JSON, answer.data(),
                                        answer.size()) == WEFT_OK,
                "complete");
        require(weft_view_complete_call(r.runtime, view, messages[0].call, 1, WEFT_VALUE_JSON, answer.data(),
                                        answer.size()) == WEFT_STALE_HANDLE &&
                    weft_view_complete_call(r.runtime, view, 999999, 1, WEFT_VALUE_JSON, "1", 1) == WEFT_STALE_HANDLE &&
                    weft_view_complete_call(r.runtime, view, messages[0].call, 2, WEFT_VALUE_JSON, "1", 1) ==
                        WEFT_INVALID_ARGUMENT,
                "stale and malformed completions");
        require(weft_view_advance(r.runtime, view, 0.032) == WEFT_OK && frame_info(r, view).revision != second.revision,
                "the answer shows");
        // Hits: the button (a pointer), the title (no cursor), the root (nothing).
        auto h = hit(r, view, 1200, 680);
        require(h.element && std::string(h.cursor) == "pointer", "the button");
        h = hit(r, view, 1200, 620);
        require(h.element && std::string(h.cursor) == "auto", "the title");
        h = hit(r, view, 10, 10);
        require(!h.element, "the root");
        require(diagnostics(r, view).empty(), "no diagnostics");

        // A page that echoes: calls the engine, answers what it gets.
        require(load(r, view, inline_page(R"JS(
            weft.engine.on('echo', v => weft.engine.send('echo', v));
            weft.engine.call('ask', 1).then(v => weft.engine.send('answer', v), e => weft.engine.send('refused', e.value));
            try { weft.engine.invoke('fails'); } catch (e) { weft.engine.send('fails', e.message); }
          )JS")) == WEFT_OK,
                "the echo page");
        messages = drain(r, view);
        require(messages.size() == 2 && messages[0].kind == WEFT_MESSAGE_CALL && messages[0].name == "ask" &&
                    messages[1].name == "fails" && messages[1].json == R"("WEFT_ENGINE: no function fails")",
                "a call and an unknown function");
        require(weft_view_complete_call(r.runtime, view, messages[0].call, 0, WEFT_VALUE_JSON, "42", 2) == WEFT_OK,
                "a refusal");
        require(load(r, view, inline_page("weft.engine.call('ask', 1).then(v => weft.engine.send('answer', v))")) ==
                    WEFT_OK,
                "a page that asks");
        messages = drain(r, view);
        const std::uint8_t packed[]{0x92, 0x01, 0xa1, 'x'};
        require(messages.size() == 1 &&
                    weft_view_complete_call(r.runtime, view, messages[0].call, 1, WEFT_VALUE_MSGPACK, packed,
                                            sizeof packed) == WEFT_OK &&
                    weft_view_complete_call(r.runtime, view, 7, 1, WEFT_VALUE_MSGPACK, packed, sizeof packed) ==
                        WEFT_STALE_HANDLE &&
                    weft_view_complete_call(r.runtime, view, 7, 1, WEFT_VALUE_MSGPACK, packed, 2) ==
                        WEFT_INVALID_ARGUMENT,
                "a completion in MessagePack");
        require(weft_view_advance(r.runtime, view, 0.040) == WEFT_OK, "advance");
        messages = drain(r, view);
        require(messages.size() == 1 && messages[0].name == "answer" && messages[0].json == R"([1,"x"])", "its answer");
        require(load(r, view, inline_page(R"JS(
            weft.engine.on('echo', v => weft.engine.send('echo', v));
            weft.engine.call('ask', 1).then(v => weft.engine.send('answer', v), e => weft.engine.send('refused', e.value));
            try { weft.engine.invoke('fails'); } catch (e) { weft.engine.send('fails', e.message); }
          )JS")) == WEFT_OK,
                "the echo page again");
        messages = drain(r, view);
        require(weft_view_complete_call(r.runtime, view, messages[0].call, 0, WEFT_VALUE_JSON, "42", 2) == WEFT_OK,
                "a refusal again");
        require(emit(r, view, "echo", R"({"a":[1,2]})") == WEFT_OK &&
                    emit(r, view, "echo", "5", WEFT_EMIT_LATEST) == WEFT_OK,
                "events");
        require(weft_view_advance(r.runtime, view, 0.048) == WEFT_OK, "advance");
        messages = drain(r, view);
        require(messages.size() == 2 && messages[0].name == "refused" && messages[0].json == "42" &&
                    messages[1].name == "echo" && messages[1].json == "5",
                "the refusal, then the echo whose value the latest replaced");
        // A function that fails is the invoke's exception.
        require(weft_view_register_function(
                    r.runtime, view, "fails", 5,
                    [](void*, const void*, size_t, weft_value_writer*) {
                        return WEFT_INVALID_ARGUMENT;
                    },
                    nullptr) == WEFT_OK,
                "a failing function");
        require(
            load(r, view,
                 inline_page("try { weft.engine.invoke('fails'); } catch (e) { weft.engine.send('x', e.message); }")) ==
                WEFT_OK,
            "a page");
        messages = drain(r, view);
        require(messages.size() == 1 &&
                    messages[0].json == R"("WEFT_ENGINE_FUNCTION: fails returned invalid argument")",
                "a failing function's exception: " + (messages.empty() ? "" : messages[0].json));
        // Owner calls from a drain callback wait.
        require(load(r, view, inline_page("weft.engine.send('a'); weft.engine.send('b');")) == WEFT_OK, "a page");

        struct Drain {
            const Runtime* r;
            weft_view_handle view;
            weft_result advance = WEFT_OK;
            weft_result destroy = WEFT_OK;
            weft_result emit = WEFT_INTERNAL_ERROR;
        } during{&r, view};

        require(weft_view_drain_messages(
                    r.runtime, view,
                    [](void* user, weft_view_handle v, const weft_message*) {
                        auto& d = *static_cast<Drain*>(user);
                        d.advance = weft_view_advance(d.r->runtime, v, 2000);
                        d.destroy = weft_view_destroy(d.r->runtime, v);
                        d.emit = weft_view_emit(d.r->runtime, v, "x", 1, WEFT_VALUE_JSON, "1", 1, 0);
                    },
                    &during) == WEFT_OK,
                "a drain");
        require(during.advance == WEFT_BUSY && during.destroy == WEFT_BUSY && during.emit == WEFT_OK,
                "a drain's owner calls wait; its emits go");
        weft_view_destroy(r.runtime, view);
    }

    void anchors_and_hits() {
        Runtime r;
        const auto view = r.view();
        require(load(r, view,
                     inline_page(R"JS(
            const pick = document.createElement('div');
            pick.className = 'pick';
            pick.setAttribute('data-anchor', '7');
            document.getElementById('root').appendChild(pick);
            pick.addEventListener('mousedown', e => e.preventDefault());
          )JS",
                                 ".pick{position:absolute;left:0;top:0;width:40px;height:40px;cursor:grab}")) ==
                    WEFT_OK,
                "a page");
        weft_anchor anchor{7, 100, 100, 1, 0, 0, 0};
        require(weft_view_write_anchors(r.runtime, view, &anchor, 1) == WEFT_OK, "anchors");
        auto h = hit(r, view, 100, 100);
        require(h.element && std::string(h.cursor) == "grab", "at its anchor");
        uint32_t prevented = 9;
        require(pointer(r, view, WEFT_POINTER_DOWN, 100, 100, 1, &prevented) == WEFT_OK && prevented == 1 &&
                    pointer(r, view, WEFT_POINTER_UP, 100, 100, 0, &prevented) == WEFT_OK && prevented == 0,
                "a prevented press");
        anchor.flags = WEFT_ANCHOR_HIDDEN;
        require(weft_view_write_anchors(r.runtime, view, &anchor, 1) == WEFT_OK && !hit(r, view, 100, 100).element,
                "hidden");
        anchor.x = std::numeric_limits<float>::quiet_NaN();
        require(weft_view_write_anchors(r.runtime, view, &anchor, 1) == WEFT_INVALID_ARGUMENT, "not finite");
        std::vector<weft_anchor> many(65537, weft_anchor{1, 0, 0, 1, 0, 0, 0});
        require(weft_view_write_anchors(r.runtime, view, many.data(), many.size()) == WEFT_CAPACITY_EXCEEDED,
                "too many");
        require(weft_view_write_anchors(r.runtime, view, nullptr, 1) == WEFT_INVALID_ARGUMENT &&
                    weft_view_write_anchors(r.runtime, view, nullptr, 0) == WEFT_OK,
                "an empty table");
        weft_view_destroy(r.runtime, view);
    }

    void threads_and_stale_handles() {
        Runtime r;
        auto view = r.view();
        require(load(r, view, inline_page("1")) == WEFT_OK, "a page");
        // Owner calls from another thread are refused; any-thread calls work there.
        std::vector<weft_result> owner;
        std::vector<weft_result> any;
        std::thread([&] {
            auto page = inline_page("1");
            const auto desc = page.desc();
            weft_hit h{};
            h.struct_size = sizeof(h);
            owner = {weft_view_load(r.runtime, view, &desc),
                     weft_view_advance(r.runtime, view, 1),
                     pointer(r, view, WEFT_POINTER_MOVE, 0, 0),
                     weft_view_hit_test(r.runtime, view, 0, 0, &h),
                     weft_view_resize(r.runtime, view, 10, 10, 1),
                     weft_view_destroy(r.runtime, view),
                     weft_view_drain_messages(
                         r.runtime, view,
                         [](void*, weft_view_handle, const weft_message*) {
                         },
                         nullptr),
                     weft_view_register_function(r.runtime, view, "f", 1, loc_get, nullptr)};
            weft_view_frame* frame{};
            const weft_anchor anchor{1, 0, 0, 1, 0, 0, 0};
            any = {emit(r, view, "e", "1"), weft_view_set_model(r.runtime, view, "m", 1, WEFT_VALUE_JSON, "{}", 2),
                   weft_view_complete_call(r.runtime, view, 5, 1, WEFT_VALUE_JSON, "1", 1),
                   weft_view_write_anchors(r.runtime, view, &anchor, 1),
                   weft_view_acquire_frame(r.runtime, view, &frame)};
            weft_view_frame_release(frame);
        }).join();
        for (const auto result : owner) {
            require(result == WEFT_WRONG_THREAD, "an owner call from another thread");
        }
        require(any == std::vector<weft_result>{WEFT_OK, WEFT_OK, WEFT_STALE_HANDLE, WEFT_OK, WEFT_OK},
                "any-thread calls from another thread");
        // A frame outlives its View; the View's handle goes stale everywhere.
        weft_view_frame* kept{};
        require(weft_view_acquire_frame(r.runtime, view, &kept) == WEFT_OK, "a frame");
        require(weft_view_destroy(r.runtime, view) == WEFT_OK, "destroy");
        weft_view_frame_info info{};
        info.struct_size = sizeof(info);
        require(weft_view_frame_get_info(kept, &info) == WEFT_OK && info.logical_width == 1280, "the frame stays");
        weft_view_frame_release(kept);
        weft_view_frame* frame{};
        const weft_anchor anchor{1, 0, 0, 1, 0, 0, 0};
        weft_hit h{};
        h.struct_size = sizeof(h);
        auto stale_page = inline_page("1");
        auto desc = stale_page.desc();
        for (const auto result :
             {weft_view_load(r.runtime, view, &desc), weft_view_advance(r.runtime, view, 1),
              pointer(r, view, WEFT_POINTER_MOVE, 0, 0), weft_view_hit_test(r.runtime, view, 0, 0, &h),
              weft_view_destroy(r.runtime, view), emit(r, view, "e", "1"),
              weft_view_set_model(r.runtime, view, "m", 1, WEFT_VALUE_JSON, "{}", 2),
              weft_view_write_anchors(r.runtime, view, &anchor, 1), weft_view_acquire_frame(r.runtime, view, &frame)}) {
            require(result == WEFT_STALE_HANDLE, "a destroyed View's handle is stale");
        }
        // The slot holds a new View: the old handle stays stale.
        const auto reused = r.view();
        require(reused.slot == view.slot && reused.generation != view.generation, "the slot reused");
        require(emit(r, view, "e", "1") == WEFT_STALE_HANDLE &&
                    weft_view_write_anchors(r.runtime, view, &anchor, 1) == WEFT_STALE_HANDLE &&
                    weft_view_advance(r.runtime, view, 1) == WEFT_STALE_HANDLE && emit(r, reused, "e", "1") == WEFT_OK,
                "an old handle to a reused slot");
        weft_view_destroy(r.runtime, reused);
        // Other threads call while the owner destroys and makes Views: each call is done or stale, never more.
        std::atomic<bool> stop{};
        std::atomic<weft_view_handle> current{r.view()};
        std::atomic<unsigned> calls{};
        std::atomic<bool> bad{};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                while (!stop) {
                    const auto v = current.load();
                    weft_result result{};
                    weft_view_frame* f{};
                    switch (t) {
                        case 0:
                            result = emit(r, v, "e", "1");
                            break;
                        case 1:
                            result = weft_view_write_anchors(r.runtime, v, &anchor, 1);
                            break;
                        case 2:
                            result = weft_view_acquire_frame(r.runtime, v, &f);
                            weft_view_frame_release(f);
                            break;
                        default:
                            result = weft_view_set_model(r.runtime, v, "m", 1, WEFT_VALUE_JSON, "{\"a\":1}", 7);
                            break;
                    }
                    if (result != WEFT_OK && result != WEFT_STALE_HANDLE && result != WEFT_BUSY &&
                        result != WEFT_CAPACITY_EXCEEDED) {
                        bad = true;
                    }
                    ++calls;
                }
            });
        }
        for (int i = 0; i < 40; ++i) {
            const auto v = current.load();
            require(load(r, v, inline_page("1")) == WEFT_OK, "a page while others call");
            require(weft_view_destroy(r.runtime, v) == WEFT_OK, "destroy while others call");
            current = r.view();
        }
        stop = true;
        for (auto& t : threads) {
            t.join();
        }
        require(!bad && calls > 0, "racing calls end done or stale");
        weft_view_destroy(r.runtime, current.load());
        weft_runtime_info runtime_info{};
        runtime_info.struct_size = sizeof(runtime_info);
        require(weft_runtime_get_info(r.runtime, &runtime_info) == WEFT_OK && runtime_info.live_views == 0,
                "no View left");
    }

    void errors_and_faults() {
        Runtime r;
        const auto view = r.view();
        require(load(r, view,
                     inline_page("weft.engine.send('before'); weft.engine.call('pending', 1);"
                                 "throw new Error('broken page')")) == WEFT_SCRIPT_ERROR,
                "a broken page");
        require(drain(r, view).empty() &&
                    weft_view_complete_call(r.runtime, view, 1, 1, WEFT_VALUE_JSON, "1", 1) == WEFT_STALE_HANDLE,
                "a page that failed to load leaves no messages and no open calls");
        auto found = diagnostics(r, view);
        require(found.size() == 1 && found[0].first == WEFT_DIAGNOSTIC_SCRIPT &&
                    found[0].second.find("broken page") != std::string::npos,
                "its diagnostic");
        weft_view_frame* frame{};
        require(load(r, view, inline_page(R"JS(
            weft.engine.on('boom', () => { throw new Error('handler boom'); });
            requestAnimationFrame(() => { throw new Error('frame boom'); });
          )JS")) == WEFT_OK &&
                    weft_view_acquire_frame(r.runtime, view, &frame) == WEFT_OK,
                "a page whose callbacks throw");
        weft_view_frame_release(frame);
        // The load's first advance ran the frame callback.
        found = diagnostics(r, view);
        require(found.size() == 1 && found[0].first == WEFT_DIAGNOSTIC_FRAME &&
                    found[0].second.find("frame boom") != std::string::npos,
                "a frame diagnostic");
        emit(r, view, "boom", "1");
        require(weft_view_advance(r.runtime, view, 0.016) == WEFT_OK, "advance despite the page's errors");
        found = diagnostics(r, view);
        require(found.size() == 1 && found[0].first == WEFT_DIAGNOSTIC_HANDLER &&
                    found[0].second.find("handler boom") != std::string::npos,
                "a handler diagnostic");
        // A frame callback that allocates past the VM's bound faults the View until a page loads: at once in the
        // load's first advance, or in a later one.
        require(load(r, view, inline_page(R"JS(
            requestAnimationFrame(() => { const a = []; for (;;) a.push(new Array(1 << 16).fill(1)); });
          )JS")) == WEFT_FAULTED &&
                    weft_view_advance(r.runtime, view, 0.020) == WEFT_FAULTED,
                "a page that faults as it loads");
        require(load(r, view, inline_page(R"JS(
            let frames = 0;
            requestAnimationFrame(function frame() {
              if (++frames > 1) { const a = []; for (;;) a.push(new Array(1 << 16).fill(1)); }
              requestAnimationFrame(frame);
            });
          )JS")) == WEFT_OK,
                "a page that will fault");
        require(weft_view_advance(r.runtime, view, 0.032) == WEFT_FAULTED &&
                    weft_view_advance(r.runtime, view, 0.048) == WEFT_FAULTED &&
                    pointer(r, view, WEFT_POINTER_MOVE, 0, 0) == WEFT_FAULTED,
                "faulted");
        require(load(r, view, inline_page("weft.engine.send('again')")) == WEFT_OK && drain(r, view).size() == 1 &&
                    weft_view_advance(r.runtime, view, 0.064) == WEFT_OK,
                "a page again");
        weft_view_destroy(r.runtime, view);
    }

    // A stylesheet or keyframes outside the scene profile fail the page as a script that throws does: an empty View,
    // a scene diagnostic that says why, never WEFT_INVALID_ARGUMENT; the transition shorthand is in the profile.
    void stylesheets() {
        Runtime r;
        const auto view = r.view();
        require(load(r, view,
                     inline_page("weft.engine.send('loaded')",
                                 ".b{transition:background-color 0.15s, transform 0.1s}")) == WEFT_OK &&
                    drain(r, view).size() == 1 && diagnostics(r, view).empty(),
                "a page with the transition shorthand");

        struct Rejected {
            Page page;
            const char* why;
        };

        Page frames = inline_page("weft.engine.send('loaded')");
        frames.keyframes = "@keyframes k{from{column-count:1}to{column-count:2}}";
        const Rejected rejected[]{
            {inline_page("weft.engine.send('loaded')", ".b{column-count:2}"), "unsupported property column-count"},
            {inline_page("weft.engine.send('loaded')", ".b{transition:opacity 1s 2s 3s}"),
             "more than a duration and a delay"},
            {inline_page("weft.engine.send('loaded')", ".b{transition:opacity 1s allow-discrete}"), "allow-discrete"},
            {inline_page("weft.engine.send('loaded')", "@media (min-width:1px){.b{opacity:1}}"), "at-rule"},
            {frames, "unsupported keyframe property column-count"},
        };
        for (const auto& [page, why] : rejected) {
            require(load(r, view, page) == WEFT_SCRIPT_ERROR, std::string("a page rejected for ") + why);
            const auto found = diagnostics(r, view);
            require(found.size() == 1 && found[0].first == WEFT_DIAGNOSTIC_SCENE &&
                        found[0].second.find(why) != std::string::npos,
                    std::string("the diagnostic of a page rejected for ") + why);
            require(drain(r, view).empty() && weft_view_advance(r.runtime, view, 0.016) == WEFT_OK &&
                        !hit(r, view, 10, 10).element,
                    std::string("a page rejected for ") + why + " ran no script and has no elements");
        }
        require(load(r, view, inline_page("weft.engine.send('again')")) == WEFT_OK && drain(r, view).size() == 1,
                "a page after a rejected stylesheet");
        weft_view_destroy(r.runtime, view);
    }

    // A 2 x 2 RGBA8 PNG.
    const std::vector<std::uint8_t> dot_png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
        0x24, 0x00, 0x00, 0x00, 0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x9f, 0xe1, 0x3f, 0x43, 0x03, 0x23, 0x98, 0xfe, 0xcf, 0xe0, 0x00, 0x00, 0x3d, 0x23, 0x06, 0xbc,
        0x3a, 0x2b, 0xe2, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    // What a resource reader was asked and found.
    struct Reader {
        weft_runtime* runtime{};
        weft_view_handle view{};
        std::vector<std::string> paths;
        weft_result reentry = WEFT_OK;
        bool arguments{};
    };

    // The reader: dot.png's bytes, bytes that are no PNG for bad.png (after checking the writer's arguments), a reader
    // error for broken.png, nothing for another path; each time it tries to reenter the View.
    weft_result read(void* user, const char* path, size_t path_size, weft_resource_writer* file) {
        auto& self = *static_cast<Reader*>(user);
        self.paths.emplace_back(path, path_size);
        self.reentry = weft_view_advance(self.runtime, self.view, 1);
        const auto& name = self.paths.back();
        if (name.ends_with("dot.png")) {
            return weft_resource_writer_set(file, dot_png.data(), dot_png.size());
        }
        if (name.ends_with("bad.png")) {
            const std::uint8_t bytes[]{1, 2, 3};
            self.arguments =
                weft_resource_writer_set(file, nullptr, 1) == WEFT_INVALID_ARGUMENT &&
                weft_resource_writer_set(nullptr, bytes, 3) == WEFT_INVALID_ARGUMENT &&
                weft_resource_writer_set(file, bytes, std::size_t(16) * 1024 * 1024 + 1) == WEFT_CAPACITY_EXCEEDED;
            return weft_resource_writer_set(file, bytes, sizeof bytes);
        }
        return name.ends_with("broken.png") ? WEFT_INTERNAL_ERROR : WEFT_OK;
    }

    void resources() {
        Runtime r;
        const auto view = r.view();
        Reader reader{r.runtime, view};
        require(weft_view_set_resource_reader(r.runtime, view, read, &reader) == WEFT_OK,
                "a reader before the View's first page");
        auto page = inline_page("for (const id of ['a', 'b']) {"
                                "  const d = document.createElement('div');"
                                "  d.id = id;"
                                "  document.getElementById('root').appendChild(d);"
                                "}",
                                "#a,#b{position:absolute;width:8px;height:8px}"
                                "#a{background-image:url(../img/dot.png)}"
                                "#b{left:10px;background:url('../img/./dot.png') no-repeat}");
        page.base = "ui/page/page.css";
        require(load(r, view, page) == WEFT_OK && reader.paths == std::vector<std::string>{"ui/img/dot.png"} &&
                    diagnostics(r, view).empty(),
                "the page's image read once by its path against the base, decoded");
        require(reader.reentry == WEFT_BUSY, "a reader cannot reenter the View");
        require(weft_view_advance(r.runtime, view, 0.016) == WEFT_OK && reader.paths.size() == 1,
                "no read again in the page load");

        // A missing file, a reader's error and bytes that are no PNG: scene diagnostics, the page goes on.
        page.stylesheet += "#a{background-image:url(missing.png)}#b{background-image:url(/broken.png)}";
        page.stylesheet += "#root{background-image:url(../img/bad.png)}";
        require(load(r, view, page) == WEFT_OK, "a page of images that fail");
        auto found = diagnostics(r, view);
        const auto said = [&](std::string_view text) {
            return std::any_of(found.begin(), found.end(), [&](const auto& d) {
                return d.first == WEFT_DIAGNOSTIC_SCENE && d.second.find(text) != std::string::npos;
            });
        };
        require(found.size() == 3 && said("url(ui/page/missing.png): no such file") &&
                    said("url(/broken.png): no such file") && said("url(ui/img/bad.png): no PNG image"),
                "each failure a scene diagnostic");
        require(reader.arguments, "the writer's arguments");
        weft_view_frame* frame{};
        require(weft_view_acquire_frame(r.runtime, view, &frame) == WEFT_OK, "a frame despite them");
        weft_view_frame_release(frame);

        // A page of the earlier ABI, without a base: URLs resolve from "". A size inside base is no page.
        page = inline_page("const d = document.createElement('div'); d.id = 'a';"
                           "document.getElementById('root').appendChild(d);",
                           "#a{width:8px;height:8px;background-image:url(img/dot.png)}");
        auto d = page.desc();
        d.struct_size = offsetof(weft_page_desc, base) + 8;
        require(weft_view_load(r.runtime, view, &d) == WEFT_INVALID_ARGUMENT, "a size inside base");
        d.struct_size = offsetof(weft_page_desc, base);
        reader.paths.clear();
        require(weft_view_load(r.runtime, view, &d) == WEFT_OK &&
                    reader.paths == std::vector<std::string>{"img/dot.png"} && diagnostics(r, view).empty(),
                "a page without a base");

        // Without a reader, an image is a diagnostic.
        require(weft_view_set_resource_reader(r.runtime, view, nullptr, nullptr) == WEFT_OK &&
                    load(r, view, page) == WEFT_OK,
                "no reader");
        found = diagnostics(r, view);
        require(found.size() == 1 && found[0].second.find("no resource reader") != std::string::npos,
                "an image without a reader");
        weft_view_destroy(r.runtime, view);
        require(weft_view_set_resource_reader(r.runtime, view, read, &reader) == WEFT_STALE_HANDLE,
                "a destroyed View's reader");
    }

    // The web platform's localStorage through the ABI: items a host restores before the first page, what pages write,
    // the revision a host saves by, the quota; console messages as their own diagnostic kinds.
    void web_storage() {
        Runtime r;
        const auto view = r.view();
        using Items = std::vector<std::pair<std::string, std::string>>;
        const auto items = [&](uint64_t revision, uint64_t* out_revision) {
            Items out;
            const auto result = weft_view_get_storage(
                r.runtime, view, revision,
                [](void* user, const char* key, size_t key_size, const char* value, size_t value_size) {
                    static_cast<Items*>(user)->emplace_back(std::string(key, key_size), std::string(value, value_size));
                },
                &out, out_revision);
            require(result == WEFT_OK, "the storage's items");
            return out;
        };
        require(weft_view_set_storage_item(r.runtime, view, "saved", 5, "1", 1) == WEFT_OK,
                "an item before the View's first page");
        uint64_t revision{};
        require(items(0, &revision) == Items{{"saved", "1"}} && revision, "the item and its revision");
        require(items(revision, nullptr).empty(), "nothing when the revision did not move");
        require(weft_view_set_storage_item(r.runtime, view, "bad", 3, "\xff", 1) == WEFT_INVALID_ARGUMENT &&
                    weft_view_set_storage_item(r.runtime, view, nullptr, 1, "x", 1) == WEFT_INVALID_ARGUMENT &&
                    weft_view_set_storage_item(r.runtime, view, "k", 1, nullptr, 1) == WEFT_INVALID_ARGUMENT &&
                    weft_view_get_storage(r.runtime, view, 0, nullptr, nullptr, nullptr) == WEFT_INVALID_ARGUMENT,
                "arguments");
        const std::string big(6u << 20, 'x');
        require(weft_view_set_storage_item(r.runtime, view, "big", 3, big.data(), big.size()) == WEFT_CAPACITY_EXCEEDED,
                "past the quota");

        auto page = inline_page("console.log('read', localStorage.getItem('saved'));"
                                "localStorage.setItem('page', 'wrote');"
                                "console.warn('w'); console.error('e');");
        page.flags = WEFT_PAGE_WEB_PLATFORM;
        require(load(r, view, page) == WEFT_OK, "a page with the web platform");
        const auto found = diagnostics(r, view);
        require(found.size() == 3 && found[0] == std::pair<uint32_t, std::string>{WEFT_DIAGNOSTIC_CONSOLE, "read 1"} &&
                    found[1] == std::pair<uint32_t, std::string>{WEFT_DIAGNOSTIC_CONSOLE_WARNING, "w"} &&
                    found[2] == std::pair<uint32_t, std::string>{WEFT_DIAGNOSTIC_CONSOLE_ERROR, "e"},
                "console messages by level");
        uint64_t after{};
        require(items(revision, &after) == Items{{"page", "wrote"}, {"saved", "1"}} && after != revision,
                "what the page wrote, at a new revision");
        require(weft_view_set_storage_item(r.runtime, view, "saved", 5, nullptr, 0) == WEFT_OK &&
                    items(0, nullptr) == Items{{"page", "wrote"}},
                "a NULL value removes the item");
        require(load(r, view, page) == WEFT_OK && diagnostics(r, view).front().second == "read null",
                "the next page sees the removal");
        require(weft_view_clear_storage(r.runtime, view) == WEFT_OK && items(0, nullptr).empty(), "cleared");
        weft_view_destroy(r.runtime, view);
        require(weft_view_clear_storage(r.runtime, view) == WEFT_STALE_HANDLE, "a destroyed View's storage");
    }

    weft_key_event key_event(uint32_t kind, std::string_view key, std::string_view code, uint32_t modifiers = 0,
                             uint32_t flags = 0) {
        return {sizeof(weft_key_event), kind, key.data(), key.size(), code.data(), code.size(), modifiers, flags, 0, 0};
    }

    std::string evaluate(const Runtime& r, weft_view_handle view, std::string_view source) {
        std::string out;
        require(weft_view_evaluate(
                    r.runtime, view, source.data(), source.size(),
                    [](void* user, uint32_t, const char* text, size_t size) {
                        static_cast<std::string*>(user)->assign(text, size);
                    },
                    &out) == WEFT_OK,
                "evaluate");
        return out;
    }

    // The safe area: env(safe-area-inset-*) in px of the insets the host gives, before the first page or after, again
    // when they change and across loads; another name takes its fallback (names are case-sensitive); bad insets,
    // a name without a fallback and env() in keyframes are refused.
    void safe_area() {
        Runtime r;
        const auto view = r.view();
        require(weft_view_set_safe_area(r.runtime, view, 20, 10, 34, 44) == WEFT_OK,
                "a safe area before the View's first page");
        const auto page = [] {
            return inline_page(
                "for (const id of ['t', 'u', 'r', 'b', 'f', 'p', 'q']) {"
                "  const e = document.createElement('div'); e.id = id; document.getElementById('root').appendChild(e);"
                "}",
                "#t,#u,#r,#b,#f,#p,#q{position:absolute;width:10px;height:10px;background-color:#000}"
                "#t{left:env(safe-area-inset-left);top:env(safe-area-inset-top)}"
                "#u{right:env(SAFE-AREA-INSET-RIGHT, 99px);top:0}"
                "#r{right:env(safe-area-inset-right);top:0}"
                "#b{left:0;bottom:calc(env(safe-area-inset-bottom) + 5px)}"
                "#f{left:env(not-a-variable, 7px);top:env(also-not, env(safe-area-inset-top, 1px))}"
                "#p{left:100px;top:100px;padding:env(safe-area-inset-top) 0 0 env( safe-area-inset-left )}"
                // The same padding in px: the shorthand with env() lays out as it does.
                "#q{left:200px;top:100px;padding:20px 0 0 44px}");
        };
        const auto rects = [&] {
            return evaluate(r, view,
                            "JSON.stringify(['t', 'u', 'r', 'b', 'f', 'p', 'q'].map(id => {"
                            "  const b = document.getElementById(id).getBoundingClientRect();"
                            "  return [b.left, b.top, b.width, b.height]; }))");
        };
        require(load(r, view, page()) == WEFT_OK && weft_view_advance(r.runtime, view, 0.016) == WEFT_OK &&
                    diagnostics(r, view).empty(),
                "a page that reads env()");
        const std::string inset = "[[44,20,10,10],[1171,0,10,10],[1260,0,10,10],[0,671,10,10],[7,20,10,10],"
                                  "[100,100,44,20],[200,100,44,20]]";
        require(rects() == inset, "env() in px of the safe area set before the page (got " + rects() + ")");
        require(weft_view_set_safe_area(r.runtime, view, 0, 0, 0, 0) == WEFT_OK &&
                    weft_view_advance(r.runtime, view, 0.032) == WEFT_OK,
                "a safe area of 0");
        require(rects() == "[[0,0,10,10],[1171,0,10,10],[1270,0,10,10],[0,705,10,10],[7,0,10,10],[100,100,10,10],"
                           "[200,100,44,20]]",
                "the page laid out again when the safe area changes (got " + rects() + ")");
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();
        constexpr float infinity = std::numeric_limits<float>::infinity();
        for (const float bad : {-1.f, nan, infinity}) {
            require(weft_view_set_safe_area(r.runtime, view, 0, bad, 0, 0) == WEFT_INVALID_ARGUMENT,
                    "an inset that is negative or not finite");
        }
        require(weft_view_set_safe_area(r.runtime, view, 20, 10, 34, 44) == WEFT_OK &&
                    load(r, view, page()) == WEFT_OK && weft_view_advance(r.runtime, view, 0.048) == WEFT_OK &&
                    rects() == inset,
                "the safe area kept across page loads (got " + rects() + ")");

        require(load(r, view,
                     inline_page("const e = document.createElement('div'); e.className = 'x';"
                                 "document.getElementById('root').appendChild(e);",
                                 ".x{position:absolute;left:env(nope);width:1px;height:1px}")) == WEFT_OK &&
                    weft_view_advance(r.runtime, view, 0.064) == WEFT_OK,
                "a page with env() of an unknown name");
        const auto found = diagnostics(r, view);
        require(!found.empty() && found[0].first == WEFT_DIAGNOSTIC_SCENE &&
                    found[0].second.find("env(nope)") != std::string::npos,
                "the diagnostic of env() of an unknown name without a fallback");

        Page frames = inline_page("");
        frames.keyframes = "@keyframes k{from{top:env(safe-area-inset-top)}to{top:0}}";
        require(load(r, view, frames) == WEFT_SCRIPT_ERROR, "env() in keyframes refused");
        const auto refused = diagnostics(r, view);
        require(refused.size() == 1 && refused[0].second.find("env() in keyframes") != std::string::npos,
                "the diagnostic of env() in keyframes");

        weft_view_destroy(r.runtime, view);
        require(weft_view_set_safe_area(r.runtime, view, 0, 0, 0, 0) == WEFT_STALE_HANDLE,
                "a safe area of a destroyed View");
    }

    // A sync function that tries the keyboard calls in the middle of JS.
    weft_result key_reentry(void* user, const void*, size_t, weft_value_writer* result) {
        auto& calls = *static_cast<Calls*>(user);
        const auto e = key_event(WEFT_KEY_DOWN, "a", "KeyA");
        calls.reentry = weft_view_key(calls.r->runtime, calls.view, &e, nullptr);
        calls.emitted = weft_view_set_focus(calls.r->runtime, calls.view, 0);
        ++calls.count;
        return weft_value_writer_set(result, WEFT_VALUE_JSON, "1", 1);
    }

    // Keyboard input (weft_view_key, weft_view_text, weft_view_set_focus, weft_keyboard_key, weft_keyboard_code):
    // arguments, what the page did with a key, the host's focus before the first page, reentry, threads and stale handles.
    void keyboard() {
        Runtime r;
        const auto view = r.view();
        // Before a page: nothing to do, but the host's focus is kept for the page.
        auto tab = key_event(WEFT_KEY_DOWN, "Tab", "Tab");
        uint32_t handled = 9;
        require(weft_view_key(r.runtime, view, &tab, &handled) == WEFT_OK && handled == 0, "a key without a page");
        require(weft_view_text(r.runtime, view, "a", 1, &handled) == WEFT_OK && handled == 0, "text without a page");
        require(weft_view_set_focus(r.runtime, view, 0) == WEFT_OK, "the host's focus before a page");
        // Arguments.
        auto e = tab;
        e.struct_size -= 4;
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a short key event");
        e = tab;
        e.kind = 2;
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "an unknown key kind");
        e = tab;
        e.reserved = 1;
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a reserved key field");
        e = tab;
        e.flags = 4;
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "unknown key flags");
        e = tab;
        e.modifiers = 128;
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "unknown modifiers");
        e = key_event(WEFT_KEY_DOWN, "", "KeyA");
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "no key value");
        e = tab;
        e.key = nullptr;
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a key value's text missing");
        const std::string long_key(65, 'a');
        e = key_event(WEFT_KEY_DOWN, long_key, "KeyA");
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a key value past 64 bytes");
        e = key_event(WEFT_KEY_DOWN, "a", long_key);
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a code past 64 bytes");
        e = key_event(WEFT_KEY_DOWN, "\xff", "KeyA");
        require(weft_view_key(r.runtime, view, &e, nullptr) == WEFT_INVALID_ARGUMENT, "a key value of no UTF-8");
        require(weft_view_key(r.runtime, view, nullptr, nullptr) == WEFT_INVALID_ARGUMENT, "no key event");
        require(weft_view_text(r.runtime, view, nullptr, 1, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_text(r.runtime, view, "\xc3", 1, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_text(r.runtime, view, std::string(4097, 'a').data(), 4097, nullptr) ==
                        WEFT_INVALID_ARGUMENT,
                "text: missing, no UTF-8, past 4096 bytes");
        require(weft_view_set_focus(r.runtime, view, 2) == WEFT_INVALID_ARGUMENT, "focus 2");

        // A page: buttons a and b; b's listener prevents x.
        Calls calls{&r, view};
        require(weft_view_register_function(r.runtime, view, "keys", 4, key_reentry, &calls) == WEFT_OK, "a function");
        require(load(r, view, inline_page(R"JS(
            for (const id of ['a', 'b']) {
              const n = document.createElement('button');
              n.setAttribute('id', id);
              document.getElementById('root').appendChild(n);
            }
            document.getElementById('b').addEventListener('keydown', e => { if (e.key === 'x') e.preventDefault(); });
          )JS",
                                          "button{width:20px;height:20px}")) == WEFT_OK,
                "a page");
        require(evaluate(r, view, "document.hasFocus()") == "false", "the host's focus kept from before the page");
        require(weft_view_set_focus(r.runtime, view, 1) == WEFT_OK &&
                    evaluate(r, view, "document.hasFocus()") == "true",
                "the host's focus back");
        require(weft_view_key(r.runtime, view, &tab, &handled) == WEFT_OK && handled == WEFT_INPUT_DEFAULT,
                "Tab moves the focus");
        require(evaluate(r, view, "document.activeElement.id") == "a", "a has the focus");
        auto up = key_event(WEFT_KEY_UP, "Tab", "Tab");
        require(weft_view_key(r.runtime, view, &up, nullptr) == WEFT_OK, "no out_handled");
        weft_view_key(r.runtime, view, &tab, nullptr);
        weft_view_key(r.runtime, view, &up, nullptr);
        auto x = key_event(WEFT_KEY_DOWN, "x", "KeyX");
        require(weft_view_key(r.runtime, view, &x, &handled) == WEFT_OK && handled == WEFT_INPUT_PREVENTED,
                "a key b prevents");
        require(weft_view_text(r.runtime, view, "x", 1, &handled) == WEFT_OK && handled == WEFT_INPUT_DEFAULT,
                "its text dropped");
        auto shift = key_event(WEFT_KEY_DOWN, "Shift", "ShiftLeft", WEFT_MODIFIER_SHIFT, WEFT_KEY_REPEAT);
        require(weft_view_key(r.runtime, view, &shift, &handled) == WEFT_OK && handled == 0, "a repeating Shift");
        // Reentry from a sync function is refused.
        require(evaluate(r, view, "weft.engine.invoke('keys', 0)") == "1" && calls.count == 1 &&
                    calls.reentry == WEFT_BUSY && calls.emitted == WEFT_BUSY,
                "keys from a sync function wait");

        // weft_keyboard_key, from any thread.
        char key[32];
        require(weft_keyboard_key("Enter", 5, 0, key) == WEFT_OK && std::string(key) == "Enter", "Enter's key");
        require(weft_keyboard_key("Numpad1", 7, 0, key) == WEFT_OK && std::string(key) == "End" &&
                    weft_keyboard_key("Numpad1", 7, WEFT_MODIFIER_NUM_LOCK, key) == WEFT_OK && std::string(key) == "1",
                "the numeric keypad");
        require(weft_keyboard_key("Nope", 4, 0, key) == WEFT_INVALID_ARGUMENT && key[0] == 0 &&
                    weft_keyboard_key("KeyA", 4, 128, key) == WEFT_INVALID_ARGUMENT &&
                    weft_keyboard_key(nullptr, 1, 0, key) == WEFT_INVALID_ARGUMENT &&
                    weft_keyboard_key("KeyA", 4, 0, nullptr) == WEFT_INVALID_ARGUMENT,
                "weft_keyboard_key's arguments");
        char code[32];
        require(weft_keyboard_code(0x0D, code) == WEFT_OK && std::string(code) == "Enter" &&
                    weft_keyboard_code(0xA1, code) == WEFT_OK && std::string(code) == "ShiftRight",
                "weft_keyboard_code");
        require(weft_keyboard_code(0xFF, code) == WEFT_INVALID_ARGUMENT && code[0] == 0 &&
                    weft_keyboard_code(0x0D, nullptr) == WEFT_INVALID_ARGUMENT,
                "weft_keyboard_code's arguments");
        weft_result elsewhere{};
        std::string typed;
        weft_result owner_key{};
        std::thread([&] {
            char k[32];
            elsewhere = weft_keyboard_key("KeyQ", 4, 0, k);
            typed = k;
            owner_key = weft_view_key(r.runtime, view, &tab, nullptr);
        }).join();
        require(elsewhere == WEFT_OK && !typed.empty() && owner_key == WEFT_WRONG_THREAD,
                "weft_keyboard_key from another thread; weft_view_key belongs to the owner");
        weft_view_destroy(r.runtime, view);
        require(weft_view_key(r.runtime, view, &tab, nullptr) == WEFT_STALE_HANDLE &&
                    weft_view_text(r.runtime, view, "a", 1, nullptr) == WEFT_STALE_HANDLE &&
                    weft_view_set_focus(r.runtime, view, 1) == WEFT_STALE_HANDLE,
                "a destroyed View's keyboard");
    }

    weft_result compose_reentry(void* user, const void*, size_t, weft_value_writer* result) {
        auto& calls = *static_cast<Calls*>(user);
        calls.reentry = weft_view_composition(calls.r->runtime, calls.view, "a", 1, 1, 1, nullptr);
        weft_text_input input{};
        input.struct_size = sizeof(input);
        calls.emitted = weft_view_text_input(calls.r->runtime, calls.view, &input);
        ++calls.count;
        return weft_value_writer_set(result, WEFT_VALUE_JSON, "1", 1);
    }

    // Text fields and input methods (weft_view_text_input, weft_view_composition, weft_view_commit_text) and the
    // pointer's modifiers: arguments, where text input goes, a composition and its commit, text committed without one,
    // a cancelled composition, a read-only field, Shift and a press, reentry, threads and stale handles.
    void text_input() {
        Runtime r;
        const auto view = r.view();
        weft_text_input input{};
        input.struct_size = sizeof(input);
        uint32_t taken = 9;
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_OK && !input.active, "no page: no text input");
        require(weft_view_composition(r.runtime, view, "a", 1, 1, 1, &taken) == WEFT_OK && taken == 0,
                "no page: no composition");
        require(weft_view_commit_text(r.runtime, view, "a", 1, &taken) == WEFT_OK && taken == 0, "no page: no commit");
        // Arguments.
        input.struct_size = sizeof(input) - 4;
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_INVALID_ARGUMENT &&
                    weft_view_text_input(r.runtime, view, nullptr) == WEFT_INVALID_ARGUMENT,
                "a short text input, none");
        input.struct_size = sizeof(input);
        require(weft_view_composition(r.runtime, view, "ab", 2, 3, 3, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_composition(r.runtime, view, "ab", 2, 0, 3, nullptr) == WEFT_INVALID_ARGUMENT,
                "a selection past the composition");
        const std::string long_text(4097, 'a');
        require(weft_view_composition(r.runtime, view, nullptr, 1, 0, 0, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_composition(r.runtime, view, "\xc3", 1, 0, 0, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_composition(r.runtime, view, long_text.data(), long_text.size(), 0, 0, nullptr) ==
                        WEFT_INVALID_ARGUMENT &&
                    weft_view_commit_text(r.runtime, view, nullptr, 1, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_commit_text(r.runtime, view, "\xc3", 1, nullptr) == WEFT_INVALID_ARGUMENT &&
                    weft_view_commit_text(r.runtime, view, long_text.data(), long_text.size(), nullptr) ==
                        WEFT_INVALID_ARGUMENT,
                "composition and commit text: missing, no UTF-8, past 4096 bytes");
        weft_pointer_event shifted{sizeof(weft_pointer_event), WEFT_POINTER_MOVE, 0, 0, 0, 128, 0, 0};
        require(weft_view_pointer(r.runtime, view, &shifted, nullptr) == WEFT_INVALID_ARGUMENT,
                "unknown pointer modifiers");

        // A page: a field f and a read-only field r, with a log of its composition and input events.
        Calls calls{&r, view};
        require(weft_view_register_function(r.runtime, view, "compose", 7, compose_reentry, &calls) == WEFT_OK,
                "a function");
        require(load(r, view, inline_page(R"JS(
            globalThis.log = [];
            for (const id of ['f', 'r']) {
              const n = document.createElement('input');
              n.setAttribute('id', id);
              if (id === 'r') n.setAttribute('readonly', '');
              document.getElementById('root').appendChild(n);
              for (const type of ['compositionstart', 'compositionend', 'beforeinput', 'input'])
                n.addEventListener(type, e => log.push(type + ':' + (e.data ?? '')));
            }
          )JS",
                                          "input{display:block;width:200px;height:24px;font-size:16px;padding:0;"
                                          "border-width:0}")) == WEFT_OK,
                "a page");
        require(weft_view_advance(r.runtime, view, 0.016) == WEFT_OK, "advance");
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_OK && !input.active, "nothing has the focus");
        evaluate(r, view, "document.getElementById('f').focus()");
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_OK && input.active && !input.composing &&
                    !input.password && input.field_x == 0 && input.field_y == 0 && input.field_width == 200 &&
                    input.field_height == 24 && input.caret_x >= 0 && input.caret_x < 2 && input.caret_height > 10,
                "a focused field takes text input at its caret");
        require(weft_view_composition(r.runtime, view, "ni", 2, 2, 2, &taken) == WEFT_OK && taken == 1 &&
                    weft_view_text_input(r.runtime, view, &input) == WEFT_OK && input.composing && input.caret_x > 5,
                "a composition, the caret after it");
        require(weft_view_composition(r.runtime, view, "\xe4\xbd\xa0", 3, 3, 3, &taken) == WEFT_OK && taken == 1,
                "the composition converted");
        require(weft_view_commit_text(r.runtime, view, "\xe4\xbd\xa0", 3, &taken) == WEFT_OK && taken == 1 &&
                    weft_view_text_input(r.runtime, view, &input) == WEFT_OK && !input.composing,
                "its commit ends it");
        require(weft_view_commit_text(r.runtime, view, "\xe5\xa5\xbd", 3, nullptr) == WEFT_OK, "text committed alone");
        weft_view_composition(r.runtime, view, "x", 1, 1, 1, nullptr);
        weft_view_composition(r.runtime, view, "", 0, 0, 0, nullptr);
        require(evaluate(r, view, "document.getElementById('f').value") == "\xe4\xbd\xa0\xe5\xa5\xbd",
                "the field's value");
        require(evaluate(r, view, "log.join(' ')") ==
                    "compositionstart: beforeinput:ni input:ni beforeinput:\xe4\xbd\xa0 input:\xe4\xbd\xa0 "
                    "beforeinput:\xe4\xbd\xa0 input:\xe4\xbd\xa0 compositionend:\xe4\xbd\xa0 "
                    "beforeinput:\xe5\xa5\xbd input:\xe5\xa5\xbd compositionstart: beforeinput:x input:x beforeinput: "
                    "input: compositionend:",
                "the events, as Chrome gives them: " + evaluate(r, view, "log.join(' ')"));
        // Shift with a press extends the selection to it.
        evaluate(r, view, "document.getElementById('f').setSelectionRange(0, 0)");
        weft_view_advance(r.runtime, view, 0.032);
        shifted = {sizeof(weft_pointer_event), WEFT_POINTER_DOWN, 190, 12, 1, WEFT_MODIFIER_SHIFT, 0, 0};
        require(weft_view_pointer(r.runtime, view, &shifted, nullptr) == WEFT_OK, "a press with Shift");
        shifted.kind = WEFT_POINTER_UP;
        shifted.buttons = 0;
        weft_view_pointer(r.runtime, view, &shifted, nullptr);
        require(evaluate(r, view, "(f => f.selectionStart + ',' + f.selectionEnd)(document.getElementById('f'))") ==
                    "0,2",
                "Shift and a press select up to it");
        // A read-only field takes no input method.
        evaluate(r, view, "document.getElementById('r').focus()");
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_OK && !input.active &&
                    weft_view_composition(r.runtime, view, "a", 1, 1, 1, &taken) == WEFT_OK && taken == 0,
                "a read-only field");
        // Without the host's focus nothing takes text input.
        evaluate(r, view, "document.getElementById('f').focus()");
        weft_view_set_focus(r.runtime, view, 0);
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_OK && !input.active, "the host's focus left");
        weft_view_set_focus(r.runtime, view, 1);
        // Reentry from a sync function is refused.
        require(evaluate(r, view, "weft.engine.invoke('compose', 0)") == "1" && calls.count == 1 &&
                    calls.reentry == WEFT_BUSY && calls.emitted == WEFT_BUSY,
                "text input from a sync function waits");
        weft_result elsewhere{};
        std::thread([&] {
            weft_text_input other{};
            other.struct_size = sizeof(other);
            elsewhere = weft_view_text_input(r.runtime, view, &other);
        }).join();
        require(elsewhere == WEFT_WRONG_THREAD, "text input belongs to the owner");
        weft_view_destroy(r.runtime, view);
        require(weft_view_text_input(r.runtime, view, &input) == WEFT_STALE_HANDLE &&
                    weft_view_composition(r.runtime, view, "a", 1, 0, 0, nullptr) == WEFT_STALE_HANDLE &&
                    weft_view_commit_text(r.runtime, view, "a", 1, nullptr) == WEFT_STALE_HANDLE,
                "a destroyed View's text input");
    }

    weft_gamepad_state gamepad_state(std::string_view id = "Xbox 360 Controller (XInput STANDARD GAMEPAD)") {
        weft_gamepad_state s{};
        s.struct_size = sizeof(s);
        s.connected = 1;
        s.id = id.data();
        s.id_size = id.size();
        return s;
    }

    // A sync function that tries weft_view_gamepad in the middle of JS.
    weft_result gamepad_reentry(void* user, const void*, size_t, weft_value_writer* result) {
        auto& calls = *static_cast<Calls*>(user);
        const auto s = gamepad_state();
        calls.reentry = weft_view_gamepad(calls.r->runtime, calls.view, &s, nullptr);
        ++calls.count;
        return weft_value_writer_set(result, WEFT_VALUE_JSON, "1", 1);
    }

    // Gamepads (weft_view_gamepad): arguments, a state before a page, navigator.getGamepads(), the D-pad as an arrow
    // key on a page with WEFT_PAGE_SPATIAL_NAVIGATION, reentry, threads and stale handles.
    void gamepads() {
        Runtime r;
        const auto view = r.view();
        auto s = gamepad_state();
        uint32_t handled = 9;
        require(weft_view_gamepad(r.runtime, view, &s, &handled) == WEFT_OK && handled == 0, "a state without a page");
        const auto rejects = [&](const weft_gamepad_state& bad, const char* what) {
            handled = 9;
            require(weft_view_gamepad(r.runtime, view, &bad, &handled) == WEFT_INVALID_ARGUMENT && handled == 0, what);
        };
        auto e = s;
        e.struct_size -= 4;
        rejects(e, "a short state");
        e = s;
        e.index = WEFT_GAMEPAD_SLOTS;
        rejects(e, "a fifth slot");
        e = s;
        e.connected = 2;
        rejects(e, "connected 2");
        e = s;
        e.reserved = 1;
        rejects(e, "a reserved field");
        e = s;
        e.pressed = 1u << WEFT_GAMEPAD_BUTTONS;
        rejects(e, "an eighteenth button pressed");
        e = s;
        e.touched = 1u << WEFT_GAMEPAD_BUTTONS;
        rejects(e, "an eighteenth button touched");
        e = s;
        e.id = nullptr;
        rejects(e, "an id's text missing");
        const std::string long_id(129, 'x');
        rejects(gamepad_state(long_id), "an id past 128 bytes");
        rejects(gamepad_state("\xff"), "an id of no UTF-8");
        e = s;
        e.buttons[3] = 1.5f;
        rejects(e, "a button past 1");
        e = s;
        e.buttons[3] = std::numeric_limits<float>::quiet_NaN();
        rejects(e, "a button of NaN");
        e = s;
        e.axes[1] = -1.25f;
        rejects(e, "an axis past -1");
        require(weft_view_gamepad(r.runtime, view, nullptr, nullptr) == WEFT_INVALID_ARGUMENT, "no state");
        e = gamepad_state({});
        require(weft_view_gamepad(r.runtime, view, &e, nullptr) == WEFT_OK, "an empty id");

        // A page with spatial navigation: buttons a and b, one under the other.
        Calls calls{&r, view};
        require(weft_view_register_function(r.runtime, view, "pad", 3, gamepad_reentry, &calls) == WEFT_OK,
                "a function");
        auto page = inline_page(R"JS(
            for (const id of ['a', 'b']) {
              const n = document.createElement('button');
              n.setAttribute('id', id);
              document.getElementById('root').appendChild(n);
            }
            document.getElementById('a').focus();
          )JS",
                                "button{display:block;width:40px;height:20px;margin:10px}");
        page.flags = WEFT_PAGE_SPATIAL_NAVIGATION;
        require(load(r, view, page) == WEFT_OK, "a page with spatial navigation");
        weft_view_advance(r.runtime, view, 0.016);
        s.buttons[13] = 1;
        s.pressed = s.touched = 1u << 13;
        require(weft_view_gamepad(r.runtime, view, &s, &handled) == WEFT_OK && handled == WEFT_INPUT_DEFAULT,
                "the D-pad's down moves the focus");
        require(evaluate(r, view, "document.activeElement.id + ' ' + navigator.getGamepads()[0].id") ==
                    "b Xbox 360 Controller (XInput STANDARD GAMEPAD)",
                "b has the focus; the gamepad shows");
        s.buttons[13] = 0;
        s.pressed = s.touched = 0;
        require(weft_view_gamepad(r.runtime, view, &s, &handled) == WEFT_OK && handled == 0, "the D-pad comes up");
        // Reentry from a sync function is refused.
        require(evaluate(r, view, "weft.engine.invoke('pad', 0)") == "1" && calls.count == 1 &&
                    calls.reentry == WEFT_BUSY,
                "a gamepad from a sync function waits");
        weft_result elsewhere{};
        std::thread([&] { elsewhere = weft_view_gamepad(r.runtime, view, &s, nullptr); }).join();
        require(elsewhere == WEFT_WRONG_THREAD, "gamepads belong to the owner");
        weft_view_destroy(r.runtime, view);
        require(weft_view_gamepad(r.runtime, view, &s, nullptr) == WEFT_STALE_HANDLE, "a destroyed View's gamepad");
    }
} // namespace

int main() {
    try {
        sizes_and_versions();
        without_a_page();
        pages_and_bridge();
        anchors_and_hits();
        threads_and_stale_handles();
        errors_and_faults();
        stylesheets();
        safe_area();
        resources();
        web_storage();
        keyboard();
        text_input();
        gamepads();
        std::cout << "C ABI view tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "C ABI view tests failed: " << e.what() << '\n';
        return 1;
    }
}
