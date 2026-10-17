#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <functional>
#include "canvas/canvas.hpp"
#include "constants/script.hpp"
#include "dom/geometry.hpp"
#include "dom/node.hpp"

namespace weft {
    namespace bridge {
        class EngineChannel;
    } // namespace bridge

    class WebStorage;

    // M0 evidence API, not a stable public runtime or C ABI. The DOM value types are in dom/node.hpp and
    // dom/geometry.hpp.
    struct ScriptStats {
        std::size_t live_nodes{};
        std::size_t created_nodes{};
        std::size_t finalized_nodes{};
        std::size_t native_roots{};
        std::size_t gc_runs{};
        std::size_t rejected_callbacks{};
        std::size_t listener_exceptions{};
        std::size_t listener_calls{};
        std::size_t vm_allocated_bytes{};
        std::size_t peak_vm_allocated_bytes{};
        std::string last_listener_error;
        std::size_t canvas_contexts{};
        std::size_t canvas_backing_bytes{};
        std::size_t peak_canvas_backing_bytes{};
        std::size_t dom_mutations{};
        std::size_t snapshot_nodes{};
        std::size_t script_evaluations{};
        std::size_t host_calls{};
    };

    using LayoutHost = std::function<std::vector<ElementGeometry>(const DomSnapshot&)>;
    using ComputedStyleHost = std::function<std::map<std::string, std::string>(const DomSnapshot&, NodeIdentity)>;
    using LayoutEpochHost = std::function<std::uint64_t()>;
    using IntegerFormatHost = std::function<std::string(std::int64_t)>;

    // A burst weft.emitParticles asks of an element (the scene's particles): the registered particles' name, how many,
    // over how long (seconds), and where they fly to: a target element's centre, or a point in the viewport's CSS px.
    struct ParticleRequest {
        std::string name;
        std::uint32_t count{};
        double duration{};
        std::optional<NodeIdentity> target;
        std::optional<std::array<double, 2>> point;
    };

    using ParticleHost = std::function<void(NodeIdentity element, const ParticleRequest&)>;

    // The web platform module (ScriptPlatformModule::WebPlatform): what the page's console writes, at its level, and
    // whether a media query holds (matchMedia). Neither can enter JS; what the media host throws becomes the call's
    // exception.
    enum class ConsoleLevel : std::uint8_t {
        Log,
        Warning,
        Error
    };

    using ConsoleHost = std::function<void(ConsoleLevel, std::string_view text)>;
    using MediaHost = std::function<bool(std::string_view query)>;

    // A message weft.engine sent the engine (docs/engine-communication.md, 6.3): a send (call 0) or a call (its id),
    // with its name and its value as a bridge value (bridge/bridge_value.hpp).
    struct EngineMessage {
        std::uint64_t call{};
        std::string name;
        std::vector<std::uint8_t> value;
    };

