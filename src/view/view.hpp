#pragma once

#include "bridge/engine_channel.hpp"
#include "scene/scene.hpp"
#include "scene/scene_anchor.hpp"
#include "scene/scene_geometry.hpp"
#include "script/script_dom.hpp"
#include "script/web_storage.hpp"
#include "text/font_catalog.hpp"
#include "view/view_images.hpp"

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
    // Engine), the scene's stylesheet and keyframes, the selector of the scene's root (#id or a tag name), and the base
    // its relative URLs resolve against (view_resolve_url). WebHost gives the document a <div id="root"> in its body.
    struct ViewPage {
        struct Script {
            std::string name;
            std::string source;
        };

        std::vector<Script> scripts;
        std::string stylesheet;
        std::string keyframes;
        std::string root = "#root";
        std::string base;
        // CRPG Framework's window.ue, __ue_dispatch and __vm_update over weft.engine (ScriptPlatformModule::CrpgBridge).
        bool crpg_bridge{};
        // CSS as browsers take it (WEFT_PAGE_FORGIVING_CSS): what the scene cannot take is a diagnostic, not a failure.
        bool forgiving_css{};
        // The web platform pages written for browsers expect (WEFT_PAGE_WEB_PLATFORM, ScriptPlatformModule::WebPlatform):
        // the console as diagnostics, the View's storage areas as localStorage and sessionStorage, focus, matchMedia.
        bool web_platform{};
        // Blink's spatial navigation (WEFT_PAGE_SPATIAL_NAVIGATION, Chrome's --enable-spatial-navigation): the arrow
        // keys move the focus to the nearest focusable element their way and the pointer with it, Escape blurs it;
        // elements meant to respond to the user (a listener for click, keys or the mouse coming over, cursor: pointer)
        // take the focus, Enter makes the focused one :active while it is down and Enter's or Space's keypress clicks
        // it; and a gamepad's D-pad and left stick, A and B are the arrow keys, Enter and Escape while the View has the
        // host's focus.
        bool spatial_navigation{};
    };

    // A frame a View published: its scene (never changed again), the View's size in CSS px and its device scale, the
    // fonts its text was laid out with (a renderer shapes with the same), and the View's anchors, whose latest table a
    // renderer takes as it draws.
    struct ViewFrame {
        std::shared_ptr<const SceneFrame> scene;
        float width{};
        float height{};
        float scale{};
        // The fonts its text was laid out with: the runtime's, or its page's (@font-face faces over the runtime's).
        std::shared_ptr<const FontCatalog> fonts;
        // The runtime's fonts, whatever the page's: one runtime's frames carry the same, which a renderer checks.
        std::shared_ptr<const FontCatalog> runtime_fonts;
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

    // Pointer input in the View's CSS px; buttons as MouseEvent.buttons; a wheel's deltas in CSS px; the modifiers held
    // (key_modifier bits: Shift extends a text field's selection to a press).
    struct PointerInput {
        PointerKind kind{};
        float x{};
        float y{};
        unsigned buttons{};
        float wheel_x{};
        float wheel_y{};
        unsigned modifiers{};
    };

    // A touch's change (the C ABI's weft_touch_point): the host's name of the touch from its start to its end, what
    // happened to it, where it is in the View's CSS px, its contact's radii (CSS px; 0 when unknown) and force (0 to 1;
    // 0 when unknown).
    enum class TouchPhase : std::uint8_t {
        Began,
        Moved,
        Ended,
        Cancelled
    };

    struct TouchChange {
        std::uint64_t id{};
        TouchPhase phase{};
        float x{};
        float y{};
        float radius_x{};
        float radius_y{};
        float force{};
    };

    // The touches that changed at once, each once, the modifiers held, and when they changed (ms, on advance's clock;
    // none: the View's time), which the release speed of a pan is measured with.
    struct TouchInput {
        std::vector<TouchChange> points;
        unsigned modifiers{};
        std::optional<double> time_ms;
    };

    // Where text input goes (the C ABI's weft_text_input): whether a text field the user may edit has the focus while
    // the View has the host's (a host turns its input method on only then), whether an input method composes in it, and
    // whether it is a password field (where Chrome keeps input methods off); its caret (where an input method's candidate
    // window goes) and its border box, as rectangles in the View's px (x, y, width, height).
    struct ViewTextInput {
        bool active{};
        bool composing{};
        bool password{};
        std::array<float, 4> caret{};
        std::array<float, 4> field{};
    };

    // Keyboard input (the C ABI's weft_key_event): a key going down or up, its KeyboardEvent key and code values, the
    // modifiers (key_modifier bits) while it happens, whether it repeats, whether an input method took it (key
    // "Process", keyCode 229, no default action), and its Windows virtual key (KeyboardEvent.keyCode; 0 derives it
    // from code as a US keyboard has it).
    struct KeyInput {
        bool down{};
        std::string key;
        std::string code;
        unsigned modifiers{};
        bool repeat{};
        bool ime{};
        unsigned key_code{};
    };

    // The modifier of the platform's editing shortcuts (key_modifier bits: Command+C copies on Apple platforms,
    // Control+C elsewhere), as Chrome's on each platform.
#if defined(__APPLE__)
    inline constexpr unsigned editing_modifier = key_modifier::meta;
#else
    inline constexpr unsigned editing_modifier = key_modifier::control;
#endif

    // The system clipboard as the host has it (plain UTF-8 text): what is on it (none: no text) and text to put on it.
    // Owner thread; neither may call into the View.
    struct ViewClipboard {
        std::function<std::optional<std::string>()> read;
        std::function<void(std::string_view text)> write;
    };

    // What became of an input (the C ABI's WEFT_INPUT_*): a listener prevented its default action; weft performed one,
    // or began one a later event finishes (it moved the focus, scrolled, clicked, made a button :active).
    inline constexpr unsigned input_prevented = 1;
    inline constexpr unsigned input_default = 2;

    // What is under a point: the topmost element that takes pointer events other than the scene's root and its
    // ancestors (none when only they are), and its cursor (the cursor property of it or its nearest ancestor that
    // declares one; "auto" when none does).
    struct ViewHit {
        NodeIdentity element;
        std::string cursor = "auto";
    };

    // Something that went wrong in the page: an exception thrown by its scripts (Script), by an engine handler
    // (Handler) or by an animation frame callback (Frame), or a scene the page's styles cannot make (Scene); or what the
    // page wrote to its console (the web platform's), by level.
    struct ViewDiagnostic {
        enum class Kind : std::uint8_t {
            Script,
            Handler,
            Frame,
            Scene,
            Console,
            ConsoleWarning,
            ConsoleError
        };

        Kind kind{};
        std::string message;
    };

#if WEFT_DEVTOOLS
    // What the pointer did while the developer tools pick an element (View::inspect_pointer_mode): the element under it
    // now, and the one it pressed since the last take, if any.
    struct InspectPointer {
        NodeIdentity hovered;
        NodeIdentity picked;
    };

    // A diagnostic with its place in the View's log, which runs on from the first page: what the tools show of earlier
    // diagnostics when they connect late, and what they have not sent yet.
    struct InspectLogEntry {
        std::uint64_t sequence{};
        ViewDiagnostic diagnostic;
        // What the parser of the page's sheet dropped, which the tools have with its place in the sheet.
        bool sheet{};
    };
#endif

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
        // The viewport's safe area in logical px, top, right, bottom and left, which CSS reads as
        // env(safe-area-inset-*): 0 on every side until set, kept across page loads. The next advance lays out the
        // values that read it again. Throws std::invalid_argument for an inset that is negative or not finite.
        void set_safe_area(float top, float right, float bottom, float left);
        // The input devices the page sees (as Chrome on such a device): @media and matchMedia's pointer, any-pointer,
        // hover and any-hover, navigator.maxTouchPoints, and the touch event handlers (ontouchstart and the like) with
        // a touch screen. A mouse until set, kept across page loads; matchMedia lists whose outcome changes get a change
        // event, and the next advance cascades again where a @media outcome changed. Throws std::invalid_argument for
        // devices other than a mouse and a touch screen, a primary one not available, or touch points (1 to 32)
        // without a touch screen or none with one.
        void set_input_devices(const InputDevices& devices, unsigned max_touch_points);
        // Dispatches the pointer events of input at the latest scene: pointer and mouse events, over, out, enter and
        // leave as the hovered element changes, and a click when a button comes up over the element it went down
        // on. Whether a listener prevented the default action of the main event.
        bool pointer(const PointerInput& input);
        // Touch input, as Chrome takes a touch screen's: each touch a pointer (pointerType "touch", over, enter, down,
        // move, up, cancel, out and leave, captured implicitly by the element it went down on) and touch events
        // (touchstart, touchmove past the slop, touchend, touchcancel, cancelable until a touchmove goes unprevented).
        // A touch that stays within the slop is a tap: compatibility mouse events and a click (detail counting taps,
        // dblclick on the second) at where it went down, the element hovered after and :active a moment; touchstart
        // or touchend prevented, none. One that moves past it pans the nearest scroll container that can scroll that
        // way, as touch-action allows (pointercancel, the rest of its touch events not cancelable), and flings on at
        // its release speed (advance runs the fling); its touchmove prevented, it drags instead. A new touch stops a
        // fling. Whether a listener prevented a touch event. Throws std::invalid_argument for a point that is not
        // finite, a touch that begins twice or changes before it began.
        bool touch(const TouchInput& input);
        // Keyboard input at the focused element (the body when none has the focus) with Chrome's default actions: Tab
        // moves the focus, Enter and Space activate buttons and links, the arrow and page keys scroll. Its input_* bits.
        unsigned key(const KeyInput& input);
        // Text a key typed (UTF-8), after its keydown: a keypress for each character. Control characters are
        // ignored (Enter, Tab and Backspace act from their keydown); after a keydown the page or a default action used,
        // the text is dropped, as Chrome drops the characters that follow such a key. Its input_* bits.
        unsigned text(std::string_view text);
        // The input method composes text at the focused text field (ScriptDom::compose_text_field): its selection at
        // [selection_start, selection_end) of text (UTF-8 offsets); empty text cancels the composition. Whether a field
        // the user may edit took it.
        bool compose(std::string_view text, std::uint32_t selection_start, std::uint32_t selection_end);
        // The input method commits text at the focused text field (ScriptDom::commit_text_field): the composition
        // ends with it; without one the text is typed (beforeinput and input, no keys). Whether a field took it.
        bool commit(std::string_view text);
        // Where text input goes now.
        ViewTextInput text_input();
        // A gamepad's state (each frame while it is connected, and once as it goes): what navigator.getGamepads()
        // shows of it (ScriptDom::set_gamepad), and on a page with spatial navigation while the View has the host's
        // focus, its D-pad and left stick, A and B as the arrow keys, Enter and Escape. Its input_* bits.
        unsigned gamepad(const GamepadState& input);
        // The host's keyboard focus came to the View or left it (ScriptDom::set_page_focus). A View starts with it,
        // and keeps it across loads.
        void set_focus(bool focused);
        bool focused() const;
        ViewHit hit(float x, float y);
        // A tool's script in the page: whether it completed, and String() of its value or of what it threw.
        std::pair<bool, std::string> evaluate(std::string_view source);

        // The diagnostics since the last take, and how many were dropped past constants::view::max_diagnostics.
        std::vector<ViewDiagnostic> take_diagnostics(std::size_t* dropped = nullptr);
        // The page's weft.engine messages since the last take.
        std::vector<EngineMessage> take_messages();
        // weft.engine.invoke(name) calls function; kept across loads.
        void install_function(std::string name, EngineFunction function);
        // Reads the files the page names (url() images) by their resolved paths, each once a page load; kept across
        // loads. Without one, such an image is a diagnostic and shows nothing.
        void set_resource_reader(ViewResourceReader reader);
        // The clipboard of copy, cut and paste in text fields and of navigator.clipboard; kept across loads. Without
        // one (or with an empty one) the View keeps its own: the text copied last, in memory.
        void set_clipboard(ViewClipboard clipboard);
        // The storage area behind the web platform's localStorage; kept across loads (a host may share one between
        // Views and save it). sessionStorage is the View's own, kept across loads too.
        void set_local_storage(std::shared_ptr<WebStorage> storage);
        const std::shared_ptr<WebStorage>& local_storage() const;
        // The UI shader effects -weft-effect(name) names; the host keeps them alive (and adds to them only on the owner
        // thread). Without them such a background is a scene diagnostic.
        void set_effects(const SceneEffects* effects);

        const std::shared_ptr<ViewShared>& shared() const {
            return shared_;
        }

        // A resource limit ended the page: only load (or destroying the View) is left.
        bool faulted() const;
        bool loaded() const;
        double time() const;
        // The owner's scene and VM, for tests and diagnostics.
        const SceneFrame& scene() const;
        // The fonts its frames carry: the runtime's, or its page's catalog (@font-face).
        std::shared_ptr<const FontCatalog> fonts() const;
        ScriptDom& script();

#if WEFT_DEVTOOLS
        // The developer tools' reach into the page (src/devtools, docs/devtools.md). Owner thread.
        //
        // Counts the pages loaded: a new document, sheet and VM state.
        std::uint64_t inspect_loads() const;
        // The stylesheet's name (the page's base) and the compiled sheet; null without a scene.
        const std::string& inspect_sheet_name() const;
        const CompiledStyle* inspect_style() const;
        // The whole document from its element (none before a page), and the revision of its subtree.
        std::optional<DomSnapshot> inspect_document();
        std::uint64_t inspect_document_revision();
        // The DOM the scene was last cascaded from (the root's subtree); null before the first scene.
        const DomSnapshot* inspect_scene_dom();
        // The element's values as getComputedStyle reads them; empty for one that is not in the scene.
        std::map<std::string, std::string> inspect_computed(NodeIdentity element);
        // The element's geometry; none for one that is not in the scene.
        std::optional<ElementGeometry> inspect_geometry(NodeIdentity element);
        // The topmost node at a point of the View, whatever it is (the root included).
        NodeIdentity inspect_hit(float x, float y);
        // The log since a sequence number (0: all that is kept).
        std::vector<InspectLogEntry> inspect_log(std::uint64_t after) const;
        // Tree of boxes the frames show over the page, which no script, cascade or hit test sees; none takes them away.
        void inspect_overlay(const std::optional<DomSnapshot>& boxes);
        // While on, pointer and touch input pick an element for the tools instead of reaching the page.
        void inspect_pointer_mode(bool on);
        InspectPointer inspect_take_pointer();

        // Reloading and restyling (docs/devtools.md, "热重载").
        //
        // The page the View was last loaded with, and the keyframes its scene has now (that page's, or the last
        // restyle's).
        const ViewPage& inspect_page() const;
        const std::string& inspect_keyframes() const;
        // Replaces the page's style sheet and keyframes and reads its images and fonts again: the document and the
        // scripts' state stay. The scene is made with the new sheet first; when it cannot be, nothing changes and
        // what went wrong is returned. quiet keeps what the sheet drops out of the diagnostics (an edit under way).
        std::optional<std::string> inspect_restyle(const std::string& stylesheet, const std::string& keyframes,
                                                   bool quiet);
        // A file the page names that the tools supply in place of the host's reader, by its resolved path; and none
        // of them any more. The page reads its files at a load or a restyle.
        void inspect_set_resource(std::string path, std::vector<std::uint8_t> bytes);
        void inspect_clear_resources();
        // Before its first load, a View that is to stand in for another takes over what that one keeps across its
        // page loads: its time, session storage and clipboard text, and the tools' log, load count and resources.
        void inspect_adopt(const View& earlier);
        // The sequence number of the log's last entry; and entries of another View's log added to this one's (those of
        // a page tried in place of this View's that did not load).
        std::uint64_t inspect_log_end() const;
        void inspect_append_log(const std::vector<InspectLogEntry>& entries);
        // Every element's geometry in tree order; null without a scene.
        const std::vector<ElementGeometry>* inspect_geometries();
        // The View's width and height in CSS px and its device scale; its safe area (top, right, bottom, left); and
        // whether its scene is animating.
        std::array<float, 3> inspect_viewport() const;
        std::array<float, 4> inspect_safe_area() const;
        bool inspect_animated() const;
#endif

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<ViewShared> shared_;
    };
} // namespace weft
