#include "script/script_dom.hpp"
#include "bridge/bridge_value.hpp"
#include "bridge/engine_channel.hpp"
#include "constants/bridge.hpp"
#include "constants/common.hpp"
#include "base/diagnostic.hpp"
#include "base/owner_thread.hpp"
#include "constants/dom.hpp"
#include "constants/input.hpp"
#include "constants/script.hpp"
#include "dom/attributes.hpp"
#include "dom/focus_rules.hpp"
#include "dom/node_table.hpp"
#include "dom/selector.hpp"
#include "dom/tree.hpp"
#include "script/web_storage.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace weft {
    namespace {
        struct PendingJsException {};

        struct Value {
            JSContext* context;
            JSValue value;

            Value(JSContext* c, JSValue v) : context(c), value(v) {
                if (JS_IsException(v)) {
                    throw PendingJsException{};
                }
            }

            ~Value() {
                JS_FreeValue(context, value);
            }

            Value(const Value&) = delete;
            Value& operator=(const Value&) = delete;
        };

        std::string string_value(JSContext* context, JSValueConst value) {
            std::size_t length{};
            const char* chars = JS_ToCStringLen(context, &length, value);
            if (!chars) {
                throw PendingJsException{};
            }

            struct CStringGuard {
                JSContext* context;
                const char* chars;

                ~CStringGuard() {
                    JS_FreeCString(context, chars);
                }
            } guard{context, chars};

            std::string result(chars, length);
            return result;
        }

        bool equal_value(JSValueConst left, JSValueConst right) {
            return JS_VALUE_GET_TAG(left) == JS_VALUE_GET_TAG(right) &&
                   JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right);
        }

        std::string lower_ascii(std::string input) {
            for (char& c : input) {
                if (c >= 'A' && c <= 'Z') {
                    c += 'a' - 'A';
                }
            }
            return input;
        }
    } // namespace

    struct ScriptDom::Impl {
        using Attribute = dom::Attribute;
        using Node = dom::Node;

        struct Listener {
            std::uint64_t id{};
            std::string type;
            JSValue callback{JS_UNDEFINED};
            bool capture{};
            bool once{};
            bool handler{};
            bool passive{};
        };

        // A node's script state. Each owning value is duplicated exactly once and is reported by the node's mark().
        struct Binding {
            // The node's wrapper, borrowed: not an extra GC root. It stays until the slot is reused, so a finalizer
            // can release references to neighbors collected earlier in the same collection.
            JSValue wrapper{JS_UNDEFINED};
            // The wrapper's own properties (D3's __data__ and __on, symbols, closures).
            JSValue expando{JS_UNDEFINED};
            std::vector<Listener> listeners;
            // A scroll event job is queued for the node.
            bool scroll_pending{};
            // A text field's script state beside its snapshot: its value is dirty (the user or script set it, so its
            // default value no longer does), the value before the user's first edit since the last change event
            // (none: no change to commit), and a selectionchange event job is queued.
            bool field_dirty{};
            std::optional<std::string> value_before_edit;
            bool selectionchange_pending{};
        };

        struct Completion {
            std::uint64_t epoch;
            std::string source;
        };

        OwnerThread owner_thread;
        JSRuntime* runtime{};
        JSContext* context{};
        JSClassID node_class{};
        JSClassID canvas_class{};
        JSValue canvas_prototype{JS_UNDEFINED};
        CanvasFactory canvas_factory;
        std::shared_ptr<CanvasBudget> canvas_budget = std::make_shared<CanvasBudget>();

        // A canvas element's 2D state, kept beside the DOM. The entry lives as long as its element: the element's
        // mark() reports the context wrapper and its finalizer releases the entry.
        struct CanvasEntry {
            // The CanvasRenderingContext2D wrapper, an owning edge of the element.
            JSValue context{JS_UNDEFINED};
            std::unique_ptr<CanvasState> canvas;
        };

        dom::NodeTable<CanvasEntry> canvases;
        JSValue document_root{JS_UNDEFINED};
        JSValue mouse_prototype{JS_UNDEFINED};
        JSValue wheel_prototype{JS_UNDEFINED};
        JSValue event_prototype{JS_UNDEFINED};
        JSValue keyboard_prototype{JS_UNDEFINED};
        JSValue focus_prototype{JS_UNDEFINED};
        JSValue input_prototype{JS_UNDEFINED};
        JSValue composition_prototype{JS_UNDEFINED};
        // HTML input and textarea elements' prototype: the node prototype with the text field API.
        JSValue field_prototype{JS_UNDEFINED};
        LayoutHost layout_host;
        ComputedStyleHost computed_style_host;
        ParticleHost particle_host;
        LayoutEpochHost layout_epoch_host;
        std::uint64_t read_revision{};
        std::uint64_t read_epoch{};
        std::optional<DomSnapshot> read_snapshot;
        std::vector<ElementGeometry> read_geometry;
        bool read_geometry_valid{};
        std::map<std::array<std::uint64_t, 3>, std::map<std::string, std::string>> read_styles;
        std::size_t read_style_bytes{};
        IntegerFormatHost integer_format_host;
        // The web platform module's hosts and storage areas (local, session).
        ConsoleHost console_host;
        MediaHost media_host;
        std::array<std::shared_ptr<WebStorage>, 2> storage;
        // The element that has the focus, with the elements whose focus flags it set (it and its ancestors).
        NodeIdentity focus;
        std::vector<NodeIdentity> focus_chain;
        // What :focus-visible reads (Blink's SelectorChecker::MatchesFocusVisiblePseudoClass): how the user last
        // moved the focus (script moves count for nothing), and whether a key went down since the mouse moved it.
        enum class LastFocus : std::uint8_t {
            None,
            Mouse,
            Other
        };
        LastFocus last_focus{};
        bool had_keyboard_event{};
        // The host's keyboard focus is in the page: the focus flags hold only then. Kept across documents.
        bool page_focus = true;
        FocusHost focus_host;
        // Spatial navigation's elements take the focus too (ScriptDom::set_spatial_navigation).
        bool spatial_navigation{};
        // Where sequential focus navigation starts without a focused element: the node focused or pressed last, and,
        // once it left the document, the parent it left and the sibling that followed it (none: it was the last).
        NodeIdentity start_node;
        NodeIdentity start_parent;
        NodeIdentity start_next;
        // The node a mouse button went down on last, and the element Space made :active.
        NodeIdentity pressed_node;
        NodeIdentity key_active;
        bool layout_running{};

        // The native DOM. Each tree edge owns one reference on the neighbor's wrapper, which mark() reports, so the
        // collector decides which nodes live; a finalized node's identity never resolves again.
        dom::Tree tree{1};
        // Script state by node slot, never shorter than the tree's slot table. A deque keeps entries in place while it
        // grows, so a Binding reference survives script that creates nodes.
        std::deque<Binding> bindings;

        // The node an edge names; edges name live nodes outside finalizers.
        Node& node_at(NodeIdentity id) const {
            return tree.at(id);
        }

        Node* parent_of(const Node& n) const {
            return tree.parent_of(n);
        }

        Binding& binding(const Node& n) {
            return bindings[n.identity.slot];
        }

        JSValue wrapper(const Node& n) const {
            return bindings[n.identity.slot].wrapper;
        }

        JSValue wrapper_at(NodeIdentity id) const {
            return wrapper(node_at(id));
        }

        // The wrapper behind an edge, live or collected earlier in the current collection.
        JSValue edge_wrapper(NodeIdentity id) const noexcept {
            return bindings[id.slot].wrapper;
        }

        std::vector<NodeIdentity> hovered_nodes;
        std::vector<NodeIdentity> active_nodes;
        std::map<std::uint64_t, JSValue> native_roots;
        std::deque<Completion> completions;
        std::uint64_t view_epoch{1};
        std::uint64_t next_root{1};
        std::uint64_t next_listener{1};
        ScriptStats counters;

        struct HostBinding {
            JSValue object{JS_UNDEFINED};
            JSValue function{JS_UNDEFINED};
        };

        enum HostMethod {
            Pump,
            Clock,
            Begin,
            Next,
            Pending,
            ErrorCount,
            Errors,
            // The engine bridge's dispatch (__ceEngineHost of the Engine platform module).
            EngineEvent,
            EngineSettle,
            EngineModel,
            EngineErrors
        };

        std::array<HostBinding, 11> host_bindings;

        JSValue call_host(HostMethod method, int argc = 0, JSValueConst* args = nullptr) {
            auto& binding = host_bindings[method];
            if (JS_IsUndefined(binding.function)) {
                Value global(context, JS_GetGlobalObject(context));
                Value object(context, JS_GetPropertyStr(context, global.value,
                                                        method <= Clock         ? "__ceHost"
                                                        : method >= EngineEvent ? "__ceEngineHost"
                                                                                : "__ceAnimationHost"));
                if (JS_IsException(object.value)) {
                    throw PendingJsException{};
                }
                if (JS_IsUndefined(object.value)) {
                    throw std::runtime_error("WEFT_HOST_BINDING: optional platform module missing");
                }
                const char* names[]{"pumpNative", "advanceClock", "begin",  "next",  "pending", "errorCount",
                                    "errors",     "event",        "settle", "model", "errors"};
                Value function(context, JS_GetPropertyStr(context, object.value, names[method]));
                if (JS_IsException(function.value)) {
                    throw PendingJsException{};
                }
                if (!JS_IsFunction(context, function.value)) {
                    throw std::runtime_error("WEFT_HOST_BINDING: optional platform module missing");
                }
                binding.object = JS_DupValue(context, object.value);
                binding.function = JS_DupValue(context, function.value);
            }
            ++counters.host_calls;
            return JS_Call(context, binding.function, binding.object, argc, args);
        }

        void changed(Node& n) noexcept {
            tree.changed(n);
            check_journal();
        }

        // An exhausted revision counter faults the View.
        void check_journal() noexcept {
            if (tree.exhausted()) {
                faulted = true;
            }
        }

        bool faulted{};
        // The limit that faulted the View first, named in its fault messages.
        enum class Limit : std::uint8_t {
            None,
            VmMemory,
            ExecutionBudget
        };
        Limit fault_limit{Limit::None};
        bool execution_active{};

        void fault_at(Limit limit) noexcept {
            if (!faulted) {
                fault_limit = limit;
            }
            faulted = true;
        }

        // "WEFT_VIEW_FAULTED: " and the limit that faulted the View, if one did.
        std::string fault_prefix() const {
            const std::string prefix = "WEFT_VIEW_FAULTED: ";
            switch (fault_limit) {
                case Limit::VmMemory:
                    return prefix + "VM memory limit of " +
                           std::to_string(constants::script::vm_memory_limit / constants::bytes_per_mib) +
                           " MiB reached; ";
                case Limit::ExecutionBudget:
                    return prefix + "execution budget of " + std::to_string(config.execution_budget_ms) +
                           " ms exceeded; ";
                case Limit::None:
                    break;
            }
            return prefix;
        }

        std::chrono::steady_clock::time_point deadline;

        struct alignas(std::max_align_t) AllocationHeader {
            std::size_t size;
        };

        std::size_t vm_bytes{};
        ScriptConfig config;

        explicit Impl(ScriptConfig input) : config(input) {
            if (config.max_stack_bytes < constants::script::min_vm_stack_bytes ||
                config.max_stack_bytes > constants::script::max_vm_stack_bytes) {
                throw std::invalid_argument("WEFT_VM_STACK: expected [128KiB,2MiB] host-backed limit");
            }
            if (config.execution_budget_ms < constants::script::min_execution_budget_ms ||
                config.execution_budget_ms > constants::script::max_execution_budget_ms) {
                throw std::invalid_argument("WEFT_VM_BUDGET: expected [10,30000] ms");
            }
            try {
                initialize();
            } catch (const PendingJsException&) {
                throw std::runtime_error("WEFT_INITIALIZATION_FAILED: VM allocation or bootstrap exception");
            }
        }

        ~Impl() {
            // QuickJS is not thread safe. The M0 evidence API intentionally cannot move.
            owner_thread.require_or_terminate();
            shutdown();
        }

        void check_owner() const {
            owner_thread.require(Diag::WrongThread, "script/DOM operation outside UI owner");
            if (layout_running) {
                throw std::logic_error("WEFT_LAYOUT_REENTRY: layout/measure must not enter JS or DOM");
            }
            if (engine_running) {
                throw std::logic_error("WEFT_ENGINE_REENTRY: a sync engine function must not enter JS or DOM");
            }
        }

        // The engine bridge: the channel, the sync functions, the page's messages not yet taken and their bytes, and
        // whether a sync function runs.
        std::shared_ptr<bridge::EngineChannel> engine;
        std::map<std::string, EngineFunction, std::less<>> engine_functions;
        std::vector<EngineMessage> engine_messages;
        std::size_t engine_message_bytes{};
        bool engine_running{};

        static void engine_name(const std::string& name) {
            if (name.empty() || name.size() > constants::bridge::max_name_bytes || !bridge::utf8(name)) {
                throw std::runtime_error("WEFT_ENGINE_NAME: a name of 1 to 256 bytes of UTF-8");
            }
        }

        // A JS string's text as Unicode scalar values (a USVString): QuickJS gives a lone surrogate as its WTF-8
        // form, which becomes U+FFFD.
        std::string scalar_text(JSValueConst value) {
            auto text = string_value(context, value);
            if (bridge::utf8(text)) {
                return text;
            }
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (static_cast<unsigned char>(text[i]) == 0xed && i + 2 < text.size() &&
                    (static_cast<unsigned char>(text[i + 1]) & 0xe0) == 0xa0) {
                    out += "\xef\xbf\xbd";
                    i += 2;
                } else {
                    out.push_back(text[i]);
                }
            }
            return out;
        }

        // A JS value as a bridge value, as weft.engine sends it: null and undefined as nil, booleans, numbers
        // (integers within 2^53 as integers, others and -0 as floats), BigInts within int64 or uint64, strings (lone
        // surrogates as U+FFFD), ArrayBuffers and typed arrays as their bytes, arrays (and proxies of arrays), objects
        // with a toJSON method as what it returns (as in JSON.stringify: a Date as its text), and other objects as maps
        // of their own enumerable string keys (members that are undefined left out). Functions, symbols, numbers that
        // are not finite and cycles are refused, as is anything nested deeper than constants::bridge::max_value_depth
        // or longer than max_value_bytes.
        std::vector<std::uint8_t> engine_value(JSValueConst value) {
            bridge::ValueWriter out;
            std::vector<void*> path;
            engine_write(out, value, path);
            if (out.data().size() > constants::bridge::max_value_bytes) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: a value past the size bound");
            }
            return out.take();
        }

        // ECMAScript's IsArray: an array, or a proxy whose target is one (as MobX's and Immer's arrays are).
        bool is_array(JSValueConst v) {
            if (!JS_IsProxy(v)) {
                return JS_IsArray(v);
            }
            Value target(context, JS_GetProxyTarget(context, v));
            for (;;) {
                if (!JS_IsProxy(target.value)) {
                    return JS_IsArray(target.value);
                }
                JSValue next = JS_GetProxyTarget(context, target.value);
                if (JS_IsException(next)) {
                    throw PendingJsException{};
                }
                JS_FreeValue(context, target.value);
                target.value = next;
            }
        }

        // path holds the objects and arrays the value is in; converted, whether v is what a toJSON returned.
        void engine_write(bridge::ValueWriter& out, JSValueConst v, std::vector<void*>& path, bool converted = false) {
            if (out.data().size() > constants::bridge::max_value_bytes) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: a value past the size bound");
            }
            if (JS_IsNull(v) || JS_IsUndefined(v)) {
                out.nil();
            } else if (JS_IsBool(v)) {
                out.boolean(JS_ToBool(context, v) > 0);
            } else if (JS_IsNumber(v)) {
                double d{};
                if (JS_ToFloat64(context, &d, v) < 0) {
                    throw PendingJsException{};
                }
                if (!std::isfinite(d)) {
                    throw std::runtime_error("WEFT_ENGINE_VALUE: a number that is not finite");
                }
                if (d == std::floor(d) && std::abs(d) <= constants::script::max_safe_integer &&
                    !(d == 0 && std::signbit(d))) {
                    out.integer(std::int64_t(d));
                } else {
                    out.number(d);
                }
            } else if (JS_IsBigInt(v)) {
                const auto digits = string_value(context, v);
                std::int64_t i{};
                std::uint64_t u{};
                const auto* first = digits.data();
                const auto* last = first + digits.size();
                if (const auto r = std::from_chars(first, last, i); r.ec == std::errc{} && r.ptr == last) {
                    out.integer(i);
                } else if (const auto q = std::from_chars(first, last, u); q.ec == std::errc{} && q.ptr == last) {
                    out.unsigned_integer(u);
                } else {
                    throw std::runtime_error("WEFT_ENGINE_VALUE: a BigInt past int64 and uint64");
                }
            } else if (JS_IsString(v)) {
                out.string(scalar_text(v));
            } else if (JS_IsSymbol(v)) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: a symbol cannot cross");
            } else if (JS_IsFunction(context, v)) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: a function cannot cross");
            } else if (!JS_IsObject(v)) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: a value that cannot cross");
            } else {
                if (!converted) {
                    Value to_json(context, JS_GetPropertyStr(context, v, "toJSON"));
                    if (JS_IsFunction(context, to_json.value)) {
                        Value result(context, JS_Call(context, to_json.value, v, 0, nullptr));
                        engine_write(out, result.value, path, true);
                        return;
                    }
                }
                auto* const identity = JS_VALUE_GET_PTR(v);
                if (std::find(path.begin(), path.end(), identity) != path.end()) {
                    throw std::runtime_error("WEFT_ENGINE_VALUE: a cycle cannot cross");
                }
                if (path.size() >= constants::bridge::max_value_depth) {
                    throw std::runtime_error("WEFT_ENGINE_VALUE: nested past the depth bound");
                }
                path.push_back(identity);

                struct Leave {
                    std::vector<void*>& path;

                    ~Leave() {
                        path.pop_back();
                    }
                } leave{path};

                if (JS_IsArrayBuffer(v)) {
                    // Null only for a detached buffer, with its TypeError.
                    std::size_t size{};
                    const auto* data = JS_GetArrayBuffer(context, &size, v);
                    if (!data) {
                        throw PendingJsException{};
                    }
                    out.bytes({data, size});
                } else if (JS_GetTypedArrayType(v) >= 0) {
                    std::size_t offset{};
                    std::size_t length{};
                    std::size_t per{};
                    Value buffer(context, JS_GetTypedArrayBuffer(context, v, &offset, &length, &per));
                    std::size_t size{};
                    const auto* data = JS_GetArrayBuffer(context, &size, buffer.value);
                    if (!data) {
                        throw PendingJsException{};
                    }
                    out.bytes({data + offset, length});
                } else if (is_array(v)) {
                    Value length_value(context, JS_GetPropertyStr(context, v, "length"));
                    std::uint32_t length{};
                    if (JS_ToUint32(context, &length, length_value.value) < 0) {
                        throw PendingJsException{};
                    }
                    out.array(length);
                    for (std::uint32_t i = 0; i < length; ++i) {
                        Value item(context, JS_GetPropertyUint32(context, v, i));
                        engine_write(out, item.value, path);
                    }
                } else {
                    JSPropertyEnum* names{};
                    std::uint32_t count{};
                    if (JS_GetOwnPropertyNames(context, &names, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                        throw PendingJsException{};
                    }

                    struct Names {
                        JSContext* context;
                        JSPropertyEnum* names;
                        std::uint32_t count;

                        ~Names() {
                            JS_FreePropertyEnum(context, names, count);
                        }
                    } held{context, names, count};

                    const auto header = out.open_map();
                    std::uint32_t written{};
                    for (std::uint32_t k = 0; k < count; ++k) {
                        Value member(context, JS_GetProperty(context, v, names[k].atom));
                        if (JS_IsUndefined(member.value)) {
                            continue;
                        }
                        Value key(context, JS_AtomToString(context, names[k].atom));
                        out.string(scalar_text(key.value));
                        engine_write(out, member.value, path);
                        ++written;
                    }
                    out.close(header, written);
                }
            }
        }

        // A bridge value as a JS value: maps as plain objects, bytes as a Uint8Array, integers past 2^53 as BigInts;
        // nothing (an engine function's empty result) as undefined.
        JSValue js_value(std::span<const std::uint8_t> bytes) {
            if (bytes.empty()) {
                return JS_UNDEFINED;
            }
            bridge::ValueReader reader(bytes);
            Value out(context, js_item(reader, 1));
            if (!reader.done()) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: bytes past the value");
            }
            return JS_DupValue(context, out.value);
        }

        // depth counts the arrays and maps the item is in, itself included.
        JSValue js_item(bridge::ValueReader& reader, unsigned depth) {
            const auto item = reader.next();
            using Type = bridge::ValueReader::Type;
            if ((item.type == Type::Array || item.type == Type::Map) && depth > constants::bridge::max_value_depth) {
                throw std::runtime_error("WEFT_ENGINE_VALUE: nested past the depth bound");
            }
            switch (item.type) {
                case Type::Nil:
                    return JS_NULL;
                case Type::Bool:
                    return JS_NewBool(context, item.boolean);
                case Type::Int:
                    return std::abs(double(item.integer)) <= constants::script::max_safe_integer
                               ? JS_NewInt64(context, item.integer)
                               : JS_NewBigInt64(context, item.integer);
                case Type::Unsigned:
                    return JS_NewBigUint64(context, item.unsigned_integer);
                case Type::Float:
                    return JS_NewFloat64(context, item.number);
                case Type::String:
                    return JS_NewStringLen(context, item.text.data(), item.text.size());
                case Type::Bytes:
                    return JS_NewUint8ArrayCopy(context, item.bytes.data(), item.bytes.size());
                case Type::Array: {
                    Value array(context, JS_NewArray(context));
                    for (std::uint32_t i = 0; i < item.count; ++i) {
                        Value element(context, js_item(reader, depth + 1));
                        if (JS_SetPropertyUint32(context, array.value, i, JS_DupValue(context, element.value)) < 0) {
                            throw PendingJsException{};
                        }
                    }
                    return JS_DupValue(context, array.value);
                }
                case Type::Map: {
                    Value object(context, JS_NewObject(context));
                    for (std::uint32_t i = 0; i < item.count; ++i) {
                        const auto key = reader.next();
                        Value member(context, js_item(reader, depth + 1));
                        define(object.value, key.text, member.value);
                    }
                    return JS_DupValue(context, object.value);
                }
            }
            return JS_NULL;
        }

        // Defines an own member (never a setter or __proto__'s prototype).
        void define(JSValueConst object, std::string_view key, JSValueConst value) {
            const JSAtom atom = JS_NewAtomLen(context, key.data(), key.size());
            if (atom == JS_ATOM_NULL) {
                throw PendingJsException{};
            }
            const int defined =
                JS_DefinePropertyValue(context, object, atom, JS_DupValue(context, value), JS_PROP_C_W_E);
            JS_FreeAtom(context, atom);
            if (defined < 0) {
                throw PendingJsException{};
            }
        }

        EngineDelivery deliver_engine() {
            require_runtime();
            EngineDelivery report;
            if (!engine) {
                return report;
            }
            auto pending = engine->take();
            if (pending.models.empty() && pending.inputs.empty()) {
                return report;
            }
            execute([&] {
                std::size_t jobs{};
                for (const auto& model : pending.models) {
                    Value fields(context, JS_NewObject(context));
                    for (const auto& [field, value] : model.fields) {
                        Value member(context, js_value(value));
                        define(fields.value, field, member.value);
                    }
                    Value name(context, JS_NewStringLen(context, model.name.data(), model.name.size()));
                    JSValueConst args[]{name.value, fields.value};
                    Value result(context, call_host(EngineModel, 2, args));
                    checkpoint(jobs);
                    ++report.models;
                }
                for (const auto& input : pending.inputs) {
                    Value value(context, js_value(input.value));
                    if (input.kind == bridge::EngineInput::Kind::Event) {
                        Value name(context, JS_NewStringLen(context, input.name.data(), input.name.size()));
                        JSValueConst args[]{name.value, value.value};
                        Value result(context, call_host(EngineEvent, 2, args));
                        ++report.events;
                    } else {
                        JSValueConst args[]{JS_NewFloat64(context, double(input.call)),
                                            JS_NewBool(context, input.succeeded), value.value};
                        Value result(context, call_host(EngineSettle, 3, args));
                        if (JS_ToBool(context, result.value) > 0) {
                            ++report.completions;
                        } else {
                            ++report.stale;
                        }
                    }
                    checkpoint(jobs);
                }
                Value errors(context, call_host(EngineErrors));
                Value length_value(context, JS_GetPropertyStr(context, errors.value, "length"));
                std::uint32_t length{};
                if (JS_ToUint32(context, &length, length_value.value) < 0) {
                    throw PendingJsException{};
                }
                for (std::uint32_t i = 0; i < length; ++i) {
                    Value error(context, JS_GetPropertyUint32(context, errors.value, i));
                    report.errors.push_back(string_value(context, error.value));
                }
                return JS_UNDEFINED;
            });
            return report;
        }

        void require_runtime() const {
            check_owner();
            if (faulted || !context) {
                throw std::runtime_error(fault_prefix() + "reset_document required after resource limit");
            }
        }

        // Authoritative VM allocator observes even an OOM swallowed by JS try/catch.
        // Header bytes are included; native DOM storage has a separate node/depth bound.
        static void* allocate(void* opaque, std::size_t size) noexcept {
            auto& owner = *static_cast<Impl*>(opaque);
            if (!size) {
                size = 1;
            }
            if (size > constants::script::vm_memory_limit - sizeof(AllocationHeader) ||
                owner.vm_bytes > constants::script::vm_memory_limit - sizeof(AllocationHeader) - size) {
                owner.fault_at(Limit::VmMemory);
                return nullptr;
            }
            auto* block = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
            if (!block) {
                owner.faulted = true;
                return nullptr;
            }
            block->size = size;
            owner.vm_bytes += sizeof(AllocationHeader) + size;
            owner.counters.peak_vm_allocated_bytes = std::max(owner.counters.peak_vm_allocated_bytes, owner.vm_bytes);
            return block + 1;
        }

        static void free_allocation(void* opaque, void* pointer) noexcept {
            if (!pointer) {
                return;
            }
            auto& owner = *static_cast<Impl*>(opaque);
            auto* block = static_cast<AllocationHeader*>(pointer) - 1;
            owner.vm_bytes -= sizeof(AllocationHeader) + block->size;
            std::free(block);
        }

        static void* calloc_allocation(void* opaque, std::size_t count, std::size_t size) noexcept {
            auto& owner = *static_cast<Impl*>(opaque);
            if (size && count > std::numeric_limits<std::size_t>::max() / size) {
                owner.faulted = true;
                return nullptr;
            }
            void* pointer = allocate(opaque, count * size);
            if (pointer) {
                std::memset(pointer, 0, count * size);
            }
            return pointer;
        }

        static void* realloc_allocation(void* opaque, void* pointer, std::size_t size) noexcept {
            if (!pointer) {
                return allocate(opaque, size);
            }
            if (!size) {
                free_allocation(opaque, pointer);
                return nullptr;
            }
            auto& owner = *static_cast<Impl*>(opaque);
            auto* old = static_cast<AllocationHeader*>(pointer) - 1;
            const auto previous = old->size;
            if (size > constants::script::vm_memory_limit - sizeof(AllocationHeader) ||
                owner.vm_bytes - previous > constants::script::vm_memory_limit - size) {
                owner.fault_at(Limit::VmMemory);
                return nullptr;
            }
            auto* block = static_cast<AllocationHeader*>(std::realloc(old, sizeof(AllocationHeader) + size));
            if (!block) {
                owner.faulted = true;
                return nullptr;
            }
            block->size = size;
            owner.vm_bytes = owner.vm_bytes - previous + size;
            owner.counters.peak_vm_allocated_bytes = std::max(owner.counters.peak_vm_allocated_bytes, owner.vm_bytes);
            return block + 1;
        }

        static std::size_t allocation_size(const void* pointer) noexcept {
            return pointer ? (static_cast<const AllocationHeader*>(pointer) - 1)->size : 0;
        }

        static Impl& state(JSContext* c) {
            return *static_cast<Impl*>(JS_GetContextOpaque(c));
        }

        static int interrupt(JSRuntime*, void* opaque) {
            auto& owner = *static_cast<Impl*>(opaque);
            if (owner.execution_active && (owner.faulted || std::chrono::steady_clock::now() >= owner.deadline)) {
                owner.fault_at(Limit::ExecutionBudget);
                return 1;
            }
            return 0;
        }

        static Node* node_unchecked(JSContext* c, JSValueConst value) {
            return static_cast<Node*>(JS_GetOpaque(value, state(c).node_class));
        }

        static Node& node(JSContext* c, JSValueConst value) {
            if (state(c).faulted) {
                throw std::runtime_error("WEFT_VIEW_FAULTED: native DOM operation after resource failure");
            }
            Node* n = node_unchecked(c, value);
            if (!n) {
                throw std::runtime_error("WEFT_DOM_TYPE: expected native Node");
            }
            if (n->identity.view_epoch != state(c).view_epoch) {
                throw std::runtime_error("WEFT_STALE_NODE: revoked View epoch");
            }
            return *n;
        }

        static void mark(JSRuntime* rt, JSValueConst value, JS_MarkFunc* mark_function) {
            auto* owner = static_cast<Impl*>(JS_GetRuntimeOpaque(rt));
            auto* n = static_cast<Node*>(JS_GetOpaque(value, owner->node_class));
            if (!n) {
                return;
            }
            for (const auto& edge : {n->parent, n->document}) {
                if (edge.generation) {
                    JS_MarkValue(rt, owner->edge_wrapper(edge), mark_function);
                }
            }
            for (const auto& child : n->children) {
                JS_MarkValue(rt, owner->edge_wrapper(child), mark_function);
            }
            const auto& binding = owner->bindings[n->identity.slot];
            JS_MarkValue(rt, binding.expando, mark_function);
            for (const Listener& l : binding.listeners) {
                JS_MarkValue(rt, l.callback, mark_function);
            }
            if (const auto* canvas = owner->canvases.find(n->identity)) {
                JS_MarkValue(rt, canvas->context, mark_function);
            }
        }

        static void finalize(JSRuntime* rt, JSValueConst value) {
            auto* owner = static_cast<Impl*>(JS_GetRuntimeOpaque(rt));
            auto* n = static_cast<Node*>(JS_GetOpaque(value, owner->node_class));
            if (!n) {
                return;
            }
            JS_SetOpaque(value, nullptr);
            // Out of the tree first, so nothing resolves the node while its references go; destroyed on return.
            const auto node = owner->tree.release(n->identity);
            ++owner->counters.finalized_nodes;
            --owner->counters.live_nodes;
            // No script, layout, or other Node dereference is allowed in finalizers.
            for (const auto& edge : {n->parent, n->document}) {
                if (edge.generation) {
                    JS_FreeValueRT(rt, owner->edge_wrapper(edge));
                }
            }
            for (const auto& child : n->children) {
                JS_FreeValueRT(rt, owner->edge_wrapper(child));
            }
            auto& binding = owner->bindings[n->identity.slot];
            JS_FreeValueRT(rt, binding.expando);
            binding.expando = JS_UNDEFINED;
            for (const Listener& l : binding.listeners) {
                JS_FreeValueRT(rt, l.callback);
            }
            binding.listeners.clear();
            binding.scroll_pending = false;
            binding.field_dirty = false;
            binding.value_before_edit.reset();
            binding.selectionchange_pending = false;
            if (auto* canvas = owner->canvases.find(n->identity)) {
                JS_FreeValueRT(rt, canvas->context);
                owner->canvases.erase(n->identity);
            }
        }

        // Expandos live in an ordinary JS object, owned by the native wrapper graph.
        // This gives __data__, __on, symbols, and arbitrary closures an explicit mark edge.
        static int own_property(JSContext* c, JSPropertyDescriptor* desc, JSValueConst value, JSAtom atom) {
            Node* n = node_unchecked(c, value);
            return n ? JS_GetOwnProperty(c, desc, state(c).binding(*n).expando, atom) : 0;
        }

        static int own_names(JSContext* c, JSPropertyEnum** table, std::uint32_t* length, JSValueConst value) {
            Node* n = node_unchecked(c, value);
            return n ? JS_GetOwnPropertyNames(c, table, length, state(c).binding(*n).expando,
                                              JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_SET_ENUM)
                     : -1;
        }

        static int delete_property(JSContext* c, JSValueConst value, JSAtom atom) {
            Node* n = node_unchecked(c, value);
            return n ? JS_DeleteProperty(c, state(c).binding(*n).expando, atom, 0) : -1;
        }

        static int define_property(JSContext* c, JSValueConst value, JSAtom atom, JSValueConst v, JSValueConst getter,
                                   JSValueConst setter, int flags) {
            Node* n = node_unchecked(c, value);
            return n ? JS_DefineProperty(c, state(c).binding(*n).expando, atom, v, getter, setter, flags) : -1;
        }

        struct CanvasHandle {
            JSValue canvas;
        };

        static void canvas_mark(JSRuntime* rt, JSValueConst value, JS_MarkFunc* mark_function) {
            auto& owner = *static_cast<Impl*>(JS_GetRuntimeOpaque(rt));
            if (auto* p = static_cast<CanvasHandle*>(JS_GetOpaque(value, owner.canvas_class))) {
                JS_MarkValue(rt, p->canvas, mark_function);
            }
        }

        static void canvas_finalize(JSRuntime* rt, JSValueConst value) {
            auto& owner = *static_cast<Impl*>(JS_GetRuntimeOpaque(rt));
            if (auto* p = static_cast<CanvasHandle*>(JS_GetOpaque(value, owner.canvas_class))) {
                JS_SetOpaque(value, nullptr);
                JS_FreeValueRT(rt, p->canvas);
                delete p;
            }
        }

        static Node& canvas_node(JSContext* c, JSValueConst self) {
            auto* n = node_unchecked(c, self);
            if (!n || n->type != constants::element_node_type || n->local != "canvas" ||
                n->space != constants::xhtml_namespace_uri) {
                JS_ThrowTypeError(c, "WEFT_CANVAS_TYPE: native HTML canvas required");
                throw PendingJsException{};
            }
            return node(c, self);
        }

        // The canvas element behind a CanvasRenderingContext2D wrapper, and its 2D state.
        struct ContextTarget {
            Node& element;
            CanvasState& canvas;
        };

        static ContextTarget context_target(JSContext* c, JSValueConst self) {
            auto* p = static_cast<CanvasHandle*>(JS_GetOpaque(self, state(c).canvas_class));
            if (!p) {
                JS_ThrowTypeError(c, "WEFT_CANVAS_CONTEXT_TYPE: native 2d context required");
                throw PendingJsException{};
            }
            auto& n = canvas_node(c, p->canvas);
            auto* entry = state(c).canvases.find(n.identity);
            if (!entry) {
                throw std::runtime_error("WEFT_CANVAS_CONTEXT_STATE");
            }
            return {n, *entry->canvas};
        }

        // A side of a canvas element's bitmap; replacement stands for the attribute while it is being set.
        static std::uint32_t canvas_dimension(const Node& n, dom::CanvasSide side,
                                              const std::string* replacement = nullptr) {
            const std::string* attribute = replacement;
            if (!attribute) {
                const std::string_view name = side == dom::CanvasSide::Width ? "width" : "height";
                const auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const Attribute& a) {
                    return a.name == name;
                });
                attribute = it == n.attributes.end() ? nullptr : &it->value;
            }
            auto value = dom::canvas_dimension(attribute, side);
            if (!value) {
                throw DiagnosticError(value.error());
            }
            return *value;
        }

        void resize_canvas_attribute(Node& n, const std::string& name, const std::string& value) {
            if (JS_IsUndefined(canvas_prototype) || n.local != "canvas" || n.space != constants::xhtml_namespace_uri ||
                (name != "width" && name != "height")) {
                return;
            }
            const auto w = canvas_dimension(n, dom::CanvasSide::Width, name == "width" ? &value : nullptr);
            const auto h = canvas_dimension(n, dom::CanvasSide::Height, name == "height" ? &value : nullptr);
            CanvasState::validate_size(w, h);
            if (auto* entry = canvases.find(n.identity)) {
                try {
                    entry->canvas->resize(w, h);
                } catch (const CanvasBackendError&) {
                    faulted = true;
                    throw;
                }
            }
        }

        static JSValue canvas_node_get(JSContext* c, JSValueConst self, int magic) {
            try {
                auto& n = canvas_node(c, self);
                auto& owner = state(c);
                if (magic < 2) {
                    return JS_NewUint32(
                        c, owner.canvas_dimension(n, magic == 0 ? dom::CanvasSide::Width : dom::CanvasSide::Height));
                }
                const auto type = magic == 2 ? "click" : "mousemove";
                for (const auto& l : owner.binding(n).listeners) {
                    if (l.handler && l.type == type) {
                        return JS_DupValue(c, l.callback);
                    }
                }
                return JS_NULL;
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            }
        }

        static JSValue canvas_node_set(JSContext* c, JSValueConst self, JSValueConst value, int magic) {
            try {
                auto& n = canvas_node(c, self);
                auto& owner = state(c);
                if (magic < 2) {
                    std::uint32_t number{};
                    if (JS_ToUint32(c, &number, value) < 0) {
                        throw PendingJsException{};
                    }
                    const std::string key = magic == 0 ? "width" : "height";
                    const std::string converted = std::to_string(number);
                    owner.resize_canvas_attribute(n, key, converted);
                    auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const auto& a) {
                        return a.space.empty() && a.name == key;
                    });
                    if (it == n.attributes.end()) {
                        n.attributes.push_back({key, key, "", converted});
                    } else {
                        it->value = converted;
                    }
                    owner.changed(n);
                    return JS_UNDEFINED;
                }
                const auto type = magic == 2 ? "click" : "mousemove";
                auto& listeners = owner.binding(n).listeners;
                auto it = std::find_if(listeners.begin(), listeners.end(), [&](const auto& l) {
                    return l.handler && l.type == type;
                });
                if (!JS_IsFunction(c, value)) {
                    if (it != listeners.end()) {
                        JS_FreeValue(c, it->callback);
                        listeners.erase(it);
                    }
                    return JS_UNDEFINED;
                }
                if (it != listeners.end()) {
                    JS_FreeValue(c, it->callback);
                    it->callback = JS_DupValue(c, value);
                } else {
                    listeners.push_back({owner.next_listener++, type, JS_UNDEFINED, false, false, true});
                    listeners.back().callback = JS_DupValue(c, value);
                }
                return JS_UNDEFINED;
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            }
        }

        static JSValue canvas_context_get(JSContext* c, JSValueConst self, int magic) {
            try {
                const auto target = context_target(c, self);
                if (magic == 0) {
                    return JS_DupValue(c, state(c).wrapper(target.element));
                }
                if (magic == 1) {
                    return JS_NewString(c, target.canvas.fill_style.c_str());
                }
                return JS_NewFloat64(c, target.canvas.global_alpha);
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            }
        }

        static JSValue canvas_context_set(JSContext* c, JSValueConst self, JSValueConst value, int magic) {
            try {
                auto& canvas = context_target(c, self).canvas;
                if (magic == 1) {
                    auto text = string_value(c, value);
                    if (auto color = parse_canvas_color(text)) {
                        std::ostringstream s;
                        s.imbue(std::locale::classic());
                        if ((*color)[3] == 1) {
                            s << '#' << std::hex << std::setfill('0');
                            for (unsigned i = 0; i < 3; ++i) {
                                s << std::setw(2) << unsigned(std::lround((*color)[i] * constants::color_channel_max));
                            }
                        } else {
                            s << "rgba(" << unsigned(std::lround((*color)[0] * constants::color_channel_max)) << ", "
                              << unsigned(std::lround((*color)[1] * constants::color_channel_max)) << ", "
                              << unsigned(std::lround((*color)[2] * constants::color_channel_max)) << ", "
                              << std::setprecision(constants::script::canvas_alpha_serialization_digits) << (*color)[3]
                              << ')';
                        }
                        canvas.color = *color;
                        canvas.fill_style = s.str();
                    }
                } else {
                    double alpha{};
                    if (JS_ToFloat64(c, &alpha, value) < 0) {
                        throw PendingJsException{};
                    }
                    if (std::isfinite(alpha) && alpha >= 0 && alpha <= 1) {
                        canvas.global_alpha = alpha;
                    }
                }
                return JS_UNDEFINED;
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            }
        }

        static JSValue canvas_draw(JSContext* c, JSValueConst self, int argc, JSValueConst* argv, int magic) {
            try {
                auto& canvas = context_target(c, self).canvas;
                if (magic == 0) {
                    canvas.begin_path();
                    return JS_UNDEFINED;
                }
                if (magic == 1) {
                    canvas.fill(argc ? string_value(c, argv[0]) : "nonzero");
                    return JS_UNDEFINED;
                }
                std::array<double, 5> values{};
                const int required = magic == 2 ? 5 : 4;
                if (argc < required) {
                    return JS_ThrowTypeError(c, "Canvas method missing required arguments");
                }
                for (int i = 0; i < required; ++i) {
                    if (JS_ToFloat64(c, &values[i], argv[i]) < 0) {
                        throw PendingJsException{};
                    }
                }
                if (magic == 2) {
                    canvas.arc(values[0], values[1], values[2], values[3], values[4],
                               argc > 5 && JS_ToBool(c, argv[5]));
                } else {
                    canvas.fill_rect(values[0], values[1], values[2], values[3], magic == 4);
                }
                return JS_UNDEFINED;
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const DiagnosticError& e) {
                if (e.code() == Diag::CanvasFillRule) {
                    return JS_ThrowTypeError(c, "%s", e.what());
                }
                if (e.code() == Diag::CanvasIndexSize) {
                    JSValue error = JS_NewError(c);
                    if (JS_IsException(error)) {
                        return error;
                    }
                    JS_SetPropertyStr(c, error, "name", JS_NewString(c, diag_name(Diag::CanvasIndexSize)));
                    JS_SetPropertyStr(c, error, "message", JS_NewString(c, e.what()));
                    return JS_Throw(c, error);
                }
                return JS_ThrowInternalError(c, "%s", e.what());
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            }
        }

        static JSValue canvas_get_context(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) {
            try {
                auto& n = canvas_node(c, self);
                auto& owner = state(c);
                if (argc < 1) {
                    return JS_ThrowTypeError(c, "getContext requires context id");
                }
                if (string_value(c, argv[0]) != "2d") {
                    return JS_NULL;
                }
                if (const auto* entry = owner.canvases.find(n.identity)) {
                    return JS_DupValue(c, entry->context);
                }
                bool alpha = true;
                if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
                    if (!JS_IsObject(argv[1])) {
                        return JS_ThrowTypeError(c, "Canvas context options require dictionary");
                    }
                    Value option(c, JS_GetPropertyStr(c, argv[1], "alpha"));
                    if (!JS_IsUndefined(option.value)) {
                        alpha = JS_ToBool(c, option.value);
                    }
                    Value space(c, JS_GetPropertyStr(c, argv[1], "colorSpace"));
                    if (!JS_IsUndefined(space.value) && string_value(c, space.value) != "srgb") {
                        throw std::invalid_argument("WEFT_CANVAS_COLOR_SPACE: only srgb");
                    }
                }
                // Reading the options runs script, which may have created the context meanwhile.
                if (const auto* entry = owner.canvases.find(n.identity)) {
                    return JS_DupValue(c, entry->context);
                }
                auto canvas = std::make_unique<CanvasState>(owner.canvas_factory, owner.canvas_budget,
                                                            canvas_dimension(n, dom::CanvasSide::Width),
                                                            canvas_dimension(n, dom::CanvasSide::Height), alpha);
                Value object(c, JS_NewObjectClass(c, owner.canvas_class));
                if (JS_IsException(object.value)) {
                    throw PendingJsException{};
                }
                auto handle = std::make_unique<CanvasHandle>();
                handle->canvas = JS_DupValue(c, self);
                JS_SetOpaque(object.value, handle.release());
                auto& entry = owner.canvases.emplace(n.identity, CanvasEntry{JS_UNDEFINED, std::move(canvas)});
                entry.context = JS_DupValue(c, object.value);
                return JS_DupValue(c, object.value);
            } catch (const CanvasBackendError& e) {
                state(c).faulted = true;
                return JS_ThrowInternalError(c, "%s", e.what());
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            }
        }

        void install_canvas(CanvasFactory factory) {
            require_runtime();
            if (execution_active || !factory || !JS_IsUndefined(canvas_prototype)) {
                throw std::logic_error("WEFT_CANVAS_INSTALL: owner factory installed once before app");
            }
            tree.for_each([](const Node& n) {
                if (n.local == "canvas") {
                    throw std::logic_error("WEFT_CANVAS_INSTALL: existing canvas node");
                }
            });
            JS_NewClassID(runtime, &canvas_class);
            JSClassDef definition{};
            definition.class_name = "CanvasRenderingContext2D";
            definition.finalizer = canvas_finalize;
            definition.gc_mark = canvas_mark;
            if (JS_NewClass(runtime, canvas_class, &definition) < 0) {
                throw std::runtime_error("WEFT_CANVAS_CLASS");
            }
            Value context_proto(context, JS_NewObject(context));
            static const JSCFunctionListEntry context_functions[] = {
                JS_CGETSET_MAGIC_DEF("canvas", canvas_context_get, nullptr, 0),
                JS_CGETSET_MAGIC_DEF("fillStyle", canvas_context_get, canvas_context_set, 1),
                JS_CGETSET_MAGIC_DEF("globalAlpha", canvas_context_get, canvas_context_set, 2),
                JS_CFUNC_MAGIC_DEF("beginPath", 0, canvas_draw, 0),
                JS_CFUNC_MAGIC_DEF("fill", 0, canvas_draw, 1),
                JS_CFUNC_MAGIC_DEF("arc", 5, canvas_draw, 2),
                JS_CFUNC_MAGIC_DEF("fillRect", 4, canvas_draw, 3),
                JS_CFUNC_MAGIC_DEF("clearRect", 4, canvas_draw, 4)};
            JS_SetPropertyFunctionList(context, context_proto.value, context_functions, std::size(context_functions));
            JS_SetClassProto(context, canvas_class, JS_DupValue(context, context_proto.value));
            Value base(context, JS_GetClassProto(context, node_class));
            Value proto(context, JS_NewObjectProto(context, base.value));
            static const JSCFunctionListEntry functions[] = {
                JS_CGETSET_MAGIC_DEF("width", canvas_node_get, canvas_node_set, 0),
                JS_CGETSET_MAGIC_DEF("height", canvas_node_get, canvas_node_set, 1),
                JS_CGETSET_MAGIC_DEF("onclick", canvas_node_get, canvas_node_set, 2),
                JS_CGETSET_MAGIC_DEF("onmousemove", canvas_node_get, canvas_node_set, 3),
                JS_CFUNC_DEF("getContext", 1, canvas_get_context)};
            JS_SetPropertyFunctionList(context, proto.value, functions, std::size(functions));
            canvas_factory = std::move(factory);
            canvas_prototype = JS_DupValue(context, proto.value);
        }

        // A new node of the document named by document (a null identity for the document node itself).
        JSValue make_node(int type, std::string local, std::string space, NodeIdentity document) {
            if (counters.live_nodes >= constants::script::max_live_nodes) {
                faulted = true;
                throw std::runtime_error("WEFT_NODE_LIMIT: native DOM node limit exceeded");
            }
            Value object(context, JS_NewObjectClass(context, node_class));
            if (type == constants::element_node_type && local == "canvas" && space == constants::xhtml_namespace_uri &&
                !JS_IsUndefined(canvas_prototype)) {
                if (JS_SetPrototype(context, object.value, canvas_prototype) < 0) {
                    throw PendingJsException{};
                }
            }
            if (type == constants::element_node_type && (local == "input" || local == "textarea") &&
                space == constants::xhtml_namespace_uri) {
                if (JS_SetPrototype(context, object.value, field_prototype) < 0) {
                    throw PendingJsException{};
                }
            }
            if (JS_IsException(object.value)) {
                faulted = true;
                throw std::runtime_error("native wrapper allocation failed");
            }
            Value expando(context, JS_NewObjectProto(context, JS_NULL));
            if (JS_IsException(expando.value)) {
                faulted = true;
                throw std::runtime_error("expando allocation failed");
            }
            // The binding table grows first: the new node takes a free slot or the next one.
            if (bindings.size() <= tree.slot_count()) {
                bindings.emplace_back();
            }
            Node& n = tree.create(type, std::move(local), std::move(space), document);
            if (document.generation) {
                JS_DupValue(context, edge_wrapper(document));
            }
            auto& binding = bindings[n.identity.slot];
            binding.wrapper = object.value;
            binding.expando = expando.value;
            expando.value = JS_UNDEFINED;
            JS_SetOpaque(object.value, &n);
            sync_field(n);
            ++counters.created_nodes;
            ++counters.live_nodes;
            return JS_DupValue(context, object.value);
        }

        // Removes child from its parent; the caller holds child. The edge pair's references go once the tree is
        // consistent, because releasing them may finalize nodes.
        void detach(Node& child) {
            // Sequential navigation goes on from where a starting node inside child was (Blink keeps a Range there).
            if (const auto* start = resolve(start_node); start && !start_parent.generation) {
                for (const Node* n = start; n; n = parent_of(*n)) {
                    if (n == &child) {
                        if (const auto* parent = parent_of(child)) {
                            const auto& siblings = parent->children;
                            const auto it = std::find(siblings.begin(), siblings.end(), child.identity);
                            start_parent = parent->identity;
                            start_next = it != siblings.end() && it + 1 != siblings.end() ? *(it + 1) : NodeIdentity{};
                        }
                        break;
                    }
                }
            }
            const auto former = tree.detach(child);
            check_journal();
            textarea_changed(former);
            if (former.generation) {
                JS_FreeValue(context, edge_wrapper(former));
                JS_FreeValue(context, wrapper(child));
            }
        }

        // Moves child into parent before `before` (last when null). The new edge pair takes its references before the
        // former pair releases its own, so child never drops to zero references in between.
        void insert(Node& parent, Node& child, Node* before) {
            const auto move = tree.insert(parent, child, before);
            check_journal();
            if (!move.moved) {
                return;
            }
            JS_DupValue(context, wrapper(child));
            JS_DupValue(context, wrapper(parent));
            if (move.former_parent.generation) {
                JS_FreeValue(context, edge_wrapper(move.former_parent));
                JS_FreeValue(context, wrapper(child));
                textarea_changed(move.former_parent);
            }
            textarea_changed(parent.identity);
        }

        std::string text(const Node& n) const {
            return tree.text(n);
        }

        void set_text(Node& n, std::string input) {
            if (n.type == constants::text_node_type) {
                if (n.data != input) {
                    n.data = std::move(input);
                    changed(n);
                    textarea_changed(n.parent);
                }
                return;
            }
            if (n.type == constants::document_node_type) {
                return; // Document.textContent setter has no effect.
            }
            Value replacement(context, JS_UNDEFINED);
            if (!input.empty()) {
                replacement.value = make_node(constants::text_node_type, "#text", "", n.document);
                node(context, replacement.value).data = std::move(input);
                n.children.reserve(std::max<std::size_t>(n.children.size(), 1));
            }
            blur_within(n, false);
            while (!n.children.empty()) {
                Node& child = node_at(n.children.back());
                Value hold(context, JS_DupValue(context, wrapper(child)));
                detach(child);
            }
            if (!JS_IsUndefined(replacement.value)) {
                insert(n, node(context, replacement.value), nullptr);
            }
            changed(n);
        }

        static std::string attr(const Node& n, std::string_view name) {
            auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const Attribute& a) {
                return a.name == name;
            });
            return it == n.attributes.end() ? "" : it->value;
        }

        // Selector lists of DOM queries: the shared grammar with escapes and :hover/:active, or Selectors 4's as the
        // page's stylesheet takes them (modern_selectors).
        bool modern_selectors{};

        std::vector<dom::Selector> selectors(std::string_view text) const {
            static const dom::SelectorGrammar strict{true, true, Diag::DomSelector, Diag::DomUnsupportedSelector};
            static const dom::SelectorGrammar modern{true, true, Diag::DomSelector, Diag::DomUnsupportedSelector, true};
            auto list =
                dom::parse_selector_list(text, modern_selectors ? modern : strict, constants::dom::max_selector_list_items);
            if (!list) {
                throw std::runtime_error(list.error().message);
            }
            return std::move(*list);
        }

        bool matches(const Node& n, std::string_view selector) const {
            return tree.matches(n, selectors(selector));
        }

        // Compiles the list once, so an unsupported selector fails even when nothing would be matched.
        void query(Node& n, std::string_view selector, std::vector<Node*>& result, bool first) const {
            tree.query(n, selectors(selector), result, first);
        }

        enum Property {
            Type,
            Name,
            LocalName,
            Namespace,
            OwnerDocument,
            Parent,
            Children,
            FirstChild,
            LastChild,
            NextSibling,
            PreviousSibling,
            TextContent,
            NodeValue,
            DocumentElement,
            Body,
            DefaultView,
            IsConnected,
            Data,
            ElementId,
            ClientWidth,
            ClientHeight,
            ScrollWidth,
            ScrollHeight,
            ScrollLeft,
            ScrollTop,
            OffsetLeft,
            OffsetTop,
            OffsetWidth,
            OffsetHeight,
            OffsetParent,
            ActiveElement,
            TabIndex,
            // The text field API (field_property, set_field_property): every magic from FieldValue on.
            FieldValue,
            DefaultValue,
            SelectionStart,
            SelectionEnd,
            SelectionDirection,
            InputType,
            Placeholder,
            ReadOnly,
            Disabled,
            MaxLength,
            FieldName,
            TextLength
        };

        static JSValue get_property(JSContext* c, JSValueConst self, int magic) {
            try {
                Node& n = node(c, self);
                Impl& owner = state(c);
                if (magic >= FieldValue) {
                    return owner.field_property(n, magic);
                }
                // A text field scrolls its text, not its box: its scroll offsets are its field's.
                if (n.field && (magic == ScrollLeft || magic == ScrollTop)) {
                    return JS_NewFloat64(c, magic == ScrollLeft ? n.field->scroll_x : n.field->scroll_y);
                }
                switch (magic) {
                    case OffsetLeft:
                    case OffsetTop:
                    case OffsetWidth:
                    case OffsetHeight:
                    case OffsetParent: {
                        const auto g = owner.geometry(n);
                        if (magic == OffsetParent) {
                            auto* p = owner.resolve(g.offset_parent);
                            return p ? JS_DupValue(c, owner.wrapper(*p)) : JS_NULL;
                        }
                        return JS_NewFloat64(c, std::round(g.offset[std::size_t(magic - OffsetLeft)]));
                    }
                    case ClientWidth:
                    case ClientHeight:
                    case ScrollWidth:
                    case ScrollHeight:
                    case ScrollLeft:
                    case ScrollTop: {
                        const auto g = owner.geometry(n);
                        double v{};
                        if (magic == ClientWidth) {
                            v = std::round(g.client_width);
                        }
                        if (magic == ClientHeight) {
                            v = std::round(g.client_height);
                        }
                        if (magic == ScrollWidth) {
                            v = std::round(g.scroll_width);
                        }
                        if (magic == ScrollHeight) {
                            v = std::round(g.scroll_height);
                        }
                        if (magic == ScrollLeft) {
                            v = g.scroll_left;
                        }
                        if (magic == ScrollTop) {
                            v = g.scroll_top;
                        }
                        return JS_NewFloat64(c, v);
                    }
                    case Type:
                        return JS_NewInt32(c, n.type);
                    case ActiveElement: {
                        // document.activeElement: the focused element, else the body (the document element without
                        // one).
                        if (n.type != constants::document_node_type) {
                            return JS_UNDEFINED;
                        }
                        if (auto* f = owner.focused_node()) {
                            return JS_DupValue(c, owner.wrapper(*f));
                        }
                        std::vector<Node*> found;
                        owner.query(n, "body", found, true);
                        if (!found.empty()) {
                            return JS_DupValue(c, owner.wrapper(*found.front()));
                        }
                        return n.children.empty() ? JS_NULL : JS_DupValue(c, owner.wrapper_at(n.children.front()));
                    }
                    case TabIndex:
                        return n.type == constants::element_node_type ? JS_NewInt32(c, owner.tab_index(n)) : JS_UNDEFINED;
                    case Name: {
                        std::string name = n.type == constants::document_node_type ? "#document" : n.local;
                        if (n.space == constants::xhtml_namespace_uri) {
                            for (char& x : name) {
                                if (x >= 'a' && x <= 'z') {
                                    x -= 'a' - 'A';
                                }
                            }
                        }
                        return JS_NewStringLen(c, name.data(), name.size());
                    }
                    case ElementId: {
                        if (n.type != constants::element_node_type) {
                            return JS_UNDEFINED;
                        }
                        const auto id = owner.attr(n, "id");
                        return JS_NewStringLen(c, id.data(), id.size());
                    }
                    case LocalName:
                        return n.type == constants::element_node_type ? JS_NewString(c, n.local.c_str()) : JS_NULL;
                    case Namespace:
                        return n.space.empty() ? JS_NULL : JS_NewString(c, n.space.c_str());
                    case OwnerDocument:
                        return n.type == constants::document_node_type || !n.document.generation
                                   ? JS_NULL
                                   : JS_DupValue(c, owner.wrapper_at(n.document));
                    case Parent:
                        return n.parent.generation ? JS_DupValue(c, owner.wrapper_at(n.parent)) : JS_NULL;
                    case Children: {
                        Value result(c, JS_NewArray(c));
                        for (std::uint32_t i{}; i < n.children.size(); ++i) {
                            if (JS_SetPropertyUint32(c, result.value, i,
                                                     JS_DupValue(c, owner.wrapper_at(n.children[i]))) < 0) {
                                return JS_EXCEPTION;
                            }
                        }
                        return JS_DupValue(c, result.value);
                    }
                    case FirstChild:
                        return n.children.empty() ? JS_NULL : JS_DupValue(c, owner.wrapper_at(n.children.front()));
                    case LastChild:
                        return n.children.empty() ? JS_NULL : JS_DupValue(c, owner.wrapper_at(n.children.back()));
                    case NextSibling:
                    case PreviousSibling: {
                        const Node* parent = owner.parent_of(n);
                        if (!parent) {
                            return JS_NULL;
                        }
                        const auto& siblings = parent->children;
                        auto it = std::find(siblings.begin(), siblings.end(), n.identity);
                        if (magic == NextSibling && it != siblings.end() && ++it != siblings.end()) {
                            return JS_DupValue(c, owner.wrapper_at(*it));
                        }
                        if (magic == PreviousSibling && it != siblings.begin() && it != siblings.end()) {
                            return JS_DupValue(c, owner.wrapper_at(*--it));
                        }
                        return JS_NULL;
                    }
                    case TextContent: {
                        if (n.type == constants::document_node_type) {
                            return JS_NULL;
                        }
                        std::string value = owner.text(n);
                        return JS_NewStringLen(c, value.data(), value.size());
                    }
                    case NodeValue:
                        return n.type == constants::text_node_type ? JS_NewStringLen(c, n.data.data(), n.data.size())
                                                                   : JS_NULL;
                    case Data:
                        return JS_NewStringLen(c, n.data.data(), n.data.size());
                    case DocumentElement:
                        return n.type == constants::document_node_type && !n.children.empty()
                                   ? JS_DupValue(c, owner.wrapper_at(n.children.front()))
                                   : JS_UNDEFINED;
                    case Body: {
                        if (n.type != constants::document_node_type) {
                            return JS_UNDEFINED;
                        }
                        std::vector<Node*> found;
                        owner.query(n, "body", found, true);
                        return found.empty() ? JS_NULL : JS_DupValue(c, owner.wrapper(*found.front()));
                    }
                    case DefaultView:
                        return n.type == constants::document_node_type ? JS_GetGlobalObject(c) : JS_UNDEFINED;
                    case IsConnected: {
                        const Node* p = &n;
                        while (const Node* next = owner.parent_of(*p)) {
                            p = next;
                        }
                        return JS_NewBool(c, p->type == constants::document_node_type);
                    }
                    default:
                        return JS_UNDEFINED;
                }
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            } catch (...) {
                state(c).faulted = true;
                return JS_ThrowInternalError(c, "WEFT_NATIVE_EXCEPTION: unknown native getter exception");
            }
        }

        static JSValue set_property(JSContext* c, JSValueConst self, JSValueConst value, int magic) {
            try {
                Node& n = node(c, self);
                if (magic >= FieldValue) {
                    state(c).set_field_property(n, magic, value);
                    return JS_UNDEFINED;
                }
                if (n.field && (magic == ScrollLeft || magic == ScrollTop)) {
                    double v{};
                    if (JS_ToFloat64(c, &v, value) < 0) {
                        throw PendingJsException{};
                    }
                    if (n.field) {
                        const auto offset = static_cast<float>(std::isfinite(v) ? std::max(0., v) : 0.);
                        state(c).set_field_view(n, n.field->caret_on, magic == ScrollLeft ? offset : n.field->scroll_x,
                                                magic == ScrollTop ? offset : n.field->scroll_y);
                    }
                    return JS_UNDEFINED;
                }
                if (magic == ScrollLeft || magic == ScrollTop) {
                    double v{};
                    if (JS_ToFloat64(c, &v, value) < 0) {
                        throw PendingJsException{};
                    }
                    if (!std::isfinite(v)) {
                        v = 0;
                    }
                    auto& owner = state(c);
                    auto g = owner.geometry(n);
                    if (magic == ScrollLeft && g.scroll_x) {
                        n.scroll_left = std::clamp(v, 0., std::max(0., g.scroll_width - g.client_width));
                    }
                    if (magic == ScrollTop && g.scroll_y) {
                        n.scroll_top = std::clamp(v, 0., std::max(0., g.scroll_height - g.client_height));
                    }
                    if ((magic == ScrollLeft && n.scroll_left != g.scroll_left) ||
                        (magic == ScrollTop && n.scroll_top != g.scroll_top)) {
                        owner.changed(n);
                        owner.queue_scroll(n);
                    }
                    return JS_UNDEFINED;
                }
                if (magic == TabIndex) {
                    // tabIndex reflects tabindex as an integer (WebIDL long).
                    std::int32_t v{};
                    if (JS_ToInt32(c, &v, value) < 0) {
                        throw PendingJsException{};
                    }
                    if (n.type == constants::element_node_type) {
                        state(c).set_attribute(n, "tabindex", std::to_string(v));
                    }
                    return JS_UNDEFINED;
                }
                if (magic == ElementId) {
                    if (n.type != constants::element_node_type) {
                        throw std::runtime_error("WEFT_DOM_TYPE: id reflection requires Element");
                    }
                    std::string id = string_value(c, value); // Convert before acquiring an attribute iterator.
                    auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [](const Attribute& a) {
                        return a.space.empty() && a.name == "id";
                    });
                    if (it == n.attributes.end()) {
                        n.attributes.push_back({"id", "id", {}, std::move(id)});
                    } else {
                        it->value = std::move(id);
                    }
                    state(c).changed(n);
                    return JS_UNDEFINED;
                }
                std::string input = JS_IsNull(value) ? "" : string_value(c, value);
                if (magic == TextContent) {
                    state(c).set_text(n, std::move(input));
                } else if (n.type == constants::text_node_type && n.data != input) {
                    n.data = std::move(input);
                    state(c).changed(n);
                    state(c).textarea_changed(n.parent);
                }
                return JS_UNDEFINED;
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            } catch (...) {
                state(c).faulted = true;
                return JS_ThrowInternalError(c, "WEFT_NATIVE_EXCEPTION: unknown native setter exception");
            }
        }

        enum Method {
            Append,
            InsertBefore,
            Remove,
            CreateElement,
            CreateElementNS,
            CreateTextNode,
            QuerySelector,
            QuerySelectorAll,
            Matches,
            Closest,
            SetAttribute,
            GetAttribute,
            RemoveAttribute,
            SetAttributeNS,
            GetAttributeNS,
            RemoveAttributeNS,
            HasAttribute,
            ComparePosition,
            AddListener,
            RemoveListener,
            DispatchEvent,
            Contains,
            GetElementById,
            BoundingRect,
            ClientRects,
            SceneGeometry,
            GetComputedStyle,
            EmitParticles,
            FormatInteger,
            EngineSend,
            EngineCall,
            EngineInvoke,
            EngineClose,
            Console,
            Storage,
            Focus,
            Focused,
            MatchMedia,
            Blur,
            HasFocus,
            ScrollIntoView,
            // The text field API (field_method).
            SetSelectionRange,
            SelectText,
            SetRangeText
        };

        static JSValue arg(int argc, JSValueConst* argv, int index) {
            return index < argc ? argv[index] : JS_UNDEFINED;
        }

        void event_property(JSValueConst event, const char* name, JSValue value) {
            if (JS_IsException(value)) {
                throw PendingJsException{};
            }
            if (JS_SetPropertyStr(context, event, name, value) < 0) {
                throw PendingJsException{};
            }
        }

        void listener_phase(Node& target, JSValueConst event, std::string_view type, bool capture, int phase) {
            event_property(event, "currentTarget", JS_DupValue(context, wrapper(target)));
            event_property(event, "eventPhase", JS_NewInt32(context, phase));
            // Listener callbacks may create nodes; the Binding stays in place.
            auto& listeners = binding(target).listeners;
            std::vector<std::uint64_t> snapshot;
            for (const Listener& l : listeners) {
                if (l.type == type && l.capture == capture) {
                    snapshot.push_back(l.id);
                }
            }
            for (std::uint64_t id : snapshot) {
                Value stop(context, JS_GetPropertyStr(context, event, "_immediateStopped"));
                if (JS_ToBool(context, stop.value)) {
                    break;
                }
                auto it = std::find_if(listeners.begin(), listeners.end(), [&](const Listener& l) {
                    return l.id == id;
                });
                if (it == listeners.end()) {
                    continue; // Removed listeners stay removed in a taken snapshot.
                }
                Value callback(context, JS_DupValue(context, it->callback));
                const bool handler = it->handler;
                const bool passive = it->passive;
                if (it->once) {
                    JS_FreeValue(context, it->callback);
                    listeners.erase(it);
                }
                JSValue args[] = {event};
                ++counters.listener_calls;
                Value method(context, JS_UNDEFINED);
                if (!JS_IsFunction(context, callback.value)) {
                    method.value = JS_GetPropertyStr(context, callback.value, "handleEvent");
                }
                Value previous_passive(context, JS_GetPropertyStr(context, event, "_passiveListener"));
                event_property(event, "_passiveListener", JS_NewBool(context, passive));
                JSValue raw_result =
                    JS_Call(context, JS_IsFunction(context, callback.value) ? callback.value : method.value,
                            JS_IsFunction(context, callback.value) ? wrapper(target) : callback.value, 1, args);
                event_property(event, "_passiveListener", JS_DupValue(context, previous_passive.value));
                if (JS_IsException(raw_result)) {
                    if (faulted) {
                        throw std::runtime_error("WEFT_SCRIPT_BUDGET: listener execution exceeded script deadline");
                    }
                    ++counters.listener_exceptions;
                    counters.last_listener_error =
                        exception_message().substr(0, constants::script::max_listener_error_bytes);
                } else {
                    Value result(context, raw_result);
                    if (handler && JS_IsBool(result.value) && !JS_ToBool(context, result.value)) {
                        Value cancelable(context, JS_GetPropertyStr(context, event, "cancelable"));
                        if (JS_ToBool(context, cancelable.value)) {
                            event_property(event, "defaultPrevented", JS_TRUE);
                        }
                    }
                }
            }
        }

        JSValue dispatch(Node& target, JSValueConst event) {
            Value type_value(context, JS_GetPropertyStr(context, event, "type"));
            const std::string type = string_value(context, type_value.value);
            Value active(context, JS_GetPropertyStr(context, event, "_dispatching"));
            if (JS_ToBool(context, active.value)) {
                throw std::runtime_error("WEFT_EVENT_ACTIVE: Event already dispatching");
            }
            std::vector<JSValue> path;
            path.reserve(constants::dom::max_depth);
            for (Node* p = &target; p; p = parent_of(*p)) {
                path.push_back(JS_DupValue(context, wrapper(*p)));
            }

            struct PathGuard {
                JSContext* c;
                std::vector<JSValue>& path;
                JSValue event;

                ~PathGuard() {
                    // Preserve a pending exception across best-effort metadata cleanup.
                    JSValue pending = JS_GetException(c);
                    JS_SetPropertyStr(c, event, "currentTarget", JS_NULL);
                    JS_SetPropertyStr(c, event, "eventPhase", JS_NewInt32(c, constants::script::event_phase_none));
                    JS_SetPropertyStr(c, event, "_dispatching", JS_FALSE);
                    JSValue cleanup_exception = JS_GetException(c);
                    JS_FreeValue(c, cleanup_exception);
                    JS_Throw(c, pending);
                    for (JSValue v : path) {
                        JS_FreeValue(c, v);
                    }
                }
            } guard{context, path, event};

            event_property(event, "_dispatching", JS_TRUE);
            event_property(event, "_stopped", JS_FALSE);
            event_property(event, "_immediateStopped", JS_FALSE);
            event_property(event, "target", JS_DupValue(context, wrapper(target)));
            auto stopped = [&] {
                Value value(context, JS_GetPropertyStr(context, event, "_stopped"));
                return JS_ToBool(context, value.value) != 0;
            };
            auto window_phase = [&](bool capture) {
                if (path.empty() || node(context, path.back()).type != constants::document_node_type) {
                    return;
                }
                Value global(context, JS_GetGlobalObject(context));
                Value callback(context, JS_GetPropertyStr(context, global.value, "__ceWindowEvent"));
                if (JS_IsFunction(context, callback.value)) {
                    JSValue args[]{event, JS_NewBool(context, capture)};
                    Value result(context, JS_Call(context, callback.value, global.value, 2, args));
                    if (JS_IsException(result.value)) {
                        throw PendingJsException{};
                    }
                }
            };
            window_phase(true);
            for (std::size_t i = path.size(); i-- > 1;) {
                if (stopped()) {
                    break;
                }
                listener_phase(node(context, path[i]), event, type, true, constants::script::event_phase_capturing);
                if (stopped()) {
                    break;
                }
            }
            if (!stopped()) {
                listener_phase(target, event, type, true, constants::script::event_phase_at_target);
                listener_phase(target, event, type, false, constants::script::event_phase_at_target);
            }
            Value bubbles(context, JS_GetPropertyStr(context, event, "bubbles"));
            if (JS_ToBool(context, bubbles.value) && !stopped()) {
                for (std::size_t i = 1; i < path.size(); ++i) {
                    listener_phase(node(context, path[i]), event, type, false, constants::script::event_phase_bubbling);
                    if (stopped()) {
                        break;
                    }
                }
            }
            if (JS_ToBool(context, bubbles.value) && !stopped()) {
                window_phase(false);
            }
            Value cancelled(context, JS_GetPropertyStr(context, event, "defaultPrevented"));
            return JS_NewBool(context, !JS_ToBool(context, cancelled.value));
        }

        static JSValue call_method(JSContext* c, JSValueConst self, int argc, JSValueConst* argv, int magic) {
            try {
                Impl& owner = state(c);
                Node& n = node(c, self);
                switch (magic) {
                    case SceneGeometry: {
                        const auto g = owner.geometry(n);
                        Value result(c, JS_NewObject(c));
                        for (const auto& [key, values] : std::vector<std::pair<const char*, std::vector<double>>>{
                                 {"matrix", {g.matrix.begin(), g.matrix.end()}},
                                 {"offset", {g.offset.begin(), g.offset.end()}}}) {
                            Value a(c, JS_NewArray(c));
                            for (std::uint32_t i = 0; i < values.size(); ++i) {
                                JS_SetPropertyUint32(c, a.value, i, JS_NewFloat64(c, values[i]));
                            }
                            JS_SetPropertyStr(c, result.value, key, JS_DupValue(c, a.value));
                        }
                        return JS_DupValue(c, result.value);
                    }
                    case GetComputedStyle: {
                        auto values = owner.computed_style(n);
                        Value result(c, JS_NewObject(c));
                        for (const auto& [key, value] : values) {
                            if (JS_SetPropertyStr(c, result.value, key.c_str(),
                                                  JS_NewStringLen(c, value.data(), value.size())) < 0) {
                                throw PendingJsException{};
                            }
                        }
                        return JS_DupValue(c, result.value);
                    }
                    case EmitParticles: {
                        // weft.emitParticles(element, name, options) of the scene platform: the name, a whole count of
                        // at least one, a duration in seconds, and a target element or a point (x, y).
                        if (!owner.particle_host) {
                            throw std::runtime_error("WEFT_PARTICLES: no particle host");
                        }
                        ParticleRequest request;
                        request.name = string_value(c, arg(argc, argv, 0));
                        double count{};
                        double duration{};
                        if (JS_ToFloat64(c, &count, arg(argc, argv, 1)) < 0 ||
                            JS_ToFloat64(c, &duration, arg(argc, argv, 2)) < 0) {
                            throw PendingJsException{};
                        }
                        if (!(count >= 1) || count != std::floor(count) ||
                            count > double(constants::max_particles_per_emission) || !std::isfinite(duration) ||
                            duration < 0) {
                            throw std::invalid_argument(
                                "WEFT_PARTICLES: a whole count of at least 1 and a duration of 0 or more");
                        }
                        request.count = std::uint32_t(count);
                        request.duration = duration;
                        if (node_unchecked(c, arg(argc, argv, 3))) {
                            request.target = node(c, arg(argc, argv, 3)).identity;
                        } else {
                            double x{};
                            double y{};
                            if (JS_ToFloat64(c, &x, arg(argc, argv, 4)) < 0 ||
                                JS_ToFloat64(c, &y, arg(argc, argv, 5)) < 0) {
                                throw PendingJsException{};
                            }
                            if (std::isfinite(x) && std::isfinite(y)) {
                                request.point = std::array<double, 2>{x, y};
                            }
                        }
                        owner.particle_host(n.identity, request);
                        return JS_UNDEFINED;
                    }
                    case EngineSend:
                    case EngineCall: {
                        // weft.engine.send(name, value) and call(name, value): a message for the host; a call's id.
                        if (!owner.engine) {
                            throw std::runtime_error("WEFT_ENGINE: no engine bridge");
                        }
                        auto name = string_value(c, arg(argc, argv, 0));
                        engine_name(name);
                        auto value = owner.engine_value(arg(argc, argv, 1));
                        if (owner.engine_messages.size() >= constants::bridge::max_pending_messages ||
                            owner.engine_message_bytes + value.size() > constants::bridge::max_pending_message_bytes) {
                            throw std::runtime_error("WEFT_ENGINE_FULL: the host has not taken the page's messages");
                        }
                        std::uint64_t call{};
                        if (magic == EngineCall) {
                            call = owner.engine->open_call();
                            if (!call) {
                                throw std::runtime_error("WEFT_ENGINE_FULL: too many open calls");
                            }
                        }
                        owner.engine_message_bytes += value.size();
                        owner.engine_messages.push_back({call, std::move(name), std::move(value)});
                        return magic == EngineCall ? JS_NewFloat64(c, double(call)) : JS_UNDEFINED;
                    }
                    case EngineInvoke: {
                        // weft.engine.invoke(name, value): the host's function, at once.
                        auto name = string_value(c, arg(argc, argv, 0));
                        const auto found = owner.engine_functions.find(name);
                        if (found == owner.engine_functions.end()) {
                            throw std::runtime_error("WEFT_ENGINE: no function " + name);
                        }
                        const auto value = owner.engine_value(arg(argc, argv, 1));
                        std::vector<std::uint8_t> result;
                        {
                            owner.engine_running = true;

                            struct Done {
                                Impl& owner;

                                ~Done() {
                                    owner.engine_running = false;
                                }
                            } done{owner};

                            result = found->second(value);
                        }
                        // Nothing is undefined.
                        std::string why;
                        if (!result.empty() && !bridge::valid_value(result, &why)) {
                            throw std::runtime_error("WEFT_ENGINE: " + name + " returned " + why);
                        }
                        return owner.js_value(result);
                    }
                    case EngineClose: {
                        // A call the page settled itself (its timeout): later completions are stale.
                        double call{};
                        if (JS_ToFloat64(c, &call, arg(argc, argv, 0)) < 0) {
                            throw PendingJsException{};
                        }
                        if (owner.engine) {
                            owner.engine->close_call(std::uint64_t(call));
                        }
                        return JS_UNDEFINED;
                    }
                    case FormatInteger: {
                        double value{};
                        if (JS_ToFloat64(c, &value, arg(argc, argv, 0)) < 0) {
                            throw PendingJsException{};
                        }
                        if (!owner.integer_format_host || !std::isfinite(value) || std::floor(value) != value ||
                            std::abs(value) > constants::script::max_safe_integer) {
                            throw std::runtime_error(
                                "WEFT_NUMBER_FORMAT_PROFILE: safe integer and host formatter required");
                        }
                        owner.layout_running = true;

                        struct Guard {
                            Impl& owner;

                            ~Guard() {
                                owner.layout_running = false;
                            }
                        } guard{owner};

                        auto result = owner.integer_format_host(std::int64_t(value));
                        if (result.size() > constants::script::max_formatted_integer_bytes) {
                            throw std::runtime_error("WEFT_NUMBER_FORMAT_LIMIT");
                        }
                        return JS_NewStringLen(c, result.data(), result.size());
                    }
                    case Console: {
                        // The web platform's console: a message at a level (0 log, 1 warning, 2 error), cut to a
                        // bound; nothing without a host.
                        double level{};
                        if (JS_ToFloat64(c, &level, arg(argc, argv, 0)) < 0) {
                            throw PendingJsException{};
                        }
                        auto text = string_value(c, arg(argc, argv, 1));
                        if (text.size() > std::size_t(constants::script::max_console_message_bytes)) {
                            text.resize(std::size_t(constants::script::max_console_message_bytes));
                            text += "...";
                        }
                        if (owner.console_host) {
                            owner.console_host(level >= 2   ? ConsoleLevel::Error
                                               : level >= 1 ? ConsoleLevel::Warning
                                                            : ConsoleLevel::Log,
                                               text);
                        }
                        return JS_UNDEFINED;
                    }
                    case Storage:
                        return owner.storage_call(argc, argv);
                    case Focus: {
                        // focus(options): an element that can take the focus takes it and scrolls into view where it
                        // does not show, unless options.preventScroll; options.focusVisible says whether it shows the
                        // focus (Chrome's FocusOptions).
                        bool prevent_scroll{};
                        std::optional<bool> visible;
                        if (JSValueConst options = arg(argc, argv, 0); JS_IsObject(options)) {
                            Value prevent(c, JS_GetPropertyStr(c, options, "preventScroll"));
                            if (JS_IsException(prevent.value)) {
                                throw PendingJsException{};
                            }
                            Value shown(c, JS_GetPropertyStr(c, options, "focusVisible"));
                            if (JS_IsException(shown.value)) {
                                throw PendingJsException{};
                            }
                            prevent_scroll = JS_ToBool(c, prevent.value) > 0;
                            if (!JS_IsUndefined(shown.value)) {
                                visible = JS_ToBool(c, shown.value) > 0;
                            }
                        }
                        if (owner.focusable(n)) {
                            owner.move_focus(&n, FocusType::Script, !prevent_scroll, visible);
                        }
                        return JS_UNDEFINED;
                    }
                    case Blur:
                        if (owner.focused_node() == &n) {
                            owner.move_focus(nullptr, FocusType::Script, false);
                        }
                        return JS_UNDEFINED;
                    case HasFocus:
                        // document.hasFocus(): whether the host's keyboard focus is in the page.
                        return JS_NewBool(c, n.type == constants::document_node_type && owner.page_focus);
                    case ScrollIntoView: {
                        // scrollIntoView(alignToTop) and scrollIntoView({block, inline}): block start by default (end
                        // for false), inline nearest.
                        auto block = ScrollAlign::Start;
                        auto inline_align = ScrollAlign::Nearest;
                        const auto align = [&](JSValueConst options, const char* key, ScrollAlign& out) {
                            Value v(c, JS_GetPropertyStr(c, options, key));
                            if (JS_IsException(v.value)) {
                                throw PendingJsException{};
                            }
                            if (JS_IsUndefined(v.value)) {
                                return;
                            }
                            const auto text = string_value(c, v.value);
                            if (text == "start") {
                                out = ScrollAlign::Start;
                            } else if (text == "center") {
                                out = ScrollAlign::Center;
                            } else if (text == "end") {
                                out = ScrollAlign::End;
                            } else if (text == "nearest") {
                                out = ScrollAlign::Nearest;
                            } else {
                                JS_ThrowTypeError(c, "scrollIntoView: '%s' is not a ScrollLogicalPosition", text.c_str());
                                throw PendingJsException{};
                            }
                        };
                        if (JSValueConst options = arg(argc, argv, 0); JS_IsObject(options)) {
                            align(options, "block", block);
                            align(options, "inline", inline_align);
                        } else if (!JS_IsUndefined(options) && JS_ToBool(c, options) == 0) {
                            block = ScrollAlign::End;
                        }
                        if (n.type == constants::element_node_type && owner.connected(n)) {
                            owner.scroll_into_view(n, block, inline_align);
                        }
                        return JS_UNDEFINED;
                    }
                    case Focused: {
                        auto* f = owner.focused_node();
                        return f ? JS_DupValue(c, owner.wrapper(*f)) : JS_NULL;
                    }
                    case MatchMedia: {
                        if (!owner.media_host) {
                            throw std::runtime_error("WEFT_MEDIA: no media host");
                        }
                        return JS_NewBool(c, owner.media_host(string_value(c, arg(argc, argv, 0))));
                    }
                    case BoundingRect:
                    case ClientRects: {
                        auto g = owner.geometry(n);
                        auto rect = [&](const std::array<double, 4>& r) {
                            Value o(c, JS_NewObject(c));
                            for (const auto& [key, v] :
                                 std::vector<std::pair<const char*, double>>{{"x", r[0]},
                                                                             {"y", r[1]},
                                                                             {"width", r[2]},
                                                                             {"height", r[3]},
                                                                             {"left", r[0]},
                                                                             {"top", r[1]},
                                                                             {"right", r[0] + r[2]},
                                                                             {"bottom", r[1] + r[3]}}) {
                                if (JS_SetPropertyStr(c, o.value, key, JS_NewFloat64(c, v)) < 0) {
                                    throw PendingJsException{};
                                }
                            }
                            return JS_DupValue(c, o.value);
                        };
                        if (magic == BoundingRect) {
                            return rect(g.rect);
                        }
                        Value a(c, JS_NewArray(c));
                        for (std::uint32_t i = 0; i < g.client_rects.size(); ++i) {
                            if (JS_SetPropertyUint32(c, a.value, i, rect(g.client_rects[i])) < 0) {
                                throw PendingJsException{};
                            }
                        }
                        return JS_DupValue(c, a.value);
                    }
                    case Append:
                    case InsertBefore: {
                        Node& child = node(c, arg(argc, argv, 0));
                        Node* before{};
                        JSValue reference = arg(argc, argv, 1);
                        if (magic == InsertBefore && !JS_IsNull(reference) && !JS_IsUndefined(reference)) {
                            before = &node(c, reference);
                        }
                        Value hold(c, JS_DupValue(c, owner.wrapper(child)));
                        Value hold_before(c, before ? JS_DupValue(c, owner.wrapper(*before)) : JS_UNDEFINED);
                        // A focused element that moves leaves its place first, and the focus with it.
                        if (child.parent.generation) {
                            owner.blur_within(child);
                        }
                        if (before && before->parent != n.identity) {
                            throw std::runtime_error("WEFT_NOT_FOUND: reference is not a child");
                        }
                        owner.insert(n, child, before);
                        return JS_DupValue(c, owner.wrapper(child));
                    }
                    case Remove: {
                        Node& child = node(c, arg(argc, argv, 0));
                        if (child.parent != n.identity) {
                            throw std::runtime_error("WEFT_NOT_FOUND: child is not in parent");
                        }
                        Value hold(c, JS_DupValue(c, owner.wrapper(child)));
                        owner.blur_within(child);
                        // The blur's listeners may have moved it already.
                        if (child.parent != n.identity) {
                            throw std::runtime_error("WEFT_NOT_FOUND: child is not in parent");
                        }
                        owner.detach(child);
                        return JS_DupValue(c, hold.value);
                    }
                    case CreateElement:
                    case CreateElementNS:
                    case CreateTextNode: {
                        if (n.type != constants::document_node_type) {
                            throw std::runtime_error("WEFT_DOM_TYPE: creation requires Document");
                        }
                        if (magic == CreateTextNode) {
                            Value object(c, owner.make_node(constants::text_node_type, "#text", "", n.identity));
                            node(c, object.value).data = string_value(c, arg(argc, argv, 0));
                            return JS_DupValue(c, object.value);
                        }
                        std::string space =
                            magic == CreateElement
                                ? constants::xhtml_namespace_uri
                                : (JS_IsNull(arg(argc, argv, 0)) ? "" : string_value(c, arg(argc, argv, 0)));
                        std::string local = string_value(c, arg(argc, argv, magic == CreateElement ? 0 : 1));
                        if (local.empty() || local.find_first_of(" <>\t\r\n") != std::string::npos) {
                            throw std::runtime_error("WEFT_DOM_NAME: invalid element name");
                        }
                        if (magic == CreateElementNS && local.find(':') != std::string::npos) {
                            throw std::runtime_error(
                                "WEFT_UNSUPPORTED_DOM: prefixed element qualified names are outside the M0 profile");
                        }
                        if (space == constants::xhtml_namespace_uri) {
                            local = lower_ascii(local);
                        }
                        return owner.make_node(constants::element_node_type, std::move(local), std::move(space),
                                               n.identity);
                    }
                    case QuerySelector:
                    case QuerySelectorAll: {
                        const std::string selector = string_value(c, arg(argc, argv, 0));
                        std::vector<Node*> results;
                        owner.query(n, selector, results, magic == QuerySelector);
                        if (magic == QuerySelector) {
                            return results.empty() ? JS_NULL : JS_DupValue(c, owner.wrapper(*results.front()));
                        }
                        Value array(c, JS_NewArray(c));
                        for (std::uint32_t i{}; i < results.size(); ++i) {
                            if (JS_SetPropertyUint32(c, array.value, i, JS_DupValue(c, owner.wrapper(*results[i]))) <
                                0) {
                                return JS_EXCEPTION;
                            }
                        }
                        return JS_DupValue(c, array.value);
                    }
                    case GetElementById: {
                        if (n.type != constants::document_node_type) {
                            throw std::runtime_error("WEFT_DOM_TYPE: getElementById requires Document");
                        }
                        const auto id = string_value(c, arg(argc, argv, 0));
                        if (id.empty()) {
                            return JS_NULL;
                        }
                        std::vector<Node*> elements;
                        owner.query(n, "*", elements, false);
                        for (const auto* element : elements) {
                            for (const auto& a : element->attributes) {
                                if (a.space.empty() && a.name == "id" && a.value == id) {
                                    return JS_DupValue(c, owner.wrapper(*element));
                                }
                            }
                        }
                        return JS_NULL;
                    }
                    case Matches:
                        return JS_NewBool(c, owner.matches(n, string_value(c, arg(argc, argv, 0))));
                    case Closest: {
                        // The element itself or its nearest ancestor element the selector list matches; the list is
                        // compiled once, so an unsupported selector fails even when nothing would match.
                        if (n.type != constants::element_node_type) {
                            throw std::runtime_error("WEFT_DOM_TYPE: closest requires Element");
                        }
                        const auto list = owner.selectors(string_value(c, arg(argc, argv, 0)));
                        for (const Node* e = &n; e && e->type == constants::element_node_type;
                             e = owner.parent_of(*e)) {
                            if (owner.tree.matches(*e, list)) {
                                return JS_DupValue(c, owner.wrapper(*e));
                            }
                        }
                        return JS_NULL;
                    }
                    case SetAttribute:
                    case GetAttribute:
                    case RemoveAttribute:
                    case SetAttributeNS:
                    case GetAttributeNS:
                    case RemoveAttributeNS:
                    case HasAttribute: {
                        if (n.type != constants::element_node_type) {
                            throw std::runtime_error("WEFT_DOM_TYPE: attributes require Element");
                        }
                        const bool ns =
                            magic == SetAttributeNS || magic == GetAttributeNS || magic == RemoveAttributeNS;
                        const std::string space =
                            ns && !JS_IsNull(arg(argc, argv, 0)) ? string_value(c, arg(argc, argv, 0)) : "";
                        std::string name = string_value(c, arg(argc, argv, ns ? 1 : 0));
                        if (!ns && n.space == constants::xhtml_namespace_uri) {
                            name = lower_ascii(name);
                        }
                        const auto colon = name.find(':');
                        const std::string local = colon == std::string::npos ? name : name.substr(colon + 1);
                        // WebIDL conversion can run arbitrary JS. Never keep a container
                        // iterator across value.toString(), which may reenter setAttribute.
                        const bool setting = magic == SetAttribute || magic == SetAttributeNS;
                        std::string converted_value;
                        if (setting) {
                            converted_value = string_value(c, arg(argc, argv, ns ? 2 : 1));
                        }
                        auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const Attribute& a) {
                            return ns ? a.space == space && a.local == (magic == SetAttributeNS ? local : name)
                                      : a.name == name;
                        });
                        if (setting) {
                            const bool modified =
                                it == n.attributes.end() || it->name != name || it->value != converted_value;
                            if (space.empty()) {
                                owner.resize_canvas_attribute(n, name, converted_value);
                            }
                            if (it == n.attributes.end()) {
                                n.attributes.push_back({name, local, space, std::move(converted_value)});
                            } else {
                                it->name = std::move(name);
                                it->value = std::move(converted_value);
                            }
                            if (modified) {
                                owner.changed(n);
                                owner.sync_field(n);
                            }
                            return JS_UNDEFINED;
                        }
                        if (magic == RemoveAttribute || magic == RemoveAttributeNS) {
                            if (it != n.attributes.end() && it->space.empty()) {
                                owner.resize_canvas_attribute(n, it->name, "");
                            }
                            if (it != n.attributes.end()) {
                                n.attributes.erase(it);
                                owner.changed(n);
                                owner.sync_field(n);
                            }
                            return JS_UNDEFINED;
                        }
                        if (magic == HasAttribute) {
                            return JS_NewBool(c, it != n.attributes.end());
                        }
                        return it == n.attributes.end() ? JS_NULL
                                                        : JS_NewStringLen(c, it->value.data(), it->value.size());
                    }
                    case Contains: {
                        Node* other = node_unchecked(c, arg(argc, argv, 0));
                        while (other) {
                            if (other == &n) {
                                return JS_TRUE;
                            }
                            other = owner.parent_of(*other);
                        }
                        return JS_FALSE;
                    }
                    case ComparePosition: {
                        Node& other = node(c, arg(argc, argv, 0));
                        if (&n == &other) {
                            return JS_NewInt32(c, 0);
                        }
                        std::vector<Node*> left;
                        std::vector<Node*> right;
                        for (Node* p = &n; p; p = owner.parent_of(*p)) {
                            left.push_back(p);
                        }
                        for (Node* p = &other; p; p = owner.parent_of(*p)) {
                            right.push_back(p);
                        }
                        if (left.back() != right.back()) {
                            return JS_NewInt32(c, constants::script::document_position_disconnected |
                                                      constants::script::document_position_implementation_specific |
                                                      (n.identity.slot < other.identity.slot
                                                           ? constants::script::document_position_following
                                                           : constants::script::document_position_preceding));
                        }
                        std::reverse(left.begin(), left.end());
                        std::reverse(right.begin(), right.end());
                        std::size_t i{};
                        while (i < left.size() && i < right.size() && left[i] == right[i]) {
                            ++i;
                        }
                        if (i == left.size()) {
                            return JS_NewInt32(c, constants::script::document_position_following |
                                                      constants::script::document_position_contained_by);
                        }
                        if (i == right.size()) {
                            return JS_NewInt32(c, constants::script::document_position_preceding |
                                                      constants::script::document_position_contains);
                        }
                        for (const auto& id : left[i - 1]->children) {
                            Node* child = &owner.node_at(id);
                            if (child == left[i]) {
                                return JS_NewInt32(c, constants::script::document_position_following);
                            }
                            if (child == right[i]) {
                                return JS_NewInt32(c, constants::script::document_position_preceding);
                            }
                        }
                        throw std::runtime_error("WEFT_TREE_CORRUPTION: disconnected order");
                    }
                    case AddListener:
                    case RemoveListener: {
                        std::string type = string_value(c, arg(argc, argv, 0));
                        JSValue callback = arg(argc, argv, 1);
                        if (JS_IsNull(callback) || JS_IsUndefined(callback)) {
                            return JS_UNDEFINED;
                        }
                        if (!JS_IsObject(callback)) {
                            return JS_UNDEFINED;
                        }
                        JSValue options = arg(argc, argv, 2);
                        bool capture{};
                        bool once{};
                        bool passive{};
                        if (JS_IsObject(options)) {
                            Value cv(c, JS_GetPropertyStr(c, options, "capture"));
                            capture = JS_ToBool(c, cv.value) != 0;
                            Value ov(c, JS_GetPropertyStr(c, options, "once"));
                            once = JS_ToBool(c, ov.value) != 0;
                            Value pv(c, JS_GetPropertyStr(c, options, "passive"));
                            passive = JS_ToBool(c, pv.value) != 0;
                        } else {
                            capture = JS_ToBool(c, options) != 0;
                        }
                        auto& listeners = owner.binding(n).listeners;
                        auto it = std::find_if(listeners.begin(), listeners.end(), [&](const Listener& l) {
                            return l.type == type && l.capture == capture && equal_value(l.callback, callback);
                        });
                        if (magic == AddListener && it == listeners.end()) {
                            if (owner.next_listener == std::numeric_limits<std::uint64_t>::max()) {
                                throw std::runtime_error("WEFT_LISTENER_EXHAUSTED: listener token cannot wrap");
                            }
                            listeners.reserve(listeners.size() + 1);
                            if (listeners.size() >= constants::script::max_listeners_per_node) {
                                throw std::runtime_error("WEFT_LISTENER_LIMIT");
                            }
                            listeners.push_back({owner.next_listener++, std::move(type), JS_DupValue(c, callback),
                                                 capture, once, false, passive});
                        } else if (magic == RemoveListener && it != listeners.end()) {
                            JS_FreeValue(c, it->callback);
                            listeners.erase(it);
                        }
                        return JS_UNDEFINED;
                    }
                    case DispatchEvent:
                        return owner.dispatch(n, arg(argc, argv, 0));
                    case SetSelectionRange:
                    case SelectText:
                    case SetRangeText:
                        return owner.field_method(n, magic, argc, argv);
                    default:
                        return JS_UNDEFINED;
                }
            } catch (const PendingJsException&) {
                return JS_EXCEPTION;
            } catch (const std::bad_alloc&) {
                state(c).faulted = true;
                return JS_ThrowOutOfMemory(c);
            } catch (const std::exception& e) {
                return JS_ThrowInternalError(c, "%s", e.what());
            } catch (...) {
                state(c).faulted = true;
                return JS_ThrowInternalError(c, "WEFT_NATIVE_EXCEPTION: unknown native method exception");
            }
        }

        void property(JSValueConst prototype, const char* name, int magic, bool writable = false) {
            // QuickJS keeps the pointer in a union and calls it with the getter or setter signature that the cproto
            // names; the cast through void(*)() states that round trip.
            using Generic = void (*)();
            JSAtom atom = JS_NewAtom(context, name);
            JSValue get =
                JS_NewCFunction2(context, reinterpret_cast<JSCFunction*>(reinterpret_cast<Generic>(get_property)), name,
                                 0, JS_CFUNC_getter_magic, magic);
            JSValue set =
                writable
                    ? JS_NewCFunction2(context, reinterpret_cast<JSCFunction*>(reinterpret_cast<Generic>(set_property)),
                                       name, 1, JS_CFUNC_setter_magic, magic)
                    : JS_UNDEFINED;
            const int result = JS_DefinePropertyGetSet(context, prototype, atom, get, set, JS_PROP_CONFIGURABLE);
            JS_FreeAtom(context, atom);
            if (result < 0) {
                throw std::runtime_error("prototype property creation failed");
            }
        }

        void method(JSValueConst prototype, const char* name, int magic, int argc) {
            if (JS_SetPropertyStr(
                    context, prototype, name,
                    JS_NewCFunctionMagic(context, call_method, name, argc, JS_CFUNC_generic_magic, magic)) < 0) {
                throw std::runtime_error("prototype method creation failed");
            }
        }

        void initialize() {
            static const JSMallocFunctions allocator{calloc_allocation, allocate, free_allocation, realloc_allocation,
                                                     allocation_size};
            runtime = JS_NewRuntime2(&allocator, this);
            if (!runtime) {
                throw std::runtime_error("QuickJS runtime allocation failed");
            }
            JS_SetRuntimeOpaque(runtime, this);
            JS_SetInterruptHandler(runtime, interrupt, this);
            // The host must reserve native stack headroom above this VM limit.
            // Default probes use 512 KiB; React probes reserve an 8 MiB host stack.
            JS_SetMaxStackSize(runtime, config.max_stack_bytes);
            context = JS_NewContext(runtime);
            if (!context) {
                JS_FreeRuntime(runtime);
                runtime = nullptr;
                throw std::runtime_error("QuickJS context allocation failed");
            }
            JS_SetContextOpaque(context, this);
            tree.reset(view_epoch);
            try {
                JS_NewClassID(runtime, &node_class);
                static JSClassExoticMethods exotic = [] {
                    JSClassExoticMethods e{};
                    e.get_own_property = own_property;
                    e.get_own_property_names = own_names;
                    e.delete_property = delete_property;
                    e.define_own_property = define_property;
                    return e;
                }();
                JSClassDef definition{};
                definition.class_name = "CaseElephantNativeNode";
                definition.finalizer = finalize;
                definition.gc_mark = mark;
                definition.exotic = &exotic;
                if (JS_NewClass(runtime, node_class, &definition) < 0) {
                    throw std::runtime_error("node class registration failed");
                }
                Value prototype(context, JS_NewObject(context));
                property(prototype.value, "nodeType", Type);
                property(prototype.value, "nodeName", Name);
                property(prototype.value, "id", ElementId, true);
                property(prototype.value, "tagName", Name);
                property(prototype.value, "localName", LocalName);
                property(prototype.value, "namespaceURI", Namespace);
                property(prototype.value, "ownerDocument", OwnerDocument);
                property(prototype.value, "parentNode", Parent);
                property(prototype.value, "childNodes", Children);
                property(prototype.value, "firstChild", FirstChild);
                property(prototype.value, "lastChild", LastChild);
                property(prototype.value, "nextSibling", NextSibling);
                property(prototype.value, "previousSibling", PreviousSibling);
                property(prototype.value, "textContent", TextContent, true);
                property(prototype.value, "nodeValue", NodeValue, true);
                property(prototype.value, "data", Data, true);
                property(prototype.value, "documentElement", DocumentElement);
                property(prototype.value, "body", Body);
                property(prototype.value, "defaultView", DefaultView);
                property(prototype.value, "isConnected", IsConnected);
                method(prototype.value, "appendChild", Append, 1);
                method(prototype.value, "insertBefore", InsertBefore, 2);
                method(prototype.value, "removeChild", Remove, 1);
                method(prototype.value, "createElement", CreateElement, 1);
                method(prototype.value, "createElementNS", CreateElementNS, 2);
                method(prototype.value, "createTextNode", CreateTextNode, 1);
                method(prototype.value, "querySelector", QuerySelector, 1);
                method(prototype.value, "querySelectorAll", QuerySelectorAll, 1);
                method(prototype.value, "getElementById", GetElementById, 1);
                method(prototype.value, "matches", Matches, 1);
                method(prototype.value, "closest", Closest, 1);
                method(prototype.value, "contains", Contains, 1);
                method(prototype.value, "setAttribute", SetAttribute, 2);
                method(prototype.value, "getAttribute", GetAttribute, 1);
                method(prototype.value, "removeAttribute", RemoveAttribute, 1);
                method(prototype.value, "hasAttribute", HasAttribute, 1);
                method(prototype.value, "setAttributeNS", SetAttributeNS, 3);
                method(prototype.value, "getAttributeNS", GetAttributeNS, 2);
                method(prototype.value, "removeAttributeNS", RemoveAttributeNS, 2);
                method(prototype.value, "compareDocumentPosition", ComparePosition, 1);
                method(prototype.value, "addEventListener", AddListener, 3);
                method(prototype.value, "removeEventListener", RemoveListener, 3);
                method(prototype.value, "dispatchEvent", DispatchEvent, 1);
                method(prototype.value, "getBoundingClientRect", BoundingRect, 0);
                method(prototype.value, "getClientRects", ClientRects, 0);
                property(prototype.value, "clientWidth", ClientWidth);
                property(prototype.value, "clientHeight", ClientHeight);
                property(prototype.value, "scrollWidth", ScrollWidth);
                property(prototype.value, "scrollHeight", ScrollHeight);
                property(prototype.value, "scrollLeft", ScrollLeft, true);
                property(prototype.value, "scrollTop", ScrollTop, true);
                property(prototype.value, "offsetLeft", OffsetLeft);
                property(prototype.value, "offsetTop", OffsetTop);
                property(prototype.value, "offsetWidth", OffsetWidth);
                property(prototype.value, "offsetHeight", OffsetHeight);
                property(prototype.value, "offsetParent", OffsetParent);
                method(prototype.value, "__ceGeometry", SceneGeometry, 0);
                method(prototype.value, "__ceComputedStyle", GetComputedStyle, 0);
                method(prototype.value, "__ceEmitParticles", EmitParticles, 6);
                method(prototype.value, "__ceFormatInteger", FormatInteger, 1);
                method(prototype.value, "__ceEngineSend", EngineSend, 2);
                method(prototype.value, "__ceEngineCall", EngineCall, 2);
                method(prototype.value, "__ceEngineInvoke", EngineInvoke, 2);
                method(prototype.value, "__ceEngineClose", EngineClose, 1);
                method(prototype.value, "__ceConsole", Console, 2);
                method(prototype.value, "__ceStorage", Storage, 4);
                method(prototype.value, "__ceFocused", Focused, 0);
                method(prototype.value, "__ceMatchMedia", MatchMedia, 1);
                method(prototype.value, "focus", Focus, 1);
                method(prototype.value, "blur", Blur, 0);
                method(prototype.value, "hasFocus", HasFocus, 0);
                method(prototype.value, "scrollIntoView", ScrollIntoView, 1);
                property(prototype.value, "activeElement", ActiveElement);
                property(prototype.value, "tabIndex", TabIndex, true);
                JS_SetClassProto(context, node_class, JS_DupValue(context, prototype.value));
                field_prototype = JS_NewObjectProto(context, prototype.value);
                if (JS_IsException(field_prototype)) {
                    field_prototype = JS_UNDEFINED;
                    throw PendingJsException{};
                }
                property(field_prototype, "value", FieldValue, true);
                property(field_prototype, "defaultValue", DefaultValue, true);
                property(field_prototype, "selectionStart", SelectionStart, true);
                property(field_prototype, "selectionEnd", SelectionEnd, true);
                property(field_prototype, "selectionDirection", SelectionDirection, true);
                property(field_prototype, "type", InputType, true);
                property(field_prototype, "placeholder", Placeholder, true);
                property(field_prototype, "readOnly", ReadOnly, true);
                property(field_prototype, "disabled", Disabled, true);
                property(field_prototype, "maxLength", MaxLength, true);
                property(field_prototype, "name", FieldName, true);
                property(field_prototype, "textLength", TextLength);
                method(field_prototype, "setSelectionRange", SetSelectionRange, 3);
                method(field_prototype, "select", SelectText, 0);
                method(field_prototype, "setRangeText", SetRangeText, 4);

                document_root = make_node(constants::document_node_type, "#document", "", {});
                const auto document = node(context, document_root).identity;
                Value html(context,
                           make_node(constants::element_node_type, "html", constants::xhtml_namespace_uri, document));
                Value body(context,
                           make_node(constants::element_node_type, "body", constants::xhtml_namespace_uri, document));
                insert(node(context, document_root), node(context, html.value), nullptr);
                insert(node(context, html.value), node(context, body.value), nullptr);
                Value global(context, JS_GetGlobalObject(context));
                JS_SetPropertyStr(context, global.value, "document", JS_DupValue(context, document_root));
                JS_SetPropertyStr(context, global.value, "window", JS_DupValue(context, global.value));
                eval(R"JS(
globalThis.Event = class Event {
  constructor(type, options = {}) {
    this.type = String(type); this.bubbles = !!options.bubbles;
    this.cancelable = !!options.cancelable; this.defaultPrevented = false;
    this.target = null; this.currentTarget = null; this.eventPhase = 0;
  }
  preventDefault() { if (this.cancelable && !this._passiveListener) this.defaultPrevented = true; }
  stopPropagation() { this._stopped = true; }
  stopImmediatePropagation() { this._stopped = this._immediateStopped = true; }
};
globalThis.CustomEvent = class CustomEvent extends Event {
  constructor(type, options = {}) { super(type, options); this.detail = options.detail ?? null; }
};
globalThis.UIEvent = class UIEvent extends Event {
  constructor(type, options = {}) { super(type, options); this.view = options.view ?? null; this.detail = options.detail ?? 0; }
};
globalThis.MouseEvent = class MouseEvent extends UIEvent {
  constructor(type, options = {}) {
    super(type, options);
    for (const key of ['clientX','clientY','screenX','screenY','button','buttons']) this[key] = Number(options[key] ?? 0);
    for (const key of ['ctrlKey','shiftKey','altKey','metaKey']) this[key] = !!options[key];
    this.relatedTarget = options.relatedTarget ?? null;
    Object.defineProperty(this, 'isTrusted', { value: false });
  }
};
globalThis.WheelEvent = class WheelEvent extends MouseEvent {
  constructor(type, options = {}) { super(type, options); for (const key of ['deltaX','deltaY','deltaZ','deltaMode']) this[key] = Number(options[key] ?? 0); }
};
globalThis.FocusEvent = class FocusEvent extends UIEvent {
  constructor(type, options = {}) {
    super(type, options);
    this.relatedTarget = options.relatedTarget ?? null;
    Object.defineProperty(this, 'isTrusted', { value: false });
  }
};
globalThis.KeyboardEvent = class KeyboardEvent extends UIEvent {
  constructor(type, options = {}) {
    super(type, options);
    this.key = String(options.key ?? ''); this.code = String(options.code ?? '');
    this.location = Number(options.location ?? 0); this.repeat = !!options.repeat; this.isComposing = !!options.isComposing;
    for (const key of ['ctrlKey','shiftKey','altKey','metaKey']) this[key] = !!options[key];
    // As in browsers, a page's own events keep the legacy codes it gives (0 otherwise).
    this.keyCode = Number(options.keyCode ?? 0); this.charCode = Number(options.charCode ?? 0);
    this.which = Number(options.which ?? this.keyCode);
    Object.defineProperty(this, '__modifiers', { value: (this.shiftKey ? 1 : 0) | (this.ctrlKey ? 2 : 0) |
      (this.altKey ? 4 : 0) | (this.metaKey ? 8 : 0) | (options.modifierAltGraph ? 16 : 0) |
      (options.modifierCapsLock ? 32 : 0) | (options.modifierNumLock ? 64 : 0) });
    Object.defineProperty(this, 'isTrusted', { value: false });
  }
  getModifierState(key) {
    const bit = { Shift: 1, Control: 2, Alt: 4, Meta: 8, AltGraph: 16, CapsLock: 32, NumLock: 64 }[key];
    return bit ? (this.__modifiers & bit) !== 0 : false;
  }
};
Object.assign(KeyboardEvent, { DOM_KEY_LOCATION_STANDARD: 0, DOM_KEY_LOCATION_LEFT: 1, DOM_KEY_LOCATION_RIGHT: 2,
  DOM_KEY_LOCATION_NUMPAD: 3 });
globalThis.InputEvent = class InputEvent extends UIEvent {
  constructor(type, options = {}) {
    super(type, options);
    this.data = options.data ?? null; this.inputType = String(options.inputType ?? '');
    this.isComposing = !!options.isComposing; this.dataTransfer = null;
    Object.defineProperty(this, 'isTrusted', { value: false });
  }
  getTargetRanges() { return []; }
};
globalThis.CompositionEvent = class CompositionEvent extends UIEvent {
  constructor(type, options = {}) {
    super(type, options);
    this.data = String(options.data ?? '');
    Object.defineProperty(this, 'isTrusted', { value: false });
  }
};
)JS",
                     "<event-probe-bootstrap>");
                Value mouse(context, JS_GetPropertyStr(context, global.value, "MouseEvent"));
                mouse_prototype = JS_GetPropertyStr(context, mouse.value, "prototype");
                if (JS_IsException(mouse_prototype)) {
                    mouse_prototype = JS_UNDEFINED;
                    throw PendingJsException{};
                }
                Value wheel(context, JS_GetPropertyStr(context, global.value, "WheelEvent"));
                wheel_prototype = JS_GetPropertyStr(context, wheel.value, "prototype");
                Value event(context, JS_GetPropertyStr(context, global.value, "Event"));
                event_prototype = JS_GetPropertyStr(context, event.value, "prototype");
                Value keyboard(context, JS_GetPropertyStr(context, global.value, "KeyboardEvent"));
                keyboard_prototype = JS_GetPropertyStr(context, keyboard.value, "prototype");
                Value focus_event(context, JS_GetPropertyStr(context, global.value, "FocusEvent"));
                focus_prototype = JS_GetPropertyStr(context, focus_event.value, "prototype");
                Value input_event(context, JS_GetPropertyStr(context, global.value, "InputEvent"));
                input_prototype = JS_GetPropertyStr(context, input_event.value, "prototype");
                Value composition_event(context, JS_GetPropertyStr(context, global.value, "CompositionEvent"));
                composition_prototype = JS_GetPropertyStr(context, composition_event.value, "prototype");
                if (JS_IsException(wheel_prototype) || JS_IsException(event_prototype) ||
                    JS_IsException(keyboard_prototype) || JS_IsException(focus_prototype) ||
                    JS_IsException(input_prototype) || JS_IsException(composition_prototype)) {
                    throw PendingJsException{};
                }
            } catch (...) {
                shutdown();
                throw;
            }
        }

        void shutdown() noexcept {
            if (!runtime) {
                return;
            }
            for (auto& [token, value] : native_roots) {
                (void)token;
                JS_FreeValue(context, value);
            }
            native_roots.clear();
            hovered_nodes.clear();
            active_nodes.clear();
            for (auto& binding : host_bindings) {
                JS_FreeValue(context, binding.function);
                JS_FreeValue(context, binding.object);
                binding = {};
            }
            JS_FreeValue(context, mouse_prototype);
            mouse_prototype = JS_UNDEFINED;
            JS_FreeValue(context, wheel_prototype);
            wheel_prototype = JS_UNDEFINED;
            JS_FreeValue(context, event_prototype);
            event_prototype = JS_UNDEFINED;
            JS_FreeValue(context, keyboard_prototype);
            keyboard_prototype = JS_UNDEFINED;
            JS_FreeValue(context, focus_prototype);
            focus_prototype = JS_UNDEFINED;
            JS_FreeValue(context, input_prototype);
            input_prototype = JS_UNDEFINED;
            JS_FreeValue(context, composition_prototype);
            composition_prototype = JS_UNDEFINED;
            JS_FreeValue(context, field_prototype);
            field_prototype = JS_UNDEFINED;
            layout_host = {};
            computed_style_host = {};
            particle_host = {};
            layout_epoch_host = {};
            console_host = {};
            media_host = {};
            storage = {};
            // The document's focus state goes with it; the host's focus (page_focus) stays.
            focus = {};
            focus_chain.clear();
            focus_visible_option.reset();
            last_focus = LastFocus::None;
            had_keyboard_event = false;
            focus_host = {};
            spatial_navigation = false;
            start_node = start_parent = start_next = {};
            pressed_node = key_active = {};
            modern_selectors = false;
            clear_host_reads();
            read_snapshot.reset();
            JS_FreeValue(context, canvas_prototype);
            canvas_prototype = JS_UNDEFINED;
            JS_FreeValue(context, document_root);
            document_root = JS_UNDEFINED;
            JS_FreeContext(context);
            context = nullptr;
            JS_RunGC(runtime);
            JS_FreeRuntime(runtime);
            canvas_factory = {};
            canvas_class = 0;
            assert(canvases.size() == 0 && canvas_budget->bytes == 0 && canvas_budget->contexts == 0);
            runtime = nullptr;
            node_class = 0;
            assert(counters.live_nodes == 0);
            assert(vm_bytes == 0);
        }

        std::string exception_message() {
            Value exception(context, JS_GetException(context));
            std::string message;
            try {
                message = string_value(context, exception.value);
            } catch (const PendingJsException&) {
                Value secondary(context, JS_GetException(context));
                message = "JavaScript exception (string conversion failed)";
            }
            try {
                if (JS_IsObject(exception.value)) {
                    Value stack(context, JS_GetPropertyStr(context, exception.value, "stack"));
                    if (!JS_IsUndefined(stack.value)) {
                        message += "\n" + string_value(context, stack.value);
                    }
                }
            } catch (const PendingJsException&) {
                Value secondary(context, JS_GetException(context));
            }
            return message;
        }

        void checkpoint(std::size_t& jobs) {
            if (faulted) {
                throw std::runtime_error(fault_prefix() + "script caught resource failure; reset_document required");
            }
            JSContext* job_context{};
            int job_result{};
            while ((job_result = JS_ExecutePendingJob(runtime, &job_context)) > 0) {
                if (faulted) {
                    throw std::runtime_error(fault_prefix() +
                                             "microtask caught resource failure; reset_document required");
                }
                if (++jobs > constants::script::max_microtask_jobs) {
                    faulted = true;
                    throw std::runtime_error("WEFT_MICROTASK_LIMIT: probe job limit exceeded; reset_document required");
                }
            }
            if (job_result < 0) {
                const auto error = exception_message();
                throw std::runtime_error((faulted ? fault_prefix() : std::string()) + error);
            }
        }

        template <class Action, class Convert>
        auto execute_result(Action&& action, Convert&& convert) {
            require_runtime();
            execution_active = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config.execution_budget_ms);

            struct ExecutionGuard {
                Impl& owner;

                ~ExecutionGuard() {
                    owner.execution_active = false;
                }
            } execution_guard{*this};

            try {
                Value result(context, action());
                std::size_t jobs{};
                checkpoint(jobs);
                if (faulted) {
                    throw std::runtime_error(fault_prefix() + "caught VM allocation failure; reset_document required");
                }
                auto converted = convert(result.value);
                if (faulted) {
                    throw std::runtime_error(fault_prefix() +
                                             "result conversion exceeded resource limit; reset_document required");
                }
                return converted;
            } catch (const PendingJsException&) {
                const auto error = exception_message();
                throw std::runtime_error((faulted ? fault_prefix() : std::string()) + error);
            } catch (const std::bad_alloc&) {
                faulted = true;
                throw std::runtime_error("WEFT_VIEW_FAULTED: native allocation failed during script evaluation");
            }
        }

        template <class Action>
        std::string execute(Action&& action) {
            return execute_result(std::forward<Action>(action), [&](JSValueConst value) {
                return string_value(context, value);
            });
        }

        std::string eval(std::string_view source, std::string_view filename) {
            ++counters.script_evaluations;
            return execute([&] {
                const std::string code(source);
                const std::string file(filename);
                return JS_Eval(context, code.c_str(), code.size(), file.c_str(), JS_EVAL_TYPE_GLOBAL);
            });
        }

        AnimationFrameResult animation_frame(double time, bool drawable) {
            require_runtime();
            if (!std::isfinite(time) || time < 0) {
                throw std::invalid_argument("WEFT_RAF_TIME: finite nonnegative timestamp required");
            }
            AnimationFrameResult report;
            execute([&] {
                JSValue args[]{JS_NewFloat64(context, time), JS_NewBool(context, drawable)};
                Value started(context, call_host(Begin, 2, args));
                std::size_t jobs{};
                checkpoint(jobs);
                std::int32_t ready{};
                if (JS_ToInt32(context, &ready, started.value) < 0) {
                    throw PendingJsException{};
                }
                if (ready < 0 || ready > constants::script::max_host_batch_callbacks) {
                    throw std::runtime_error("WEFT_RAF_HOST: invalid batch count");
                }
                for (;;) {
                    Value next(context, call_host(Next));
                    std::int32_t invoked{};
                    if (JS_ToInt32(context, &invoked, next.value) < 0) {
                        throw PendingJsException{};
                    }
                    checkpoint(jobs);
                    if (!invoked) {
                        break;
                    }
                    if (++report.callbacks > constants::script::max_host_batch_callbacks) {
                        faulted = true;
                        throw std::runtime_error("WEFT_RAF_LIMIT: batch limit exceeded");
                    }
                }
                auto count = [&](HostMethod method) {
                    Value value(context, call_host(method));
                    std::int64_t n{};
                    if (JS_ToInt64(context, &n, value.value) < 0) {
                        throw PendingJsException{};
                    }
                    if (n < 0 || n > constants::script::max_host_pending_callbacks) {
                        throw std::runtime_error("WEFT_RAF_HOST: invalid count");
                    }
                    return std::size_t(n);
                };
                report.pending = count(Pending);
                report.exceptions = report.callbacks ? count(ErrorCount) : 0;
                report.exception_messages_json = "[]";
                if (report.exceptions) {
                    Value errors(context, call_host(Errors));
                    Value json(context, JS_JSONStringify(context, errors.value, JS_UNDEFINED, JS_UNDEFINED));
                    report.exception_messages_json = string_value(context, json.value);
                }
                return JS_UNDEFINED;
            });
            return report;
        }

        void advance_clock(double time) {
            require_runtime();
            if (!std::isfinite(time) || time < 0) {
                throw std::invalid_argument("WEFT_HOST_TIME: finite nonnegative timestamp required");
            }
            clock_ms = time;
            execute_result(
                [&] {
                    JSValue argument = JS_NewFloat64(context, time);
                    return call_host(Clock, 1, &argument);
                },
                [](JSValueConst) {
                    return 0;
                });
        }

        TaskResult pump_tasks(double time) {
            require_runtime();
            if (!std::isfinite(time) || time < 0) {
                throw std::invalid_argument("WEFT_HOST_TIME: finite nonnegative timestamp required");
            }
            const auto result = execute_result(
                [&] {
                    JSValue argument = JS_NewFloat64(context, time);
                    return call_host(Pump, 1, &argument);
                },
                [&](JSValueConst value) {
                    std::uint32_t packed{};
                    if (JS_ToUint32(context, &packed, value) < 0) {
                        throw PendingJsException{};
                    }
                    return packed;
                });
            if (result / constants::script::host_task_pack_radix > constants::script::max_host_batch_callbacks) {
                throw std::runtime_error("WEFT_TASK_HOST: invalid batch count");
            }
            return {result / constants::script::host_task_pack_radix, result % constants::script::host_task_pack_radix};
        }

        void queue_scroll(Node& n) {
            auto& pending = binding(n).scroll_pending;
            if (pending) {
                return;
            }
            pending = true;
            JSValue argument = wrapper(n);
            if (JS_EnqueueJob(
                    context,
                    [](JSContext* c, int, JSValueConst* args) -> JSValue {
                        try {
                            auto& owner = state(c);
                            auto& n = node(c, args[0]);
                            owner.binding(n).scroll_pending = false;
                            if (owner.resolve(n.identity) != &n || !owner.connected(n)) {
                                return JS_UNDEFINED;
                            }
                            Value event(c, JS_NewObjectProto(c, owner.event_prototype));
                            owner.event_property(event.value, "type", JS_NewString(c, "scroll"));
                            owner.event_property(event.value, "bubbles", JS_FALSE);
                            owner.event_property(event.value, "cancelable", JS_FALSE);
                            owner.event_property(event.value, "defaultPrevented", JS_FALSE);
                            if (JS_DefinePropertyValueStr(c, event.value, "isTrusted", JS_TRUE, 0) < 0) {
                                throw PendingJsException{};
                            }
                            Value result(c, owner.dispatch(n, event.value));
                            return JS_UNDEFINED;
                        } catch (const PendingJsException&) {
                            return JS_EXCEPTION;
                        } catch (const std::exception& e) {
                            return JS_ThrowInternalError(c, "%s", e.what());
                        } catch (...) {
                            return JS_ThrowInternalError(c, "WEFT_SCROLL_JOB: native exception");
                        }
                    },
                    1, &argument) < 0) {
                pending = false;
                throw PendingJsException{};
            }
        }

        // The document copy is kept: host_snapshot() refreshes it.
        void clear_host_reads() {
            read_geometry.clear();
            read_geometry_valid = false;
            read_styles.clear();
            read_style_bytes = 0;
            read_revision = read_epoch = 0;
        }

        bool prepare_host_reads() {
            if (!layout_epoch_host) {
                clear_host_reads();
                return false;
            }
            std::uint64_t epoch{};
            {
                layout_running = true;

                struct Guard {
                    Impl& owner;

                    ~Guard() {
                        owner.layout_running = false;
                    }
                } guard{*this};

                epoch = layout_epoch_host();
            }
            if (epoch != read_epoch || tree.revision() != read_revision) {
                clear_host_reads();
                read_epoch = epoch;
                read_revision = tree.revision();
            }
            return true;
        }

        // The document as hosts read it. One copy is kept and refreshed, so a read after a small change copies only
        // what changed.
        const DomSnapshot& host_snapshot() {
            const Node& document = node(context, document_root);
            if (!read_snapshot) {
                read_snapshot = tree.snapshot(document);
                return *read_snapshot;
            }
            try {
                tree.refresh(document, *read_snapshot);
            } catch (...) {
                read_snapshot.reset();
                throw;
            }
            return *read_snapshot;
        }

        std::map<std::string, std::string> computed_style(Node& n) {
            if (!computed_style_host) {
                throw std::runtime_error("WEFT_STYLE_HOST: no computed style host");
            }
            const bool cache = prepare_host_reads();
            const std::array<std::uint64_t, 3> key{n.identity.view_epoch, n.identity.slot, n.identity.generation};
            if (cache) {
                if (auto it = read_styles.find(key); it != read_styles.end()) {
                    return it->second;
                }
            }
            std::map<std::string, std::string> values;
            {
                layout_running = true;

                struct Guard {
                    Impl& owner;

                    ~Guard() {
                        owner.layout_running = false;
                    }
                } guard{*this};

                values = computed_style_host(host_snapshot(), n.identity);
            }
            if (values.size() > constants::script::max_computed_style_properties) {
                throw std::runtime_error("WEFT_STYLE_HOST_LIMIT");
            }
            std::size_t bytes{};
            for (const auto& [k, v] : values) {
                if (k.size() > constants::script::max_computed_style_name_bytes ||
                    v.size() > constants::script::max_computed_style_value_bytes) {
                    throw std::runtime_error("WEFT_STYLE_HOST_LIMIT");
                }
                bytes += k.size() + v.size();
            }
            if (cache && bytes <= constants::script::computed_style_cache_max_bytes) {
                if (read_styles.size() >= constants::script::computed_style_cache_max_entries ||
                    read_style_bytes + bytes > constants::script::computed_style_cache_max_bytes) {
                    read_styles.clear();
                    read_style_bytes = 0;
                }
                read_style_bytes += bytes;
                read_styles.emplace(key, values);
            }
            return values;
        }

        const std::vector<ElementGeometry>& refresh_geometry() {
            require_runtime();
            if (!layout_host) {
                throw std::runtime_error("WEFT_GEOMETRY_HOST: no layout host installed");
            }
            const bool cache = prepare_host_reads();
            if (cache && read_geometry_valid) {
                return read_geometry;
            }
            std::vector<ElementGeometry> values;
            {
                layout_running = true;

                struct Guard {
                    Impl& owner;

                    ~Guard() {
                        owner.layout_running = false;
                    }
                } guard{*this};

                values = layout_host(host_snapshot());
            }
            if (values.size() > constants::max_document_nodes) {
                throw std::runtime_error("WEFT_GEOMETRY_LIMIT: element bound");
            }
            std::set<std::uint32_t> seen;
            for (const auto& g : values) {
                auto* n = resolve(g.identity);
                if (!n || !connected(*n) || n->type != constants::element_node_type ||
                    !seen.insert(g.identity.slot).second) {
                    throw std::runtime_error("WEFT_GEOMETRY_IDENTITY: stale/duplicate geometry");
                }
                if (g.client_rects.size() > constants::script::max_client_rects) {
                    throw std::runtime_error("WEFT_GEOMETRY_LIMIT: fragments");
                }
                for (double v : g.rect) {
                    if (!std::isfinite(v) || std::abs(v) > constants::script::max_logical_pixel_magnitude) {
                        throw std::runtime_error("WEFT_GEOMETRY_LIMIT: rectangle");
                    }
                }
                for (double v :
                     {g.client_width, g.client_height, g.scroll_width, g.scroll_height, g.scroll_left, g.scroll_top}) {
                    if (!std::isfinite(v) || v < 0 || v > constants::script::max_logical_pixel_magnitude) {
                        throw std::runtime_error("WEFT_GEOMETRY_LIMIT: scroll metrics");
                    }
                }
                for (auto v : g.offset) {
                    if (!std::isfinite(v) || std::abs(v) > constants::script::max_logical_pixel_magnitude) {
                        throw std::runtime_error("WEFT_GEOMETRY_LIMIT: offset");
                    }
                }
                for (auto v : g.matrix) {
                    if (!std::isfinite(v) || std::abs(v) > constants::script::max_geometry_matrix_component) {
                        throw std::runtime_error("WEFT_GEOMETRY_LIMIT: matrix");
                    }
                }
                if (g.offset[2] < 0 || g.offset[3] < 0 || (g.offset_parent.generation && !resolve(g.offset_parent))) {
                    throw std::runtime_error("WEFT_GEOMETRY_LIMIT: offset parent/size");
                }
                if (g.rect[2] < 0 || g.rect[3] < 0 || g.scroll_width < g.client_width ||
                    g.scroll_height < g.client_height || g.scroll_left > g.scroll_width - g.client_width ||
                    g.scroll_top > g.scroll_height - g.client_height || (!g.scroll_x && g.scroll_left != 0) ||
                    (!g.scroll_y && g.scroll_top != 0)) {
                    throw std::runtime_error("WEFT_GEOMETRY_LIMIT: inconsistent metrics");
                }
                for (const auto& r : g.client_rects) {
                    for (double v : r) {
                        if (!std::isfinite(v) || std::abs(v) > constants::script::max_logical_pixel_magnitude) {
                            throw std::runtime_error("WEFT_GEOMETRY_LIMIT: fragment");
                        }
                    }
                    if (r[2] < 0 || r[3] < 0) {
                        throw std::runtime_error("WEFT_GEOMETRY_LIMIT: fragment size");
                    }
                }
            }
            for (const auto& g : values) {
                auto& n = *resolve(g.identity);
                if (n.scroll_left != g.scroll_left || n.scroll_top != g.scroll_top) {
                    n.scroll_left = g.scroll_left;
                    n.scroll_top = g.scroll_top;
                    changed(n);
                    queue_scroll(n);
                }
            }
            read_geometry = std::move(values);
            read_geometry_valid = cache;
            return read_geometry;
        }

        ElementGeometry geometry(Node& n) {
            if (n.type != constants::element_node_type || !connected(n)) {
                return {};
            }
            const auto& values = refresh_geometry();
            for (const auto& g : values) {
                if (g.identity == n.identity) {
                    return g;
                }
            }
            return {};
        }

        // The focus option focus({focusVisible}) gave the focused element, which :focus-visible follows while it keeps
        // the focus.
        std::optional<bool> focus_visible_option;

        // An attribute without a namespace by name; nullptr when absent.
        static const std::string* attribute(const Node& n, std::string_view name) {
            const auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const Attribute& a) {
                return a.space.empty() && a.name == name;
            });
            return it == n.attributes.end() ? nullptr : &it->value;
        }

        // HTML's focus rules (dom/focus_rules.hpp) on an element of the tree.
        static auto attributes_of(const Node& n) {
            return [&n](std::string_view name) {
                return attribute(n, name);
            };
        }

        static bool editable(const Node& n) {
            return n.type == constants::element_node_type &&
                   dom::editable_element(n.local, n.space == constants::xhtml_namespace_uri, attributes_of(n));
        }

        static bool focusable_by_default(const Node& n) {
            return n.type == constants::element_node_type &&
                   dom::focusable_by_default(n.local, n.space == constants::xhtml_namespace_uri, attributes_of(n));
        }

        static bool disabled(const Node& n) {
            return n.type == constants::element_node_type &&
                   dom::disabled_control(n.local, n.space == constants::xhtml_namespace_uri, attributes_of(n));
        }

        static std::optional<std::int32_t> integer_attribute(const Node& n, std::string_view name) {
            const auto* text = attribute(n, name);
            return text ? dom::html_integer(*text) : std::nullopt;
        }

        // tabIndex: the tabindex attribute's integer, else 0 for an element focusable by default and -1 otherwise.
        static std::int32_t tab_index(const Node& n) {
            if (const auto v = integer_attribute(n, "tabindex")) {
                return *v;
            }
            return focusable_by_default(n) ? 0 : -1;
        }

        // A script listener that lets spatial navigation focus n (Blink's HasJSBasedEventListeners in
        // Element::HasSpatialNavigationFocusHeuristics).
        bool spatial_listener(const Node& n) const {
            const bool svg = n.space == constants::svg_namespace_uri;
            for (const Listener& l : bindings[n.identity.slot].listeners) {
                const auto in = [&](const auto& types) {
                    return std::find(types.begin(), types.end(), l.type) != types.end();
                };
                if (in(dom::spatial_navigation_events) || (svg && in(dom::svg_focus_events))) {
                    return true;
                }
            }
            return false;
        }

        // Whether n can take the focus by focus(): HTML's focusable areas, or with spatial navigation an element meant to
        // respond to the user (a listener, the focus host's pointer), being rendered (the focus host).
        bool focusable(Node& n) {
            if (n.type != constants::element_node_type || !connected(n) || disabled(n)) {
                return false;
            }
            const bool rule = focusable_by_default(n) || integer_attribute(n, "tabindex");
            const bool listening = !rule && spatial_navigation && spatial_listener(n);
            if (!rule && !listening && !spatial_navigation) {
                return false;
            }
            if (!focus_host) {
                return rule || listening;
            }
            layout_running = true;

            struct Guard {
                Impl& owner;

                ~Guard() {
                    owner.layout_running = false;
                }
            } guard{*this};

            const auto place = focus_host(host_snapshot(), n.identity);
            return place.rendered && (rule || listening || place.pointer);
        }

        // Whether the focused element n shows its focus (Blink's MatchesFocusVisiblePseudoClass): as focus() asked,
        // else always when it is editable, else unless the mouse moved the focus last and no key went down since.
        bool shows_focus(const Node& n) const {
            if (focus_visible_option) {
                return *focus_visible_option;
            }
            return editable(n) || last_focus != LastFocus::Mouse || had_keyboard_event;
        }

        // Brings the focus flags of the nodes up to date with the focus: :focus and :focus-visible on the focused
        // element, :focus-within on it and its ancestors, while the page has the host's focus. Only nodes whose flags
        // change are restyled.
        void update_focus_flags() {
            std::vector<NodeIdentity> chain;
            Node* target = resolve(focus);
            if (target && (!connected(*target) || !page_focus)) {
                target = nullptr;
            }
            for (Node* p = target; p; p = parent_of(*p)) {
                chain.push_back(p->identity);
            }
            for (const auto id : focus_chain) {
                if (std::find(chain.begin(), chain.end(), id) != chain.end()) {
                    continue;
                }
                if (auto* n = resolve(id); n && (n->focused || n->focus_visible || n->focus_within)) {
                    n->focused = n->focus_visible = n->focus_within = false;
                    changed(*n);
                }
            }
            for (const auto id : chain) {
                Node& n = node_at(id);
                const bool focused = &n == target;
                const bool visible = focused && shows_focus(n);
                if (n.focused != focused || n.focus_visible != visible || !n.focus_within) {
                    n.focused = focused;
                    n.focus_visible = visible;
                    n.focus_within = true;
                    changed(n);
                }
            }
            focus_chain = std::move(chain);
        }

        // Gives target the focus (none: no element has it) without events, as the focus fixup rule does.
        void set_focus(Node* target, std::optional<bool> visible = {}) {
            focus = target ? target->identity : NodeIdentity{};
            focus_visible_option = target ? visible : std::nullopt;
            update_focus_flags();
        }

        // The element that has the focus; one that left the document loses it (the focus fixup rule).
        Node* focused_node() {
            auto* n = resolve(focus);
            if (n && !connected(*n)) {
                set_focus(nullptr);
                return nullptr;
            }
            return n;
        }

        // The page's clock when the host last gave it (performance.now()), the time stamp of events it makes itself.
        double clock_ms{};

        // A trusted FocusEvent at target: focus and blur do not bubble, focusin, focusout and their DOM Level 2 names
        // do; none of them is cancelable.
        void dispatch_focus_event(Node& target, const char* type, Node* related) {
            Value event(context, JS_NewObjectProto(context, focus_prototype));
            if (JS_IsException(event.value)) {
                throw PendingJsException{};
            }
            const std::string_view name(type);
            event_property(event.value, "type", JS_NewString(context, type));
            event_property(event.value, "bubbles", JS_NewBool(context, name != "focus" && name != "blur"));
            event_property(event.value, "cancelable", JS_FALSE);
            event_property(event.value, "composed", JS_TRUE);
            event_property(event.value, "defaultPrevented", JS_FALSE);
            event_property(event.value, "currentTarget", JS_NULL);
            event_property(event.value, "eventPhase", JS_NewInt32(context, constants::script::event_phase_none));
            event_property(event.value, "view", JS_GetGlobalObject(context));
            event_property(event.value, "detail", JS_NewInt32(context, 0));
            event_property(event.value, "timeStamp", JS_NewFloat64(context, clock_ms));
            event_property(event.value, "relatedTarget",
                           related && resolve(related->identity) ? JS_DupValue(context, wrapper(*related)) : JS_NULL);
            if (JS_DefinePropertyValueStr(context, event.value, "isTrusted", JS_TRUE, 0) < 0) {
                throw PendingJsException{};
            }
            Value result(context, dispatch(target, event.value));
        }

        // A gamepad's state to the scene platform's __ceGamepad (index, connected, id, values, pressed, touched, axes,
        // time), which keeps what navigator.getGamepads() shows and fires the gamepad events.
        void gamepad(const GamepadState& state, double time_ms) {
            Value global(context, JS_GetGlobalObject(context));
            Value function(context, JS_GetPropertyStr(context, global.value, "__ceGamepad"));
            if (JS_IsException(function.value)) {
                throw PendingJsException{};
            }
            if (!JS_IsFunction(context, function.value)) {
                return;
            }
            const auto numbers = [&](const auto& values, Value& array) {
                array.value = JS_NewArray(context);
                if (JS_IsException(array.value)) {
                    throw PendingJsException{};
                }
                for (std::size_t k = 0; k < values.size(); ++k) {
                    if (JS_SetPropertyUint32(context, array.value, std::uint32_t(k), JS_NewFloat64(context, values[k])) <
                        0) {
                        throw PendingJsException{};
                    }
                }
            };
            Value id(context, JS_NewStringLen(context, state.id.data(), state.id.size()));
            Value buttons(context, JS_UNDEFINED);
            Value axes(context, JS_UNDEFINED);
            numbers(state.buttons, buttons);
            numbers(state.axes, axes);
            JSValue args[]{JS_NewUint32(context, state.index),
                           JS_NewBool(context, state.connected),
                           id.value,
                           buttons.value,
                           JS_NewUint32(context, state.pressed),
                           JS_NewUint32(context, state.touched),
                           axes.value,
                           JS_NewFloat64(context, time_ms)};
            Value result(context, JS_Call(context, function.value, global.value, 8, args));
            if (JS_IsException(result.value)) {
                throw PendingJsException{};
            }
        }

        // window's focus or blur, through the window target of the scene platform (nothing without it).
        void dispatch_window_focus_event(const char* type) {
            Value global(context, JS_GetGlobalObject(context));
            Value function(context, JS_GetPropertyStr(context, global.value, "dispatchEvent"));
            if (JS_IsException(function.value)) {
                throw PendingJsException{};
            }
            if (!JS_IsFunction(context, function.value)) {
                return;
            }
            Value event(context, JS_NewObjectProto(context, focus_prototype));
            if (JS_IsException(event.value)) {
                throw PendingJsException{};
            }
            event_property(event.value, "type", JS_NewString(context, type));
            event_property(event.value, "bubbles", JS_FALSE);
            event_property(event.value, "cancelable", JS_FALSE);
            event_property(event.value, "defaultPrevented", JS_FALSE);
            event_property(event.value, "view", JS_GetGlobalObject(context));
            event_property(event.value, "detail", JS_NewInt32(context, 0));
            event_property(event.value, "timeStamp", JS_NewFloat64(context, clock_ms));
            event_property(event.value, "relatedTarget", JS_NULL);
            if (JS_DefinePropertyValueStr(context, event.value, "isTrusted", JS_TRUE, 0) < 0) {
                throw PendingJsException{};
            }
            JSValue argument = event.value;
            Value result(context, JS_Call(context, function.value, global.value, 1, &argument));
            if (JS_IsException(result.value)) {
                throw PendingJsException{};
            }
        }

        // HTML's focusing steps as Blink's Document::SetFocusedElement runs them: the old element loses the focus
        // (activeElement is the body meanwhile) and gets blur, focusout and DOMFocusOut, related to the new one; the
        // new one takes the focus and gets focus, focusin and DOMFocusIn. A listener that moves the focus meanwhile
        // wins and the rest is skipped. Without the host's focus the page gets these events when it comes back. Whether
        // `to` has the focus afterwards.
        bool move_focus(Node* to, FocusType type, bool scroll, std::optional<bool> visible = {}) {
            Node* from = focused_node();
            // Listeners may drop every other reference to the two nodes.
            Value hold_from(context, from ? JS_DupValue(context, wrapper(*from)) : JS_UNDEFINED);
            Value hold_to(context, to ? JS_DupValue(context, wrapper(*to)) : JS_UNDEFINED);
            if (from == to) {
                if (to && visible != focus_visible_option) {
                    focus_visible_option = visible;
                    update_focus_flags();
                }
                if (to && scroll) {
                    scroll_into_view(*to, ScrollAlign::IfNeeded, ScrollAlign::IfNeeded);
                }
                return true;
            }
            // The mouse moving the focus hides it until a key goes down (Blink's Element::SetFocused).
            if (type == FocusType::Mouse) {
                had_keyboard_event = false;
            }
            if (from) {
                // A text field the focus leaves ends its composition (Chrome's FinishComposingText), then, without the
                // focus, commits its change before blur (TextControlElement's blur handling).
                if (page_focus && from->field) {
                    finish_composition(*from);
                    if (focused_node() != from) {
                        return resolve(focus) == to && to;
                    }
                }
                set_focus(nullptr);
                if (page_focus) {
                    if (from->field) {
                        commit_change(*from);
                    }
                    dispatch_focus_event(*from, "blur", to);
                    // focusout follows whatever blur's listeners did; it names no new element when they moved the
                    // focus.
                    const bool blocked = focus.generation != 0;
                    dispatch_focus_event(*from, "focusout", blocked ? nullptr : to);
                    dispatch_focus_event(*from, "DOMFocusOut", blocked ? nullptr : to);
                    if (focus.generation) {
                        return resolve(focus) == to && to;
                    }
                }
            }
            if (!to || !connected(*to)) {
                return !to;
            }
            if (type != FocusType::Script) {
                last_focus = type == FocusType::Mouse ? LastFocus::Mouse : LastFocus::Other;
            }
            set_focus(to, visible);
            start_node = to->identity;
            start_parent = start_next = {};
            // The keyboard selects all of an input it focuses before focus, and select follows (Chrome's
            // SelectionBehaviorOnFocus::kReset).
            bool selected{};
            if (type == FocusType::Keyboard && to->field && !to->field->multiline) {
                auto next = field_copy(*to);
                next->selection_start = 0;
                next->selection_end = static_cast<std::uint32_t>(next->value.size());
                next->backward = false;
                selected = store_field(*to, std::move(next));
            }
            if (page_focus) {
                const auto id = to->identity;
                for (const char* name : {"focus", "focusin", "DOMFocusIn"}) {
                    dispatch_focus_event(*to, name, from);
                    if (focus != id) {
                        return false;
                    }
                }
            }
            if (selected) {
                dispatch_plain_event(*to, "select");
            }
            if (scroll) {
                scroll_into_view(*to, ScrollAlign::IfNeeded, ScrollAlign::IfNeeded);
            }
            return true;
        }

        // The host's focus came to the page or left it (Blink's DispatchEventsOnWindowAndFocusedElement).
        void set_page_focus(bool focused) {
            if (focused == page_focus) {
                return;
            }
            page_focus = focused;
            Node* target = focused_node();
            Value hold(context, target ? JS_DupValue(context, wrapper(*target)) : JS_UNDEFINED);
            if (!focused) {
                if (target && target->field) {
                    finish_composition(*target);
                }
                update_focus_flags();
                if (target) {
                    if (target->field) {
                        commit_change(*target);
                    }
                    dispatch_focus_event(*target, "blur", nullptr);
                    if (focused_node() == target) {
                        dispatch_focus_event(*target, "focusout", nullptr);
                    }
                }
                dispatch_window_focus_event("blur");
                return;
            }
            dispatch_window_focus_event("focus");
            target = focused_node();
            update_focus_flags();
            if (target) {
                const auto id = target->identity;
                dispatch_focus_event(*target, "focus", nullptr);
                if (focus == id) {
                    dispatch_focus_event(*target, "focusin", nullptr);
                }
            }
        }

        // A key went down without Control, Alt or Meta: the focused element shows its focus from now on, whoever gave
        // it (Blink's Node::UpdateHadKeyboardEvent).
        void note_keyboard_event() {
            if (!had_keyboard_event) {
                had_keyboard_event = true;
                update_focus_flags();
            }
        }

        // Tree order (pre-order) over the document's nodes.
        Node* next_in_order(const Node& n, bool children = true) const {
            if (children && !n.children.empty()) {
                return &node_at(n.children.front());
            }
            for (const Node* p = &n; p; p = parent_of(*p)) {
                const Node* parent = parent_of(*p);
                if (!parent) {
                    return nullptr;
                }
                const auto& siblings = parent->children;
                const auto it = std::find(siblings.begin(), siblings.end(), p->identity);
                if (it != siblings.end() && it + 1 != siblings.end()) {
                    return &node_at(*(it + 1));
                }
            }
            return nullptr;
        }

        Node* previous_in_order(const Node& n) const {
            const Node* parent = parent_of(n);
            if (!parent) {
                return nullptr;
            }
            const auto& siblings = parent->children;
            const auto it = std::find(siblings.begin(), siblings.end(), n.identity);
            if (it == siblings.begin() || it == siblings.end()) {
                return const_cast<Node*>(parent);
            }
            Node* last = &node_at(*(it - 1));
            while (!last->children.empty()) {
                last = &node_at(last->children.back());
            }
            return last;
        }

        // Blink's Document::SequentialFocusNavigationStartingPoint for a focus moving forward or backward.
        std::optional<NodeIdentity> navigation_start(bool forward) {
            if (auto* f = focused_node()) {
                return f->identity;
            }
            const auto element = [](Node* n) -> std::optional<NodeIdentity> {
                return n && n->type == constants::element_node_type ? std::optional(n->identity) : std::nullopt;
            };
            if (auto* start = resolve(start_node); start && connected(*start)) {
                if (start->type == constants::element_node_type) {
                    return start->identity;
                }
                // A text node: the element before it (forward) or after it (backward), else its parent.
                for (Node* n = forward ? previous_in_order(*start) : next_in_order(*start); n;
                     n = forward ? previous_in_order(*n) : next_in_order(*n)) {
                    if (n->type == constants::element_node_type) {
                        return n->identity;
                    }
                }
                return element(parent_of(*start));
            }
            auto* parent = resolve(start_parent);
            if (!parent || !connected(*parent)) {
                return std::nullopt;
            }
            // The node at the place the starting node left: the sibling that followed it, or what follows the parent.
            Node* at = resolve(start_next);
            if (!at || parent_of(*at) != parent) {
                at = next_in_order(*parent, false);
            }
            if (forward) {
                for (Node* n = at ? previous_in_order(*at) : nullptr; n; n = previous_in_order(*n)) {
                    if (n->type == constants::element_node_type) {
                        return n->identity;
                    }
                }
            }
            for (Node* n = at; n; n = next_in_order(*n)) {
                if (n->type == constants::element_node_type) {
                    return n->identity;
                }
            }
            return std::nullopt;
        }

        // The focused element is about to leave the document with subtree (subtree itself, or with self false only its
        // descendants): it gets blur and focusout first, as Blink's Document::RemoveFocusedElementOfSubtree gives them.
        void blur_within(Node& subtree, bool self = true) {
            Node* f = focused_node();
            if (!f || (!self && f == &subtree) || !connected(subtree)) {
                return;
            }
            for (Node* p = f; p; p = parent_of(*p)) {
                if (p == &subtree) {
                    move_focus(nullptr, FocusType::Script, false);
                    return;
                }
            }
        }

        // The element Space made :active (HTMLElement::HandleKeyboardActivation).
        void set_key_active(Node* target) {
            if (auto* old = resolve(key_active); old && old != target && old->active) {
                old->active = false;
                changed(*old);
            }
            key_active = target ? target->identity : NodeIdentity{};
            if (target && !target->active) {
                target->active = true;
                changed(*target);
            }
        }

        // The geometry of an element in the latest layout; none for one layout does not place.
        static const ElementGeometry* find_geometry(const std::vector<ElementGeometry>& values, NodeIdentity id) {
            const auto it = std::find_if(values.begin(), values.end(), [&](const ElementGeometry& g) {
                return g.identity == id;
            });
            return it == values.end() ? nullptr : &*it;
        }

        // How far a scroll container moves its content so that an element shows in it as align asks, along one axis:
        // the element's start and size and the scrollport's, in the same coordinates (CSSOM View's "scroll an
        // element into view", and Blink's ScrollAlignment for IfNeeded).
        static double align_delta(double start, double size, double port, double extent, ScrollAlign align) {
            const double end = start + size;
            const double port_end = port + extent;
            const auto nearest = [&] {
                // Both edges outside: nothing; the near edge in when it fits, else the far one.
                if ((start < port && end > port_end) || (start >= port && end <= port_end)) {
                    return 0.0;
                }
                if ((start < port && size <= extent) || (end > port_end && size > extent)) {
                    return start - port;
                }
                return end - port_end;
            };
            switch (align) {
                case ScrollAlign::Start:
                    return start - port;
                case ScrollAlign::End:
                    return end - port_end;
                case ScrollAlign::Center:
                    return (start + size / 2) - (port + extent / 2);
                case ScrollAlign::Nearest:
                    return nearest();
                case ScrollAlign::IfNeeded:
                    if (start >= port && end <= port_end) {
                        return 0;
                    }
                    if (end <= port || start >= port_end) {
                        return (start + size / 2) - (port + extent / 2);
                    }
                    return nearest();
            }
            return 0;
        }

        // Scrolls target's scroll containers, innermost first, so that it shows (nothing without a layout host).
        void scroll_into_view(Node& target, ScrollAlign block, ScrollAlign inline_align) {
            if (!layout_host || target.type != constants::element_node_type || !connected(target)) {
                return;
            }
            const auto values = refresh_geometry();
            const auto* own = find_geometry(values, target.identity);
            if (!own) {
                return;
            }
            auto rect = own->rect;
            for (Node* n = parent_of(target); n; n = parent_of(*n)) {
                const auto* g = find_geometry(values, n->identity);
                if (!g || (!g->scroll_x && !g->scroll_y)) {
                    continue;
                }
                bool moved{};
                if (g->scroll_y) {
                    const double max = std::max(0.0, g->scroll_height - g->client_height);
                    const double top = std::clamp(
                        g->scroll_top +
                            align_delta(rect[1], rect[3], g->rect[1] + g->client_top, g->client_height, block),
                        0.0, max);
                    if (top != g->scroll_top) {
                        n->scroll_top = top;
                        rect[1] -= top - g->scroll_top;
                        moved = true;
                    }
                }
                if (g->scroll_x) {
                    const double max = std::max(0.0, g->scroll_width - g->client_width);
                    const double left = std::clamp(
                        g->scroll_left + align_delta(rect[0], rect[2], g->rect[0] + g->client_left, g->client_width,
                                                     inline_align),
                        0.0, max);
                    if (left != g->scroll_left) {
                        n->scroll_left = left;
                        rect[0] -= left - g->scroll_left;
                        moved = true;
                    }
                }
                if (moved) {
                    changed(*n);
                    queue_scroll(*n);
                }
            }
        }

        // Keyboard scrolling (Blink's ScrollManager::LogicalScroll): the nearest user-scrollable container, from start
        // up, that can still go that way moves a line (40 px), seven eighths of its scrollport, or to its end.
        bool scroll_by_key(Node& start, ScrollDirection direction, ScrollGranularity granularity) {
            if (!layout_host || !connected(start)) {
                return false;
            }
            const auto values = refresh_geometry();
            const bool vertical = direction == ScrollDirection::Up || direction == ScrollDirection::Down;
            const double sign = direction == ScrollDirection::Up || direction == ScrollDirection::Left ? -1 : 1;
            for (Node* n = &start; n; n = parent_of(*n)) {
                const auto* g = find_geometry(values, n->identity);
                if (!g || !(vertical ? g->wheel_y : g->wheel_x)) {
                    continue;
                }
                const double extent = vertical ? g->client_height : g->client_width;
                const double max = std::max(0.0, vertical ? g->scroll_height - g->client_height
                                                          : g->scroll_width - g->client_width);
                const double now = vertical ? g->scroll_top : g->scroll_left;
                double next = now;
                switch (granularity) {
                    case ScrollGranularity::Line:
                        next = now + sign * constants::input::keyboard_line_step;
                        break;
                    case ScrollGranularity::Page:
                        next = now + sign * std::max(1.0, extent * constants::input::keyboard_page_fraction);
                        break;
                    case ScrollGranularity::Document:
                        next = sign < 0 ? 0 : max;
                        break;
                }
                next = std::clamp(next, 0.0, max);
                if (next == now) {
                    continue;
                }
                (vertical ? n->scroll_top : n->scroll_left) = next;
                changed(*n);
                queue_scroll(*n);
                return true;
            }
            return false;
        }

        // Sets an attribute without a namespace (an HTML element's name in lower case).
        void set_attribute(Node& n, const std::string& name, std::string value) {
            auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const Attribute& a) {
                return a.space.empty() && a.name == name;
            });
            if (it != n.attributes.end() && it->value == value) {
                return;
            }
            if (it == n.attributes.end()) {
                n.attributes.push_back({name, name, {}, std::move(value)});
            } else {
                it->value = std::move(value);
            }
            changed(n);
            sync_field(n);
        }

        // Text fields (dom/text_field.hpp): an HTML input of a text type and a textarea. A field's state is its node's
        // field, replaced whole when it changes (snapshots share the old one); its offsets are the value's UTF-8 bytes,
        // the DOM's UTF-16 code units.

        // Line breaks as a textarea keeps them: CR LF and CR become LF.
        static std::string line_feeds(std::string text) {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '\r') {
                    out += text[i];
                    continue;
                }
                out += '\n';
                if (i + 1 < text.size() && text[i + 1] == '\n') {
                    ++i;
                }
            }
            return out;
        }

        // A valid floating-point number (HTML §2.3.4.3): an optional minus, digits with an optional fraction (or only
        // the fraction), and an optional exponent.
        static bool valid_float(std::string_view s) {
            std::size_t i = 0;
            const auto digits = [&] {
                const auto from = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                    ++i;
                }
                return i > from;
            };
            if (i < s.size() && s[i] == '-') {
                ++i;
            }
            const bool whole = digits();
            if (i < s.size() && s[i] == '.') {
                ++i;
                if (!digits()) {
                    return false;
                }
            } else if (!whole) {
                return false;
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                    ++i;
                }
                if (!digits()) {
                    return false;
                }
            }
            return i == s.size();
        }

        // HTML's value sanitization algorithm of the field's type: a textarea's line breaks become LF; an input loses
        // its line breaks, an email or url also its leading and trailing white space, and a number that is not a valid
        // floating-point number becomes empty.
        static std::string sanitize_value(const Node& n, std::string value) {
            if (n.local == "textarea") {
                return line_feeds(std::move(value));
            }
            std::erase_if(value, [](char c) {
                return c == '\r' || c == '\n';
            });
            const auto type = text_field_type(attribute(n, "type"));
            if (type == "email" || type == "url") {
                const auto space = [](char c) {
                    return c == ' ' || c == '\t' || c == '\f';
                };
                while (!value.empty() && space(value.back())) {
                    value.pop_back();
                }
                value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
            }
            if (type == "number" && !valid_float(value)) {
                value.clear();
            }
            return value;
        }

        // A field's default value: an input's value content attribute, a textarea's child text.
        std::string default_value(const Node& n) const {
            if (n.local != "textarea") {
                const auto* v = attribute(n, "value");
                return v ? *v : std::string();
            }
            std::string text;
            for (const auto id : n.children) {
                if (const auto& child = node_at(id); child.type == constants::text_node_type) {
                    text += child.data;
                }
            }
            return text;
        }

        // An offset into a value: at most its size, at the start of a character.
        static std::uint32_t boundary(std::string_view value, std::size_t offset) {
            offset = std::min(offset, value.size());
            while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xC0) == 0x80) {
                ++offset;
            }
            return static_cast<std::uint32_t>(offset);
        }

        // Keeps a field's selection and composition in its value; a composition that does not fit ends.
        static void clamp_field(TextFieldState& f) {
            f.selection_start = boundary(f.value, f.selection_start);
            f.selection_end = std::max(f.selection_start, boundary(f.value, f.selection_end));
            if (f.composing && (f.composition_start > f.composition_end || f.composition_end > f.value.size())) {
                f.composing = false;
            }
            if (!f.composing) {
                f.composition_start = f.composition_end = f.clause_start = f.clause_end = 0;
                return;
            }
            f.clause_start = std::clamp(f.clause_start, f.composition_start, f.composition_end);
            f.clause_end = std::clamp(f.clause_end, f.clause_start, f.composition_end);
        }

        std::shared_ptr<TextFieldState> field_copy(const Node& n) const {
            return std::make_shared<TextFieldState>(*n.field);
        }

        // Makes next n's field state when it differs from the one there: n restyles, and selectionchange follows when
        // the selection moved. Whether it changed.
        bool store_field(Node& n, std::shared_ptr<TextFieldState> next) {
            clamp_field(*next);
            const auto* old = n.field.get();
            if (old && *old == *next) {
                return false;
            }
            const bool moved =
                old && (old->selection_start != next->selection_start || old->selection_end != next->selection_end);
            n.field = std::move(next);
            changed(n);
            if (moved) {
                queue_event(n, "selectionchange");
            }
            return true;
        }

        // Brings n's text field state up to date with the element: whether it is a field (an input's type decides),
        // its placeholder, whether the user may edit it, and its value, its default value while the user and script
        // have not set it (the value is not dirty). The selection and composition stay in the value.
        void sync_field(Node& n) {
            if (n.type != constants::element_node_type || n.space != constants::xhtml_namespace_uri ||
                (n.local != "input" && n.local != "textarea")) {
                return;
            }
            auto& b = binding(n);
            const auto* type = attribute(n, "type");
            if (!text_field_element(n.local, true, type)) {
                if (n.field) {
                    n.field.reset();
                    b.field_dirty = false;
                    b.value_before_edit.reset();
                    changed(n);
                }
                return;
            }
            auto next = n.field ? field_copy(n) : std::make_shared<TextFieldState>();
            next->multiline = n.local == "textarea";
            next->password = !next->multiline && text_field_type(type) == "password";
            const auto* placeholder = attribute(n, "placeholder");
            next->placeholder = placeholder ? *placeholder : std::string();
            if (next->multiline) {
                next->placeholder = line_feeds(std::move(next->placeholder));
            } else {
                std::erase_if(next->placeholder, [](char c) {
                    return c == '\r' || c == '\n';
                });
            }
            next->editable = !attribute(n, "readonly") && !disabled(n);
            next->value = sanitize_value(n, b.field_dirty ? std::move(next->value) : default_value(n));
            store_field(n, std::move(next));
        }

        // A textarea's child text changed: its default value with it.
        void textarea_changed(NodeIdentity parent) {
            if (auto* p = resolve(parent); p && p->local == "textarea") {
                sync_field(*p);
            }
        }

        // Sets what a trusted event starts with: its type and flags, no target yet, the page's clock.
        void init_event(JSValueConst event, const char* type, bool bubbles, bool cancelable, bool composed) {
            event_property(event, "type", JS_NewString(context, type));
            event_property(event, "bubbles", JS_NewBool(context, bubbles));
            event_property(event, "cancelable", JS_NewBool(context, cancelable));
            event_property(event, "composed", JS_NewBool(context, composed));
            event_property(event, "defaultPrevented", JS_FALSE);
            event_property(event, "currentTarget", JS_NULL);
            event_property(event, "eventPhase", JS_NewInt32(context, constants::script::event_phase_none));
            event_property(event, "timeStamp", JS_NewFloat64(context, clock_ms));
            if (JS_DefinePropertyValueStr(context, event, "isTrusted", JS_TRUE, 0) < 0) {
                throw PendingJsException{};
            }
        }

        // An Event of type at target, bubbling and not cancelable: change, select, selectionchange, a search field's
        // input.
        void dispatch_plain_event(Node& target, const char* type) {
            Value event(context, JS_NewObjectProto(context, event_prototype));
            init_event(event.value, type, true, false, false);
            Value result(context, dispatch(target, event.value));
        }

        // An InputEvent at target (beforeinput, input): its inputType, its data (none: null) and whether a composition
        // goes on. Whether its default action may go ahead.
        bool dispatch_input_event(Node& target, const char* type, std::string_view input_type,
                                  std::optional<std::string_view> data, bool composing, bool cancelable) {
            Value event(context, JS_NewObjectProto(context, input_prototype));
            init_event(event.value, type, true, cancelable, true);
            event_property(event.value, "view", JS_GetGlobalObject(context));
            event_property(event.value, "detail", JS_NewInt32(context, 0));
            event_property(event.value, "inputType", JS_NewStringLen(context, input_type.data(), input_type.size()));
            event_property(event.value, "data", data ? JS_NewStringLen(context, data->data(), data->size()) : JS_NULL);
            event_property(event.value, "isComposing", JS_NewBool(context, composing));
            event_property(event.value, "dataTransfer", JS_NULL);
            Value result(context, dispatch(target, event.value));
            return JS_ToBool(context, result.value) > 0;
        }

        // A CompositionEvent at target: compositionstart (cancelable, to no effect), compositionupdate, compositionend.
        void dispatch_composition_event(Node& target, const char* type, std::string_view data) {
            Value event(context, JS_NewObjectProto(context, composition_prototype));
            init_event(event.value, type, true, std::string_view(type) == "compositionstart", true);
            event_property(event.value, "view", JS_GetGlobalObject(context));
            event_property(event.value, "detail", JS_NewInt32(context, 0));
            event_property(event.value, "data", JS_NewStringLen(context, data.data(), data.size()));
            Value result(context, dispatch(target, event.value));
        }

        // Queues an Event of type at n, as a task fires it after the running script: selectionchange once until it
        // fires, select (a script's selection, Chrome's ScheduleSelectEvent) each time.
        void queue_event(Node& n, const char* type) {
            const bool coalesced = std::string_view(type) == "selectionchange";
            if (coalesced) {
                if (binding(n).selectionchange_pending) {
                    return;
                }
                binding(n).selectionchange_pending = true;
            }
            Value name(context, JS_NewString(context, type));
            JSValue args[]{wrapper(n), name.value};
            if (JS_EnqueueJob(
                    context,
                    [](JSContext* c, int, JSValueConst* args) -> JSValue {
                        try {
                            auto& owner = state(c);
                            auto& n = node(c, args[0]);
                            const auto type = string_value(c, args[1]);
                            if (type == "selectionchange") {
                                owner.binding(n).selectionchange_pending = false;
                            }
                            if (owner.resolve(n.identity) != &n || !owner.connected(n)) {
                                return JS_UNDEFINED;
                            }
                            owner.dispatch_plain_event(n, type.c_str());
                            return JS_UNDEFINED;
                        } catch (const PendingJsException&) {
                            return JS_EXCEPTION;
                        } catch (const std::exception& e) {
                            return JS_ThrowInternalError(c, "%s", e.what());
                        } catch (...) {
                            return JS_ThrowInternalError(c, "WEFT_FIELD_EVENT_JOB: native exception");
                        }
                    },
                    2, args) < 0) {
                if (coalesced) {
                    binding(n).selectionchange_pending = false;
                }
                throw PendingJsException{};
            }
        }

        // Throws a DOMException named name (a RangeError without the web platform module's DOMException).
        [[noreturn]] void throw_dom(const char* name, const char* message) {
            Value global(context, JS_GetGlobalObject(context));
            Value constructor(context, JS_GetPropertyStr(context, global.value, "DOMException"));
            if (JS_IsFunction(context, constructor.value)) {
                Value text(context, JS_NewString(context, message));
                Value kind(context, JS_NewString(context, name));
                JSValue args[]{text.value, kind.value};
                JSValue error = JS_CallConstructor(context, constructor.value, 2, args);
                if (!JS_IsException(error)) {
                    JS_Throw(context, error);
                }
                throw PendingJsException{};
            }
            JS_ThrowRangeError(context, "%s: %s", name, message);
            throw PendingJsException{};
        }

        void remove_attribute(Node& n, std::string_view name) {
            const auto it = std::find_if(n.attributes.begin(), n.attributes.end(), [&](const Attribute& a) {
                return a.space.empty() && a.name == name;
            });
            if (it != n.attributes.end()) {
                n.attributes.erase(it);
                changed(n);
                sync_field(n);
            }
        }

        // Whether the selection API (selectionStart, selectionEnd, selectionDirection, setSelectionRange, setRangeText)
        // applies to a text field: a textarea, or an input of type text, search, url, tel or password (HTML: not email
        // and number, whose selection script cannot see).
        static bool selectable(const Node& n) {
            if (!n.field) {
                return false;
            }
            if (n.local == "textarea") {
                return true;
            }
            const auto type = text_field_type(attribute(n, "type"));
            return type != "email" && type != "number";
        }

        // The text field API's attributes (HTMLInputElement, HTMLTextAreaElement), on an input of another type too.
        JSValue field_property(Node& n, int magic) {
            const auto* f = selectable(n) || (magic != SelectionStart && magic != SelectionEnd &&
                                              magic != SelectionDirection)
                                ? n.field.get()
                                : nullptr;
            const auto string = [&](std::string_view s) {
                return JS_NewStringLen(context, s.data(), s.size());
            };
            const auto attribute_text = [&](std::string_view name) {
                const auto* v = attribute(n, name);
                return v ? string(*v) : string({});
            };
            switch (magic) {
                case FieldValue: {
                    if (f) {
                        return string(f->value);
                    }
                    // An input of another type: its value attribute ("on" for a checkbox or radio without one).
                    if (const auto* v = attribute(n, "value")) {
                        return string(*v);
                    }
                    const auto type = text_field_type(attribute(n, "type"));
                    return string(type == "checkbox" || type == "radio" ? "on" : "");
                }
                case DefaultValue:
                    return string(default_value(n));
                case SelectionStart:
                case SelectionEnd:
                    if (!f) {
                        return JS_NULL;
                    }
                    return JS_NewUint32(context, static_cast<std::uint32_t>(utf16_offset(
                                                     f->value, magic == SelectionStart ? f->selection_start
                                                                                       : f->selection_end)));
                case SelectionDirection:
                    // Windows has no "none": a selection without a direction goes forward (Chrome's
                    // ShouldConsiderSelectionAsDirectional).
                    return f ? JS_NewString(context, f->backward ? "backward" : "forward") : JS_NULL;
                case InputType:
                    return n.local == "textarea" ? JS_NewString(context, "textarea")
                                                 : string(text_field_type(attribute(n, "type")));
                case Placeholder:
                    return attribute_text("placeholder");
                case ReadOnly:
                    return JS_NewBool(context, attribute(n, "readonly") != nullptr);
                case Disabled:
                    return JS_NewBool(context, attribute(n, "disabled") != nullptr);
                case MaxLength: {
                    const auto v = integer_attribute(n, "maxlength");
                    return JS_NewInt32(context, v && *v >= 0 ? *v : -1);
                }
                case FieldName:
                    return attribute_text("name");
                case TextLength:
                    return JS_NewUint32(context, f ? static_cast<std::uint32_t>(utf16_length(f->value)) : 0);
                default:
                    return JS_UNDEFINED;
            }
        }

        void set_field_property(Node& n, int magic, JSValueConst value) {
            switch (magic) {
                case FieldValue: {
                    // [LegacyNullToEmptyString]
                    std::string text = JS_IsNull(value) ? std::string() : string_value(context, value);
                    if (!n.field) {
                        set_attribute(n, "value", std::move(text));
                    } else {
                        set_value(n, std::move(text));
                    }
                    return;
                }
                case DefaultValue: {
                    std::string text = string_value(context, value);
                    if (n.local == "textarea") {
                        set_text(n, std::move(text));
                    } else {
                        set_attribute(n, "value", std::move(text));
                    }
                    return;
                }
                case SelectionStart:
                case SelectionEnd:
                case SelectionDirection: {
                    // Conversions first: they may run script.
                    std::uint32_t units{};
                    std::string direction;
                    if (magic == SelectionDirection) {
                        direction = string_value(context, value);
                    } else if (JS_ToUint32(context, &units, value) < 0) {
                        throw PendingJsException{};
                    }
                    if (!selectable(n)) {
                        throw_dom("InvalidStateError", "The element's type does not support selection.");
                    }
                    const auto& f = *n.field;
                    auto start = f.selection_start;
                    auto end = f.selection_end;
                    bool backward = f.backward;
                    const auto offset = static_cast<std::uint32_t>(byte_offset(f.value, units));
                    if (magic == SelectionStart) {
                        start = offset;
                        end = std::max(end, offset);
                    } else if (magic == SelectionEnd) {
                        end = offset;
                    } else {
                        backward = direction == "backward";
                    }
                    set_selection_range(n, start, end, backward);
                    return;
                }
                case InputType:
                    if (n.local != "textarea") {
                        set_attribute(n, "type", string_value(context, value));
                    }
                    return;
                case Placeholder:
                    set_attribute(n, "placeholder", string_value(context, value));
                    return;
                case FieldName:
                    set_attribute(n, "name", string_value(context, value));
                    return;
                case ReadOnly:
                case Disabled: {
                    const int on = JS_ToBool(context, value);
                    if (on < 0) {
                        throw PendingJsException{};
                    }
                    const char* name = magic == ReadOnly ? "readonly" : "disabled";
                    if (on) {
                        set_attribute(n, name, "");
                    } else {
                        remove_attribute(n, name);
                    }
                    return;
                }
                case MaxLength: {
                    std::int32_t v{};
                    if (JS_ToInt32(context, &v, value) < 0) {
                        throw PendingJsException{};
                    }
                    if (v < 0) {
                        throw_dom("IndexSizeError", "The value provided is negative.");
                    }
                    set_attribute(n, "maxlength", std::to_string(v));
                    return;
                }
                default:
                    return;
            }
        }

        // setSelectionRange(start, end, direction), select() and setRangeText(replacement, start, end, selectionMode).
        JSValue field_method(Node& n, int magic, int argc, JSValueConst* argv) {
            switch (magic) {
                case SetSelectionRange: {
                    std::uint32_t start{};
                    std::uint32_t end{};
                    if (JS_ToUint32(context, &start, arg(argc, argv, 0)) < 0 ||
                        JS_ToUint32(context, &end, arg(argc, argv, 1)) < 0) {
                        throw PendingJsException{};
                    }
                    const auto direction = arg(argc, argv, 2);
                    const bool backward = !JS_IsUndefined(direction) && string_value(context, direction) == "backward";
                    if (!selectable(n)) {
                        throw_dom("InvalidStateError", "The element's type does not support selection.");
                    }
                    const auto& value = n.field->value;
                    set_selection_range(n, static_cast<std::uint32_t>(byte_offset(value, start)),
                                        static_cast<std::uint32_t>(byte_offset(value, end)), backward);
                    return JS_UNDEFINED;
                }
                case SelectText:
                    if (!n.field) {
                        return JS_UNDEFINED;
                    }
                    set_selection_range(n, 0, static_cast<std::uint32_t>(n.field->value.size()), false);
                    // select() focuses the field and keeps the selection (Chrome's TextControlElement::select).
                    if (focusable(n)) {
                        move_focus(&n, FocusType::Script, true);
                    }
                    return JS_UNDEFINED;
                case SetRangeText: {
                    if (argc < 1 || argc == 2) {
                        JS_ThrowTypeError(context, "setRangeText: a replacement, or a replacement, start and end");
                        throw PendingJsException{};
                    }
                    std::string text = string_value(context, argv[0]);
                    std::optional<std::pair<std::uint32_t, std::uint32_t>> range;
                    std::string mode = "preserve";
                    if (argc >= 3) {
                        std::uint32_t start{};
                        std::uint32_t end{};
                        if (JS_ToUint32(context, &start, argv[1]) < 0 || JS_ToUint32(context, &end, argv[2]) < 0) {
                            throw PendingJsException{};
                        }
                        range.emplace(start, end);
                        if (argc >= 4 && !JS_IsUndefined(argv[3])) {
                            mode = string_value(context, argv[3]);
                            if (mode != "select" && mode != "start" && mode != "end" && mode != "preserve") {
                                JS_ThrowTypeError(context, "setRangeText: '%s' is not a SelectionMode", mode.c_str());
                                throw PendingJsException{};
                            }
                        }
                    }
                    if (!selectable(n)) {
                        throw_dom("InvalidStateError", "The element's type does not support selection.");
                    }
                    if (range && range->first > range->second) {
                        throw_dom("IndexSizeError", "The start is greater than the end.");
                    }
                    set_range_text(n, std::move(text), range, mode);
                    return JS_UNDEFINED;
                }
                default:
                    return JS_UNDEFINED;
            }
        }

        // HTML's set the selection range, as script sets one: start no later than end; a change fires select as a task
        // (Chrome's ScheduleSelectEvent).
        void set_selection_range(Node& n, std::uint32_t start, std::uint32_t end, bool backward) {
            auto next = field_copy(n);
            next->selection_end = boundary(next->value, end);
            next->selection_start = std::min(boundary(next->value, start), next->selection_end);
            next->backward = backward;
            if (store_field(n, std::move(next))) {
                queue_event(n, "select");
            }
        }

        // value = text by script: the value is dirty; when it changes the caret goes to its end and a composition ends
        // without events (Chrome's TextFieldInputType::SetValue).
        void set_value(Node& n, std::string text) {
            binding(n).field_dirty = true;
            text = sanitize_value(n, std::move(text));
            if (text == n.field->value) {
                return;
            }
            auto next = field_copy(n);
            next->value = std::move(text);
            next->selection_start = next->selection_end = static_cast<std::uint32_t>(next->value.size());
            next->backward = false;
            next->composing = false;
            store_field(n, std::move(next));
        }

        // HTML's setRangeText: [start, end) (without them, the selection) becomes text; the selection then selects it,
        // goes to its start or end, or stays where it was in the text around it (preserve). The value is dirty; select
        // follows as a task when the selection changed.
        void set_range_text(Node& n, std::string text, std::optional<std::pair<std::uint32_t, std::uint32_t>> range,
                            std::string_view mode) {
            auto next = field_copy(n);
            auto& f = *next;
            text = f.multiline ? line_feeds(std::move(text)) : text;
            if (!f.multiline) {
                std::erase_if(text, [](char c) {
                    return c == '\r' || c == '\n';
                });
            }
            std::uint32_t start = f.selection_start;
            std::uint32_t end = f.selection_end;
            if (range) {
                end = boundary(f.value, byte_offset(f.value, range->second));
                start = std::min(boundary(f.value, byte_offset(f.value, range->first)), end);
            }
            const auto old_start = f.selection_start;
            const auto old_end = f.selection_end;
            const bool old_backward = f.backward;
            f.value.replace(start, end - start, text);
            const auto new_end = static_cast<std::uint32_t>(start + text.size());
            const auto delta = static_cast<std::int64_t>(text.size()) - static_cast<std::int64_t>(end - start);
            std::uint32_t s = start;
            std::uint32_t e = new_end;
            if (mode == "start") {
                e = start;
            } else if (mode == "end") {
                s = new_end;
            } else if (mode == "preserve") {
                const auto keep = [&](std::uint32_t o, bool at_end) {
                    if (o > end) {
                        return static_cast<std::uint32_t>(o + delta);
                    }
                    return o > start ? (at_end ? new_end : start) : o;
                };
                s = keep(old_start, false);
                e = keep(old_end, true);
            }
            f.selection_start = s;
            f.selection_end = e;
            f.backward = false;
            binding(n).field_dirty = true;
            store_field(n, std::move(next));
            const auto& now = *n.field;
            if (now.selection_start != old_start || now.selection_end != old_end || now.backward != old_backward) {
                queue_event(n, "select");
            }
        }

        // As much of text as the field's maxlength lets replace [start, end) of its value, in whole characters (lengths
        // in UTF-16 code units, as HTML counts them).
        std::string fit_max_length(const Node& n, const TextFieldState& f, std::uint32_t start, std::uint32_t end,
                                   std::string text) const {
            const auto limit = integer_attribute(n, "maxlength");
            if (!limit || *limit < 0 || text.empty()) {
                return text;
            }
            const std::string_view value(f.value);
            const auto kept = utf16_length(value.substr(0, start)) + utf16_length(value.substr(end));
            auto room = static_cast<std::size_t>(*limit) > kept ? static_cast<std::size_t>(*limit) - kept : 0;
            std::size_t i = 0;
            while (i < text.size()) {
                const auto lead = static_cast<unsigned char>(text[i]);
                const std::size_t bytes = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                const std::size_t units = bytes == 4 ? 2 : 1;
                if (units > room) {
                    break;
                }
                room -= units;
                i += bytes;
            }
            text.resize(std::min(i, text.size()));
            return text;
        }

        // [start, end) of f, n's next state, becomes text with the caret after it, by the user: the value is dirty, and
        // change compares with what it was before, unless an edit since the last change remembered that already.
        void user_edit(Node& n, TextFieldState& f, std::uint32_t start, std::uint32_t end, std::string_view text) {
            auto& b = binding(n);
            if (!b.value_before_edit) {
                b.value_before_edit = n.field->value;
            }
            b.field_dirty = true;
            f.value.replace(start, end - start, text);
            f.selection_start = f.selection_end = static_cast<std::uint32_t>(start + text.size());
            f.backward = false;
        }

        // An edit the user makes (ScriptDom::edit_text_field).
        TextEditResult edit_field(Node& n, std::uint32_t start, std::uint32_t end, std::string text,
                                  std::string_view input_type, bool data) {
            Value hold(context, JS_DupValue(context, wrapper(n)));
            if (!dispatch_input_event(n, "beforeinput", input_type,
                                      data ? std::optional<std::string_view>(text) : std::nullopt, false, true)) {
                return TextEditResult::Prevented;
            }
            if (!n.field || !n.field->editable || !connected(n)) {
                return TextEditResult::Refused;
            }
            auto next = field_copy(n);
            auto& f = *next;
            end = boundary(f.value, end);
            start = std::min(boundary(f.value, start), end);
            text = fit_max_length(n, f, start, end, std::move(text));
            if (start == end && text.empty()) {
                return TextEditResult::Unchanged;
            }
            user_edit(n, f, start, end, text);
            f.composing = false;
            store_field(n, std::move(next));
            dispatch_input_event(n, "input", input_type, data ? std::optional<std::string_view>(text) : std::nullopt,
                                 false, false);
            return TextEditResult::Changed;
        }

        // The user selects from anchor to extent, its focus (ScriptDom::select_text_field).
        bool select_field(Node& n, std::uint32_t anchor, std::uint32_t extent, bool select_event) {
            if (!n.field) {
                return false;
            }
            auto next = field_copy(n);
            anchor = boundary(next->value, anchor);
            extent = boundary(next->value, extent);
            next->selection_start = std::min(anchor, extent);
            next->selection_end = std::max(anchor, extent);
            next->backward = extent < anchor;
            const bool moved = store_field(n, std::move(next));
            if (moved && select_event && anchor != extent) {
                dispatch_plain_event(n, "select");
            }
            return moved;
        }

        // Selects the composition, as Chrome's InputMethodController does before it replaces it.
        void select_composition(Node& n) {
            auto next = field_copy(n);
            next->selection_start = next->composition_start;
            next->selection_end = next->composition_end;
            next->backward = false;
            store_field(n, std::move(next));
        }

        // The input method composes (ScriptDom::compose_text_field).
        void compose_field(Node& n, std::string text, std::uint32_t selection_start, std::uint32_t selection_end,
                           std::uint32_t clause_start, std::uint32_t clause_end) {
            Value hold(context, JS_DupValue(context, wrapper(n)));
            if (!n.field->composing) {
                if (text.empty()) {
                    return;
                }
                const auto& f = *n.field;
                const std::string selected = f.value.substr(f.selection_start, f.selection_end - f.selection_start);
                dispatch_composition_event(n, "compositionstart", selected);
                if (!n.field || !connected(n)) {
                    return;
                }
                auto next = field_copy(n);
                next->composing = true;
                next->composition_start = next->selection_start;
                next->composition_end = next->selection_end;
                store_field(n, std::move(next));
            } else {
                select_composition(n);
            }
            dispatch_composition_event(n, "compositionupdate", text);
            dispatch_input_event(n, "beforeinput", "insertCompositionText", text, true, false);
            if (!n.field || !n.field->composing || !connected(n)) {
                return;
            }
            auto next = field_copy(n);
            auto& f = *next;
            const auto start = f.composition_start;
            user_edit(n, f, start, f.composition_end, text);
            const bool cancel = text.empty();
            f.composing = !cancel;
            f.composition_end = static_cast<std::uint32_t>(start + text.size());
            const auto a = boundary(text, selection_start);
            const auto b = boundary(text, selection_end);
            f.selection_start = start + std::min(a, b);
            f.selection_end = start + std::max(a, b);
            f.clause_start = start + boundary(text, clause_start);
            f.clause_end = start + boundary(text, clause_end);
            store_field(n, std::move(next));
            dispatch_input_event(n, "input", "insertCompositionText",
                                 cancel ? std::nullopt : std::optional<std::string_view>(text), true, false);
            if (cancel) {
                dispatch_composition_event(n, "compositionend", "");
            }
        }

        // The input method commits (ScriptDom::commit_text_field).
        void commit_field(Node& n, std::string text) {
            Value hold(context, JS_DupValue(context, wrapper(n)));
            if (!n.field->composing) {
                if (!text.empty()) {
                    const auto& f = *n.field;
                    edit_field(n, f.selection_start, f.selection_end, std::move(text), "insertText", true);
                }
                return;
            }
            select_composition(n);
            dispatch_composition_event(n, "compositionupdate", text);
            dispatch_input_event(n, "beforeinput", "insertCompositionText", text, true, false);
            if (!n.field || !n.field->composing || !connected(n)) {
                return;
            }
            auto next = field_copy(n);
            auto& f = *next;
            text = fit_max_length(n, f, f.composition_start, f.composition_end, std::move(text));
            user_edit(n, f, f.composition_start, f.composition_end, text);
            f.composing = false;
            store_field(n, std::move(next));
            dispatch_input_event(n, "input", "insertCompositionText",
                                 text.empty() ? std::nullopt : std::optional<std::string_view>(text), true, false);
            dispatch_composition_event(n, "compositionend", text);
        }

        // The focus leaves a composition: it ends where it is and its text stays (Chrome's FinishComposingText).
        void finish_composition(Node& n) {
            if (!n.field || !n.field->composing) {
                return;
            }
            auto next = field_copy(n);
            const std::string text =
                next->value.substr(next->composition_start, next->composition_end - next->composition_start);
            next->composing = false;
            store_field(n, std::move(next));
            dispatch_composition_event(n, "compositionend", text);
        }

        // Chrome's DispatchFormControlChangeEvent: change when the value differs from what it was before the user's
        // first edit since the last change.
        void commit_change(Node& n) {
            auto& b = binding(n);
            if (!b.value_before_edit) {
                return;
            }
            const bool differs = n.field && *b.value_before_edit != n.field->value;
            b.value_before_edit.reset();
            if (differs) {
                dispatch_plain_event(n, "change");
            }
        }

        // A text field of the document.
        Node* field_node(NodeIdentity element) const {
            auto* n = resolve(element);
            return n && connected(*n) && n->field ? n : nullptr;
        }

        // The user sets the whole value (ScriptDom::set_text_field_value).
        void set_field_value_for_user(Node& n, std::string text) {
            text = sanitize_value(n, std::move(text));
            if (text == n.field->value) {
                return;
            }
            auto next = field_copy(n);
            user_edit(n, *next, 0, static_cast<std::uint32_t>(next->value.size()), text);
            next->composing = false;
            store_field(n, std::move(next));
            dispatch_plain_event(n, "input");
        }

        // What a field shows that its page does not see (ScriptDom::set_text_field_view); its text scrolling fires
        // scroll.
        void set_field_view(Node& n, bool caret_on, float scroll_x, float scroll_y) {
            const auto& f = *n.field;
            const bool scrolled = f.scroll_x != scroll_x || f.scroll_y != scroll_y;
            if (f.caret_on == caret_on && !scrolled) {
                return;
            }
            auto next = field_copy(n);
            next->caret_on = caret_on;
            next->scroll_x = scroll_x;
            next->scroll_y = scroll_y;
            store_field(n, std::move(next));
            if (scrolled) {
                queue_scroll(n);
            }
        }

        // document.__ceStorage(area, op, key, value) of the web platform: area 0 is localStorage, 1 sessionStorage;
        // op 0 getItem, 1 setItem (false past the quota), 2 removeItem, 3 clear, 4 key(index), 5 length, 6 the keys.
        JSValue storage_call(int argc, JSValueConst* argv) {
            double area{};
            double op{};
            if (JS_ToFloat64(context, &area, arg(argc, argv, 0)) < 0 ||
                JS_ToFloat64(context, &op, arg(argc, argv, 1)) < 0) {
                throw PendingJsException{};
            }
            if (area != 0 && area != 1) {
                throw std::runtime_error("WEFT_STORAGE: an area");
            }
            auto& s = storage[std::size_t(area)];
            if (!s) {
                throw std::runtime_error("WEFT_STORAGE: storage is not available");
            }
            const auto text = [&](std::optional<std::string_view> v) {
                return v ? JS_NewStringLen(context, v->data(), v->size()) : JS_NULL;
            };
            switch (int(op)) {
                case 0:
                    return text(s->get(string_value(context, arg(argc, argv, 2))));
                case 1:
                    return JS_NewBool(context, s->set(string_value(context, arg(argc, argv, 2)),
                                                      string_value(context, arg(argc, argv, 3))));
                case 2:
                    s->remove(string_value(context, arg(argc, argv, 2)));
                    return JS_UNDEFINED;
                case 3:
                    s->clear();
                    return JS_UNDEFINED;
                case 4: {
                    double index{};
                    if (JS_ToFloat64(context, &index, arg(argc, argv, 2)) < 0) {
                        throw PendingJsException{};
                    }
                    if (!(index >= 0) || index != std::floor(index)) {
                        return JS_NULL;
                    }
                    return text(s->key(std::size_t(std::min(index, double(s->size())))));
                }
                case 5:
                    return JS_NewFloat64(context, double(s->size()));
                case 6: {
                    Value keys(context, JS_NewArray(context));
                    std::uint32_t i{};
                    for (const auto& item : s->items()) {
                        JS_SetPropertyUint32(context, keys.value, i++,
                                             JS_NewStringLen(context, item.first.data(), item.first.size()));
                    }
                    return JS_DupValue(context, keys.value);
                }
                default:
                    throw std::runtime_error("WEFT_STORAGE: an operation");
            }
        }

        bool connected(const Node& target) const {
            return tree.connected(target);
        }

        Node* resolve(NodeIdentity id) const {
            return tree.find(id);
        }

        std::string dump(const Node& n) const {
            return tree.dump(n);
        }

        DomSnapshot snapshot(const Node& n) const {
            return tree.snapshot(n);
        }
    };

    ScriptDom::ScriptDom(ScriptConfig config) : impl_(std::make_unique<Impl>(config)) {
    }

    ScriptDom::~ScriptDom() = default;

    void ScriptDom::install_layout_host(LayoutHost host) {
        impl_->require_runtime();
        impl_->layout_host = std::move(host);
        impl_->clear_host_reads();
    }

    void ScriptDom::install_computed_style_host(ComputedStyleHost host) {
        impl_->require_runtime();
        impl_->computed_style_host = std::move(host);
        impl_->clear_host_reads();
    }

    void ScriptDom::install_engine(std::shared_ptr<bridge::EngineChannel> channel) {
        impl_->check_owner();
        impl_->engine = std::move(channel);
    }

    void ScriptDom::install_engine_function(std::string name, EngineFunction function) {
        impl_->check_owner();
        Impl::engine_name(name);
        impl_->engine_functions[std::move(name)] = std::move(function);
    }

    std::vector<EngineMessage> ScriptDom::take_engine_messages() {
        impl_->check_owner();
        impl_->engine_message_bytes = 0;
        return std::exchange(impl_->engine_messages, {});
    }

    EngineDelivery ScriptDom::deliver_engine() {
        return impl_->deliver_engine();
    }

    void ScriptDom::install_particle_host(ParticleHost host) {
        impl_->require_runtime();
        impl_->particle_host = std::move(host);
    }

    void ScriptDom::install_layout_cache_host(LayoutEpochHost host) {
        impl_->require_runtime();
        impl_->layout_epoch_host = std::move(host);
        impl_->clear_host_reads();
    }

    void ScriptDom::install_integer_format_host(IntegerFormatHost host) {
        impl_->require_runtime();
        impl_->integer_format_host = std::move(host);
    }

    void ScriptDom::install_console_host(ConsoleHost host) {
        impl_->require_runtime();
        impl_->console_host = std::move(host);
    }

    void ScriptDom::install_storage(std::shared_ptr<WebStorage> local, std::shared_ptr<WebStorage> session) {
        impl_->require_runtime();
        impl_->storage = {std::move(local), std::move(session)};
    }

    void ScriptDom::install_media_host(MediaHost host) {
        impl_->require_runtime();
        impl_->media_host = std::move(host);
    }

    std::optional<NodeIdentity> ScriptDom::focused() const {
        impl_->check_owner();
        auto* n = impl_->resolve(impl_->focus);
        if (!n || !impl_->connected(*n)) {
            return std::nullopt;
        }
        return n->identity;
    }

    bool ScriptDom::focus(std::optional<NodeIdentity> target, FocusType type, bool scroll) {
        impl_->require_runtime();
        Impl::Node* to = nullptr;
        if (target) {
            to = impl_->resolve(*target);
            if (!to || !impl_->connected(*to) || to->type != constants::element_node_type) {
                return false;
            }
        }
        bool moved{};
        impl_->execute([&] {
            moved = impl_->move_focus(to, type, scroll);
            return JS_UNDEFINED;
        });
        return moved;
    }

    bool ScriptDom::focusable(NodeIdentity target) {
        impl_->require_runtime();
        auto* n = impl_->resolve(target);
        return n && impl_->focusable(*n);
    }

    void ScriptDom::install_focus_host(FocusHost host) {
        impl_->require_runtime();
        impl_->focus_host = std::move(host);
    }

    void ScriptDom::set_spatial_navigation(bool on) {
        impl_->check_owner();
        impl_->spatial_navigation = on;
    }

    bool ScriptDom::spatial_navigation_listener(NodeIdentity target) const {
        impl_->check_owner();
        const auto* n = impl_->resolve(target);
        return n && n->type == constants::element_node_type && impl_->spatial_listener(*n);
    }

    void ScriptDom::set_page_focus(bool focused) {
        impl_->require_runtime();
        impl_->execute([&] {
            impl_->set_page_focus(focused);
            return JS_UNDEFINED;
        });
    }

    bool ScriptDom::page_focused() const {
        impl_->check_owner();
        return impl_->page_focus;
    }

    void ScriptDom::set_gamepad(const GamepadState& state, double time_ms) {
        impl_->require_runtime();
        impl_->execute([&] {
            impl_->gamepad(state, time_ms);
            return JS_UNDEFINED;
        });
    }

    void ScriptDom::set_pressed(std::optional<NodeIdentity> node) {
        impl_->check_owner();
        impl_->pressed_node = node.value_or(NodeIdentity{});
        // Sequential navigation starts from where the mouse went down (Blink's MouseEventManager).
        if (node) {
            impl_->start_node = *node;
            impl_->start_parent = impl_->start_next = {};
        }
    }

    std::optional<NodeIdentity> ScriptDom::pressed() const {
        impl_->check_owner();
        auto* n = impl_->resolve(impl_->pressed_node);
        if (!n || !impl_->connected(*n)) {
            return std::nullopt;
        }
        return n->identity;
    }

    std::optional<NodeIdentity> ScriptDom::navigation_start(bool forward) const {
        impl_->check_owner();
        return impl_->navigation_start(forward);
    }

    void ScriptDom::set_key_active(std::optional<NodeIdentity> target) {
        impl_->check_owner();
        impl_->set_key_active(target ? impl_->resolve(*target) : nullptr);
    }

    std::optional<NodeIdentity> ScriptDom::key_active() const {
        impl_->check_owner();
        auto* n = impl_->resolve(impl_->key_active);
        if (!n || !n->active) {
            return std::nullopt;
        }
        return n->identity;
    }

    DispatchResult ScriptDom::dispatch_key(NodeIdentity identity, const KeyEvent& input) {
        impl_->require_runtime();
        if (!std::isfinite(input.time_ms) || input.time_ms < 0 || input.key.size() > constants::input::max_key_bytes ||
            input.code.size() > constants::input::max_key_bytes || input.modifiers > key_modifier::all ||
            input.location > 3) {
            throw std::invalid_argument("WEFT_KEY_PROFILE: a key and code of at most 64 bytes, modifiers, a location");
        }
        auto* target = impl_->resolve(identity);
        if (!target || !impl_->connected(*target)) {
            return DispatchResult::StaleTarget;
        }
        if (input.type == KeyType::Down &&
            !(input.modifiers & (key_modifier::control | key_modifier::alt | key_modifier::meta))) {
            impl_->note_keyboard_event();
        }
        const char* type = input.type == KeyType::Down ? "keydown" : input.type == KeyType::Press ? "keypress" : "keyup";
        const auto result = impl_->execute([&] {
            auto* c = impl_->context;
            Value pinned(c, JS_DupValue(c, impl_->wrapper(*target)));
            Value event(c, JS_NewObjectProto(c, impl_->keyboard_prototype));
            if (JS_IsException(event.value)) {
                throw PendingJsException{};
            }
            auto property = [&](const char* key, JSValue value) {
                impl_->event_property(event.value, key, value);
            };
            property("type", JS_NewString(c, type));
            property("bubbles", JS_TRUE);
            property("cancelable", JS_TRUE);
            property("composed", JS_TRUE);
            property("defaultPrevented", JS_FALSE);
            property("currentTarget", JS_NULL);
            property("eventPhase", JS_NewInt32(c, constants::script::event_phase_none));
            property("view", JS_GetGlobalObject(c));
            property("detail", JS_NewInt32(c, 0));
            property("key", JS_NewStringLen(c, input.key.data(), input.key.size()));
            property("code", JS_NewStringLen(c, input.code.data(), input.code.size()));
            property("location", JS_NewUint32(c, input.location));
            property("repeat", JS_NewBool(c, input.repeat));
            property("isComposing", JS_NewBool(c, input.composing));
            property("ctrlKey", JS_NewBool(c, input.modifiers & key_modifier::control));
            property("shiftKey", JS_NewBool(c, input.modifiers & key_modifier::shift));
            property("altKey", JS_NewBool(c, input.modifiers & key_modifier::alt));
            property("metaKey", JS_NewBool(c, input.modifiers & key_modifier::meta));
            property("keyCode", JS_NewUint32(c, input.key_code));
            property("which", JS_NewUint32(c, input.key_code));
            property("charCode", JS_NewUint32(c, input.char_code));
            property("timeStamp", JS_NewFloat64(c, input.time_ms));
            if (JS_DefinePropertyValueStr(c, event.value, "__modifiers", JS_NewUint32(c, input.modifiers), 0) < 0 ||
                JS_DefinePropertyValueStr(c, event.value, "isTrusted", JS_TRUE, 0) < 0) {
                throw PendingJsException{};
            }
            if (!impl_->connected(*target)) {
                return JS_FALSE;
            }
            return impl_->dispatch(*target, event.value);
        });
        return result == "true" ? DispatchResult::DefaultAllowed : DispatchResult::DefaultPrevented;
    }

    bool ScriptDom::scroll_by_key(NodeIdentity start, ScrollDirection direction, ScrollGranularity granularity) {
        impl_->require_runtime();
        auto* n = impl_->resolve(start);
        return n && impl_->scroll_by_key(*n, direction, granularity);
    }

    void ScriptDom::scroll_into_view(NodeIdentity target, ScrollAlign block, ScrollAlign inline_align) {
        impl_->require_runtime();
        if (auto* n = impl_->resolve(target)) {
            impl_->scroll_into_view(*n, block, inline_align);
        }
    }

    void ScriptDom::set_modern_selectors(bool modern) {
        impl_->check_owner();
        impl_->modern_selectors = modern;
    }

    DispatchResult ScriptDom::dispatch_wheel(NodeIdentity identity, double dx, double dy) {
        return dispatch_wheel(identity, WheelEvent{dx, dy});
    }

    DispatchResult ScriptDom::dispatch_wheel(NodeIdentity identity, const WheelEvent& input) {
        impl_->require_runtime();
        const auto dx = input.delta_x;
        const auto dy = input.delta_y;
        for (double v : {dx, dy, input.client_x, input.client_y, input.screen_x, input.screen_y, input.time_ms}) {
            if (!std::isfinite(v)) {
                throw std::invalid_argument("WEFT_WHEEL_PROFILE: finite input values");
            }
        }
        if (std::abs(dx) > constants::script::max_logical_pixel_magnitude ||
            std::abs(dy) > constants::script::max_logical_pixel_magnitude || input.time_ms < 0) {
            throw std::invalid_argument("WEFT_WHEEL_PROFILE: bounded pixel deltas/time");
        }
        auto* target = impl_->resolve(identity);
        if (!target || !impl_->connected(*target)) {
            return DispatchResult::StaleTarget;
        }
        const auto result = impl_->execute([&] {
            auto* c = impl_->context;
            Value pinned(c, JS_DupValue(c, impl_->wrapper(*target)));
            Value e(c, JS_NewObjectProto(c, impl_->wheel_prototype));
            auto property = [&](const char* k, JSValue v) {
                impl_->event_property(e.value, k, v);
            };
            property("type", JS_NewString(c, "wheel"));
            property("bubbles", JS_TRUE);
            property("cancelable", JS_TRUE);
            property("defaultPrevented", JS_FALSE);
            property("deltaX", JS_NewFloat64(c, dx));
            property("deltaY", JS_NewFloat64(c, dy));
            property("deltaZ", JS_NewInt32(c, 0));
            property("deltaMode", JS_NewInt32(c, constants::script::wheel_delta_mode_pixel));
            property("view", JS_GetGlobalObject(c));
            property("button", JS_NewInt32(c, 0));
            property("buttons", JS_NewInt32(c, 0));
            property("clientX", JS_NewFloat64(c, input.client_x));
            property("clientY", JS_NewFloat64(c, input.client_y));
            property("screenX", JS_NewFloat64(c, input.screen_x));
            property("screenY", JS_NewFloat64(c, input.screen_y));
            property("timeStamp", JS_NewFloat64(c, input.time_ms));
            property("detail", JS_NewInt32(c, 0));
            property("ctrlKey", JS_NewBool(c, input.ctrl));
            property("shiftKey", JS_NewBool(c, input.shift));
            property("altKey", JS_NewBool(c, input.alt));
            property("metaKey", JS_NewBool(c, input.meta));
            property("relatedTarget", JS_NULL);
            if (JS_DefinePropertyValueStr(c, e.value, "isTrusted", JS_TRUE, 0) < 0) {
                throw PendingJsException{};
            }
            return impl_->dispatch(*target, e.value);
        });
        if (result != "true") {
            return DispatchResult::DefaultPrevented;
        }
        // Default action revalidates after listener/microtask mutations. At a limit
        // the remaining axis chains to an ancestor; hidden scrollers ignore wheel.
        impl_->execute([&] {
            auto* n = impl_->resolve(identity);
            if (!n || !impl_->connected(*n)) {
                return JS_UNDEFINED;
            }
            auto geometry = impl_->refresh_geometry();
            bool x_done = dx == 0;
            bool y_done = dy == 0;
            for (; n && (!x_done || !y_done); n = impl_->parent_of(*n)) {
                auto it = std::find_if(geometry.begin(), geometry.end(), [&](const auto& g) {
                    return g.identity == n->identity;
                });
                if (it == geometry.end()) {
                    continue;
                }
                bool changed{};
                if (!x_done && it->wheel_x) {
                    auto v = std::clamp(it->scroll_left + dx, 0., std::max(0., it->scroll_width - it->client_width));
                    if (v != n->scroll_left) {
                        n->scroll_left = v;
                        x_done = changed = true;
                    }
                }
                if (!y_done && it->wheel_y) {
                    auto v = std::clamp(it->scroll_top + dy, 0., std::max(0., it->scroll_height - it->client_height));
                    if (v != n->scroll_top) {
                        n->scroll_top = v;
                        y_done = changed = true;
                    }
                }
                if (changed) {
                    impl_->changed(*n);
                    impl_->queue_scroll(*n);
                }
            }
            return JS_UNDEFINED;
        });
        return DispatchResult::DefaultAllowed;
    }

    DispatchResult ScriptDom::dispatch_mouse(NodeIdentity identity, const MouseEvent& input) {
        impl_->require_runtime();
        if (!std::isfinite(input.client_x) || !std::isfinite(input.client_y) || !std::isfinite(input.screen_x) ||
            !std::isfinite(input.screen_y) || !std::isfinite(input.time_ms) || input.time_ms < 0 ||
            input.buttons > constants::script::primary_mouse_buttons_mask) {
            throw std::invalid_argument("WEFT_MOUSE_PROFILE: finite coordinates/time and primary button required");
        }
        auto* target = impl_->resolve(identity);
        if (!target || !impl_->connected(*target)) {
            return DispatchResult::StaleTarget;
        }
        const bool over = input.type == MouseType::Over;
        const bool out = input.type == MouseType::Out;
        const bool down = input.type == MouseType::Down;
        const bool up = input.type == MouseType::Up;
        if (over || out || down || up) {
            auto& tracked = (over || out) ? impl_->hovered_nodes : impl_->active_nodes;
            auto ancestor = [&](Impl::Node* p, const Impl::Node* n) {
                for (; p; p = impl_->parent_of(*p)) {
                    if (p == n) {
                        return true;
                    }
                }
                return false;
            };
            // Weak generational identities, including detached nodes still alive.
            // State updates touch the old/new paths instead of every live slot.
            std::erase_if(tracked, [&](auto id) {
                auto* n = impl_->resolve(id);
                if (!n) {
                    return true;
                }
                bool& flag = (over || out) ? n->hovered : n->active;
                if (up || (out && !ancestor(impl_->resolve(input.related_target), n))) {
                    if (flag) {
                        flag = false;
                        impl_->changed(*n);
                    }
                    return true;
                }
                return !flag;
            });
            if (over || down) {
                for (auto* n = target; n; n = impl_->parent_of(*n)) {
                    bool& flag = over ? n->hovered : n->active;
                    if (!flag) {
                        flag = true;
                        impl_->changed(*n);
                        tracked.push_back(n->identity);
                    }
                }
            }
        }
        const char* type{};
        switch (input.type) {
            case MouseType::Down:
                type = "mousedown";
                break;
            case MouseType::Up:
                type = "mouseup";
                break;
            case MouseType::Move:
                type = "mousemove";
                break;
            case MouseType::Click:
                type = "click";
                break;
            case MouseType::Over:
                type = "mouseover";
                break;
            case MouseType::Out:
                type = "mouseout";
                break;
            case MouseType::Enter:
                type = "mouseenter";
                break;
            case MouseType::Leave:
                type = "mouseleave";
                break;
            case MouseType::PointerDown:
                type = "pointerdown";
                break;
            case MouseType::PointerUp:
                type = "pointerup";
                break;
            case MouseType::PointerMove:
                type = "pointermove";
                break;
            case MouseType::PointerOver:
                type = "pointerover";
                break;
            case MouseType::PointerOut:
                type = "pointerout";
                break;
            case MouseType::PointerEnter:
                type = "pointerenter";
                break;
            case MouseType::PointerLeave:
                type = "pointerleave";
                break;
            case MouseType::PointerCancel:
                type = "pointercancel";
                break;
            case MouseType::DblClick:
                type = "dblclick";
                break;
            default:
                throw std::invalid_argument("WEFT_MOUSE_PROFILE: unsupported event type");
        }
        // As in Chrome: click is a PointerEvent too; the pointer events' positions are fractional and the mouse
        // events' (click's) whole CSS px; detail counts the clicks of down, up and click, and is 0 for the others. A
        // simulated click (the keyboard's) has pointerId -1, no pointer type, no position and detail 0.
        const bool pointer = std::string_view(type).starts_with("pointer");
        const bool counted = down || up || input.type == MouseType::Click || input.type == MouseType::DblClick;
        const bool simulated = input.simulated && input.type == MouseType::Click;
        const auto position = [&](double v) {
            return simulated ? 0.0 : pointer ? v : std::floor(v);
        };
        const auto result = impl_->execute([&] {
            auto* c = impl_->context;
            Value pinned(c, JS_DupValue(c, impl_->wrapper(*target)));
            Value prototype(c, JS_DupValue(c, impl_->mouse_prototype));
            if (pointer || input.type == MouseType::Click) {
                Value global(c, JS_GetGlobalObject(c));
                Value constructor(c, JS_GetPropertyStr(c, global.value, "PointerEvent"));
                if (JS_IsObject(constructor.value)) {
                    Value p(c, JS_GetPropertyStr(c, constructor.value, "prototype"));
                    if (JS_IsObject(p.value)) {
                        JS_FreeValue(c, prototype.value);
                        prototype.value = JS_DupValue(c, p.value);
                    }
                }
            }
            Value event(c, JS_NewObjectProto(c, prototype.value));
            auto property = [&](const char* key, JSValue value) {
                impl_->event_property(event.value, key, value);
            };
            property("type", JS_NewString(c, type));
            property("bubbles",
                     JS_NewBool(c, input.type != MouseType::Enter && input.type != MouseType::Leave &&
                                       input.type != MouseType::PointerEnter && input.type != MouseType::PointerLeave));
            property("cancelable", JS_TRUE);
            if (pointer || input.type == MouseType::Click) {
                property("pointerId", JS_NewInt32(c, simulated ? -1 : constants::script::mouse_pointer_id));
                property("pointerType", JS_NewString(c, simulated ? "" : "mouse"));
                property("isPrimary", JS_NewBool(c, !simulated));
            }
            property("defaultPrevented", JS_FALSE);
            property("currentTarget", JS_NULL);
            property("eventPhase", JS_NewInt32(c, constants::script::event_phase_none));
            property("view", JS_GetGlobalObject(c));
            const auto* related = impl_->resolve(input.related_target);
            property("relatedTarget", related ? JS_DupValue(c, impl_->wrapper(*related)) : JS_NULL);
            property("detail", JS_NewUint32(c, counted && !pointer && !simulated ? std::max(1u, input.clicks) : 0));
            property("button", JS_NewInt32(c, 0));
            property("buttons", JS_NewUint32(c, simulated ? 0 : input.buttons));
            property("clientX", JS_NewFloat64(c, position(input.client_x)));
            property("clientY", JS_NewFloat64(c, position(input.client_y)));
            property("screenX", JS_NewFloat64(c, position(input.screen_x)));
            property("screenY", JS_NewFloat64(c, position(input.screen_y)));
            property("timeStamp", JS_NewFloat64(c, input.time_ms));
            property("ctrlKey", JS_NewBool(c, input.ctrl));
            property("shiftKey", JS_NewBool(c, input.shift));
            property("altKey", JS_NewBool(c, input.alt));
            property("metaKey", JS_NewBool(c, input.meta));
            if (JS_DefinePropertyValueStr(c, event.value, "isTrusted", JS_TRUE, 0) < 0) {
                throw PendingJsException{};
            }
            if (!impl_->connected(*target)) {
                return JS_FALSE;
            }
            return impl_->dispatch(*target, event.value);
        });
        return result == "true" ? DispatchResult::DefaultAllowed : DispatchResult::DefaultPrevented;
    }

    std::string ScriptDom::eval(std::string_view source, std::string_view filename) {
        return impl_->eval(source, filename);
    }

    AnimationFrameResult ScriptDom::animation_frame(double time_ms, bool drawable) {
        return impl_->animation_frame(time_ms, drawable);
    }

    void ScriptDom::advance_clock(double time_ms) {
        impl_->advance_clock(time_ms);
    }

    TaskResult ScriptDom::pump_tasks(double time_ms) {
        return impl_->pump_tasks(time_ms);
    }

    void ScriptDom::install_canvas_host(CanvasFactory factory) {
        impl_->install_canvas(std::move(factory));
    }

    CanvasSnapshot ScriptDom::canvas_snapshot(NodeIdentity identity) {
        impl_->require_runtime();
        auto* n = impl_->resolve(identity);
        auto* entry = n ? impl_->canvases.find(n->identity) : nullptr;
        if (!entry) {
            throw std::runtime_error("WEFT_CANVAS_SNAPSHOT: stale node or missing 2d context");
        }
        try {
            auto& s = *entry->canvas;
            return {n->identity, s.width, s.height, s.version, s.commands, s.fills, s.alpha, s.pixels()};
        } catch (...) {
            impl_->faulted = true;
            throw;
        }
    }

    void ScriptDom::collect_garbage() {
        impl_->check_owner();
        if (impl_->runtime) {
            JS_RunGC(impl_->runtime);
        }
        ++impl_->counters.gc_runs;
    }

    ScriptStats ScriptDom::stats() const {
        impl_->check_owner();
        auto result = impl_->counters;
        result.native_roots = impl_->native_roots.size() + (impl_->context ? 1 : 0);
        result.dom_mutations = impl_->tree.mutations();
        result.snapshot_nodes = impl_->tree.snapshot_nodes();
        result.vm_allocated_bytes = impl_->vm_bytes;
        result.canvas_contexts = impl_->canvas_budget->contexts;
        result.canvas_backing_bytes = impl_->canvas_budget->bytes;
        result.peak_canvas_backing_bytes = impl_->canvas_budget->peak_bytes;
        return result;
    }

    std::uint64_t ScriptDom::epoch() const {
        impl_->check_owner();
        return impl_->view_epoch;
    }

    void ScriptDom::reset_document() {
        impl_->check_owner();
        if (impl_->view_epoch == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("WEFT_EPOCH_EXHAUSTED: View epoch cannot wrap");
        }
        impl_->shutdown();
        ++impl_->view_epoch;
        impl_->faulted = false;
        impl_->fault_limit = Impl::Limit::None;
        // The page's calls are gone with it, and what waited for it.
        if (impl_->engine) {
            impl_->engine->reset();
        }
        impl_->engine_messages.clear();
        impl_->engine_message_bytes = 0;
        try {
            impl_->initialize();
        } catch (const PendingJsException&) {
            impl_->faulted = true;
            throw std::runtime_error("WEFT_INITIALIZATION_FAILED: VM allocation or bootstrap exception");
        } catch (...) {
            impl_->faulted = true;
            throw;
        }
    }

    NodeIdentity ScriptDom::node_identity(std::string_view selector) const {
        impl_->require_runtime();
        std::vector<Impl::Node*> found;
        impl_->query(Impl::node(impl_->context, impl_->document_root), selector, found, true);
        if (found.empty()) {
            throw std::runtime_error("WEFT_NOT_FOUND: no node for selector " + std::string(selector));
        }
        return found.front()->identity;
    }

    bool ScriptDom::is_live(NodeIdentity identity) const {
        impl_->check_owner();
        return impl_->resolve(identity) != nullptr;
    }

    std::uint64_t ScriptDom::retain_node(NodeIdentity identity) {
        impl_->require_runtime();
        auto* node = impl_->resolve(identity);
        if (!node) {
            throw std::runtime_error("WEFT_STALE_NODE: native root identity is invalid");
        }
        if (impl_->next_root == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("WEFT_ROOT_EXHAUSTED: native root token cannot wrap");
        }
        const auto token = impl_->next_root++;
        auto [it, inserted] = impl_->native_roots.emplace(token, JS_UNDEFINED);
        (void)inserted;
        it->second = JS_DupValue(impl_->context, impl_->wrapper(*node));
        return token;
    }

    void ScriptDom::release_node(std::uint64_t root_token) {
        impl_->check_owner();
        auto it = impl_->native_roots.find(root_token);
        if (it == impl_->native_roots.end()) {
            throw std::runtime_error("WEFT_STALE_ROOT: native root token is invalid");
        }
        JS_FreeValue(impl_->context, it->second);
        impl_->native_roots.erase(it);
    }

    void ScriptDom::queue_callback(std::uint64_t callback_epoch, std::string source) {
        impl_->check_owner();
        impl_->completions.push_back({callback_epoch, std::move(source)});
    }

    std::size_t ScriptDom::drain_callbacks() {
        impl_->check_owner();
        std::size_t executed{};
        // Snapshot the batch: callbacks cannot create an unbounded same-drain chain.
        const auto size = impl_->completions.size();
        for (std::size_t i{}; i < size; ++i) {
            auto completion = std::move(impl_->completions.front());
            impl_->completions.pop_front();
            if (completion.epoch != impl_->view_epoch) {
                ++impl_->counters.rejected_callbacks;
                continue;
            }
            impl_->eval(completion.source, "<epoch-callback>");
            ++executed;
        }
        return executed;
    }

    std::string ScriptDom::dump_document() const {
        impl_->require_runtime();
        return impl_->dump(Impl::node(impl_->context, impl_->document_root));
    }

    DomSnapshot ScriptDom::snapshot_subtree(std::string_view selector) const {
        impl_->require_runtime();
        std::vector<Impl::Node*> found;
        impl_->query(Impl::node(impl_->context, impl_->document_root), selector, found, true);
        if (found.empty()) {
            throw std::runtime_error("WEFT_NOT_FOUND: no snapshot root");
        }
        return impl_->snapshot(*found.front());
    }

    bool ScriptDom::refresh_snapshot(NodeIdentity id, DomSnapshot& snapshot) const {
        impl_->require_runtime();
        auto* node = impl_->resolve(id);
        if (!node) {
            throw std::runtime_error("WEFT_STALE_NODE: snapshot root");
        }
        return impl_->tree.refresh(*node, snapshot);
    }

    DomSnapshot ScriptDom::snapshot_subtree(NodeIdentity id) const {
        impl_->require_runtime();
        auto* node = impl_->resolve(id);
        if (!node) {
            throw std::runtime_error("WEFT_STALE_NODE: snapshot root");
        }
        return impl_->snapshot(*node);
    }

    std::uint64_t ScriptDom::subtree_revision(NodeIdentity id) const {
        impl_->require_runtime();
        auto* node = impl_->resolve(id);
        if (!node) {
            throw std::runtime_error("WEFT_STALE_NODE: revision root");
        }
        return node->subtree_revision;
    }

    DomChanges ScriptDom::changes_since(std::uint64_t revision) const {
        impl_->require_runtime();
        return impl_->tree.changes_since(revision);
    }

    namespace {
        void check_text(std::string_view text) {
            if (text.size() > constants::input::max_text_bytes || !bridge::utf8(text)) {
                throw std::invalid_argument("WEFT_TEXT_FIELD: UTF-8 text of at most 4096 bytes");
            }
        }
    } // namespace

    std::shared_ptr<const TextFieldState> ScriptDom::text_field(NodeIdentity element) const {
        impl_->check_owner();
        const auto* n = impl_->resolve(element);
        return n ? n->field : nullptr;
    }

    TextEditResult ScriptDom::edit_text_field(NodeIdentity element, std::uint32_t start, std::uint32_t end,
                                              std::string_view text, std::string_view input_type, bool data) {
        impl_->require_runtime();
        check_text(text);
        auto* n = impl_->field_node(element);
        if (!n) {
            return TextEditResult::Refused;
        }
        auto result = TextEditResult::Refused;
        impl_->execute([&] {
            result = impl_->edit_field(*n, start, end, std::string(text), input_type, data);
            return JS_UNDEFINED;
        });
        return result;
    }

    bool ScriptDom::select_text_field(NodeIdentity element, std::uint32_t anchor, std::uint32_t focus,
                                      bool select_event) {
        impl_->require_runtime();
        auto* n = impl_->field_node(element);
        if (!n) {
            return false;
        }
        bool moved{};
        impl_->execute([&] {
            moved = impl_->select_field(*n, anchor, focus, select_event);
            return JS_UNDEFINED;
        });
        return moved;
    }

    void ScriptDom::dispatch_select(NodeIdentity element) {
        impl_->require_runtime();
        if (auto* n = impl_->field_node(element)) {
            impl_->execute([&] {
                impl_->dispatch_plain_event(*n, "select");
                return JS_UNDEFINED;
            });
        }
    }

    void ScriptDom::compose_text_field(NodeIdentity element, std::string_view text, std::uint32_t selection_start,
                                       std::uint32_t selection_end, std::uint32_t clause_start,
                                       std::uint32_t clause_end) {
        impl_->require_runtime();
        check_text(text);
        if (auto* n = impl_->field_node(element)) {
            impl_->execute([&] {
                impl_->compose_field(*n, std::string(text), selection_start, selection_end, clause_start, clause_end);
                return JS_UNDEFINED;
            });
        }
    }

    void ScriptDom::commit_text_field(NodeIdentity element, std::string_view text) {
        impl_->require_runtime();
        check_text(text);
        if (auto* n = impl_->field_node(element)) {
            impl_->execute([&] {
                impl_->commit_field(*n, std::string(text));
                return JS_UNDEFINED;
            });
        }
    }

    void ScriptDom::commit_text_field_change(NodeIdentity element) {
        impl_->require_runtime();
        if (auto* n = impl_->field_node(element)) {
            impl_->execute([&] {
                impl_->commit_change(*n);
                return JS_UNDEFINED;
            });
        }
    }

    void ScriptDom::set_text_field_value(NodeIdentity element, std::string_view value) {
        impl_->require_runtime();
        check_text(value);
        if (auto* n = impl_->field_node(element)) {
            impl_->execute([&] {
                impl_->set_field_value_for_user(*n, std::string(value));
                return JS_UNDEFINED;
            });
        }
    }

    void ScriptDom::set_text_field_view(NodeIdentity element, bool caret_on, float scroll_x, float scroll_y) {
        impl_->require_runtime();
        if (!std::isfinite(scroll_x) || !std::isfinite(scroll_y)) {
            throw std::invalid_argument("WEFT_TEXT_FIELD: finite scroll offsets");
        }
        if (auto* n = impl_->field_node(element)) {
            impl_->execute([&] {
                impl_->set_field_view(*n, caret_on, scroll_x, scroll_y);
                return JS_UNDEFINED;
            });
        }
    }

} // namespace weft