    // A sync engine function (weft.engine.invoke): its argument as a bridge value, its result as one; what it throws
    // becomes the invoke's exception. It runs on the owner in the middle of JS and cannot enter the ScriptDom.
    using EngineFunction = std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t> argument)>;

    // What a delivery handed the page: view-model batches, events and call completions; completions of calls the
    // page no longer waited for (stale); and the messages of the exceptions its handlers threw.
    struct EngineDelivery {
        std::size_t models{};
        std::size_t events{};
        std::size_t completions{};
        std::size_t stale{};
        std::vector<std::string> errors;
    };

    struct ScriptConfig {
        std::size_t max_stack_bytes{
            constants::script::default_vm_stack_bytes}; // Host must reserve additional native headroom.
        std::uint32_t execution_budget_ms{
            constants::script::default_execution_budget_ms}; // Bounded wall time per eval + microtask checkpoint.
    };

    struct AnimationFrameResult {
        std::size_t callbacks{};
        std::size_t pending{};
        std::size_t exceptions{};
        std::string exception_messages_json;
    };

    struct TaskResult {
        std::size_t callbacks{};
        std::size_t ready{};
    };

    struct CanvasSnapshot {
        NodeIdentity identity;
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint64_t version{};
        std::uint64_t commands{};
        std::uint64_t fills{};
        bool alpha{};
        std::shared_ptr<const std::vector<std::uint8_t>> rgba; // immutable sRGB straight
    };
    enum class MouseType {
        Down,
        Up,
        Move,
        Click,
        Over,
        Out,
        Enter,
        Leave,
        PointerDown,
        PointerUp,
        PointerMove,
        PointerOver,
        PointerOut,
        PointerEnter,
        PointerLeave,
        PointerCancel
    };

    struct MouseEvent {
        MouseType type{};
        double client_x{};
        double client_y{};
        double screen_x{};
        double screen_y{};
        double time_ms{};
        unsigned buttons{};
        bool ctrl{};
        bool shift{};
        bool alt{};
        bool meta{};
        NodeIdentity related_target{}; // Null identity or same-epoch live node.
    };

    struct WheelEvent {
        // Owner-normalized logical pixel units.
        double delta_x{};
        double delta_y{};
        double client_x{};
        double client_y{};
        double screen_x{};
        double screen_y{};
        double time_ms{};
        bool ctrl{};
        bool shift{};
        bool alt{};
        bool meta{};
    };
    enum class DispatchResult {
        StaleTarget,
        DefaultAllowed,
        DefaultPrevented
    };

    // Every operation, including destruction, belongs to the creating thread.
    // The DOM is a native C++ graph traced through QuickJS custom-class gc_mark.
    // No browser layout, SVG rasterization, timers, networking, or shell is exposed.
    class ScriptDom final {
    public:
        explicit ScriptDom(ScriptConfig config = {});
        ~ScriptDom();
        ScriptDom(const ScriptDom&) = delete;
        ScriptDom& operator=(const ScriptDom&) = delete;
        ScriptDom(ScriptDom&&) = delete;
        ScriptDom& operator=(ScriptDom&&) = delete;

        // Evaluates a classic script and returns its result converted to UTF-8.
        // JS errors and owner violations become std::runtime_error/logic_error.
        std::string eval(std::string_view source, std::string_view filename = "<probe>");
        // Optional animation-frame platform bootstrap must be installed. One shared
        // execution budget, with a microtask checkpoint after every callback.
        AnimationFrameResult animation_frame(double time_ms, bool drawable = true);
        // Optional platform module, direct calls with the same budget/checkpoints.
        TaskResult pump_tasks(double time_ms);
        // The same module's clock (performance.now()) reads time_ms from now on (monotonic); no task runs.
        void advance_clock(double time_ms);
        // Optional owner raster adapter, installed before creating canvas nodes.
        void install_canvas_host(CanvasFactory);
        CanvasSnapshot canvas_snapshot(NodeIdentity);
        void collect_garbage();
        ScriptStats stats() const;
        std::uint64_t epoch() const;
        void reset_document();

        // These identities are weak. Retention creates a VM-visible native root.
        NodeIdentity node_identity(std::string_view selector) const;
        bool is_live(NodeIdentity identity) const;
        std::uint64_t retain_node(NodeIdentity identity);
        void release_node(std::uint64_t root_token);

        // Completion data contains no JSValue or native Node pointer.
        void queue_callback(std::uint64_t callback_epoch, std::string source);
        std::size_t drain_callbacks();
        std::string dump_document() const;
        DomSnapshot snapshot_subtree(std::string_view selector) const;
        DomSnapshot snapshot_subtree(NodeIdentity) const;
        // Brings an earlier snapshot of the subtree at root up to date, copying only what changed since it was taken;
        // a snapshot of another node or epoch is replaced. Returns whether its content changed. After an exception,
        // discard the snapshot.
        bool refresh_snapshot(NodeIdentity root, DomSnapshot& snapshot) const;
        std::uint64_t subtree_revision(NodeIdentity) const;
        // Cursor belongs to epoch(); discard it on epoch change. A journal overflow
        // requires a full snapshot. Journal identities do not retain detached nodes.
        DomChanges changes_since(std::uint64_t revision) const;
        // Owner-only host event. Weak identity is revalidated; detached targets are rejected.
        DispatchResult dispatch_mouse(NodeIdentity target, const MouseEvent& event);
        // Fresh layout on every geometry read. The host cannot enter JS or mutate
        // this owner while measuring; only owned snapshots/weak identities cross it.
        void install_layout_host(LayoutHost);
        void install_computed_style_host(ComputedStyleHost);
        // Optional owner cache: DOM mutation invalidates automatically. The host
        // epoch must change for clock/viewport/font/style inputs outside the DOM.
        // Epoch callback cannot enter JS/DOM. No Node or JSValue is cached here.
        // Without this opt-in each read continues through the full host path.
        void install_layout_cache_host(LayoutEpochHost);
        void install_integer_format_host(IntegerFormatHost);
        // The web platform module's hosts: the console, the storage areas of localStorage and sessionStorage (null:
        // their methods throw), and matchMedia.
        void install_console_host(ConsoleHost);
        void install_storage(std::shared_ptr<WebStorage> local, std::shared_ptr<WebStorage> session);
        void install_media_host(MediaHost);
        // The element that has the focus (the web platform's focus()); none when it has none or left the document.
        std::optional<NodeIdentity> focused() const;
        // The owner's particles: weft.emitParticles (the scene platform) hands each burst to the host, which records it
        // for the next frame and cannot enter JS; what it throws becomes the call's exception.
        void install_particle_host(ParticleHost);
        // The engine bridge (the Engine platform module's weft.engine): the channel through which the engine gives the
        // page events, call completions and view models, and where the page's calls open. A reset drops what waits
        // there and closes the calls.
        void install_engine(std::shared_ptr<bridge::EngineChannel> channel);
        // weft.engine.invoke(name, value) calls function.
        void install_engine_function(std::string name, EngineFunction function);
        // What the page sent and called since the last take, oldest first.
        std::vector<EngineMessage> take_engine_messages();
        // Hands the page what waits in the channel: view-model batches, then events and completions in the order
        // given, each followed by a microtask checkpoint, under one execution budget. Nothing without a channel.
        EngineDelivery deliver_engine();
        DispatchResult dispatch_wheel(NodeIdentity target, double delta_x, double delta_y);
        DispatchResult dispatch_wheel(NodeIdentity target, const WheelEvent& event);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace weft
