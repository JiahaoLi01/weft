#include "script/script_dom.hpp"
#include "bridge/bridge_value.hpp"
#include "bridge/engine_channel.hpp"
#include "constants/bridge.hpp"
#include "constants/common.hpp"
#include "base/diagnostic.hpp"
#include "base/owner_thread.hpp"
#include "constants/dom.hpp"
#include "constants/script.hpp"
#include "dom/attributes.hpp"
#include "dom/node_table.hpp"
#include "dom/selector.hpp"
#include "dom/tree.hpp"

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
            ++counters.created_nodes;
            ++counters.live_nodes;
            return JS_DupValue(context, object.value);
        }

        // Removes child from its parent; the caller holds child. The edge pair's references go once the tree is
        // consistent, because releasing them may finalize nodes.
        void detach(Node& child) {
            const auto former = tree.detach(child);
            check_journal();
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
            }
        }

        std::string text(const Node& n) const {
            return tree.text(n);
        }

        void set_text(Node& n, std::string input) {
            if (n.type == constants::text_node_type) {
                if (n.data != input) {
                    n.data = std::move(input);
                    changed(n);
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

        // Selector lists of DOM queries: the shared grammar with escapes and :hover/:active.
        static std::vector<dom::Selector> selectors(std::string_view text) {
            static const dom::SelectorGrammar grammar{true, true, Diag::DomSelector, Diag::DomUnsupportedSelector};
            auto list = dom::parse_selector_list(text, grammar, constants::dom::max_selector_list_items);
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
            OffsetParent
        };

        static JSValue get_property(JSContext* c, JSValueConst self, int magic) {
            try {
                Node& n = node(c, self);
                Impl& owner = state(c);
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
            EngineClose
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
                        owner.insert(n, child, before);
                        return JS_DupValue(c, owner.wrapper(child));
                    }
                    case Remove: {
                        Node& child = node(c, arg(argc, argv, 0));
                        if (child.parent != n.identity) {
                            throw std::runtime_error("WEFT_NOT_FOUND: child is not in parent");
                        }
                        Value hold(c, JS_DupValue(c, owner.wrapper(child)));
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
                JS_SetClassProto(context, node_class, JS_DupValue(context, prototype.value));

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
                if (JS_IsException(wheel_prototype) || JS_IsException(event_prototype)) {
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
            layout_host = {};
            computed_style_host = {};
            particle_host = {};
            layout_epoch_host = {};
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
            default:
                throw std::invalid_argument("WEFT_MOUSE_PROFILE: unsupported event type");
        }
        const auto result = impl_->execute([&] {
            auto* c = impl_->context;
            Value pinned(c, JS_DupValue(c, impl_->wrapper(*target)));
            Value prototype(c, JS_DupValue(c, impl_->mouse_prototype));
            if (std::string_view(type).starts_with("pointer")) {
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
            if (std::string_view(type).starts_with("pointer")) {
                property("pointerId", JS_NewInt32(c, constants::script::mouse_pointer_id));
                property("pointerType", JS_NewString(c, "mouse"));
                property("isPrimary", JS_TRUE);
            }
            property("defaultPrevented", JS_FALSE);
            property("currentTarget", JS_NULL);
            property("eventPhase", JS_NewInt32(c, constants::script::event_phase_none));
            property("view", JS_GetGlobalObject(c));
            const auto* related = impl_->resolve(input.related_target);
            property("relatedTarget", related ? JS_DupValue(c, impl_->wrapper(*related)) : JS_NULL);
            property("detail", JS_NewInt32(c, input.type == MouseType::Move ? 0 : 1));
            property("button", JS_NewInt32(c, 0));
            property("buttons", JS_NewUint32(c, input.buttons));
            property("clientX", JS_NewFloat64(c, input.client_x));
            property("clientY", JS_NewFloat64(c, input.client_y));
            property("screenX", JS_NewFloat64(c, input.screen_x));
            property("screenY", JS_NewFloat64(c, input.screen_y));
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

} // namespace weft
