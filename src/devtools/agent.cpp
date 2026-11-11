#include "devtools/agent.hpp"

#include "base/css_scalar.hpp"
#include "constants/common.hpp"
#include "style/style.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace weft::devtools {
    namespace {
        constexpr int document_id = 1;
        constexpr int method_not_found = -32601;
        constexpr int server_error = -32000;
        constexpr int invalid_params = -32602;
        constexpr std::string_view frame_id = "weft-main";
        constexpr std::string_view loader_id = "weft-loader";
        constexpr std::string_view sheet_id = "weft-sheet";
        constexpr std::string_view inline_prefix = "inline:";
        constexpr std::string_view dropped_prefix = "WEFT_STYLE_DROPPED: ";
        constexpr int context_id = 1;
        constexpr int comment_node_type = 8;
        // The epoch of the overlay's own nodes, which no document has.
        constexpr std::uint64_t overlay_epoch = std::uint64_t(1) << 40;
        constexpr int overlay_label_height = 22;
        constexpr int overlay_label_margin = 4;
        constexpr int top_z_index = 2147483647;

        struct ProtocolError {
            int code{};
            std::string message;
        };

        double now_ms() {
            return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        bool only_space(std::string_view text) {
            return std::all_of(text.begin(), text.end(), [](char c) {
                return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
            });
        }

        // The children the tools show: Chrome leaves out text nodes of white space alone.
        std::vector<const DomSnapshot*> visible(const DomSnapshot& node) {
            std::vector<const DomSnapshot*> out;
            for (const auto& child : node.children) {
                if (child.type != constants::text_node_type || !only_space(child.data)) {
                    out.push_back(&child);
                }
            }
            return out;
        }

        std::string upper(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                return char(std::toupper(c));
            });
            return text;
        }

        // A value in px of a computed style; 0 without one.
        double px(const std::map<std::string, std::string>& style, const std::string& name) {
            const auto it = style.find(name);
            if (it == style.end() || !it->second.ends_with("px")) {
                return 0;
            }
            const auto value = css::parse_number<double>(it->second.substr(0, it->second.size() - 2));
            return value ? *value : 0;
        }

        // UTF-16 length of UTF-8 text, as the protocol's columns count.
        std::size_t utf16_length(std::string_view text) {
            std::size_t out{};
            for (const char c : text) {
                const auto byte = static_cast<unsigned char>(c);
                if ((byte & 0xC0) != 0x80) {
                    out += byte >= 0xF0 ? 2 : 1;
                }
            }
            return out;
        }

        Json quad(const std::array<double, 4>& r) {
            return Json::Array{r[0], r[1], r[2], r[1], r[2], r[3], r[0], r[3]};
        }

        std::string color_text(const Json* color, std::string_view fallback) {
            if (!color || !color->object()) {
                return std::string(fallback);
            }
            char text[64];
            std::snprintf(text, sizeof text, "rgba(%d,%d,%d,%.3f)", int(color->number_at("r")),
                          int(color->number_at("g")), int(color->number_at("b")), color->number_at("a", 1));
            return text;
        }

        // A declaration's value as it is written, !important and all: what the protocol's value is.
        std::string written_value(const WrittenDeclaration& w) {
            const auto colon = w.text.find(':');
            return colon == std::string::npos ? std::string() : css::trim(std::string_view(w.text).substr(colon + 1));
        }

        // A declaration a block holds in a comment, and the comment's place in the block's text.
        struct Commented {
            std::string name;
            std::string value;
            std::size_t start{};
            std::size_t end{};
        };

        // The comments directly in a declaration block (not those of its nested rules) that hold one declaration.
        std::vector<Commented> commented(std::string_view text) {
            std::vector<Commented> out;
            int depth{};
            char quote{};
            for (std::size_t p = 0; p < text.size(); ++p) {
                const char c = text[p];
                if (quote) {
                    if (c == '\\') {
                        ++p;
                    } else if (c == quote) {
                        quote = 0;
                    }
                    continue;
                }
                if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                } else if (c == '/' && p + 1 < text.size() && text[p + 1] == '*') {
                    const auto close = text.find("*/", p + 2);
                    if (close == std::string_view::npos) {
                        break;
                    }
                    auto inner = css::trim(text.substr(p + 2, close - p - 2));
                    if (inner.ends_with(';')) {
                        inner = css::trim(std::string_view(inner).substr(0, inner.size() - 1));
                    }
                    const auto colon = inner.find(':');
                    const auto name = colon == std::string::npos ? std::string() : css::trim(inner.substr(0, colon));
                    const auto value = colon == std::string::npos ? std::string() : css::trim(inner.substr(colon + 1));
                    const bool named = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char n) {
                        return std::isalnum(n) || n == '-' || n == '_';
                    });
                    if (depth == 0 && named && !value.empty() && value.find_first_of(";{}") == std::string::npos) {
                        out.push_back({name, value, p, close + 2});
                    }
                    p = close + 1;
                }
            }
            return out;
        }

        // The bytes of base64 text; none for text that is not base64.
        std::optional<std::vector<std::uint8_t>> base64_bytes(std::string_view text) {
            std::vector<std::uint8_t> out;
            out.reserve(text.size() / 4 * 3);
            std::uint32_t bits{};
            int held{};
            for (const char c : text) {
                int value;
                if (c >= 'A' && c <= 'Z') {
                    value = c - 'A';
                } else if (c >= 'a' && c <= 'z') {
                    value = c - 'a' + 26;
                } else if (c >= '0' && c <= '9') {
                    value = c - '0' + 52;
                } else if (c == '+' || c == '-') {
                    value = 62;
                } else if (c == '/' || c == '_') {
                    value = 63;
                } else if (c == '=' || c == '\n' || c == '\r') {
                    continue;
                } else {
                    return std::nullopt;
                }
                bits = bits << 6 | std::uint32_t(value);
                held += 6;
                if (held >= 8) {
                    held -= 8;
                    out.push_back(std::uint8_t(bits >> held & 0xFF));
                }
            }
            return out;
        }

        std::string px_text(double value) {
            char text[32];
            std::snprintf(text, sizeof text, "%.2fpx", value);
            return text;
        }

        // Evaluates a script and types what it made as the protocol's RemoteObject, in a function so that it leaves
        // nothing in the page; the script runs as global code.
        constexpr std::string_view evaluate_wrapper = R"((function(source){try{
