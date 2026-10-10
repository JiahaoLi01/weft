#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <functional>
#include "caseelephant/canvas_probe.hpp"
#include "caseelephant/constants/script.hpp"

namespace caseelephant {

    // M0 evidence API, not a stable public runtime or C ABI.
    struct ProbeNodeIdentity {
        std::uint32_t slot{};
        std::uint32_t generation{};
        std::uint64_t view_epoch{};
        friend bool operator==(const ProbeNodeIdentity&, const ProbeNodeIdentity&) = default;
    };

    struct ScriptProbeStats {
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

    // Owner-produced value copy. Contains no Node pointer or JSValue.
    struct ProbeDomSnapshot {
        ProbeNodeIdentity identity;
        int type{};
        std::string local_name;
        std::string namespace_uri;
        std::string data;
        std::map<std::string, std::string> attributes;
        std::vector<ProbeDomSnapshot> children;
        double scroll_left{};
        double scroll_top{};
        bool hovered{};
        bool active{};
        friend bool operator==(const ProbeDomSnapshot&, const ProbeDomSnapshot&) = default;
    };

    struct ProbeElementGeometry {
        ProbeNodeIdentity identity;
        std::array<double, 4> rect{};
        std::vector<std::array<double, 4>> client_rects;
        double client_width{};
        double client_height{};
        double scroll_width{};
        double scroll_height{};
        double scroll_left{};
        double scroll_top{};
        bool scroll_x{};
        bool scroll_y{};
        bool wheel_x{};
        bool wheel_y{};
        std::array<double, 4> offset{};                 // Untransformed border box relative to offset parent.
        std::array<double, 6> matrix{1, 0, 0, 1, 0, 0}; // SVG/local coordinates to logical viewport.
        ProbeNodeIdentity offset_parent{};
    };

    using ProbeLayoutHost = std::function<std::vector<ProbeElementGeometry>(const ProbeDomSnapshot&)>;
    using ProbeComputedStyleHost =
        std::function<std::map<std::string, std::string>(const ProbeDomSnapshot&, ProbeNodeIdentity)>;
    using ProbeLayoutEpochHost = std::function<std::uint64_t()>;
    using ProbeIntegerFormatHost = std::function<std::string(std::int64_t)>;

    struct ScriptProbeConfig {
        std::size_t max_stack_bytes{
            constants::script::default_vm_stack_bytes}; // Host must reserve additional native headroom.
        std::uint32_t execution_budget_ms{
            constants::script::default_execution_budget_ms}; // Bounded wall time per eval + microtask checkpoint.
    };

    struct ProbeAnimationFrameResult {
        std::size_t callbacks{};
        std::size_t pending{};
        std::size_t exceptions{};
        std::string exception_messages_json;
    };

    struct ProbeTaskResult {
        std::size_t callbacks{};
        std::size_t ready{};
    };

    struct ProbeDomChanges {
        std::uint64_t epoch{};
        std::uint64_t revision{};
        bool full_required{};
        std::vector<ProbeNodeIdentity> nodes; // Weak identities; no extra GC roots.
    };

    struct ProbeCanvasSnapshot {
        ProbeNodeIdentity identity;
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint64_t version{};
        std::uint64_t commands{};
        std::uint64_t fills{};
        bool alpha{};
        std::shared_ptr<const std::vector<std::uint8_t>> rgba; // immutable sRGB straight
    };
    enum class ProbeMouseType {
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

    struct ProbeMouseEvent {
        ProbeMouseType type{};
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
        ProbeNodeIdentity related_target{}; // Null identity or same-epoch live node.
    };

    struct ProbeWheelEvent {
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
    enum class ProbeDispatchResult {
        StaleTarget,
        DefaultAllowed,
        DefaultPrevented
    };

    // Every operation, including destruction, belongs to the creating thread.
    // The DOM is a native C++ graph traced through QuickJS custom-class gc_mark.
    // No browser layout, SVG rasterization, timers, networking, or shell is exposed.
    class ScriptDomProbe final {
    public:
        explicit ScriptDomProbe(ScriptProbeConfig config = {});
        ~ScriptDomProbe();
        ScriptDomProbe(const ScriptDomProbe&) = delete;
        ScriptDomProbe& operator=(const ScriptDomProbe&) = delete;
        ScriptDomProbe(ScriptDomProbe&&) = delete;
        ScriptDomProbe& operator=(ScriptDomProbe&&) = delete;

        // Evaluates a classic script and returns its result converted to UTF-8.
        // JS errors and owner violations become std::runtime_error/logic_error.
        std::string eval(std::string_view source, std::string_view filename = "<probe>");
        // Optional animation-frame platform bootstrap must be installed. One shared
        // execution budget, with a microtask checkpoint after every callback.
        ProbeAnimationFrameResult animation_frame(double time_ms, bool drawable = true);
        // Optional platform module, direct calls with the same budget/checkpoints.
        ProbeTaskResult pump_tasks(double time_ms);
        // Optional owner raster adapter, installed before creating canvas nodes.
        void install_canvas_host(ProbeCanvasFactory);
        ProbeCanvasSnapshot canvas_snapshot(ProbeNodeIdentity);
        void collect_garbage();
        ScriptProbeStats stats() const;
        std::uint64_t epoch() const;
        void reset_document();

        // These identities are weak. Retention creates a VM-visible native root.
        ProbeNodeIdentity node_identity(std::string_view selector) const;
        bool is_live(ProbeNodeIdentity identity) const;
        std::uint64_t retain_node(ProbeNodeIdentity identity);
        void release_node(std::uint64_t root_token);

        // Completion data contains no JSValue or native Node pointer.
        void queue_callback(std::uint64_t callback_epoch, std::string source);
        std::size_t drain_callbacks();
        std::string dump_document() const;
        ProbeDomSnapshot snapshot_subtree(std::string_view selector) const;
        ProbeDomSnapshot snapshot_subtree(ProbeNodeIdentity) const;
        std::uint64_t subtree_revision(ProbeNodeIdentity) const;
        // Cursor belongs to epoch(); discard it on epoch change. A journal overflow
        // requires a full snapshot. Journal identities do not retain detached nodes.
        ProbeDomChanges changes_since(std::uint64_t revision) const;
        // Owner-only host event. Weak identity is revalidated; detached targets are rejected.
        ProbeDispatchResult dispatch_mouse(ProbeNodeIdentity target, const ProbeMouseEvent& event);
        // Fresh layout on every geometry read. The host cannot enter JS or mutate
        // this owner while measuring; only owned snapshots/weak identities cross it.
        void install_layout_host(ProbeLayoutHost);
        void install_computed_style_host(ProbeComputedStyleHost);
        // Optional owner cache: DOM mutation invalidates automatically. The host
        // epoch must change for clock/viewport/font/style inputs outside the DOM.
        // Epoch callback cannot enter JS/DOM. No Node or JSValue is cached here.
        // Without this opt-in each read continues through the full host path.
        void install_layout_cache_host(ProbeLayoutEpochHost);
        void install_integer_format_host(ProbeIntegerFormatHost);
        ProbeDispatchResult dispatch_wheel(ProbeNodeIdentity target, double delta_x, double delta_y);
        ProbeDispatchResult dispatch_wheel(ProbeNodeIdentity target, const ProbeWheelEvent& event);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace caseelephant
