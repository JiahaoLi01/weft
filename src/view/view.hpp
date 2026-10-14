#pragma once

#include "bridge/engine_channel.hpp"
#include "scene/scene.hpp"
#include "scene/scene_anchor.hpp"
#include "scene/scene_geometry.hpp"
#include "script/script_dom.hpp"
#include "text/font_catalog.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A View (docs/engine-communication.md section 8): one page, its VM and its scene, driven by a host. The owner thread
// loads the page, advances it, hands it pointer input and asks what is under a point; other threads write the View's
// anchors, give the page engine events, call completions and view models, and take the frames it published.
namespace weft {
    // A page: the scripts evaluated in order after the platform modules (WebHost, AnimationFrame, ScenePlatform,
    // Engine), the scene's stylesheet and keyframes, and the selector of the scene's root (#id or a tag name). WebHost
    // gives the document a <div id="root"> in its body.
    struct ViewPage {
        struct Script {
            std::string name;
            std::string source;
        };

        std::vector<Script> scripts;
        std::string stylesheet;
        std::string keyframes;
        std::string root = "#root";
        // CRPG Framework's window.ue, __ue_dispatch and __vm_update over weft.engine (ScriptPlatformModule::CrpgBridge).
        bool crpg_bridge{};
    };

    // A frame a View published: its scene (never changed again), the View's size in CSS px and its device scale, the
    // fonts its text was laid out with (a renderer shapes with the same), and the View's anchors, whose latest table a
    // renderer takes as it draws.
    struct ViewFrame {
        std::shared_ptr<const SceneFrame> scene;
        float width{};
        float height{};
        float scale{};
        std::shared_ptr<const FontCatalog> fonts;
        std::shared_ptr<SceneAnchorChannel> anchors;
    };

    // What other threads reach of a View; it outlives the View while they hold it. Thread-safe.
    class ViewShared {
    public:
        explicit ViewShared(std::shared_ptr<bridge::EngineChannel> engine);

        // The engine writes all its anchors (scene_anchor.hpp); writers take turns.
        SceneAnchorWrite write_anchors(std::span<const SceneAnchor> anchors);

        const std::shared_ptr<SceneAnchorChannel>& anchors() const {
            return anchors_;
        }

        const std::shared_ptr<bridge::EngineChannel>& engine() const {
            return engine_;
        }

        // The frame published last; none before the first.
        std::shared_ptr<const ViewFrame> frame() const;
        void publish(std::shared_ptr<const ViewFrame> frame);

    private:
        std::shared_ptr<SceneAnchorChannel> anchors_ = std::make_shared<SceneAnchorChannel>();
        std::shared_ptr<bridge::EngineChannel> engine_;
        mutable std::mutex frame_mutex_;
        std::shared_ptr<const ViewFrame> frame_;
        std::mutex writer_;
    };

    enum class PointerKind : std::uint8_t {
        Move,
        Down,
        Up,
        Wheel,
        // The pointer left the View: what it hovered is left.
        Leave
    };

    // Pointer input in the View's CSS px; buttons as MouseEvent.buttons; a wheel's deltas in CSS px.
    struct PointerInput {
        PointerKind kind{};
        float x{};
        float y{};
        unsigned buttons{};
        float wheel_x{};
        float wheel_y{};
    };

    // What is under a point: the topmost element that takes pointer events other than the scene's root and its
    // ancestors (none when only they are), and its cursor (the cursor property of it or its nearest ancestor that
    // declares one; "auto" when none does).
    struct ViewHit {
        NodeIdentity element;
        std::string cursor = "auto";
    };

    // Something that went wrong in the page: an exception thrown by its scripts (Script), by an engine handler
    // (Handler) or by an animation frame callback (Frame), or a scene the page's styles cannot make (Scene).
    struct ViewDiagnostic {
        enum class Kind : std::uint8_t {
            Script,
            Handler,
            Frame,
            Scene
        };

        Kind kind{};
        std::string message;
    };

    // A page's script threw while it loaded (the View is empty again), or the View faulted.
    struct ViewError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Owner thread: the thread that creates it, with constants::view::owner_stack_bytes of stack. text is the owner's
    // SceneText of fonts, and outlives the View. shared is what other threads reach (a new one when null).
    class View {
    public:
        View(std::shared_ptr<const FontCatalog> fonts, SceneText& text, float width, float height, float scale,
             std::shared_ptr<ViewShared> shared = nullptr);
        ~View();
        View(const View&) = delete;
        View& operator=(const View&) = delete;

        // Replaces the page: a new document and VM state, the page's scripts, styles and root. A script that throws
        // leaves an empty View, the error a diagnostic, and throws ViewError. The engine channel and functions stay.
        void load(const ViewPage& page);
        // Runs the page up to time_ms (non-decreasing): engine inputs, tasks, animation frames, observers and the
        // scene; publishes a frame when what it shows changed. Exceptions of the page become diagnostics.
        void advance(double time_ms);
        void resize(float width, float height, float scale);
        // Dispatches the pointer events of input at the latest scene: pointer and mouse events, over, out, enter and
        // leave as the hovered element changes, and a click when a button comes up over the element it went down
        // on. Whether a listener prevented the default action of the main event.
        bool pointer(const PointerInput& input);
        ViewHit hit(float x, float y);

        // The diagnostics since the last take, and how many were dropped past constants::view::max_diagnostics.
        std::vector<ViewDiagnostic> take_diagnostics(std::size_t* dropped = nullptr);
        // The page's weft.engine messages since the last take.
        std::vector<EngineMessage> take_messages();
        // weft.engine.invoke(name) calls function; kept across loads.
        void install_function(std::string name, EngineFunction function);

        const std::shared_ptr<ViewShared>& shared() const {
            return shared_;
        }

        // A resource limit ended the page: only load (or destroying the View) is left.
        bool faulted() const;
        bool loaded() const;
        double time() const;
        // The owner's scene and VM, for tests and diagnostics.
        const SceneFrame& scene() const;
        ScriptDom& script();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<ViewShared> shared_;
    };
} // namespace weft