var value=(0,eval)(source),type=typeof value,out={type:type};
if(value===null){out.type='object';out.subtype='null';out.value=null;}
else if(type==='number'){if(!isFinite(value)||Object.is(value,-0)){out.unserializableValue=Object.is(value,-0)?'-0':String(value);}else{out.value=value;}out.description=String(value);}
else if(type==='boolean'||type==='string'){out.value=value;out.description=String(value);}
else if(type==='bigint'){out.unserializableValue=String(value)+'n';out.description=String(value)+'n';}
else if(type==='symbol'||type==='function'){out.description=String(value);}
else if(type==='object'){
var name=value.constructor&&value.constructor.name||'Object';out.className=name;
if(Array.isArray(value)){out.subtype='array';out.description='Array('+value.length+')';}
else if(value.nodeType){out.subtype='node';out.description=(value.localName||value.nodeName)+(value.id?'#'+value.id:'');}
else if(value instanceof Error){out.subtype='error';out.description=String(value.stack||value);}
else{out.description=name;}
var keys=Object.keys(value),props=[];
for(var i=0;i<keys.length&&i<32;++i){var v=value[keys[i]],t=typeof v;
props.push({name:keys[i],type:t==='object'&&v===null?'object':t==='bigint'?'bigint':t,subtype:v===null?'null':undefined,value:t==='object'&&v?(Array.isArray(v)?'Array('+v.length+')':'Object'):String(v)});}
out.preview={type:'object',subtype:out.subtype,description:out.description,overflow:keys.length>32,properties:props};}
return JSON.stringify({ok:true,result:out});
}catch(e){return JSON.stringify({ok:false,text:String(e),stack:e&&e.stack?String(e.stack):''});}})()";

    } // namespace

    Agent::Agent(View& view, std::shared_ptr<Server> server, std::string target, AgentHost host)
        : view_(&view),
          host_(std::move(host)),
          server_(std::move(server)),
          target_(std::move(target)),
          next_id_(document_id + 1) {
    }

    Agent::~Agent() {
        server_->remove_target(target_);
    }

    void Agent::detach() {
        if (highlight_ || picking_) {
            view_->inspect_overlay(std::nullopt);
            view_->inspect_pointer_mode(false);
        }
        highlight_.reset();
        picking_ = false;
    }

    void Agent::sync_page() {
        const auto loads = view_->inspect_loads();
        if (loads != loads_) {
            loads_ = loads;
            read_sheet();
            page_changed();
        }
    }

    std::optional<std::string> Agent::load_page(const ViewPage& page) {
        std::string error;
        if (host_.reload) {
            if (auto* fresh = host_.reload(page, error)) {
                // The View the agent had is gone.
                view_ = fresh;
                view_->inspect_pointer_mode(picking_);
            }
        } else {
            try {
                view_->load(page);
            } catch (const std::exception& e) {
                error = e.what();
            }
        }
        sync_page();
        return error.empty() ? std::nullopt : std::optional<std::string>(std::move(error));
    }

    Agent::Session* Agent::find_session(std::uint64_t connection) {
        for (auto& session : sessions_) {
            if (session.connection == connection) {
                return &session;
            }
        }
        return nullptr;
    }

    void Agent::forget(std::uint64_t connection) {
        std::erase_if(sessions_, [&](const Session& session) {
            return session.connection == connection;
        });
        // What the connection supplied goes with it: the host's next load of the page is the host's.
        if (supplied_ && supplied_by_ == connection) {
            supplied_.reset();
            view_->inspect_clear_resources();
        }
        if (sessions_.empty()) {
            detach();
        }
    }

    void Agent::send(const Session& session, std::string_view method, Json::Object params) {
        server_->send(session.connection, Json(Json::Object{{"method", method}, {"params", std::move(params)}}).dump());
    }

    void Agent::pump() {
        sync_page();
        for (auto& event : server_->take_events(target_)) {
            switch (event.kind) {
                case ServerEvent::Kind::Opened:
                    sessions_.push_back({event.connection});
                    break;
                case ServerEvent::Kind::Closed:
                    forget(event.connection);
                    break;
                case ServerEvent::Kind::Message:
                    if (auto* session = find_session(event.connection)) {
                        dispatch(*session, event.text);
                    }
                    break;
            }
        }
        if (sessions_.empty()) {
            return;
        }
        refresh_document();
        for (auto& session : sessions_) {
            forward_log(session);
        }
        update_picking();
        update_overlay();
    }

    void Agent::dispatch(Session& session, const std::string& text) {
        static const std::unordered_map<std::string_view, Handler> handlers{
            {"DOM.enable", &Agent::dom_enable},
            {"DOM.getDocument", &Agent::dom_get_document},
            {"DOM.requestChildNodes", &Agent::dom_request_child_nodes},
            {"DOM.describeNode", &Agent::dom_describe_node},
            {"DOM.pushNodesByBackendIdsToFrontend", &Agent::dom_push_nodes},
            {"DOM.getBoxModel", &Agent::dom_get_box_model},
            {"DOM.getNodeForLocation", &Agent::dom_get_node_for_location},
            {"DOM.getOuterHTML", &Agent::dom_get_outer_html},
            {"DOM.setAttributeValue", &Agent::dom_set_attribute_value},
            {"DOM.setAttributesAsText", &Agent::dom_set_attributes_as_text},
            {"DOM.removeAttribute", &Agent::dom_remove_attribute},
            {"DOM.setNodeValue", &Agent::dom_set_node_value},
            {"DOM.removeNode", &Agent::dom_remove_node},
            {"DOM.setInspectedNode", &Agent::dom_set_inspected_node},
            {"CSS.enable", &Agent::css_enable},
            {"CSS.getMatchedStylesForNode", &Agent::css_get_matched_styles},
            {"CSS.getInlineStylesForNode", &Agent::css_get_inline_styles},
            {"CSS.getComputedStyleForNode", &Agent::css_get_computed_style},
            {"CSS.getStyleSheetText", &Agent::css_get_style_sheet_text},
            {"CSS.setStyleTexts", &Agent::css_set_style_texts},
            {"CSS.setStyleSheetText", &Agent::css_set_style_sheet_text},
            {"CSS.setRuleSelector", &Agent::css_set_rule_selector},
            {"CSS.addRule", &Agent::css_add_rule},
            {"CSS.createStyleSheet", &Agent::css_create_style_sheet},
            {"Page.reload", &Agent::page_reload},
            {"Weft.enable", &Agent::weft_enable},
            {"Weft.getPage", &Agent::weft_get_page},
            {"Weft.setPage", &Agent::weft_set_page},
            {"Weft.setStyleSheet", &Agent::weft_set_style_sheet},
            {"Weft.setResources", &Agent::weft_set_resources},
            {"Weft.getSnapshot", &Agent::weft_get_snapshot},
            {"CSS.getPlatformFontsForNode", &Agent::css_get_platform_fonts},
            {"CSS.getAnimatedStylesForNode", &Agent::css_get_animated_styles},
            {"CSS.getEnvironmentVariables", &Agent::css_get_environment_variables},
            {"CSS.takeComputedStyleUpdates", &Agent::css_take_computed_style_updates},
            {"Network.emulateNetworkConditionsByRule", &Agent::network_emulate_by_rule},
            {"DOM.querySelector", &Agent::dom_query_selector},
            {"DOM.querySelectorAll", &Agent::dom_query_selector_all},
            {"DOM.resolveNode", &Agent::dom_resolve_node},
            {"Overlay.highlightNode", &Agent::overlay_highlight_node},
            {"Overlay.hideHighlight", &Agent::overlay_hide_highlight},
            {"Overlay.setInspectMode", &Agent::overlay_set_inspect_mode},
            {"Runtime.enable", &Agent::runtime_enable},
            {"Runtime.evaluate", &Agent::runtime_evaluate},
            {"Runtime.discardConsoleEntries", &Agent::runtime_discard_console_entries},
            {"Runtime.getIsolateId", &Agent::runtime_get_isolate_id},
            {"Log.enable", &Agent::log_enable},
            {"Page.getResourceTree", &Agent::page_get_resource_tree},
            {"Page.getResourceContent", &Agent::page_get_resource_content},
            {"Debugger.enable", &Agent::debugger_enable},
            {"Weft.getCompatibilityReport", &Agent::weft_get_compatibility_report},
        };
        // What the front end asks for as it starts, of features that have nothing to report yet. Anything else is an
        // error: the front end should not take a change as made when none was.
        static const std::set<std::string_view> accepted{
            "Accessibility.enable",
            "Animation.enable",
            "Audits.enable",
            "Autofill.enable",
            "Autofill.setAddresses",
            "CSS.trackComputedStyleUpdates",
            "CSS.trackComputedStyleUpdatesForNode",
            "DOM.disable",
            "DOM.discardSearchResults",
            "DOM.markUndoableState",
            "DOMDebugger.setBreakOnCSPViolation",
            "DOMStorage.enable",
            "Debugger.setAsyncCallStackDepth",
            "Debugger.setBlackboxPatterns",
            "Debugger.setPauseOnExceptions",
            "Emulation.setEmulatedMedia",
            "Emulation.setEmulatedVisionDeficiency",
            "Emulation.setFocusEmulationEnabled",
            "Emulation.setTouchEmulationEnabled",
            "HeapProfiler.enable",
            "IndexedDB.enable",
            "Inspector.enable",
            "Log.startViolationsReport",
            "Log.stopViolationsReport",
            "Network.configureDurableMessages",
            "Network.overrideNetworkState",
            "Network.enable",
            "Network.setAttachDebugStack",
            "Network.setBlockedURLs",
            "Network.setCacheDisabled",
            "Network.setBypassServiceWorker",
            "Overlay.disable",
            "Overlay.enable",
            "Overlay.setPausedInDebuggerMessage",
            "Overlay.setShowAdHighlights",
            "Overlay.setShowContainerQueryOverlays",
            "Overlay.setShowDebugBorders",
            "Overlay.setShowFPSCounter",
            "Overlay.setShowFlexOverlays",
            "Overlay.setShowGridOverlays",
            "Overlay.setShowHinge",
            "Overlay.setShowIsolatedElements",
            "Overlay.setShowLayoutShiftRegions",
            "Overlay.setShowPaintRects",
            "Overlay.setShowScrollBottleneckRects",
            "Overlay.setShowScrollSnapOverlays",
            "Overlay.setShowViewportSizeOnResize",
            "Overlay.setShowWebVitals",
            "Page.addScriptToEvaluateOnNewDocument",
            "Page.bringToFront",
            "Page.enable",
            "Page.getAdScriptAncestry",
            "Page.setAdBlockingEnabled",
            "Page.setBypassCSP",
            "Page.setInterceptFileChooserDialog",
            "Page.setLifecycleEventsEnabled",
            "Page.startScreencast",
            "Page.stopLoading",
            "Page.stopScreencast",
            "Performance.enable",
            "Preload.enable",
            "Profiler.enable",
            "Runtime.addBinding",
            "Runtime.releaseObject",
            "Runtime.releaseObjectGroup",
            "Runtime.runIfWaitingForDebugger",
            "Runtime.setAsyncCallStackDepth",
            "Runtime.setCustomObjectFormatterEnabled",
            "Security.enable",
            "ServiceWorker.enable",
            "Storage.getStorageKey",
            "Target.setAutoAttach",
            "Target.setDiscoverTargets",
            "Target.setRemoteLocations",
        };
        const auto message = Json::parse(text);
        if (!message || !message->number_at("id", -1) || message->number_at("id", -1) < 0) {
            return;
        }
        const double id = message->number_at("id");
        const auto method = message->string_at("method");
        const Json empty{Json::Object{}};
        const auto* params = message->find("params");
        Json::Object reply{{"id", id}};
        const auto failed = [&](int code, std::string_view why) {
            reply.emplace_back("error", Json::Object{{"code", code}, {"message", why}});
        };
        try {
            if (const auto handler = handlers.find(method); handler != handlers.end()) {
                reply.emplace_back("result", (this->*handler->second)(session, params ? *params : empty));
            } else if (accepted.contains(method) || method.ends_with(".disable")) {
                reply.emplace_back("result", Json::Object{});
            } else {
                failed(method_not_found, "'" + method + "' wasn't found");
            }
        } catch (const ProtocolError& e) {
            failed(e.code, e.message);
        } catch (const std::exception& e) {
            failed(server_error, e.what());
        }
        server_->send(session.connection, Json(std::move(reply)).dump());
    }

    // ---- The document ----

    int Agent::id_of(const NodeIdentity& identity) {
        const auto [it, added] =
            ids_.try_emplace(Key{identity.slot, identity.generation, identity.view_epoch}, next_id_);
        if (added) {
            ++next_id_;
        }
        return it->second;
    }

    void Agent::reindex() {
        index_.clear();
        if (!document_) {
            return;
        }
        const auto visit = [&](auto&& self, const DomSnapshot& node, const DomSnapshot* parent, std::size_t i) -> void {
            index_[id_of(node.identity)] = {&node, parent, i};
            for (std::size_t k = 0; k < node.children.size(); ++k) {
                self(self, node.children[k], &node, k);
            }
        };
        visit(visit, *document_, nullptr, 0);
    }

    void Agent::ensure_document() {
        const auto revision = view_->inspect_document_revision();
        if (document_ && revision == document_revision_) {
            return;
        }
        auto next = view_->inspect_document();
        if (next && document_) {
            for (auto& session : sessions_) {
                if (session.dom && session.children_sent.contains(document_id)) {
                    diff(*document_, *next, session);
                }
            }
        }
        document_ = std::move(next);
        document_revision_ = revision;
        last_refresh_ = view_->time();
        reindex();
    }

    void Agent::refresh_document() {
        const bool wanted = std::any_of(sessions_.begin(), sessions_.end(), [](const Session& session) {
            return session.dom;
        });
        if (wanted &&
            (last_refresh_ < 0 || view_->time() - last_refresh_ >= constants::devtools::mutation_interval_ms)) {
            ensure_document();
        }
    }

    void Agent::page_changed() {
        document_.reset();
        document_revision_ = 0;
        index_.clear();
        highlight_.reset();
        overlay_key_.clear();
        for (auto& session : sessions_) {
            session.children_sent.clear();
            session.inspected = 0;
            if (session.runtime) {
                send(session, "Runtime.executionContextsCleared", {});
                send(session, "Runtime.executionContextCreated",
                     {{"context",
                       Json::Object{{"id", context_id},
                                    {"origin", ""},
                                    {"name", ""},
                                    {"uniqueId", std::to_string(loads_)},
                                    {"auxData",
                                     Json::Object{{"isDefault", true}, {"type", "default"}, {"frameId", frame_id}}}}}});
            }
            if (session.css) {
                send(session, "CSS.styleSheetRemoved", {{"styleSheetId", sheet_id}});
                add_sheet(session);
            }
            if (session.dom) {
                send(session, "DOM.documentUpdated", {});
            }
            if (session.log) {
                send_sheet_problems(session);
            }
            if (session.weft) {
                send(session, "Weft.pageChanged", page_state());
            }
        }
        server_->set_target({target_, sheet_name_.empty() ? "Weft page" : sheet_name_, sheet_header_url()});
        if (std::exchange(announce_, false)) {
            std::fprintf(
                stderr,
                "[weft devtools] open devtools://devtools/bundled/devtools_app.html?ws=127.0.0.1:%u/devtools/page/%s\n",
                unsigned{server_->port()}, target_.c_str());
        }
    }

    const Agent::Located& Agent::located(const Json& params) {
        ensure_document();
        auto id = int(params.number_at("nodeId"));
        if (!id) {
            id = int(params.number_at("backendNodeId"));
        }
        const auto it = index_.find(id);
        if (it == index_.end()) {
            throw ProtocolError{invalid_params, "Could not find node with given id"};
        }
        return it->second;
    }

    Json::Array Agent::children_json(const DomSnapshot& node, int depth, Session& session) {
        Json::Array out;
        session.children_sent.insert(id_of(node.identity));
        for (const auto* child : visible(node)) {
            out.push_back(node_json(*child, depth < 0 ? depth : depth - 1, session));
        }
        return out;
    }

    Json Agent::node_json(const DomSnapshot& node, int depth, Session& session) {
        const int id = id_of(node.identity);
        const bool element = node.type == constants::element_node_type;
        const auto children = visible(node);
        Json::Object out{{"nodeId", id},
                         {"backendNodeId", id},
                         {"nodeType", node.type},
                         {"nodeName", element                                  ? upper(node.local_name)
                                      : node.type == constants::text_node_type ? std::string("#text")
                                      : node.type == comment_node_type         ? std::string("#comment")
                                                                               : node.local_name},
                         {"localName", element ? node.local_name : std::string()},
                         {"nodeValue", element ? std::string() : node.data},
                         {"childNodeCount", children.size()}};
        if (element) {
            Json::Array attributes;
            for (const auto& [name, value] : node.attributes) {
                attributes.emplace_back(name);
                attributes.emplace_back(value);
            }
            out.emplace_back("attributes", std::move(attributes));
        }
        // The front end shows a lone text child in its element's line: it comes along, as Chrome sends it.
        const bool lone_text = children.size() == 1 && children[0]->type == constants::text_node_type;
        if (depth != 0 || lone_text) {
            out.emplace_back("children", children_json(node, depth != 0 ? depth : 1, session));
        }
        return out;
    }

    void Agent::forget_children(const DomSnapshot& node, Session& session) {
        session.children_sent.erase(id_of(node.identity));
        for (const auto& child : node.children) {
            forget_children(child, session);
        }
    }

    void Agent::diff(const DomSnapshot& old, const DomSnapshot& now, Session& session) {
        if (old.revision && old.revision == now.revision) {
            return;
        }
        const int id = id_of(now.identity);
        if (old.type == constants::element_node_type) {
            for (const auto& [name, value] : now.attributes) {
                const auto was = old.attributes.find(name);
                if (was == old.attributes.end() || was->second != value) {
                    send(session, "DOM.attributeModified", {{"nodeId", id}, {"name", name}, {"value", value}});
                }
            }
            for (const auto& [name, value] : old.attributes) {
                if (!now.attributes.contains(name)) {
                    send(session, "DOM.attributeRemoved", {{"nodeId", id}, {"name", name}});
                }
            }
        } else if (old.data != now.data) {
            send(session, "DOM.characterDataModified", {{"nodeId", id}, {"characterData", now.data}});
        }
        const auto before = visible(old);
        const auto after = visible(now);
        if (!session.children_sent.contains(id)) {
            if (before.size() != after.size()) {
                send(session, "DOM.childNodeCountUpdated", {{"nodeId", id}, {"childNodeCount", after.size()}});
            }
            return;
        }
        const auto holds = [](const std::vector<const DomSnapshot*>& list, const NodeIdentity& identity) {
            return std::any_of(list.begin(), list.end(), [&](const DomSnapshot* n) {
                return n->identity == identity;
            });
        };
        // What stays, in each order: when they differ, what moved goes and comes back.
        std::vector<NodeIdentity> kept_before;
        std::vector<NodeIdentity> kept_after;
        for (const auto* n : before) {
            if (holds(after, n->identity)) {
                kept_before.push_back(n->identity);
            }
        }
        for (const auto* n : after) {
            if (holds(before, n->identity)) {
                kept_after.push_back(n->identity);
            }
        }
        const bool reordered = kept_before != kept_after;
        for (const auto* n : before) {
            if (reordered || !holds(after, n->identity)) {
                send(session, "DOM.childNodeRemoved", {{"parentNodeId", id}, {"nodeId", id_of(n->identity)}});
                forget_children(*n, session);
            }
        }
        int previous{};
        for (const auto* n : after) {
            const auto was = std::find_if(before.begin(), before.end(), [&](const DomSnapshot* b) {
                return b->identity == n->identity;
            });
            if (reordered || was == before.end()) {
                send(session, "DOM.childNodeInserted",
                     {{"parentNodeId", id}, {"previousNodeId", previous}, {"node", node_json(*n, 0, session)}});
            } else {
                diff(**was, *n, session);
            }
            previous = id_of(n->identity);
        }
    }

    void Agent::push_path(int id, Session& session) {
        std::vector<const Located*> chain;
        for (auto it = index_.find(id); it != index_.end();) {
            chain.push_back(&it->second);
            it = it->second.parent ? index_.find(id_of(it->second.parent->identity)) : index_.end();
        }
        std::reverse(chain.begin(), chain.end());
        for (const auto* at : chain) {
            if (!at->parent) {
                continue;
            }
            const int parent = id_of(at->parent->identity);
            if (!session.children_sent.contains(parent)) {
                auto nodes = children_json(*at->parent, 0 + 1, session);
                send(session, "DOM.setChildNodes", {{"parentId", parent}, {"nodes", std::move(nodes)}});
            }
        }
    }

    std::string Agent::node_expression(int id) {
        if (id == document_id) {
            return "document";
        }
        const auto& at = index_.at(id);
        if (!at.parent) {
            return "document.documentElement";
        }
        return node_expression(id_of(at.parent->identity)) + ".childNodes[" + std::to_string(at.index) + "]";
    }

    std::string Agent::run(const std::string& script) {
        auto [ok, text] = view_->evaluate(script);
        if (!ok) {
            throw ProtocolError{server_error, text};
        }
        return text;
    }

    std::string Agent::outer_html(const DomSnapshot& node) const {
        const auto escape = [](std::string_view text, bool attribute) {
            std::string out;
            for (const char c : text) {
                out += c == '&'                ? std::string("&amp;")
                       : c == '<'              ? std::string("&lt;")
                       : c == '>'              ? std::string("&gt;")
                       : c == '"' && attribute ? std::string("&quot;")
                                               : std::string(1, c);
            }
            return out;
        };
        if (node.type == constants::text_node_type) {
            return escape(node.data, false);
        }
        if (node.type == comment_node_type) {
            return "<!--" + node.data + "-->";
        }
        std::string out = "<" + node.local_name;
        for (const auto& [name, value] : node.attributes) {
            out += " " + name + "=\"" + escape(value, true) + "\"";
        }
        out += ">";
        for (const auto& child : node.children) {
            out += outer_html(child);
        }
        return out + "</" + node.local_name + ">";
    }

    Json Agent::dom_enable(Session& session, const Json&) {
        session.dom = true;
        return Json::Object{};
    }

    Json Agent::dom_get_document(Session& session, const Json& params) {
        session.dom = true;
        ensure_document();
        const int depth = int(params.number_at("depth", 1));
        Json::Object out{{"nodeId", document_id},
                         {"backendNodeId", document_id},
                         {"nodeType", constants::document_node_type},
                         {"nodeName", "#document"},
                         {"localName", ""},
                         {"nodeValue", ""},
                         {"childNodeCount", document_ ? 1 : 0},
                         {"documentURL", sheet_header_url()},
                         {"baseURL", sheet_header_url()},
                         {"xmlVersion", ""},
                         {"compatibilityMode", "NoQuirksMode"}};
        Json::Array children;
        session.children_sent.clear();
        session.children_sent.insert(document_id);
        if (document_) {
            children.push_back(node_json(*document_, depth < 0 ? depth : std::max(depth - 1, 0), session));
        }
        out.emplace_back("children", std::move(children));
        return Json::Object{{"root", std::move(out)}};
    }

    void Agent::push_children(const DomSnapshot& node, int depth, Session& session) {
        // Children sent again would be new nodes to the front end, and what it holds of the old ones would be lost.
        const int id = id_of(node.identity);
        if (session.children_sent.contains(id)) {
            if (depth == 1) {
                return;
            }
            for (const auto* child : visible(node)) {
                push_children(*child, depth < 0 ? depth : depth - 1, session);
            }
            return;
        }
        auto nodes = children_json(node, depth, session);
        send(session, "DOM.setChildNodes", {{"parentId", id}, {"nodes", std::move(nodes)}});
    }

    Json Agent::dom_request_child_nodes(Session& session, const Json& params) {
        push_children(*located(params).node, int(params.number_at("depth", 1)), session);
        return Json::Object{};
    }

    Json Agent::dom_describe_node(Session& session, const Json& params) {
        const auto& at = located(params);
        return Json::Object{{"node", node_json(*at.node, int(params.number_at("depth", 0)), session)}};
    }

    Json Agent::dom_push_nodes(Session& session, const Json& params) {
        ensure_document();
        Json::Array out;
        if (const auto* ids = params.find("backendNodeIds"); ids && ids->array()) {
            for (const auto& item : *ids->array()) {
                const int id = item.number() ? int(*item.number()) : 0;
                if (index_.contains(id)) {
                    push_path(id, session);
                    out.emplace_back(id);
                } else {
                    out.emplace_back(0);
                }
            }
        }
        return Json::Object{{"nodeIds", std::move(out)}};
    }

    Json Agent::dom_get_box_model(Session&, const Json& params) {
        const auto& at = located(params);
        const auto boxes = boxes_of(at.node->identity);
        if (!boxes) {
            throw ProtocolError{server_error, "Could not compute box model."};
        }
        return Json::Object{{"model", Json::Object{{"content", quad(boxes->content)},
                                                   {"padding", quad(boxes->padding)},
                                                   {"border", quad(boxes->border)},
                                                   {"margin", quad(boxes->margin)},
                                                   {"width", boxes->border[2] - boxes->border[0]},
                                                   {"height", boxes->border[3] - boxes->border[1]}}}};
    }

    Json Agent::dom_get_node_for_location(Session& session, const Json& params) {
        ensure_document();
        const auto hit = originating(view_->inspect_hit(float(params.number_at("x")), float(params.number_at("y"))));
        const int id = id_of(hit);
        if (!index_.contains(id)) {
            throw ProtocolError{server_error, "No node found at given location"};
        }
        push_path(id, session);
        return Json::Object{{"backendNodeId", id}, {"frameId", frame_id}, {"nodeId", id}};
    }

    Json Agent::dom_get_outer_html(Session&, const Json& params) {
        return Json::Object{{"outerHTML", outer_html(*located(params).node)}};
    }

    Json Agent::dom_set_attribute_value(Session&, const Json& params) {
        const auto& at = located(params);
        run(node_expression(id_of(at.node->identity)) + ".setAttribute(" + Json(params.string_at("name")).dump() + "," +
            Json(params.string_at("value")).dump() + ")");
        return Json::Object{};
    }

    Json Agent::dom_set_attributes_as_text(Session&, const Json& params) {
        const auto& at = located(params);
        const auto element = node_expression(id_of(at.node->identity));
        // The attributes as markup writes them, which replace the one named.
        const auto text = params.string_at("text");
        std::string script;
        if (const auto name = params.string_at("name"); !name.empty()) {
            script += element + ".removeAttribute(" + Json(name).dump() + ");";
        }
        std::size_t p{};
        const auto space = [&] {
            while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) {
                ++p;
            }
        };
        for (space(); p < text.size(); space()) {
            const auto start = p;
            while (p < text.size() && text[p] != '=' && !std::isspace(static_cast<unsigned char>(text[p]))) {
                ++p;
            }
            const auto name = text.substr(start, p - start);
            std::string value;
            space();
            if (p < text.size() && text[p] == '=') {
                ++p;
                space();
                if (p < text.size() && (text[p] == '"' || text[p] == '\'')) {
                    const char quote = text[p++];
                    const auto end = std::min(text.find(quote, p), text.size());
                    value = text.substr(p, end - p);
                    p = end + 1;
                } else {
                    const auto from = p;
                    while (p < text.size() && !std::isspace(static_cast<unsigned char>(text[p]))) {
                        ++p;
                    }
                    value = text.substr(from, p - from);
                }
            }
            if (name.empty()) {
                ++p;
                continue;
            }
            script += element + ".setAttribute(" + Json(name).dump() + "," + Json(value).dump() + ");";
        }
        run(script);
        return Json::Object{};
    }

    Json Agent::dom_remove_attribute(Session&, const Json& params) {
        const auto& at = located(params);
        run(node_expression(id_of(at.node->identity)) + ".removeAttribute(" + Json(params.string_at("name")).dump() +
            ")");
        return Json::Object{};
    }

    Json Agent::dom_set_node_value(Session&, const Json& params) {
        const auto& at = located(params);
        run(node_expression(id_of(at.node->identity)) + ".nodeValue=" + Json(params.string_at("value")).dump());
        return Json::Object{};
    }

    Json Agent::dom_remove_node(Session&, const Json& params) {
        const auto& at = located(params);
        const auto node = node_expression(id_of(at.node->identity));
        run(node + ".parentNode.removeChild(" + node + ")");
        return Json::Object{};
    }

    std::vector<int> Agent::query(Session& session, const Json& params, bool all) {
        ensure_document();
        const bool from_document = int(params.number_at("nodeId")) == document_id;
        const DomSnapshot* start = from_document && document_ ? &*document_ : located(params).node;
        const auto root = from_document ? std::string("document") : node_expression(id_of(start->identity));
        // The matches as paths of childNodes indices from the element the copy of the document starts at, each after a
        // mark, as that element's own path is empty.
        const auto text =
            run("(function(root,selector,all){var stop=root===document?document.documentElement:root;"
                "var list=all?[].slice.call(root.querySelectorAll(selector)):"
                "[root.querySelector(selector)];return list.filter(Boolean).map(function(e){var p=[];"
                "while(e!==stop){p.unshift([].indexOf.call(e.parentNode.childNodes,e));e=e.parentNode;}"
                "return '='+p.join(',');}).join(';');})(" +
                root + "," + Json(params.string_at("selector")).dump() + "," + (all ? "true" : "false") + ")");
        std::vector<int> out;
        for (const auto& path : css::split(text, ';').parts) {
            if (path.empty()) {
                continue;
            }
            const DomSnapshot* node = start;
            for (const auto& step : css::split(std::string_view(path).substr(1), ',').parts) {
                if (!step.empty()) {
                    node = &node->children.at(std::size_t(std::atoi(step.c_str())));
                }
            }
            const int id = id_of(node->identity);
            push_path(id, session);
            out.push_back(id);
        }
        return out;
    }

    Json Agent::dom_query_selector(Session& session, const Json& params) {
        const auto found = query(session, params, false);
        return Json::Object{{"nodeId", found.empty() ? 0 : found.front()}};
    }

    Json Agent::dom_query_selector_all(Session& session, const Json& params) {
        Json::Array ids;
        for (const int id : query(session, params, true)) {
            ids.emplace_back(id);
        }
        return Json::Object{{"nodeIds", std::move(ids)}};
    }

    Json Agent::dom_resolve_node(Session&, const Json& params) {
        const auto& at = located(params);
        return Json::Object{{"object", Json::Object{{"type", "object"},
                                                    {"subtype", "node"},
                                                    {"className", "HTMLElement"},
                                                    {"description", at.node->local_name}}}};
    }

    Json Agent::dom_set_inspected_node(Session& session, const Json& params) {
        session.inspected = int(params.number_at("nodeId"));
        return Json::Object{};
    }

    // ---- Styles ----

    void Agent::read_sheet() {
        sheet_text_.clear();
        line_starts_.assign(1, 0);
        sheet_name_ = view_->inspect_sheet_name();
        if (const auto* style = view_->inspect_style()) {
            sheet_text_ = style->source_text();
            for (std::size_t i = 0; i < sheet_text_.size(); ++i) {
                if (sheet_text_[i] == '\n') {
                    line_starts_.push_back(i + 1);
                }
            }
        }
    }

    std::string Agent::sheet_header_url() const {
        if (sheet_name_.starts_with("/")) {
            return "file://" + sheet_name_;
        }
        return "weft://page/" + (sheet_name_.empty() ? std::string("page.css") : sheet_name_);
    }

    Json Agent::range(std::size_t start, std::size_t end) const {
        const auto place = [&](std::size_t offset) {
            const auto next = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
            const auto line = std::size_t(next - line_starts_.begin()) - 1;
            const auto from = line_starts_[line];
            return std::pair<std::size_t, std::size_t>{
                line, utf16_length(std::string_view(sheet_text_).substr(from, offset - from))};
        };
        const auto [start_line, start_column] = place(start);
        const auto [end_line, end_column] = place(end);
        return Json::Object{
            {"startLine", start_line}, {"startColumn", start_column}, {"endLine", end_line}, {"endColumn", end_column}};
    }

    void Agent::add_sheet(const Session& session) {
        const auto end = line_starts_.size() - 1;
        send(session, "CSS.styleSheetAdded",
             {{"header", Json::Object{
                             {"styleSheetId", sheet_id},
                             {"frameId", frame_id},
                             {"sourceURL", sheet_header_url()},
                             {"origin", "regular"},
                             {"title", ""},
                             {"disabled", false},
                             {"isInline", false},
                             {"isMutable", false},
                             {"isConstructed", false},
                             {"startLine", 0},
                             {"startColumn", 0},
                             {"length", utf16_length(sheet_text_)},
                             {"endLine", end},
                             {"endColumn", utf16_length(std::string_view(sheet_text_).substr(line_starts_[end]))},
                         }}});
    }

    std::size_t Agent::offset_at(double line, double column) const {
        const auto row = std::min(std::size_t(std::max(line, 0.0)), line_starts_.size() - 1);
        const auto units = std::size_t(std::max(column, 0.0));
        std::size_t offset = line_starts_[row];
        std::size_t counted{};
        while (offset < sheet_text_.size() && counted < units && sheet_text_[offset] != '\n') {
            const auto byte = static_cast<unsigned char>(sheet_text_[offset]);
            counted += byte >= 0xF0 ? 2 : 1;
            offset += byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
        }
        return std::min(offset, sheet_text_.size());
    }

    std::pair<std::size_t, std::size_t> Agent::edited_range(const Json& edit) const {
        const auto* place = edit.find("range");
        if (!place || !place->object()) {
            throw ProtocolError{invalid_params, "a range"};
        }
        const auto start = offset_at(place->number_at("startLine"), place->number_at("startColumn"));
        const auto end = offset_at(place->number_at("endLine"), place->number_at("endColumn"));
        if (start > end) {
            throw ProtocolError{invalid_params, "a range that ends before it starts"};
        }
        return {start, end};
    }

    // A declaration block's cssProperties and shorthandEntries: what is written in it as the engine takes each (places
    // in `text`), the declarations it holds in comments as disabled ones in their places (the tools turn a declaration
    // off by making it a comment, and on again from that), and after each shorthand the longhands it stands for. `place`
    // gives the range of a part of text; null: a block with no place, and no ranges.
    Json Agent::style_json(const std::vector<WrittenDeclaration>& written, const Json* id, std::string_view text,
                           const Place* place) {
        const auto off = place ? commented(text) : std::vector<Commented>{};
        Json::Array properties;
        Json::Array shorthands;
        std::size_t next_off{};
        const auto disabled_before = [&](std::size_t offset) {
            for (; next_off < off.size() && off[next_off].start < offset; ++next_off) {
                const auto& c = off[next_off];
                properties.push_back(Json::Object{{"name", c.name},
                                                  {"value", c.value},
                                                  {"important", false},
                                                  {"implicit", false},
                                                  {"text", std::string(text.substr(c.start, c.end - c.start))},
                                                  {"parsedOk", true},
                                                  {"disabled", true},
                                                  {"range", (*place)(c.start, c.end)}});
            }
        };
        for (const auto& w : written) {
            const bool placed = place && w.start <= w.end && w.end <= text.size();
            if (placed) {
                disabled_before(w.start);
            }
            Json::Object property{{"name", w.name},
                                  {"value", written_value(w)},
                                  {"important", w.important},
                                  {"implicit", false},
                                  {"text", placed ? std::string(text.substr(w.start, w.end - w.start)) : w.text + ";"},
                                  {"parsedOk", w.supported},
                                  {"disabled", false}};
            if (placed) {
                property.emplace_back("range", (*place)(w.start, w.end));
            }
            properties.emplace_back(std::move(property));
            if (w.supported && (w.longhands.size() != 1 || w.longhands[0].first != w.name)) {
                shorthands.push_back(Json::Object{{"name", w.name}, {"value", w.value}, {"important", w.important}});
                for (const auto& [name, value] : w.longhands) {
                    properties.push_back(Json::Object{{"name", name}, {"value", value}, {"important", w.important}});
                }
            }
        }
        if (place) {
            disabled_before(text.size() + 1);
        }
        Json::Object style{{"cssProperties", std::move(properties)}, {"shorthandEntries", std::move(shorthands)}};
        if (id) {
            style.emplace_back("styleSheetId", *id);
        }
        if (place) {
            style.emplace_back("cssText", std::string(text));
            style.emplace_back("range", (*place)(0, text.size()));
        }
        return style;
    }

    // The style of a block of the sheet, [start, end) of its text: read from the text, so that a block just edited
    // and the same block asked for later are one thing.
    Json Agent::sheet_style(std::size_t start, std::size_t end) {
        const auto text = std::string_view(sheet_text_).substr(start, end - start);
        const auto* style = view_->inspect_style();
        const Json id{std::string(sheet_id)};
        const Place place = [&](std::size_t from, std::size_t to) {
            return range(start + from, start + to);
        };
        return style_json(style ? style->written_declarations(text) : std::vector<WrittenDeclaration>{}, &id, text,
                          &place);
    }

    // A rule as the protocol has it. texts, when given, takes the trimmed text of each selector of its list.
    Json Agent::css_rule(const RuleSource& source, bool user_agent, Json::Array* texts) {
        const bool regular = !user_agent;
        const bool placed = regular && source.located && source.body_end <= sheet_text_.size() &&
                            source.selector_end <= sheet_text_.size();
        const auto items =
            css::split(source.selector, ',', {.quotes = true, .brackets = true, .trim_parts = true}).parts;
        // Each selector's place, where the list is written as it reads (a nested rule's is not: its selectors are
        // not edited in place).
        std::vector<std::pair<std::size_t, std::size_t>> places;
        if (placed) {
            const auto written = std::string_view(sheet_text_)
                                     .substr(source.selector_start, source.selector_end - source.selector_start);
            std::size_t from{};
            int depth{};
            char quote{};
            for (std::size_t p = 0; p <= written.size(); ++p) {
                const char c = p < written.size() ? written[p] : ',';
                if (p < written.size()) {
                    if (quote) {
                        quote = c == quote ? char{} : quote;
                        continue;
                    }
                    if (c == '"' || c == '\'') {
                        quote = c;
                        continue;
                    }
                    depth += c == '(' || c == '[' ? 1 : c == ')' || c == ']' ? -1 : 0;
                    if (c != ',' || depth) {
                        continue;
                    }
                }
                auto first = from;
                auto last = p;
                while (first < last && std::isspace(static_cast<unsigned char>(written[first]))) {
                    ++first;
                }
                while (last > first && std::isspace(static_cast<unsigned char>(written[last - 1]))) {
                    --last;
                }
                places.emplace_back(source.selector_start + first, source.selector_start + last);
                from = p + 1;
            }
            bool same = places.size() == items.size();
            for (std::size_t i = 0; same && i < items.size(); ++i) {
                same = std::string_view(sheet_text_).substr(places[i].first, places[i].second - places[i].first) ==
                       css::trim(items[i]);
            }
            if (!same) {
                places.clear();
            }
        }
        Json::Array selectors;
        for (std::size_t i = 0; i < items.size(); ++i) {
            Json::Object selector{{"text", css::trim(items[i])}};
            if (!places.empty()) {
                selector.emplace_back("range", range(places[i].first, places[i].second));
            }
            selectors.emplace_back(std::move(selector));
            if (texts) {
                texts->emplace_back(css::trim(items[i]));
            }
        }
        const Json id{std::string(sheet_id)};
        Json::Object rule{
            {"selectorList", Json::Object{{"selectors", std::move(selectors)}, {"text", source.selector}}},
            {"origin", regular ? "regular" : "user-agent"},
            {"style", placed ? sheet_style(source.body_start, source.body_end)
                             : style_json(source.declarations, regular ? &id : nullptr, {}, nullptr)}};
        if (regular) {
            rule.emplace_back("styleSheetId", id);
        }
        return rule;
    }

    Json Agent::rule_json(const std::vector<const MatchedRule*>& matches) {
        const auto& first = *matches.front();
        Json::Array texts;
        auto rule = css_rule(*first.source, first.user_agent, &texts);
        Json::Array matching;
        for (std::size_t i = 0; i < texts.size(); ++i) {
            if (std::any_of(matches.begin(), matches.end(), [&](const MatchedRule* m) {
                    return m->selector == *texts[i].string();
                })) {
                matching.emplace_back(i);
            }
        }
        return Json::Object{{"rule", std::move(rule)}, {"matchingSelectors", std::move(matching)}};
    }

    Json::Array Agent::matched_rules_json(const NodeIdentity& identity) {
        Json::Array out;
        const auto* style = view_->inspect_style();
        const auto* scene_dom = view_->inspect_scene_dom();
        if (!style || !scene_dom) {
            return out;
        }
        const auto matches = style->matched_rules(*scene_dom, identity);
        // Selectors of one list that match are one rule, at the place of the last of them, which ranks highest.
        std::vector<std::vector<const MatchedRule*>> rules;
        for (const auto& match : matches) {
            if (!match.source) {
                continue;
            }
            const auto same = std::find_if(rules.begin(), rules.end(), [&](const std::vector<const MatchedRule*>& r) {
                return r.front()->source == match.source;
            });
            std::vector<const MatchedRule*> group;
            if (same != rules.end()) {
                group = std::move(*same);
                rules.erase(same);
            }
            group.push_back(&match);
            rules.push_back(std::move(group));
        }
        for (const auto& group : rules) {
            out.push_back(rule_json(group));
        }
        return out;
    }

    Json Agent::inline_style(const DomSnapshot& node) {
        const auto attribute = node.attributes.find("style");
        const std::string text = attribute == node.attributes.end() ? std::string() : attribute->second;
        const auto* style = view_->inspect_style();
        const Json id{std::string(inline_prefix) + std::to_string(id_of(node.identity))};
        const Place place = [&](std::size_t from, std::size_t to) {
            return Json::Object{{"startLine", 0},
                                {"startColumn", utf16_length(std::string_view(text).substr(0, from))},
                                {"endLine", 0},
                                {"endColumn", utf16_length(std::string_view(text).substr(0, to))}};
        };
        return style_json(style ? style->written_declarations(text) : std::vector<WrittenDeclaration>{}, &id, text,
                          &place);
    }

    Json Agent::css_enable(Session& session, const Json&) {
        session.css = true;
        add_sheet(session);
        return Json::Object{};
    }

    Json Agent::css_get_matched_styles(Session&, const Json& params) {
        const auto& at = located(params);
        if (at.node->type != constants::element_node_type) {
            return Json::Object{};
        }
        Json::Object out{{"inlineStyle", inline_style(*at.node)},
                         {"matchedCSSRules", matched_rules_json(at.node->identity)},
                         {"pseudoElements", Json::Array{}}};
        Json::Array inherited;
        for (const auto* parent = at.parent; parent;) {
            if (parent->type == constants::element_node_type) {
                inherited.push_back(Json::Object{{"inlineStyle", inline_style(*parent)},
                                                 {"matchedCSSRules", matched_rules_json(parent->identity)}});
            }
            const auto it = index_.find(id_of(parent->identity));
            parent = it == index_.end() ? nullptr : it->second.parent;
        }
        out.emplace_back("inherited", std::move(inherited));
        return out;
    }

    Json Agent::css_get_inline_styles(Session&, const Json& params) {
        return Json::Object{{"inlineStyle", inline_style(*located(params).node)}};
    }

    Json Agent::css_get_computed_style(Session&, const Json& params) {
        const auto& at = located(params);
        Json::Array out;
        for (const auto& [name, value] : view_->inspect_computed(at.node->identity)) {
            out.push_back(Json::Object{{"name", name}, {"value", value}});
        }
        return Json::Object{{"computedStyle", std::move(out)}};
    }

    Json Agent::css_get_style_sheet_text(Session&, const Json& params) {
        if (params.string_at("styleSheetId") != sheet_id) {
            throw ProtocolError{server_error, "No style sheet with given id found"};
        }
        return Json::Object{{"text", sheet_text_}};
    }

    // A sheet in the place of the page's, the scripts' state as it was; what went wrong, with the sheet as it was.
    std::optional<std::string> Agent::restyle(const std::string& text, const std::string& keyframes, bool quiet) {
        if (auto error = view_->inspect_restyle(text, keyframes, quiet)) {
            return error;
        }
        read_sheet();
        for (auto& session : sessions_) {
            if (session.css) {
                send(session, "CSS.styleSheetChanged", {{"styleSheetId", sheet_id}});
            }
            if (!quiet) {
                send_sheet_problems(session);
            }
        }
        return std::nullopt;
    }

    void Agent::replace_sheet_text(std::size_t start, std::size_t end, std::string_view text) {
        std::string next = sheet_text_;
        next.replace(start, end - start, text);
        // An edit under way in the tools: what it drops is not worth a diagnostic at each key.
        if (const auto error = restyle(next, view_->inspect_keyframes(), true)) {
            throw ProtocolError{server_error, *error};
        }
    }

    // The first rule written at or after a place of the sheet.
    std::shared_ptr<const RuleSource> Agent::rule_from(std::size_t offset) const {
        std::shared_ptr<const RuleSource> found;
        if (const auto* style = view_->inspect_style()) {
            for (const auto& source : style->rule_sources()) {
                if (source->located && source->selector_start >= offset &&
                    (!found || source->selector_start < found->selector_start)) {
                    found = source;
                }
            }
        }
        return found;
    }

    Json Agent::css_set_style_texts(Session&, const Json& params) {
        Json::Array styles;
        const auto* edits = params.find("edits");
        if (edits && edits->array()) {
            for (const auto& edit : *edits->array()) {
                const auto id = edit.string_at("styleSheetId");
                const auto text = edit.string_at("text");
                if (id == sheet_id) {
                    const auto [start, end] = edited_range(edit);
                    replace_sheet_text(start, end, text);
                    styles.push_back(sheet_style(start, start + text.size()));
                    continue;
                }
                if (!id.starts_with(inline_prefix)) {
                    throw ProtocolError{server_error, "No style sheet with given id found"};
                }
                ensure_document();
                const int node = std::atoi(id.c_str() + inline_prefix.size());
                const auto it = index_.find(node);
                if (it == index_.end()) {
                    throw ProtocolError{invalid_params, "Could not find node with given id"};
                }
                run(node_expression(node) + ".setAttribute('style'," + Json(text).dump() + ")");
                ensure_document();
                styles.push_back(inline_style(*index_.at(node).node));
            }
        }
        return Json::Object{{"styles", std::move(styles)}};
    }

    Json Agent::css_set_style_sheet_text(Session&, const Json& params) {
        if (params.string_at("styleSheetId") != sheet_id) {
            throw ProtocolError{server_error, "No style sheet with given id found"};
        }
        if (const auto error = restyle(params.string_at("text"), view_->inspect_keyframes(), false)) {
            throw ProtocolError{server_error, *error};
        }
        return Json::Object{};
    }

    Json Agent::css_set_rule_selector(Session&, const Json& params) {
        if (params.string_at("styleSheetId") != sheet_id) {
            throw ProtocolError{server_error, "No style sheet with given id found"};
        }
        const auto [start, end] = edited_range(params);
        const auto before = sheet_text_;
        replace_sheet_text(start, end, params.string_at("selector"));
        const auto rule = rule_from(start);
        if (!rule || rule->selector_start != start) {
            // A selector the sheet does not take leaves no rule: the sheet is as it was.
            restyle(before, view_->inspect_keyframes(), true);
            throw ProtocolError{server_error, "SyntaxError: Selector or media text is not valid."};
        }
        const auto described = css_rule(*rule, false);
        return Json::Object{{"selectorList", *described.find("selectorList")}};
    }

    Json Agent::css_add_rule(Session&, const Json& params) {
        if (params.string_at("styleSheetId") != sheet_id) {
            throw ProtocolError{server_error, "No style sheet with given id found"};
        }
        const auto* location = params.find("location");
        if (!location || !location->object()) {
            throw ProtocolError{invalid_params, "a location"};
        }
        const auto start = offset_at(location->number_at("startLine"), location->number_at("startColumn"));
        const auto before = sheet_text_;
        replace_sheet_text(start, start, params.string_at("ruleText"));
        const auto rule = rule_from(start);
        if (!rule || rule->selector_start >= start + params.string_at("ruleText").size()) {
            restyle(before, view_->inspect_keyframes(), true);
            throw ProtocolError{server_error, "SyntaxError: Rule text is not valid."};
        }
        return Json::Object{{"rule", css_rule(*rule, false)}};
    }

    // A page has one sheet: the rules the tools add go to its end.
    Json Agent::css_create_style_sheet(Session&, const Json&) {
        return Json::Object{{"styleSheetId", sheet_id}};
    }

    Json Agent::css_get_platform_fonts(Session&, const Json&) {
        return Json::Object{{"fonts", Json::Array{}}};
    }

    Json Agent::css_get_animated_styles(Session&, const Json&) {
        return Json::Object{{"animationStyles", Json::Array{}}, {"inherited", Json::Array{}}};
    }

    Json Agent::css_get_environment_variables(Session&, const Json&) {
        return Json::Object{{"environmentVariables", Json::Object{}}};
    }

    Json Agent::css_take_computed_style_updates(Session&, const Json&) {
        return Json::Object{{"nodeIds", Json::Array{}}};
    }

    // ---- The log ----

    void Agent::send_sheet_problems(Session& session) {
        const auto* style = view_->inspect_style();
        if (!style || !session.log) {
            return;
        }
        const auto entry = [&](std::string text, std::size_t offset) {
            const auto next = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
            send(session, "Log.entryAdded",
                 {{"entry", Json::Object{{"source", "rendering"},
                                         {"level", "warning"},
                                         {"text", std::move(text)},
                                         {"timestamp", now_ms()},
                                         {"url", sheet_header_url()},
                                         {"lineNumber", std::size_t(next - line_starts_.begin()) - 1}}}});
        };
        for (const auto& source : style->rule_sources()) {
            for (const auto& w : source->declarations) {
                if (!w.supported) {
                    entry("Weft does not take \"" + w.text + "\" in " + source->selector + ": " + w.reason, w.start);
                }
            }
        }
        for (const auto& problem : style->sheet_problems()) {
            entry(problem.message, problem.offset);
        }
    }

    void Agent::forward_log(Session& session) {
        if (!session.runtime && !session.log) {
            return;
        }
        for (const auto& entry : view_->inspect_log(std::min(session.runtime ? session.runtime_sent : UINT64_MAX,
                                                             session.log ? session.log_sent : UINT64_MAX))) {
            const auto& [kind, message] = entry.diagnostic;
            if (kind == ViewDiagnostic::Kind::Scene) {
                if (session.log && entry.sequence > session.log_sent) {
                    // What the sheet's parser dropped has been sent with its place in the sheet.
                    if (!message.starts_with(dropped_prefix) || !entry.sheet) {
                        send(session, "Log.entryAdded",
                             {{"entry", Json::Object{{"source", "rendering"},
                                                     {"level", "warning"},
                                                     {"text", message},
                                                     {"timestamp", now_ms()}}}});
                    }
                }
            } else if (session.runtime && entry.sequence > session.runtime_sent) {
                const bool console = kind == ViewDiagnostic::Kind::Console ||
                                     kind == ViewDiagnostic::Kind::ConsoleWarning ||
                                     kind == ViewDiagnostic::Kind::ConsoleError;
                if (console) {
                    send(session, "Runtime.consoleAPICalled",
                         {{"type", kind == ViewDiagnostic::Kind::Console          ? "log"
                                   : kind == ViewDiagnostic::Kind::ConsoleWarning ? "warning"
                                                                                  : "error"},
                          {"args", Json::Array{Json::Object{{"type", "string"}, {"value", message}}}},
                          {"executionContextId", context_id},
                          {"timestamp", now_ms()}});
                } else {
                    send(session, "Runtime.exceptionThrown",
                         {{"timestamp", now_ms()},
                          {"exceptionDetails", Json::Object{{"exceptionId", entry.sequence},
                                                            {"text", message},
                                                            {"lineNumber", 0},
                                                            {"columnNumber", 0},
                                                            {"executionContextId", context_id}}}});
                }
            }
            if (session.runtime) {
                session.runtime_sent = std::max(session.runtime_sent, entry.sequence);
            }
            if (session.log) {
                session.log_sent = std::max(session.log_sent, entry.sequence);
            }
        }
    }

    Json Agent::report() {
        Json::Array declarations;
        Json::Array problems;
        Json::Array runtime;
        const auto line_of = [&](std::size_t offset) {
            const auto next = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
            return std::size_t(next - line_starts_.begin());
        };
        const auto* style = view_->inspect_style();
        const auto* scene_dom = view_->inspect_scene_dom();
        if (style) {
            std::map<const RuleSource*, std::size_t> counts;
            if (scene_dom) {
                for (const auto& [element, source] : style->unsupported_matches(*scene_dom)) {
                    ++counts[source.get()];
                }
            }
            for (const auto& source : style->rule_sources()) {
                for (const auto& w : source->declarations) {
                    if (!w.supported) {
                        const auto count = counts.find(source.get());
                        declarations.push_back(Json::Object{{"selector", source->selector},
                                                            {"property", w.name},
                                                            {"value", w.value},
                                                            {"reason", w.reason},
                                                            {"line", source->located ? line_of(w.start) : 0},
                                                            {"elements", count == counts.end() ? 0 : count->second}});
                    }
                }
            }
            for (const auto& problem : style->sheet_problems()) {
                problems.push_back(Json::Object{{"message", problem.message}, {"line", line_of(problem.offset)}});
            }
        }
        for (const auto& entry : view_->inspect_log(0)) {
            const auto& [kind, message] = entry.diagnostic;
            if (kind == ViewDiagnostic::Kind::Scene && !entry.sheet) {
                runtime.emplace_back(message);
            }
        }
        return Json::Object{{"sheet", sheet_name_},
                            {"declarations", std::move(declarations)},
                            {"problems", std::move(problems)},
                            {"runtime", std::move(runtime)}};
    }

    // ---- Pages and their files (docs/devtools.md, "热重载") ----

    // Whether the page a tool supplies is the one the View shows. The host may have loaded another page of its own
    // since (a game going from one screen to the next): the tool's page then waits for the host to load the page it
    // stands for, and what the tool sends meanwhile is kept for it and does not touch the page showing.
    bool Agent::supplied_shown() const {
        return supplied_ && supplied_->base == view_->inspect_page().base;
    }

    Json::Object Agent::page_state() const {
        return {{"base", view_->inspect_page().base},
                {"supplied", supplied_.has_value()},
                {"suppliedShown", supplied_shown()}};
    }

    Json Agent::weft_enable(Session& session, const Json&) {
        session.weft = true;
        return page_state();
    }

    Json Agent::page_reload(Session&, const Json&) {
        // What the page showing was last loaded with, or what a tool supplies for it: a copy, as loading replaces the
        // View's.
        const ViewPage page = supplied_shown() ? *supplied_ : view_->inspect_page();
        if (const auto error = load_page(page)) {
            throw ProtocolError{server_error, *error};
        }
        return Json::Object{};
    }

    Json Agent::weft_get_page(Session&, const Json&) {
        const auto& page = view_->inspect_page();
        Json::Array scripts;
        for (const auto& script : page.scripts) {
            scripts.push_back(Json::Object{{"name", script.name}, {"source", script.source}});
        }
        return Json::Object{{"base", page.base},
                            {"root", page.root},
                            {"scripts", std::move(scripts)},
                            {"stylesheet", sheet_text_},
                            {"keyframes", view_->inspect_keyframes()},
                            {"crpgBridge", page.crpg_bridge},
                            {"forgivingCss", page.forgiving_css},
                            {"webPlatform", page.web_platform},
                            {"spatialNavigation", page.spatial_navigation},
                            {"supplied", supplied_.has_value()},
                            {"suppliedShown", supplied_shown()}};
    }

    Json Agent::weft_set_resources(Session&, const Json& params) {
        // The files are those of the page the tool supplies, when it supplies one: the View may show another.
        const bool waiting = supplied_ && !supplied_shown();
        const auto base = supplied_ ? supplied_->base : view_->inspect_page().base;
        std::size_t count{};
        if (const auto* resources = params.find("resources"); resources && resources->array()) {
            for (const auto& resource : *resources->array()) {
                auto bytes = base64_bytes(resource.string_at("data"));
                const auto path = resource.string_at("path");
                if (!bytes || path.empty()) {
                    throw ProtocolError{invalid_params, "a resource is a path and base64 data"};
                }
                // By the path the page's url() of it resolves to.
                view_->inspect_set_resource(view_resolve_url(base, path), std::move(*bytes));
                ++count;
            }
        }
        // The page reads its files again; its sheet is the same.
        if (params.bool_at("apply", true) && count && !waiting) {
            if (const auto error = restyle(sheet_text_, view_->inspect_keyframes(), true)) {
                throw ProtocolError{server_error, *error};
            }
        }
        return Json::Object{{"resources", count}, {"deferred", waiting}};
    }

    Json Agent::weft_set_style_sheet(Session& session, const Json& params) {
        const auto text = params.string_at("text");
        if (supplied_ && !supplied_shown()) {
            // The View shows another page: the sheet is kept for the page it belongs to.
            supplied_->stylesheet = text;
            if (params.find("keyframes")) {
                supplied_->keyframes = params.string_at("keyframes");
            }
            supplied_by_ = session.connection;
            return Json::Object{{"applied", false}, {"deferred", true}};
        }
        const auto keyframes = params.find("keyframes") ? params.string_at("keyframes") : view_->inspect_keyframes();
        if (const auto error = restyle(text, keyframes, false)) {
            return Json::Object{{"applied", false}, {"error", *error}};
        }
        // The sheet is the page's from now on: a load of the page has it too.
        if (!supplied_) {
            supplied_ = view_->inspect_page();
            supplied_by_ = session.connection;
        }
        supplied_->stylesheet = text;
        supplied_->keyframes = keyframes;
        return Json::Object{{"applied", true}};
    }

    Json Agent::weft_set_page(Session& session, const Json& params) {
        const auto* given = params.find("page");
        if (!given || !given->object()) {
            throw ProtocolError{invalid_params, "a page"};
        }
        // The page stands for the one loaded now, whose files its own resolve as; what it leaves out is that page's.
        ViewPage page = supplied_ ? *supplied_ : view_->inspect_page();
        if (const auto* scripts = given->find("scripts")) {
            if (!scripts->array()) {
                throw ProtocolError{invalid_params, "scripts"};
            }
            page.scripts.clear();
            for (const auto& script : *scripts->array()) {
                page.scripts.push_back({script.string_at("name"), script.string_at("source")});
            }
        }
        if (given->find("stylesheet")) {
            page.stylesheet = given->string_at("stylesheet");
        }
        if (given->find("keyframes")) {
            page.keyframes = given->string_at("keyframes");
        }
        if (given->find("root")) {
            page.root = given->string_at("root", "#root");
        }
        page.crpg_bridge = given->bool_at("crpgBridge", page.crpg_bridge);
        page.forgiving_css = given->bool_at("forgivingCss", page.forgiving_css);
        page.web_platform = given->bool_at("webPlatform", page.web_platform);
        page.spatial_navigation = given->bool_at("spatialNavigation", page.spatial_navigation);
        if (const auto* resources = params.find("resources")) {
            weft_set_resources(session, Json(Json::Object{{"resources", *resources}, {"apply", false}}));
        }
        if (supplied_ && !supplied_shown()) {
            // The View shows another page: this one is kept for when the host loads the page it stands for. It has
            // not been tried; if it does not load then, the host's own does (weft_view_load).
            supplied_ = std::move(page);
            supplied_by_ = session.connection;
            return Json::Object{{"loaded", false}, {"deferred", true}};
        }
        // A page that does not load is not the page: what stood for the host's before stands for it still.
        if (const auto error = load_page(page)) {
            return Json::Object{{"loaded", false}, {"error", *error}};
        }
        supplied_ = std::move(page);
        supplied_by_ = session.connection;
        return Json::Object{{"loaded", true}};
    }

    // The page as it is now, for laying the same document out elsewhere (tools/devtools_compare.mjs): its sheet, and
    // the scene's elements with their attributes, text and state and the box each has.
    Json Agent::weft_get_snapshot(Session&, const Json&) {
        const auto* dom = view_->inspect_scene_dom();
        if (!dom) {
            throw ProtocolError{server_error, "the page has no scene"};
        }
        const auto key = [](const NodeIdentity& identity) {
            return Key{identity.slot, identity.generation, identity.view_epoch};
        };
        std::map<Key, const ElementGeometry*> boxes;
        if (const auto* geometries = view_->inspect_geometries()) {
            for (const auto& geometry : *geometries) {
                boxes.emplace(key(geometry.identity), &geometry);
            }
        }
        const auto* style = view_->inspect_style();
        std::map<Key, Json::Array> unsupported;
        if (style) {
            for (const auto& [element, source] : style->unsupported_matches(*dom)) {
                for (const auto& w : source->declarations) {
                    if (!w.supported) {
                        unsupported[key(element)].emplace_back(source->selector + " { " + w.text + " }");
                    }
                }
            }
        }
        const auto node = [&](auto&& self, const DomSnapshot& n) -> Json {
            Json::Object out{{"tag", n.local_name}};
            if (!n.namespace_uri.empty() && n.namespace_uri != constants::xhtml_namespace_uri) {
                out.emplace_back("ns", n.namespace_uri);
            }
            Json::Object attributes;
            for (const auto& [name, value] : n.attributes) {
                attributes.emplace_back(name, value);
            }
            out.emplace_back("attributes", std::move(attributes));
            if (const auto box = boxes.find(key(n.identity)); box != boxes.end()) {
                const auto& r = box->second->rect;
                out.emplace_back("rect", Json::Array{r[0], r[1], r[2], r[3]});
            }
            std::string state;
            for (const auto& [on, name] :
                 {std::pair{n.hovered, "hover"}, std::pair{n.active, "active"}, std::pair{n.focused, "focus"},
                  std::pair{n.focus_visible, "focus-visible"}, std::pair{n.focus_within, "focus-within"}}) {
                if (on) {
                    state += (state.empty() ? "" : " ") + std::string(name);
                }
            }
            if (!state.empty()) {
                out.emplace_back("state", state);
            }
            if (n.scroll_left != 0 || n.scroll_top != 0) {
                out.emplace_back("scroll", Json::Array{n.scroll_left, n.scroll_top});
            }
            if (n.field) {
                out.emplace_back("value", n.field->value);
            }
            if (n.animations && !n.animations->empty()) {
                out.emplace_back("animated", true);
            }
            auto dropped = unsupported.find(key(n.identity));
            Json::Array drops = dropped == unsupported.end() ? Json::Array{} : std::move(dropped->second);
            if (const auto inline_text = n.attributes.find("style"); style && inline_text != n.attributes.end()) {
                for (const auto& w : style->written_declarations(inline_text->second)) {
                    if (!w.supported) {
                        drops.emplace_back("style attribute { " + w.text + " }");
                    }
                }
            }
            if (!drops.empty()) {
                out.emplace_back("unsupported", std::move(drops));
            }
            Json::Array children;
            for (const auto& child : n.children) {
                if (child.type == constants::element_node_type) {
                    children.push_back(self(self, child));
                } else if (child.type == constants::text_node_type) {
                    children.push_back(Json::Object{{"text", child.data}});
                }
            }
            out.emplace_back("children", std::move(children));
            return out;
        };
        const auto viewport = view_->inspect_viewport();
        const auto area = view_->inspect_safe_area();
        const auto& page = view_->inspect_page();
        return Json::Object{{"width", viewport[0]},
                            {"height", viewport[1]},
                            {"scale", viewport[2]},
                            {"safeArea", Json::Array{area[0], area[1], area[2], area[3]}},
                            {"animated", view_->inspect_animated()},
                            {"base", page.base},
                            {"root", page.root},
                            {"stylesheet", sheet_text_},
                            {"keyframes", view_->inspect_keyframes()},
                            {"tree", node(node, *dom)}};
    }

    // ---- The overlay ----

    std::optional<Agent::Boxes> Agent::boxes_of(const NodeIdentity& identity) {
        const auto geometry = view_->inspect_geometry(identity);
        if (!geometry) {
            return std::nullopt;
        }
        const auto style = view_->inspect_computed(identity);
        const auto& r = geometry->rect;
        Boxes out;
        out.border = {r[0], r[1], r[0] + r[2], r[1] + r[3]};
        const auto edges = [&](const std::string& prefix, const std::string& suffix) {
            return std::array<double, 4>{px(style, prefix + "-left" + suffix), px(style, prefix + "-top" + suffix),
                                         px(style, prefix + "-right" + suffix), px(style, prefix + "-bottom" + suffix)};
        };
        const auto inset = [](const std::array<double, 4>& box, const std::array<double, 4>& by, double sign) {
            auto x0 = box[0] + sign * by[0];
            auto y0 = box[1] + sign * by[1];
            auto x1 = box[2] - sign * by[2];
            auto y1 = box[3] - sign * by[3];
            return std::array<double, 4>{x0, y0, std::max(x0, x1), std::max(y0, y1)};
        };
        out.margin = inset(out.border, edges("margin", ""), -1);
        out.padding = inset(out.border, edges("border", "-width"), 1);
        out.content = inset(out.padding, edges("padding", ""), 1);
        return out;
    }

    void Agent::highlight(int node, const Json* config) {
        Highlight next;
        next.node = node;
        // Chrome's defaults.
        next.colors = {color_text(config ? config->find("contentColor") : nullptr, "rgba(111,168,220,0.66)"),
                       color_text(config ? config->find("paddingColor") : nullptr, "rgba(147,196,125,0.55)"),
                       color_text(config ? config->find("borderColor") : nullptr, "rgba(255,229,153,0.66)"),
                       color_text(config ? config->find("marginColor") : nullptr, "rgba(246,178,107,0.66)")};
        next.info = !config || config->bool_at("showInfo", true);
        highlight_ = std::move(next);
    }

    void Agent::clear_highlight() {
        highlight_.reset();
        overlay_key_.clear();
        view_->inspect_overlay(std::nullopt);
    }

    void Agent::update_overlay() {
        if (!highlight_) {
            return;
        }
        const auto it = index_.find(highlight_->node);
        const auto boxes = it == index_.end() ? std::nullopt : boxes_of(it->second.node->identity);
        const auto frame = view_->shared()->frame();
        if (!boxes || !frame) {
            if (!overlay_key_.empty()) {
                overlay_key_.clear();
                view_->inspect_overlay(std::nullopt);
            }
            return;
        }
        const auto& node = *it->second.node;
        std::string key =
            std::to_string(highlight_->node) + "/" + std::to_string(frame->width) + "/" + std::to_string(frame->height);
        for (const auto* box : {&boxes->margin, &boxes->border, &boxes->padding, &boxes->content}) {
            for (const auto v : *box) {
                key += "," + std::to_string(v);
            }
        }
        for (const auto& color : highlight_->colors) {
            key += color;
        }
        if (key == overlay_key_) {
            return;
        }
        overlay_key_ = key;

        std::uint32_t slot = 1;
        const auto element = [&](std::string style) {
            DomSnapshot out;
            out.identity = {slot++, 1, overlay_epoch};
            out.type = constants::element_node_type;
            out.local_name = "div";
            out.namespace_uri = constants::xhtml_namespace_uri;
            out.attributes["style"] = std::move(style);
            return out;
        };
        DomSnapshot root = element("position:absolute;left:0;top:0;width:" + px_text(frame->width) +
                                   ";height:" + px_text(frame->height) + ";z-index:" + std::to_string(top_z_index) +
                                   ";pointer-events:none;overflow:hidden");
        const auto add = [&](double x0, double y0, double x1, double y1, const std::string& color) {
            if (x1 > x0 && y1 > y0) {
                root.children.push_back(element("position:absolute;left:" + px_text(x0) + ";top:" + px_text(y0) +
                                                ";width:" + px_text(x1 - x0) + ";height:" + px_text(y1 - y0) +
                                                ";background-color:" + color));
            }
        };
        // A ring between two boxes, in four strips; the middle is filled by the next one.
        const auto ring = [&](const std::array<double, 4>& outer, const std::array<double, 4>& inner,
                              const std::string& color) {
            add(outer[0], outer[1], outer[2], inner[1], color);
            add(outer[0], inner[3], outer[2], outer[3], color);
            add(outer[0], inner[1], inner[0], inner[3], color);
            add(inner[2], inner[1], outer[2], inner[3], color);
        };
        add(boxes->content[0], boxes->content[1], boxes->content[2], boxes->content[3], highlight_->colors[0]);
        ring(boxes->padding, boxes->content, highlight_->colors[1]);
        ring(boxes->border, boxes->padding, highlight_->colors[2]);
        ring(boxes->margin, boxes->border, highlight_->colors[3]);
        if (highlight_->info) {
            std::string label = node.local_name;
            if (const auto id = node.attributes.find("id"); id != node.attributes.end() && !id->second.empty()) {
                label += "#" + id->second;
            }
            if (const auto classes = node.attributes.find("class"); classes != node.attributes.end()) {
                for (const auto& name : css::split(classes->second, ' ', {.trim_parts = true}).parts) {
                    if (!name.empty()) {
                        label += "." + name;
                    }
                }
            }
            char size[64];
            std::snprintf(size, sizeof size, "  %.0f \xC3\x97 %.0f", boxes->border[2] - boxes->border[0],
                          boxes->border[3] - boxes->border[1]);
            label += size;
            const double above = boxes->margin[1] - overlay_label_height - overlay_label_margin;
            const double top = above >= 0 ? above
                                          : std::min<double>(boxes->margin[3] + overlay_label_margin,
                                                             frame->height - overlay_label_height);
            auto text =
                element("position:absolute;left:" + px_text(std::max(boxes->border[0], 0.0)) + ";top:" + px_text(top) +
                        ";height:" + px_text(overlay_label_height) + ";line-height:" + px_text(overlay_label_height) +
                        ";padding:0 8px;border-radius:3px;background-color:#ffffff;color:#222222;font-size:12px;"
                        "font-family:system-ui;white-space:nowrap");
            DomSnapshot words;
            words.identity = {slot++, 1, overlay_epoch};
            words.type = constants::text_node_type;
            words.data = label;
            text.children.push_back(std::move(words));
            root.children.push_back(std::move(text));
        }
        view_->inspect_overlay(root);
    }

    void Agent::update_picking() {
        if (!picking_) {
            return;
        }
        const auto pointer = view_->inspect_take_pointer();
        const auto hovered = originating(pointer.hovered);
        if (hovered.generation && !(hovered == last_hovered_)) {
            last_hovered_ = hovered;
            ensure_document();
            const int id = id_of(hovered);
            if (index_.contains(id)) {
                highlight_ = picking_style_;
                if (highlight_) {
                    highlight_->node = id;
                }
                for (auto& session : sessions_) {
                    if (session.dom) {
                        push_path(id, session);
                        send(session, "Overlay.nodeHighlightRequested", {{"nodeId", id}});
                    }
                }
            }
        }
        if (pointer.picked.generation) {
            ensure_document();
            const int id = id_of(originating(pointer.picked));
            if (index_.contains(id)) {
                for (auto& session : sessions_) {
                    if (session.dom) {
                        push_path(id, session);
                    }
                    send(session, "Overlay.inspectNodeRequested", {{"backendNodeId", id}});
                }
            }
            picking_ = false;
            view_->inspect_pointer_mode(false);
        }
    }

    Json Agent::overlay_highlight_node(Session&, const Json& params) {
        const auto& at = located(params);
        highlight(id_of(at.node->identity), params.find("highlightConfig"));
        return Json::Object{};
    }

    Json Agent::overlay_hide_highlight(Session&, const Json&) {
        clear_highlight();
        return Json::Object{};
    }

    Json Agent::overlay_set_inspect_mode(Session&, const Json& params) {
        picking_ = params.string_at("mode") != "none";
        view_->inspect_pointer_mode(picking_);
        last_hovered_ = {};
        if (picking_) {
            highlight(0, params.find("highlightConfig"));
            picking_style_ = highlight_;
            highlight_.reset();
        } else {
            picking_style_.reset();
            clear_highlight();
        }
        return Json::Object{};
    }

    // ---- The rest ----

    Json Agent::runtime_enable(Session& session, const Json&) {
        session.runtime = true;
        send(session, "Runtime.executionContextCreated",
             {{"context",
               Json::Object{
                   {"id", context_id},
                   {"origin", ""},
                   {"name", ""},
                   {"uniqueId", std::to_string(loads_)},
                   {"auxData", Json::Object{{"isDefault", true}, {"type", "default"}, {"frameId", frame_id}}}}}});
        return Json::Object{};
    }

    Json Agent::runtime_evaluate(Session& session, const Json& params) {
        // The console's preview as one types: nothing here can promise a script has no side effects.
        if (params.bool_at("throwOnSideEffect")) {
            throw ProtocolError{server_error, "Weft does not evaluate without side effects"};
        }
        std::string source;
        if (session.inspected && index_.contains(session.inspected)) {
            source = "globalThis.$0=" + node_expression(session.inspected) + ";";
        }
        if (!source.empty()) {
            view_->evaluate(source);
        }
        const auto [ok, text] =
            view_->evaluate(std::string(evaluate_wrapper) + Json(params.string_at("expression")).dump() + ")");
        if (!ok) {
            throw ProtocolError{server_error, text};
        }
        auto outcome = Json::parse(text);
        if (!outcome) {
            throw ProtocolError{server_error, "the evaluation gave no result"};
        }
        if (outcome->bool_at("ok")) {
            const auto* result = outcome->find("result");
            return Json::Object{{"result", result ? *result : Json(Json::Object{})}};
        }
        const auto description = outcome->string_at("text");
        return Json::Object{
            {"result", Json::Object{{"type", "object"}, {"subtype", "error"}, {"description", description}}},
            {"exceptionDetails", Json::Object{{"exceptionId", 1},
                                              {"text", "Uncaught"},
                                              {"lineNumber", 0},
                                              {"columnNumber", 0},
                                              {"exception", Json::Object{{"type", "object"},
                                                                         {"subtype", "error"},
                                                                         {"className", "Error"},
                                                                         {"description", description}}}}}};
    }

    // What the page said so far is not sent to this connection, whose console was cleared or which only wants what
    // happens from now on (a View keeps what it said for the tools that connect late).
    Json Agent::runtime_discard_console_entries(Session& session, const Json&) {
        session.runtime_sent = view_->inspect_log_end();
        session.log_sent = session.runtime_sent;
        return Json::Object{};
    }

    Json Agent::runtime_get_isolate_id(Session&, const Json&) {
        return Json::Object{{"id", "weft"}};
    }

    Json Agent::network_emulate_by_rule(Session&, const Json&) {
        return Json::Object{{"ruleIds", Json::Array{}}};
    }

    Json Agent::debugger_enable(Session&, const Json&) {
        return Json::Object{{"debuggerId", "weft"}};
    }

    Json Agent::log_enable(Session& session, const Json&) {
        session.log = true;
        send_sheet_problems(session);
        return Json::Object{};
    }

    Json Agent::page_get_resource_tree(Session&, const Json&) {
        const auto url = sheet_header_url();
        return Json::Object{
            {"frameTree", Json::Object{{"frame", Json::Object{{"id", frame_id},
                                                              {"loaderId", loader_id},
                                                              {"url", url},
                                                              {"domainAndRegistry", ""},
                                                              {"securityOrigin", "weft://page"},
                                                              {"mimeType", "text/html"},
                                                              {"secureContextType", "Secure"},
                                                              {"crossOriginIsolatedContextType", "NotIsolated"},
                                                              {"gatedAPIFeatures", Json::Array{}}}},
                                       {"resources", Json::Array{Json::Object{{"url", url},
                                                                              {"type", "Stylesheet"},
                                                                              {"mimeType", "text/css"},
                                                                              {"contentSize", sheet_text_.size()}}}}}}};
    }

    Json Agent::page_get_resource_content(Session&, const Json&) {
        return Json::Object{{"content", sheet_text_}, {"base64Encoded", false}};
    }

    Json Agent::weft_get_compatibility_report(Session&, const Json&) {
        return report();
    }
} // namespace weft::devtools
