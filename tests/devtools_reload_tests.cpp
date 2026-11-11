// Reloading from the developer tools through the C ABI, as a host has it (docs/devtools.md, "热重载"): a page a tool
// supplies is tried in a new View, and one that does not load leaves the old page as it was, with its scripts' state
// and the engine's calls; the tool's page stands for the host's while the tool is connected; the sheet alone is replaced
// with the scripts' state kept; and the page's files come from the tool.
#include <weft/weft.h>

#include "bridge/engine_channel.hpp"
#include "support/check.hpp"
#include "support/devtools_client.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
    using weft::devtools::Json;
    using weft::testing::require;
    using Tool = weft::testing::DevtoolsClient;

    // What a page tried in another's place must give back when it does not load.
    void check_channel() {
        using weft::bridge::ChannelStatus;
        weft::bridge::EngineChannel channel;
        require(channel.emit_json("first", "1") == ChannelStatus::Ok, "an event");
        require(channel.set_model_json("model", R"({"x":1,"y":2})") == ChannelStatus::Ok, "a model");
        const auto call = channel.open_call();
        auto kept = channel.checkpoint();
        // The page tried: the channel starts over for it, and the engine goes on giving.
        channel.reset();
        require(channel.complete_json(call, true, "1") == ChannelStatus::Stale, "the call is closed meanwhile");
        const auto other = channel.open_call();
        require(channel.emit_json("second", "2") == ChannelStatus::Ok, "an event since");
        require(channel.set_model_json("model", R"({"y":3})") == ChannelStatus::Ok, "a field since");
        channel.restore(std::move(kept));
        require(channel.open_calls() == 1, "the calls open then are open again");
        require(channel.complete_json(other, true, "1") == ChannelStatus::Stale, "the tried page's call is not");
        require(channel.complete_json(call, true, "7") == ChannelStatus::Ok, "the kept page's call completes");
        const auto pending = channel.take();
        require(pending.inputs.size() == 3 && pending.inputs[0].name == "first" && pending.inputs[1].name == "second" &&
                    pending.inputs[2].kind == weft::bridge::EngineInput::Kind::Completion,
                "what waited then, then what was given since");
        require(pending.models.size() == 1 && pending.models[0].fields.size() == 2, "the model's fields");
        for (const auto& [field, value] : pending.models[0].fields) {
            // MessagePack positive fixints.
            require(value.size() == 1 && value[0] == (field == "x" ? 1 : 3), "a field set since is the later value");
        }
    }

    struct Host {
        weft_runtime* runtime{};
        weft_view_handle view{};
        double time{};
        std::vector<std::string> read;
        std::vector<std::pair<std::string, std::uint64_t>> calls;
        std::vector<std::string> diagnostics;

        void step() {
            time += 0.016;
            require(weft_view_advance(runtime, view, time) == WEFT_OK, "advance");
        }

        std::string evaluate(const std::string& source) {
            std::string out;
            require(weft_view_evaluate(
                        runtime, view, source.data(), source.size(),
                        [](void* user, uint32_t, const char* text, size_t size) {
                            static_cast<std::string*>(user)->assign(text, size);
                        },
                        &out) == WEFT_OK,
                    "evaluate " + source);
            return out;
        }

        weft_result load(const std::string& base, const std::string& script, const std::string& sheet) {
            const weft_script scripts[]{{"host.js", 7, script.data(), script.size()}};
            weft_page_desc page{};
            page.struct_size = sizeof page;
            page.flags = WEFT_PAGE_FORGIVING_CSS | WEFT_PAGE_WEB_PLATFORM;
            page.scripts = scripts;
            page.script_count = 1;
            page.stylesheet = sheet.data();
            page.stylesheet_size = sheet.size();
            page.base = base.data();
            page.base_size = base.size();
            return weft_view_load(runtime, view, &page);
        }

        void drain() {
            require(weft_view_drain_messages(
                        runtime, view,
                        [](void* user, weft_view_handle, const weft_message* message) {
                            static_cast<Host*>(user)->calls.emplace_back(std::string(message->name, message->name_size),
                                                                         message->call_id);
                        },
                        this) == WEFT_OK,
                    "messages");
            diagnostics.clear();
            require(weft_view_drain_diagnostics(
                        runtime, view,
                        [](void* user, weft_view_handle, const weft_diagnostic* diagnostic) {
                            static_cast<Host*>(user)->diagnostics.emplace_back(diagnostic->message,
                                                                               diagnostic->message_size);
                        },
                        this, nullptr) == WEFT_OK,
                    "diagnostics");
        }

        bool diagnosed(std::string_view text) const {
            for (const auto& diagnostic : diagnostics) {
                if (diagnostic.find(text) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }
    };

    // Weft.setPage's parameters for a page of one script; with a sheet, a page of the strict profile with that sheet.
    Json::Object page_of(std::string_view name, std::string_view source, std::string_view strict_sheet = {}) {
        Json::Object page{{"scripts", Json::Array{Json::Object{{"name", name}, {"source", source}}}}};
        if (!strict_sheet.empty()) {
            page.emplace_back("forgivingCss", false);
            page.emplace_back("stylesheet", strict_sheet);
        }
        return Json::Object{{"page", std::move(page)}};
    }

    const std::string host_sheet = "#root { position: relative; } #mark { width: 20px; height: 5px; }";
    const std::string host_script = "window.which = 'host'; var m = document.createElement('div'); m.id = 'mark';"
                                    "document.getElementById('root').appendChild(m);";
    const std::string tool_script = "window.which = 'tool'; var m = document.createElement('div'); m.id = 'mark';"
                                    "document.getElementById('root').appendChild(m);";
    const std::string tool2_script = "window.which = 'tool2'; var m = document.createElement('div'); m.id = 'mark';"
                                     "document.getElementById('root').appendChild(m);";
    const std::string width = "String(document.getElementById('mark').getBoundingClientRect().width)";
} // namespace

int main() {
    try {
        check_channel();

        Host host;
        require(weft_runtime_create(nullptr, &host.runtime) == WEFT_OK, "a runtime");
        require(weft_devtools_start(host.runtime, nullptr) == WEFT_OK, "the tools");
        weft_devtools_info info{sizeof info, 0};
        require(weft_devtools_get_info(host.runtime, &info) == WEFT_OK && info.port != 0, "their port");
        const weft_view_desc desc{sizeof desc, 0, 400, 300, 1, 0};
        require(weft_view_create(host.runtime, &desc, &host.view) == WEFT_OK, "a view");
        // The host has none of the page's files.
        require(weft_view_set_resource_reader(
                    host.runtime, host.view,
                    [](void* user, const char* path, size_t size, weft_resource_writer*) {
                        static_cast<Host*>(user)->read.emplace_back(path, size);
                        return WEFT_OK;
                    },
                    &host) == WEFT_OK,
                "a reader");
        require(host.load("/pages/a/page.css", host_script, host_sheet) == WEFT_OK, "the host's page");
        host.step();

        const auto list = weft::testing::devtools_http_get(std::uint16_t(info.port), "/json/list");
        const auto at = list.find("/devtools/page/");
        require(at != std::string::npos, "the view is listed: " + list);
        const auto path = list.substr(at, list.find('"', at) - at);
        const std::function<void()> step = [&] {
            host.step();
        };
        {
            Tool tool(step, std::uint16_t(info.port), path);
            require(tool.head().find("101") != std::string::npos, "connected: " + tool.head());
            tool.result("Runtime.enable");
            tool.result("DOM.getDocument");

            // A page from the tool, in a new View: the storage of the session and the page's time go on.
            host.evaluate("sessionStorage.setItem('kept', 'yes'); window.old = 1");
            const auto before = std::stod(host.evaluate("String(performance.now())"));
            tool.clear();
            auto loaded = tool.result("Weft.setPage", page_of("tool.js", tool_script));
            require(loaded.bool_at("loaded"), "the tool's page loads: " + loaded.dump());
            require(host.evaluate("window.which + String(window.old)") == "toolundefined", "it is the page now");
            require(host.evaluate("sessionStorage.getItem('kept')") == "yes", "the session's storage is kept");
            require(std::stod(host.evaluate("String(performance.now())")) >= before, "time does not start over");
            require(tool.take("Runtime.executionContextsCleared").has_value() &&
                        tool.take("DOM.documentUpdated").has_value(),
                    "the tool is told of the new page");
            require(host.evaluate(width) == "20", "the sheet is the host's still");

            // A page that does not load: the old one stays, with its state and the call it waits for.
            host.evaluate("window.keep = 7; weft.engine.call('slow').then(function (v) { window.answered = v; })");
            host.step();
            host.drain();
            require(host.calls.size() == 1 && host.calls[0].first == "slow", "the page's call");
            tool.clear();
            loaded = tool.result("Weft.setPage",
                                 page_of("broken.js", "window.which = 'broken'; throw new Error('broken page')"));
            require(!loaded.bool_at("loaded") && loaded.string_at("error").find("broken page") != std::string::npos,
                    "a page that throws does not load: " + loaded.dump());
            require(host.evaluate("window.which + String(window.keep)") == "tool7", "the old page stays, as it was");
            require(tool.take("Runtime.exceptionThrown", "broken page").has_value(), "the console says why");
            require(weft_view_complete_call(host.runtime, host.view, host.calls[0].second, 1, WEFT_VALUE_JSON, "42",
                                            2) == WEFT_OK,
                    "the call the old page made is open still");
            host.step();
            require(host.evaluate("String(window.answered)") == "42", "and the page gets its answer");
            // So does one whose sheet no scene can be made with.
            loaded = tool.result("Weft.setPage", page_of("tool.js", tool_script, "#mark { column-count: 2; }"));
            require(!loaded.bool_at("loaded") && loaded.string_at("error").find("column-count") != std::string::npos,
                    "a page whose sheet fails does not load: " + loaded.dump());
            require(host.evaluate("window.which + String(window.keep)") == "tool7", "the old page stays again");

            // The host loads its page again: the tool's stands for it. Another page of the host's is the host's.
            require(host.load("/pages/a/page.css", host_script, host_sheet) == WEFT_OK, "the host's load");
            require(host.evaluate("window.which") == "tool", "the tool's page stands for the host's");
            require(host.load("/pages/b/page.css", "window.which = 'other'", host_sheet) == WEFT_OK, "another page");
            require(host.evaluate("window.which") == "other", "another page is the host's own");
            host.step();

            // While the View shows that other page, what the tool sends is kept for its own page: the page showing
            // is not restyled, replaced or reloaded with it.
            const auto shown = tool.result("Weft.enable");
            require(shown.bool_at("supplied") && !shown.bool_at("suppliedShown") &&
                        shown.string_at("base") == "/pages/b/page.css",
                    "the tool's page waits for its own: " + shown.dump());
            const auto kept_sheet = tool.result(
                "Weft.setStyleSheet", {{"text", "#root { position: relative; } #mark { width: 31px; height: 5px; }"}});
            require(!kept_sheet.bool_at("applied") && kept_sheet.bool_at("deferred"),
                    "a sheet for a page that is not showing is kept: " + kept_sheet.dump());
            require(tool.result("Weft.getPage").string_at("stylesheet") == host_sheet,
                    "the page showing keeps its sheet");
            auto kept_page = tool.result("Weft.setPage", page_of("tool2.js", tool2_script));
            require(!kept_page.bool_at("loaded") && kept_page.bool_at("deferred"),
                    "a page for one that is not showing is kept: " + kept_page.dump());
            tool.result("Page.reload");
            require(host.evaluate("window.which") == "other", "the page showing stays, and a reload is of it");
            // The host loads the page the tool's stands for: what was kept is the page.
            tool.clear();
            require(host.load("/pages/a/page.css", host_script, host_sheet) == WEFT_OK, "the first page again");
            require(host.evaluate("window.which") == "tool2" && host.evaluate(width) == "31",
                    "the page and the sheet kept for it");
            host.step();
            const auto changed = tool.take("Weft.pageChanged");
            require(changed && changed->find("params") && changed->find("params")->bool_at("suppliedShown"),
                    "the tool is told its page shows");

            // A page kept that does not load when its turn comes leaves the host its own, and the tools say why.
            require(host.load("/pages/b/page.css", "window.which = 'other'", host_sheet) == WEFT_OK, "the other page");
            host.step();
            kept_page = tool.result("Weft.setPage", page_of("broken.js", "throw new Error('kept and broken')"));
            require(kept_page.bool_at("deferred"), "a page that would not load is kept all the same: it is not tried");
            tool.clear();
            require(host.load("/pages/a/page.css", host_script, host_sheet) == WEFT_OK, "the host's load succeeds");
            require(host.evaluate("window.which") == "host", "the host's own page loads in its place");
            host.step();
            require(tool.take("Runtime.exceptionThrown", "kept and broken").has_value(), "and the console says why");
            // Mended while its page shows, it is tried and loads.
            loaded = tool.result("Weft.setPage", page_of("tool.js", tool_script));
            require(loaded.bool_at("loaded") && host.evaluate("window.which") == "tool", "the page mended loads");
            require(host.evaluate(width) == "31", "with the sheet the tool gave");
            host.step();

            // The page anew.
            host.evaluate("window.keep = 9");
            tool.result("Page.reload");
            require(host.evaluate("window.which + String(window.keep)") == "toolundefined",
                    "a reload starts the page over");

            // The sheet alone: the scripts' state stays, and the sheet is the page's from then on.
            host.evaluate("window.keep = 11");
            const auto styled = tool.result(
                "Weft.setStyleSheet",
                {{"text",
                  "#root { position: relative; } #mark { width: 77px; height: 5px; background-image: url(dot.png); }"}});
            require(styled.bool_at("applied"), "the sheet is taken: " + styled.dump());
            host.step();
            require(host.evaluate(width + " + String(window.keep)") == "7711",
                    "the new sheet, the same scripts' state");
            tool.result("Page.reload");
            require(host.evaluate(width) == "77", "a reload has the sheet too");

            // The page's files: the host has no dot.png, and says so; the tool's file is read instead of asking it.
            host.step();
            host.drain();
            const auto asked = host.read.size();
            require(asked != 0 && host.read.back() == "/pages/a/dot.png", "the host is asked for the page's file");
            tool.result("Weft.setResources",
                        {{"resources", Json::Array{Json::Object{
                                           {"path", "dot.png"},
                                           {"data", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDw"
                                                    "AEhQGAhKmMIQAAAABJRU5ErkJggg=="}}}}});
            host.step();
            host.drain();
            require(host.read.size() == asked && !host.diagnosed("dot.png"), "the tool's file is the page's");
            require(host.evaluate("String(window.keep)") == "undefined" && host.evaluate("window.which") == "tool",
                    "and the page was not loaded for it");
        }
        // The tool is gone: at the host's next load its page is its own again, and its files.
        for (int i = 0; i < 20; ++i) {
            host.step();
        }
        require(host.load("/pages/a/page.css", host_script, host_sheet) == WEFT_OK, "the host's load");
        require(host.evaluate("window.which") == "host" && host.evaluate(width) == "20",
                "without the tool the host's page is the host's");

        require(weft_view_destroy(host.runtime, host.view) == WEFT_OK, "the view goes");
        require(weft_devtools_stop(host.runtime) == WEFT_OK, "the tools stop");
        require(weft_runtime_destroy(host.runtime) == WEFT_OK, "the runtime goes");
        std::cout << "devtools reload: ok\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
